// sphere_demo: ForwardPipeline 默认前向管线的最小演示。
//
// 渲染多个 UV 球 (不透明金属 / alpha-test / 半透明双面) + 一块接收阴影的地面,
// 点光源 + Principled BRDF, 支持点光源立方体阴影。走引擎默认的 forward_pass 着色
// (shader 源随引擎部署在 <exe>/shaderlib/forward_pipeline/forward_pass.hlsl)。

#include <radray/runtime/application.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/camera_control_component.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/render_framework/forward_pipeline_shader.h>
#include <radray/runtime/render_framework/standard_material_factory.h>

#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/logger.h>

#include <algorithm>
#include <limits>
#include <string_view>

using namespace radray;

namespace {

constexpr uint32_t kSphereSlices = 64;
constexpr float kSphereRadius = 1.0f;

// 计算 TriangleMesh 的 AABB。
void ComputeBounds(const TriangleMesh& mesh, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax) {
    outMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    outMax = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
    for (const Eigen::Vector3f& p : mesh.Positions) {
        outMin = outMin.cwiseMin(p);
        outMax = outMax.cwiseMax(p);
    }
}

// 自定义 StaticMesh 加载协程: 上传 GPU 数据后补全 section + bounds。
AssetLoadTask LoadDemoMesh(FrameUploadScheduler& frameUploads, MeshResource meshResource, Eigen::Vector3f boundsMin, Eigen::Vector3f boundsMax) {
    if (meshResource.Primitives.empty()) {
        co_return AssetLoadResult::Failure("demo mesh resource has no primitive");
    }
    const uint32_t indexCount = meshResource.Primitives[0].IndexBuffer.IndexCount;
    const uint32_t vertexCount = meshResource.Primitives[0].VertexCount;

    FrameUploadScope frame = co_await frameUploads.BeginUpload();
    std::optional<GpuMesh> renderMesh =
        frame.GetUploader().UploadMeshResource(frame.GetCommandBuffer(), meshResource);
    if (!renderMesh.has_value()) {
        co_return AssetLoadResult::Failure("demo mesh upload recording failed");
    }
    co_await frame.WaitGpu();

    auto mesh = make_unique<StaticMesh>(
        std::move(meshResource),
        make_shared<GpuMesh>(std::move(renderMesh.value())));
    vector<StaticMeshSection> sections;
    sections.push_back(StaticMeshSection{
        /*primitiveIndex*/ 0,
        /*firstIndex*/ 0,
        /*indexCount*/ indexCount,
        /*minVertexIndex*/ 0,
        /*maxVertexIndex*/ vertexCount > 0 ? vertexCount - 1 : 0});
    mesh->SetSections(std::move(sections));
    mesh->SetBounds(boundsMin, boundsMax);
    co_return AssetLoadResult::Success(std::move(mesh));
}

}  // namespace

class SphereDemoApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;

    void OnInit() override {
        if (!InitializeMaterialFactory()) {
            RADRAY_ERR_LOG("sphere_demo: failed to initialize the standard material factory");
            return;
        }
        BuildLight();
        BuildSpheres();
        BuildGround();
        BuildCamera(Eigen::Vector3f::Zero(), 4.0f);
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        (void)ctx;
        TryAssignMaterials();
    }

    void OnShutdown() override {}

