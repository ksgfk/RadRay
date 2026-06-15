#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/renderer/scene.h>
#include <radray/runtime/renderer/scene_renderer.h>
#include <radray/runtime/renderer/render_context.h>
#include <radray/runtime/renderer/render_pass.h>
#include <radray/runtime/renderer/render_pipeline.h>
#include <radray/runtime/renderer/render_resource_pool.h>
#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/triangle_mesh.h>
#include <radray/logger.h>

#include <filesystem>
#include <numbers>
#include <span>
#include <string_view>

#ifndef RADRAY_EXAMPLE_ASSET_DIR
#define RADRAY_EXAMPLE_ASSET_DIR "."
#endif

using namespace radray;

// —— 一个 game 侧自定义的 RenderPass:画场景的不透明几何 ——
// runtime 只提供 RenderPipeline/RenderPass/RenderContext/RenderResourcePool 这套"怎么组织
// pass、怎么共享资源"的词汇表,不带任何具体 pass。具体"画什么"在 game 层:
// 这里 ScenePass 从 ctx.Resources 共享池申请名为 "SceneDepth" 的 depth(下游 pass 可按名接力),
// 从 ctx.Visible 共享可见集挑子集,自己 BeginRenderPass→录制→End,内部驱动 SceneRenderer。
class ScenePass : public RenderPass {
public:
    ScenePass(
        render::TextureFormat colorFormat,
        render::TextureFormat depthFormat,
        render::ColorClearValue clearColor)
        : _colorFormat(colorFormat),
          _depthFormat(depthFormat),
          _clearColor(clearColor) {}

    std::string_view GetName() const noexcept override { return "ScenePass"; }

    // 在自己的 render pass(color Clear + depth Clear)里画场景不透明几何。
    void Execute(RenderContext& ctx) override {
        if (ctx.Width == 0 || ctx.Height == 0 || ctx.Scene == nullptr ||
            ctx.CmdBuffer == nullptr || ctx.ColorTarget == nullptr ||
            ctx.Resources == nullptr || ctx.Device == nullptr) {
            return;
        }

        // 从共享池申请/复用本 flight 的 depth(尺寸变了池子自动重建)。名字 "SceneDepth"
        // 是 producer/consumer 跨 pass 交接的句柄。
        render::TextureDescriptor depthDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Width = ctx.Width,
            .Height = ctx.Height,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleCount = 1,
            .Format = _depthFormat,
            .Memory = render::MemoryType::Device,
            .Usage = render::TextureUse::DepthStencilWrite,
            .Hints = render::ResourceHint::None};
        if (ctx.Resources->Acquire("SceneDepth", ctx.FlightIndex, depthDesc, *ctx.Device) == nullptr) {
            return;
        }
        // 迁到 DepthWrite:barrier 由池子按跟踪态发出。
        ctx.Resources->Transition("SceneDepth", ctx.FlightIndex, render::TextureState::DepthWrite, *ctx.CmdBuffer);

