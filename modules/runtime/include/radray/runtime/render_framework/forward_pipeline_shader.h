#pragma once

#include <optional>
#include <string_view>

#include <radray/render/common.h>
#include <radray/types.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>

namespace radray {

class AssetManager;
class RenderSystem;

/// ForwardPipeline 默认前向管线的 shader ABI 与构建设施。
///
/// 汇集 forward-pipeline 着色的常量约定 (pass tag / keyword / 绑定名 / 顶点布局) 与
/// shader 变体构建入口。shader 源随引擎部署在 <exe>/shaderlib/forward_pipeline/ 下 (*_pass.hlsl):
///   - forward_pass.hlsl:  统一前向着色 (点光源 + Principled BRDF, 贴图 keyword, 点光源阴影)。
///   - shadow_pass.hlsl:   点光源立方体阴影深度生成 (ShadowCaster, depth-only)。
namespace forward_pipeline {

/// pass tag (对齐 ForwardPipeline / MeshPassExecutor 的 PassTag)。
inline constexpr std::string_view kForwardPassTag = "UniversalForward";
inline constexpr std::string_view kShadowPassTag = "ShadowCaster";

/// forward-pipeline shader 源文件名 (位于 shaderlib/forward_pipeline/)。
inline constexpr std::string_view kForwardPassFile = "forward_pass.hlsl";
inline constexpr std::string_view kShadowPassFile = "shadow_pass.hlsl";

/// alpha / 双面 keyword。
inline constexpr std::string_view kKwAlphaTest = "_ALPHATEST_ON";
inline constexpr std::string_view kKwDoubleSided = "_DOUBLESIDED_ON";
inline constexpr std::string_view kKwAlphaBlend = "_ALPHABLEND_ON";

/// 点光源阴影 keyword (对应 URP 的 _MAIN_LIGHT_SHADOWS multi_compile)。
/// 由管线按帧全局开关 (非 per-material): 本帧有投影阴影的点光源时启用, 让 forward_pass.hlsl
/// 编译进阴影采样路径; 无阴影时关闭, 变体里整块阴影代码 + 阴影图绑定被剔除。
inline constexpr std::string_view kKwPointShadows = "_POINT_SHADOWS";

/// 贴图存在性 keyword (顺序即 bit 位, 与 shader 声明一致, 勿改)。
inline constexpr std::string_view kKwBaseColorMap = "_BASECOLOR_MAP";
inline constexpr std::string_view kKwMetalRoughMap = "_METALROUGHNESS_MAP";
inline constexpr std::string_view kKwNormalMap = "_NORMAL_MAP";
inline constexpr std::string_view kKwOcclusionMap = "_OCCLUSION_MAP";
inline constexpr std::string_view kKwEmissiveMap = "_EMISSIVE_MAP";

/// 贴图 / 采样器绑定名 (与 MaterialAsset::SetTexture/SetSampler 名字一致)。
inline constexpr std::string_view kTexBaseColor = "gBaseColorMap";
inline constexpr std::string_view kTexMetalRough = "gMetalRoughMap";
inline constexpr std::string_view kTexNormal = "gNormalMap";
inline constexpr std::string_view kTexOcclusion = "gOcclusionMap";
inline constexpr std::string_view kTexEmissive = "gEmissiveMap";
inline constexpr std::string_view kSamplerName = "gSampler";

/// 顶点交错布局步幅: POSITION3f + NORMAL3f + TEXCOORD2f + TANGENT4f。
inline constexpr uint64_t kVertexStride = sizeof(float) * (3 + 3 + 2 + 4);

}  // namespace forward_pipeline

/// forward_pass.hlsl 的 MaterialConstants (push_constant), 逐字节对应 (float4 x6)。
/// 统一覆盖程序化几何 (纯常量) 与 metallic-roughness 材质。
struct ForwardMaterialConstants {
    float BaseColor[4];    // rgb 基础色 (= baseColorFactor), a 不透明度
    float Pbr[4];          // x metallic, y roughness, z alphaCutoff, w normalScale
    float Emissive[4];     // rgb 自发光 (已乘 strength), w occlusionStrength
    float Principled0[4];  // x specular, y specular tint, z clearcoat, w clearcoat gloss
    float Principled1[4];  // x sheen, y sheen tint, z anisotropic, w flatness
    float Principled2[4];  // x spec trans, y eta, zw 保留
};

/// 一对 forward-pipeline shader 变体 (opaque / transparent)。
/// 只差 PSO 固定状态 (深度写 / cull / blend), 共享同源 + 同 keyword 表。
/// opaque 变体可附带 ShadowCaster pass (depth-only), 让不透明物体投射点光源阴影。
struct ForwardShaderPair {
    StreamingAssetRef<ShaderAsset> Opaque{};
    StreamingAssetRef<ShaderAsset> Transparent{};

    bool IsValid() const noexcept { return Opaque.IsValid() && Transparent.IsValid(); }
};

/// forward_pass.hlsl 的完整 keyword 表 (贴图存在性 + alpha/双面; 顺序即 bit 位)。
ShaderKeywordSet MakeForwardKeywordSet();

/// 构建一对 forward-pipeline shader 变体 (forward_pass.hlsl)。
///
/// 从 <renderSystem 的 shader include 根>/forward_pipeline/forward_pass.hlsl 读取着色源;
/// withShadowCaster=true 时 opaque 变体附带 shadow_pass.hlsl 的 ShadowCaster pass。
/// 顶点布局固定为 POSITION/NORMAL/TEXCOORD/TANGENT (统一 shader 始终消费 TANGENT)。
///
/// 读取源失败返回 nullopt (并记录错误)。
std::optional<ForwardShaderPair> BuildForwardShaderPair(
    AssetManager& assets,
    RenderSystem& renderSystem,
    const ShaderKeywordSet& keywords,
    render::TextureFormat colorFormat,
    bool withShadowCaster);

}  // namespace radray
