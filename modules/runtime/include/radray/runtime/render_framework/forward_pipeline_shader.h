#pragma once

#include <optional>
#include <string_view>

#include <radray/render/common.h>
#include <radray/types.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/render_framework/render_pipeline.h>

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

/// program 稳定逻辑名 (透传为 ShaderVariantDescriptor::LogicalName)。
/// 跨进程稳定, 供预编译缓存 (DXC 缺失路径) 定位磁盘烘焙产物; 也是 tools/bake_shaders.py
/// 变体集合里的 program 名。opaque / transparent 共享同源 + 同 keyword, 使用同一逻辑名
/// (blend / 深度写属 PSO 固定状态, 不影响编译产物)。
inline constexpr std::string_view kForwardProgramName = "forward_pipeline/forward";
inline constexpr std::string_view kShadowProgramName = "forward_pipeline/shadow";

/// alpha / 双面 keyword。
inline constexpr std::string_view kKwAlphaTest = "_ALPHATEST_ON";
inline constexpr std::string_view kKwDoubleSided = "_DOUBLESIDED_ON";
inline constexpr std::string_view kKwAlphaBlend = "_ALPHABLEND_ON";

/// 点光源阴影 keyword (对应 URP 的 _MAIN_LIGHT_SHADOWS multi_compile)。
/// 由管线按帧全局开关 (非 per-material): 本帧有投影阴影的点光源时启用, 让 forward_pass.hlsl
/// 编译进阴影采样路径; 无阴影时关闭, 变体里整块阴影代码 + 阴影图绑定被剔除。
inline constexpr std::string_view kKwPointShadows = "_POINT_SHADOWS";

/// 方向光级联阴影 keyword (对应 URP 的 _MAIN_LIGHT_SHADOWS_CASCADE)。
/// 由管线按帧全局开关: 本帧有投影级联阴影的方向光时启用, 让 forward_pass.hlsl 编译进
/// 级联阴影采样路径并绑定 Texture2DArray 阴影图; 无阴影时关闭, 整块 CSM 代码被剔除。
inline constexpr std::string_view kKwDirectionalShadows = "_DIRECTIONAL_SHADOWS";

/// ShadowCaster 专用 keyword：VS 以 6 个 instance 输出到 cubemap 的 6 个 array layer。
/// 仅在设备支持从 vertex shader 写 SV_RenderTargetArrayIndex 时由点光源阴影 pass 启用。
inline constexpr std::string_view kKwPointShadowLayered = "_POINT_SHADOW_LAYERED";

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

/// forward_pass.hlsl 的 MaterialConstants cbuffer, 逐字节对应 (float4 x6)。
/// 统一覆盖程序化几何 (纯常量) 与 metallic-roughness 材质。
struct ForwardMaterialConstants {
    float BaseColor[4];    // rgb 基础色 (= baseColorFactor), a 不透明度
    float Pbr[4];          // x metallic, y roughness, z alphaCutoff, w normalScale
    float Emissive[4];     // rgb 自发光 (已乘 strength), w occlusionStrength
    float Principled0[4];  // x specular, y specular tint, z clearcoat, w clearcoat gloss
    float Principled1[4];  // x sheen, y sheen tint, z anisotropic, w flatness
    float Principled2[4];  // x spec trans, y eta, zw 保留
};

/// forward_pass.hlsl 的完整 keyword 表 (贴图存在性 + alpha/双面; 顺序即 bit 位)。
ShaderKeywordSet MakeForwardKeywordSet();

/// 半透明材质的 PSO 固定功能状态覆盖 (对齐旧 transparent pass 的烘死值):
/// 标准 alpha blend (SrcAlpha / OneMinusSrcAlpha) + 关闭深度写 (复用不透明已写深度做遮挡) +
/// 双面可见 (不剔除)。opaque / mask 材质无需覆盖 (沿用 shader pass 基线)。
inline MaterialRenderState MakeForwardTransparentRenderState() noexcept {
    MaterialRenderState rs{};
    rs.Cull = render::CullMode::None;
    rs.DepthWrite = false;
    rs.OverrideBlend = true;
    rs.Blend = render::BlendState{
        render::BlendComponent{
            render::BlendFactor::SrcAlpha,
            render::BlendFactor::OneMinusSrcAlpha,
            render::BlendOperation::Add},
        render::BlendComponent{
            render::BlendFactor::One,
            render::BlendFactor::OneMinusSrcAlpha,
            render::BlendOperation::Add}};
    return rs;
}

/// 构建 forward-pipeline shader (forward_pass.hlsl)。
///
/// 从 <renderSystem 的 shader include 根>/forward_pipeline/forward_pass.hlsl 读取着色源;
/// withShadowCaster=true 时附带 shadow_pass.hlsl 的 ShadowCaster pass (depth-only), 让物体投射
/// 点光源阴影。顶点布局固定为 POSITION/NORMAL/TEXCOORD/TANGENT (统一 shader 始终消费 TANGENT)。
///
/// PSO 固定功能状态 (blend / zwrite / cull) 采基线值 (不透明: 深度写开、背面剔除、无混合);
/// opaque / transparent / 双面 等差异由材质经 MaterialRenderState 在 PSO 构建时覆盖, 因此
/// 同一份 shader + 同一 keyword 表只需一个 ShaderAsset (对齐 Unity 的 [_Prop] 渲染状态)。
///
/// 读取源失败返回 nullopt (并记录错误)。
std::optional<StreamingAssetRef<ShaderAsset>> BuildForwardShader(
    AssetManager& assets,
    RenderSystem& renderSystem,
    const ShaderKeywordSet& keywords,
    render::TextureFormat colorFormat,
    bool withShadowCaster);

}  // namespace radray
