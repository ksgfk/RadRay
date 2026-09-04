#include <radray/runtime/forward_pipeline/forward_pipeline.h>

#include "forward_bindings.h"
#include "forward_frame.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#include <radray/logger.h>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/window_manager.h>

namespace radray {
using namespace forward_detail;
namespace {

constexpr render::TextureFormat kForwardDepthFormat = render::TextureFormat::D32_FLOAT;
constexpr uint32_t kMaxDirectionalLights = 8;
constexpr uint32_t kMaxPointLights = 8;

bool IsTransparent(const MaterialRenderData& material) noexcept {
    return static_cast<int32_t>(material.Queue) >= static_cast<int32_t>(RenderQueue::GeometryLast);
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

bool forward_detail::FillViewParameters(
    ShaderParameterStorage& storage,
    const ForwardFrameInput& input,
    float aspect,
    bool& lightOverflowWarned) {
    struct SelectedLight {
        LightRenderParameters Parameters;
        float Radius;
        float DistanceSquared;
    };
    if (!storage.SetMatrix4x4(
            "ViewProj",
            PerspectiveLH<float>(input.Camera.FovY, aspect, input.Camera.NearZ, input.Camera.FarZ) * input.Camera.View)) {
        return false;
    }
    const Eigen::Vector3f eye = input.Camera.EyePosition;
    if (!storage.SetFloat4(
            "EyePosition",
            Eigen::Vector4f{eye.x(), eye.y(), eye.z(), 1.0f})) {
        return false;
    }

    vector<SelectedLight> directional;
    vector<SelectedLight> points;
    for (const ForwardFrameLight& light : input.Lights) {
        SelectedLight selected{
            .Parameters = light.Parameters,
            .Radius = light.Radius,
            .DistanceSquared = (light.Parameters.WorldPosition - eye).squaredNorm()};
        if (light.Type == LightType::Directional) {
            directional.push_back(selected);
        } else if (light.Type == LightType::Point) {
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
        !lightOverflowWarned) {
        RADRAY_WARN_LOG("forward pipeline light limit exceeded; nearest supported lights are used");
        lightOverflowWarned = true;
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
                    selected.Radius},
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

    struct PreparedDraw {
        ForwardFrameDraw Item;
        ForwardProgramBindings Bindings{};
        Nullable<render::GraphicsPipelineState*> PipelineState{nullptr};
        Nullable<render::ShaderParameterSet*> ViewSet{nullptr};
        Nullable<render::ShaderParameterSet*> MaterialSet{nullptr};
        Nullable<render::ShaderParameterSet*> ObjectSet{nullptr};
        vector<render::ShaderParameterDynamicOffset> ViewOffsets;
        vector<render::ShaderParameterDynamicOffset> MaterialOffsets;
        vector<render::ShaderParameterDynamicOffset> ObjectOffsets;
        bool Valid{false};
    };

    struct FlightResources {
        ForwardFrameInput Input;
        unique_ptr<DynamicCBufferArena> Arena;
        vector<ResidentProgramSets> ProgramSets;
        ForwardMaterialSets MaterialSets;
        vector<PreparedDraw> Prepared;
    };

    Impl(
        Application* application,
        Scene* renderScene,
        CameraComponent* viewCamera)
        : RenderScene(renderScene),
          ViewCamera(viewCamera),
          Device(application->GetDevice()),
          Registry(application->GetRenderSystem()->GetRenderPassRegistry()) {
        const GpuSystem* gpu = application->GetGpuSystem();
        if (gpu != nullptr) {
            Flights.resize(gpu->GetFlightDataCount());
        }
    }

    Scene* RenderScene;
    CameraComponent* ViewCamera;
    render::Device* Device;
    render::RenderPassRegistry* Registry;
    ForwardBindingCache Bindings;
    vector<FlightResources> Flights;
    unordered_map<AppWindow*, DepthTarget> DepthTargets;
    bool LightOverflowWarned{false};

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
            descriptor.MaxResetSize = 1024 * 1024;
            descriptor.NamePrefix = "ForwardPipeline";
            flight.Arena = make_unique<DynamicCBufferArena>(
                Device,
                &frame.GetHostWrites(),
                descriptor);
        }
        flight.Prepared.clear();
        flight.ProgramSets.clear();
        flight.MaterialSets.Clear();
        flight.Arena->Reset();
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

    Nullable<ResidentProgramSets*> GetOrCreateProgramSets(
        FlightResources& flight,
        ShaderProgram* program,
        const ShaderParameterBufferLayout& viewBuffer,
        const DynamicCBufferArena::Allocation& viewAllocation,
        const ShaderParameterBufferLayout& objectBuffer,
        const DynamicCBufferArena::Allocation& objectAllocation) {
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
                .GroupIndex = viewBuffer.Group});
        Nullable<unique_ptr<render::ShaderParameterSet>> objectSet =
            Device->CreateShaderParameterSet(render::ShaderParameterSetDescriptor{
                .Layout = program->GetPipelineLayout(),
                .GroupIndex = objectBuffer.Group});
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

    bool PrepareTarget(RenderPipelineContext& ctx, const AppFrameTarget& target) {
        FlightResources& flight = Flights[ctx.Frame.FlightIndex()];
        const ForwardFrameInput& input = flight.Input;
        vector<PreparedDraw>& Prepared = flight.Prepared;
        Prepared.clear();
        if (target.Window == nullptr || target.BackBuffer == nullptr) {
            return false;
        }
        const render::TextureDescriptor targetDesc =
            target.BackBuffer->GetDesc();
        if (targetDesc.Width == 0 || targetDesc.Height == 0 ||
            !EnsureDepth(
                target.Window,
                targetDesc.Width,
                targetDesc.Height,
                targetDesc.SampleCount)) {
            return false;
        }

        Prepared.reserve(input.Draws.size());
        for (const ForwardFrameDraw& item : input.Draws) {
            Prepared.push_back(PreparedDraw{.Item = item});
        }

        if (flight.Arena == nullptr || !flight.Arena->IsValid()) {
            return false;
        }
        DynamicCBufferArena& arena = *flight.Arena;
        vector<ShaderProgram*> programs;
        for (const PreparedDraw& draw : Prepared) {
            ShaderProgram* program = input.Materials[draw.Item.MaterialIndex].Program.Get();
            if (std::find(programs.begin(), programs.end(), program) == programs.end()) {
                programs.push_back(program);
            }
        }
        for (ShaderProgram* program : programs) {
            const auto resolved = Bindings.Resolve(program);
            if (!resolved.HasValue()) {
                continue;
            }
            const ForwardProgramBindings& bindings = *resolved.Get();
            const ShaderParameterLayout& layout = program->GetParameterLayout();
            const uint32_t viewBufferIndex =
                bindings.ViewBufferIndex;
            const uint32_t objectBufferIndex =
                bindings.ObjectBufferIndex;
            const ShaderParameterBufferLayout& viewBuffer =
                layout.Buffers()[viewBufferIndex];
            const ShaderParameterBufferLayout& objectBuffer =
                layout.Buffers()[objectBufferIndex];

            ShaderParameterStorage viewValues{&layout};
            if (!FillViewParameters(
                    viewValues,
                    input,
                    static_cast<float>(targetDesc.Width) /
                        static_cast<float>(targetDesc.Height),
                    LightOverflowWarned)) {
                continue;
            }
            const std::optional<DynamicCBufferArena::Allocation> viewAllocation =
                UploadBytes(arena, viewValues.GetBufferData(viewBufferIndex));
            if (!viewAllocation.has_value()) {
                continue;
            }

            vector<size_t> drawIndices;
            for (size_t index = 0; index < Prepared.size(); ++index) {
                if (input.Materials[Prepared[index].Item.MaterialIndex].Program.Get() == program) {
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
                draw.Bindings = bindings;
                draw.ViewSet = sets.Get()->ViewSet.get();
                draw.ObjectSet = sets.Get()->ObjectSet.get();
                draw.ViewOffsets = {{.Binding = viewBuffer.Binding,
                                     .Offset = static_cast<uint32_t>(viewAllocation->Offset)}};
                draw.ObjectOffsets = {{.Binding = objectBuffer.Binding,
                                       .Offset = static_cast<uint32_t>(
                                           objectAllocation.Offset + objectStride * localIndex)}};
            }
        }

        for (uint32_t materialIndex = 0; materialIndex < input.Materials.size(); ++materialIndex) {
            const MaterialRenderData& material = input.Materials[materialIndex];
            if (!material.Program.HasValue()) {
                continue;
            }
            ShaderProgram* program = material.Program.Get();
            const auto resolved = Bindings.Resolve(program);
            if (!resolved.HasValue() || material.ParameterGroup != resolved.Get()->MaterialGroup) {
                continue;
            }
            const ShaderParameterLayout& layout = program->GetParameterLayout();
            vector<ForwardBufferBinding> bindings;
            vector<render::ShaderParameterDynamicOffset> offsets;
            bool valid = true;
            for (uint32_t bufferIndex = 0; bufferIndex < layout.Buffers().size(); ++bufferIndex) {
                const ShaderParameterBufferLayout& buffer = layout.Buffers()[bufferIndex];
                if (buffer.Group != material.ParameterGroup) {
                    continue;
                }
                const auto allocation = UploadBytes(arena, material.Parameters.GetBufferData(bufferIndex));
                if (!allocation.has_value()) {
                    valid = false;
                    break;
                }
                const bool dynamic = program->IsBufferDynamic(buffer.Name);
                bindings.push_back(ForwardBufferBinding{
                    .BufferIndex = bufferIndex,
                    .Value = render::ShaderBufferBinding{
                        .Target = allocation->Target,
                        .Range = render::BufferRange{dynamic ? 0 : allocation->Offset, buffer.Size}}});
                if (dynamic) {
                    offsets.push_back(render::ShaderParameterDynamicOffset{
                        .Binding = buffer.Binding,
                        .Offset = static_cast<uint32_t>(allocation->Offset)});
                }
            }
            const auto set = valid ? flight.MaterialSets.GetOrCreate(materialIndex, material, bindings) : nullptr;
            if (!set.HasValue()) {
                continue;
            }
            for (PreparedDraw& draw : Prepared) {
                if (draw.Item.MaterialIndex != materialIndex || !draw.ViewSet.HasValue()) {
                    continue;
                }
                draw.MaterialSet = set.Get();
                draw.MaterialOffsets = offsets;
                draw.Valid = true;
            }
        }
        return true;
    }

    bool Execute(RenderPipelineContext& ctx, AppFrameTarget& frameTarget, bool transparent) {
        if (frameTarget.Window == nullptr || frameTarget.BackBuffer == nullptr ||
            frameTarget.BackBufferView == nullptr || Registry == nullptr) {
            return false;
        }
        FlightResources& flight = Flights[ctx.Frame.FlightIndex()];
        const ForwardFrameInput& input = flight.Input;
        vector<PreparedDraw>& Prepared = flight.Prepared;
        AppFrameTarget* target = &frameTarget;
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

        const GraphicsPassState passState{
            vector<render::TextureFormat>{targetDesc.Format}, kForwardDepthFormat, targetDesc.SampleCount, pass.Get()};
        for (PreparedDraw& draw : Prepared) {
            const MaterialRenderData& material = input.Materials[draw.Item.MaterialIndex];
            if (draw.Valid && IsTransparent(material) == transparent) {
                draw.PipelineState = material.Program.Get()->GetOrCreateGraphicsPipelineState(
                    material.PipelineState, draw.Item.Geometry->VertexLayout, draw.Item.Geometry->Topology, passState);
            }
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

        for (const PreparedDraw& draw : Prepared) {
            if (!draw.Valid || !draw.PipelineState.HasValue() || IsTransparent(input.Materials[draw.Item.MaterialIndex]) != transparent) {
                continue;
            }
            graphics->BindGraphicsPipelineState(draw.PipelineState.Get());
            graphics->BindShaderParameterSet(
                draw.Bindings.ViewGroup,
                draw.ViewSet.Get(),
                draw.ViewOffsets);
            graphics->BindShaderParameterSet(
                draw.Bindings.MaterialGroup,
                draw.MaterialSet.Get(),
                draw.MaterialOffsets);
            graphics->BindShaderParameterSet(
                draw.Bindings.ObjectGroup,
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
    : _impl(make_unique<Impl>(app, scene, camera)) {}

ForwardPipeline::~ForwardPipeline() noexcept = default;

void ForwardPipeline::PrepareFrame(const AppUpdateContext& ctx, vector<StreamingAssetRefAny>& retainedAssets) {
    RADRAY_ASSERT(ctx.FlightIndex < _impl->Flights.size());
    CollectFrameInput(_impl->RenderScene, _impl->ViewCamera, _impl->Flights[ctx.FlightIndex].Input, retainedAssets);
}

void ForwardPipeline::Render(RenderPipelineContext& ctx) {
    if (!_impl->BeginFrame(ctx.Frame)) {
        return;
    }
    for (RenderPipelineTarget& target : ctx.Targets) {
        if (!_impl->PrepareTarget(ctx, target.Target)) {
            continue;
        }
        if (!_impl->Execute(ctx, target.Target, false)) {
            continue;
        }
        target.ContentDrawn = true;
        const auto& flight = _impl->Flights[ctx.Frame.FlightIndex()];
        const bool hasTransparent = std::any_of(flight.Prepared.begin(), flight.Prepared.end(),
                                                [&](const Impl::PreparedDraw& draw) {
                                                    return draw.Valid && IsTransparent(flight.Input.Materials[draw.Item.MaterialIndex]);
                                                });
        if (hasTransparent) {
            _impl->Execute(ctx, target.Target, true);
        }
    }
}

const forward_detail::ForwardFrameInput& ForwardPipeline::GetFrameInput(uint32_t flightIndex) const noexcept {
    return _impl->Flights[flightIndex].Input;
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
