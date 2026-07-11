#include <radray/runtime/render_framework/forward_pipeline.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <radray/logger.h>
#include <radray/runtime/sampler_cache.h>
#include <radray/runtime/application.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_framework/material_render_snapshot.h>
#include <radray/runtime/render_framework/forward_pipeline_shader.h>
#include <radray/runtime/render_framework/standard_material_factory.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/directional_light_scene_proxy.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/render_framework/point_light_scene_proxy.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_queue.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/texture_asset.h>

namespace radray {

namespace {

constexpr std::string_view kForwardPassTag = "UniversalForward";
constexpr std::string_view kShadowPassTag = "ShadowCaster";
constexpr std::string_view kPerObjectName = "gPerObject";
constexpr std::string_view kViewName = "gView";
constexpr std::string_view kShadowViewName = "gShadowView";
constexpr std::string_view kShadowCubeName = "gShadowCube";
constexpr std::string_view kShadowArrayName = "gShadowArray";
constexpr std::string_view kShadowSamplerName = "gShadowSampler";

ImageData MakeSolidRgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    ImageData image{};
    image.Width = 1;
    image.Height = 1;
    image.Format = ImageFormat::RGBA8_BYTE;
    image.Data = make_unique<byte[]>(4);
    image.Data[0] = static_cast<byte>(r);
    image.Data[1] = static_cast<byte>(g);
    image.Data[2] = static_cast<byte>(b);
    image.Data[3] = static_cast<byte>(a);
    return image;
}

// cube 6 面的 forward / up 方向, 面序严格匹配 point_shadow.hlsl 的 point_shadow_cube_face:
//   0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z。
// up 向量只影响面内朝向, 不影响 compareDepth (只依赖 forward), 取标准 cube 约定。
// (Eigen 向量非 constexpr 可构造, 用普通 float 数组存储。)
constexpr float kCubeForward[6][3] = {
    {1.0f, 0.0f, 0.0f},   // +X
    {-1.0f, 0.0f, 0.0f},  // -X
    {0.0f, 1.0f, 0.0f},   // +Y
    {0.0f, -1.0f, 0.0f},  // -Y
    {0.0f, 0.0f, 1.0f},   // +Z
    {0.0f, 0.0f, -1.0f},  // -Z
};
constexpr float kCubeUp[6][3] = {
    {0.0f, 1.0f, 0.0f},   // +X
    {0.0f, 1.0f, 0.0f},   // -X
    {0.0f, 0.0f, -1.0f},  // +Y (forward 与常规 up 平行, 换一个)
    {0.0f, 0.0f, 1.0f},   // -Y
    {0.0f, 1.0f, 0.0f},   // +Z
    {0.0f, 1.0f, 0.0f},   // -Z
};

// 生成一盏点光源某个 cube 面的 世界->裁剪 矩阵。90 度 FOV, 1:1 宽高比, near..radius。
Eigen::Matrix4f MakeCubeFaceViewProj(const Eigen::Vector3f& lightPos, uint32_t face, float radius) {
    const float nearZ = 0.05f;
    const float farZ = std::max(radius, nearZ + 0.01f);
    Eigen::Matrix4f proj = PerspectiveLH<float>(Radian(90.0f), 1.0f, nearZ, farZ);
    Eigen::Vector3f forward{kCubeForward[face][0], kCubeForward[face][1], kCubeForward[face][2]};
    Eigen::Vector3f up{kCubeUp[face][0], kCubeUp[face][1], kCubeUp[face][2]};
    Eigen::Matrix4f view = LookAtFrontLH<float>(lightPos, forward, up);
    return proj * view;
}

// ── 方向光级联阴影 (CSM) 计算 ──────────────────────────────────────────────
//
// 参考 UE5 UDirectionalLightComponent 的 practical split scheme + 稳定化 texel snapping,
// 但采用引擎的左手 + 标准深度约定 (OrthoLH: z in [near,far] -> [0,1])。

// 单个级联的划分结果。
struct CascadeSplit {
    float NearZ{0.0f};
    float FarZ{0.0f};
};

// practical split scheme (Zhang et al.): 对数分布与均匀分布按 lambda 混合。
//   uniform_i  = near + (far-near) * i/N
//   log_i      = near * (far/near)^(i/N)
//   split_i    = lerp(uniform_i, log_i, lambda)
// lambda=0 纯均匀, lambda=1 纯对数。返回每级联的 [near,far] (视图空间距离)。
void ComputeCascadeSplits(
    float nearZ,
    float shadowFar,
    uint32_t cascadeCount,
    float lambda,
    CascadeSplit* outSplits) {
    const float clipRange = shadowFar - nearZ;
    const float minZ = nearZ;
    const float maxZ = shadowFar;
    const float ratio = maxZ / std::max(minZ, 1e-4f);
    float lastSplit = nearZ;
    for (uint32_t i = 0; i < cascadeCount; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(cascadeCount);
        const float logSplit = minZ * std::pow(ratio, p);
        const float uniformSplit = minZ + clipRange * p;
        const float splitDist = Lerp(uniformSplit, logSplit, lambda);
        outSplits[i].NearZ = lastSplit;
        outSplits[i].FarZ = splitDist;
        lastSplit = splitDist;
    }
}

// 手动划分 (对齐 Unity URP): ratios 为 [0,1] 的累积归一化边界 (相对 [nearZ,shadowFar] 区间)。
// 对 N 个级联使用前 N-1 个比例, 最后一个级联的远边界固定为 shadowFar。
//   split_i.far = near + (far-near) * ratio_i     (i < N-1)
//   split_(N-1).far = far
// ratios 已在组件侧 clamp 到 [0,1] 且单调不减; 此处再夹一次保证有序稳健。
void ComputeCascadeSplitsManual(
    float nearZ,
    float shadowFar,
    uint32_t cascadeCount,
    const std::array<float, 3>& ratios,
    CascadeSplit* outSplits) {
    const float clipRange = shadowFar - nearZ;
    float lastSplit = nearZ;
    float prevRatio = 0.0f;
    for (uint32_t i = 0; i < cascadeCount; ++i) {
        float splitDist;
        if (i + 1 >= cascadeCount) {
            splitDist = shadowFar;
        } else {
            const float r = std::clamp(ratios[i], prevRatio, 1.0f);
            prevRatio = r;
            splitDist = nearZ + clipRange * r;
        }
        outSplits[i].NearZ = lastSplit;
        outSplits[i].FarZ = splitDist;
        lastSplit = splitDist;
    }
}

// 用相机基与视场角, 拟合 [splitNear,splitFar] 视锥切片的最小包围球 (球心沿视轴)。
// 解析法 (MJP stable CSM): 令球心在视轴距相机 x 处, 使近/远角点等距。
//   a2 = tanHalfH^2 + tanHalfV^2
//   x  = (1+a2)(f+n)/2, clamp 到 [n,f]
//   r  = 距远角点距离
void ComputeCascadeSphere(
    const Eigen::Vector3f& eye,
    const Eigen::Vector3f& forward,
    float tanHalfV,
    float tanHalfH,
    float splitNear,
    float splitFar,
    Eigen::Vector3f& outCenter,
    float& outRadius) {
    const float a2 = tanHalfH * tanHalfH + tanHalfV * tanHalfV;
    float x = (1.0f + a2) * (splitFar + splitNear) * 0.5f;
    x = Clamp(x, splitNear, splitFar);
    const float dFar = splitFar - x;
    const float radius = std::sqrt(dFar * dFar + splitFar * splitFar * a2);
    outCenter = eye + forward * x;
    outRadius = radius;
}

// 为一个级联 (包围球 center/radius, 光照方向 lightDir) 构造 世界->阴影裁剪 矩阵。
// 稳定化: 把球心在光空间的 xy 吸附到 texel 网格 (消除相机移动时的阴影抖动)。
// zExtend: 沿光照方向的额外深度, 用于捕获级联球之外 (上游) 的投影者。
Eigen::Matrix4f MakeCascadeViewProj(
    const Eigen::Vector3f& center,
    float radius,
    const Eigen::Vector3f& lightDir,
    float shadowMapSize,
    float zExtend) {
    Eigen::Vector3f L = lightDir;
    if (L.squaredNorm() <= 1e-8f) {
        L = Eigen::Vector3f::UnitZ();
    }
    L.normalize();

    // 选一个与光照方向不平行的 up。
    Eigen::Vector3f up = std::abs(L.y()) > 0.99f ? Eigen::Vector3f::UnitX() : Eigen::Vector3f::UnitY();

    // 仅旋转的光空间变换 (球心置于原点), 用于把球心投影到 texel 网格并吸附。
    Eigen::Matrix4f rotView = LookAtFrontLH<float>(Eigen::Vector3f::Zero(), L, up);
    Eigen::Vector3f centerLS = (rotView * center.homogeneous()).head<3>();

    const float worldUnitsPerTexel = (2.0f * radius) / std::max(shadowMapSize, 1.0f);
    centerLS.x() = std::floor(centerLS.x() / worldUnitsPerTexel) * worldUnitsPerTexel;
    centerLS.y() = std::floor(centerLS.y() / worldUnitsPerTexel) * worldUnitsPerTexel;

    // 吸附后的球心 (世界空间)。rotView 上-左 3x3 为正交阵, 逆 = 转置。
    Eigen::Matrix3f rot = rotView.block<3, 3>(0, 0);
    Eigen::Vector3f snappedCenter = rot.transpose() * centerLS;

    // 光相机置于球后 (沿 -L), 使 near 平面能容纳上游投影者。
    const float backExtrude = std::max(zExtend, radius);
    Eigen::Vector3f lightEye = snappedCenter - L * (radius + backExtrude);
    Eigen::Matrix4f view = LookAtFrontLH<float>(lightEye, L, up);

    // 正交投影: xy 覆盖 [-r,r], z 覆盖 [0, 2r + backExtrude] (标准深度)。
    const float zFar = 2.0f * radius + backExtrude;
    Eigen::Matrix4f proj = OrthoLH<float>(-radius, radius, -radius, radius, 0.0f, zFar);
    return proj * view;
}

/// forward 管线的标准材质工厂: 持有 forward_pass shader 对 + 共享采样器,
/// 把中性 StandardMaterialDescription 翻译成 forward_pass 材质。
///
/// 由 ForwardPipeline 持有并经 RenderPipeline::GetStandardMaterialFactory() 暴露 (以接口形式),
/// 具体类型隐藏于本 .cpp; 生命周期 == 管线, 故无"须存活到 SpawnScene 结束"的裸捕获约束。
class ForwardStandardMaterialFactory final : public IStandardMaterialFactory {
public:
    ForwardStandardMaterialFactory() noexcept = default;