        render::TextureViewDescriptor depthViewDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Format = _depthFormat,
            .Range = render::SubresourceRange::AllSub(),
            .Usage = render::TextureViewUsage::DepthWrite};
        render::TextureView* depthView = ctx.Resources->GetView("SceneDepth", ctx.FlightIndex, depthViewDesc, *ctx.Device);
        if (depthView == nullptr) {
            return;
        }

        render::ColorAttachment colorAttachment{
            .Target = ctx.ColorTarget,
            .Load = render::LoadAction::Clear,
            .Store = render::StoreAction::Store,
            .ClearValue = _clearColor};
        render::DepthStencilAttachment depthAttachment{
            .Target = depthView,
            .DepthLoad = render::LoadAction::Clear,
            .DepthStore = render::StoreAction::Store,
            .StencilLoad = render::LoadAction::DontCare,
            .StencilStore = render::StoreAction::Discard,
            .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
        render::RenderPassDescriptor passDesc{
            .ColorAttachments = std::span{&colorAttachment, 1},
            .DepthStencilAttachment = depthAttachment,
            .Name = "Scene Pass"};
        auto encoderOpt = ctx.CmdBuffer->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin scene render pass");
            return;
        }
        auto encoder = encoderOpt.Release();

        Viewport vp{
            .X = 0.0f,
            .Y = 0.0f,
            .Width = static_cast<float>(ctx.Width),
            .Height = static_cast<float>(ctx.Height),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f};
        if (ctx.Device->GetBackend() == render::RenderBackend::Vulkan) {
            vp.Y = static_cast<float>(ctx.Height);
            vp.Height = -static_cast<float>(ctx.Height);
        }
        encoder->SetViewport(vp);
        encoder->SetScissor(Rect{0, 0, ctx.Width, ctx.Height});

        MeshPassProcessor::Config processorConfig{};
        processorConfig.Cache = &ctx.Gpu->GetPSOCache();
        processorConfig.RtFormats.ColorFormats = {_colorFormat};
        processorConfig.RtFormats.DepthFormat = _depthFormat;
        processorConfig.ObjectConstantsParam = "gScene";

        // 从共享可见集挑子集。本 pass 画全部(filter 为空);将来 Opaque/Transparent 各传自己的 filter。
        if (ctx.Visible != nullptr) {
            _sceneRenderer.DrawRenderers(encoder.get(), *ctx.Visible, ctx.View, processorConfig);
        }

        ctx.CmdBuffer->EndRenderPass(std::move(encoder));
    }

private:
    SceneRenderer _sceneRenderer;
    render::TextureFormat _colorFormat;
    render::TextureFormat _depthFormat;
    render::ColorClearValue _clearColor;
};

