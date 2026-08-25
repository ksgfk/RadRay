# ADR-0051 RootSignature policy 经 target layout resolution 映射到 D3D12 与 Vulkan

状态: 生效
日期: 2026-08
影响: RadRay DXC fork、shader metadata schema、artifact decoder、D3D12/Vulkan pipeline layout、runtime program cache、binding handle 与 shader authoring

## 背景

ADR-0035 把 HLSL `[RootSignature]` 的 serialized carrier 正确交给 D3D12，却把它的语义范围
限定为 DXIL，并只把 static sampler 单独桥接到 Vulkan。ADR-0049 随后增加 group-wide
`ShaderLayoutPolicy`，在 artifact decode 时把逻辑 `CBuffer` 改写成 `DynamicCBuffer`，以便同一条
per-object 数据路径在 D3D12 使用 root CBV、在 Vulkan 使用 dynamic uniform buffer。

这两个局部决策组合后暴露了错误的层次：

- `[RootSignature]` 明明已经表达 descriptor table、root descriptor、root constants 与 static
  sampler policy，却不能成为 Vulkan layout 的 policy 输入；
- group-wide policy 同时选择多个 declaration、跨两个 backend，并把 native placement 塞进逻辑
  resource kind，作用域过大；
- runtime 需要重新判断 compiler policy，甚至承担 stage merge/link 一类本应由 compiler 完成的工作；
- program cache、layout hash 与 command-time dynamic offset 继续依赖未解析的输入形状，无法准确表达
  最终 native layout。

DXIL 与 SPIR-V 的 binding 数字和 native layout 本来就不应相同。需要统一的是“作者对同一 HLSL
declaration 选择了什么 policy”，而不是两套 API 的数字、对象或能力集合。

## 决策

### 1. `[RootSignature]` 是跨 target 的 base policy

`[RootSignature]` 改称 **RootSignature policy**。它仍由 HLSL 作者声明，但不再被定义成 DXIL-only
policy。每个 concrete Variant 先经过一次 compiler-owned、与输出 target 无关的 policy frontend
pass；该 pass 使用与 lane 相同的 root source、include path 顺序、`Defines`、keyword assignments
和完整 `CompilePolicy`，按 canonical HLSL declaration identity 建立 policy。随后才分别 lower：

- DXIL lane 保留标准 serialized Root Signature carrier，并让 D3D12 原样消费；
- SPIR-V lane 输出 Vulkan-specific fixed-width layout records；runtime 不解析 DXIL carrier，也不执行
  stage link 或跨 artifact policy 合并。

两套槽位继续由 HLSL 分别声明：普通资源同时具有 DX `register/space` 与 `VK_BINDING(binding, set)`；
push declaration 同时具有 DX `register` 与 `VK_PUSH_CONSTANT`，且不得再写 `VK_BINDING`。compiler
以 declaration identity 关联两侧，不发布也不要求 runtime 维护数字配对表。

RootSignature policy 的 Vulkan lowering 只实例化当前 SPIR-V Variant 的 active declarations，并只对
能按同一 declaration identity 关联到 policy parameter 的 declaration 应用下表语义。仅在某个 target
存在、因而没有对应 policy parameter 的 declaration 继续由该 target 的 `VK_BINDING`/
`VK_PUSH_CONSTANT` 表达 ordinary descriptor/push 语义，不为它编造 D3 pairing：

| RootSignature policy | D3D12 | Vulkan |
|---|---|---|
| descriptor table range | serialized RS 中的 descriptor table | declaration 的普通 descriptor |
| root CBV | root CBV | dynamic uniform buffer descriptor |
| root SRV/UAV buffer | root SRV/UAV | dynamic storage buffer descriptor |
| `RootConstants` | root constants | 与同一 declaration 对应的 `VK_PUSH_CONSTANT` block |
| `StaticSampler` | 原样 static sampler state | 完整 state 的 immutable sampler |

RootSignature 的 parameter order、table grouping/range offset、range/root/RS flags、input-assembler flag、
deny flags 等没有 Vulkan 对应物的字段明确属于 D3D12，不产生 Vulkan modifier。RootSignature
visibility 用于验证已关联 declaration 的 active stages；Vulkan descriptor/push `stageFlags` 始终取
compiler 已知的实际 active stages，不把 stable superset visibility 原样扩张过去。

当前范围只接受 ordinary graphics/compute global Root Signature 1.0/1.1。DXR Local Root Signature、
directly-indexed heaps、需要多个 active Vulkan push blocks 的 policy 均不在本决策范围。

### 2. 没有 `[RootSignature]` 时不合成公共 policy

没有 `[RootSignature]` 不代表 dynamic buffer 被禁止，也不触发 compiler 猜测：

