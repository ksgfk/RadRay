#include <radray/runtime/forward_pipeline/forward_pipeline.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#include <radray/logger.h>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/application.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/mesh_draw.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/window_manager.h>

namespace radray {
namespace {

constexpr render::TextureFormat kForwardDepthFormat = render::TextureFormat::D32_FLOAT;
constexpr uint32_t kMaxDirectionalLights = 8;
constexpr uint32_t kMaxPointLights = 8;

bool IsTransparent(const MeshDrawItem& item) noexcept {
    return static_cast<int32_t>(item.DrawMaterial->GetRenderQueue()) >=
           static_cast<int32_t>(RenderQueue::GeometryLast);
}

std::optional<uint32_t> FindSingleBuffer(
    const ShaderParameterLayout& layout,
    uint32_t group) noexcept {
    std::optional<uint32_t> result;
    for (uint32_t index = 0; index < layout.Buffers().size(); ++index) {
        if (layout.Buffers()[index].Group != group) {
            continue;
        }
        if (result.has_value()) {
            return std::nullopt;
        }
        result = index;
    }
    return result;
}

std::optional<DynamicCBufferArena::Allocation> UploadBytes(
    DynamicCBufferArena& arena,
    std::span<const byte> data) noexcept {
    if (data.empty()) {
        return std::nullopt;
    }
    DynamicCBufferArena::Reservation reservation = arena.Reserve(data.size());
    if (!reservation.IsValid()) {
        return std::nullopt;
    }
    std::memcpy(reservation.Data(), data.data(), data.size());
    DynamicCBufferArena::Allocation allocation = reservation.Commit(data.size());
    if (!allocation.IsValid() ||
        allocation.Offset > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return allocation;
}

}  // namespace

class ForwardDrawPass final : public RenderPipelinePass {
public:
    ForwardDrawPass(
        ForwardPipeline* pipeline,
        bool transparent) noexcept
        : RenderPipelinePass(
              transparent
                  ? RenderPassEvent::BeforeRenderingTransparents
                  : RenderPassEvent::BeforeRenderingOpaques),
          _pipeline(pipeline),
          _transparent(transparent) {}

    void Execute(
        RenderPipelineContext& ctx,
        const RenderCamera& camera) override {
        if (_pipeline->ExecutePreparedPass(ctx, camera, _transparent)) {
            MarkContentDrawn();
        }
    }

private:
    ForwardPipeline* _pipeline;
    bool _transparent;
};

struct ForwardPipeline::Impl {
    struct DepthTarget {
        unique_ptr<render::Texture> Texture;
        unique_ptr<render::TextureView> View;
        uint32_t Width{0};
        uint32_t Height{0};
        uint32_t SampleCount{0};
        render::TextureStates State{render::TextureState::Undefined};
    };

    struct ResidentProgramSets {
        ShaderProgram* Program;
        render::Buffer* ViewTarget;
        render::Buffer* ObjectTarget;
        unique_ptr<render::ShaderParameterSet> ViewSet;
        unique_ptr<render::ShaderParameterSet> ObjectSet;
    };

    struct FlightResources {
        unique_ptr<DynamicCBufferArena> Arena;
        vector<ResidentProgramSets> ProgramSets;
    };

    struct PreparedDraw {
        MeshDrawItem Item;
        Nullable<render::ShaderParameterSet*> ViewSet{nullptr};
        Nullable<render::ShaderParameterSet*> MaterialSet{nullptr};
        Nullable<render::ShaderParameterSet*> ObjectSet{nullptr};
        vector<render::ShaderParameterDynamicOffset> ViewOffsets;
        vector<render::ShaderParameterDynamicOffset> MaterialOffsets;
        vector<render::ShaderParameterDynamicOffset> ObjectOffsets;
        bool Valid{false};
    };

    struct SelectedLight {
        const LightSceneProxy* Proxy;
        LightRenderParameters Parameters;
        float DistanceSquared{0.0f};
    };