    /// 构建 forward_pass 的 opaque/transparent shader 对 (含 ShadowCaster) + 采样器。成功返回 true。
    bool Initialize(AssetManager& assets, RenderSystem& renderSystem, render::TextureFormat colorFormat) {
        _assets = &assets;

        std::optional<StreamingAssetRef<ShaderAsset>> shader = BuildForwardShader(
            assets,
            renderSystem,
            MakeForwardKeywordSet(),
            colorFormat,
            /*withShadowCaster*/ true);
        if (!shader.has_value()) {
            return false;
        }
        _shader = std::move(shader.value());

        // 共享 trilinear + repeat 采样器描述 (glTF 默认 wrap)。
        // 只存 descriptor: 实际 sampler 由 SamplerCache 在绑定时去重创建并永生持有。
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

        Application* app = renderSystem.GetApplication();
        GpuSystem* gpu = app != nullptr ? app->GetGpuSystem() : nullptr;
        if (gpu == nullptr) {
            return false;
        }
        auto loadDefault = [&](std::string_view name, ImageData image, bool srgb) {
            return LoadTextureAssetFromImage(
                assets,
                gpu->GetFrameUploadScheduler(),
                Guid::NewGuid(),
                string{name},
                std::move(image),
                TextureAssetLoadOptions{.Srgb = srgb});
        };
        _whiteSrgb = loadDefault("forward_default_white_srgb", MakeSolidRgba8(255, 255, 255), true);
        _whiteLinear = loadDefault("forward_default_white", MakeSolidRgba8(255, 255, 255), false);
        _blackLinear = loadDefault("forward_default_black", MakeSolidRgba8(0, 0, 0), false);
        _flatNormal = loadDefault("forward_default_flat_normal", MakeSolidRgba8(128, 128, 255), false);
        return true;
    }

    bool IsValid() const noexcept { return _shader.IsValid(); }

    StreamingAssetRef<MaterialAsset> CreateMaterial(
        const StandardMaterialDescription& desc,
        std::span<const StandardMaterialTexture> textures) override {
        if (_assets == nullptr || !_shader.IsValid()) {
            return {};
        }

        const bool transparent = desc.AlphaMode == StandardAlphaMode::Blend;
        StreamingAssetRef<MaterialAsset> mat = _assets->AddReady<MaterialAsset>(
            Guid::NewGuid(),
            make_unique<MaterialAsset>(_shader));

        // 渲染队列: blend -> Transparent, mask -> AlphaTest, 否则 Geometry。
        RenderQueue queue = RenderQueue::Geometry;
        if (transparent) {
            queue = RenderQueue::Transparent;
        } else if (desc.AlphaMode == StandardAlphaMode::Mask) {
            queue = RenderQueue::AlphaTest;
        }
        mat->SetRenderQueue(queue);

        // PSO 固定功能状态 (blend / zwrite / cull): 透明走 alpha blend + 关深度写 + 双面;
        // 不透明/mask 沿用 shader pass 基线。双面材质额外覆盖 cull=None。
        if (transparent) {
            mat->SetRenderState(MakeForwardTransparentRenderState());
        } else if (desc.DoubleSided) {
            MaterialRenderState rs{};
            rs.Cull = render::CullMode::None;
            mat->SetRenderState(rs);
        }

        // keyword: alpha/双面。
        if (desc.AlphaMode == StandardAlphaMode::Mask) {
            mat->EnableKeyword(forward_pipeline::kKwAlphaTest);
        }
        if (transparent) {
            mat->EnableKeyword(forward_pipeline::kKwAlphaBlend);
        }
        if (desc.DoubleSided) {
            mat->EnableKeyword(forward_pipeline::kKwDoubleSided);
        }

        // keyword + 绑定: 贴图存在性。
        auto bindTexture = [&] (
                               int index,
                               std::string_view keyword,
                               std::string_view slot,
                               const StreamingAssetRef<TextureAsset>& fallback) {
            StreamingAssetRef<TextureAsset> selected = fallback;
            if (index >= 0 && static_cast<size_t>(index) < textures.size()) {
                const StreamingAssetRef<TextureAsset>& texRef = textures[static_cast<size_t>(index)].Texture;
                TextureAsset* tex = texRef.Get();
                if (tex != nullptr && tex->GetSrv() != nullptr) {
                    selected = texRef;
                    mat->EnableKeyword(keyword);
                }
            }
            mat->SetTexture(slot, std::move(selected));
        };
        bindTexture(desc.BaseColorTexture, forward_pipeline::kKwBaseColorMap, forward_pipeline::kTexBaseColor, _whiteSrgb);
        bindTexture(desc.MetallicRoughnessTexture, forward_pipeline::kKwMetalRoughMap, forward_pipeline::kTexMetalRough, _whiteLinear);
        bindTexture(desc.NormalTexture, forward_pipeline::kKwNormalMap, forward_pipeline::kTexNormal, _flatNormal);
        bindTexture(desc.OcclusionTexture, forward_pipeline::kKwOcclusionMap, forward_pipeline::kTexOcclusion, _whiteLinear);
        bindTexture(desc.EmissiveTexture, forward_pipeline::kKwEmissiveMap, forward_pipeline::kTexEmissive, _blackLinear);

        mat->SetSampler(forward_pipeline::kSamplerName, _samplerDesc);

        // 常量块 (逐字段填 ForwardMaterialConstants)。
        ForwardMaterialConstants mc{};
        mc.BaseColor[0] = desc.BaseColorFactor.x();
        mc.BaseColor[1] = desc.BaseColorFactor.y();
        mc.BaseColor[2] = desc.BaseColorFactor.z();
        mc.BaseColor[3] = desc.BaseColorFactor.w();
        mc.Pbr[0] = desc.MetallicFactor;
        mc.Pbr[1] = desc.RoughnessFactor;
        mc.Pbr[2] = desc.AlphaCutoff;
        mc.Pbr[3] = desc.NormalScale;
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
        // Principled1.zw (anisotropic/flatness) 与 Principled2 (specTrans/eta) 走默认: glTF 无对应字段。
        mc.Principled2[1] = 1.5f;  // eta 默认
        mat->SetConstantBlock("gMaterial", &mc, sizeof(mc));

        return mat;
    }

