// gltf_viewer: 最小前向渲染示例。
//
// 两种模式:
//   - 默认 (无 --gltf): 演示 ForwardPipeline (点光源 + Principled BRDF) 渲染多个 UV 球
//     (不透明金属 / alpha-test / 半透明双面), 用 forward.hlsl。
//   - --gltf <path>: 用 LoadGltfAsset 异步加载一个 .gltf/.glb, 走 gltf_standard.hlsl
//     的 metallic-roughness 标准材质 (含贴图 + KHR 扩展), SpawnScene 挂到 World。
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
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/gltf_asset.h>
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
constexpr std::string_view kShadowPassTag = "ShadowCaster";
constexpr uint32_t kSphereSlices = 64;
constexpr float kSphereRadius = 1.0f;

// ── forward.hlsl (多球演示) 的 MaterialConstants (push_constant), 逐字节对应 ──
struct SphereMaterialConstants {
    float BaseColor[4];    // rgb 基础色, a 不透明度 (仅 _ALPHABLEND_ON 时消费)
    float Principled0[4];  // x metallic, y roughness, z specular, w specular tint
    float Principled1[4];  // x anisotropic, y sheen, z sheen tint, w flatness
    float Principled2[4];  // x clearcoat, y clearcoat gloss, z spec trans, w eta
    float AlphaParams[4];  // x alphaCutoff (仅 _ALPHATEST_ON 时消费), yzw 保留
};

// ── gltf_standard.hlsl 的 MaterialConstants (push_constant), 逐字节对应 ──
// 布局: float4 x5, 顺序严格与 shader struct 一致。
struct GltfMaterialConstants {
    float BaseColorFactor[4];  // rgb 基础色, a 不透明度
    float MetalRough[4];       // x metallic, y roughness, z alphaCutoff, w normalScale
    float Emissive[4];         // rgb 自发光 (已乘 strength), w occlusionStrength
    float Principled0[4];      // x specular, y specularTint, z clearcoat, w clearcoatGloss
    float Principled1[4];      // x sheen, y sheenTint, zw 保留
};

// forward.hlsl 的 keyword (顺序即 bit 位, 与 shader 注释一致)。
constexpr std::string_view kKwAlphaTest = "_ALPHATEST_ON";
constexpr std::string_view kKwDoubleSided = "_DOUBLESIDED_ON";
constexpr std::string_view kKwAlphaBlend = "_ALPHABLEND_ON";

// gltf_standard.hlsl 的贴图存在性 keyword (顺序即 bit 位, 与 shader 声明一致, 勿改)。
constexpr std::string_view kKwBaseColorMap = "_BASECOLOR_MAP";
constexpr std::string_view kKwMetalRoughMap = "_METALROUGHNESS_MAP";
constexpr std::string_view kKwNormalMap = "_NORMAL_MAP";
constexpr std::string_view kKwOcclusionMap = "_OCCLUSION_MAP";
constexpr std::string_view kKwEmissiveMap = "_EMISSIVE_MAP";

// gltf_standard.hlsl 的贴图 / 采样器绑定名 (与 SetTexture / SetSampler 名字一致)。
constexpr std::string_view kTexBaseColor = "gBaseColorMap";
constexpr std::string_view kTexMetalRough = "gMetalRoughMap";
constexpr std::string_view kTexNormal = "gNormalMap";
constexpr std::string_view kTexOcclusion = "gOcclusionMap";
constexpr std::string_view kTexEmissive = "gEmissiveMap";
constexpr std::string_view kSamplerName = "gSampler";

// 顶点交错布局: POSITION3f + NORMAL3f + TEXCOORD2f + TANGENT4f (ToSimpleMeshResource / gltf loader 顺序)。
constexpr uint64_t kVertexStride = sizeof(float) * (3 + 3 + 2 + 4);

// 计算 TriangleMesh 的 AABB。
void ComputeBounds(const TriangleMesh& mesh, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax) {
    outMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    outMax = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
    for (const Eigen::Vector3f& p : mesh.Positions) {
        outMin = outMin.cwiseMin(p);
        outMax = outMax.cwiseMax(p);
    }
}

