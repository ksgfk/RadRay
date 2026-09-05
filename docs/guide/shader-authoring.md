> - 适用: 新增或修改 shaderlib 根 `.hlsl` pass、keyword domain、binding 或 target gate
> - 权威: 本文是 schema 7 当前 HLSL authoring 契约；wire 与 runtime 边界见 shader pipeline 架构文档
> - 锚点: `shaderlib/core/platform.hlsli`, `shaderlib/pipelines/forward/bindings.hlsli`, `shaderlib/pipelines/forward/forward.hlsl`, `shaderlib/passes/depth.hlsl`, `shaderlib/passes/compute.hlsl`, `modules/shader_compiler/tests/test_shaderlib_passes.cpp`

# HLSL authoring

## 文件边界

`shaderlib/` 本身就是 include root。根 `.hlsl` 是一个 pass source unit，`.hlsli` 是共享库
header；include 必须使用 root-relative、尖括号路径：

```hlsl
#include <core/platform.hlsli>
#include <core/color.hlsli>
```

不要把 `shaderlib/` 重复写进 include path，也不要用物理文件系统路径作为 `SourceName`。caller
传入的逻辑路径应类似 `pipelines/forward/forward.hlsl`，它会进入诊断和 compile input identity。

## Entry 与 keyword

使用标准 stage attribute，entry name 不需要额外登记：

```hlsl
#pragma radray_keyword_group QUALITY "low" "high"

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0f);
}
```

graphics 至少有 vertex；pixel 可省略以表达 depth-only。compute source 只能有一个 compute
entry。entry 不能被条件编译包围，也不能让 graphics 与 compute 共用一个 source unit。

每个 keyword group 的 concrete compile request 必须选择一个合法值。普通 `Defines` 只用于
不属于 keyword domain 的编译输入；不要用普通 define 覆盖 keyword group。

`[RootSignature(...)]` 是可选的跨 target **RootSignature policy**。作者一旦在任一相关 entry
写了它，graphics stages 必须解析为一致且覆盖全部 active resources 的合法 policy；额外的 D3
stable-superset ranges/parameters/static samplers 可以保留。compiler 在 target lanes 之前解析一次
policy：DXIL lane 发布 standard serialized Root Signature，SPIR-V lane 按同一 HLSL declaration
identity 发布 Vulkan layout records。runtime 不解析 DXIL blob 来重建 Vulkan layout。

policy 一旦出现，就必须写在该 source 的**每个** entry 上，且各处文本逐字一致（惯用写法是
`#define RS "..."` 后各 entry 写 `[RootSignature(RS)]`）。只写在部分 entry 上会让缺少 attribute
的 stage 编译出不含 serialized Root Signature 的 DXIL，与 frontend 观察到的 policy 不一致，
编译以 2106 失败，而不是把另一个 entry 的 policy 静默套用到该 stage 上。

可移植的 policy 子集按下表 lower：

| RootSignature 项 | D3D12 | Vulkan |
|---|---|---|
| descriptor table range | descriptor table | 普通 descriptor |
| root CBV | root CBV | dynamic uniform buffer |
| root SRV/UAV buffer | root SRV/UAV | dynamic storage buffer |
| `RootConstants` | root constants | 对应 declaration 的 `VK_PUSH_CONSTANT` block |
| `StaticSampler` | full native static sampler | full-state immutable sampler |

parameter/range 顺序、table offset、Root Signature/range/root flags、IA/deny flags 等没有 Vulkan
对应语义的项只约束 D3。对能按同一 declaration identity 关联到 policy parameter 的 Vulkan
declaration，RootSignature visibility 必须覆盖其 active stages；Vulkan stage flags 仍取当前 Variant
的实际 active stages。只在某个 target 存在的 declaration 保留该 target 的标准 ordinary/push
attribute 语义，不需要伪造另一 target 的槽位。ordinary graphics/compute global Root Signature
1.0/1.1 是当前范围；
Local Root Signature、directly-indexed heaps 与多个 active Vulkan push blocks 不支持。

