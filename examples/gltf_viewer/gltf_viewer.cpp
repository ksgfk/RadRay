#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gltf_asset.h>
#include <radray/runtime/material.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/camera_control_component.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/renderer/render_context.h>
#include <radray/runtime/renderer/render_pass.h>
#include <radray/runtime/renderer/render_pipeline.h>
#include <radray/runtime/renderer/render_resource_pool.h>
#include <radray/runtime/renderer/scene.h>
#include <radray/runtime/renderer/scene_renderer.h>
#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/logger.h>
#include <radray/types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#ifndef RADRAY_GLTF_VIEWER_ASSET_DIR
#define RADRAY_GLTF_VIEWER_ASSET_DIR "."
#endif

using namespace radray;

namespace {

class ScenePass : public RenderPass {
public:
    ScenePass(
        render::TextureFormat colorFormat,
        render::TextureFormat depthFormat,
        render::ColorClearValue clearColor)
        : _colorFormat(colorFormat),
          _depthFormat(depthFormat),
          _clearColor(clearColor) {}

    std::string_view GetName() const noexcept override { return "GltfViewerScenePass"; }

    void Execute(RenderContext& ctx) override {
        if (ctx.Width == 0 || ctx.Height == 0 || ctx.Scene == nullptr ||
            ctx.CmdBuffer == nullptr || ctx.ColorTarget == nullptr ||
            ctx.Resources == nullptr || ctx.Device == nullptr || ctx.Gpu == nullptr) {
            return;
        }

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
        if (ctx.Resources->Acquire("GltfViewerDepth", ctx.FlightIndex, depthDesc, *ctx.Device) == nullptr) {
            return;
        }
        ctx.Resources->Transition("GltfViewerDepth", ctx.FlightIndex, render::TextureState::DepthWrite, *ctx.CmdBuffer);

        render::TextureViewDescriptor depthViewDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Format = _depthFormat,
            .Range = render::SubresourceRange::AllSub(),
            .Usage = render::TextureViewUsage::DepthWrite};
        render::TextureView* depthView = ctx.Resources->GetView("GltfViewerDepth", ctx.FlightIndex, depthViewDesc, *ctx.Device);
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
            .Name = "glTF Viewer Scene"};
        auto encoderOpt = ctx.CmdBuffer->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin glTF viewer scene pass");
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
        processorConfig.Gpu = ctx.Gpu;

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

}  // namespace

class GltfViewerApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    void SetInitialLoadPath(std::filesystem::path path) {
        _initialLoadPath = std::move(path);
    }

    void OnInit() override {
        InitMaterial();
        InitPipeline();
        SpawnCamera();
        ConfigureCameraControl();
        SetCameraFrame(Eigen::Vector3f::Zero(), 4.0f);

        _loadPath.resize(1024);
        const std::filesystem::path defaultPath = _initialLoadPath.empty()
            ? (std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / "model.gltf")
            : _initialLoadPath;
        const string initial = defaultPath.string();
        std::memcpy(_loadPath.data(), initial.c_str(), std::min(initial.size(), _loadPath.size() - 1));
        if (!_initialLoadPath.empty()) {
            _pendingLoadPath = _initialLoadPath;
            _pendingLoad = true;
        }
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        if (_pendingLoad) {
            LoadSceneFromPath(_pendingLoadPath);
            _pendingLoad = false;
        }
        TryExportReadyAsset();
        if (ImGuiSystem* imgui = GetSubsystem<ImGuiSystem>()) {
            if (imgui->Begin(ctx)) {
                DrawUi(ctx);
                imgui->End();
            }
        }
    }

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

        if (rc.Scene != nullptr) {
            SceneRenderer::Cull(*rc.Scene, rc.View, _visible);
            rc.Visible = &_visible;
        }
        rc.Resources = &_resourcePool;

        _pipeline.Render(rc);
        return true;
    }

    void OnShutdown() override {
        DestroyExportedScene();
        _resourcePool.Clear();
        _gltfAsset.Reset();
        _cameraControlComp = nullptr;
        _cameraComp = nullptr;
        _viewerMaterial.Reset();
    }