// 自定义 StaticMesh 加载协程 (球演示用): 上传 GPU 数据后补全 section + bounds。
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

    void SetGltfPath(std::filesystem::path path) { _gltfPath = std::move(path); }

    void OnInit() override {
        if (!BuildShaders()) {
            RADRAY_ERR_LOG("gltf_viewer: failed to build shaders");
            return;
        }
        BuildSampler();
        BuildLight();
        if (_gltfPath.empty()) {
            BuildSpheres();
            BuildGround();
            BuildCamera(Eigen::Vector3f::Zero(), 4.0f);
        } else {
            BuildGltfCamera();  // 相机先给默认取景, 待资产就绪后重新 frame。
            RequestGltf();
        }
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        (void)ctx;
        TryAssignSphereMaterials();
        TrySpawnGltf();
    }

    void OnShutdown() override {
        // 采样器现由 RenderSystem 的 SamplerCache 去重持有并统一释放, example 无需手动管理。
    }

private:
    // ─────────────────────────── 球演示实例 ───────────────────────────
    struct SphereInstance {
        StaticMeshComponent* Mesh{nullptr};
        StreamingAssetRef<MaterialAsset> Material{};
        bool Assigned{false};
    };

    // ─────────────────────────── shader / PSO ───────────────────────────

    // 构造单 pass 的 ShaderPassDesc。blend / depthWrite / cull 属 PSO 固定状态,
    // keyword 变体无法控制 (见 shader_asset.h:62), 故按 opaque / transparent 分别显式配置。
    // withTangent=true 时布局带 TANGENT (gltf_standard.hlsl 消费法线贴图)。
    ShaderPassDesc MakePass(const string& source, std::string_view shaderRoot, bool transparent, bool withTangent) {
        ShaderPassDesc pass{};
        pass.PassTag = string{kForwardPassTag};
        pass.Source = source;
        pass.VertexEntry = "VSMain";
        pass.PixelEntry = "PSMain";
        pass.Primitive = render::PrimitiveState::Default();
        pass.MultiSample = render::MultiSampleState::Default();

        render::DepthStencilState ds = render::DepthStencilState::Default();
        ds.Format = render::TextureFormat::D32_FLOAT;
        render::ColorTargetState color = render::ColorTargetState::Default(BackBufferFormat);
        if (transparent) {
            // 透明: alpha blend, 关闭深度写 (复用不透明已写深度做遮挡), 双面可见 (不剔除)。
            ds.DepthWriteEnable = false;
            pass.Primitive.Cull = render::CullMode::None;
            color.Blend = render::BlendState{
                render::BlendComponent{
                    render::BlendFactor::SrcAlpha,
                    render::BlendFactor::OneMinusSrcAlpha,
                    render::BlendOperation::Add},
                render::BlendComponent{
                    render::BlendFactor::One,
                    render::BlendFactor::OneMinusSrcAlpha,
                    render::BlendOperation::Add}};
        }
        pass.DepthStencil = ds;
        pass.ColorTargets.push_back(color);
        pass.IncludeDirs.push_back(string{shaderRoot});

        // 顶点布局: 交错 POSITION/NORMAL/TEXCOORD(/TANGENT), 单 buffer。
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
        if (withTangent) {
            layout.Elements.push_back(render::VertexElement{
                .Offset = sizeof(float) * 8,
                .Semantic = "TANGENT",
                .SemanticIndex = 0,
                .Format = render::VertexFormat::FLOAT32X4,
                .Location = 3});
        }
        pass.VertexLayouts.push_back(std::move(layout));
        return pass;
    }

    // 构造 ShadowCaster pass (depth-only): 用 point_shadow_depth.hlsl, 无 color target,
    // 深度写 + 正常深度测试。顶点布局与 forward pass 一致 (POSITION/NORMAL/TEXCOORD[/TANGENT])。
    ShaderPassDesc MakeShadowCasterPass(const string& source, std::string_view shaderRoot, bool withTangent) {
        ShaderPassDesc pass{};
        pass.PassTag = string{kShadowPassTag};
        pass.Source = source;
        pass.VertexEntry = "VSMain";
        pass.PixelEntry = "PSMain";
        pass.Primitive = render::PrimitiveState::Default();
        pass.MultiSample = render::MultiSampleState::Default();

        render::DepthStencilState ds = render::DepthStencilState::Default();
        ds.Format = render::TextureFormat::D32_FLOAT;
        ds.DepthWriteEnable = true;
        pass.DepthStencil = ds;
        // depth-only: 无 color target。
        pass.IncludeDirs.push_back(string{shaderRoot});

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
        if (withTangent) {
            layout.Elements.push_back(render::VertexElement{
                .Offset = sizeof(float) * 8,
                .Semantic = "TANGENT",
                .SemanticIndex = 0,
                .Format = render::VertexFormat::FLOAT32X4,
                .Location = 3});
        }
        pass.VertexLayouts.push_back(std::move(layout));
        return pass;
    }

    // forward.hlsl 的 keyword 表 (顺序即 bit 位)。
    ShaderKeywordSet MakeSphereKeywordSet() {
        ShaderKeywordSet kw{};
        kw.Add(kKwAlphaTest);
        kw.Add(kKwDoubleSided);
        kw.Add(kKwAlphaBlend);
        return kw;
    }

    // gltf_standard.hlsl 的 keyword 表 (贴图存在性 + alpha/双面)。
    ShaderKeywordSet MakeGltfKeywordSet() {
        ShaderKeywordSet kw{};
        kw.Add(kKwBaseColorMap);
        kw.Add(kKwMetalRoughMap);
        kw.Add(kKwNormalMap);
        kw.Add(kKwOcclusionMap);
        kw.Add(kKwEmissiveMap);
        kw.Add(kKwAlphaTest);
        kw.Add(kKwAlphaBlend);
        kw.Add(kKwDoubleSided);
        return kw;
    }

    // 读一个 shader 源文件 (相对 asset 目录)。
    std::optional<string> ReadShaderSource(std::string_view file) {
        const std::filesystem::path shaderPath =
            std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / file;
        std::optional<string> source = ReadTextFile(shaderPath);
        if (!source.has_value()) {
            RADRAY_ERR_LOG("gltf_viewer: cannot read shader {}", shaderPath.string());
        }
        return source;
    }

    // 建两个 ShaderAsset (opaque / transparent): 只差 PSO 固定状态, 共享同源 + 同 keyword 表。
    // opaque shader 额外带 ShadowCaster pass (depth-only), 让不透明物体投射点光源阴影。
    bool BuildShaderPair(
        const string& source,
        std::string_view shaderRoot,
        const ShaderKeywordSet& keywords,
        bool withTangent,
        StreamingAssetRef<ShaderAsset>& outOpaque,
        StreamingAssetRef<ShaderAsset>& outTransparent) {
        AssetManager* assets = GetAssetManager();

        // shadow caster 深度 shader (所有不透明材质共用同一份源)。
        std::optional<string> shadowSource = ReadShaderSource("point_shadow_depth.hlsl");

        vector<ShaderPassDesc> opaquePasses;
        opaquePasses.push_back(MakePass(source, shaderRoot, /*transparent*/ false, withTangent));
        if (shadowSource.has_value()) {
            opaquePasses.push_back(MakeShadowCasterPass(shadowSource.value(), shaderRoot, withTangent));
        }
        outOpaque = assets->AddReady<ShaderAsset>(
            Guid::NewGuid(),
            make_unique<ShaderAsset>(keywords, std::move(opaquePasses)));

        vector<ShaderPassDesc> transparentPasses;
        transparentPasses.push_back(MakePass(source, shaderRoot, /*transparent*/ true, withTangent));
        outTransparent = assets->AddReady<ShaderAsset>(
            Guid::NewGuid(),
            make_unique<ShaderAsset>(keywords, std::move(transparentPasses)));
        return true;
    }

    bool BuildShaders() {
        RenderSystem* renderSystem = GetRenderSystem();
        if (renderSystem == nullptr) {
            return false;
        }
        const string shaderRoot = renderSystem->GetShaderIncludeRoot();

        if (_gltfPath.empty()) {
            std::optional<string> source = ReadShaderSource("forward.hlsl");
            if (!source.has_value()) {
                return false;
            }
            return BuildShaderPair(
                source.value(), shaderRoot, MakeSphereKeywordSet(),
                /*withTangent*/ false, _opaqueShader, _transparentShader);
        }

        std::optional<string> source = ReadShaderSource("gltf_standard.hlsl");
        if (!source.has_value()) {
            return false;
        }
        return BuildShaderPair(
            source.value(), shaderRoot, MakeGltfKeywordSet(),
            /*withTangent*/ true, _opaqueShader, _transparentShader);
    }

    // 共享 trilinear + repeat 采样器描述 (glTF 默认 wrap)。
    // 只存 descriptor: 实际 sampler 由 SamplerCache 在绑定时去重创建并永生持有,
    // 材质快照据此持稳定指针, 无需 example 手动管理 sampler 生命周期。
    void BuildSampler() {
        _samplerDesc = render::SamplerDescriptor{};
        _samplerDesc.AddressS = render::AddressMode::Repeat;
        _samplerDesc.AddressT = render::AddressMode::Repeat;
        _samplerDesc.AddressR = render::AddressMode::Repeat;
        _samplerDesc.MinFilter = render::FilterMode::Linear;
        _samplerDesc.MagFilter = render::FilterMode::Linear;
        _samplerDesc.MipmapFilter = render::FilterMode::Linear;
        _samplerDesc.LodMin = 0.0f;
        _samplerDesc.LodMax = 32.0f;
        _samplerDesc.Compare = std::nullopt;
        _samplerDesc.AnisotropyClamp = 8;
    }

    // ─────────────────────────── 球演示 ───────────────────────────

    // proxy 由 StaticMeshComponent 异步创建。故每帧尝试, 直到 proxy 出现并绑材质到所有 section。
    // 走组件 SetMaterial: 组件持材质引用, Tick 时生成快照发布给 proxy (无锁跨线程)。
    void TryAssignSphereMaterials() {
        for (SphereInstance& inst : _spheres) {
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

    static SphereMaterialConstants MakeSphereConstants(
        float r, float g, float b, float a,
        float metallic, float roughness, float alphaCutoff = 0.5f) {
        SphereMaterialConstants mc{};
        mc.BaseColor[0] = r;
        mc.BaseColor[1] = g;
        mc.BaseColor[2] = b;
        mc.BaseColor[3] = a;
        mc.Principled0[0] = metallic;
        mc.Principled0[1] = roughness;
        mc.Principled0[2] = 0.5f;  // specular
        mc.Principled0[3] = 0.0f;  // specular tint
        mc.Principled2[1] = 1.0f;  // clearcoat gloss
        mc.Principled2[3] = 1.5f;  // eta
        mc.AlphaParams[0] = alphaCutoff;
        return mc;
    }

    StreamingAssetRef<MaterialAsset> CreateSphereMaterial(
        bool transparent,
        RenderQueue queue,
        const SphereMaterialConstants& mc,
        std::initializer_list<std::string_view> keywords) {
        AssetManager* assets = GetAssetManager();
        StreamingAssetRef<ShaderAsset> shader = transparent ? _transparentShader : _opaqueShader;
        StreamingAssetRef<MaterialAsset> mat = assets->AddReady<MaterialAsset>(
            Guid::NewGuid(),
            make_unique<MaterialAsset>(shader));
        mat->SetRenderQueue(queue);
        for (std::string_view kw : keywords) {
            mat->EnableKeyword(kw);
        }
        mat->SetConstantBlock("gMaterial", &mc, sizeof(mc));
        _materialRefs.push_back(mat);
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
            .Task = LoadSphereMesh(uploads, std::move(meshResource), boundsMin, boundsMax),
            .DebugName = debugName});

        Actor* actor = GetWorld()->SpawnActor<Actor>();
        StaticMeshComponent* mesh = actor->AddComponent<StaticMeshComponent>();
        actor->SetRootComponent(mesh);
        mesh->SetStaticMesh(meshRef);
        mesh->SetWorldLocation(location);
        _spheres.push_back(SphereInstance{mesh, std::move(material), false});
    }

    void BuildSpheres() {
        StreamingAssetRef<MaterialAsset> opaque = CreateSphereMaterial(
            /*transparent*/ false, RenderQueue::Geometry,
            MakeSphereConstants(0.82f, 0.67f, 0.34f, 1.0f, /*metallic*/ 1.0f, /*roughness*/ 0.35f),
            {});
        SpawnSphere(Eigen::Vector3f{-2.4f, 0.0f, 0.0f}, opaque, "OpaqueSphere");

        StreamingAssetRef<MaterialAsset> masked = CreateSphereMaterial(
            /*transparent*/ false, RenderQueue::AlphaTest,
            MakeSphereConstants(0.35f, 0.75f, 0.35f, 1.0f, /*metallic*/ 0.0f, /*roughness*/ 0.6f, /*cutoff*/ 0.5f),
            {kKwAlphaTest});
        SpawnSphere(Eigen::Vector3f{0.0f, 0.0f, 0.0f}, masked, "AlphaTestSphere");

        StreamingAssetRef<MaterialAsset> transparent = CreateSphereMaterial(
            /*transparent*/ true, RenderQueue::Transparent,
            MakeSphereConstants(0.30f, 0.45f, 0.90f, 0.45f, /*metallic*/ 0.0f, /*roughness*/ 0.15f),
            {kKwAlphaBlend, kKwDoubleSided});
        SpawnSphere(Eigen::Vector3f{2.4f, 0.0f, 0.0f}, transparent, "TransparentSphere");
    }

    // ─────────────────────────── glTF 模式 ───────────────────────────

    void RequestGltf() {
        AssetManager* assets = GetAssetManager();
        FrameUploadScheduler& uploads = GetGpuSystem()->GetFrameUploadScheduler();
        _gltfAsset = LoadGltfAsset(*assets, uploads, _gltfPath);
    }

    // 把中性材质描述翻译成 gltf_standard.hlsl 材质。按贴图存在性 + alphaMode + 双面选 keyword/队列。
    StreamingAssetRef<MaterialAsset> TranslateMaterial(const GltfMaterialDesc& desc, std::span<const GltfTextureRef> textures) {
        AssetManager* assets = GetAssetManager();

        const bool transparent = desc.AlphaMode == GltfAlphaMode::Blend;
        StreamingAssetRef<ShaderAsset> shader = transparent ? _transparentShader : _opaqueShader;
        StreamingAssetRef<MaterialAsset> mat = assets->AddReady<MaterialAsset>(
            Guid::NewGuid(),
            make_unique<MaterialAsset>(shader));

        // 渲染队列: blend -> Transparent, mask -> AlphaTest, 否则 Geometry。
        RenderQueue queue = RenderQueue::Geometry;
        if (transparent) {
            queue = RenderQueue::Transparent;
        } else if (desc.AlphaMode == GltfAlphaMode::Mask) {
            queue = RenderQueue::AlphaTest;
        }
        mat->SetRenderQueue(queue);

        // keyword: alpha/双面。
        if (desc.AlphaMode == GltfAlphaMode::Mask) {
            mat->EnableKeyword(kKwAlphaTest);
        }
        if (transparent) {
            mat->EnableKeyword(kKwAlphaBlend);
        }
        if (desc.DoubleSided) {
            mat->EnableKeyword(kKwDoubleSided);
        }

        // keyword + 绑定: 贴图存在性。
        auto bindTexture = [&](int index, std::string_view keyword, std::string_view slot) {
            if (index < 0 || static_cast<size_t>(index) >= textures.size()) {
                return;
            }
            const StreamingAssetRef<TextureAsset>& texRef = textures[static_cast<size_t>(index)].Texture;
            TextureAsset* tex = texRef.Get();
            if (tex == nullptr || tex->GetSrv() == nullptr) {
                return;
            }
            mat->EnableKeyword(keyword);
            // 持资产引用而非裸 SRV: 快照跨帧/跨线程持有安全 (generation 兜底悬垂)。
            mat->SetTexture(slot, texRef);
        };
        bindTexture(desc.BaseColorTexture, kKwBaseColorMap, kTexBaseColor);
        bindTexture(desc.MetallicRoughnessTexture, kKwMetalRoughMap, kTexMetalRough);
        bindTexture(desc.NormalTexture, kKwNormalMap, kTexNormal);
        bindTexture(desc.OcclusionTexture, kKwOcclusionMap, kTexOcclusion);
        bindTexture(desc.EmissiveTexture, kKwEmissiveMap, kTexEmissive);

        mat->SetSampler(kSamplerName, _samplerDesc);

        // 常量块 (逐字段填 GltfMaterialConstants)。
        GltfMaterialConstants mc{};
        mc.BaseColorFactor[0] = desc.BaseColorFactor.x();
        mc.BaseColorFactor[1] = desc.BaseColorFactor.y();
        mc.BaseColorFactor[2] = desc.BaseColorFactor.z();
        mc.BaseColorFactor[3] = desc.BaseColorFactor.w();
        mc.MetalRough[0] = desc.MetallicFactor;
        mc.MetalRough[1] = desc.RoughnessFactor;
        mc.MetalRough[2] = desc.AlphaCutoff;
        mc.MetalRough[3] = desc.NormalScale;
        // 自发光已乘 KHR_materials_emissive_strength。
        mc.Emissive[0] = desc.EmissiveFactor.x() * desc.EmissiveStrength;
        mc.Emissive[1] = desc.EmissiveFactor.y() * desc.EmissiveStrength;
        mc.Emissive[2] = desc.EmissiveFactor.z() * desc.EmissiveStrength;
        mc.Emissive[3] = desc.OcclusionStrength;
        mc.Principled0[0] = desc.Specular;
        mc.Principled0[1] = desc.SpecularTint;
        mc.Principled0[2] = desc.Clearcoat;
        mc.Principled0[3] = desc.ClearcoatGloss;
        mc.Principled1[0] = desc.Sheen;
        mc.Principled1[1] = desc.SheenTint;
        mat->SetConstantBlock("gMaterial", &mc, sizeof(mc));

        _materialRefs.push_back(mat);
        return mat;
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

        GltfMaterialFactory factory =
            [this](const GltfMaterialDesc& desc, std::span<const GltfTextureRef> textures) -> StreamingAssetRef<MaterialAsset> {
            return TranslateMaterial(desc, textures);
        };
        asset->SpawnScene(*GetWorld(), factory);
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

    // ─────────────────────────── 场景通用 ───────────────────────────

    void BuildLight() {
        Actor* actor = GetWorld()->SpawnActor<Actor>();
        PointLightComponent* light = actor->AddComponent<PointLightComponent>();
        actor->SetRootComponent(light);
        light->SetWorldLocation(Eigen::Vector3f{3.0f, 4.0f, 3.0f});
        light->SetLightColor(Eigen::Vector3f{1.0f, 0.95f, 0.9f});
        light->SetIntensity(60.0f);
        light->SetAttenuationRadius(50.0f);
    }

    // 地面: 一块压扁的大 cube, 不透明漫反射, 接收 (并投射) 点光源阴影。
    void BuildGround() {
        TriangleMesh cube;
        cube.InitAsCube(1.0f);
        // 压扁 + 放大成地板: XZ 铺开, Y 方向很薄。
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
            .Task = LoadSphereMesh(uploads, std::move(meshResource), boundsMin, boundsMax),
            .DebugName = "Ground"});

        StreamingAssetRef<MaterialAsset> mat = CreateSphereMaterial(
            /*transparent*/ false, RenderQueue::Geometry,
            MakeSphereConstants(0.5f, 0.5f, 0.55f, 1.0f, /*metallic*/ 0.0f, /*roughness*/ 0.8f),
            {});

        Actor* actor = GetWorld()->SpawnActor<Actor>();
        StaticMeshComponent* mesh = actor->AddComponent<StaticMeshComponent>();
        actor->SetRootComponent(mesh);
        mesh->SetStaticMesh(meshRef);
        mesh->SetWorldLocation(Eigen::Vector3f{0.0f, -1.6f, 0.0f});
        _spheres.push_back(SphereInstance{mesh, std::move(mat), false});
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

    void BuildGltfCamera() {
        // glTF 就绪前给个中性取景, 待 TrySpawnGltf 用真实包围盒 SetFrame。
        BuildCamera(Eigen::Vector3f::Zero(), 3.0f);
    }

    // ─────────────────────────── 状态 ───────────────────────────
    StreamingAssetRef<ShaderAsset> _opaqueShader{};
    StreamingAssetRef<ShaderAsset> _transparentShader{};
    vector<StreamingAssetRef<MaterialAsset>> _materialRefs{};  // 保活材质资产
    render::SamplerDescriptor _samplerDesc{};                  // 共享采样器描述 (实际 sampler 由 SamplerCache 去重永生)
    vector<SphereInstance> _spheres{};

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
    desc.PresentMode = render::PresentMode::FIFO;

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