    Impl(
        Application* application,
        Scene* renderScene,
        CameraComponent* viewCamera,
        ForwardPipeline* owner)
        : App(application),
          RenderScene(renderScene),
          ViewCamera(viewCamera),
          Device(application->GetDevice()),
          Registry(application->GetRenderSystem()->GetRenderPassRegistry()),
          BindingGroups(ForwardPipeline::GetBindingGroupPlan()),
          OpaquePass(owner, false),
          TransparentPass(owner, true) {
        const GpuSystem* gpu = application->GetGpuSystem();
        if (gpu != nullptr) {
            Flights.resize(gpu->GetFlightDataCount());
        }
    }

    Application* App;
    Scene* RenderScene;
    CameraComponent* ViewCamera;
    render::Device* Device;
    render::RenderPassRegistry* Registry;
    BindingGroupPlan BindingGroups;
    MeshDrawList DrawList;
    vector<PreparedDraw> Prepared;
    vector<FlightResources> Flights;
    unordered_map<AppWindow*, DepthTarget> DepthTargets;
    vector<ShaderProgram*> InvalidPrograms;
    bool LightOverflowWarned{false};
    ForwardDrawPass OpaquePass;
    ForwardDrawPass TransparentPass;

    ~Impl() noexcept {
        if (Registry != nullptr) {
            for (auto& [window, depth] : DepthTargets) {
                (void)window;
                Registry->RemoveFramebuffersUsing(depth.View.get());
            }
        }
    }

    bool BeginFrame(AppFrameContext& frame) {
        const uint32_t flightIndex = frame.FlightIndex();
        if (Device == nullptr || flightIndex >= Flights.size()) {
            return false;
        }
        FlightResources& flight = Flights[flightIndex];
        if (flight.Arena == nullptr) {
            DynamicCBufferArena::Descriptor descriptor;
            descriptor.BasicSize = 1024 * 1024;
            descriptor.Alignment = std::max<uint64_t>(Device->GetDetail().CBufferAlignment, 1);
            // MaxResetSize must stay 0: FlightResources::ProgramSets caches raw
            // render::Buffer* arena targets across frames and is never cleared, so
            // Reset() must only rewind blocks and never release them.
            descriptor.MaxResetSize = 0;
            descriptor.NamePrefix = "ForwardPipeline";
            flight.Arena = make_unique<DynamicCBufferArena>(
                Device,
                &frame.GetHostWrites(),
                descriptor);
        }
        flight.Arena->Reset();
        Prepared.clear();
        return flight.Arena->IsValid();
    }

    bool EnsureDepth(
        AppWindow* window,
        uint32_t width,
        uint32_t height,
        uint32_t sampleCount) {
        DepthTarget& depth = DepthTargets[window];
        if (depth.Texture != nullptr && depth.View != nullptr &&
            depth.Width == width && depth.Height == height &&
            depth.SampleCount == sampleCount) {
            return true;
        }
        if (Registry != nullptr && depth.View != nullptr) {
            Registry->RemoveFramebuffersUsing(depth.View.get());
        }
        depth.View.reset();
        depth.Texture.reset();
        depth.Width = width;
        depth.Height = height;
        depth.SampleCount = sampleCount;
        depth.State = render::TextureState::Undefined;

        Nullable<unique_ptr<render::Texture>> texture = Device->CreateTexture(
            render::TextureDescriptor{
                .Dim = render::TextureDimension::Dim2D,
                .Width = width,
                .Height = height,
                .DepthOrArraySize = 1,
                .MipLevels = 1,
                .SampleCount = sampleCount,
                .Format = kForwardDepthFormat,
                .Memory = render::MemoryType::Device,
                .Usage = render::TextureUse::DepthStencilRead |
                         render::TextureUse::DepthStencilWrite,
                .Hints = render::ResourceHint::None});
        if (!texture.HasValue()) {
            return false;
        }
        depth.Texture = texture.Release();
        Nullable<unique_ptr<render::TextureView>> view = Device->CreateTextureView(
            render::TextureViewDescriptor{
                .Target = depth.Texture.get(),
                .Dim = render::TextureDimension::Dim2D,
                .Format = kForwardDepthFormat,
                .Range = render::SubresourceRange{0, 1, 0, 1},
                .Usage = render::TextureViewUsage::DepthWrite});
        if (!view.HasValue()) {
            depth.Texture.reset();
            return false;
        }
        depth.View = view.Release();
        return true;
    }