    StreamingAssetRef<MaterialAsset> GetDefaultMaterial() override {
        if (!_defaultMaterial.IsValid()) {
            _defaultMaterial = CreateMaterial(StandardMaterialDescription{}, {});
        }
        return _defaultMaterial;
    }

private:
    AssetManager* _assets{nullptr};
    StreamingAssetRef<ShaderAsset> _shader{};
    render::SamplerDescriptor _samplerDesc{};
    StreamingAssetRef<TextureAsset> _whiteSrgb{};
    StreamingAssetRef<TextureAsset> _whiteLinear{};
    StreamingAssetRef<TextureAsset> _blackLinear{};
    StreamingAssetRef<TextureAsset> _flatNormal{};
    StreamingAssetRef<MaterialAsset> _defaultMaterial{};
};

}  // namespace

ForwardPipeline::ShadowCasterPass::ShadowCasterPass(ForwardPipeline* owner) noexcept
    : RenderPipelinePass(RenderPassEvent::BeforeRenderingShadows), _owner(owner) {}

ForwardPipeline::DirectionalShadowCasterPass::DirectionalShadowCasterPass(ForwardPipeline* owner) noexcept
    : RenderPipelinePass(RenderPassEvent::BeforeRenderingShadows), _owner(owner) {}

ForwardPipeline::ForwardColorPass::ForwardColorPass(ForwardPipeline* owner) noexcept
    : RenderPipelinePass(RenderPassEvent::BeforeRenderingOpaques), _owner(owner) {}

ForwardPipeline::ForwardPipeline(RenderSystem* renderSystem) noexcept
    : _device(renderSystem != nullptr && renderSystem->GetApplication() != nullptr
                  ? renderSystem->GetApplication()->GetDevice()
                  : nullptr),
      _shadowPass(this),
      _dirShadowPass(this),
      _colorPass(this),
      _renderSystem(renderSystem) {
    if (_device != nullptr && renderSystem != nullptr) {
        Application* app = renderSystem->GetApplication();
        GpuSystem* gpu = app != nullptr ? app->GetGpuSystem() : nullptr;
        _renderPassRegistry = gpu != nullptr ? gpu->GetRenderPassRegistry() : nullptr;
        _samplerCache = renderSystem->GetSamplerCache();
        _executor = make_unique<MeshPassExecutor>(
            _device,
            renderSystem->GetShaderVariantLibrary(),
            renderSystem->GetGraphicsPipelineStateLibrary(),
            renderSystem->GetSamplerCache(),
            std::string{kPerObjectName});
        _shadowExecutor = make_unique<MeshPassExecutor>(
            _device,
            renderSystem->GetShaderVariantLibrary(),
            renderSystem->GetGraphicsPipelineStateLibrary(),
            renderSystem->GetSamplerCache(),
            std::string{kPerObjectName});
    }
}

ForwardPipeline::~ForwardPipeline() noexcept {
    if (_renderPassRegistry == nullptr) {
        return;
    }
    for (const DepthTarget& target : _depthTargets) {
        _renderPassRegistry->RemoveFramebuffersUsing(target.View.get());
    }
    for (const ShadowCube& cube : _shadowCubes) {
        _renderPassRegistry->RemoveFramebuffersUsing(cube.LayeredDsv.get());
        for (const auto& face : cube.FaceDsv) {
            _renderPassRegistry->RemoveFramebuffersUsing(face.get());
        }
    }
    for (const ShadowArray& array : _shadowArrays) {
        for (const auto& slice : array.SliceDsv) {
            _renderPassRegistry->RemoveFramebuffersUsing(slice.get());
        }
    }
}

void ForwardPipeline::OnBeginFrame(RenderPipelineContext& ctx) {
    RADRAY_UNUSED(ctx);
}

Nullable<IStandardMaterialFactory*> ForwardPipeline::GetStandardMaterialFactory() noexcept {
    if (!_materialFactoryInit) {
        _materialFactoryInit = true;  // 只尝试一次 (成功与否)。
        Application* app = _renderSystem != nullptr ? _renderSystem->GetApplication() : nullptr;
        AssetManager* assets = app != nullptr ? app->GetAssetManager() : nullptr;
        WindowManager* windows = app != nullptr ? app->GetWindowManager() : nullptr;
        if (assets != nullptr && windows != nullptr) {
            const render::TextureFormat colorFormat = windows->GetMainBackBufferFormat();
            auto factory = make_unique<ForwardStandardMaterialFactory>();
            if (factory->Initialize(*assets, *_renderSystem, colorFormat)) {
                _materialFactory = std::move(factory);
            } else {
                RADRAY_ERR_LOG("ForwardPipeline: failed to build standard material factory");
            }
        }
    }
    if (_materialFactory == nullptr) {
        return nullptr;
    }
    return _materialFactory.get();
}

void ForwardPipeline::OnBuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras) {
    cameras.Clear();
    if (ctx.App == nullptr) {
        return;
    }
    World* world = ctx.App->GetWorld();
    if (world == nullptr) {
        return;
    }
    Scene* scene = world->GetScene();
    if (scene == nullptr) {
        return;
    }

    // 最小实现: 取第一个相机, 渲染进第一个 target (主窗口)。
    CameraComponent* camera = nullptr;
    for (const unique_ptr<Actor>& actor : world->GetActors()) {
        if (actor == nullptr) {
            continue;
        }
        if (CameraComponent* found = actor->FindComponent<CameraComponent>(); found != nullptr) {
            camera = found;
            break;
        }
    }
    if (camera == nullptr) {
        return;
    }

    AppFrameTarget* target = ctx.Targets.empty() ? nullptr : &ctx.Targets.front().Target;
    cameras.Add(scene, camera, target);
}

ForwardPipeline::DepthTarget* ForwardPipeline::AcquireDepthTarget(uint32_t flight, uint32_t width, uint32_t height) {
    if (_device == nullptr) {
        return nullptr;
    }
    if (_depthTargets.size() <= flight) {
        _depthTargets.resize(static_cast<size_t>(flight) + 1);
    }
    DepthTarget& dt = _depthTargets[flight];
    if (dt.Texture != nullptr && dt.View != nullptr && dt.Width == width && dt.Height == height) {
        return &dt;
    }

    // 尺寸变化 (或首次): 重建。
    if (_renderPassRegistry != nullptr) {
        _renderPassRegistry->RemoveFramebuffersUsing(dt.View.get());
    }
    dt.View.reset();
    dt.Texture.reset();
    dt.State = render::TextureState::Undefined;

    render::TextureDescriptor texDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = kDepthFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite,
        .Hints = render::ResourceHint::None};
    auto texOpt = _device->CreateTexture(texDesc);
    if (!texOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create depth texture");
        return nullptr;
    }
    dt.Texture = texOpt.Release();
    dt.Texture->SetDebugName("ForwardPipeline Depth");

    render::TextureViewDescriptor viewDesc{
        .Target = dt.Texture.get(),
        .Dim = render::TextureDimension::Dim2D,
        .Format = kDepthFormat,
        .Range = render::SubresourceRange{0, 1, 0, 1},
        .Usage = render::TextureViewUsage::DepthWrite};
    auto viewOpt = _device->CreateTextureView(viewDesc);
    if (!viewOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create depth view");
        dt.Texture.reset();
        return nullptr;
    }
    dt.View = viewOpt.Release();
    dt.Width = width;
    dt.Height = height;
    dt.State = render::TextureState::Undefined;
    return &dt;
}