private:
    struct MeshInstance {
        StaticMeshComponent* Mesh{nullptr};
        StreamingAssetRef<MaterialAsset> Material{};
        bool Assigned{false};
    };

    bool InitializeMaterialFactory() {
        RenderSystem* renderSystem = GetRenderSystem();
        if (renderSystem == nullptr) {
            return false;
        }
        _materialFactory = renderSystem->GetStandardMaterialFactory();
        return _materialFactory.HasValue();
    }

    // proxy 由 StaticMeshComponent 异步创建。故每帧尝试, 直到 proxy 出现并绑材质到所有 section。
    void TryAssignMaterials() {
        for (MeshInstance& inst : _instances) {
            if (inst.Assigned || inst.Mesh == nullptr || !inst.Material.IsReady()) {
                continue;
            }
            PrimitiveSceneProxy* proxy = inst.Mesh->GetSceneProxy();
            if (proxy == nullptr) {
                continue;
            }
            auto* smProxy = static_cast<StaticMeshSceneProxy*>(proxy);
            const uint32_t sectionCount = smProxy->GetSectionCount();
            if (sectionCount == 0) {
                continue;
            }
            for (uint32_t s = 0; s < sectionCount; ++s) {
                inst.Mesh->SetMaterial(s, inst.Material);
            }
            inst.Assigned = true;
        }
    }

    static ForwardMaterialConstants MakeConstants(
        float r, float g, float b, float a,
        float metallic, float roughness, float alphaCutoff = 0.5f) {
        ForwardMaterialConstants mc{};
        mc.BaseColor[0] = r;
        mc.BaseColor[1] = g;
        mc.BaseColor[2] = b;
        mc.BaseColor[3] = a;
        mc.Pbr[0] = metallic;
        mc.Pbr[1] = roughness;
        mc.Pbr[2] = alphaCutoff;   // alphaCutoff (仅 _ALPHATEST_ON 时消费)
        mc.Pbr[3] = 1.0f;          // normalScale (无法线贴图时无影响)
        mc.Principled0[0] = 0.5f;  // specular
        mc.Principled0[1] = 0.0f;  // specular tint
        mc.Principled0[3] = 1.0f;  // clearcoat gloss
        mc.Principled2[1] = 1.5f;  // eta
        return mc;
    }

    StreamingAssetRef<MaterialAsset> CreateMaterial(
        bool transparent,
        RenderQueue queue,
        const ForwardMaterialConstants& mc,
        std::initializer_list<std::string_view> keywords) {
        StandardMaterialDescription desc{};
        desc.AlphaMode = transparent
                             ? StandardAlphaMode::Blend
                             : (queue == RenderQueue::AlphaTest ? StandardAlphaMode::Mask
                                                                : StandardAlphaMode::Opaque);
        desc.DoubleSided = std::ranges::find(keywords, forward_pipeline::kKwDoubleSided) != keywords.end();
        StreamingAssetRef<MaterialAsset> mat = _materialFactory->CreateMaterial(desc, {});
        if (!mat.IsValid()) {
            RADRAY_ERR_LOG("sphere_demo: failed to create material");
            return {};
        }
        mat->SetRenderQueue(queue);
        // PSO 固定功能状态 (blend / zwrite / cull) 由材质覆盖: 透明走 alpha blend + 关深度写 + 双面。
        if (transparent) {
            mat->SetRenderState(MakeForwardTransparentRenderState());
        }
        for (std::string_view kw : keywords) {
            mat->EnableKeyword(kw);
        }
        mat->SetConstantBlock("gMaterial", &mc, sizeof(mc));
        return mat;
    }

    void SpawnSphere(const Eigen::Vector3f& location, StreamingAssetRef<MaterialAsset> material, const char* debugName) {
        TriangleMesh triangle;
        triangle.InitAsUVSphere(kSphereRadius, kSphereSlices);
        Eigen::Vector3f boundsMin, boundsMax;
        ComputeBounds(triangle, boundsMin, boundsMax);

        MeshResource meshResource;
        triangle.ToSimpleMeshResource(&meshResource);

        AssetManager* assets = GetAssetManager();
        FrameUploadScheduler& uploads = GetGpuSystem()->GetFrameUploadScheduler();
        StreamingAssetRef<StaticMesh> meshRef = assets->Load<StaticMesh>(AssetLoadRequest{
            .Id = Guid::NewGuid(),
            .Task = LoadDemoMesh(uploads, std::move(meshResource), boundsMin, boundsMax),
            .DebugName = debugName});

        Actor* actor = GetWorld()->SpawnActor<Actor>();
        StaticMeshComponent* mesh = actor->AddComponent<StaticMeshComponent>();
        actor->SetRootComponent(mesh);
        mesh->SetStaticMesh(meshRef);
        mesh->SetWorldLocation(location);
        _instances.push_back(MeshInstance{mesh, std::move(material), false});
    }

    void BuildSpheres() {
        StreamingAssetRef<MaterialAsset> opaque = CreateMaterial(
            /*transparent*/ false, RenderQueue::Geometry,
            MakeConstants(0.82f, 0.67f, 0.34f, 1.0f, /*metallic*/ 1.0f, /*roughness*/ 0.35f),
            {});
        SpawnSphere(Eigen::Vector3f{-2.4f, 0.0f, 0.0f}, opaque, "OpaqueSphere");

        StreamingAssetRef<MaterialAsset> masked = CreateMaterial(
            /*transparent*/ false, RenderQueue::AlphaTest,
            MakeConstants(0.35f, 0.75f, 0.35f, 1.0f, /*metallic*/ 0.0f, /*roughness*/ 0.6f, /*cutoff*/ 0.5f),
            {forward_pipeline::kKwAlphaTest});
        SpawnSphere(Eigen::Vector3f{0.0f, 0.0f, 0.0f}, masked, "AlphaTestSphere");

        StreamingAssetRef<MaterialAsset> transparent = CreateMaterial(
            /*transparent*/ true, RenderQueue::Transparent,
            MakeConstants(0.30f, 0.45f, 0.90f, 0.45f, /*metallic*/ 0.0f, /*roughness*/ 0.15f),
            {forward_pipeline::kKwAlphaBlend, forward_pipeline::kKwDoubleSided});
        SpawnSphere(Eigen::Vector3f{2.4f, 0.0f, 0.0f}, transparent, "TransparentSphere");
    }

    // 地面: 一块压扁的大 cube, 不透明漫反射, 接收 (并投射) 点光源阴影。
    void BuildGround() {
        TriangleMesh cube;
        cube.InitAsCube(1.0f);
        const Eigen::Vector3f scale{12.0f, 0.15f, 12.0f};
        for (Eigen::Vector3f& p : cube.Positions) {
            p = p.cwiseProduct(scale);
        }
        Eigen::Vector3f boundsMin, boundsMax;
        ComputeBounds(cube, boundsMin, boundsMax);

        MeshResource meshResource;
        cube.ToSimpleMeshResource(&meshResource);

        AssetManager* assets = GetAssetManager();
        FrameUploadScheduler& uploads = GetGpuSystem()->GetFrameUploadScheduler();
        StreamingAssetRef<StaticMesh> meshRef = assets->Load<StaticMesh>(AssetLoadRequest{
            .Id = Guid::NewGuid(),
            .Task = LoadDemoMesh(uploads, std::move(meshResource), boundsMin, boundsMax),
            .DebugName = "Ground"});

        StreamingAssetRef<MaterialAsset> mat = CreateMaterial(
            /*transparent*/ false, RenderQueue::Geometry,
            MakeConstants(0.5f, 0.5f, 0.55f, 1.0f, /*metallic*/ 0.0f, /*roughness*/ 0.8f),
            {});

        Actor* actor = GetWorld()->SpawnActor<Actor>();
        StaticMeshComponent* mesh = actor->AddComponent<StaticMeshComponent>();
        actor->SetRootComponent(mesh);
        mesh->SetStaticMesh(meshRef);
        mesh->SetWorldLocation(Eigen::Vector3f{0.0f, -1.6f, 0.0f});
        _instances.push_back(MeshInstance{mesh, std::move(mat), false});
    }

    void BuildLight() {
        Actor* actor = GetWorld()->SpawnActor<Actor>();
        PointLightComponent* light = actor->AddComponent<PointLightComponent>();
        actor->SetRootComponent(light);
        light->SetWorldLocation(Eigen::Vector3f{3.0f, 4.0f, 3.0f});
        light->SetLightColor(Eigen::Vector3f{1.0f, 0.95f, 0.9f});
        light->SetIntensity(60.0f);
        light->SetAttenuationRadius(50.0f);

        // 方向光 + 级联阴影 (CSM): 从斜上方照下, 让场景产生方向性阴影。
        Actor* dirActor = GetWorld()->SpawnActor<Actor>();
        DirectionalLightComponent* dir = dirActor->AddComponent<DirectionalLightComponent>();
        dirActor->SetRootComponent(dir);
        // 光照方向取自世界旋转的 +Z: 让 +Z 指向 (-0.5, -1, -0.5) 的斜下方。
        const Eigen::Vector3f lightDir = Eigen::Vector3f{-0.5f, -1.0f, -0.5f}.normalized();
        dir->SetWorldRotation(Eigen::Quaternionf::FromTwoVectors(Eigen::Vector3f::UnitZ(), lightDir));
        dir->SetLightColor(Eigen::Vector3f{1.0f, 0.98f, 0.92f});
        dir->SetIntensity(3.0f);
        dir->SetCascadeCount(4);
        dir->SetShadowDistance(40.0f);
        dir->SetShadowMapResolution(2048);
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
    }

    Nullable<IStandardMaterialFactory*> _materialFactory{nullptr};
    vector<MeshInstance> _instances{};
};

int main(int argc, char* argv[]) {
    ApplicationRuntimeDescriptor desc{};
    desc.Backend = render::RenderBackend::Vulkan;
    desc.AppName = "RadRay Sphere Demo";
    desc.EngineName = "RadRay";
    desc.WindowTitle = "RadRay Sphere Demo";
    desc.WindowWidth = 1280;
    desc.WindowHeight = 720;
    desc.BackBufferFormat = SphereDemoApp::BackBufferFormat;
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

    SphereDemoApp app{};
    return app.Run(desc);
}