作者不写 `[RootSignature]` 时，compiler 不猜一份公共 policy：DXIL artifact 不携带 serialized RS，
D3D12 RHI 按 active bindings 生成 implicit descriptor-table layout；Vulkan 使用普通 descriptors。
这不会禁止 dynamic cbuffer；具体 pipeline 仍可在 layout resolve 时对精确 declaration 选择合法的
backend-specific placement。malformed policy、跨 stage 冲突、未覆盖 D3 active resource，或一个
已被 policy 指向的 active Vulkan declaration 无法关联/表示时会使编译失败，不回退到缺省路径。

policy 相关的编译诊断：

| Code | 触发条件 |
|---|---|
| 2105 | 同一 translation unit 声明了多份不同的 `[RootSignature]`；policy 是 translation-unit fact，多个 entry 必须写同一份 |
| 2117 | `[RootSignature]` 无法解析或序列化，或含当前不支持的 parameter |
| 2118 | active declaration 的类型不在 contract 内（`TextureBuffer`、acceleration structure、feedback texture 等）；dead declaration 不受约束 |
| 2119 | declaration 记录的 logical kind 与 lane 实际 lower 出的 kind 不一致 |
| 2120 | push declaration 缺 `register()`，无法按 D3 register 关联到 policy parameter |
| 2121 | DXIL lane 的 active resource 未被 policy 覆盖；SPIR-V lane 视为 target-only declaration，保留为 table |
| 2122 | placement 对该 kind/count 非法：只有 count=1 的 `CBuffer`/structured/raw buffer 能做 root descriptor，`StaticSampler` 只能落在 sampler 上 |
| 2123 | policy visibility 不覆盖使用该资源的 active stage |
| 2124 | push/`RootConstants` 不一致：policy 写了 `RootConstants` 但 declaration 缺 `VK_PUSH_CONSTANT`、push block 未被 policy 授权、超出 `num32BitConstants`，或跨 stage 不一致 |

2111/2113 见下一节。这些检查都发生在死资源剥除之后。

## Binding 与 target gate

每个普通资源声明都必须同时写明两侧的 binding：SPIR-V 侧用 `core/platform.hlsli` 的
`VK_BINDING` gate 宏，DXIL 侧用 `register()`。

```hlsl
VK_BINDING(6, 2) Texture2D<float4> AlbedoTexture : register(t0);
VK_BINDING(7, 2) SamplerState LinearSampler : register(s0);
```

SPIR-V lane 会把宏展开为标准 `vk::binding` 属性，DXIL lane 会展开为空；两套 target 的实际
binding 数字可以不同，不能假设 set 等于 space 或 binding 数字相等。compiler按canonical HLSL
declaration identity关联RootSignature policy与两套槽位，caller不维护pairing table。

vertex位置使用`VK_LOCATION`。push declaration是唯一例外：它同时写DX `register()`与
`VK_PUSH_CONSTANT`，但**不得**再写`VK_BINDING`，因为`vk::push_constant`与`vk::binding`不能
同时装饰同一declaration：

```hlsl
struct DrawPushData {
    uint DrawIndex;
};

VK_PUSH_CONSTANT ConstantBuffer<DrawPushData> DrawPush : register(b3, space2);
```

如果RootSignature policy用`RootConstants`覆盖这条declaration，compiler把它lower到Vulkan的同一
push block。一个SPIR-V Variant最多一条active logical push declaration。

两侧都必须显式，因为缺任何一侧时 DXC 都会自行分配槽位，而分配结果在最终 artifact 里与作者
写定的槽位无法区分：DXIL 侧会顺序分配寄存器，SPIR-V 侧会按 `-fvk-*-shift` 策略从 DXIL 寄存器
换算 Vulkan 槽位。两种情况下 wire metadata 里的 binding 号都会变成工具链默认值的函数，改一个
选项就会静默漂移。编译器对此 fail closed：DXIL lane 缺 `register()` 报 2111，SPIR-V lane 缺
`vk::binding` 报 2113。检查发生在死资源剥除之后，未被使用的资源不受约束。