ForwardPipeline::ShadowCube* ForwardPipeline::AcquireShadowCube(uint32_t flight, uint32_t size) {
    if (_device == nullptr) {
        return nullptr;
    }
    if (_shadowCubes.size() <= flight) {
        _shadowCubes.resize(static_cast<size_t>(flight) + 1);
    }
    ShadowCube& sc = _shadowCubes[flight];
    if (sc.Texture != nullptr && sc.Srv != nullptr && sc.Size == size) {
        return &sc;
    }

    // 首次 (或尺寸变化): 重建 cube 深度纹理 + cube SRV + layered/逐面 DSV。
    if (_renderPassRegistry != nullptr) {
        _renderPassRegistry->RemoveFramebuffersUsing(sc.LayeredDsv.get());
    }
    sc.LayeredDsv.reset();
    for (auto& dsv : sc.FaceDsv) {
        if (_renderPassRegistry != nullptr) {
            _renderPassRegistry->RemoveFramebuffersUsing(dsv.get());
        }
        dsv.reset();
    }
    sc.Srv.reset();
    sc.Texture.reset();
    sc.State = render::TextureState::Undefined;

    render::TextureDescriptor texDesc{
        .Dim = render::TextureDimension::Cube,
        .Width = size,
        .Height = size,
        .DepthOrArraySize = kCubeFaceCount,  // cube = 6 层 2D array
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = kShadowFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite | render::TextureUse::Resource,
        .Hints = render::ResourceHint::None};
    auto texOpt = _device->CreateTexture(texDesc);
    if (!texOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow cube texture");
        return nullptr;
    }
    sc.Texture = texOpt.Release();
    sc.Texture->SetDebugName("ForwardPipeline ShadowCube");

    // cube SRV (采样用, 覆盖全 6 面)。用 Resource usage 走 SRV 路径 (深度格式会被
    // 映射成 R32_FLOAT 采样); DepthRead 会误入 DSV 路径, 不支持 Cube 维度。
    // SubresourceRange 字段序 = {BaseArrayLayer, ArrayLayerCount, BaseMipLevel, MipLevelCount}:
    // 6 个 array 层 (cube 面), 1 个 mip。
    render::TextureViewDescriptor srvDesc{
        .Target = sc.Texture.get(),
        .Dim = render::TextureDimension::Cube,
        .Format = kShadowFormat,
        .Range = render::SubresourceRange{0, kCubeFaceCount, 0, 1},
        .Usage = render::TextureViewUsage::Resource};
    auto srvOpt = _device->CreateTextureView(srvDesc);
    if (!srvOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow cube SRV");
        sc.Texture.reset();
        return nullptr;
    }
    sc.Srv = srvOpt.Release();

    // 支持 VS 写 SV_RenderTargetArrayIndex 时，单个 DSV 覆盖 cube 的全部六层。
    if (_device->GetDetail().IsLayeredRenderingFromVertexShaderSupported) {
        render::TextureViewDescriptor layeredDsvDesc{
            .Target = sc.Texture.get(),
            .Dim = render::TextureDimension::Dim2DArray,
            .Format = kShadowFormat,
            .Range = render::SubresourceRange{0, kCubeFaceCount, 0, 1},
            .Usage = render::TextureViewUsage::DepthWrite};
        auto layeredDsvOpt = _device->CreateTextureView(layeredDsvDesc);
        if (layeredDsvOpt.HasValue()) {
            sc.LayeredDsv = layeredDsvOpt.Release();
        } else {
            RADRAY_WARN_LOG(
                "ForwardPipeline: failed to create layered shadow cube DSV; using six-pass fallback");
        }
    }

    // 正常路径只保留一个 layered DSV；逐面 DSV 仅在能力不足或 layered 提交失败时创建。
    if (sc.LayeredDsv == nullptr && !EnsureShadowCubeFaceDsvs(sc)) {
        sc.Srv.reset();
        sc.Texture.reset();
        return nullptr;
    }

    sc.Size = size;
    sc.State = render::TextureState::Undefined;
    return &sc;
}

bool ForwardPipeline::EnsureShadowCubeFaceDsvs(ShadowCube& cube) {
    if (_device == nullptr || cube.Texture == nullptr) {
        return false;
    }
    for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
        if (cube.FaceDsv[face] != nullptr) {
            continue;
        }
        render::TextureViewDescriptor dsvDesc{
            .Target = cube.Texture.get(),
            .Dim = render::TextureDimension::Dim2DArray,
            .Format = kShadowFormat,
            .Range = render::SubresourceRange{face, 1, 0, 1},
            .Usage = render::TextureViewUsage::DepthWrite};
        auto dsvOpt = _device->CreateTextureView(dsvDesc);
        if (!dsvOpt.HasValue()) {
            RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow cube face DSV {}", face);
            for (auto& dsv : cube.FaceDsv) {
                if (_renderPassRegistry != nullptr) {
                    _renderPassRegistry->RemoveFramebuffersUsing(dsv.get());
                }
                dsv.reset();
            }
            return false;
        }
        cube.FaceDsv[face] = dsvOpt.Release();
    }
    return true;
}

ForwardPipeline::ShadowArray* ForwardPipeline::AcquireShadowArray(uint32_t flight, uint32_t size, uint32_t sliceCount) {
    if (_device == nullptr || sliceCount == 0) {
        return nullptr;
    }
    sliceCount = std::min(sliceCount, kMaxCascades);
    if (_shadowArrays.size() <= flight) {
        _shadowArrays.resize(static_cast<size_t>(flight) + 1);
    }
    ShadowArray& sa = _shadowArrays[flight];
    if (sa.Texture != nullptr && sa.Srv != nullptr && sa.Size == size && sa.SliceCount == sliceCount) {
        return &sa;
    }

    // 首次 (或尺寸/层数变化): 重建 2DArray 深度纹理 + 2DArray SRV + 每层 DSV。
    for (auto& dsv : sa.SliceDsv) {
        if (_renderPassRegistry != nullptr) {
            _renderPassRegistry->RemoveFramebuffersUsing(dsv.get());
        }
        dsv.reset();
    }
    sa.Srv.reset();
    sa.Texture.reset();
    sa.State = render::TextureState::Undefined;

    render::TextureDescriptor texDesc{
        .Dim = render::TextureDimension::Dim2DArray,
        .Width = size,
        .Height = size,
        .DepthOrArraySize = sliceCount,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = kShadowFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilWrite | render::TextureUse::Resource,
        .Hints = render::ResourceHint::None};
    auto texOpt = _device->CreateTexture(texDesc);
    if (!texOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow array texture");
        return nullptr;
    }
    sa.Texture = texOpt.Release();
    sa.Texture->SetDebugName("ForwardPipeline ShadowArray");

    // 2DArray SRV (采样用, 覆盖全部层)。深度格式走 Resource usage 被映射为 R32_FLOAT 采样。
    render::TextureViewDescriptor srvDesc{
        .Target = sa.Texture.get(),
        .Dim = render::TextureDimension::Dim2DArray,
        .Format = kShadowFormat,
        .Range = render::SubresourceRange{0, sliceCount, 0, 1},
        .Usage = render::TextureViewUsage::Resource};
    auto srvOpt = _device->CreateTextureView(srvDesc);
    if (!srvOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow array SRV");
        sa.Texture.reset();
        return nullptr;
    }
    sa.Srv = srvOpt.Release();

    // 每层一个 DSV (渲染用)。
    for (uint32_t slice = 0; slice < sliceCount; ++slice) {
        render::TextureViewDescriptor dsvDesc{
            .Target = sa.Texture.get(),
            .Dim = render::TextureDimension::Dim2DArray,
            .Format = kShadowFormat,
            .Range = render::SubresourceRange{slice, 1, 0, 1},
            .Usage = render::TextureViewUsage::DepthWrite};
        auto dsvOpt = _device->CreateTextureView(dsvDesc);
        if (!dsvOpt.HasValue()) {
            RADRAY_ERR_LOG("ForwardPipeline: failed to create shadow array slice DSV {}", slice);
            sa.Srv.reset();
            sa.Texture.reset();
            return nullptr;
        }
        sa.SliceDsv[slice] = dsvOpt.Release();
    }

    sa.Size = size;
    sa.SliceCount = sliceCount;
    sa.State = render::TextureState::Undefined;
    return &sa;
}

