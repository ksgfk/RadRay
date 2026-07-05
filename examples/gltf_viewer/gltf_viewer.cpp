// gltf_viewer: 最小前向渲染示例。
//
// 演示 ForwardPipeline (点光源 + Principled BRDF) 渲染一个 UV 球:
//   - 用 TriangleMesh 生成 UV 球, 异步上传为 StaticMesh (含 section + bounds)。
//   - 一个 StaticMeshComponent + PBR 材质 (forward.hlsl)。
//   - 一个 PointLightComponent 提供照明。
//   - 一个相机 + CameraControlComponent 支持轨道操作。
//
// shader 编译设施 (Dxc / ShaderVariantCache / GraphicsPipelineStateCache) 与 shaderlib
// include 根目录由 RenderSystem 持有; 材质 pass 通过 IncludeDirs 传入该根目录以解析 #include。

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
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/camera_control_component.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>

#include <radray/render/common.h>
#include <radray/basic_math.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/file.h>
#include <radray/logger.h>

#include <cstring>
#include <filesystem>
#include <string_view>

#ifndef RADRAY_GLTF_VIEWER_ASSET_DIR
#define RADRAY_GLTF_VIEWER_ASSET_DIR "."
#endif

using namespace radray;

namespace {

constexpr std::string_view kForwardPassTag = "UniversalForward";
constexpr uint32_t kSphereSlices = 64;
constexpr float kSphereRadius = 1.0f;

// 与 forward.hlsl 的 MaterialConstants (push_constant) 逐字节对应。列主序无关 (全是 float4)。
struct MaterialConstants {
    float BaseColor[4];    // rgb 基础色
    float Principled0[4];  // x metallic, y roughness, z specular, w specular tint
    float Principled1[4];  // x anisotropic, y sheen, z sheen tint, w flatness
    float Principled2[4];  // x clearcoat, y clearcoat gloss, z spec trans, w eta
};

// UV 球顶点交错布局: POSITION3f + NORMAL3f + TEXCOORD2f + TANGENT4f (ToSimpleMeshResource 顺序)。
// 前向 shader 只消费前三个属性, TANGENT 仍在缓冲里 (stride 需完整计入)。
constexpr uint64_t kVertexStride = sizeof(float) * (3 + 3 + 2 + 4);

// 计算 AABB。
void ComputeBounds(const TriangleMesh& mesh, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax) {
    outMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    outMax = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
    for (const Eigen::Vector3f& p : mesh.Positions) {
        outMin = outMin.cwiseMin(p);
        outMax = outMax.cwiseMax(p);
    }
}

// 自定义 StaticMesh 加载协程: 上传 GPU 数据后补全 section (覆盖整个 mesh) + bounds。
// LoadStaticMesh 只上传几何, 不设置 section; 无 section 的 mesh 不会产生任何 draw。
AssetLoadTask LoadSphereMesh(FrameUploadScheduler& frameUploads, MeshResource meshResource, Eigen::Vector3f boundsMin, Eigen::Vector3f boundsMax) {
    if (meshResource.Primitives.empty()) {
        co_return AssetLoadResult::Failure("sphere mesh resource has no primitive");
    }
    const uint32_t indexCount = meshResource.Primitives[0].IndexBuffer.IndexCount;
    const uint32_t vertexCount = meshResource.Primitives[0].VertexCount;

    FrameUploadScope frame = co_await frameUploads.BeginUpload();
    std::optional<render::RenderMesh> renderMesh =
        frame.GetUploader().UploadMeshResource(frame.GetCommandBuffer(), meshResource);
    if (!renderMesh.has_value()) {
        co_return AssetLoadResult::Failure("sphere mesh upload recording failed");
    }
    co_await frame.WaitGpu();

    auto mesh = make_unique<StaticMesh>(std::move(meshResource), std::move(renderMesh.value()));
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

class GltfViewerApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;

    void OnInit() override {
        if (!BuildShaderAndMaterial()) {
            RADRAY_ERR_LOG("gltf_viewer: failed to build shader/material");
            return;
        }
        BuildSphere();
        BuildLight();
        BuildCamera();
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        (void)ctx;
        TryAssignMaterial();
    }

private:
    // proxy 由 StaticMeshComponent 在 mesh 异步加载完成后才创建, 无组件级材质槽,
    // 故每帧尝试, 直到 proxy 出现并把材质绑到 section 0。
    void TryAssignMaterial() {
        if (_materialAssigned || _meshComponent == nullptr || _material.Get() == nullptr) {
            return;
        }
        PrimitiveSceneProxy* proxy = _meshComponent->GetSceneProxy();
        if (proxy == nullptr) {
            return;
        }
        auto* smProxy = static_cast<StaticMeshSceneProxy*>(proxy);
        if (smProxy->GetSectionCount() == 0) {
            return;
        }
        for (uint32_t s = 0; s < smProxy->GetSectionCount(); ++s) {
            smProxy->SetSectionMaterial(s, _material.Get());
        }
        _materialAssigned = true;
        RADRAY_INFO_LOG("gltf_viewer: material assigned to {} section(s)", smProxy->GetSectionCount());
    }

    bool BuildShaderAndMaterial() {
        RenderSystem* renderSystem = GetRenderSystem();
        if (renderSystem == nullptr) {
            return false;
        }

        // forward.hlsl 由 radray_example_files 部署到 assets/gltf_viewer (见 CMakeLists)。
        const std::filesystem::path shaderPath =
            std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / "forward.hlsl";
        std::optional<string> source = ReadTextFile(shaderPath);
        if (!source.has_value()) {
            RADRAY_ERR_LOG("gltf_viewer: cannot read shader {}", shaderPath.string());
            return false;
        }

        ShaderPassDesc pass{};
        pass.PassTag = string{kForwardPassTag};
        pass.Source = std::move(source.value());
        pass.VertexEntry = "VSMain";
        pass.PixelEntry = "PSMain";
        pass.Primitive = render::PrimitiveState::Default();
        pass.DepthStencil = render::DepthStencilState::Default();
        pass.DepthStencil->Format = render::TextureFormat::D32_FLOAT;
        pass.MultiSample = render::MultiSampleState::Default();
        pass.ColorTargets.push_back(render::ColorTargetState::Default(BackBufferFormat));
        // shaderlib include 根目录 (解析 #include "common.hlsl" 等)。
        pass.IncludeDirs.push_back(renderSystem->GetShaderIncludeRoot());

        // 顶点布局: 交错 POSITION/NORMAL/TEXCOORD/TANGENT, 单 buffer。
        OwningVertexBufferLayout layout{};
        layout.ArrayStride = kVertexStride;
        layout.StepMode = render::VertexStepMode::Vertex;
        layout.Elements.push_back(render::VertexElement{
            .Offset = 0,
            .Semantic = "POSITION",
            .SemanticIndex = 0,
            .Format = render::VertexFormat::FLOAT32X3,
            .Location = 0});
        layout.Elements.push_back(render::VertexElement{
            .Offset = sizeof(float) * 3,
            .Semantic = "NORMAL",
            .SemanticIndex = 0,
            .Format = render::VertexFormat::FLOAT32X3,
            .Location = 1});
        layout.Elements.push_back(render::VertexElement{
            .Offset = sizeof(float) * 6,
            .Semantic = "TEXCOORD",
            .SemanticIndex = 0,
            .Format = render::VertexFormat::FLOAT32X2,
            .Location = 2});
        pass.VertexLayouts.push_back(std::move(layout));

        vector<ShaderPassDesc> passes;
        passes.push_back(std::move(pass));

        AssetManager* assets = GetAssetManager();
        _shader = assets->AddReady<ShaderAsset>(
            Guid::NewGuid(),
            make_unique<ShaderAsset>(ShaderKeywordSet{}, std::move(passes)));

        _material = assets->AddReady<MaterialAsset>(
            Guid::NewGuid(),
            make_unique<MaterialAsset>(_shader));
        _material->SetRenderQueue(RenderQueue::Geometry);

        MaterialConstants mc{};
        mc.BaseColor[0] = 0.82f;
        mc.BaseColor[1] = 0.67f;
        mc.BaseColor[2] = 0.34f;  // 金色调
        mc.BaseColor[3] = 1.0f;
        mc.Principled0[0] = 1.0f;    // metallic
        mc.Principled0[1] = 0.35f;   // roughness
        mc.Principled0[2] = 0.5f;    // specular
        mc.Principled0[3] = 0.0f;    // specular tint
        mc.Principled1[0] = 0.0f;    // anisotropic
        mc.Principled1[1] = 0.0f;    // sheen
        mc.Principled1[2] = 0.0f;    // sheen tint
        mc.Principled1[3] = 0.0f;    // flatness
        mc.Principled2[0] = 0.0f;    // clearcoat
        mc.Principled2[1] = 1.0f;    // clearcoat gloss
        mc.Principled2[2] = 0.0f;    // spec trans
        mc.Principled2[3] = 1.5f;    // eta
        _material->SetConstantBlock("gMaterial", &mc, sizeof(mc));
        return true;
    }

    void BuildSphere() {
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
            .Task = LoadSphereMesh(uploads, std::move(meshResource), boundsMin, boundsMax),
            .DebugName = "UVSphere"});

        Actor* actor = GetWorld()->SpawnActor<Actor>();
        _meshComponent = actor->AddComponent<StaticMeshComponent>();
        actor->SetRootComponent(_meshComponent);
        _meshComponent->SetStaticMesh(meshRef);
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

    void BuildCamera() {
        Actor* actor = GetWorld()->SpawnActor<Actor>();
        CameraComponent* camera = actor->AddComponent<CameraComponent>();
        actor->SetRootComponent(camera);
        camera->SetPerspective(Radian(50.0f), 0.05f, 500.0f);
        camera->SetWorldLocation(Eigen::Vector3f{0.0f, 0.0f, 4.0f});

        CameraControlComponent* control = actor->AddComponent<CameraControlComponent>();
        control->SetCamera(camera);
        control->SetFrame(Eigen::Vector3f::Zero(), 4.0f, 0.0f, 0.0f);
        control->BindToMainWindow(*this);
    }

    StreamingAssetRef<ShaderAsset> _shader{};
    StreamingAssetRef<MaterialAsset> _material{};
    StaticMeshComponent* _meshComponent{nullptr};
    bool _materialAssigned{false};
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

    GltfViewerApp app{};
    return app.Run(desc);
}
