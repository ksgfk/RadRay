#ifndef MATERIAL_HLSLI
#define MATERIAL_HLSLI

#include "common.hlsl"

// ── 绑定频率约定(descriptor set 索引 == register space)──
// 这是引擎与所有材质 shader 之间的硬约定,run-time 的 MaterialParameterLayout
// 按同一约定从反射里抽取每个频率的资源:
//   space0        = per-view     (相机 / 光照等每视图常量,set0)
//   space1        = per-material (材质参数 cbuffer + 贴图 + 采样器,set1)
//   push-constant = per-object   (world matrix 等每物体常量)
#define RADRAY_SPACE_PER_VIEW     space0
#define RADRAY_SPACE_PER_MATERIAL space1

// per-material 常量缓冲。统一落在 space1 的 b0。
// 用法: RADRAY_MATERIAL_CBUFFER(MaterialParams, gMaterial);
#define RADRAY_MATERIAL_CBUFFER(type, name) \
    VK_BINDING(0, 1) ConstantBuffer<type> name : register(b0, space1)

// per-material 贴图 / 采样器。Vulkan 下同一 set 内 binding 号必须唯一,
// 故 cbuffer 占 binding 0,贴图 / 采样器从 binding 1 起手动编号。
// 用法: RADRAY_MATERIAL_TEXTURE2D(gBaseColor, 0, 1);  // t0, vk binding 1
//        RADRAY_MATERIAL_SAMPLER(gBaseColorSampler, 0, 2);  // s0, vk binding 2
#define RADRAY_MATERIAL_TEXTURE2D(name, slot, bind) \
    VK_BINDING(bind, 1) Texture2D name : register(t##slot, space1)
#define RADRAY_MATERIAL_SAMPLER(name, slot, bind) \
    VK_BINDING(bind, 1) SamplerState name : register(s##slot, space1)

#endif
