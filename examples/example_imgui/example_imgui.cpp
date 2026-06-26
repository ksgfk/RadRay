#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/render/scene.h>
#include <radray/runtime/render/scene_view.h>
#include <radray/runtime/render/shader.h>
#include <radray/runtime/render/shader_variant_cache.h>
#include <radray/runtime/render/render_context.h>
#include <radray/runtime/render/render_pipeline.h>
#include <radray/runtime/render/draw_objects_pass.h>
#include <radray/runtime/render/render_resource_pool.h>
#include <radray/runtime/render/cull.h>
#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/triangle_mesh.h>
#include <radray/logger.h>

#include <cstring>
#include <filesystem>
#include <numbers>
#include <span>
#include <string_view>

#ifndef RADRAY_EXAMPLE_ASSET_DIR
#define RADRAY_EXAMPLE_ASSET_DIR "."
#endif

using namespace radray;

namespace {

/// per-object 常量,匹配 sphere.hlsl 的 SceneConstants(b0, space0, push constant)。
/// float4x4 MVP; float4x4 Model —— 列主序,与 Eigen 存储一致。
struct SphereSceneConstants {
    float MVP[16];
    float Model[16];
};

void WriteMatrix(float (&dst)[16], const Eigen::Matrix4f& m) noexcept {
    // Eigen 默认列主序,HLSL ConstantBuffer 也按列主序读 → 直接 memcpy。
    std::memcpy(dst, m.data(), sizeof(float) * 16);
}

}  // namespace

// 这个 example 是一个"纯游戏应用":只重写窄接口 OnInit / OnUpdate / OnRenderView / OnShutdown。
//
// 渲染走新的 srp 框架:game 持有 srp::Shader(描述 LightMode pass)、ShaderVariantCache、
// RenderContext、RenderPipeline。OnRenderView 里 Cull → 配 DrawObjectsPass → BeginRenderPass +
// SetEncoder → pipeline.RenderSingleCamera → EndRenderPass。runtime 不带任何默认 pass。
class ExampleApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    void OnInit() override {
        // ── srp::Shader:一个 UniversalForward pass(VSMain + PSMain)──
        std::filesystem::path shaderPath = std::filesystem::path{RADRAY_EXAMPLE_ASSET_DIR} / "sphere.hlsl";
        _sphereShader = make_unique<srp::Shader>(srp::ShaderId{0xB001}, "sphere");
        srp::ShaderPassSource pass{};
        pass.ShaderPath = shaderPath.string();
        pass.ShaderName = "sphere";
        pass.VsEntry = "VSMain";
        pass.PsEntry = "PSMain";
        pass.Tags.Tags.emplace_back("LightMode", "UniversalForward");
        _sphereShader->AddPass(std::move(pass));

        _variantCache = make_unique<srp::ShaderVariantCache>(GetGpuSystem());

