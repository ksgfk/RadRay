#include "atrium_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <radray/file.h>
#include <radray/image_data.h>
#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/shader_jit.h>

namespace radray::atrium {
namespace {

render::ShaderProgramLayoutRecipe DynamicRecipe(std::string_view name) {
    render::ShaderProgramLayoutRecipe recipe;
    const render::ShaderLayoutSelector selector{string{name}, shader::ShaderBindingKind::CBuffer};
    recipe.D3D12.BufferPlacements.push_back({selector, render::D3D12BufferPlacement::RootDescriptor});
    recipe.Vulkan.BufferDescriptors.push_back({selector, render::VulkanBufferDescriptorPlacement::Dynamic});
    return recipe;
}

MaterialPipelineState FlatState(bool blend = false, bool depth = false) {
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    state.Primitive.UnclippedDepth = false;
    state.DepthStencil.DepthTestEnable = depth;
    state.DepthStencil.DepthWriteEnable = false;
    state.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
    if (blend) {
        state.Blend = render::BlendState::Default();
        state.Blend->Color = {render::BlendFactor::SrcAlpha, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add};
    }
    return state;
}

class AtriumMeshProcessor final : public MeshPassProcessor {
public:
    AtriumMeshProcessor(FrameDrawResources& resources, const Settings& settings) : _resources(resources), _settings(settings) {}
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene, const MeshBatch& batch, MeshPassDrawListContext& out) override {
        const auto pass = scene.Materials[batch.Material].FindPass(desc.MaterialPassName);
        if (!pass || !batch.Geometry || !ValidateMeshGeometry(*batch.Geometry.Get(), batch.FirstIndex, batch.IndexCount)) {
            out.Reject(MeshPassRejectReason::InvalidGeometry);
            return;
        }
        auto* program = pass->Program.Get();
        const auto& layout = program->GetParameterLayout();
        const auto findGroup = [&](std::string_view name) -> std::optional<uint32_t> {
            for (const auto& b : layout.Buffers())
                if (b.Name == name) return b.Group;
            return std::nullopt;
        };
        const auto vg = findGroup("ForwardView"), og = findGroup("ForwardObject");
        if (!vg || !og) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        auto [view, added] = _views.try_emplace(program, std::nullopt);
        if (added) {
            ShaderParameterStorage storage{&layout, *vg};
            bool valid = storage.SetMatrix4x4("ForwardView.ViewProj", desc.View->ViewProjection);
            if (pass->ParameterGroup) {
                const auto& eye = desc.View->WorldPosition;
                valid &= storage.SetFloat4("ForwardView.EyePosition", {eye.x(), eye.y(), eye.z(), 1});
                uint32_t directions = 0, points = 0;
                auto lights = desc.Culling->Lights;
                std::stable_sort(lights.begin(), lights.end(), [](const auto& a, const auto& b) { return a.DistanceSquared < b.DistanceSquared; });
                for (const auto& visible : lights) {
                    const auto& light = scene.Lights[visible.Light];
                    const auto& p = light.Parameters;
                    const auto color = (p.Color * p.DiffuseScale).eval();
                    if (light.Type == LightType::Directional && directions < 8) {
                        valid &= storage.SetFloat4("ForwardView.DirectionalLights.Direction", {p.Direction.x(), p.Direction.y(), p.Direction.z(), 0}, directions);
                        valid &= storage.SetFloat4("ForwardView.DirectionalLights.Irradiance", {color.x(), color.y(), color.z(), 0}, directions++);
                    } else if (light.Type == LightType::Point && points < 8) {
                        valid &= storage.SetFloat4("ForwardView.PointLights.Position", {p.WorldPosition.x(), p.WorldPosition.y(), p.WorldPosition.z(), light.WorldBounds.Radius}, points);
                        valid &= storage.SetFloat4("ForwardView.PointLights.Intensity", {color.x(), color.y(), color.z(), 0}, points++);
                    }
                }
                valid &= storage.SetUInt("ForwardView.DirectionalLightCount", directions);
                valid &= storage.SetUInt("ForwardView.PointLightCount", points);
            }
            if (valid) view->second = _resources.PrepareGroup(*program, *vg, storage);
        }
        if (!view->second) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        ShaderParameterStorage object{&layout, *og};
        if (!object.SetMatrix4x4("ForwardObject.LocalToWorld", scene.Primitives[batch.Primitive].LocalToWorld)) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        auto objectGroup = _resources.PrepareGroup(*program, *og, object);
        if (!objectGroup) {
            out.Reject(MeshPassRejectReason::PrepareResourceFailed);
            return;
        }
        MeshDrawCommand command;
        command.Program = program;
        command.Geometry = batch.Geometry;
        command.FirstIndex = batch.FirstIndex;
        command.IndexCount = batch.IndexCount;
        command.VertexOffset = batch.VertexOffset;
        command.PipelineState = pass->PipelineState;
        command.PipelineState.Primitive.Poly = _settings.Wireframe ? render::PolygonMode::Line : render::PolygonMode::Fill;
        command.PipelineState.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
        command.PipelineState.DepthStencil.DepthWriteEnable = RenderQueueRange::Opaque().Contains(scene.Materials[batch.Material].Queue);
        command.Groups = {*view->second, *objectGroup};
        if (pass->ParameterGroup) {
            auto [material, first] = _materials.try_emplace(batch.Material, std::nullopt);
            if (first) material->second = _resources.PrepareGroup(*program, *pass->ParameterGroup, pass->Parameters, pass->Textures, pass->Samplers);
            if (!material->second) {
                out.Reject(MeshPassRejectReason::PrepareResourceFailed);
                return;
            }
            command.Groups.push_back(*material->second);
        }
        if (!FinalizeMeshDrawCommand(command)) {
            out.Reject(MeshPassRejectReason::InvalidBindings);
            return;
        }
        out.AddCommand(std::move(command));
    }

private:
    FrameDrawResources& _resources;
    const Settings& _settings;
    unordered_map<ShaderProgram*, std::optional<PreparedShaderGroup>> _views;
    unordered_map<uint32_t, std::optional<PreparedShaderGroup>> _materials;
};

struct HudVertex {
    float X, Y, U, V, R, G, B, A;
};
struct PreparedView {
    ResolvedRenderView View;
    CullingResults Culling;
    RendererList Depth, Opaque, Transparent;
};
struct SignalProgram {
    unique_ptr<render::PipelineLayout> Layout;
    unique_ptr<render::Shader> Shader;
    unique_ptr<render::ComputePipelineState> Pso;
    ShaderParameterLayout Parameters;
    uint32_t ParameterBuffer{0};
};

std::optional<SignalProgram> CompileSignal(Application& app) {
    const auto path = app.GetShaderSourceRoot() / "examples/example_tidal_atrium/shaders/signal.hlsl";
    auto source = ReadBinaryFile(path);
    if (!source) return std::nullopt;
    ShaderJit jit{app.GetShaderIncludePaths()};
    const auto target = render::GetShaderTargetForBackend(app.GetDevice()->GetBackend());
    if (!target) return std::nullopt;
    const auto hash = jit.DiscoverContractHash("examples/example_tidal_atrium/shaders/signal.hlsl", *source, *target);
    if (!hash) return std::nullopt;
    shader::CompileVariantRequest request{};
    request.SourceName = "examples/example_tidal_atrium/shaders/signal.hlsl";
    request.RootSource = std::move(*source);
    request.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(*target));
    request.ExpectedContract = *hash;
    const auto compiled = jit.Compile(request, *target);
    if (!compiled) return std::nullopt;
    auto artifact = render::CreateBackendShaderArtifact(*app.GetDevice(), compiled->Metadata, {*target, compiled->ExpectedGpuArtifact});
    if (!artifact) return std::nullopt;
    auto parameters = ShaderParameterLayout::Create(*artifact);
    if (!parameters || !parameters->Find("SignalFrame.State")) return std::nullopt;
    const uint32_t parameterBuffer = parameters->Find("SignalFrame.State")->BufferIndex;
    const auto bytes = artifact->Generic().FindStageBytecode(shader::ShaderStage::Compute);
    if (!bytes) return std::nullopt;
    auto shader = app.GetDevice()->CreateShader({*bytes, artifact->Category, render::ShaderStage::Compute});
    if (!shader) return std::nullopt;
    SignalProgram result{std::move(artifact->Layout), shader.Release(), {}, std::move(*parameters), parameterBuffer};
    auto pso = app.GetDevice()->CreateComputePipelineState({result.Layout.get(), {result.Shader.get(), "CSMain"}});
    if (!pso) return std::nullopt;
    result.Pso = pso.Release();
    return result;
}

}  // namespace