bool ForwardPipeline::RenderPointShadow(
    RenderPipelineContext& ctx,
    Scene* scene,
    const LightSceneProxy& light,
    uint32_t flight,
    PointShadowGpu& outShadow,
    ShadowCube*& outCube) {
    render::CommandBuffer* cmd = ctx.Frame.GetCommandBuffer();
    if (cmd == nullptr || _shadowExecutor == nullptr) {
        return false;
    }
    ShadowCube* cube = AcquireShadowCube(flight, kShadowCubeSize);
    if (cube == nullptr) {
        return false;
    }

    const Eigen::Vector3f lightPos = light.GetOrigin();
    const float radius = light.GetRadius();

    // 6 面矩阵 (供 depth pass 逐面 + 采样 shader 重投影)。
    std::array<Eigen::Matrix4f, kCubeFaceCount> faceVp;
    for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
        faceVp[face] = MakeCubeFaceViewProj(lightPos, face, radius);
        // Eigen 列主序, HLSL float4x4 默认 column_major, 内存一致, 直接拷贝。
        std::memcpy(outShadow.ViewProj[face], faceVp[face].data(), sizeof(float) * 16);
    }
    outShadow.LightPosInvRadius[0] = lightPos.x();
    outShadow.LightPosInvRadius[1] = lightPos.y();
    outShadow.LightPosInvRadius[2] = lightPos.z();
    outShadow.LightPosInvRadius[3] = radius > 0.0f ? 1.0f / radius : 0.0f;

    // 用户无量纲 bias 倍率 (URP 风格): 乘以 cube 面的 texel 世界尺寸得到世界空间偏移。
    // cube 每面 90° 视锥, 远平面 (距光源 radius) 处半宽 = radius, 故 frustum 宽度 = 2*radius,
    // texelSize = 2r / size, 与级联阴影一致。
    const auto& ptLight = static_cast<const PointLightSceneProxy&>(light);
    const float ptTexelSize = (2.0f * radius) / static_cast<float>(cube->Size);
    outShadow.Params[0] = ptLight.GetShadowDepthBias() * ptTexelSize;    // depthBias (世界空间)
    outShadow.Params[1] = ptLight.GetShadowNormalBias() * ptTexelSize;   // normalBias (世界空间)
    outShadow.Params[2] = 1.0f / static_cast<float>(cube->Size);         // invResolution
    outShadow.Params[3] = 1.0f;                                          // enable

    // 构建 shadow caster DrawList (ShadowCaster tag; shader 无该 pass 的 primitive 自动跳过)。
    DrawList casterList;
    for (const unique_ptr<PrimitiveSceneProxy>& proxy : scene->Primitives()) {
        if (proxy == nullptr) {
            continue;
        }
        const uint32_t sectionCount = proxy->GetSectionCount();
        for (uint32_t s = 0; s < sectionCount; ++s) {
            shared_ptr<const MaterialRenderSnapshot> snapshot = proxy->GetSectionSnapshot(s);
            if (snapshot == nullptr) {
                continue;
            }
            casterList.AddPrimitive(std::move(snapshot), proxy.get(), kShadowPassTag, s, 0.0f);
        }
    }
    if (casterList.Empty()) {
        // 没有投影者: 视为无阴影 (但仍需把 cube 清成远深度并转采样布局, 避免采样到脏数据)。
        outShadow.Params[3] = 0.0f;
    } else {
        casterList.SortOpaque();
    }

    // cube 转 DepthWrite 布局 (整资源 barrier)。
    if (cube->State != render::TextureState::DepthWrite) {
        render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
            .Target = cube->Texture.get(),
            .Before = cube->State,
            .After = render::TextureState::DepthWrite};
        cmd->ResourceBarrier(std::span{&barrier, 1});
        cube->State = render::TextureState::DepthWrite;
    }

    _shadowExecutor->ClearGlobals();

    render::RenderPassDepthStencilAttachmentDescriptor depthAttachment{
        .Format = kShadowFormat,
        .SampleCount = 1,
        .DepthLoad = render::LoadAction::Clear,
        .DepthStore = render::StoreAction::Store,
        .StencilLoad = render::LoadAction::DontCare,
        .StencilStore = render::StoreAction::Discard};
    render::RenderPassDescriptor passDesc{
        .ColorAttachments = {},
        .DepthStencilAttachment = depthAttachment};
    auto passOpt = _renderPassRegistry != nullptr
                       ? _renderPassRegistry->GetOrCreateRenderPass(passDesc)
                       : Nullable<render::RenderPass*>{};
    if (!passOpt.HasValue()) {
        return false;
    }
    render::RenderPass* renderPass = passOpt.Get();

    auto renderLayerRange = [&](render::TextureView* depthView,
                                uint32_t layerCount,
                                uint32_t instanceCount,
                                std::string_view name) -> std::optional<uint32_t> {
        auto framebufferOpt = _renderPassRegistry->GetOrCreateFramebuffer(
            renderPass, {}, depthView, cube->Size, cube->Size, layerCount);
        if (!framebufferOpt.HasValue()) {
            return std::nullopt;
        }
        render::RenderPassBeginDescriptor beginDesc{
            .Pass = renderPass,
            .Target = framebufferOpt.Get(),
            .ColorClearValues = {},
            .DepthStencilClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}},
            .Name = name};
        auto encoderOpt = cmd->BeginRenderPass(beginDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("ForwardPipeline: failed to begin '{}'", name);
            return std::nullopt;
        }
        auto encoder = encoderOpt.Release();

        Viewport vp{
            .X = 0.0f,
            .Y = 0.0f,
            .Width = static_cast<float>(cube->Size),
            .Height = static_cast<float>(cube->Size),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f};
        if (_device->GetBackend() == render::RenderBackend::Vulkan) {
            vp.Y = static_cast<float>(cube->Size);
            vp.Height = -static_cast<float>(cube->Size);
        }
        encoder->SetViewport(vp);
        encoder->SetScissor(Rect{0, 0, cube->Size, cube->Size});

        _shadowExecutor->SetRenderPass(renderPass);
        const uint32_t submitted = _shadowExecutor->Execute(
            encoder.get(), casterList, ctx.Frame.GetFrameResources(), instanceCount);
        cmd->EndRenderPass(std::move(encoder));
        return submitted;
    };

    const bool useLayeredRendering =
        _device->GetDetail().IsLayeredRenderingFromVertexShaderSupported &&
        cube->LayeredDsv != nullptr;
    bool pointShadowRendered = false;
    if (useLayeredRendering) {
        ShadowViewConstants sv{};
        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            std::memcpy(sv.ViewProj[face], faceVp[face].data(), sizeof(float) * 16);
        }
        _shadowExecutor->SetViewConstants(
            kShadowViewName,
            std::span<const byte>{reinterpret_cast<const byte*>(&sv), sizeof(sv)});
        _shadowExecutor->EnableGlobalKeyword(forward_pipeline::kKwPointShadowLayered);
        const std::optional<uint32_t> submitted = renderLayerRange(
            cube->LayeredDsv.get(), kCubeFaceCount, kCubeFaceCount, "Point Shadow Layered");
        if (submitted.has_value() && submitted.value() == casterList.Size()) {
            pointShadowRendered = true;
        } else {
            RADRAY_WARN_LOG(
                "ForwardPipeline: layered point shadow failed; using six-pass fallback");
            _shadowExecutor->ClearGlobals();
        }
    }

    // 能力不足或 layered variant 提交失败时，保留逐面路径。
    if (!pointShadowRendered) {
        if (!EnsureShadowCubeFaceDsvs(*cube)) {
            return false;
        }
        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            ShadowViewConstants sv{};
            std::memcpy(sv.ViewProj[0], faceVp[face].data(), sizeof(float) * 16);
            _shadowExecutor->SetViewConstants(
                kShadowViewName,
                std::span<const byte>{reinterpret_cast<const byte*>(&sv), sizeof(sv)});
            const std::optional<uint32_t> submitted = renderLayerRange(
                cube->FaceDsv[face].get(), 1, 1, "Point Shadow Face");
            if (!submitted.has_value() || submitted.value() != casterList.Size()) {
                return false;
            }
        }
    }

    // cube 转采样布局 (ShaderRead)。作为常规 SRV (R32_FLOAT) 在 pixel shader 里比较采样,
    // 故用 ShaderRead (映射到 PIXEL_SHADER_RESOURCE), 而非 DepthRead (DEPTH_READ, 不可采样)。
    render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
        .Target = cube->Texture.get(),
        .Before = render::TextureState::DepthWrite,
        .After = render::TextureState::ShaderRead};
    cmd->ResourceBarrier(std::span{&barrier, 1});
    cube->State = render::TextureState::ShaderRead;

    outCube = cube;
    return true;
}