    bool ValidateProgram(ShaderProgram* program) {
        if (std::find(InvalidPrograms.begin(), InvalidPrograms.end(), program) !=
            InvalidPrograms.end()) {
            return false;
        }
        const uint32_t groups[] = {
            BindingGroups.ViewGroup,
            BindingGroups.MaterialGroup,
            BindingGroups.ObjectGroup};
        for (const uint32_t group : groups) {
            // The pipeline uploads each of these buffers from a per-frame arena, so the
            // declaration has to be the group's only buffer and has to take its offset at bind
            // time.
            const std::optional<uint32_t> buffer =
                FindSingleBuffer(program->GetParameterLayout(), group);
            if (!buffer.has_value() ||
                !program->IsBufferDynamic(
                    program->GetParameterLayout().Buffers()[buffer.value()].Name)) {
                InvalidPrograms.push_back(program);
                RADRAY_ERR_LOG("forward pipeline rejected an incompatible shader program");
                return false;
            }
        }
        return true;
    }

    bool FillViewParameters(
        ShaderParameterStorage& storage,
        const RenderCamera& camera,
        float aspect) {
        if (!storage.SetMatrix4x4(
                "ViewProj",
                camera.ViewCamera->ComputeViewProjMatrix(aspect))) {
            return false;
        }
        const Eigen::Vector3f eye = camera.ViewCamera->GetEyePosition();
        if (!storage.SetFloat4(
                "EyePosition",
                Eigen::Vector4f{eye.x(), eye.y(), eye.z(), 1.0f})) {
            return false;
        }

        vector<SelectedLight> directional;
        vector<SelectedLight> points;
        for (const unique_ptr<LightSceneProxy>& light : camera.RenderScene->Lights()) {
            if (light == nullptr || !light->AffectsWorld()) {
                continue;
            }
            LightRenderParameters parameters;
            light->GetLightRenderParameters(parameters);
            SelectedLight selected{
                .Proxy = light.get(),
                .Parameters = parameters,
                .DistanceSquared =
                    (parameters.WorldPosition - eye).squaredNorm()};
            if (light->GetLightType() == LightType::Directional) {
                directional.push_back(selected);
            } else if (light->GetLightType() == LightType::Point) {
                points.push_back(selected);
            }
        }
        const auto sortByDistance = [](vector<SelectedLight>& lights) {
            std::stable_sort(
                lights.begin(),
                lights.end(),
                [](const SelectedLight& lhs, const SelectedLight& rhs) noexcept {
                    return lhs.DistanceSquared < rhs.DistanceSquared;
                });
        };
        sortByDistance(directional);
        sortByDistance(points);
        if ((directional.size() > kMaxDirectionalLights ||
             points.size() > kMaxPointLights) &&
            !LightOverflowWarned) {
            RADRAY_WARN_LOG("forward pipeline light limit exceeded; nearest supported lights are used");
            LightOverflowWarned = true;
        }
        directional.resize(std::min<size_t>(directional.size(), kMaxDirectionalLights));
        points.resize(std::min<size_t>(points.size(), kMaxPointLights));
        if (!storage.SetUInt(
                "DirectionalLightCount",
                static_cast<uint32_t>(directional.size())) ||
            !storage.SetUInt(
                "PointLightCount",
                static_cast<uint32_t>(points.size()))) {
            return false;
        }
        for (uint32_t index = 0; index < directional.size(); ++index) {
            const LightRenderParameters& light = directional[index].Parameters;
            if (!storage.SetFloat4(
                    "Direction",
                    Eigen::Vector4f{
                        light.Direction.x(),
                        light.Direction.y(),
                        light.Direction.z(),
                        0.0f},
                    index) ||
                !storage.SetFloat4(
                    "Irradiance",
                    Eigen::Vector4f{
                        light.Color.x() * light.DiffuseScale,
                        light.Color.y() * light.DiffuseScale,
                        light.Color.z() * light.DiffuseScale,
                        0.0f},
                    index)) {
                return false;
            }
        }
        for (uint32_t index = 0; index < points.size(); ++index) {
            const SelectedLight& selected = points[index];
            const LightRenderParameters& light = selected.Parameters;
            if (!storage.SetFloat4(
                    "Position",
                    Eigen::Vector4f{
                        light.WorldPosition.x(),
                        light.WorldPosition.y(),
                        light.WorldPosition.z(),
                        selected.Proxy->GetRadius()},
                    index) ||
                !storage.SetFloat4(
                    "Intensity",
                    Eigen::Vector4f{
                        light.Color.x() * light.DiffuseScale,
                        light.Color.y() * light.DiffuseScale,
                        light.Color.z() * light.DiffuseScale,
                        0.0f},
                    index)) {
                return false;
            }
        }
        return true;
    }

