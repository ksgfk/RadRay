#ifndef RADRAY_SHADOW_HLSL
#define RADRAY_SHADOW_HLSL

// 通用阴影采样设施 (shared shadow sampling library)。
//
// 只放各类阴影技术共用的采样原语: 世界->阴影 投影 (world_to_shadow_uv)、偏移 (bias)、
// 比较采样 tap 与 PCF 核 (4-tap / 5x5 tent)。具体技术各自成文件并 #include 本文件:
//   - cascade_shadow.hlsl: 方向光级联阴影 (Texture2DArray)
//   - point_shadow.hlsl:   点光源立方体阴影 (TextureCube)

#include "common.hlsl"

float3 world_to_shadow_uv(float4x4 worldToShadow, float3 posW, out bool inside) {
    float4 c = mul(worldToShadow, float4(posW, 1.0f));
    float3 ndc = c.xyz / c.w;
    float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    inside = (ndc.z > 0.0f && ndc.z < 1.0f && all(uv >= 0.0f) && all(uv <= 1.0f));
    return float3(uv, ndc.z);
}

float3 apply_shadow_bias(float3 posW, float3 normalW, float3 dirToLight, float depthBias, float normalBias) {
    float invNdotL = 1.0f - saturate(dot(dirToLight, normalW));
    float scale = invNdotL * normalBias;
    posW = dirToLight * depthBias + posW;
    posW = normalW * scale + posW;
    return posW;
}

float SampleShadow_GetTriangleTexelArea(float triangleHeight) {
    return triangleHeight - 0.5f;
}

void SampleShadow_GetTexelAreas_Tent_3x3(float offset, out float4 computedArea, out float4 computedAreaUncut) {
    float offset01SquaredHalved = (offset + 0.5f) * (offset + 0.5f) * 0.5f;
    computedAreaUncut.x = computedArea.x = offset01SquaredHalved - offset;
    computedAreaUncut.w = computedArea.w = offset01SquaredHalved;

    computedAreaUncut.y = SampleShadow_GetTriangleTexelArea(1.5f - offset);
    float clampedOffsetLeft = min(offset, 0.0f);
    float areaOfSmallLeftTriangle = clampedOffsetLeft * clampedOffsetLeft;
    computedArea.y = computedAreaUncut.y - areaOfSmallLeftTriangle;

    computedAreaUncut.z = SampleShadow_GetTriangleTexelArea(1.5f + offset);
    float clampedOffsetRight = max(offset, 0.0f);
    float areaOfSmallRightTriangle = clampedOffsetRight * clampedOffsetRight;
    computedArea.z = computedAreaUncut.z - areaOfSmallRightTriangle;
}

void SampleShadow_GetTexelWeights_Tent_5x5(float offset, out float3 texelsWeightsA, out float3 texelsWeightsB) {
    float4 computedArea;
    float4 computedAreaUncut;
    SampleShadow_GetTexelAreas_Tent_3x3(offset, computedArea, computedAreaUncut);

    texelsWeightsA.x = 0.16f * computedArea.x;
    texelsWeightsA.y = 0.16f * computedAreaUncut.y;
    texelsWeightsA.z = 0.16f * (computedArea.y + 1.0f);
    texelsWeightsB.x = 0.16f * (computedArea.z + 1.0f);
    texelsWeightsB.y = 0.16f * computedAreaUncut.z;
    texelsWeightsB.z = 0.16f * computedArea.w;
}