bool ForwardPipeline::RenderDirectionalShadow(
    RenderPipelineContext& ctx,
    Scene* scene,
    const DirectionalLightSceneProxy& light,
    uint32_t flight,
    CascadeShadowGpu& outShadow,
    ShadowArray*& outArray) {
    render::CommandBuffer* cmd = ctx.Frame.GetCommandBuffer();
    if (cmd == nullptr || _shadowExecutor == nullptr) {
        return false;
    }

    const uint32_t cascadeCount = std::clamp<uint32_t>(light.GetCascadeCount(), 1u, kMaxCascades);
    const uint32_t shadowSize = light.GetShadowMapResolution();
    ShadowArray* array = AcquireShadowArray(flight, shadowSize, cascadeCount);
    if (array == nullptr) {
        return false;
    }

    // ── 级联划分 (自动: practical split scheme / 手动: URP 风格归一化比例) ──
    const float nearZ = std::max(_frame.CameraNearZ, 1e-3f);
    const float shadowFar = std::max(std::min(_frame.CameraFarZ, light.GetShadowDistance()), nearZ + 1e-2f);
    CascadeSplit splits[kMaxCascades];
    if (light.GetCascadeSplitMode() == CascadeSplitMode::Manual) {
        ComputeCascadeSplitsManual(nearZ, shadowFar, cascadeCount, light.GetCascadeSplitRatios(), splits);
    } else {
        ComputeCascadeSplits(nearZ, shadowFar, cascadeCount, light.GetCascadeSplitLambda(), splits);
    }

    // 相机基 (从 view 矩阵行取, LH: row0=right, row1=up, row2=forward)。
    const Eigen::Matrix4f& view = _frame.CameraView;
    const Eigen::Vector3f right = view.block<1, 3>(0, 0).transpose();
    const Eigen::Vector3f upv = view.block<1, 3>(1, 0).transpose();
    const Eigen::Vector3f forward = view.block<1, 3>(2, 0).transpose();
    (void)right;
    (void)upv;
    const float tanHalfV = std::tan(_frame.CameraFovY * 0.5f);
    const float tanHalfH = tanHalfV * _frame.CameraAspect;
    const Eigen::Vector3f eye = _frame.Eye;
    const Eigen::Vector3f lightDir = light.GetDirection();

    // 用户无量纲 bias 倍率 (URP 风格): 逐级联乘以该级联的 texel 世界尺寸得到实际世界偏移,
    // 故不同分辨率 / 不同级联 frustum 大小下表现自动一致。
    const float depthBiasScale = light.GetShadowDepthBias();
    const float normalBiasScale = light.GetShadowNormalBias();

    // 逐级联: 包围球 -> 稳定正交光锥。
    std::array<Eigen::Matrix4f, kMaxCascades> cascadeVp;
    for (uint32_t i = 0; i < cascadeCount; ++i) {
        Eigen::Vector3f center;
        float radius;
        ComputeCascadeSphere(eye, forward, tanHalfV, tanHalfH, splits[i].NearZ, splits[i].FarZ, center, radius);
        radius = std::ceil(radius * 16.0f) / 16.0f;  // 量化半径, 进一步稳定
        // 沿光照方向额外后拉光相机, 以捕获级联球之外 (上游) 的投影者。取半径的固定倍数
        // (而非场景尺度): 既能覆盖多数上游投影者, 又不至于让深度范围过大丢精度。
        const float zExtend = radius * 3.0f;
        cascadeVp[i] = MakeCascadeViewProj(center, radius, lightDir, static_cast<float>(shadowSize), zExtend);

        std::memcpy(outShadow.WorldToShadow[i], cascadeVp[i].data(), sizeof(float) * 16);
        outShadow.CascadeSphere[i][0] = center.x();
        outShadow.CascadeSphere[i][1] = center.y();
        outShadow.CascadeSphere[i][2] = center.z();
        outShadow.CascadeSphere[i][3] = radius * radius;  // shader 用 dist^2 < r^2 判定

        // 该级联正交视锥覆盖 [-radius, radius], 故 frustum 宽度 = 2*radius, texelSize = 2r / size。
        const float texelSize = (2.0f * radius) / std::max(static_cast<float>(shadowSize), 1.0f);
        outShadow.CascadeBias[i][0] = depthBiasScale * texelSize;
        outShadow.CascadeBias[i][1] = normalBiasScale * texelSize;
        outShadow.CascadeBias[i][2] = 0.0f;
        outShadow.CascadeBias[i][3] = 0.0f;
    }
    // 未使用的级联填成不可命中 (半径^2 = 0)。
    for (uint32_t i = cascadeCount; i < kMaxCascades; ++i) {
        std::memset(outShadow.WorldToShadow[i], 0, sizeof(float) * 16);
        outShadow.CascadeSphere[i][0] = 0.0f;
        outShadow.CascadeSphere[i][1] = 0.0f;
        outShadow.CascadeSphere[i][2] = 0.0f;
        outShadow.CascadeSphere[i][3] = 0.0f;
        std::memset(outShadow.CascadeBias[i], 0, sizeof(float) * 4);
    }
    outShadow.Params[0] = 1.0f;                                // enable
    outShadow.Params[1] = static_cast<float>(shadowSize);      // shadowmap size (px)
    outShadow.Params[2] = static_cast<float>(cascadeCount);    // cascade count
    outShadow.Params[3] = static_cast<float>(light.GetShadowSoftMode());  // soft mode

    // ── 构建 shadow caster DrawList (ShadowCaster tag) ──
    DrawList casterList;
    for (const unique_ptr<PrimitiveSceneProxy>& proxy : scene->Primitives()) {
        if (proxy == nullptr) {
            continue;
        }
        const uint32_t sectionCount = proxy->GetSectionCount();
        for (uint32_t s = 0; s < sectionCount; ++s) {
            shared_ptr<const MaterialRenderSnapshot> snapshot = proxy->GetSectionSnapshot(s);
            if (snapshot == nullptr) {
                continue;
            }
            casterList.AddPrimitive(std::move(snapshot), proxy.get(), kShadowPassTag, s, 0.0f);
        }
    }
    if (casterList.Empty()) {
        outShadow.Params[0] = 0.0f;  // 无投影者: 视为无阴影 (仍清深度并转采样布局)。
    } else {
        casterList.SortOpaque();
    }

    // 阴影图转 DepthWrite 布局 (整资源 barrier)。
    if (array->State != render::TextureState::DepthWrite) {
        render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
            .Target = array->Texture.get(),
            .Before = array->State,
            .After = render::TextureState::DepthWrite};
        cmd->ResourceBarrier(std::span{&barrier, 1});
        array->State = render::TextureState::DepthWrite;
    }

    _shadowExecutor->ClearGlobals();

    render::RenderPassDepthStencilAttachmentDescriptor depthAttachment{
        .Format = kShadowFormat,
        .SampleCount = 1,
        .DepthLoad = render::LoadAction::Clear,
        .DepthStore = render::StoreAction::Store,
        .StencilLoad = render::LoadAction::DontCare,
        .StencilStore = render::StoreAction::Discard};
    render::RenderPassDescriptor passDesc{
        .ColorAttachments = {},
        .DepthStencilAttachment = depthAttachment};
    auto passOpt = _renderPassRegistry != nullptr
                       ? _renderPassRegistry->GetOrCreateRenderPass(passDesc)
                       : Nullable<render::RenderPass*>{};
    if (!passOpt.HasValue()) {
        return false;
    }
    render::RenderPass* renderPass = passOpt.Get();

    // 逐级联渲染: 每级联一个 depth-only render pass, 写入对应层。
    for (uint32_t i = 0; i < cascadeCount; ++i) {
        ShadowViewConstants sv{};
        std::memcpy(sv.ViewProj[0], cascadeVp[i].data(), sizeof(float) * 16);
        _shadowExecutor->SetViewConstants(
            kShadowViewName,
            std::span<const byte>{reinterpret_cast<const byte*>(&sv), sizeof(ShadowViewConstants)});

        auto framebufferOpt = _renderPassRegistry->GetOrCreateFramebuffer(
            renderPass, {}, array->SliceDsv[i].get(), shadowSize, shadowSize);
        if (!framebufferOpt.HasValue()) {
            return false;
        }
        render::RenderPassBeginDescriptor beginDesc{
            .Pass = renderPass,
            .Target = framebufferOpt.Get(),
            .ColorClearValues = {},
            .DepthStencilClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}},
            .Name = "Directional Shadow Cascade"};
        auto encoderOpt = cmd->BeginRenderPass(beginDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("ForwardPipeline: failed to begin cascade pass {}", i);
            return false;
        }
        auto encoder = encoderOpt.Release();

        Viewport vp{
            .X = 0.0f,
            .Y = 0.0f,
            .Width = static_cast<float>(shadowSize),
            .Height = static_cast<float>(shadowSize),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f};
        if (_device->GetBackend() == render::RenderBackend::Vulkan) {
            vp.Y = static_cast<float>(shadowSize);
            vp.Height = -static_cast<float>(shadowSize);
        }
        encoder->SetViewport(vp);
        encoder->SetScissor(Rect{0, 0, shadowSize, shadowSize});

        _shadowExecutor->SetRenderPass(renderPass);
        _shadowExecutor->Execute(encoder.get(), casterList, ctx.Frame.GetFrameResources());
        cmd->EndRenderPass(std::move(encoder));
    }

    // 阴影图转采样布局 (ShaderRead)。
    render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
        .Target = array->Texture.get(),
        .Before = render::TextureState::DepthWrite,
        .After = render::TextureState::ShaderRead};
    cmd->ResourceBarrier(std::span{&barrier, 1});
    array->State = render::TextureState::ShaderRead;

    outArray = array;
    return true;
}