private:
    void InitMaterial() {
        std::filesystem::path shaderPath = std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / "gltf_viewer.hlsl";
        MaterialDescriptor matDesc{};
        matDesc.ShaderPath = shaderPath;
        matDesc.ShaderName = "gltf_viewer";
        matDesc.VsEntry = "VSMain";
        matDesc.PsEntry = "PSMain";
        matDesc.Primitive = render::PrimitiveState::Default();
        matDesc.Primitive.Cull = render::CullMode::Back;
        matDesc.DepthStencil = render::DepthStencilState::Default();
        matDesc.DepthStencil.Format = DepthFormat;
        const AssetId matId = Guid::Parse("91f5f8e0-7cb3-41c5-8b19-a62253f19f2a");
        _viewerMaterial = GetAssetManager()->Load<Material>(AssetLoadRequest{
            .Id = matId,
            .Task = LoadMaterial(*GetGpuSystem(), matDesc),
            .DebugName = "glTF viewer material"});
        if (!_viewerMaterial.IsValid()) {
            RADRAY_ERR_LOG("failed to start loading glTF viewer material");
        }
    }

    void InitPipeline() {
        auto pass = make_unique<ScenePass>(
            BackBufferFormat,
            DepthFormat,
            render::ColorClearValue{{0.06f, 0.07f, 0.08f, 1.0f}});
        _pipeline.AddPass(std::move(pass));
    }

    void SpawnCamera() {
        Actor* cameraActor = GetWorld()->SpawnActor<Actor>();
        _cameraComp = cameraActor->AddComponent<CameraComponent>();
        cameraActor->SetRootComponent(_cameraComp);
        _cameraComp->SetPerspective(Radian(60.0f), 0.01f, 10000.0f);
        _cameraControlComp = cameraActor->AddComponent<CameraControlComponent>();
        _cameraControlComp->SetCamera(_cameraComp);
        _cameraControlComp->BindToMainWindow(*this);
    }

    void ConfigureCameraControl() {
        if (_cameraControlComp == nullptr) {
            return;
        }
        CameraControl& control = _cameraControlComp->GetControl();
        control.MinDistance = 0.05f;
        control.MaxDistance = 10000.0f;
        control.OrbitSensitivity = 0.003f;
        control.PanSensitivity = 0.003f;
        control.DollySensitivity = 0.15f;
        control.UseTrackball = false;
        control.InvertZoom = false;
    }

    void LoadSceneFromPath(const std::filesystem::path& path) {
        UnloadCurrentScene();
        _loadError.clear();
        _selectedNode = -1;

        GltfAssetLoadOptions options{};
        options.DefaultMaterial = _viewerMaterial.AsAny();
        _gltfAsset = LoadGltfAsset(
            *GetAssetManager(),
            GetGpuSystem()->GetFrameUploadScheduler(),
            path,
            options);
        if (!_gltfAsset.IsValid()) {
            _loadError = "failed to start glTF asset load";
            RADRAY_ERR_LOG("{}", _loadError);
            return;
        }
    }

    void UnloadCurrentScene() {
        DestroyExportedScene();
        if (_gltfAsset.IsValid()) {
            if (GltfAsset* asset = _gltfAsset.Get()) {
                for (const GltfTextureDesc& texture : asset->GetTextures()) {
                    if (texture.Texture.IsValid()) {
                        GetAssetManager()->Unload(texture.Texture.GetAssetId());
                    }
                }
            }
            GetAssetManager()->Unload(_gltfAsset.GetAssetId());
            _gltfAsset.Reset();
        }
    }

    static void CollectAttachedActors(SceneComponent* component, Actor* exclude, vector<Actor*>& out) {
        if (component == nullptr) {
            return;
        }
        for (SceneComponent* child : component->GetAttachChildren()) {
            if (child == nullptr) {
                continue;
            }
            CollectAttachedActors(child, exclude, out);
            auto owner = child->GetOwner();
            if (!owner || owner.Get() == exclude) {
                continue;
            }
            Actor* actor = owner.Get();
            if (std::find(out.begin(), out.end(), actor) == out.end()) {
                out.push_back(actor);
            }
        }
    }

    void DestroyExportedScene() {
        Actor* rootActor = _rootActor;
        if (rootActor == nullptr) {
            return;
        }

        vector<Actor*> childActors;
        if (auto rootComponent = rootActor->GetRootComponent()) {
            CollectAttachedActors(rootComponent.Get(), rootActor, childActors);
        }

        for (Actor* actor : childActors) {
            if (actor == nullptr) {
                continue;
            }
            auto actorWorld = actor->GetWorld();
            if (actorWorld) {
                actorWorld.Get()->DestroyActor(actor);
            }
        }

        auto rootWorld = rootActor->GetWorld();
        if (rootWorld) {
            rootWorld.Get()->DestroyActor(rootActor);
        }
        _rootActor = nullptr;
    }

    void TryExportReadyAsset() {
        if (!_gltfAsset.IsValid()) {
            return;
        }
        if (_gltfAsset.IsFaulted()) {
            if (_loadError.empty()) {
                _loadError = "glTF asset load failed";
            }
            return;
        }
        if (_rootActor == nullptr && _gltfAsset.IsReady()) {
            _rootActor = _gltfAsset->ExportToScene(*GetWorld());
            FrameCameraToAsset();
        }
    }

    void FrameCameraToAsset() {
        Eigen::Vector3f center{Eigen::Vector3f::Zero()};
        float distance = 4.0f;
        const GltfAsset* asset = _gltfAsset.Get();
        if (asset != nullptr && asset->HasBounds()) {
            center = (asset->GetBoundsMin() + asset->GetBoundsMax()) * 0.5f;
            const float radius = std::max((asset->GetBoundsMax() - asset->GetBoundsMin()).norm() * 0.5f, 0.1f);
            distance = radius * 2.8f;
        }
        SetCameraFrame(center, distance);
    }

    void SetCameraFrame(const Eigen::Vector3f& center, float distance) {
        if (_cameraControlComp != nullptr) {
            _cameraControlComp->SetFrame(center, distance, 0.0f, Radian(20.0f));
        }
    }

    void DrawNodeTree(const GltfAsset& asset, int nodeIndex) {
        const vector<GltfNodeDesc>& nodes = asset.GetNodes();
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size())) {
            return;
        }
        const GltfNodeDesc& node = nodes[static_cast<size_t>(nodeIndex)];
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (node.Children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (_selectedNode == nodeIndex) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<intptr_t>(nodeIndex)),
            flags,
            "%s%s",
            node.Name.c_str(),
            node.HasMesh ? "  [mesh]" : "");
        if (ImGui::IsItemClicked()) {
            _selectedNode = nodeIndex;
        }
        if (open) {
            for (int child : node.Children) {
                DrawNodeTree(asset, child);
            }
            ImGui::TreePop();
        }
    }

    void DrawUi(const AppUpdateContext& ctx) {
        ImGui::SetNextWindowSize(ImVec2{380.0f, 560.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("glTF Viewer")) {
            ImGui::Text("Frame %.3f ms  GPU %.3f ms", ctx.DeltaTime.count() * 1000.0f, GetGpuSystem()->GetLastGpuTimeMs());
            ImGui::InputText("Path", _loadPath.data(), _loadPath.size());
            if (ImGui::Button("Load")) {
                _pendingLoadPath = _loadPath.data();
                _pendingLoad = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Unload")) {
                UnloadCurrentScene();
                _selectedNode = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Frame")) {
                FrameCameraToAsset();
            }
            if (!_loadError.empty()) {
                ImGui::TextWrapped("Error: %s", _loadError.c_str());
            }
            ImGui::Separator();
            if (_gltfAsset.IsValid()) {
                const char* state = "loading";
                if (_gltfAsset.IsReady()) {
                    state = "ready";
                } else if (_gltfAsset.IsFaulted()) {
                    state = "faulted";
                } else if (_gltfAsset.IsCanceled()) {
                    state = "canceled";
                }
                ImGui::Text("Asset: %s", state);
            }
            if (GltfAsset* asset = _gltfAsset.Get()) {
                const size_t primitiveCount = asset->GetPrimitives().size();
                const size_t actorCount = _rootActor != nullptr ? primitiveCount + 1 : 0;
                const size_t componentCount = _rootActor != nullptr ? primitiveCount : 0;
                ImGui::Text("Primitives: %zu", primitiveCount);
                ImGui::Text("Materials: %zu", asset->GetMaterialNames().size());
                ImGui::Text("Textures: %zu", asset->GetTextures().size());
                ImGui::Text("Nodes: %zu", asset->GetNodes().size());
                ImGui::Text("Actors: %zu", actorCount);
                ImGui::Text("Components: %zu", componentCount);
                if (ImGui::CollapsingHeader("Scene Nodes", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (int root : asset->GetRootNodes()) {
                        DrawNodeTree(*asset, root);
                    }
                }
                if (ImGui::CollapsingHeader("Materials")) {
                    const vector<string>& materialNames = asset->GetMaterialNames();
                    for (size_t i = 0; i < materialNames.size(); ++i) {
                        ImGui::BulletText("%zu: %s", i, materialNames[i].c_str());
                    }
                }
                if (_selectedNode >= 0 && _selectedNode < static_cast<int>(asset->GetNodes().size())) {
                    const GltfNodeDesc& node = asset->GetNodes()[static_cast<size_t>(_selectedNode)];
                    ImGui::Separator();
                    ImGui::Text("Selected: %s", node.Name.c_str());
                    ImGui::Text("Parent: %d", node.Parent);
                    ImGui::Text("Children: %zu", node.Children.size());
                    ImGui::Text("Has mesh: %s", node.HasMesh ? "yes" : "no");
                }
            }
        }
        ImGui::End();
    }

    RenderPipeline _pipeline;
    RenderResourcePool _resourcePool;
    VisiblePrimitiveList _visible;

    StreamingAssetRef<GltfAsset> _gltfAsset;
    Actor* _rootActor{nullptr};
    StreamingAssetRef<Material> _viewerMaterial;

    vector<char> _loadPath;
    string _loadError;
    std::filesystem::path _initialLoadPath;
    bool _pendingLoad{false};
    std::filesystem::path _pendingLoadPath;
    int _selectedNode{-1};

    CameraComponent* _cameraComp{nullptr};
    CameraControlComponent* _cameraControlComp{nullptr};
};

int main(int argc, char* argv[]) {
    ApplicationRuntimeDescriptor desc{};
    desc.Backend = render::RenderBackend::Vulkan;
    desc.AppName = "RadRay glTF Viewer";
    desc.EngineName = "RadRay";
    desc.WindowTitle = "RadRay glTF Viewer";
    desc.WindowWidth = 1280;
    desc.WindowHeight = 720;
    desc.BackBufferFormat = GltfViewerApp::BackBufferFormat;
    desc.PresentMode = render::PresentMode::FIFO;

    std::optional<std::filesystem::path> loadPath;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--backend" && i + 1 < argc) {
            std::string_view backendStr{argv[++i]};
            if (backendStr == "vulkan") {
                desc.Backend = render::RenderBackend::Vulkan;
            } else if (backendStr == "d3d12") {
                desc.Backend = render::RenderBackend::D3D12;
            }
        } else if (arg == "--valid-layer") {
            desc.EnableValidation = true;
        } else if (arg == "--multithread") {
            desc.Multithreaded = true;
        } else if (arg == "--load" && i + 1 < argc) {
            loadPath = std::filesystem::path{argv[++i]};
        }
    }

    GltfViewerApp app{};
    if (loadPath.has_value()) {
        app.SetInitialLoadPath(loadPath.value());
    }
    app.RegisterSubsystem<ImGuiSystem>();
    return app.Run(desc);
}