- D3D12 以 active DXIL metadata 生成 implicit descriptor-table Root Signature；
- Vulkan 以 active SPIR-V metadata 生成普通 descriptors；
- concrete pipeline 可在 layout resolve 时对精确 declaration 应用 backend-specific modifier。

compiler 不生成默认 RootSignature policy，D3D12 fallback 也不污染 Vulkan artifact。

### 3. `Target layout modifier` 只能精确修改合法 placement

移除 group-wide `ShaderLayoutPolicy`。替代物称为 **Target layout modifier**，并放在并列而不抹平
差异的 `ShaderProgramLayoutRecipe` 中：D3D12 options 与 Vulkan options 是两种公开、强类型的
字段，只有当前 backend 的字段参与 resolve 和 cache identity。

每个 modifier 用“canonical HLSL declaration name + expected logical resource kind”选择当前 target
artifact 中的一项。它不按 group、数字 binding 或访问频率推断，也不改变 compiler artifact。

可表达的 modifier 被限制为：

| Target | declaration kind | 唯一允许的修改 |
|---|---|---|
| D3D12 Implicit | count 为 1 且 D3D12 允许作为 root descriptor 的 cbuffer/structured/raw buffer | descriptor table 与 root descriptor 互换 |
| Vulkan | uniform/storage buffer | regular 与 dynamic descriptor 互换 |
| Vulkan | sampler | 指定完整 Vulkan-semantic immutable sampler state；base 已是 immutable 时整体替换 |

D3D12 Explicit serialized Root Signature 是最终权威，不接受 modifier 重写。modifier 不能改变逻辑
resource kind、array count、group/binding、stage visibility 或 push range；texture、typed texel buffer、
storage image 与 push declaration 没有 placement modifier。

Vulkan immutable sampler record 与 modifier 使用 Vulkan-semantic、fixed-width target type；不为了
表面统一而扩张公共 `SamplerDescriptor` 去重述 D3 serialized static sampler。

selector 在当前 Variant 不存在、expected kind 不匹配、同一 declaration 被重复修改、D3D12 Explicit
路径出现 D3 modifier，均表示 framework 自身构造了矛盾 recipe，是 Debug assertion/invariant，
不设计 recoverable validation 或 fallback。HLSL policy 无法映射、device capability 不满足或 native
object 创建失败仍在其自然边界产生 diagnostic/failure。immutable sampler 不得因 device 不支持而
静默降级；pipeline 可以用 Vulkan sampler replacement modifier 明确换成可支持的 state。

### 4. logical kind、resolved layout 与 native object 分层

layout 创建固定为三段：

1. decode target artifact；
2. 用当前 backend options resolve 成 owning、可比较、可 hash 的 `ResolvedD3D12Layout` 或
   `ResolvedVulkanLayout`；
3. 仅从 resolved target layout 创建 native layout 与 command-time metadata。

compiler wire 只表达 logical resource kind 与 base target policy。`CBuffer`、typed
`Buffer<T>/RWBuffer<T>`、structured buffer、raw buffer、texture/storage texture 和 sampler 必须
保持可区分；Table/RootDescriptor 与 Regular/Dynamic 只存在于对应 resolved layout。既有
`ShaderParameterBindingType::DynamicCBuffer`、`DynamicBuffer`、`DynamicRWBuffer` 枚举成员作为公开
identifier 保留且不重命名，但 schema 6 的新 artifact resolve 路径不再生产它们来伪装 logical kind。

compiler 产出的 `BasePipelineLayoutHash` 覆盖 base target semantics。render resolver 另行计算
`ResolvedLayoutHash`，覆盖 canonical resolved semantics，不 hash modifier 输入顺序、重复的默认值或
native handles。modifier 不进入 compiler artifact key；program/layout identity 是 artifact identity
加当前 backend 的 `ResolvedLayoutHash`。本决策不恢复全局 native layout cache。

### 5. program request 与 command binding 使用 resolved identity

runtime 入口收敛为 `GetOrCreateShaderProgram(const ShaderProgramRequest&)`。request 明确携带 source
compile input（含 logical source 与 structured `Defines`）、canonical keyword assignments、完整
`CompilePolicy` 与 `ShaderProgramLayoutRecipe`；source discovery 与 compile 必须收到同一份完整
compile inputs。compiler/artifact cache 区分 source inputs/Defines、assignments、完整 policy、target
与 toolchain，不能只按 source name + assignments 命中。

`BindingHandle` 保留，但公共面只允许 validity 与 equality。factory、generation、namespace 和 table
index 都是 layout 内部实现。`PipelineLayout` 持有 metadata table，descriptor record 保存 logical
kind、group/binding 与一个或多个 native destinations；push record保存 size/stages 及 D3 root
destinations/Vulkan ranges。D3 visibility-disjoint fan-out 因而仍由一个 handle 驱动多个 destinations。

