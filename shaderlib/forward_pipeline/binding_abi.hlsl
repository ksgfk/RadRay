#ifndef RADRAY_FORWARD_BINDING_ABI_HLSL
#define RADRAY_FORWARD_BINDING_ABI_HLSL

// forward 系管线的绑定 ABI: binding group / space 编号, 以及所有 pass 共用的 per-object 常量。
// 这些编号必须与 CPU 端 forward 管线的 binding group 分配保持一致。

#include "common.hlsl"

#define RADRAY_FORWARD_OBJECT_BINDING_GROUP 0
#define RADRAY_FORWARD_PIPELINE_BINDING_GROUP 1
#define RADRAY_FORWARD_MATERIAL_BINDING_GROUP 2

#define RADRAY_FORWARD_OBJECT_SPACE space0
#define RADRAY_FORWARD_PIPELINE_SPACE space1
#define RADRAY_FORWARD_MATERIAL_SPACE space2

// per-object 常量。所有 forward 系 pass 共用同一份布局与同一个绑定槽 (b1, space0),
// 故定义在这里而不是各 pass 内部 —— 两份独立定义无法被编译器检查出布局分歧。
struct PerObject {
    float4x4 ObjectToWorld;
};

VK_BINDING(1, RADRAY_FORWARD_OBJECT_BINDING_GROUP)
ConstantBuffer<PerObject> gPerObject : register(b1, RADRAY_FORWARD_OBJECT_SPACE);

#endif
