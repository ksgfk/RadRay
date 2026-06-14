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
#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/triangle_mesh.h>
#include <radray/logger.h>

#include <filesystem>
#include <numbers>
#include <string_view>

#ifndef RADRAY_EXAMPLE_ASSET_DIR
#define RADRAY_EXAMPLE_ASSET_DIR "."
#endif

using namespace radray;

// 这个 example 现在是一个"纯游戏应用":只重写窄接口
//   OnInit / OnUpdate / OnRenderView / OnShutdown,
// 不再手写设备/窗口/GpuSystem 初始化、帧序、多 viewport acquire/present、
// backbuffer barrier 状态追踪、GPU 计时这些通用流程——全部已收束到 runtime。
// ImGui 是显式注册的应用子系统,应用按需 GetSubsystem<ImGuiSystem>() 提交 UI 绘制。
//
// 仍保留在 game 层的,只有 demo 自己的内容:球体 mesh/material/camera 的创建、
// 旋转逻辑、monitor UI,以及"主 viewport 背后画什么"(场景渲染)所需的 per-flight
// depth buffer(尺寸/格式由 demo 决定,故归 demo 自管)。
class ExampleApp : public Application {
    // 每条 flight 一个 depth buffer。runtime 在复用该 flight 槽位前会等其 fence,
    // 故 resize 时重建上一份 depth 是安全的。
    struct DepthResource {
        unique_ptr<render::Texture> Tex;
        unique_ptr<render::TextureView> View;
        render::TextureStates State{render::TextureState::Undefined};
        uint32_t Width{0};
        uint32_t Height{0};
    };

public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    // ── 一次性初始化:运行时(device/window/gpu/imgui/asset/world)已就绪 ──
    void OnInit() override {
        _depths.resize(GetGpuSystem()->GetFlightDataCount());
        InitScene();
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

        _sphereReady = true;
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

    // ── viewport UI 背后的场景内容:把球体画进 backbuffer(ImGui 随后 Load 叠加)──
    // demo 只绘制主 viewport；secondary ImGui platform viewport 保持 UI-only。
    bool OnRenderView(AppFrameContext& ctx, const AppFrameTarget& target) override {
        if (!_sphereReady || !target.Window->IsMainWindow()) {
            return false;
        }
        render::TextureDescriptor bbDesc = target.BackBuffer->GetDesc();
        return RecordSphere(
            ctx.GetCommandBuffer(),
            _depths[ctx.FlightIndex()],
            target.BackBufferView,
            bbDesc.Width,
            bbDesc.Height);
    }

    // ── 关闭前清理(GPU 已 idle):释放 demo 自管的 per-flight depth + 置空 World 指针 ──
    void OnShutdown() override {
        _depths.clear();
        _sphereMeshComp = nullptr;
        _cameraComp = nullptr;
    }

private:
    // 确保该 flight 的 depth buffer 匹配给定尺寸;不匹配则重建。
    render::TextureView* EnsureDepthBuffer(DepthResource& depth, uint32_t width, uint32_t height) {
        if (depth.Tex != nullptr && depth.Width == width && depth.Height == height) {
            return depth.View.get();
        }
        depth.View.reset();
        depth.Tex.reset();
        depth.State = render::TextureState::Undefined;

        render::TextureDescriptor texDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Width = width,
            .Height = height,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleCount = 1,
            .Format = DepthFormat,
            .Memory = render::MemoryType::Device,
            .Usage = render::TextureUse::DepthStencilWrite,
            .Hints = render::ResourceHint::None};
        auto texOpt = GetDevice()->CreateTexture(texDesc);
        if (!texOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create sphere depth texture");
            return nullptr;
        }
        depth.Tex = texOpt.Release();
        depth.Tex->SetDebugName("sphere_depth");

        render::TextureViewDescriptor viewDesc{
            .Target = depth.Tex.get(),
            .Dim = render::TextureDimension::Dim2D,
            .Format = DepthFormat,
            .Range = render::SubresourceRange::AllSub(),
            .Usage = render::TextureViewUsage::DepthWrite};
        auto viewOpt = GetDevice()->CreateTextureView(viewDesc);
        if (!viewOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create sphere depth view");
            depth.Tex.reset();
            return nullptr;
        }
        depth.View = viewOpt.Release();
        depth.Width = width;
        depth.Height = height;
        return depth.View.get();
    }

    // 在自己的 render pass(color Clear + depth)里画球体。返回 true 表示已向 colorView 写入。
    bool RecordSphere(
        render::CommandBuffer* cmdBuffer,
        DepthResource& depth,
        render::TextureView* colorView,
        uint32_t width,
        uint32_t height) {
        if (!_sphereReady || width == 0 || height == 0) {
            return false;
        }
        render::TextureView* depthView = EnsureDepthBuffer(depth, width, height);
        if (depthView == nullptr) {
            return false;
        }

        render::ResourceBarrierDescriptor toDepthWrite = render::BarrierTextureDescriptor{
            .Target = depth.Tex.get(),
            .Before = depth.State,
            .After = render::TextureState::DepthWrite};
        cmdBuffer->ResourceBarrier(std::span{&toDepthWrite, 1});
        depth.State = render::TextureState::DepthWrite;

        SceneView sceneView{};
        if (_cameraComp != nullptr) {
            _cameraComp->FillSceneView(sceneView, width, height);
        }

        render::ColorAttachment colorAttachment{
            .Target = colorView,
            .Load = render::LoadAction::Clear,
            .Store = render::StoreAction::Store,
            .ClearValue = render::ColorClearValue{{0.08f, 0.10f, 0.14f, 1.0f}}};
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
            .Name = "Sphere Pass"};
        auto encoderOpt = cmdBuffer->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin sphere render pass");
            return false;
        }
        auto encoder = encoderOpt.Release();

        Viewport vp{
            .X = 0.0f,
            .Y = 0.0f,
            .Width = static_cast<float>(width),
            .Height = static_cast<float>(height),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f};
        if (GetDevice()->GetBackend() == render::RenderBackend::Vulkan) {
            vp.Y = static_cast<float>(height);
            vp.Height = -static_cast<float>(height);
        }
        encoder->SetViewport(vp);
        encoder->SetScissor(Rect{0, 0, width, height});

        MeshPassProcessor::Config processorConfig{};
        processorConfig.Cache = &GetGpuSystem()->GetPSOCache();
        processorConfig.RtFormats.ColorFormats = {BackBufferFormat};
        processorConfig.RtFormats.DepthFormat = DepthFormat;
        processorConfig.ObjectConstantsParam = "gScene";

        _sceneRenderer.Render(encoder.get(), *GetWorld()->GetScene(), sceneView, processorConfig);

        cmdBuffer->EndRenderPass(std::move(encoder));
        return true;
    }

    // —— demo 内容状态 ——
    SceneRenderer _sceneRenderer;
    vector<DepthResource> _depths;
    bool _sphereReady{false};
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
