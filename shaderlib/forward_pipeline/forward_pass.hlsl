// forward_pass.hlsl —— 入口 shader (entry point)。
//
// 用途: ForwardPipeline 默认前向管线的统一着色 pass。
// 入口: VSMain (Vertex), PSMain (Pixel)。
//
// 着色模型为 Mitsuba3 风格的 Disney Principled BRDF (bsdf/principled.hlsli),
// 支持方向光 (级联阴影) 与点光源 (立方体阴影)。贴图/自发光/AO 全走 keyword 编译期分支,
// 无贴图时 (如程序化几何) 退化为纯常量材质。
//
// 混合 (blend) 与双面 (cull) 不是本 pass 的 keyword —— 它们是纯固定功能状态, 由材质侧的
// MaterialRenderState 表达。shader 无条件写出 Alpha 并按 SV_IsFrontFace 翻转法线, 两者在
// 对应的 BlendState / CullMode 下自然失效。判据见 AGENTS.md 的 keyword 三条准则。
//
// 绑定 (编号定义见 forward_pipeline/bindings.hlsli):
//   group 0: gPerObject (b1)  每 draw
//   group 1: gView (b0) + 阴影资源 (t1/t2/s3)  每视图, 由 forward 管线提供
//   group 2: gMaterial (b0) + 贴图 (t1..t5) + gSampler (s6)  材质持久绑定
//
// 下面的 pragma 是本文件 keyword 组的【唯一权威声明】, tools/shader_gen 据此生成 manifest
// 的 KeywordGroups (详见 runtime/shader_asset_template.h)。DXC 忽略未知 pragma, 故不影响编译。
// 必须写在任何 #if / #ifdef 之外, 否则会形成"要先知道 keyword 才能发现 keyword"的循环。
//
// 阴影两组 (_POINT_SHADOWS / _DIRECTIONAL_SHADOWS) 由提供那些绑定的
// forward_pipeline/view.hlsli 声明, 经 include 自动继承, 这里不重复。
#pragma radray_keyword_group(BaseColorMap, _BASECOLOR_MAP) stages(Pixel)
#pragma radray_keyword_group(MetalRoughnessMap, _METALROUGHNESS_MAP) stages(Pixel)
#pragma radray_keyword_group(NormalMap, _NORMAL_MAP) stages(Pixel)
#pragma radray_keyword_group(OcclusionMap, _OCCLUSION_MAP) stages(Pixel)
#pragma radray_keyword_group(EmissiveMap, _EMISSIVE_MAP) stages(Pixel)
// alpha test 需要 clip(), 固定功能状态表达不了, 故必须是 keyword。
// 混合与双面【不是】keyword: 它们由 MaterialRenderState 的 Blend / Cull 表达 (见 AGENTS.md)。
#pragma radray_keyword_group(AlphaMode, _ALPHATEST_ON) stages(Pixel)

#include <bsdf/principled.hlsli>
#include <core/color.hlsli>
#include <core/frame.hlsli>
#include <forward_pipeline/standard_material.hlsli>
#include <forward_pipeline/view.hlsli>

struct VertexInput {
    float3 Position : POSITION0;
    float3 Normal : NORMAL0;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent : TANGENT0;  // xyz = 切线, w = 副切线手性符号
};

struct VertexOutput {
    float4 Position : SV_Position;
    float3 PositionWorld : POSITION0;
    float3 NormalWorld : NORMAL0;
    float2 TexCoord : TEXCOORD0;
    float4 TangentWorld : TANGENT0;  // xyz = 世界切线, w = 手性
};

VertexOutput VSMain(VertexInput input) {
    float4 position_world = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0f));

    VertexOutput output;
    output.Position = mul(gView.ViewProj, position_world);
    output.PositionWorld = position_world.xyz;
    // 只做旋转/缩放, 故 w = 0。非均匀缩放需要逆转置矩阵, 当前 ABI 未提供。
    output.NormalWorld = mul(gPerObject.ObjectToWorld, float4(input.Normal, 0.0f)).xyz;
    output.TexCoord = input.TexCoord;
    float3 tangent_world = mul(gPerObject.ObjectToWorld, float4(input.Tangent.xyz, 0.0f)).xyz;
    output.TangentWorld = float4(tangent_world, input.Tangent.w);
    return output;
}

float4 PSMain(VertexOutput input, bool is_front_face : SV_IsFrontFace) : SV_Target0 {
    SurfaceSample surface = sample_standard_material(
        input.TexCoord, input.NormalWorld, input.TangentWorld, is_front_face);

#ifdef _ALPHATEST_ON
    // alpha cutoff: "是否裁剪"由 keyword 决定, 阈值 (数值) 走 cbuffer。
    clip(surface.Alpha - saturate(gMaterial.Pbr.z));
#endif

    PrincipledMaterial material = make_standard_principled_material(surface);
    Frame3 frame = make_frame(surface.Normal);

    // 方向约定见 bsdf/principled.hlsli 文件头: wi 指向相机, wo 指向光源, 均在局部系。
    float3 view_dir_world = safe_normalize(
        gView.CameraPosition.xyz - input.PositionWorld, surface.Normal);
    float3 wi = frame_to_local(frame, view_dir_world);
    // wi.z <= 0 (掠射/背向相机) 不在此 early-out: eval_principled_reflection 的
    // front_side 检查已使这些像素的每个瓣都返回 0。提前返回反而会连 emissive 一起吃掉。

    float3 radiance = (float3)0.0f;

    // ── 方向光 ──
    uint directional_count = view_directional_light_count();
    for (uint d = 0; d < directional_count; ++d) {
        DirectionalLight light = gView.DirectionalLights[d];
        float3 light_dir_world = directional_light_direction(light);
        float3 wo = frame_to_local(frame, light_dir_world);
        if (wo.z <= 0.0f) {
            continue;
        }
        float3 irradiance = eval_directional_irradiance(light);
        irradiance *= view_directional_shadow(
            d, input.PositionWorld, surface.Normal, light_dir_world);
        radiance += eval_principled_reflection(material, wi, wo) * irradiance;
    }

    // ── 点光源 ──
    uint point_count = view_point_light_count();
    for (uint p = 0; p < point_count; ++p) {
        PointLight light = gView.PointLights[p];
        float3 light_dir_world = safe_normalize(
            light.Position.xyz - input.PositionWorld, surface.Normal);
        float3 wo = frame_to_local(frame, light_dir_world);
        if (wo.z <= 0.0f) {
            continue;
        }
        float3 irradiance = eval_point_irradiance(light, input.PositionWorld);
        irradiance *= view_point_shadow(p, input.PositionWorld, surface.Normal);
        radiance += eval_principled_reflection(material, wi, wo) * irradiance;
    }

    float3 color = radiance * surface.Occlusion + surface.Emissive;
    color = tonemap_reinhard(color);
    color = linear_to_srgb(saturate(color));
    return float4(color, surface.Alpha);
}