struct AtriumPipeline::Impl {
    struct Flight {
        explicit Flight(render::Device* device) : Resources(device) {}
        ~Flight() noexcept {
            Views.clear();
            ExtraSets.clear();
        }
        Settings State;
        RenderSceneSnapshot Scene;
        FrameDrawResources Resources;
        vector<PreparedView> Views;
        vector<unique_ptr<render::ShaderParameterSet>> ExtraSets;
        unique_ptr<render::Buffer> HudBuffer, SignalBuffer, Readback;
        std::optional<RenderExternalBuffer> HudImport, SignalImport, CaptureImport;
        Nullable<TextureAsset*> Font{nullptr};
        vector<HudVertex> HudVertices;
        uint32_t CaptureWidth{0}, CaptureHeight{0};
        uint64_t CapturePitch{0};
        string PendingCapture, Report;
        DrawExecutionStats Execution;
        bool HistoryValid{false};
    };
    Application* App;
    Scene* Source;
    CameraComponent* Camera;
    render::Device* Device;
    StreamingAssetRef<TextureAsset> Font;
    std::filesystem::path CaptureDirectory;
    vector<unique_ptr<Flight>> Flights;
    array<ViewStateId, 3> Ids{AllocateViewStateId(), AllocateViewStateId(), AllocateViewStateId()};
    Nullable<ShaderProgram*> Sky{nullptr}, Panel{nullptr}, Hud{nullptr};
    std::optional<SignalProgram> Signal;
    unique_ptr<render::Texture> CameraTexture;
    unique_ptr<render::TextureView> CameraRtv;
    RenderOutputId CameraOutput;
    std::atomic_bool Error{false};
    RenderGraphExecutionReport LastReport;

    Impl(Application* app, Scene* scene, CameraComponent* camera, StreamingAssetRef<TextureAsset> font, std::filesystem::path captures)
        : App(app), Source(scene), Camera(camera), Device(app->GetDevice()), Font(std::move(font)), CaptureDirectory(std::move(captures)) {
        auto* renderSystem = App->GetRenderSystem();
        Sky = renderSystem->GetOrCreateShaderProgram({.SourceName = "examples/example_tidal_atrium/shaders/sky.hlsl", .LayoutRecipe = DynamicRecipe("SkyFrame")});
        Panel = renderSystem->GetOrCreateShaderProgram({.SourceName = "examples/example_tidal_atrium/shaders/panel.hlsl", .LayoutRecipe = DynamicRecipe("PanelFrame")});
        Hud = renderSystem->GetOrCreateShaderProgram({.SourceName = "examples/example_tidal_atrium/shaders/hud.hlsl", .LayoutRecipe = DynamicRecipe("HudFrame")});
        Signal = CompileSignal(*app);
        auto texture = Device->CreateTexture({render::TextureDimension::Dim2D, 512, 384, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource});
        if (texture) {
            CameraTexture = texture.Release();
            auto view = Device->CreateTextureView({CameraTexture.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget});
            if (view) {
                CameraRtv = view.Release();
                CameraOutput = renderSystem->GetOutputs().RegisterExternal({"Atrium live orthographic camera", CameraTexture.get(), CameraRtv.get()});
            }
        }
        for (uint32_t i = 0; i < App->GetGpuSystem()->GetFlightDataCount(); ++i) Flights.push_back(make_unique<Flight>(Device));
        Error = !Sky || !Panel || !Hud || !Signal || !CameraOutput.IsValid();
    }

    ~Impl() {
        Flights.clear();
        if (CameraOutput.IsValid()) App->GetRenderSystem()->GetOutputs().Unregister(CameraOutput);
        if (CameraRtv) App->GetRenderSystem()->GetRenderPassRegistry()->RemoveFramebuffersUsing(CameraRtv.get());
    }

    Nullable<render::ShaderParameterSet*> TextureSet(Flight& flight, render::PipelineLayout* layout,
                                                     std::string_view textureName, std::string_view samplerName, render::TextureView* texture) {
        auto set = Device->CreateShaderParameterSet({layout, 1});
        const auto sampler = Device->GetOrCreateSampler({.AddressS = render::AddressMode::ClampToEdge, .AddressT = render::AddressMode::ClampToEdge, .MinFilter = render::FilterMode::Linear, .MagFilter = render::FilterMode::Linear});
        if (!set || !sampler || !set->Set(layout->FindBinding(textureName), 0, texture) ||
            !set->Set(layout->FindBinding(samplerName), 0, sampler.Get()) || !set->FlushWrites()) {
            Error = true;
            return nullptr;
        }
        auto* pointer = set.Get();
        flight.ExtraSets.push_back(set.Release());
        return pointer;
    }

