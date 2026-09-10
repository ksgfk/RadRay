#include <core/platform.hlsli>
#include <pipelines/forward/cbuffers.hlsli>

VK_BINDING(0, 0) ConstantBuffer<Forward_ViewData> ForwardView : register(b0, space0);
VK_BINDING(0, 1) ConstantBuffer<Forward_MaterialData> ForwardMaterial : register(b0, space1);
VK_BINDING(0, 2) ConstantBuffer<Forward_ObjectData> ForwardObject : register(b0, space2);
VK_BINDING(0, 3) ConstantBuffer<Forward_PassData> ForwardPass : register(b0, space3);
VK_BINDING(0, 4) ConstantBuffer<Forward_EffectsData> Effects : register(b0, space4);
VK_BINDING(0, 5) ConstantBuffer<Forward_OutputSurfaceData> OutputSurface : register(b0, space5);
VK_BINDING(0, 6) RWStructuredBuffer<float4> Output : register(u0, space6);

[shader("compute")]
[numthreads(1, 1, 1)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    float4 acc = 0;
    acc += ForwardView.ViewProj[0];
    acc += ForwardView.PreviousViewProj[0];
    acc += ForwardView.EyePosition;
    acc += ForwardView.DirectionalLightCount;
    acc += ForwardView.PointLightCount;
    acc += ForwardView.LightCountPadding.xyxx;
    [unroll] for (uint d = 0; d < RADRAY_MAX_DIRECTIONAL_LIGHTS; ++d) {
        acc += ForwardView.DirectionalLights[d].Direction;
        acc += ForwardView.DirectionalLights[d].Irradiance;
    }
    [unroll] for (uint p = 0; p < RADRAY_MAX_POINT_LIGHTS; ++p) {
        acc += ForwardView.PointLights[p].Position;
        acc += ForwardView.PointLights[p].Intensity;
    }
    acc += ForwardMaterial.BaseColor;
    acc += ForwardMaterial.Surface;
    acc += ForwardMaterial.Transmission;
    acc += ForwardMaterial.UVTransform;
    acc += ForwardObject.LocalToWorld[0];
    acc += ForwardObject.NormalToWorld[0];
    acc += ForwardObject.PreviousLocalToWorld[0];
    acc += ForwardObject.MotionValid;
    acc += ForwardPass.ShadowMatrix0[0];
    acc += ForwardPass.ShadowMatrix1[0];
    acc += ForwardPass.ShadowMatrix2[0];
    acc += ForwardPass.ShadowMatrix3[0];
    [unroll] for (uint c = 0; c < 4; ++c) {
        acc += ForwardPass.ShadowSphere[c];
        acc += ForwardPass.ShadowBias[c];
    }
    acc += ForwardPass.ShadowParams;
    acc += ForwardPass.Extent;
    acc += ForwardPass.LocalLightCount;
    acc += ForwardPass.UseTiles;
    acc += ForwardPass.UseAo;
    acc += ForwardPass.Transparent;
    acc += Effects.InverseProjection[0];
    acc += Effects.InverseViewProjection[0];
    acc += Effects.PreviousViewProjection[0];
    acc += Effects.WorldToView[0];
    acc += Effects.Projection[0];
    acc += Effects.Extent;
    acc += Effects.Options;
    acc += Effects.Eye;
    acc += Effects.LocalLightCount;
    acc += Effects.TileCapacity;
    acc += Effects.HistoryValid;
    acc += Effects.DebugMode;
    acc += OutputSurface.LocalToClip[0];
    acc += OutputSurface.Options;
    Output[id.x] = acc;
}