void SampleShadow_ComputeSamples_Tent_5x5(float4 shadowmapSize, float2 coord, out float fetchesWeights[9], out float2 fetchesUV[9]) {
    float2 tentCenterInTexelSpace = coord.xy * shadowmapSize.zw;
    float2 centerOfFetchesInTexelSpace = floor(tentCenterInTexelSpace + 0.5f);
    float2 offsetFromTentCenterToCenterOfFetches = tentCenterInTexelSpace - centerOfFetchesInTexelSpace;

    float3 texelsWeightsU_A, texelsWeightsU_B;
    float3 texelsWeightsV_A, texelsWeightsV_B;
    SampleShadow_GetTexelWeights_Tent_5x5(offsetFromTentCenterToCenterOfFetches.x, texelsWeightsU_A, texelsWeightsU_B);
    SampleShadow_GetTexelWeights_Tent_5x5(offsetFromTentCenterToCenterOfFetches.y, texelsWeightsV_A, texelsWeightsV_B);

    float3 fetchesWeightsU = float3(texelsWeightsU_A.xz, texelsWeightsU_B.y) + float3(texelsWeightsU_A.y, texelsWeightsU_B.xz);
    float3 fetchesWeightsV = float3(texelsWeightsV_A.xz, texelsWeightsV_B.y) + float3(texelsWeightsV_A.y, texelsWeightsV_B.xz);

    float3 fetchesOffsetsU = float3(texelsWeightsU_A.y, texelsWeightsU_B.xz) / fetchesWeightsU.xyz + float3(-2.5f, -0.5f, 1.5f);
    float3 fetchesOffsetsV = float3(texelsWeightsV_A.y, texelsWeightsV_B.xz) / fetchesWeightsV.xyz + float3(-2.5f, -0.5f, 1.5f);
    fetchesOffsetsU *= shadowmapSize.xxx;
    fetchesOffsetsV *= shadowmapSize.yyy;

    float2 bilinearFetchOrigin = centerOfFetchesInTexelSpace * shadowmapSize.xy;
    fetchesUV[0] = bilinearFetchOrigin + float2(fetchesOffsetsU.x, fetchesOffsetsV.x);
    fetchesUV[1] = bilinearFetchOrigin + float2(fetchesOffsetsU.y, fetchesOffsetsV.x);
    fetchesUV[2] = bilinearFetchOrigin + float2(fetchesOffsetsU.z, fetchesOffsetsV.x);
    fetchesUV[3] = bilinearFetchOrigin + float2(fetchesOffsetsU.x, fetchesOffsetsV.y);
    fetchesUV[4] = bilinearFetchOrigin + float2(fetchesOffsetsU.y, fetchesOffsetsV.y);
    fetchesUV[5] = bilinearFetchOrigin + float2(fetchesOffsetsU.z, fetchesOffsetsV.y);
    fetchesUV[6] = bilinearFetchOrigin + float2(fetchesOffsetsU.x, fetchesOffsetsV.z);
    fetchesUV[7] = bilinearFetchOrigin + float2(fetchesOffsetsU.y, fetchesOffsetsV.z);
    fetchesUV[8] = bilinearFetchOrigin + float2(fetchesOffsetsU.z, fetchesOffsetsV.z);

    fetchesWeights[0] = fetchesWeightsU.x * fetchesWeightsV.x;
    fetchesWeights[1] = fetchesWeightsU.y * fetchesWeightsV.x;
    fetchesWeights[2] = fetchesWeightsU.z * fetchesWeightsV.x;
    fetchesWeights[3] = fetchesWeightsU.x * fetchesWeightsV.y;
    fetchesWeights[4] = fetchesWeightsU.y * fetchesWeightsV.y;
    fetchesWeights[5] = fetchesWeightsU.z * fetchesWeightsV.y;
    fetchesWeights[6] = fetchesWeightsU.x * fetchesWeightsV.z;
    fetchesWeights[7] = fetchesWeightsU.y * fetchesWeightsV.z;
    fetchesWeights[8] = fetchesWeightsU.z * fetchesWeightsV.z;
}

float sample_shadow_tap(Texture2DArray<float> shadowMap, SamplerComparisonState cmp, float2 uv, float slice, float depth) {
    return shadowMap.SampleCmpLevelZero(cmp, float3(uv, slice), depth);
}

float sample_shadow_low(Texture2DArray<float> shadowMap, SamplerComparisonState cmp, float4 shadowmapSize, float2 uv, float slice, float depth) {
    float2 h = shadowmapSize.xy * 0.5f;
    float4 a;
    a.x = sample_shadow_tap(shadowMap, cmp, uv + float2(-h.x, -h.y), slice, depth);
    a.y = sample_shadow_tap(shadowMap, cmp, uv + float2( h.x, -h.y), slice, depth);
    a.z = sample_shadow_tap(shadowMap, cmp, uv + float2(-h.x,  h.y), slice, depth);
    a.w = sample_shadow_tap(shadowMap, cmp, uv + float2( h.x,  h.y), slice, depth);
    return (a.x + a.y + a.z + a.w) * 0.25f;
}

float sample_shadow_medium(Texture2DArray<float> shadowMap, SamplerComparisonState cmp, float4 shadowmapSize, float2 uv, float slice, float depth) {
    float fetchesWeights[9];
    float2 fetchesUV[9];
    SampleShadow_ComputeSamples_Tent_5x5(shadowmapSize, uv, fetchesWeights, fetchesUV);

    float attenuation = 0.0f;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        attenuation += fetchesWeights[i] * sample_shadow_tap(shadowMap, cmp, fetchesUV[i], slice, depth);
    }
    return attenuation;
}

#endif