    std::optional<PreparedShaderGroup> PanelValues(Flight& flight, const Eigen::Matrix4f& matrix, const Eigen::Vector4f& tint = Eigen::Vector4f::Ones()) {
        ShaderParameterStorage values{&Panel->GetParameterLayout(), 0};
        if (!values.SetMatrix4x4("PanelFrame.Transform", matrix) || !values.SetFloat4("PanelFrame.Tint", tint)) return std::nullopt;
        return flight.Resources.PrepareGroup(*Panel.Get(), 0, values);
    }

    void DrawPanel(Flight& flight, RenderGraphRasterContext& ctx, render::TextureView* texture, const PreparedShaderGroup& values, bool depth) {
        const auto pso = Panel->GetOrCreateGraphicsPipelineState(FlatState(false, depth), {}, PrimitiveTopology::TriangleList, ctx.PassState());
        const auto set = TextureSet(flight, Panel->GetPipelineLayout(), "PanelTexture", "PanelSampler", texture);
        if (!pso || !set) {
            Error = true;
            return;
        }
        ctx.Encoder().BindGraphicsPipelineState(pso.Get());
        ctx.Encoder().BindShaderParameterSet(0, values.Set.Get(), values.DynamicOffsets);
        ctx.Encoder().BindShaderParameterSet(1, set.Get());
        ctx.Encoder().Draw(6, 1, 0, 0);
    }

    bool WriteUpload(unique_ptr<render::Buffer>& buffer, std::span<const byte> bytes, render::BufferUses usage) {
        if (!buffer || buffer->GetDesc().Size < bytes.size()) {
            auto created = Device->CreateBuffer({std::max<uint64_t>(256, bytes.size()), render::MemoryType::Upload, usage | render::BufferUse::MapWrite, {}});
            if (!created) {
                Error = true;
                return false;
            }
            buffer = created.Release();
        }
        ScopedBufferMap map{buffer.get(), {0, bytes.size()}};
        if (!map.Data()) {
            Error = true;
            return false;
        }
        std::memcpy(map.Data(), bytes.data(), bytes.size());
        return true;
    }