    Nullable<ResidentProgramSets*> GetOrCreateProgramSets(
        FlightResources& flight,
        ShaderProgram* program,
        const ShaderParameterBufferLayout& viewBuffer,
        const DynamicCBufferArena::Allocation& viewAllocation,
        const ShaderParameterBufferLayout& objectBuffer,
        const DynamicCBufferArena::Allocation& objectAllocation) {
        // These sets hold raw arena buffer pointers for the lifetime of the flight,
        // which is only sound while the arena never frees a block on Reset().
        // See the MaxResetSize note in BeginFrame.
        RADRAY_ASSERT(flight.Arena != nullptr && flight.Arena->GetMaxResetSize() == 0);
        const auto found = std::find_if(
            flight.ProgramSets.begin(),
            flight.ProgramSets.end(),
            [&](const ResidentProgramSets& sets) noexcept {
                return sets.Program == program &&
                       sets.ViewTarget == viewAllocation.Target &&
                       sets.ObjectTarget == objectAllocation.Target;
            });
        if (found != flight.ProgramSets.end()) {
            return &*found;
        }

        Nullable<unique_ptr<render::ShaderParameterSet>> viewSet =
            Device->CreateShaderParameterSet(render::ShaderParameterSetDescriptor{
                .Layout = program->GetPipelineLayout(),
                .GroupIndex = BindingGroups.ViewGroup});
        Nullable<unique_ptr<render::ShaderParameterSet>> objectSet =
            Device->CreateShaderParameterSet(render::ShaderParameterSetDescriptor{
                .Layout = program->GetPipelineLayout(),
                .GroupIndex = BindingGroups.ObjectGroup});
        if (!viewSet.HasValue() || !objectSet.HasValue() ||
            !viewSet->Set(
                viewBuffer.Binding,
                0,
                render::ShaderBufferBinding{
                    .Target = viewAllocation.Target,
                    .Range = render::BufferRange{0, viewBuffer.Size}}) ||
            !objectSet->Set(
                objectBuffer.Binding,
                0,
                render::ShaderBufferBinding{
                    .Target = objectAllocation.Target,
                    .Range = render::BufferRange{0, objectBuffer.Size}}) ||
            !viewSet->FlushWrites() || !objectSet->FlushWrites()) {
            return nullptr;
        }
        flight.ProgramSets.push_back(ResidentProgramSets{
            .Program = program,
            .ViewTarget = viewAllocation.Target,
            .ObjectTarget = objectAllocation.Target,
            .ViewSet = viewSet.Release(),
            .ObjectSet = objectSet.Release()});
        return &flight.ProgramSets.back();
    }