void ForwardPipeline::OnSetupCamera(RenderPipelineContext& ctx, const RenderCamera& camera) {
    // 重置 per-camera 共享状态 (URP: 每相机重建 CameraData)。
    _frame = FrameData{};
    if (_executor == nullptr || _device == nullptr || camera.RenderScene == nullptr || camera.ViewCamera == nullptr) {
        return;
    }
    if (!camera.Target.HasValue() || camera.Target.Get() == nullptr) {
        return;
    }
    const AppFrameTarget& target = *camera.Target.Get();
    if (target.BackBuffer == nullptr || target.BackBufferView == nullptr) {
        return;
    }
    for (RenderPipelineTarget& pipelineTarget : ctx.Targets) {
        if (&pipelineTarget.Target == &target) {
            _frame.PipelineTarget = &pipelineTarget;
            break;
        }
    }
    const render::TextureDescriptor bbDesc = target.BackBuffer->GetDesc();
    const uint32_t width = bbDesc.Width;
    const uint32_t height = bbDesc.Height;
    if (width == 0 || height == 0) {
        return;
    }

    CameraComponent* cameraComp = camera.ViewCamera;
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const Eigen::Matrix4f viewProj = cameraComp->ComputeViewProjMatrix(aspect);
    const Eigen::Vector3f eye = cameraComp->GetEyePosition();

    _frame.RenderScene = camera.RenderScene;
    _frame.Target = &target;
    _frame.Width = width;
    _frame.Height = height;
    _frame.Flight = ctx.Frame.FlightIndex();
    _frame.Eye = eye;
    // 相机参数 (供方向光 CSM 计算级联划分 / 视锥切片)。
    _frame.CameraView = cameraComp->ComputeViewMatrix();
    _frame.CameraNearZ = cameraComp->GetNearZ();
    _frame.CameraFarZ = cameraComp->GetFarZ();
    _frame.CameraFovY = cameraComp->GetFovY();
    _frame.CameraAspect = aspect;

    // ── per-view 常量 (ViewProj + 相机位置; 灯光在 OnSetupLights 填充) ──
    ViewConstants& view = _frame.View;
    // Eigen 列主序, HLSL cbuffer float4x4 默认 column_major, 内存布局一致, 直接拷贝。
    std::memcpy(view.ViewProj, viewProj.data(), sizeof(float) * 16);
    view.CameraPosition[0] = eye.x();
    view.CameraPosition[1] = eye.y();
    view.CameraPosition[2] = eye.z();
    view.CameraPosition[3] = 1.0f;
}

void ForwardPipeline::OnSetupLights(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
    if (_frame.RenderScene == nullptr) {
        return;
    }
    ViewConstants& view = _frame.View;

    // 选取第一盏投影阴影的点光源 (记下它在灯光数组里的序号)。
    uint32_t lightCount = 0;
    for (const unique_ptr<LightSceneProxy>& light : _frame.RenderScene->Lights()) {
        if (light == nullptr || light->GetLightType() != LightType::Point || !light->AffectsWorld()) {
            continue;
        }
        if (lightCount >= kMaxPointLights) {
            break;
        }
        const Eigen::Vector3f pos = light->GetOrigin();
        const Eigen::Vector3f color = light->GetColor();  // = lightColor * intensity
        const float range = light->GetRadius();
        PointLightGpu& gpu = view.PointLights[lightCount];
        gpu.Position[0] = pos.x();
        gpu.Position[1] = pos.y();
        gpu.Position[2] = pos.z();
        gpu.Position[3] = range;
        gpu.Intensity[0] = color.x();
        gpu.Intensity[1] = color.y();
        gpu.Intensity[2] = color.z();
        gpu.Intensity[3] = 0.0f;
        if (_frame.ShadowLight == nullptr && light->CastShadow()) {
            _frame.ShadowLight = light.get();
            _frame.ShadowLightIndex = static_cast<int32_t>(lightCount);
        }
        ++lightCount;
    }
    view.LightCounts[0] = lightCount;
    view.LightCounts[1] = 0;  // 默认无阴影, 阴影 pass 成功后置为 index+1 (0 表示无)

    // 收集方向光为 DirectionalLightGpu 数组, 选取第一盏投影阴影的方向光。
    uint32_t dirCount = 0;
    for (const unique_ptr<LightSceneProxy>& light : _frame.RenderScene->Lights()) {
        if (light == nullptr || light->GetLightType() != LightType::Directional || !light->AffectsWorld()) {
            continue;
        }
        if (dirCount >= kMaxDirectionalLights) {
            break;
        }
        const Eigen::Vector3f dir = light->GetDirection();
        const Eigen::Vector3f color = light->GetColor();  // = lightColor * intensity
        DirectionalLightGpu& gpu = view.DirectionalLights[dirCount];
        gpu.Direction[0] = dir.x();
        gpu.Direction[1] = dir.y();
        gpu.Direction[2] = dir.z();
        gpu.Direction[3] = 0.0f;
        gpu.Irradiance[0] = color.x();
        gpu.Irradiance[1] = color.y();
        gpu.Irradiance[2] = color.z();
        gpu.Irradiance[3] = 0.0f;
        if (_frame.DirShadowLight == nullptr && light->CastShadow()) {
            _frame.DirShadowLight = light.get();
            _frame.DirShadowLightIndex = static_cast<int32_t>(dirCount);
        }
        ++dirCount;
    }
    view.LightCounts[2] = dirCount;
    view.LightCounts[3] = 0;  // 默认无方向光阴影, CSM pass 成功后置为 index+1
}

void ForwardPipeline::OnAddRenderPasses(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
    if (_frame.RenderScene == nullptr || _frame.Target == nullptr) {
        return;  // OnSetupCamera 判定不可渲染
    }
    // URP 风格: 按需把持有的逻辑 pass 注入本相机的执行队列, 基类按 RenderPassEvent 排序执行。
    if (_frame.ShadowLight != nullptr) {
        EnqueuePass(&_shadowPass);
    }
    if (_frame.DirShadowLight != nullptr) {
        EnqueuePass(&_dirShadowPass);
    }
    EnqueuePass(&_colorPass);
}

void ForwardPipeline::ShadowCasterPass::Execute(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)camera;
    FrameData& frame = _owner->_frame;
    if (frame.ShadowLight == nullptr || frame.RenderScene == nullptr) {
        return;
    }
    frame.ShadowReady = _owner->RenderPointShadow(
        ctx, frame.RenderScene, *frame.ShadowLight, frame.Flight, frame.View.PointShadow, frame.ShadowCube);
    if (frame.ShadowReady) {
        frame.View.LightCounts[1] = static_cast<uint32_t>(frame.ShadowLightIndex) + 1u;
    }
}

void ForwardPipeline::DirectionalShadowCasterPass::Execute(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)camera;
    FrameData& frame = _owner->_frame;
    if (frame.DirShadowLight == nullptr || frame.RenderScene == nullptr) {
        return;
    }
    const auto* dirLight = static_cast<const DirectionalLightSceneProxy*>(frame.DirShadowLight);
    frame.DirShadowReady = _owner->RenderDirectionalShadow(
        ctx, frame.RenderScene, *dirLight, frame.Flight, frame.View.DirectionalShadow, frame.ShadowArray);
    if (frame.DirShadowReady) {
        frame.View.LightCounts[3] = static_cast<uint32_t>(frame.DirShadowLightIndex) + 1u;
    }
}