    RgTextureHandle AddSignal(RenderPipelineContext& context, RenderGraph& graph, Flight& flight, const ResolvedRenderViewFamily& family) {
        HistoryTextureRequest request{"Tidal signal", "Tidal feedback history", {}};
        request.Desc.Extent = {RenderExtentMode::Absolute, 512, 320};
        request.Desc.Format = render::TextureFormat::RGBA32_FLOAT;
        request.Desc.Usage = render::TextureUse::UnorderedAccess | render::TextureUse::Resource;
        string reason;
        auto history = context.AcquireHistoryTexture(family.Views.front(), family, request, reason);
        if (!history.Current) {
            RADRAY_ERR_LOG("Atrium history: {}", reason);
            Error = true;
            return {};
        }
        flight.HistoryValid = history.PreviousValid;
        const auto current = graph.ImportTexture(*history.Current.Get(), "Signal history current", RenderGraphExternalAccess::ObservableOutput);
        RgTextureHandle previous;
        if (history.PreviousValid)
            previous = graph.ImportTexture(*history.Previous.Get(), "Signal history previous", RenderGraphExternalAccess::ReadOnly);
        else {
            previous = graph.CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource}, "History reset black");
            graph.AddRasterPass<uint32_t>("Signal.Reset", [&](uint32_t&, RenderGraphRasterBuilder& b) { b.SetColorAttachment(0, previous); }, +[](const uint32_t&, RenderGraphRasterContext&) {});
        }
        ShaderParameterStorage constants{&Signal->Parameters, 0};
        if (!constants.SetFloat4("SignalFrame.State", {flight.State.Time, flight.State.History && history.PreviousValid ? 1.f : 0.f, flight.State.Paused ? 1.f : 0.f, history.PreviousValid ? 1.f : 0.f})) {
            Error = true;
            return {};
        }
        if (!WriteUpload(flight.SignalBuffer, constants.GetBufferData(Signal->ParameterBuffer), render::BufferUse::CBuffer)) return {};
        flight.SignalImport = RenderExternalBuffer{flight.SignalBuffer.get(), flight.SignalBuffer->GetDesc(), render::BufferState::HostWrite, true};
        const auto buffer = graph.ImportBuffer(*flight.SignalImport, "Signal constants", RenderGraphExternalAccess::ReadOnly);
        struct Pass {
            Impl* Self;
            Flight* Frame;
            RgTextureViewHandle Current, Previous;
            RgBufferHandle Constants;
        };
        graph.AddComputePass<Pass>("Signal.ComputeFeedback", [&](Pass& p, RenderGraphComputeBuilder& b) { p = {this, &flight, b.WriteTexture(current), b.ReadTexture(previous), b.ReadBuffer(buffer, RgBufferAccess::Constant)}; }, +[](const Pass& p, RenderGraphComputeContext& ctx) {
            auto& self=*p.Self; auto* layout=self.Signal->Layout.get();
            auto constants=self.Device->CreateShaderParameterSet({layout,0});
            auto textures=self.Device->CreateShaderParameterSet({layout,1});
            const auto sampler=self.Device->GetOrCreateSampler({.AddressS=render::AddressMode::ClampToEdge,.AddressT=render::AddressMode::ClampToEdge,.MinFilter=render::FilterMode::Linear,.MagFilter=render::FilterMode::Linear});
            if (!constants || !textures || !sampler ||
                !constants->Set(layout->FindBinding("SignalFrame"),0,render::ShaderBufferBinding{ctx.GetBuffer(p.Constants),{0,self.Signal->Parameters.Buffers()[self.Signal->ParameterBuffer].Size}}) ||
                !textures->Set(layout->FindBinding("SignalOutput"),0,ctx.GetTextureView(p.Current)) ||
                !textures->Set(layout->FindBinding("SignalPrevious"),0,ctx.GetTextureView(p.Previous)) ||
                !textures->Set(layout->FindBinding("SignalSampler"),0,sampler.Get()) || !constants->FlushWrites() || !textures->FlushWrites()) { self.Error=true; return; }
            ctx.Encoder().BindComputePipelineState(self.Signal->Pso.get());
            ctx.Encoder().BindShaderParameterSet(0,constants.Get()); ctx.Encoder().BindShaderParameterSet(1,textures.Get());
            ctx.Encoder().Dispatch(64,40,1);
            p.Frame->ExtraSets.push_back(constants.Release()); p.Frame->ExtraSets.push_back(textures.Release()); });
        return current;
    }

    void AddWorldScreens(RenderGraph& graph, Flight& flight, const ResolvedRenderViewFamily& family, RgTextureHandle color, RgTextureHandle depth, RgTextureHandle signal, RgTextureHandle observer, size_t first) {
        for (size_t i = 0; i < family.Views.size(); ++i) {
            const auto& view = flight.Views[first + i].View;
            for (int screen = 0; screen < 2; ++screen) {
                Eigen::Matrix4f world = Eigen::Matrix4f::Identity();
                world(0, 0) = 6.6f;
                world(1, 1) = 4.0f;
                world(0, 3) = screen ? 9.f : -9.f;
                world(1, 3) = 3;
                world(2, 3) = 19.6f;
                const auto values = PanelValues(flight, (view.ViewProjection * world).eval());
                if (!values) {
                    Error = true;
                    return;
                }
                struct Pass {
                    Impl* Self;
                    Flight* Frame;
                    RgTextureViewHandle Source;
                    PreparedShaderGroup Values;
                    Rect Viewport, Scissor;
                };
                graph.AddRasterPass<Pass>(screen ? "Gallery.LiveCameraScreen" : "Gallery.FeedbackScreen", [&](Pass& p, RenderGraphRasterBuilder& b) {
                    p={this,&flight,b.ReadTexture(screen?observer:signal),*values,view.ViewRect,view.ScissorRect};
                    b.SetColorAttachment(0,color,{.Load=render::LoadAction::Load}); b.SetDepthAttachment(depth,{.Load=render::LoadAction::Load,.ReadOnly=true}); }, +[](const Pass& p, RenderGraphRasterContext& ctx) {
                    ctx.Encoder().SetViewport(MakeViewport(p.Self->Device->GetBackend(),float(p.Viewport.X),float(p.Viewport.Y),float(p.Viewport.Width),float(p.Viewport.Height)));
                    ctx.Encoder().SetScissor(p.Scissor); p.Self->DrawPanel(*p.Frame,ctx,ctx.GetTextureView(p.Source),p.Values,true); });
            }
        }
    }

    void HudQuad(Flight& frame, float x, float y, float w, float h, const Eigen::Vector4f& color, float u0, float v0, float u1, float v1) {
        const HudVertex vertices[]{
            {x, y, u0, v0, color.x(), color.y(), color.z(), color.w()}, {x + w, y, u1, v0, color.x(), color.y(), color.z(), color.w()}, {x + w, y + h, u1, v1, color.x(), color.y(), color.z(), color.w()}, {x, y, u0, v0, color.x(), color.y(), color.z(), color.w()}, {x + w, y + h, u1, v1, color.x(), color.y(), color.z(), color.w()}, {x, y + h, u0, v1, color.x(), color.y(), color.z(), color.w()}};
        frame.HudVertices.insert(frame.HudVertices.end(), std::begin(vertices), std::end(vertices));
    }
    void HudRect(Flight& frame, float x, float y, float w, float h, const Eigen::Vector4f& color) {
        HudQuad(frame, x, y, w, h, color, 510.f / 512, 190.f / 192, 510.f / 512, 190.f / 192);
    }
    void HudText(Flight& frame, float x, float y, std::string_view text, float size, const Eigen::Vector4f& color) {
        for (unsigned char c : text) {
            if (c < 32 || c > 126) c = '?';
            const uint32_t index = c - 32;
            HudQuad(frame, x, y, size * 32 / 24, size * 32 / 24, color, float(index % 16) / 16, float(index / 16) / 6, float(index % 16 + 1) / 16, float(index / 16 + 1) / 6);
            x += size * .55f;
        }
    }
    void AddHud(RenderGraph& graph, Flight& flight, const ResolvedRenderViewFamily& family, RgTextureHandle color, RgTextureHandle signal, RgTextureHandle observer, size_t first) {
        const float width = std::max(1100.f, float(family.OutputSize.Width)), height = std::max(720.f, float(family.OutputSize.Height));
        flight.HudVertices.clear();
        const Eigen::Vector4f white{.88f, .88f, .80f, 1}, muted{.48f, .66f, .68f, 1}, teal{.22f, .88f, .79f, 1}, back{.015f, .032f, .043f, .86f};
        HudRect(flight, 20, 20, width - 40, 82, back);
        HudRect(flight, 36, 36, 4, 48, teal);
        HudText(flight, 53, 31, "T I D A L  /  A T R I U M", 27, white);
        HudText(flight, 55, 69, "A FIELD GUIDE TO LIGHT     /     RADRAY", 13, muted);
        HudText(flight, width - 228, 39, fmt::format("{}  /  {:.0f} FPS", Device->GetBackend(), flight.State.Fps), 14, teal);
        HudText(flight, width - 228, 68, flight.State.Paused ? "TIME HELD  /  SPACE" : "LIVE LIGHT MUSEUM", 12, muted);
        const array<std::string_view, 5> names{"01  LIGHT COURT", "02  CHROMATIC WALK", "03  MATERIAL LIBRARY", "04  SIGNAL GARDEN", "05  OBSERVATORY"};
        const array<std::string_view, 5> descriptions{
            "Orbiting lights / depth prepass / animated transforms",
            "Layered glass / back-to-front transparency",
            "Mips + sampling / two vertex streams / two sections",
            "Compute feedback / persistent history / live camera",
            "Perspective + orthographic / culling / layer masks"};
        HudRect(flight, 20, height - 165, 590, 145, back);
        HudText(flight, 36, height - 151, names[flight.State.Station], 20, white);
        HudText(flight, 36, height - 119, descriptions[flight.State.Station], 13, muted);
        HudText(flight, 36, height - 88, "WASD move   RMB / arrows look   Q/E down/up   SHIFT fast", 14, white);
        HudText(flight, 36, height - 61, "1-5 viewpoints   SPACE pause   H details   TAB hide UI", 13, teal);
        if (flight.State.Help) {
            HudRect(flight, 20, 117, 431, 119, back);
            HudText(flight, 36, 129, fmt::format("F2 depth {:3}   F3 wireframe {:3}", flight.State.Depth ? "ON" : "OFF", flight.State.Wireframe ? "ON" : "OFF"), 14, white);
            HudText(flight, 36, 154, fmt::format("F4 split {:3}   F5 beacons   {:3}", flight.State.Split ? "ON" : "OFF", flight.State.Beacons ? "ON" : "OFF"), 14, white);
            HudText(flight, 36, 179, fmt::format("F6 history {:3}   history {}", flight.State.History ? "ON" : "OFF", flight.HistoryValid ? "valid" : "reset"), 14, teal);
            HudText(flight, 36, 207, fmt::format("F7 resolution {:.0f}%   ESC release / quit", flight.State.RenderScale * 100), 12, muted);
        }
        const auto& main = flight.Views[first];
        HudRect(flight, width - 254, 117, 234, 346, back);
        HudText(flight, width - 244, 128, "LIVE / ORTHOGRAPHIC", 13, teal);
        HudText(flight, width - 244, 308, "SIGNAL / TEMPORAL INK", 13, teal);
        HudRect(flight, width - 254, 476, 234, 118, back);
        HudText(flight, width - 244, 487, fmt::format("VISIBLE {:3} / {}", main.Culling.Stats.VisiblePrimitives, flight.Scene.Primitives.size()), 13, white);
        HudText(flight, width - 244, 510, fmt::format("DEPTH {:3}  LIT {:3}  T {:2}", main.Depth.Commands.size(), main.Opaque.Commands.size(), main.Transparent.Commands.size()), 12, muted);
        HudText(flight, width - 244, 533, fmt::format("GRAPH {} PASSES / {} BARRIERS", LastReport.LivePasses, LastReport.TransitionBarriers), 12, muted);
        HudText(flight, width - 244, 556, fmt::format("POOL {} HITS / {} CREATED", LastReport.Pool.Hits, LastReport.Pool.Created), 12, muted);
        if (flight.State.Split) {
            HudText(flight, 36, 251, "ALL LAYERS", 16, teal);
            HudText(flight, width * .5f + 16, 251, "ARCHITECTURE ONLY", 16, teal);
            HudRect(flight, width * .5f - 1, 245, 2, height - 421, teal);
        }
        if (!WriteUpload(flight.HudBuffer, std::as_bytes(std::span{flight.HudVertices}), render::BufferUse::Vertex)) return;
        flight.HudImport = RenderExternalBuffer{flight.HudBuffer.get(), flight.HudBuffer->GetDesc(), render::BufferState::HostWrite, true};
        const auto buffer = graph.ImportBuffer(*flight.HudImport, "HUD vertices", RenderGraphExternalAccess::ReadOnly);
        ShaderParameterStorage storage{&Hud->GetParameterLayout(), 0};
        if (!storage.SetFloat4("HudFrame.Size", {width, height, 0, 0})) {
            Error = true;
            return;
        }
        const auto values = flight.Resources.PrepareGroup(*Hud.Get(), 0, storage);
        if (!values) {
            Error = true;
            return;
        }
        struct Pass {
            Impl* Self;
            Flight* Frame;
            RgBufferHandle Vertices;
            PreparedShaderGroup Values;
            uint32_t Width, Height;
        };
        graph.AddRasterPass<Pass>("Atrium.Interface", [&](Pass& p, RenderGraphRasterBuilder& b) {
            p={this,&flight,b.ReadBuffer(buffer,RgBufferAccess::Vertex),*values,family.RenderSize.Width,family.RenderSize.Height}; b.SetColorAttachment(0,color,{.Load=render::LoadAction::Load}); }, +[](const Pass& p, RenderGraphRasterContext& ctx) {
            PrimitiveVertexLayout layout;
            layout.Buffers={{0,sizeof(HudVertex),render::VertexStepMode::Vertex}};
            layout.Attributes={{"POSITION",0,0,0,render::VertexFormat::FLOAT32X2},{"TEXCOORD",0,0,8,render::VertexFormat::FLOAT32X2},{"COLOR",0,0,16,render::VertexFormat::FLOAT32X4}};
            const auto pso=p.Self->Hud->GetOrCreateGraphicsPipelineState(FlatState(true),layout,PrimitiveTopology::TriangleList,ctx.PassState());
            const auto set=p.Self->TextureSet(*p.Frame,p.Self->Hud->GetPipelineLayout(),"FontTexture","FontSampler",p.Frame->Font->GetSrv());
            if (!pso || !set) { p.Self->Error=true; return; }
            ctx.Encoder().SetViewport(MakeViewport(p.Self->Device->GetBackend(),0,0,float(p.Width),float(p.Height)));
            ctx.Encoder().SetScissor({0,0,p.Width,p.Height}); ctx.Encoder().BindGraphicsPipelineState(pso.Get());
            ctx.Encoder().BindShaderParameterSet(0,p.Values.Set.Get(),p.Values.DynamicOffsets); ctx.Encoder().BindShaderParameterSet(1,set.Get());
            const render::VertexBufferBinding vb{0,{ctx.GetBuffer(p.Vertices),0,p.Frame->HudVertices.size()*sizeof(HudVertex)}};
            ctx.Encoder().BindVertexBuffers(std::span{&vb,1}); ctx.Encoder().Draw(uint32_t(p.Frame->HudVertices.size()),1,0,0); });
        for (int screen = 0; screen < 2; ++screen) {
            const float x = width - 244, y = screen ? 331.f : 150.f, w = 214, h = screen ? 122.f : 150.f;
            Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
            matrix(0, 0) = 2 * w / width;
            matrix(1, 1) = 2 * h / height;
            matrix(0, 3) = 2 * (x + w * .5f) / width - 1;
            matrix(1, 3) = 1 - 2 * (y + h * .5f) / height;
            const auto panelValues = PanelValues(flight, matrix);
            if (!panelValues) {
                Error = true;
                return;
            }
            struct Mini {
                Impl* Self;
                Flight* Frame;
                RgTextureViewHandle Texture;
                PreparedShaderGroup Values;
                uint32_t Width, Height;
            };
            graph.AddRasterPass<Mini>(screen ? "Interface.Signal" : "Interface.LiveMap", [&](Mini& p, RenderGraphRasterBuilder& b) {
                p={this,&flight,b.ReadTexture(screen?signal:observer),*panelValues,family.RenderSize.Width,family.RenderSize.Height}; b.SetColorAttachment(0,color,{.Load=render::LoadAction::Load}); }, +[](const Mini& p, RenderGraphRasterContext& ctx) {
                ctx.Encoder().SetViewport(MakeViewport(p.Self->Device->GetBackend(),0,0,float(p.Width),float(p.Height)));
                ctx.Encoder().SetScissor({0,0,p.Width,p.Height}); p.Self->DrawPanel(*p.Frame,ctx,ctx.GetTextureView(p.Texture),p.Values,false); });
        }
    }

    RgTextureHandle AddScene(RenderGraph& graph, Flight& flight, const ResolvedRenderViewFamily& family, RgTextureHandle color, size_t first, RgTextureHandle signal = {}, RgTextureHandle observer = {}) {
        const render::TextureFormat formats[]{render::TextureFormat::D32_FLOAT, render::TextureFormat::D24_UNORM_S8_UINT, render::TextureFormat::D16_UNORM};
        const auto usage = render::TextureUse::DepthStencilWrite | render::TextureUse::DepthStencilRead;
        const auto format = SelectFirstSupportedFormat(*Device, formats, render::TextureDimension::Dim2D, usage, 1);
        if (!format) {
            Error = true;
            return {};
        }
        const auto depth = graph.CreateTexture({render::TextureDimension::Dim2D, family.RenderSize.Width, family.RenderSize.Height, 1, 1, 1, *format, render::MemoryType::Device, usage}, "Atrium depth");
        struct ScenePass {
            Impl* Self;
            Flight* Frame;
            size_t First, Count;
            int Kind;
        };
        const size_t count = family.Views.size();
        bool hasDepth = false, hasTransparent = false;
        for (size_t i = 0; i < count; ++i) {
            hasDepth |= !flight.Views[first + i].Depth.Commands.empty();
            hasTransparent |= !flight.Views[first + i].Transparent.Commands.empty();
        }
        struct SkyPass {
            Impl* Self;
            Flight* Frame;
            size_t First;
            vector<PreparedShaderGroup> Values;
        };
        vector<PreparedShaderGroup> skyValues;
        for (size_t i = 0; i < count; ++i) {
            const auto& view = flight.Views[first + i].View;
            Eigen::Matrix4f rotation = view.View;
            rotation.block<3, 1>(0, 3).setZero();
            const Eigen::Matrix4f clipToWorld = (view.Projection * rotation).inverse();
            ShaderParameterStorage values{&Sky->GetParameterLayout(), 0};
            if (!values.SetMatrix4x4("SkyFrame.ClipToWorld", clipToWorld)) {
                Error = true;
                return {};
            }
            const auto prepared = flight.Resources.PrepareGroup(*Sky.Get(), 0, values);
            if (!prepared) {
                Error = true;
                return {};
            }
            skyValues.push_back(*prepared);
        }
        graph.AddRasterPass<SkyPass>("Atrium.Sky", [&](SkyPass& p, RenderGraphRasterBuilder& b) {
            p={this,&flight,first,std::move(skyValues)}; b.SetColorAttachment(0,color); }, +[](const SkyPass& p, RenderGraphRasterContext& ctx) {
            const auto pso=p.Self->Sky->GetOrCreateGraphicsPipelineState(FlatState(),{},PrimitiveTopology::TriangleList,ctx.PassState());
            if (!pso) { p.Self->Error=true; return; }
            ctx.Encoder().BindGraphicsPipelineState(pso.Get());
            for (size_t i=0;i<p.Values.size();++i) {
                const auto& view=p.Frame->Views[p.First+i].View;
                const auto& values=p.Values[i];
                ctx.Encoder().SetViewport(MakeViewport(p.Self->Device->GetBackend(),float(view.ViewRect.X),float(view.ViewRect.Y),float(view.ViewRect.Width),float(view.ViewRect.Height)));
                ctx.Encoder().SetScissor(view.ScissorRect);
                ctx.Encoder().BindShaderParameterSet(0,values.Set.Get(),values.DynamicOffsets);
                ctx.Encoder().Draw(3,1,0,0);
            } });
        for (int kind = 0; kind < 3; ++kind) {
            if (kind == 2 && signal.IsValid() && observer.IsValid()) AddWorldScreens(graph, flight, family, color, depth, signal, observer, first);
            if ((kind == 0 && !hasDepth) || (kind == 2 && !hasTransparent)) continue;
            graph.AddRasterPass<ScenePass>(kind == 0 ? "Forward.DepthPrepass" : kind == 1 ? "Forward.Opaque"
                                                                                          : "Forward.Transparent",
                                           [&](ScenePass& p, RenderGraphRasterBuilder& b) {
                    p={this,&flight,first,count,kind};
                    if (kind) b.SetColorAttachment(0,color,{.Load=render::LoadAction::Load});
                    b.SetDepthAttachment(depth,{.Load=kind==0||(kind==1&&!hasDepth)?render::LoadAction::Clear:render::LoadAction::Load,.ReadOnly=kind==2}); }, +[](const ScenePass& p, RenderGraphRasterContext& ctx) {
                    for (size_t i=0;i<p.Count;++i) {
                        const auto& v=p.Frame->Views[p.First+i];
                        ctx.Encoder().SetViewport(MakeViewport(p.Self->Device->GetBackend(),float(v.View.ViewRect.X),float(v.View.ViewRect.Y),float(v.View.ViewRect.Width),float(v.View.ViewRect.Height)));
                        ctx.Encoder().SetScissor(v.View.ScissorRect);
                        SubmitRendererList(p.Kind==0?v.Depth:p.Kind==1?v.Opaque:v.Transparent,ctx,ctx.PassState(),p.Frame->Execution);
                    } });
        }
        return depth;
    }
};

