#ifndef RADRAY_FORWARD_PIPELINE_BINDINGS_HLSLI
#define RADRAY_FORWARD_PIPELINE_BINDINGS_HLSLI

#include <core/platform.hlsli>

// forward 系管线的绑定 ABI。
//
// 这里是 D3D register / Vulkan binding 编号的唯一定义处, 编号必须与 CPU 端 forward 管线的
// binding group 分配一致。所有 pass 通过下面的宏声明绑定, 不直接写 register(...) 字面量 ——
// 散落的字面量无法被编译器检查出与 group 编号的分歧。
//
// group 划分:
//   0 = per-object  每 draw 更新
//   1 = per-view    每视图更新 (pipeline 提供)
//   2 = per-material 材质持久绑定 (由具体 pass 按需声明)

#define RADRAY_FORWARD_OBJECT_GROUP 0
#define RADRAY_FORWARD_VIEW_GROUP 1
#define RADRAY_FORWARD_MATERIAL_GROUP 2

#define RADRAY_FORWARD_OBJECT_SPACE space0
#define RADRAY_FORWARD_VIEW_SPACE space1
#define RADRAY_FORWARD_MATERIAL_SPACE space2

// 同一份声明在 D3D 与 Vulkan 下各自展开。slot 与 binding 分开传是因为 D3D 的 t/s/b
// 各自独立编号, 而 Vulkan 的 binding 在一个 set 内统一编号。
#define RADRAY_FORWARD_VIEW_CBUFFER(type, name, slot, binding) \
    VK_BINDING(binding, RADRAY_FORWARD_VIEW_GROUP)             \
    ConstantBuffer<type> name : register(b##slot, RADRAY_FORWARD_VIEW_SPACE)

#define RADRAY_FORWARD_MATERIAL_CBUFFER(type, name, slot, binding) \
    VK_BINDING(binding, RADRAY_FORWARD_MATERIAL_GROUP)             \
    ConstantBuffer<type> name : register(b##slot, RADRAY_FORWARD_MATERIAL_SPACE)

#define RADRAY_FORWARD_MATERIAL_TEXTURE2D(name, slot, binding) \
    VK_BINDING(binding, RADRAY_FORWARD_MATERIAL_GROUP)         \
    Texture2D name : register(t##slot, RADRAY_FORWARD_MATERIAL_SPACE)

#define RADRAY_FORWARD_MATERIAL_SAMPLER(name, slot, binding) \
    VK_BINDING(binding, RADRAY_FORWARD_MATERIAL_GROUP)       \
    SamplerState name : register(s##slot, RADRAY_FORWARD_MATERIAL_SPACE)

/// per-object 常量。所有 forward 系 pass 共用同一份布局与同一个槽 (b1, space0),
/// 故定义在这里而非各 pass 内部 —— 两份独立定义的布局分歧编译器查不出来。
struct ObjectConstants {
    float4x4 ObjectToWorld;
};

VK_BINDING(1, RADRAY_FORWARD_OBJECT_GROUP)
ConstantBuffer<ObjectConstants> gPerObject : register(b1, RADRAY_FORWARD_OBJECT_SPACE);

#endif