// 这个 example 是一个"纯游戏应用":只重写窄接口 OnInit / OnUpdate / OnRenderView / OnShutdown,
// 不再手写设备/窗口/GpuSystem 初始化、帧序、多 viewport acquire/present、backbuffer barrier
// 状态追踪、GPU 计时这些通用流程——全部已收束到 runtime。
//
// 渲染组织也走 runtime 的 RenderPipeline/RenderPass 词汇表:game 自己持有一个 RenderPipeline,
// 装填自定义的 ScenePass,在 OnRenderView 里组装 RenderContext 并驱动管线。runtime 不带任何
// 默认 pass、不强加渲染策略。
//
// ImGui 是显式注册的应用子系统,应用按需 GetSubsystem<ImGuiSystem>() 提交 UI 绘制。
class ExampleApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    // ── 一次性初始化:运行时(device/window/gpu/imgui/asset/world)已就绪 ──
    void OnInit() override {
        InitScene();

        // 装填渲染管线:一个 ScenePass。runtime 不做任何默认装填,全由 game 决定。
        auto scenePass = make_unique<ScenePass>(
            BackBufferFormat,
            DepthFormat,
            render::ColorClearValue{{0.08f, 0.10f, 0.14f, 1.0f}});
        _pipeline.AddPass(std::move(scenePass));
    }

    // 构建球体 StaticMesh + Material 资产、Spawn Actor + StaticMeshComponent,
    // Spawn 相机 Actor + CameraComponent。资产生命周期由 AssetManager + 组件
    // StreamingAssetRef 持有,App 不持引用。
    void InitScene() {
        // ── Material 资产(ctor 编译 shader + 取共享 root signature)──
        std::filesystem::path shaderPath = std::filesystem::path{RADRAY_EXAMPLE_ASSET_DIR} / "sphere.hlsl";
        MaterialDescriptor matDesc{};
        matDesc.ShaderPath = shaderPath;
        matDesc.ShaderName = "sphere";
        matDesc.VsEntry = "VSMain";
        matDesc.PsEntry = "PSMain";
        matDesc.Primitive = render::PrimitiveState::Default();
        matDesc.Primitive.Cull = render::CullMode::None;
        matDesc.DepthStencil = render::DepthStencilState::Default();
        matDesc.DepthStencil.Format = DepthFormat;
        const AssetId matId = Guid::Parse("7f1d3b2a-0000-4000-8000-00000000b001");
        StreamingAssetRef<Material> material = GetAssetManager()->Load<Material>(AssetLoadRequest{
            .Id = matId,
            .Task = LoadMaterial(*GetGpuSystem(), matDesc),
            .DebugName = "default material"});
        if (!material.IsValid()) {
            RADRAY_ERR_LOG("failed to start loading sphere Material asset");
            return;
        }

        // ── CPU 网格数据 + 异步 StaticMesh 加载(走 AssetManager)──
        TriangleMesh tri{};
        tri.InitAsUVSphere(1.0f, 64);
        MeshResource meshResource{};
        tri.ToSimpleMeshResource(&meshResource);
        meshResource.Name = "sphere";
        if (meshResource.Primitives.empty()) {
            RADRAY_ERR_LOG("failed to build sphere mesh resource");
            return;
        }
        const AssetId meshId = Guid::Parse("7f1d3b2a-0000-4000-8000-00000000a001");
        StreamingAssetRef<StaticMesh> mesh = GetAssetManager()->Load<StaticMesh>(AssetLoadRequest{
            .Id = meshId,
            .Task = LoadStaticMesh(GetGpuSystem()->GetFrameUploadScheduler(), std::move(meshResource)),
            .DebugName = "sphere mesh"});
        if (!mesh.IsValid()) {
            RADRAY_ERR_LOG("failed to start loading sphere StaticMesh asset");
            return;
        }

        // ── Spawn Actor + StaticMeshComponent ──
        Actor* actor = GetWorld()->SpawnActor<Actor>();
        _sphereMeshComp = actor->AddComponent<StaticMeshComponent>();
        _sphereMeshComp->SetStaticMesh(mesh);
        _sphereMeshComp->SetMaterial(material);
        actor->SetRootComponent(_sphereMeshComp);

        // ── Spawn 相机 Actor + CameraComponent ──
        Actor* cameraActor = GetWorld()->SpawnActor<Actor>();
        _cameraComp = cameraActor->AddComponent<CameraComponent>();
        cameraActor->SetRootComponent(_cameraComp);
        _cameraComp->SetWorldLocation(Eigen::Vector3f{0.0f, 0.0f, -3.0f});
        _cameraComp->SetPerspective(Radian(60.0f), 0.1f, 100.0f);
    }

    // ── 每帧游戏逻辑(World::Tick 之前)── 推进球体自旋,经组件 transform 流到代理 ──
    void OnUpdate(const AppUpdateContext& ctx) override {
        _sphereSpin += ctx.DeltaTime.count();
        if (_sphereSpin > 2.0f * std::numbers::pi_v<float>) {
            _sphereSpin -= 2.0f * std::numbers::pi_v<float>;
        }
        if (_sphereMeshComp != nullptr) {
            _sphereMeshComp->SetRelativeRotation(
                Eigen::Quaternionf{Eigen::AngleAxisf(_sphereSpin, Eigen::Vector3f::UnitY())});
        }

        if (ImGuiSystem* imgui = GetSubsystem<ImGuiSystem>()) {
            if (imgui->Begin(ctx)) {
                DrawMonitorUi(ctx);
                imgui->End();
            }
        }
    }

    // ── 每帧 UI(ImGuiSystem::Begin 与 End 之间)──
    void DrawMonitorUi(const AppUpdateContext& ctx) {
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoMove;
        constexpr float PAD = 10.0f;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 workPos = viewport->WorkPos;
        ImVec2 windowPos{workPos.x + PAD, workPos.y + PAD};
        ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("RadrayMonitor", &_showMonitor, windowFlags)) {
            ImGui::Text("Delta time: %06.3f ms", ctx.DeltaTime.count() * 1000.0f);
            ImGui::Text("Frame latency: %06.3f ms", ctx.LastFrameLatency.count() * 1000.0f);
            ImGui::Text("GPU time: %06.3f ms", GetGpuSystem()->GetLastGpuTimeMs());
            static constexpr render::PresentMode kModes[] = {
                render::PresentMode::FIFO,
                render::PresentMode::Mailbox,
                render::PresentMode::Immediate};
            render::PresentMode currentMode = render::PresentMode::FIFO;
            if (GetWindowManager() != nullptr) {
                currentMode = GetWindowManager()->GetMainPresentMode();
            }
            string preview{render::format_as(currentMode)};
            if (ImGui::BeginCombo("Present Mode", preview.c_str())) {
                for (render::PresentMode mode : kModes) {
                    string item{render::format_as(mode)};
                    const bool selected = mode == currentMode;
                    if (ImGui::Selectable(item.c_str(), selected) && mode != currentMode) {
                        GetWindowManager()->SetPresentMode(mode);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::End();

        ImGui::ShowDemoWindow();
    }

    // ── viewport UI 背后的场景内容:组装 RenderContext,驱动渲染管线把场景画进 backbuffer ──
    // demo 只绘制主 viewport；secondary ImGui platform viewport 保持 UI-only(返回 false 走框架 clear)。
    bool OnRenderView(AppFrameContext& ctx, const AppFrameTarget& target) override {
        if (!target.Window->IsMainWindow()) {
            return false;
        }
        render::TextureDescriptor bbDesc = target.BackBuffer->GetDesc();

        RenderContext rc{};
        rc.FlightIndex = ctx.FlightIndex();
        rc.CmdBuffer = ctx.GetCommandBuffer();
        rc.Device = GetDevice();
        rc.Gpu = GetGpuSystem();
        rc.Scene = GetWorld()->GetScene();
        rc.ColorTarget = target.BackBufferView;
        rc.ColorFormat = bbDesc.Format;
        rc.Width = bbDesc.Width;
        rc.Height = bbDesc.Height;
        if (_cameraComp != nullptr) {
            _cameraComp->FillSceneView(rc.View, bbDesc.Width, bbDesc.Height);
        }

        // cull 一次,产出共享可见集,挂进 context 供各 pass 共享(对应 Unity context.Cull)。
        if (rc.Scene != nullptr) {
            SceneRenderer::Cull(*rc.Scene, rc.View, _visible);
            rc.Visible = &_visible;
        }
        // 跨 pass 共享的瞬态资源池(depth 等在这里按名交接)。
        rc.Resources = &_resourcePool;

        _pipeline.Render(rc);
        return true;
    }

    // —— 关闭前清理(GPU 已 idle):释放共享资源池 + 置空 World 指针 ——
    void OnShutdown() override {
        _resourcePool.Clear();
        _sphereMeshComp = nullptr;
        _cameraComp = nullptr;
    }

private:
    // —— 渲染组织(game 侧持有)——
    RenderPipeline _pipeline;
    RenderResourcePool _resourcePool;       // 跨 pass 共享的瞬态资源(如 SceneDepth)。
    VisiblePrimitiveList _visible;          // 本帧共享可见集(cull 一次,各 pass 读)。

    // —— demo 内容状态 ——
    float _sphereSpin{0.0f};
    bool _showMonitor{true};

    // 指向 World 的非 owning 指针(资产生命周期归 AssetManager + 组件 StreamingAssetRef)。
    StaticMeshComponent* _sphereMeshComp{nullptr};
    CameraComponent* _cameraComp{nullptr};
};

int main(int argc, char* argv[]) {
    ApplicationRuntimeDescriptor desc{};
    desc.Backend = render::RenderBackend::Vulkan;
    desc.AppName = "Example ImGui App";
    desc.EngineName = "RadRay";
    desc.WindowTitle = "Example ImGui App";
    desc.WindowWidth = 1280;
    desc.WindowHeight = 720;
    desc.BackBufferFormat = ExampleApp::BackBufferFormat;
    desc.PresentMode = render::PresentMode::FIFO;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--backend" && i + 1 < argc) {
            std::string_view backendStr{argv[i + 1]};
            if (backendStr == "vulkan") {
                desc.Backend = render::RenderBackend::Vulkan;
            } else if (backendStr == "d3d12") {
                desc.Backend = render::RenderBackend::D3D12;
            }
        }
        if (arg == "--valid-layer") {
            desc.EnableValidation = true;
        }
        if (arg == "--multithread") {
            desc.Multithreaded = true;
        }
    }

    ExampleApp app{};
    app.RegisterSubsystem<ImGuiSystem>();
    return app.Run(desc);
}