不要在 pass 中手写 backend attribute、创建 numbered binding wrapper，或维护 sidecar metadata。
HLSL declaration name 是 runtime lookup identity；render layout 会为当前 artifact 生成不透明
`BindingHandle`。descriptor与push declaration都通过`FindBinding(name)`取得handle；handle的group、
binding、namespace或内部index都不是authoring ABI。

## Target layout modifier 不是 HLSL metadata

concrete pipeline可以在program request中提供side-by-side D3D12/Vulkan layout recipe，但它不是第二套
shader metadata。每个modifier只用“canonical declaration name + expected logical resource kind”选择
一项：

- D3D12 Implicit：合法、count=1的buffer在descriptor table与root descriptor之间切换；Explicit
  serialized RS不接受改写。
- Vulkan：uniform/storage buffer在regular与dynamic descriptor之间切换；sampler可以指定完整
  immutable sampler state，base已有immutable state时整体替换。

不要通过group index、binding number、更新频率或命名约定推断placement。modifier不能改变资源
kind/count、visibility、slot或push range；texture、typed texel buffer、storage image和push没有
placement modifier。无`[RootSignature]`时使用modifier是正常路径，不是fallback漏洞。

group 的语义属于具体 pipeline，不是 shaderlib 全局规则。当前 forward HLSL 声明为：

| Group | 更新频率 | 当前 binding |
|---|---|---|
| 0 | per-view | `ForwardView`：view-projection、eye 与光照数组 |
| 1 | per-material | `ForwardMaterial`、`AlbedoTexture`、`LinearSampler` |
| 2 | per-object | `ForwardObject`：local-to-world |

Forward 按 `ForwardView` / `ForwardMaterial` / `ForwardObject` declaration name 从当前 artifact
读取 group；上表只是产品 shader 的当前数字，不是 CPU ABI。两 target 的组号可各自变化。
新增或修改 binding 时核对 Forward 私有 resolver 的 declaration/dynamic/同组资源检查；material
通过 `Material::Create(program, "ForwardMaterial")` 选组，消费者不再传 group plan 或写 0/1/2。
view/material/object 数值 buffer 使用具名 struct 加 `ConstantBuffer<T>`，让 artifact type tree
为 CPU 按名打包保留完整根结构与成员 offset；CPU 不声明 mirror struct。

CPU 参数的 canonical 名称从 CBuffer declaration 开始并包含完整成员路径，例如
`ForwardMaterial.BaseColor` 或 `ForwardView.Lights.Direction`。全 program 唯一的叶名仍可作为简写；
两个路径以同名叶子结尾时，仅该简写不可用，必须写 qualified path，program 不会因此拒绝创建。
struct array 的下标不写进名称，由 setter 的 `element` 参数选择。Texture/Sampler declaration 保持
顶层 exact name；若它与 CBuffer 叶名相同，资源 exact name 优先，字段仍可用 qualified path 访问。

## 现有最小 pass

| Pass | 用途 | contract facts |
|---|---|---|
| `pipelines/forward/forward.hlsl` | 内置 forward vertex + pixel | view/material/object、纹理、sampler、Lambert 光照、`QUALITY` |
| `passes/depth.hlsl` | vertex-only | depth topology、`DEPTH_MODE` |
| `passes/compute.hlsl` | compute dispatch | storage buffer、`COMPUTE_MODE` |

这三条 pass 由 `RadRayShaderLibPass` 读取真实 shaderlib source，执行 discovery，并同时编译
DXIL/SPIR-V lane。新增 pass 后应在同一测试中验证 entry 数量、active binding、stage visibility
和 target-specific binding facts。

## 验收命令

```powershell
cmake --build build_debug --parallel 24
ctest --test-dir build_debug -C Debug -R "RadRayShaderLibPass" --output-on-failure
```

GPU runtime pass 由 `RadRayRuntimeShaderJit` 验证；没有可用 GPU 时测试可以 skip，但已创建设备
后的资源、pipeline、提交或 readback 错误必须失败。