AtriumPipeline::AtriumPipeline(Application* app, Scene* scene, CameraComponent* camera, StreamingAssetRef<TextureAsset> font, std::filesystem::path captureDirectory)
    : _impl(make_unique<Impl>(app, scene, camera, std::move(font), std::move(captureDirectory))) {}
AtriumPipeline::~AtriumPipeline() noexcept = default;
bool AtriumPipeline::IsValid() const noexcept { return !_impl->Error; }
bool AtriumPipeline::Failed() const noexcept { return _impl->Error; }

void AtriumPipeline::PrepareFrame(RenderPrepareContext& ctx) {
    auto& flight = *_impl->Flights[ctx.App.FlightIndex];
    flight.State = GameSettings;
    if (!flight.State.Ready) {
        ctx.Workloads.AddPresentationOutputs();
        return;
    }
    flight.Font = _impl->Font.Get();
    ctx.RetainedAssets.push_back(_impl->Font.AsAny());
    if (!BuildRenderSceneSnapshot(*_impl->Source, flight.Scene, ctx.RetainedAssets)) {
        _impl->Error = true;
        return;
    }
    const auto& stats = flight.Scene.Stats;
    if (stats.MissingGeometry || stats.EmptyDraw || stats.InvalidDrawRange || stats.MaterialUnavailable || stats.InvalidBounds) {
        RADRAY_ERR_LOG("Atrium scene snapshot contains invalid input");
        _impl->Error = true;
        return;
    }
    for (const auto& output : ctx.Outputs) {
        if (!output.Active || output.Kind != RenderOutputKind::Presentation) continue;
        RenderViewDesc view;
        view.Name = "Free flight";
        view.StateId = _impl->Ids[0];
        view.WorldPosition = _impl->Camera->GetEyePosition();
        view.WorldToView = _impl->Camera->ComputeViewMatrix();
        view.Projection = PerspectiveProjectionDesc{_impl->Camera->GetFovY(), .1f, 250};
        view.LayerMask = flight.State.Beacons ? 3u : 1u;
        view.CameraCut = flight.State.CameraCut;
        vector<RenderViewDesc> views{view};
        if (flight.State.Split) {
            views[0].ViewRect = views[0].ScissorRect = {0, 0, .5f, 1};
            view.Name = "Layer-isolated observation";
            view.StateId = _impl->Ids[1];
            view.ViewRect = view.ScissorRect = {.5f, 0, .5f, 1};
            view.LayerMask = 1;
            views.push_back(view);
        }
        ctx.Workloads.AddViewFamily({"Tidal Atrium", output.Id, flight.State.RenderScale, std::move(views)});
    }
    RenderViewDesc overhead;
    overhead.Name = "Orthographic live camera";
    overhead.StateId = _impl->Ids[2];
    overhead.WorldPosition = {0, 48, 6};
    overhead.WorldToView = LookAtLH(overhead.WorldPosition, Eigen::Vector3f{0, 0, 6}, Eigen::Vector3f{0, 0, 1});
    overhead.Projection = OrthographicProjectionDesc{62, .1f, 100};
    overhead.LayerMask = 3;
    ctx.Workloads.AddViewFamily({"Live observer output", _impl->CameraOutput, 1, {overhead}});
}

