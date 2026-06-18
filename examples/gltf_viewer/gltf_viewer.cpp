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
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/renderer/render_context.h>
#include <radray/runtime/renderer/render_pass.h>
#include <radray/runtime/renderer/render_pipeline.h>
#include <radray/runtime/renderer/render_resource_pool.h>
#include <radray/runtime/renderer/scene.h>
#include <radray/runtime/renderer/scene_renderer.h>
#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/camera_control.h>
#include <radray/logger.h>
#include <radray/types.h>
#include <radray/window/native_window.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#include <radray/platform/win32_headers.h>
#endif

#include <algorithm>
#include <cmath>
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

Eigen::Quaternionf MakeCameraRotation(const Eigen::Vector3f& forward) {
    Eigen::Vector3f f = forward.squaredNorm() > 1e-8f ? forward.normalized() : Eigen::Vector3f{0.0f, 0.0f, 1.0f};
    Eigen::Vector3f up = Eigen::Vector3f::UnitY();
    if (std::abs(f.dot(up)) > 0.98f) {
        up = Eigen::Vector3f::UnitZ();
    }
    Eigen::Vector3f right = up.cross(f).normalized();
    Eigen::Vector3f cameraUp = f.cross(right).normalized();
    Eigen::Matrix3f rotation = Eigen::Matrix3f::Identity();
    rotation.col(0) = right;
    rotation.col(1) = cameraUp;
    rotation.col(2) = f;
    Eigen::Quaternionf quat{rotation};
    quat.normalize();
    return quat;
}

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
        ConfigureCameraControl();
        AttachCameraInput();
        SpawnCamera();
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
        PollCameraInput();
        ApplyCameraToComponent();
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
        _inputConnections.clear();
        _resourcePool.Clear();
        _gltfAsset.Reset();
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
        matDesc.Primitive.Cull = render::CullMode::None;
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
    }

    void ConfigureCameraControl() {
        _cameraControl.MinDistance = 0.05f;
        _cameraControl.MaxDistance = 10000.0f;
        _cameraControl.OrbitSensitivity = 0.003f;
        _cameraControl.PanSensitivity = 0.003f;
        _cameraControl.DollySensitivity = 0.15f;
        _cameraControl.UseTrackball = false;
        _cameraControl.InvertZoom = false;
    }

    void AttachCameraInput() {
        WindowManager* windows = GetWindowManager();
        AppWindow* mainWindow = windows != nullptr ? windows->GetMainWindow() : nullptr;
        NativeWindow* nativeWindow = mainWindow != nullptr ? mainWindow->GetNativeWindow() : nullptr;
        if (nativeWindow == nullptr) {
            RADRAY_WARN_LOG("glTF viewer camera input disabled: no main window");
            return;
        }

        _mainNativeWindow = nativeWindow;
#ifndef RADRAY_PLATFORM_WINDOWS
        _inputConnections.emplace_back(nativeWindow->EventTouch().connect([this](int x, int y, MouseButton button, Action action) {
            OnCameraPointer(x, y, button, action);
        }));
#endif
        _inputConnections.emplace_back(nativeWindow->EventMouseWheel().connect([this](int delta) {
            OnCameraWheel(delta);
        }));
        _inputConnections.emplace_back(nativeWindow->EventMouseLeave().connect([this]() {
            _cameraControl.IsOrbiting = false;
            _cameraControl.IsPanning = false;
            _cameraControl.IsDollying = false;
            _cameraControl.WheelDelta = 0.0f;
        }));
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
        _cameraControl.Reset();
        _cameraControl.SetOrbitTarget(center);
        distance = Clamp(distance, _cameraControl.MinDistance, _cameraControl.MaxDistance);
        _cameraTarget = center;
        _cameraDistance = distance;
        _cameraYaw = 0.0f;
        _cameraPitch = Radian(20.0f);
        UpdateCameraTransform();
        ApplyCameraToComponent();
    }

    bool IsUiBlockingCameraInput() const {
        if (ImGui::GetCurrentContext() == nullptr) {
            return false;
        }
        const ImGuiIO& io = ImGui::GetIO();
        return io.WantCaptureMouse ||
               ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ||
               ImGui::IsAnyItemActive();
    }

    void ApplyCameraControl(const Eigen::Vector2f& mousePos, bool orbit, bool pan, bool inputBlocked) {
        const Eigen::Vector2f delta = mousePos - _lastCameraPollMousePos;
        _lastCameraPollMousePos = mousePos;
        _cameraControl.CurrentMousePos = mousePos;
        if (inputBlocked) {
            _cameraControl.IsOrbiting = false;
            _cameraControl.IsPanning = false;
            _cameraControl.IsDollying = false;
            _cameraControl.LastMousePos = mousePos;
            return;
        }

        if (delta.squaredNorm() < 1e-6f) {
            return;
        }

        if (orbit) {
            _cameraYaw += delta.x() * _cameraControl.OrbitSensitivity;
            _cameraPitch -= delta.y() * _cameraControl.OrbitSensitivity;
            _cameraPitch = Clamp(_cameraPitch, Radian(-85.0f), Radian(85.0f));
            UpdateCameraTransform();
        } else if (pan) {
            const Eigen::Vector3f right = _cameraRotation * Eigen::Vector3f::UnitX();
            const Eigen::Vector3f up = _cameraRotation * Eigen::Vector3f::UnitY();
            const float scale = _cameraDistance * _cameraControl.PanSensitivity * 0.5f;
            const Eigen::Vector3f panVector = right * (delta.x() * scale) + up * (delta.y() * scale);
            _cameraTarget += panVector;
            _cameraControl.SetOrbitTarget(_cameraTarget);
            UpdateCameraTransform();
        }
    }

    void PollCameraInput() {
#ifdef RADRAY_PLATFORM_WINDOWS
        if (_mainNativeWindow == nullptr) {
            _cameraPollAnyButtonDown = false;
            _cameraInputCapturedByUi = false;
            _cameraControl.IsOrbiting = false;
            _cameraControl.IsPanning = false;
            return;
        }

        POINT screenPos{};
        if (::GetCursorPos(&screenPos) == 0) {
            return;
        }
        const Eigen::Vector2i clientPos = _mainNativeWindow->ScreenToClient(Eigen::Vector2i{screenPos.x, screenPos.y});
        const Eigen::Vector2i windowSize = _mainNativeWindow->GetSize();
        const bool insideClient =
            clientPos.x() >= 0 &&
            clientPos.y() >= 0 &&
            clientPos.x() < windowSize.x() &&
            clientPos.y() < windowSize.y();

        const bool leftDown = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool rightDown = (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool middleDown = (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        const bool anyDown = leftDown || rightDown || middleDown;
        const Eigen::Vector2f mousePos{static_cast<float>(clientPos.x()), static_cast<float>(clientPos.y())};
        const bool uiBlockingCamera = IsUiBlockingCameraInput();

        if (!anyDown) {
            _cameraPollAnyButtonDown = false;
            _cameraInputCapturedByUi = false;
            _cameraControl.IsOrbiting = false;
            _cameraControl.IsPanning = false;
            _cameraControl.IsDollying = false;
            _cameraControl.LastMousePos = mousePos;
            _lastCameraPollMousePos = mousePos;
            return;
        }

        if (!_cameraPollAnyButtonDown) {
            _cameraPollAnyButtonDown = true;
            _cameraInputCapturedByUi = !insideClient || uiBlockingCamera;
            _cameraControl.LastMousePos = mousePos;
            _lastCameraPollMousePos = mousePos;
        }

        const bool pan = rightDown || middleDown;
        const bool orbit = leftDown && !pan;
        ApplyCameraControl(mousePos, orbit, pan, _cameraInputCapturedByUi);
#endif
    }

    void OnCameraPointer(int x, int y, MouseButton button, Action action) {
        if (action == Action::UNKNOWN || button == MouseButton::UNKNOWN) {
            return;
        }

        const Eigen::Vector2f mousePos{static_cast<float>(x), static_cast<float>(y)};
        _cameraControl.CurrentMousePos = mousePos;

        if (action == Action::PRESSED) {
            if (IsUiBlockingCameraInput()) {
                _cameraInputCapturedByUi = true;
                _cameraControl.IsOrbiting = false;
                _cameraControl.IsPanning = false;
                _cameraControl.IsDollying = false;
                _cameraControl.LastMousePos = mousePos;
                return;
            }
            _cameraInputCapturedByUi = false;
            _cameraControl.LastMousePos = mousePos;
            if (button == MouseButton::BUTTON_LEFT) {
                _cameraControl.IsOrbiting = true;
            } else if (button == MouseButton::BUTTON_RIGHT || button == MouseButton::BUTTON_MIDDLE) {
                _cameraControl.IsPanning = true;
            }
            return;
        }

        if (action == Action::RELEASED) {
            if (button == MouseButton::BUTTON_LEFT) {
                _cameraControl.IsOrbiting = false;
            } else if (button == MouseButton::BUTTON_RIGHT || button == MouseButton::BUTTON_MIDDLE) {
                _cameraControl.IsPanning = false;
            }
            _cameraControl.LastMousePos = mousePos;
            if (!_cameraControl.IsOrbiting && !_cameraControl.IsPanning) {
                _cameraInputCapturedByUi = false;
            }
            return;
        }

        if (action == Action::REPEATED && !_cameraInputCapturedByUi) {
            ApplyCameraControl(mousePos, _cameraControl.IsOrbiting, _cameraControl.IsPanning, false);
        }
    }

    void OnCameraWheel(int delta) {
        if (IsUiBlockingCameraInput()) {
            return;
        }
        const float wheel = static_cast<float>(delta) / 120.0f;
        const float move = wheel * _cameraControl.DollySensitivity * _cameraDistance;
        _cameraDistance = Clamp(_cameraDistance - move, _cameraControl.MinDistance, _cameraControl.MaxDistance);
        UpdateCameraTransform();
    }

    void UpdateCameraTransform() {
        const float cp = std::cos(_cameraPitch);
        const Eigen::Vector3f dir{
            std::sin(_cameraYaw) * cp,
            std::sin(_cameraPitch),
            std::cos(_cameraYaw) * cp};
        _cameraPosition = _cameraTarget - dir * _cameraDistance;
        _cameraRotation = MakeCameraRotation(_cameraTarget - _cameraPosition);
        _cameraControl.SetOrbitTarget(_cameraTarget);
        _cameraControl.UpdateDistance(_cameraPosition);
    }

    void ApplyCameraToComponent() {
        if (_cameraComp == nullptr) {
            return;
        }
        _cameraComp->SetWorldLocation(_cameraPosition);
        _cameraComp->SetWorldRotation(_cameraRotation);
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
                ImGui::Text("Images: %zu", asset->GetImages().size());
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

    CameraControl _cameraControl;
    Eigen::Vector3f _cameraTarget{Eigen::Vector3f::Zero()};
    float _cameraDistance{4.0f};
    float _cameraYaw{0.0f};
    float _cameraPitch{Radian(20.0f)};
    Eigen::Vector3f _cameraPosition{0.0f, 0.0f, -4.0f};
    Eigen::Quaternionf _cameraRotation{Eigen::Quaternionf::Identity()};
    CameraComponent* _cameraComp{nullptr};
    NativeWindow* _mainNativeWindow{nullptr};
    vector<sigslot::scoped_connection> _inputConnections;
    bool _cameraInputCapturedByUi{false};
    bool _cameraPollAnyButtonDown{false};
    Eigen::Vector2f _lastCameraPollMousePos{Eigen::Vector2f::Zero()};
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