    bool PrepareCamera(
        RenderPipelineContext& ctx,
        const RenderCamera& camera) {
        Prepared.clear();
        if (!camera.Target.HasValue() || camera.Target.Get()->Window == nullptr ||
            camera.Target.Get()->BackBuffer == nullptr || camera.ViewCamera == nullptr ||
            camera.RenderScene == nullptr || ctx.Frame.FlightIndex() >= Flights.size()) {
            return false;
        }
        const render::TextureDescriptor targetDesc =
            camera.Target.Get()->BackBuffer->GetDesc();
        if (targetDesc.Width == 0 || targetDesc.Height == 0 ||
            !EnsureDepth(
                camera.Target.Get()->Window,
                targetDesc.Width,
                targetDesc.Height,
                targetDesc.SampleCount)) {
            return false;
        }

        DrawList.Collect(camera.RenderScene, camera.ViewCamera->ComputeViewMatrix());
        DrawList.Sort();
        Prepared.reserve(DrawList.Size());
        for (const MeshDrawItem& item : DrawList.Items()) {
            Prepared.push_back(PreparedDraw{.Item = item});
        }

        FlightResources& flight = Flights[ctx.Frame.FlightIndex()];
        if (flight.Arena == nullptr || !flight.Arena->IsValid()) {
            return false;
        }
        DynamicCBufferArena& arena = *flight.Arena;
        vector<ShaderProgram*> programs;
        for (const PreparedDraw& draw : Prepared) {
            ShaderProgram* program = draw.Item.DrawMaterial->GetProgram();
            if (std::find(programs.begin(), programs.end(), program) == programs.end()) {
                programs.push_back(program);
            }
        }
        for (ShaderProgram* program : programs) {
            if (!ValidateProgram(program)) {
                continue;
            }
            const ShaderParameterLayout& layout = program->GetParameterLayout();
            const uint32_t viewBufferIndex =
                FindSingleBuffer(layout, BindingGroups.ViewGroup).value();
            const uint32_t objectBufferIndex =
                FindSingleBuffer(layout, BindingGroups.ObjectGroup).value();
            const ShaderParameterBufferLayout& viewBuffer =
                layout.Buffers()[viewBufferIndex];
            const ShaderParameterBufferLayout& objectBuffer =
                layout.Buffers()[objectBufferIndex];

            ShaderParameterStorage viewValues{&layout};
            if (!FillViewParameters(
                    viewValues,
                    camera,
                    static_cast<float>(targetDesc.Width) /
                        static_cast<float>(targetDesc.Height))) {
                continue;
            }
            const std::optional<DynamicCBufferArena::Allocation> viewAllocation =
                UploadBytes(arena, viewValues.GetBufferData(viewBufferIndex));
            if (!viewAllocation.has_value()) {
                continue;
            }

            vector<size_t> drawIndices;
            for (size_t index = 0; index < Prepared.size(); ++index) {
                if (Prepared[index].Item.DrawMaterial->GetProgram() == program) {
                    drawIndices.push_back(index);
                }
            }
            if (drawIndices.empty()) {
                continue;
            }
            const uint64_t objectStride = Align(
                objectBuffer.Size,
                std::max<uint64_t>(Device->GetDetail().CBufferAlignment, 1));
            const uint64_t objectBytes =
                objectStride * (drawIndices.size() - 1) + objectBuffer.Size;
            DynamicCBufferArena::Reservation objectReservation =
                arena.Reserve(objectBytes);
            if (!objectReservation.IsValid()) {
                continue;
            }
            std::memset(objectReservation.Data(), 0, objectBytes);
            bool objectValuesValid = true;
            for (size_t localIndex = 0; localIndex < drawIndices.size(); ++localIndex) {
                ShaderParameterStorage objectValues{&layout};
                if (!objectValues.SetMatrix4x4(
                        "LocalToWorld",
                        Prepared[drawIndices[localIndex]].Item.LocalToWorld)) {
                    objectValuesValid = false;
                    break;
                }
                const std::span<const byte> data =
                    objectValues.GetBufferData(objectBufferIndex);
                std::memcpy(
                    static_cast<byte*>(objectReservation.Data()) +
                        objectStride * localIndex,
                    data.data(),
                    data.size());
            }
            if (!objectValuesValid) {
                objectReservation.Commit(0);
                continue;
            }
            const DynamicCBufferArena::Allocation objectAllocation =
                objectReservation.Commit(objectBytes);
            if (!objectAllocation.IsValid() ||
                objectAllocation.Offset + objectBytes >
                    std::numeric_limits<uint32_t>::max()) {
                continue;
            }
            const Nullable<ResidentProgramSets*> sets = GetOrCreateProgramSets(
                flight,
                program,
                viewBuffer,
                viewAllocation.value(),
                objectBuffer,
                objectAllocation);
            if (!sets.HasValue()) {
                continue;
            }
            for (size_t localIndex = 0; localIndex < drawIndices.size(); ++localIndex) {
                PreparedDraw& draw = Prepared[drawIndices[localIndex]];
                draw.ViewSet = sets.Get()->ViewSet.get();
                draw.ObjectSet = sets.Get()->ObjectSet.get();
                draw.ViewOffsets = {{.Binding = viewBuffer.Binding,
                                     .Offset = static_cast<uint32_t>(viewAllocation->Offset)}};
                draw.ObjectOffsets = {{.Binding = objectBuffer.Binding,
                                       .Offset = static_cast<uint32_t>(
                                           objectAllocation.Offset + objectStride * localIndex)}};
            }
        }

        vector<Material*> materials;
        for (const PreparedDraw& draw : Prepared) {
            if (draw.ViewSet.HasValue() &&
                std::find(
                    materials.begin(),
                    materials.end(),
                    draw.Item.DrawMaterial) == materials.end()) {
                materials.push_back(draw.Item.DrawMaterial);
            }
        }
        for (Material* material : materials) {
            ShaderProgram* program = material->GetProgram();
            const ShaderParameterLayout& layout = program->GetParameterLayout();
            vector<MaterialBufferBinding> bindings;
            vector<render::ShaderParameterDynamicOffset> offsets;
            bool valid = true;
            for (uint32_t bufferIndex = 0;
                 bufferIndex < layout.Buffers().size();
                 ++bufferIndex) {
                const ShaderParameterBufferLayout& buffer =
                    layout.Buffers()[bufferIndex];
                if (buffer.Group != BindingGroups.MaterialGroup) {
                    continue;
                }
                const std::optional<DynamicCBufferArena::Allocation> allocation =
                    UploadBytes(
                        arena,
                        material->GetParameterStorage().GetBufferData(bufferIndex));
                if (!allocation.has_value()) {
                    valid = false;
                    break;
                }
                bindings.push_back(MaterialBufferBinding{
                    .BufferIndex = bufferIndex,
                    .Value = render::ShaderBufferBinding{
                        .Target = allocation->Target,
                        .Range = render::BufferRange{0, buffer.Size}}});
                offsets.push_back(render::ShaderParameterDynamicOffset{
                    .Binding = buffer.Binding,
                    .Offset = static_cast<uint32_t>(allocation->Offset)});
            }
            const Nullable<render::ShaderParameterSet*> set =
                valid
                    ? material->PrepareParameterSet(
                          ctx.Frame.FlightIndex(),
                          bindings)
                    : nullptr;
            if (!set.HasValue()) {
                continue;
            }
            for (PreparedDraw& draw : Prepared) {
                if (draw.Item.DrawMaterial != material || !draw.ViewSet.HasValue()) {
                    continue;
                }
                draw.MaterialSet = set.Get();
                draw.MaterialOffsets = offsets;
                draw.Valid = true;
            }
        }
        return true;
    }