        InitScene();
    }

    void InitScene() {
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

        // ── Spawn Actor + StaticMeshComponent(注入 srp::Shader + device + blend)──
        Actor* actor = GetWorld()->SpawnActor<Actor>();
        _sphereMeshComp = actor->AddComponent<StaticMeshComponent>();
        _sphereMeshComp->SetStaticMesh(mesh);
        _sphereMeshComp->SetRenderShader(_sphereShader.get());
        _sphereMeshComp->SetRenderDevice(GetDevice());
        _sphereMeshComp->SetBlendMode(srp::BlendMode::Opaque);
        _sphereMeshComp->SetTwoSided(true);  // sphere.hlsl 原本 Cull=None
        actor->SetRootComponent(_sphereMeshComp);

        // ── Spawn 相机 Actor + CameraComponent ──
        Actor* cameraActor = GetWorld()->SpawnActor<Actor>();
        _cameraComp = cameraActor->AddComponent<CameraComponent>();
        cameraActor->SetRootComponent(_cameraComp);
        _cameraComp->SetWorldLocation(Eigen::Vector3f{0.0f, 0.0f, -3.0f});
        _cameraComp->SetPerspective(Radian(60.0f), 0.1f, 100.0f);
    }

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

    // ── 组装 srp 渲染:Cull → 配 DrawObjectsPass → BeginRenderPass + SetEncoder → 管线 → End ──
    bool OnRenderView(AppFrameContext& ctx, const AppFrameTarget& target) override {
        if (!target.Window->IsMainWindow()) {
            return false;
        }
        render::TextureDescriptor bbDesc = target.BackBuffer->GetDesc();
        const uint32_t width = bbDesc.Width;
        const uint32_t height = bbDesc.Height;
        if (width == 0 || height == 0) {
            return false;
        }

        srp::Scene* scene = GetWorld()->GetScene();
        if (scene == nullptr || _cameraComp == nullptr) {
            return false;
        }

        srp::SceneView view{};
        _cameraComp->FillSceneView(view, width, height);
        srp::CullingResults cull = srp::CullAll(*scene, view);

        const uint32_t flight = ctx.FlightIndex();
        render::CommandBuffer* cmd = ctx.GetCommandBuffer();
        render::Device* device = GetDevice();

        // 瞬态 depth(本 flight),resize 自动重建。
        render::TextureDescriptor depthDesc{
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
        if (_resourcePool.Acquire("SceneDepth", flight, depthDesc, *device) == nullptr) {
            return false;
        }
        _resourcePool.Transition("SceneDepth", flight, render::TextureState::DepthWrite, *cmd);
        render::TextureViewDescriptor depthViewDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Format = DepthFormat,
            .Range = render::SubresourceRange::AllSub(),
            .Usage = render::TextureViewUsage::DepthWrite};
        render::TextureView* depthView = _resourcePool.GetView("SceneDepth", flight, depthViewDesc, *device);
        if (depthView == nullptr) {
            return false;
        }

        // 配置一个 UniversalForward DrawObjectsPass。
        srp::DrawObjectsPass::Desc passDesc{};
        passDesc.Event = srp::RenderPassEvent::BeforeRenderingOpaques;
        passDesc.ShaderTags = {"UniversalForward"};
        passDesc.SortFlags = srp::SortingCriteria::FrontToBack;
        passDesc.RenderState = srp::MeshPassRenderState::Opaque();
        passDesc.RTFormats.ColorFormats = {BackBufferFormat};
        passDesc.RTFormats.DepthFormat = DepthFormat;
        passDesc.PerObjectParamName = "gScene";
        passDesc.PerObjectByteSize = sizeof(SphereSceneConstants);
        passDesc.PerObjectFn = [](std::span<byte> dst, const srp::Renderer& renderer, const srp::SceneView& v) {
            if (dst.size() < sizeof(SphereSceneConstants)) {
                return;
            }
            SphereSceneConstants sc{};
            const Eigen::Matrix4f& model = renderer.WorldMatrix();
            const Eigen::Matrix4f mvp = v.ViewProjMatrix * model;
            WriteMatrix(sc.MVP, mvp);
            WriteMatrix(sc.Model, model);
            std::memcpy(dst.data(), &sc, sizeof(SphereSceneConstants));
        };
        srp::DrawObjectsPass scenePass{std::move(passDesc)};

        srp::RenderContext rc{GetGpuSystem(), _variantCache.get()};

        render::ColorAttachment colorAttachment{
            .Target = target.BackBufferView,
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
        render::RenderPassDescriptor rpDesc{
            .ColorAttachments = std::span{&colorAttachment, 1},
            .DepthStencilAttachment = depthAttachment,
            .Name = "Scene Pass"};
        auto encoderOpt = cmd->BeginRenderPass(rpDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin scene render pass");
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
        if (device->GetBackend() == render::RenderBackend::Vulkan) {
            vp.Y = static_cast<float>(height);
            vp.Height = -static_cast<float>(height);
        }
        encoder->SetViewport(vp);
        encoder->SetScissor(Rect{0, 0, width, height});

        rc.SetEncoder(encoder.get());
        _pipeline.SetSetupPasses([&](srp::RenderPipelineExecutor& exec, const srp::SceneView&, const srp::CullingResults&) {
            exec.EnqueuePass(&scenePass);
        });
        _pipeline.RenderSingleCamera(rc, cull);
        rc.SetEncoder(nullptr);

        cmd->EndRenderPass(std::move(encoder));
        return true;
    }

    void OnShutdown() override {
        _resourcePool.Clear();
        _sphereMeshComp = nullptr;
        _cameraComp = nullptr;
        _variantCache.reset();
        _sphereShader.reset();
    }

private:
    // —— srp 渲染设施(game 侧持有)——
    unique_ptr<srp::Shader> _sphereShader;
    unique_ptr<srp::ShaderVariantCache> _variantCache;
    srp::RenderPipeline _pipeline;
    srp::RenderResourcePool _resourcePool;

    // —— demo 内容状态 ——
    float _sphereSpin{0.0f};
    bool _showMonitor{true};

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
