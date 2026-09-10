#ifndef RADRAY_PIPELINES_FORWARD_BINDINGS_HLSLI
#define RADRAY_PIPELINES_FORWARD_BINDINGS_HLSLI

#include <core/platform.hlsli>
#include <pipelines/forward/cbuffers.hlsli>

VK_BINDING(0, 0)
ConstantBuffer<Forward_ViewData> ForwardView : register(b0, space0);

VK_BINDING(0, 1)
ConstantBuffer<Forward_MaterialData> ForwardMaterial : register(b0, space1);

VK_BINDING(1, 1)
Texture2D<float4> AlbedoTexture : register(t0, space1);
VK_BINDING(2, 1)
SamplerState LinearSampler : register(s0, space1);

VK_BINDING(0, 2)
ConstantBuffer<Forward_ObjectData> ForwardObject : register(b0, space2);

#endif