void AtriumPipeline::Render(RenderPipelineContext& ctx) {
    if (_impl->Error) return;
    auto& self = *_impl;
    auto& flight = *self.Flights[ctx.FlightIndex()];
    if (!flight.State.Ready) return;
    flight.Views.clear();
    flight.ExtraSets.clear();
    flight.Execution = {};
    if (!flight.Resources.BeginFrame(ctx.HostWrites())) {
        self.Error = true;
        return;
    }
    size_t viewCount = 0;
    for (const auto& family : ctx.ViewFamilies()) viewCount += family.Views.size();
    flight.Views.resize(viewCount);
    vector<size_t> firstViews;
    size_t cursor = 0;
    for (const auto& family : ctx.ViewFamilies()) {
        firstViews.push_back(cursor);
        for (const auto& view : family.Views) {
            auto& work = flight.Views[cursor++];
            work.View = view;
            if (!Cull({&flight.Scene, &work.View}, work.Culling)) {
                self.Error = true;
                return;
            }
            AtriumMeshProcessor depth{flight.Resources, flight.State}, opaque{flight.Resources, flight.State}, transparent{flight.Resources, flight.State};
            if (flight.State.Depth) BuildRendererList({"Depth", "DepthOnly", &work.Culling, &work.View, RenderQueueRange::Opaque()}, depth, work.Depth);
            BuildRendererList({"Opaque", "ForwardLit", &work.Culling, &work.View, RenderQueueRange::Opaque()}, opaque, work.Opaque);
            BuildRendererList({"Transparent", "ForwardLit", &work.Culling, &work.View, RenderQueueRange::Transparent(), ~0u, RendererListSorting::BackToFront}, transparent, work.Transparent);
            for (const auto* list : {&work.Depth, &work.Opaque, &work.Transparent}) {
                if (list->Stats.InvalidBindings || list->Stats.InvalidGeometry || list->Stats.PrepareResourceFailed || list->Stats.ProcessorRejected) {
                    RADRAY_ERR_LOG("Atrium draw list rejected a batch: bindings={}, geometry={}, resources={}, processor={}", list->Stats.InvalidBindings, list->Stats.InvalidGeometry, list->Stats.PrepareResourceFailed, list->Stats.ProcessorRejected);
                    self.Error = true;
                    return;
                }
            }
        }
    }
    auto graph = ctx.CreateRenderGraph("Tidal Atrium / live framework gallery");
    RgTextureHandle observer;
    for (const auto& family : ctx.ViewFamilies())
        if (family.OutputId == self.CameraOutput && family.OutputAvailable) {
            observer = ctx.ImportOutput(graph, family.OutputId);
            self.AddScene(graph, flight, family, observer, firstViews[family.FrameLocalIndex]);
        }
    for (const auto& family : ctx.ViewFamilies()) {
        if (!family.OutputAvailable || family.OutputId == self.CameraOutput) continue;
        const auto signal = self.AddSignal(ctx, graph, flight, family);
        if (!signal.IsValid() || !observer.IsValid()) {
            self.Error = true;
            return;
        }
        const auto sceneColor = graph.CreateTexture({render::TextureDimension::Dim2D, family.RenderSize.Width, family.RenderSize.Height, 1, 1, 1,
                                                     render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource},
                                                    "Atrium scene color");
        self.AddScene(graph, flight, family, sceneColor, firstViews[family.FrameLocalIndex], signal, observer);
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, family.OutputSize.Width, family.OutputSize.Height, 1, 1, 1,
                                                render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource},
                                               "Atrium display color");
        const auto output = ctx.ImportOutput(graph, family.OutputId);
        Eigen::Matrix4f fullscreen = Eigen::Matrix4f::Identity();
        fullscreen(0, 0) = fullscreen(1, 1) = 2;
        const auto values = self.PanelValues(flight, fullscreen);
        if (!values) {
            self.Error = true;
            return;
        }
        struct Present {
            Impl* Self;
            Impl::Flight* Frame;
            RgTextureViewHandle Color;
            PreparedShaderGroup Values;
            uint32_t Width, Height;
        };
        graph.AddRasterPass<Present>("Atrium.Downsample", [&](Present& p, RenderGraphRasterBuilder& b) {
            p={&self,&flight,b.ReadTexture(sceneColor),*values,family.OutputSize.Width,family.OutputSize.Height}; b.SetColorAttachment(0,color); }, +[](const Present& p, RenderGraphRasterContext& context) {
            context.Encoder().SetViewport(MakeViewport(p.Self->Device->GetBackend(),0,0,float(p.Width),float(p.Height)));
            context.Encoder().SetScissor({0,0,p.Width,p.Height}); p.Self->DrawPanel(*p.Frame,context,context.GetTextureView(p.Color),p.Values,false); });
        auto displayFamily = family;
        displayFamily.RenderSize = family.OutputSize;
        if (flight.State.ShowUi) self.AddHud(graph, flight, displayFamily, color, signal, observer, firstViews[family.FrameLocalIndex]);
        graph.AddRasterPass<Present>("Atrium.Present", [&](Present& p, RenderGraphRasterBuilder& b) {
            p={&self,&flight,b.ReadTexture(color),*values,family.OutputSize.Width,family.OutputSize.Height}; b.SetColorAttachment(0,output); }, +[](const Present& p, RenderGraphRasterContext& context) {
            context.Encoder().SetViewport(MakeViewport(p.Self->Device->GetBackend(),0,0,float(p.Width),float(p.Height)));
            context.Encoder().SetScissor({0,0,p.Width,p.Height}); p.Self->DrawPanel(*p.Frame,context,context.GetTextureView(p.Color),p.Values,false); });
        if (!flight.State.CaptureName.empty() && !self.CaptureDirectory.empty()) {
            flight.CaptureWidth = family.OutputSize.Width;
            flight.CaptureHeight = family.OutputSize.Height;
            flight.CapturePitch = Align(uint64_t{flight.CaptureWidth} * 4, self.Device->GetDetail().TextureDataPitchAlignment);
            const auto size = flight.CapturePitch * flight.CaptureHeight;
            if (!flight.Readback || flight.Readback->GetDesc().Size < size) {
                auto buffer = self.Device->CreateBuffer({size, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
                if (!buffer) {
                    self.Error = true;
                    return;
                }
                flight.Readback = buffer.Release();
                flight.CaptureImport = RenderExternalBuffer{flight.Readback.get(), flight.Readback->GetDesc(), render::BufferState::CopyDestination};
            }
            const auto target = graph.ImportBuffer(*flight.CaptureImport, "Screenshot", RenderGraphExternalAccess::ObservableOutput);
            graph.AddCopyTextureToBufferPass("Atrium.Capture", color, target);
            graph.AddComputePass<uint32_t>("Atrium.CaptureHostVisibility", [&](uint32_t&, RenderGraphComputeBuilder& b) { b.ReadBuffer(target,RgBufferAccess::HostRead); b.SetSideEffect(); }, +[](const uint32_t&, RenderGraphComputeContext&) {});
            flight.PendingCapture = flight.State.CaptureName;
        }
    }
    const auto result = ctx.ExecuteGraph(graph);
    self.LastReport = graph.GetReport();
    if (!flight.PendingCapture.empty()) flight.Report = self.LastReport.ToJson();
    if (!result.Success) RADRAY_ERR_LOG("Atrium graph failed: {}", self.LastReport.ToText());
    if (!result.Success || flight.Execution.Skipped) self.Error = true;
    if (result.Success)
        for (const auto& view : flight.Views) ctx.CommitView(view.View.StateId);
}