    bool Execute(
        RenderPipelineContext& ctx,
        const RenderCamera& camera,
        bool transparent) {
        if (!camera.Target.HasValue() || camera.Target.Get()->Window == nullptr ||
            camera.Target.Get()->BackBuffer == nullptr ||
            camera.Target.Get()->BackBufferView == nullptr || Registry == nullptr) {
            return false;
        }
        AppFrameTarget* target = camera.Target.Get();
        const render::TextureDescriptor targetDesc = target->BackBuffer->GetDesc();
        auto depthIt = DepthTargets.find(target->Window);
        if (depthIt == DepthTargets.end() || depthIt->second.View == nullptr) {
            return false;
        }
        DepthTarget& depth = depthIt->second;
        if (depth.State != render::TextureState::DepthWrite) {
            const render::ResourceBarrierDescriptor barrier =
                render::BarrierTextureDescriptor{
                    .Target = depth.Texture.get(),
                    .Before = depth.State,
                    .After = render::TextureState::DepthWrite};
            ctx.Frame.GetCommandBuffer()->ResourceBarrier(
                std::span{&barrier, 1});
            depth.State = render::TextureState::DepthWrite;
        }

        const render::LoadAction load = transparent
                                            ? render::LoadAction::Load
                                            : render::LoadAction::Clear;
        const render::RenderPassColorAttachmentDescriptor colorAttachment{
            .Format = targetDesc.Format,
            .SampleCount = targetDesc.SampleCount,
            .Load = load,
            .Store = render::StoreAction::Store};
        const render::RenderPassDepthStencilAttachmentDescriptor depthAttachment{
            .Format = kForwardDepthFormat,
            .SampleCount = targetDesc.SampleCount,
            .DepthLoad = load,
            .DepthStore = render::StoreAction::Store,
            .StencilLoad = render::LoadAction::DontCare,
            .StencilStore = render::StoreAction::Discard};
        const Nullable<render::RenderPass*> pass =
            Registry->GetOrCreateRenderPass(render::RenderPassDescriptor{
                .ColorAttachments = std::span{&colorAttachment, 1},
                .DepthStencilAttachment = depthAttachment});
        render::TextureView* colorView = target->BackBufferView;
        const Nullable<render::Framebuffer*> framebuffer =
            pass.HasValue()
                ? Registry->GetOrCreateFramebuffer(render::FramebufferDescriptor{
                      .Pass = pass.Get(),
                      .ColorAttachments =
                          std::span<render::TextureView* const>{&colorView, 1},
                      .DepthStencilAttachment = depth.View.get(),
                      .Width = targetDesc.Width,
                      .Height = targetDesc.Height,
                      .Layers = 1})
                : nullptr;
        if (!pass.HasValue() || !framebuffer.HasValue()) {
            return false;
        }

        const render::ColorClearValue colorClear{{0.025f, 0.030f, 0.040f, 1.0f}};
        const render::DepthStencilClearValue depthClear{1.0f, 0};
        Nullable<unique_ptr<render::GraphicsCommandEncoder>> encoder =
            ctx.Frame.GetCommandBuffer()->BeginRenderPass(
                render::RenderPassBeginDescriptor{
                    .Pass = pass.Get(),
                    .Target = framebuffer.Get(),
                    .ColorClearValues = transparent
                                            ? std::span<const render::ColorClearValue>{}
                                            : std::span{&colorClear, 1},
                    .DepthStencilClearValue = transparent
                                                  ? std::nullopt
                                                  : std::optional{
                                                        depthClear},
                    .Name = transparent ? "Forward Transparent" : "Forward Opaque"});
        if (!encoder.HasValue()) {
            return false;
        }
        unique_ptr<render::GraphicsCommandEncoder> graphics = encoder.Release();
        graphics->SetViewport(MakeViewport(
            Device->GetBackend(), targetDesc.Width, targetDesc.Height));
        graphics->SetScissor(Rect{
            0, 0, targetDesc.Width, targetDesc.Height});

        const GraphicsPassState passState{
            vector<render::TextureFormat>{targetDesc.Format},
            kForwardDepthFormat,
            targetDesc.SampleCount,
            pass.Get()};
        for (const PreparedDraw& draw : Prepared) {
            if (!draw.Valid || IsTransparent(draw.Item) != transparent) {
                continue;
            }
            Material* material = draw.Item.DrawMaterial;
            const Nullable<render::GraphicsPipelineState*> pso =
                material->GetProgram()->GetOrCreateGraphicsPipelineState(
                    material->GetPipelineState(),
                    draw.Item.Geometry->VertexLayout,
                    draw.Item.Geometry->Topology,
                    passState);
            if (!pso.HasValue()) {
                continue;
            }
            graphics->BindGraphicsPipelineState(pso.Get());
            graphics->BindShaderParameterSet(
                BindingGroups.ViewGroup,
                draw.ViewSet.Get(),
                draw.ViewOffsets);
            graphics->BindShaderParameterSet(
                BindingGroups.MaterialGroup,
                draw.MaterialSet.Get(),
                draw.MaterialOffsets);
            graphics->BindShaderParameterSet(
                BindingGroups.ObjectGroup,
                draw.ObjectSet.Get(),
                draw.ObjectOffsets);
            const render::VertexBufferBinding vertexBinding{
                .Binding = draw.Item.Geometry->VertexLayout.Buffers.front().Binding,
                .View = draw.Item.Geometry->Vbv};
            graphics->BindVertexBuffers(std::span{&vertexBinding, 1});
            graphics->BindIndexBuffer(draw.Item.Geometry->Ibv);
            graphics->DrawIndexed(
                draw.Item.IndexCount,
                1,
                draw.Item.FirstIndex,
                draw.Item.VertexOffset,
                0);
        }
        ctx.Frame.GetCommandBuffer()->EndRenderPass(std::move(graphics));
        return true;
    }
};

ForwardPipeline::ForwardPipeline(
    Application* app,
    Scene* scene,
    CameraComponent* camera)
    : _impl(make_unique<Impl>(app, scene, camera, this)) {}

ForwardPipeline::~ForwardPipeline() noexcept = default;

void ForwardPipeline::OnBeginFrame(RenderPipelineContext& ctx) {
    _impl->BeginFrame(ctx.Frame);
}

void ForwardPipeline::OnBuildCameraList(
    RenderPipelineContext& ctx,
    RenderCameraList& cameras) {
    cameras.Clear();
    for (RenderPipelineTarget& target : ctx.Targets) {
        cameras.Add(_impl->RenderScene, _impl->ViewCamera, &target.Target);
    }
}

void ForwardPipeline::OnAddRenderPasses(
    RenderPipelineContext& ctx,
    const RenderCamera& camera) {
    if (!_impl->PrepareCamera(ctx, camera)) {
        return;
    }
    EnqueuePass(&_impl->OpaquePass);
    const bool hasTransparent = std::any_of(
        _impl->Prepared.begin(),
        _impl->Prepared.end(),
        [](const Impl::PreparedDraw& draw) noexcept {
            return draw.Valid && IsTransparent(draw.Item);
        });
    if (hasTransparent) {
        EnqueuePass(&_impl->TransparentPass);
    }
}

bool ForwardPipeline::ExecutePreparedPass(
    RenderPipelineContext& ctx,
    const RenderCamera& camera,
    bool transparent) {
    return _impl->Execute(ctx, camera, transparent);
}

render::ShaderProgramLayoutRecipe ForwardPipeline::GetLayoutRecipe() noexcept {
    // Names must match shaderlib/pipelines/forward/bindings.hlsli; a mismatch fails layout
    // resolution instead of silently binding the wrong way.
    static constexpr std::string_view kDynamicBuffers[]{
        "ForwardView",
        "ForwardMaterial",
        "ForwardObject"};
    render::ShaderProgramLayoutRecipe recipe;
    for (const std::string_view name : kDynamicBuffers) {
        const render::ShaderLayoutSelector selector{
            .DeclarationName = string{name},
            .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
        recipe.D3D12.BufferPlacements.push_back(
            {.Selector = selector,
             .Placement = render::D3D12BufferPlacement::RootDescriptor});
        recipe.Vulkan.BufferDescriptors.push_back(
            {.Selector = selector,
             .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    }
    return recipe;
}

}  // namespace radray