void ForwardPipeline::ForwardColorPass::Execute(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)camera;
    ForwardPipeline* self = _owner;
    FrameData& frame = self->_frame;
    if (frame.RenderScene == nullptr || frame.Target == nullptr) {
        return;
    }
    render::CommandBuffer* cmd = ctx.Frame.GetCommandBuffer();
    if (cmd == nullptr) {
        return;
    }
    const AppFrameTarget& target = *frame.Target;
    const uint32_t width = frame.Width;
    const uint32_t height = frame.Height;
    const uint32_t flight = frame.Flight;

    self->_executor->ClearGlobals();
    self->_executor->SetViewConstants(
        kViewName,
        std::span<const byte>{reinterpret_cast<const byte*>(&frame.View), sizeof(ViewConstants)});
    // 阴影相关的全局资源 / keyword 仅在本帧确有投影阴影时绑定: 开启编译期 _POINT_SHADOWS
    // keyword 让 forward_pass.hlsl 编进阴影采样变体并绑定阴影图 + 比较采样器。无阴影帧不开,
    // 变体里整块阴影代码被剔除, 阴影图 / 采样器绑定名不存在于变体, 跳过绑定。
    if (frame.ShadowReady && frame.ShadowCube != nullptr && frame.ShadowCube->Srv != nullptr) {
        self->_executor->EnableGlobalKeyword(forward_pipeline::kKwPointShadows);
        self->_executor->SetGlobalTexture(kShadowCubeName, frame.ShadowCube->Srv.get());

        // shadow comparison sampler (LessEqual): 标准深度, 近值小, 通过测试 (可见) 时返回 1。
        if (self->_samplerCache != nullptr) {
            render::SamplerDescriptor sd{};
            sd.AddressS = render::AddressMode::ClampToEdge;
            sd.AddressT = render::AddressMode::ClampToEdge;
            sd.AddressR = render::AddressMode::ClampToEdge;
            sd.MinFilter = render::FilterMode::Linear;
            sd.MagFilter = render::FilterMode::Linear;
            sd.MipmapFilter = render::FilterMode::Nearest;
            sd.LodMin = 0.0f;
            sd.LodMax = 0.0f;
            sd.Compare = render::CompareFunction::LessEqual;
            sd.AnisotropyClamp = 0;
            auto samplerOpt = self->_samplerCache->GetOrCreate(sd);
            if (samplerOpt.HasValue()) {
                self->_executor->SetGlobalSampler(kShadowSamplerName, samplerOpt.Get());
            }
        }
    }

    // 方向光级联阴影: 本帧确有投影阴影的方向光时, 开启编译期 _DIRECTIONAL_SHADOWS keyword
    // 并绑定级联阴影图 (Texture2DArray) + 比较采样器。无阴影帧关闭, 变体里整块 CSM 代码被剔除。
    if (frame.DirShadowReady && frame.ShadowArray != nullptr && frame.ShadowArray->Srv != nullptr) {
        self->_executor->EnableGlobalKeyword(forward_pipeline::kKwDirectionalShadows);
        self->_executor->SetGlobalTexture(kShadowArrayName, frame.ShadowArray->Srv.get());

        // 方向光级联阴影与点光源阴影共用同一比较采样器名 (gShadowSampler)。
        // 若点光源阴影未启用则在此单独设置一次。
        if (self->_samplerCache != nullptr) {
            render::SamplerDescriptor sd{};
            sd.AddressS = render::AddressMode::ClampToEdge;
            sd.AddressT = render::AddressMode::ClampToEdge;
            sd.AddressR = render::AddressMode::ClampToEdge;
            sd.MinFilter = render::FilterMode::Linear;
            sd.MagFilter = render::FilterMode::Linear;
            sd.MipmapFilter = render::FilterMode::Nearest;
            sd.LodMin = 0.0f;
            sd.LodMax = 0.0f;
            sd.Compare = render::CompareFunction::LessEqual;
            sd.AnisotropyClamp = 0;
            auto samplerOpt = self->_samplerCache->GetOrCreate(sd);
            if (samplerOpt.HasValue()) {
                self->_executor->SetGlobalSampler(kShadowSamplerName, samplerOpt.Get());
            }
        }
    }

    // ── 构建 DrawList (全收, 无视锥裁剪) ──
    // 按 material->IsTransparent() 分成两个 list: 不透明先画 (写深度), 透明后画 (读深度做遮挡)。
    DrawList opaqueList;
    DrawList transparentList;
    for (const unique_ptr<PrimitiveSceneProxy>& proxy : frame.RenderScene->Primitives()) {
        if (proxy == nullptr) {
            continue;
        }
        const uint32_t sectionCount = proxy->GetSectionCount();
        if (sectionCount == 0) {
            continue;
        }
        const Eigen::Vector3f center = proxy->GetLocalToWorld().block<3, 1>(0, 3);
        const float viewDistance = (center - frame.Eye).norm();
        for (uint32_t s = 0; s < sectionCount; ++s) {
            shared_ptr<const MaterialRenderSnapshot> snapshot = proxy->GetSectionSnapshot(s);
            if (snapshot == nullptr) {
                continue;
            }
            DrawList& drawList = snapshot->IsTransparent() ? transparentList : opaqueList;
            drawList.AddPrimitive(std::move(snapshot), proxy.get(), kForwardPassTag, s, viewDistance);
        }
    }
    opaqueList.SortOpaque();            // RenderQueue 升序 -> material 批处理 -> 近到远
    transparentList.SortTransparent();  // RenderQueue 升序 -> 远到近 (back-to-front)

    // ── 深度目标 + 单 render pass ──
    DepthTarget* depth = self->AcquireDepthTarget(flight, width, height);
    if (depth == nullptr || depth->View == nullptr) {
        return;
    }

    // 深度纹理转换到 DepthWrite 布局 (首次为 Undefined, 后续帧保持 DepthWrite)。
    if (depth->State != render::TextureState::DepthWrite) {
        render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
            .Target = depth->Texture.get(),
            .Before = depth->State,
            .After = render::TextureState::DepthWrite};
        cmd->ResourceBarrier(std::span{&barrier, 1});
        depth->State = render::TextureState::DepthWrite;
    }

    render::RenderPassColorAttachmentDescriptor colorAttachment{
        .Format = target.BackBuffer->GetDesc().Format,
        .SampleCount = 1,
        .Load = frame.PipelineTarget != nullptr && frame.PipelineTarget->ContentDrawn
                    ? render::LoadAction::Load
                    : render::LoadAction::Clear,
        .Store = render::StoreAction::Store};
    render::RenderPassDepthStencilAttachmentDescriptor depthAttachment{
        .Format = kDepthFormat,
        .SampleCount = 1,
        .DepthLoad = render::LoadAction::Clear,
        .DepthStore = render::StoreAction::Store,
        .StencilLoad = render::LoadAction::DontCare,
        .StencilStore = render::StoreAction::Discard};
    render::RenderPassDescriptor passDesc{
        .ColorAttachments = std::span{&colorAttachment, 1},
        .DepthStencilAttachment = depthAttachment};
    auto passOpt = self->_renderPassRegistry != nullptr
                       ? self->_renderPassRegistry->GetOrCreateRenderPass(passDesc)
                       : Nullable<render::RenderPass*>{};
    if (!passOpt.HasValue()) {
        return;
    }
    render::TextureView* colorView = target.BackBufferView;
    auto framebufferOpt = self->_renderPassRegistry->GetOrCreateFramebuffer(
        passOpt.Get(), std::span<render::TextureView* const>{&colorView, 1}, depth->View.get(), width, height);
    if (!framebufferOpt.HasValue()) {
        return;
    }
    const render::ColorClearValue colorClear{{0.02f, 0.02f, 0.03f, 1.0f}};
    render::RenderPassBeginDescriptor beginDesc{
        .Pass = passOpt.Get(),
        .Target = framebufferOpt.Get(),
        .ColorClearValues = std::span{&colorClear, 1},
        .DepthStencilClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}},
        .Name = "Forward Opaque"};
    auto encoderOpt = cmd->BeginRenderPass(beginDesc);
    if (!encoderOpt.HasValue()) {
        RADRAY_ERR_LOG("ForwardPipeline: failed to begin forward render pass");
        return;
    }
    auto encoder = encoderOpt.Release();

    Viewport vp{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(width),
        .Height = static_cast<float>(height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f};
    if (self->_device->GetBackend() == render::RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(height);
        vp.Height = -static_cast<float>(height);
    }
    encoder->SetViewport(vp);
    encoder->SetScissor(Rect{0, 0, width, height});

    // 同一 render pass、同一深度缓冲: 先不透明 (写深度), 再透明 (depth-write-off + alpha blend,
    // 复用不透明已写入的深度做遮挡测试; back-to-front 保证半透明叠加顺序正确)。
    FrameResources& resources = ctx.Frame.GetFrameResources();
    self->_executor->SetRenderPass(passOpt.Get());
    self->_executor->Execute(encoder.get(), opaqueList, resources);
    self->_executor->Execute(encoder.get(), transparentList, resources);

    cmd->EndRenderPass(std::move(encoder));
    if (frame.PipelineTarget != nullptr) {
        frame.PipelineTarget->ContentDrawn = true;
    }
}

}  // namespace radray
