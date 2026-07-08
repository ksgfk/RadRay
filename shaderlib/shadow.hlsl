#ifndef RADRAY_SHADOW_HLSL
#define RADRAY_SHADOW_HLSL

#include "common.hlsl"

#define RADRAY_MAX_CASCADES 4
#define RADRAY_MAX_ADD_SHADOW_SLICES 16

struct ShadowParam {
    float4x4 WorldToShadow[RADRAY_MAX_CASCADES];
    float4 CascadeSphere[RADRAY_MAX_CASCADES]; // xyz=center, w=radius^2
    float4 Params; // x enable, y shadowmap size(px), z cascade count, w soft mode
};

// Additional (spot) light shadow atlas. Each spot uses 1 slice.
// (Point light cube shadows live in point_shadow.hlsl and use a TextureCube instead.)
struct AdditionalShadowParam {
    float4x4 WorldToShadow[RADRAY_MAX_ADD_SHADOW_SLICES];
    float4 Params; // x enable, y atlas size(px), z slice count, w soft mode
};

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

float4 apply_shadow_clamping(float4 posCS) {
    posCS.z = max(posCS.z, 0.0f);
    return posCS;
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

uint compute_cascade_index(ShadowParam sp, float3 posW, uint count) {
    uint index = count;
    [unroll]
    for (uint i = 0; i < RADRAY_MAX_CASCADES; ++i) {
        if (i >= count) {
            break;
        }
        float3 d = posW - sp.CascadeSphere[i].xyz;
        float dist2 = dot(d, d);
        if (dist2 < sp.CascadeSphere[i].w && index == count) {
            index = i;
        }
    }
    return index;
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

float sample_shadow_cascade(
    Texture2DArray<float> shadowMap,
    SamplerComparisonState cmp,
    ShadowParam sp,
    float3 posW) {
    if (sp.Params.x < 0.5f) {
        return 1.0f;
    }

    uint count = (uint)sp.Params.z;
    uint cascadeIndex = compute_cascade_index(sp, posW, count);
    if (cascadeIndex >= count) {
        return 1.0f;
    }

    bool inside;
    float3 uvz = world_to_shadow_uv(sp.WorldToShadow[cascadeIndex], posW, inside);
    if (!inside) {
        return 1.0f;
    }

    float slice = (float)cascadeIndex;
    float size = max(sp.Params.y, 1.0f);
    float4 shadowmapSize = float4(1.0f / size, 1.0f / size, size, size);

    uint softMode = (uint)sp.Params.w;
    if (softMode == 1u) {
        return sample_shadow_low(shadowMap, cmp, shadowmapSize, uvz.xy, slice, uvz.z);
    }
    if (softMode == 2u) {
        return sample_shadow_medium(shadowMap, cmp, shadowmapSize, uvz.xy, slice, uvz.z);
    }
    return sample_shadow_tap(shadowMap, cmp, uvz.xy, slice, uvz.z);
}

// Sample one slice of the additional-light shadow atlas with the configured PCF mode.
float sample_additional_shadow_slice(
    Texture2DArray<float> shadowMap,
    SamplerComparisonState cmp,
    AdditionalShadowParam sp,
    uint slice,
    float3 posW) {
    if (sp.Params.x < 0.5f || slice >= (uint)sp.Params.z) {
        return 1.0f;
    }

    bool inside;
    float3 uvz = world_to_shadow_uv(sp.WorldToShadow[slice], posW, inside);
    if (!inside) {
        return 1.0f;
    }

    float size = max(sp.Params.y, 1.0f);
    float4 shadowmapSize = float4(1.0f / size, 1.0f / size, size, size);
    float sliceF = (float)slice;

    uint softMode = (uint)sp.Params.w;
    if (softMode == 1u) {
        return sample_shadow_low(shadowMap, cmp, shadowmapSize, uvz.xy, sliceF, uvz.z);
    }
    if (softMode == 2u) {
        return sample_shadow_medium(shadowMap, cmp, shadowmapSize, uvz.xy, sliceF, uvz.z);
    }
    return sample_shadow_tap(shadowMap, cmp, uvz.xy, sliceF, uvz.z);
}

// Spot light shadow: one perspective slice at firstSlice. firstSlice < 0 => no shadow (returns 1).
float sample_spot_shadow(
    Texture2DArray<float> shadowMap,
    SamplerComparisonState cmp,
    AdditionalShadowParam sp,
    float firstSlice,
    float3 posW) {
    if (firstSlice < 0.0f) {
        return 1.0f;
    }
    return sample_additional_shadow_slice(shadowMap, cmp, sp, (uint)firstSlice, posW);
}

#endif