void AtriumPipeline::Complete(uint32_t index) {
    auto& flight = *_impl->Flights[index];
    if (flight.PendingCapture.empty() || !flight.Readback) return;
    const auto size = flight.CapturePitch * flight.CaptureHeight;
    const auto* bytes = static_cast<const byte*>(flight.Readback->Map(0, size));
    if (!bytes) {
        _impl->Error = true;
        return;
    }
    flight.Readback->InvalidateMappedRange({0, size});
    ImageData image;
    image.Width = flight.CaptureWidth;
    image.Height = flight.CaptureHeight;
    image.Format = ImageFormat::RGBA8_BYTE;
    image.Data = make_unique<byte[]>(uint64_t{image.Width} * image.Height * 4);
    for (uint32_t y = 0; y < image.Height; ++y) std::memcpy(image.Data.get() + uint64_t{y} * image.Width * 4, bytes + y * flight.CapturePitch, uint64_t{image.Width} * 4);
    flight.Readback->Unmap();
    const auto path = (_impl->CaptureDirectory / (flight.PendingCapture + ".png")).string();
    if (!image.WritePNG({path, false})) _impl->Error = true;
    std::ofstream report{_impl->CaptureDirectory / (flight.PendingCapture + ".json")};
    report << flight.Report;
    if (!report) _impl->Error = true;
    std::ofstream metrics{_impl->CaptureDirectory / (flight.PendingCapture + ".metrics.json")};
    const auto& state = flight.State;
    metrics << fmt::format("{{\"frame\":{},\"time\":{},\"depth\":{},\"wireframe\":{},\"split\":{},\"beacons\":{},\"history\":{},\"historyValid\":{},\"renderScale\":{},\"primitives\":{},\"batches\":{},\"draws\":{},\"skipped\":{},\"views\":[",
                           state.Frame, state.Time, state.Depth, state.Wireframe, state.Split, state.Beacons, state.History, flight.HistoryValid, state.RenderScale, flight.Scene.Primitives.size(), flight.Scene.MeshBatches.size(), flight.Execution.Draws, flight.Execution.Skipped);
    for (size_t i = 0; i < flight.Views.size(); ++i) {
        const auto& view = flight.Views[i];
        metrics << fmt::format("{}{{\"visible\":{},\"layerRejected\":{},\"frustumRejected\":{},\"depth\":{},\"opaque\":{},\"transparent\":{},\"missingDepthPass\":{}}}",
                               i ? "," : "", view.Culling.Stats.VisiblePrimitives, view.Culling.Stats.LayerRejected, view.Culling.Stats.FrustumRejected, view.Depth.Commands.size(), view.Opaque.Commands.size(), view.Transparent.Commands.size(), view.Depth.Stats.MissingPass);
    }
    metrics << "]}";
    if (!metrics) _impl->Error = true;
    RADRAY_INFO_LOG("Atrium captured {} ({} primitives, {} draws)", path, flight.Scene.Primitives.size(), flight.Execution.Draws);
    flight.PendingCapture.clear();
}

}  // namespace radray::atrium