`FindBinding(name)` 对 descriptor 与 push declaration 都返回 handle。dynamic offset 改为
`BindingHandle + Offset`；公共 `BindShaderParameterSet(..., dynamicOffsets)` 保留：D3D12 root
destination 提交 base GPU VA + offset，Vulkan 按 resolved dynamic binding order 打包
`pDynamicOffsets`。push 写入改为 `SetPushConstants(BindingHandle, bytes)`；每次只写 block 的
`[0,size)` prefix，要求 `0 < size <= resolved size` 且 4-byte aligned，未覆盖的 remainder 保持不变，
不提供 destination offset。

## 放弃的方案及代价

- **继续扩张 group-wide `ShaderLayoutPolicy`**：改动小，但一个开关同时修改多个 declaration，无法
  表达 sampler replacement，也继续混淆 logical kind 与 native placement。
- **把所有 layout 选择都放进 `[RootSignature]`**：能减少 runtime options，却无法表达没有 RS 时的
  pipeline 选择、Vulkan device-specific sampler replacement，也会强迫两 backend 拥有相同能力。
- **让 runtime 解析 DXIL serialized RS 再“链接” Vulkan layout**：避免改 compiler fork，但把
  declaration identity、stage merge 与 target lowering 错放到 runtime，并产生第二个 compiler。
- **定义一套完全统一的 DX/Vulkan layout descriptor**：调用表面整齐，却会丢失 D3 parameter
  topology/flags 和 Vulkan dynamic order/immutable sampler semantics。
- **无 `[RootSignature]` 就禁止 dynamic cbuffer**：规则简单，但把“没有公共 base policy”错误解释为
  “pipeline 不能选择 target placement”，直接破坏 per-object data path。
- **modifier 缺项时忽略或静默降级 sampler**：能掩盖调用错误，却会让性能与采样语义依 Variant/device
  静默变化。
- **用数字 binding 或公开 handle 字段作为 selector**：省去 name lookup，却把 recipe 绑定到某个
  target/Variant 的物理编号，无法跨重编译稳定表达作者意图。

## 必须保持为真

- `[RootSignature]` policy frontend 与各 target lane 使用相同 concrete compile inputs；stage/policy
  merge 只在 compiler 内完成。
- DXIL 继续原样携带 serialized RS；Vulkan 只消费 SPIR-V/Vulkan-specific records，不解析该 carrier。
- 普通 declaration 同时写 DX register 与 `VK_BINDING`；push declaration 写 DX register 与
  `VK_PUSH_CONSTANT`，且不写 `VK_BINDING`；跨 target 只按 declaration identity 关联。
- 有 RS 时 root CBV/SRV/UAV、RootConstants 和 StaticSampler 按本文表格映射；无 RS 时两 backend
  各用自己的普通默认布局，dynamic cbuffer 仍可由精确 target modifier 选择。
- Vulkan 只实例化当前 target 的 active declarations；已关联 policy 的 declaration 按映射表 lower，
  target-only declaration 保留标准 Vulkan attribute 语义，stage flags 来自实际 active stages。
- D3D12 Explicit RS 不被 modifier 改写；所有 modifier 都精确选择 declaration，且只能表达本文列出的
  target/kind 组合。
- logical resource kind 不再编码 native placement；typed、structured 与 raw buffer wire records 可区分。
- Vulkan immutable sampler record携带完整 state；D3 static sampler现有 direct-consumption 路径保持。
- full sampler state保持Vulkan target-typed，不扩张公共`SamplerDescriptor`形成第二份D3 policy。
- `ResolvedD3D12Layout` / `ResolvedVulkanLayout` 是 native creation 的唯一数据输入，且其 hash 不依赖
  modifier 顺序或 native handles。
- program cache覆盖完整 compile identity和当前 backend resolved identity；layout recipe不污染
  compiler artifact identity；不存在全局 native layout cache。
- descriptor 与 push 都通过 layout-local opaque `BindingHandle` 提交；dynamic offset不再携带裸 binding
  number，D3 root descriptor与Vulkan dynamic descriptor共享同一公共调用。
- metadata schema 原子升级到 6 并拒绝 schema 4/5；extension ABI 因 interface shape未变继续为 3。

本决策部分取代 ADR-0035 的“Vulkan 与跨 target 边界”：serialized DXIL carrier仍不成为 Vulkan wire，
但 `[RootSignature]` 的 policy 必须由 compiler lower 为 Vulkan records；ADR-0035 的 D3D12
Explicit/Implicit carrier、direct consumption 与 fallback 决策继续生效。

本决策同时部分取代 ADR-0049 的 group-wide residency policy 与以 `Dynamic*` logical type承载
placement 的机制；ADR-0049 的 per-object/per-view dynamic constant-buffer arena、一次 descriptor
写入与 per-draw offset 数据路径继续生效。
