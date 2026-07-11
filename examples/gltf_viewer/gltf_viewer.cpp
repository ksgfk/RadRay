// gltf_viewer: glTF 场景查看器示例 (ForwardPipeline 默认前向管线)。
//
// 用 LoadGltfAsset 异步加载一个 .gltf/.glb, 走引擎默认的 gltf_standard 前向材质
// (metallic-roughness + 贴图 + KHR 扩展, 支持点光源立方体阴影), SpawnScene 挂到 World。
//
// 材质翻译由当前渲染管线的标准材质工厂 (RenderSystem::GetStandardMaterialFactory) 提供
// (shader 源随引擎部署在 <exe>/shaderlib/forward_pipeline/)。示例只负责加载 + 取景。

#include <radray/runtime/application.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/gltf_asset.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/camera_control_component.h>
#include <radray/runtime/render_framework/standard_material_factory.h>

#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/logger.h>

#include <algorithm>
#include <filesystem>
#include <string_view>

using namespace radray;

class GltfViewerApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;

    void SetGltfPath(std::filesystem::path path) { _gltfPath = std::move(path); }

    void OnInit() override {
        if (_gltfPath.empty()) {
            RADRAY_ERR_LOG("gltf_viewer: no glTF path provided (use --gltf <path>)");
            return;
        }
        RenderSystem* renderSystem = GetRenderSystem();
        if (renderSystem == nullptr) {
            RADRAY_ERR_LOG("gltf_viewer: render system unavailable");
            return;
        }
        BuildLight();
        BuildCamera(Eigen::Vector3f::Zero(), 3.0f);  // 待资产就绪后重新 frame。
        RequestGltf();
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        (void)ctx;
        TrySpawnGltf();
    }

    void OnShutdown() override {}

private:
    void RequestGltf() {
        AssetManager* assets = GetAssetManager();
        FrameUploadScheduler& uploads = GetGpuSystem()->GetFrameUploadScheduler();
        _gltfAsset = LoadGltfAsset(*assets, uploads, _gltfPath);
    }

    void TrySpawnGltf() {
        if (_gltfSpawned || !_gltfAsset.IsValid()) {
            return;
        }
        if (_gltfAsset.IsFaulted() || _gltfAsset.IsCanceled()) {
            RADRAY_ERR_LOG("gltf_viewer: failed to load glTF '{}'", _gltfPath.string());
            _gltfSpawned = true;  // 别再重试。
            return;
        }
        GltfAsset* asset = _gltfAsset.Get();
        if (asset == nullptr) {
            return;  // 仍在加载。
        }
        RenderSystem* renderSystem = GetRenderSystem();
        Nullable<IStandardMaterialFactory*> factory =
            renderSystem != nullptr ? renderSystem->GetStandardMaterialFactory() : nullptr;
        if (factory == nullptr) {
            RADRAY_ERR_LOG("gltf_viewer: no standard material factory from render pipeline");
            _gltfSpawned = true;
            return;
        }

        asset->SpawnScene(*GetWorld(), *factory);
        _gltfSpawned = true;

        // 相机取景到场景包围盒。
        if (asset->HasBounds()) {
            const Eigen::Vector3f center = 0.5f * (asset->GetBoundsMin() + asset->GetBoundsMax());
            const float radius = 0.5f * (asset->GetBoundsMax() - asset->GetBoundsMin()).norm();
            const float distance = std::max(radius * 2.5f, 0.1f);
            if (_cameraControl != nullptr) {
                _cameraControl->SetFrame(center, distance);
            }
            if (_camera != nullptr) {
                _camera->SetPerspective(Radian(50.0f), std::max(radius * 0.01f, 0.01f), std::max(radius * 20.0f, 100.0f));
            }
        }
        RADRAY_INFO_LOG("gltf_viewer: spawned glTF scene from '{}'", _gltfPath.string());
    }

    void BuildLight() {
        Actor* actor = GetWorld()->SpawnActor<Actor>();
        PointLightComponent* light = actor->AddComponent<PointLightComponent>();
        actor->SetRootComponent(light);
        light->SetWorldLocation(Eigen::Vector3f{3.0f, 4.0f, 3.0f});
        light->SetLightColor(Eigen::Vector3f{1.0f, 0.95f, 0.9f});
        light->SetIntensity(60.0f);
        light->SetAttenuationRadius(50.0f);
    }

    void BuildCamera(const Eigen::Vector3f& target, float distance) {
        Actor* actor = GetWorld()->SpawnActor<Actor>();
        CameraComponent* camera = actor->AddComponent<CameraComponent>();
        actor->SetRootComponent(camera);
        camera->SetPerspective(Radian(50.0f), 0.05f, 500.0f);
        camera->SetWorldLocation(target + Eigen::Vector3f{0.0f, 0.0f, distance});

        CameraControlComponent* control = actor->AddComponent<CameraControlComponent>();
        control->SetCamera(camera);
        control->SetFrame(target, distance, 0.0f, 0.0f);
        control->BindToMainWindow(*this);
        _camera = camera;
        _cameraControl = control;
    }

    std::filesystem::path _gltfPath{};
    StreamingAssetRef<GltfAsset> _gltfAsset{};
    bool _gltfSpawned{false};

    CameraComponent* _camera{nullptr};
    CameraControlComponent* _cameraControl{nullptr};
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
    desc.PresentMode = render::PresentMode::Immediate;

    std::filesystem::path gltfPath;
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
        if (arg == "--gltf" && i + 1 < argc) {
            gltfPath = std::filesystem::path{argv[i + 1]};
        }
        if (arg == "--valid-layer") {
            desc.EnableValidation = true;
        }
        if (arg == "--multithread") {
            desc.Multithreaded = true;
        }
    }

    GltfViewerApp app{};
    if (!gltfPath.empty()) {
        app.SetGltfPath(std::move(gltfPath));
    }
    return app.Run(desc);
}
