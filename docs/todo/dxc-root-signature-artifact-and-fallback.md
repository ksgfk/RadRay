> - 适用: 将显式 HLSL Root Signature 的 DXC serialized blob 纳入 DXIL artifact，并保留缺省 `[RootSignature]` 时由 D3D12 RHI 自动生成 Root Signature 的 fallback
> - 权威: 本文保留 schema 5 optional DXIL carrier/D3 fallback 的历史实施基线；Vulkan policy lowering 与后续 schema cutover 以 ADR-0051 和 `shader-layout-contract-correction.md` 为准
> - 状态: 部分被 ADR-0051 取代（2026-08-25；D3 Explicit/Implicit carrier 路径继续生效；不要再单独发布 `.radray.4`，后续原子升级 schema 6 / `.radray.5`）
> - 锚点: `CONTEXT.md`, `docs/adr/0016-hlsl-and-radray-dxc-are-shader-authority.md`, `docs/architecture/shader-pipeline.md`, `docs/architecture/render-rhi.md`, `docs/research/dxc-serialized-root-signature-artifact.md`, `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/include/radray/shader/shader_artifact.h`, `modules/render/src/shader_artifact.cpp`, `modules/render/src/d3d12/d3d12_impl.cpp`

# DXIL optional serialized Root Signature 与 D3D12 RHI fallback 计划

> 后续修正：ADR-0051 取代本文“`[RootSignature]` 只影响 DXIL、Vulkan 只做 static-sampler bridge”
> 的边界，也取代任何把 placement 称为 residency 的表述。serialized carrier 仍只由 D3D12 消费，
> 但 compiler 必须把同一 RootSignature policy lower 为 Vulkan-specific records；新实施项、wire shape、
> cache/handle contract 与测试矩阵统一见 `shader-layout-contract-correction.md`。本文其余 D3 carrier、
> direct-consumption、Implicit fallback 和 artifact-local coalescing 记录保持为历史基线。

## 目标

DXIL artifact 采用两条互斥路径，是否存在作者显式 `[RootSignature]` 是唯一分界：

- **Explicit 路径**：作者写了有效 `[RootSignature]`；forked DXC 校验并发布其 serialized Root
  Signature，D3D12 RHI 直接用该 blob 创建 native Root Signature，并从同一 blob 建立 CPU binding
  plan。
- **Implicit 路径**：相关 entries 全部没有 `[RootSignature]`；DXC artifact 的 Root Signature range
  为空，只发布当前 Variant 的 active DXIL binding metadata；D3D12 RHI 继续拥有并执行自动生成
  Root Signature 的算法。
- compiler **不实现** fallback Root Signature generator，也不为 Implicit 路径伪造 serialized RS。
- SPIR-V 继续消费 target-specific descriptor、push-constant 与 immutable-sampler metadata；DXIL
  serialized Root Signature 不成为 Vulkan layout wire。

全部决策已关闭；本计划已进入实现阶段。实现仍保持本计划的边界：DXC 不生成 Implicit
fallback，D3D12 RHI 负责该路径，artifact 只做 lane 内 RS coalescing。

## 可行性结论

**可行，而且无需把自动生成算法放进 DXC。**

- DXC 已支持把显式 `[RootSignature]` 编译为 `DXC_OUT_ROOT_SIGNATURE`，该 blob 可以作为 DXIL-only
  artifact range 原样持久化，并直接传给 `ID3D12Device::CreateRootSignature`。
- 这不是需要 RadRay 新实现的编译功能：`dxcompilerobj.cpp` 已把非空 Root Signature stream 作为
  `DXC_OUT_ROOT_SIGNATURE` 放入 `IDxcResult`，`DxilContainerAssembler.cpp` 已用现成 serializer 把
  module 中的 serialized bytes 包成独立 `RTS0` container，并走 DXC validator。
- 当前 fork 的缺口只是 `CompileStage` 只读取 `DXC_OUT_OBJECT`；observer 虽已读取、反序列化并校验
  `DxilModule::GetSerializedRootSignature()`，最终只保留 hash 与扁平 facts。接入应直接调用
  `IDxcResult::GetOutput(DXC_OUT_ROOT_SIGNATURE, ...)` 并保留 output bytes，不新增 HLSL RS parser、
  validator 或 serializer。
- 当前 RadRay D3D12 backend 已能从 binding metadata 生成并序列化 Root Signature，因此缺省路径
  可以保留在 RHI，不要求 DXC 产生 serialized RS。
- 显式 blob 不只用于创建 native object。RHI 还需通过 versioned Root Signature deserializer 读取
  parameter/range/static-sampler 结构，并与 active binding metadata 匹配，建立 descriptor writes、
  root descriptor 与 RootConstants 的 CPU binding plan。
- 两条路径最终都能得到 serialized bytes：Explicit 直接来自 artifact；Implicit 由 RHI generator
  调用 D3D12 serializer 得到。本计划只在 compiler artifact内把同一 lane各 stage的相同 Explicit
  RS outputs合并为一份；跨 artifact/package 或 native object的共享由上层另行处理。

## 工作术语

**Explicit DXIL Root Signature**：
HLSL 作者通过 `[RootSignature(...)]` 声明的 DXIL binding policy，包括 descriptor table、root
descriptor、root constants、static sampler、parameter order、visibility 与 flags；forked DXC
输出其校验后的 serialized form。

**Implicit D3D12 Root Signature**：
DXIL Variant 没有选择 Explicit DXIL Root Signature 时，由 D3D12 RHI 根据 artifact 的 active
binding metadata 自动生成的 Root Signature。它是 runtime backend policy，不是 compiler output。

**Root Signature source**：
一个 DXIL Variant 创建 D3D12 pipeline layout 时采用的互斥来源：`Explicit` 或 `Implicit`。Explicit
以非空 artifact Root Signature range 表达；Implicit 以该 range 为空表达。

避免使用 **Generated DXIL Root Signature** 和 **Canonical DXIL Root Signature**：前者会误导为
compiler 产物，后者会掩盖 artifact 可不携带 RS 以及 backend 存在两条构造路径。

## 已确认需求

1. HLSL `[RootSignature]` **不是必写项**。
2. 作者没有写 `[RootSignature]` 时仍应得到可运行的 DXIL pipeline layout；D3D12 RHI 根据 compiler
   提供的 active binding metadata 自动生成 Root Signature。
3. forked DXC 不拥有、不实现 Implicit D3D12 Root Signature 算法；缺省 artifact 不携带 serialized
   Root Signature。
4. 显式 `[RootSignature]` 是 root descriptor、descriptor table、RootConstants、static sampler、
   parameter order、visibility 与 flags 的作者 policy 来源。
5. Explicit 路径由 D3D12 直接消费 compiler-produced serialized Root Signature，不再从扁平 binding
   records 重建作者已定义的 RS。
6. graphics 的 source 选择属于整个 Variant：任一 entry 携带显式 RS 即选择 Explicit；其他 entry
   可以省略 attribute；多个非空 stage payload 必须逐字节一致。全部 entries 都没有显式 RS 时才选择
   Implicit。compute entry 独立选择。
7. Implicit 路径保持当前 D3D12 backend 的保守能力边界：普通 CBV/SRV/UAV 与 sampler 使用 descriptor
   tables；不推断 root descriptor、RootConstants 或 static sampler。
8. Root Signature去重只指 artifact-local coalescing：graphics stage相同 RS output逐字节核对后，
   一个 DXIL lane只发布一份 optional blob。跨 artifact/package/native object去重不在本计划范围。
9. DXIL 与 SPIR-V layout 保持 target-specific；Implicit D3D12 Root Signature 不要求 Vulkan 生成
   同构 layout。
10. 一旦任一相关 entry 写了 `[RootSignature]`，内容错误、跨 stage 冲突或与 active resources 不兼容
    必须编译失败；不能将错误显式 RS 当成“缺失”并转入 Implicit fallback。
11. Explicit RS 的编译、校验、序列化使用 DXC 现有实现；RadRay extension 只接入并传递
    `DXC_OUT_ROOT_SIGNATURE`。

## 设计基线

### D1：source 选择与 owner

状态：**已确认（2026-08-09，取代此前 compiler-owned fallback 提案）**。

| Source | 触发条件 | serialized RS 来源 | 自动布局 owner |
|---|---|---|---|
| Explicit | 任一相关 entry 有有效 `[RootSignature]` | DXC artifact | 无；作者已定义 |
| Implicit | 所有相关 entries 均无 `[RootSignature]` | D3D12 RHI 运行时序列化 | D3D12 RHI |

RHI 不能在 Explicit 路径根据 binding records 重建、改写或优化作者 RS；compiler 也不能在 Implicit
路径生成默认 RS。二者分别拥有不同 source 下的单一权威，不构成同一路径的双实现。

### D2：graphics stage 合并

状态：**已确认（2026-08-09）**。

- 任一 graphics entry 携带显式 RS，整个 DXIL Variant 进入 Explicit；所有非空 stage RS payload
  必须逐字节一致，所选 RS 必须覆盖 graphics active-stage union。
- 没有 attribute 的其他 stage 共享这份 RS，不要求重复声明。
- 所有 graphics entries 都没有显式 RS，整个 Variant 进入 Implicit。
- compute 只有一个 entry，按该 entry 是否携带显式 RS 选择 source。
- 不允许同一 graphics Variant 混合一部分 Explicit 参数和另一部分 RHI-generated 参数。

### D3：Implicit RHI fallback

状态：**已确认 owner 与保守能力边界；低层布局以当前实现为迁移基线，不再作为产品设计问题逐项
询问**。

首期保留 `DeviceD3D12::CreateRootSignatureInternal` 的现有可观察语义：

- 输入是当前 Variant 的 active binding metadata；
- 普通 CBV/SRV/UAV 与 sampler 通过 descriptor tables 访问；
- 不自动猜 root CBV/SRV/UAV、RootConstants 或 static sampler；
- range 划分、parameter 顺序、visibility 与 flags 以现有实现的行为为 compatibility baseline；
- 实现整理只能消除不稳定的偶然遍历顺序，不应顺便改变 descriptor offset、root parameter index 或
  shader visibility；优化另立设计。

这不是说作者需要重新定义缺省 RS。作者没写 RS 时，RHI fallback policy 本来就负责选择缺省结构；
本计划仅要求迁移不能无意改变现有行为。

### D4：显式 RS 的 active-layout 语义

这里仍需区分“没有覆盖 active resource”的错误与“RS 额外声明当前 Variant 未使用参数”的 stable
superset。当前 fork 拒绝后者，仓库旧文档则描述 projection 到 exact active RS；两者都会迫使不同
Variant 产生不同 RS，或需要 compiler 改写作者 blob。

- **推荐允许作者声明 stable superset**：每个 active DXIL resource 都必须由 RS 覆盖，但额外的合法
  parameters/ranges/static samplers 可以保留；fork 原样发布 DXC serialized blob，不做 projection。
- malformed、重叠、visibility 不允许 active stage、缺少 active resource 等仍是 Explicit validation
  error，遵守 D5 fail closed。
- backend 不删除 inactive parameters、不合并 ranges、不改变 visibility，也不重新序列化作者 policy。
- lane merge按完整 serialized bytes判断相同；不同作者 blob即使对当前 active subset效果相同，也不
  偷偷归一化或合并。

状态：**已确认（2026-08-09）**。

### D5：缺失与错误的边界

**推荐：只有“所有相关 entries 完全没有 `[RootSignature]`”才进入 Implicit。**

只要 source 选择了 Explicit，malformed Root Signature、unsupported version、graphics 非空 payload
不一致、显式 RS 与 active union 不兼容、static sampler 无法关联 declaration 或超出 D3D12 limits
都应 hard fail，不能把作者错误当成“没写”并静默回退到 RHI 自动生成。

状态：**已确认（2026-08-09）**。

### D6：artifact wire 与 identity

建议 wire 增加一个 DXIL-only optional Root Signature range：

- Explicit：range 非空，原样保存完整 `DXC_OUT_ROOT_SIGNATURE` 独立 container，不另行抽取裸 `RTS0`
  payload，也不重新包装；
- Implicit：range 为空，active binding metadata 仍完整；
- SPIR-V：range 必须为空；
- decoder 校验 target、range bounds/alignment、空/非空约束与 schema/toolchain identity；
- 不另加 `RootSignatureSource` enum；非空就是 Explicit，空就是 Implicit。合法 serialized RS 不可能是
  零字节，额外 enum只会制造 `Implicit + non-empty`、`Explicit + empty` 等矛盾状态。

Identity 重新权衡后的建议是区分 carrier bytes与layout semantics：

- **不把 optional range 的完整 `DXC_OUT_ROOT_SIGNATURE` container bytes直接加入 compiler hash。**
  外层 container只是 carrier；相同 `RTS0` payload 不应仅因 wrapper/checksum不同就得到不同 layout
  identity。
- **Explicit 的 serialized `RTS0` payload digest必须进入 `PipelineLayoutHash`。** fork observer已经对
  `DxilModule::GetSerializedRootSignature()` 计算 `RootSignatureHash`，可直接把这个 compiler-owned
  digest作为 target-native layout record的一部分；无需新增 parser或对外挂 container再 hash。
- 这样 active binding coordinates相同但 root/table residency、parameter order、flags、visibility或
  static sampler state不同的 Explicit RS不会错误共享 `PipelineLayoutHash`。stable-superset 中未被当前
  Variant使用的作者 policy也仍属于实际 native layout identity。
- `GpuArtifactHash` 继续覆盖 bytecode与 `PipelineLayoutHash` 所代表的 GPU layout metadata，不需要再
  单独追加 standalone carrier bytes。首期 embedded `RTS0` 也在 bytecode中；这份物理重复不改变
  “RS semantics只作为一项 layout record定义”的契约。
- Implicit artifact 没有 compiler-produced RS semantics，`PipelineLayoutHash` 只覆盖 active binding
  metadata并以 Implicit domain区分；它不能声称覆盖尚未由 RHI生成的 native RS。
- 当前 decoder只比较 compiler-produced `GpuArtifactHash` 与 caller expected value，不重算整个 artifact
  content。以上 hash 是 identity，不是 corruption/integrity保证；完整内容校验由未来
  `ArtifactContentHash`/publisher解决。
- 因为 Explicit 的 RS semantic digest已明确进入 layout identity，未来 strip embedded `RTS0` 或外提
  package object时无需改变这条语义；但 bytecode物理变化仍会自然改变 `BytecodeHash`/
  `GpuArtifactHash`。

D6a carrier 状态：**已确认（2026-08-09）**。D6b hash/identity 状态：**已确认（2026-08-09）**。

### D7：D3D12 CPU binding plan 与支持子集

Explicit 路径不能只把 blob 传给 `CreateRootSignature`：

1. decoder 提供 serialized RS 与 active binding metadata；
2. RHI 用 `D3D12CreateVersionedRootSignatureDeserializer` 只读解析同一 blob；
3. 将 register class/register/space/count/stage mask 与 root parameters、ranges、static samplers 匹配；
4. 建立 descriptor table offset、root descriptor index、RootConstants index 与 static-sampler policy；
5. 每个 active declaration保存一个或多个 destinations；合法 visibility-disjoint fan-out由一次
   `BindingHandle` value write驱动全部 root/table destinations；
6. command encoder 只消费该 plan，不依赖当前 Implicit generator 的固定 parameter 排列。

Implicit 路径在 RHI 构造 native description时同步建立同类 plan，不需要先反序列化 artifact blob。

标准 D3D12 Root Signature 的表达力超过当前 RadRay parameter-set model。首期建议 fork 对不支持或
无法无歧义映射的合法 D3D12 形状 fail closed。当前 ordinary graphics/compute global RS 支持
descriptor tables、root CBV/SRV/UAV、RootConstants、static samplers、作者 parameter/range顺序、
visibility/flags、显式 offsets、1.1 flags，以及一个 active declaration按互不重叠 visibility映射到
多个 root locations的合法 fan-out；CPU plan把一次 binding value write展开到全部 destinations。
DXR Local RS 与 SM 6.6 directly-indexed heaps不属于这个 contract，分别等待 DXR/SBT 与 bindless RHI
专项设计。

状态：**已确认（2026-08-09）**；实现前仍需把上述边界转成逐项 validator/test cases。

### D8：artifact-local 去重层级

状态：**已确认（2026-08-09）**。

1. 每个 stage compile从现有 `DXC_OUT_ROOT_SIGNATURE` 取得 optional blob。
2. graphics中所有非空 outputs必须逐字节相同；hash只可作快速筛选，不能替代 byte comparison。lane
   merge只保存一份。compute最多保存自己的单份。
3. 不恢复历史 runtime `PipelineLayoutCache`，也不在 `DeviceD3D12` 新增 native RS cache。
4. 不做跨 artifact、Variant、package或Explicit/Implicit native object去重；这些上层策略由上层负责。
5. raw artifact wire不引入跨 artifact引用或package-level content table。

D8b 状态：**已确认（2026-08-09）**。最终 artifact对Explicit stage DXIL启用
`-Qstrip_rootsignature`，物理上只保留lane-level standalone range这一份 RS。实施时先保留 embedded
完成direct-consumption GPU parity gate，验证通过后再strip并原子断代bytecode/artifact goldens。

## Vulkan 与跨 target policy

- Implicit D3D12 Root Signature 只存在于 D3D12 runtime，不进入 SPIR-V artifact，也不参与 Vulkan
  pipeline layout 推导。
- 没有 Explicit RS 时，SPIR-V sampler 保持普通 dynamic sampler；不运行 static-sampler bridge。
- 有 Explicit RS 时，fork 可按 HLSL declaration identity 将 static sampler 关联到 SPIR-V binding，
  metadata 需要表达完整且可无损映射的 immutable sampler state。
- DXIL register/space 与 Vulkan binding/set 不要求相等；跨 target policy 只能按 declaration identity
  关联。
- RootConstants 与 SPIR-V push constant 保持 target-specific，不从任一 D3D12 RS 强造一一对应。

## 实施阶段与检查站

### M0：关闭设计决策并更新领域模型

1. 已关闭全部 D1-D8b；把已确认的 Explicit RS subset转成可测试 validator规则。
2. 用 Explicit DXIL Root Signature、Implicit D3D12 Root Signature 与 Root Signature source 修正
   `CONTEXT.md`，删除 Generated/Canonical DXIL Root Signature 误导性术语。
3. ADR-0035已接受，记录两条source的authority、optional artifact contract、hash与artifact-local
   coalescing边界，并澄清ADR-0016的exact projection表述。
4. 同步 shader pipeline、render RHI 与 shader authoring 架构契约。

检查站：TODO 不再假定 compiler-generated fallback；全部待确认决策关闭；用户明确确认达到共享理解。

### M1：fork 只发布 Optional Explicit Root Signature

1. stage compile 直接从现有 `IDxcResult::GetOutput(DXC_OUT_ROOT_SIGNATURE, ...)` 取得并保留 output
   bytes，不新增 RS parser、validator 或 serializer。
2. 实现 Variant-level Explicit/Implicit source resolution、graphics union validation 与非空 payload
   byte equality check。
3. Explicit 发布 serialized RS；Implicit 发布空 RS range。fork 中不加入 fallback generator。
4. malformed explicit、mode conflict 与 unsupported shape 保留稳定 diagnostics，batch 继续原子发布。
5. extension ABI、metadata、toolchain/package identity 按最终 wire shape 断代。

检查站：显式 descriptor table/root descriptor/RootConstants/static sampler fixtures 带 RS blob；无
attribute fixtures 的 RS range 为空但 active binding metadata 完整；fork 中不存在默认 RS 构造算法。

### M2：artifact wire、decoder 与 hashes

1. 增加 optional DXIL RS range；source由 range presence唯一决定，不增加 carrier digest字段。
2. decoder 校验 target、range、schema/toolchain、hash identity 和矛盾状态。
3. `DxilShaderArtifactView` 暴露 optional serialized RS；`SpirvShaderArtifactView` 拒绝非空 range。
4. 不 hash optional RS carrier container；把现有 compiler-owned `RootSignatureHash` 作为 Explicit
   layout record纳入 `PipelineLayoutHash`，并用 goldens证明 wrapper-only变化不改变 layout identity、
   RS policy变化会改变 layout/artifact identity。
5. pure-runtime fixture 在无 SDK/DXC 环境仍可 decode 两种 artifact。

检查站：Explicit/Implicit、corruption、旧 schema、target 错配和 source/range 矛盾全部有覆盖。

### M3：D3D12 双路径

1. Explicit 从 artifact blob 创建/deserialise RS并建立 CPU binding plan，不重建作者 RS。
2. Implicit 保留 RHI generator，并在构造 native description 时建立 plan；以当前 backend 行为为
   compatibility baseline。
3. 改造 descriptor writes、root descriptor、RootConstants 与 static sampler 提交映射，消除只适用于
   当前 Implicit parameter 排列的假设。
4. PSO 与 command list 使用同一 native object/plan；wrapper 与 `BindingHandle` generation 保持
   layout-local。

检查站：descriptor table、root CBV/SRV/UAV、多个 RootConstants、显式 offsets、1.1 flags、static
sampler、graphics/compute draw/dispatch/readback通过；Implicit 旧用例 layout parity 通过；本计划没有
新增 runtime/native RS cache。

### M4：Vulkan bridge、artifact 收尾与文档

1. 完整实现已确认的 Explicit static-sampler bridge，不让 Implicit D3D12 policy 污染 Vulkan layout。
2. 先用保留 embedded `RTS0` 的中间检查站完成direct-consumption GPU验证，再启用
   `-Qstrip_rootsignature`并原子断代 bytecode/artifact goldens；最终 artifact只留lane-level RS range。
3. 更新 SDK/package、goldens、compiler-free gates、shader authoring 与架构文档。
4. 正式 artifact publisher/index 与 package-level RS table 继续延期。

检查站：compiler/JIT/compiler-free、D3D12/Vulkan、SDK/package/docs gates 全部通过；Explicit 路径
不存在 backend RS 重建；Implicit 路径不存在 compiler RS generator。

## 测试矩阵

### Source 与 diagnostics

- graphics 全 stage 无 RS -> Implicit/空 range；一个 stage有 RS -> Explicit/非空 range；多个相同
  RS -> Explicit；多个不同 RS -> fail。
- compute 无/有 RS分别得到 Implicit/Explicit。
- malformed、资源不匹配、超限、unsupported shape 与 static sampler关联失败均 fail且不 fallback。
- no-resource Implicit 仍能由 RHI 生成有效 empty/default Root Signature。

### Artifact 与 backend

- Explicit blob逐字节持久化、decode、deserialize 并用于真实 PSO；Implicit range 必须为空。
- Implicit 的相同 active metadata 在迁移前后得到相同 decoded root layout与命令绑定语义。
- compiler-free runtime 可用 Explicit artifact直接创建 RS，也可用 Implicit metadata走 RHI generator。
- corrupted range、schema mismatch、source/range矛盾、SPIR-V 非空 RS range全部 fail closed。

### Artifact-local coalescing

- graphics多个非空 stage RS outputs逐字节相同则lane只保存一份；bytes不同即编译失败。
- hash collision但bytes不同不得合并；compute最多一份；Implicit/SPIR-V为零份。
- 不存在跨 artifact/package/native object cache的测试承诺。
- embedded 与未来 stripped DXIL都能用同一 standalone Explicit blob创建并执行 PSO。

### Cross target

- Implicit DXIL + 普通 SPIR-V descriptors；Explicit DXIL static sampler + Vulkan immutable sampler。
- target binding数字不同仍按 declaration identity关联。
- SPIR-V-only 且无 Explicit policy 时不执行无意义的辅助 DXIL Root Signature lane。

## 非目标

- 不在 DXC 中实现缺省 Root Signature 自动生成算法。
- 不从 shader访问频率猜 root descriptor、RootConstants 或 static sampler。
- 不把 serialized DXIL Root Signature直接作为 Vulkan pipeline layout。
- 不让 RHI 在 Explicit 路径根据 binding metadata 重建作者 RS。
- 不恢复历史 runtime `PipelineLayoutCache`，不新增 `DeviceD3D12` native RS cache，也不处理任何上层
  cross-artifact/package/native-object去重。
- 不在本阶段实现正式 artifact publisher/index、package-level RS table 或上层缓存共享。

## 对齐记录

### 已确认

- **2026-08-09 / C1**：HLSL `[RootSignature]` 可选；作者不写时必须存在自动生成 layout fallback。
- **2026-08-09 / C2**：先编写 TODO 并逐项 grilling 对齐；共享理解确认前不实施。
- **2026-08-09 / C3（已被 C7 取代）**：曾接受 compiler-owned fallback；本轮明确撤销，不再作为
  当前方案。
- **2026-08-09 / C4**：DXIL Root Signature source 属于整个 Variant。graphics 任一 entry携带显式
  RS 即进入 Explicit，其他 entry可不重复；多个非空 stage payload必须逐字节一致。全部 entries无
  RS 时进入 Implicit。
- **2026-08-09 / C5**：Implicit fallback 使用 descriptor tables；不推断 root descriptor、
  RootConstants 或 static sampler。
- **2026-08-09 / C6（撤回的问题）**：曾尝试另行定义 compiler-generated canonical topology；源码
  审计发现会改变当前 backend visibility/range行为。随着 C7 确认 RHI owner，该问题不再成立。
- **2026-08-09 / C7**：DXC 只在作者写了 RS 时发布 serialized RS；作者未写时 artifact RS range
  为空，D3D12 RHI 独立拥有自动生成算法。
- **2026-08-09 / C8（D5）**：一旦出现显式 `[RootSignature]`，错误内容必须编译失败；malformed、
  跨 stage冲突或 active resource不兼容都不得转入 RHI fallback。Explicit 编译/校验/序列化复用
  DXC 现有实现，RadRay extension 只接入 `DXC_OUT_ROOT_SIGNATURE`。
- **2026-08-09 / C9（D4）**：Explicit RS 允许作为跨 Variant stable superset。它必须覆盖全部 active
  resources，但可以保留当前 Variant未使用的合法 parameters/ranges/static samplers；compiler 与
  backend均原样保留作者 serialized blob，不做 exact-active projection。
- **2026-08-09 / C10（D6a）**：Explicit artifact 的 optional RS range 原样保存完整
  `DXC_OUT_ROOT_SIGNATURE` 独立 container；Implicit 与 SPIR-V range为空。wire 不增加重复的
  `RootSignatureSource` enum，source完全由 range presence确定。
- **2026-08-09 / C11（D6b）**：完整 `DXC_OUT_ROOT_SIGNATURE` carrier container不直接进入 compiler
  hash；复用 DXC 对 serialized `RTS0` payload计算的 `RootSignatureHash`，把 RS semantics作为
  Explicit layout record纳入 `PipelineLayoutHash`/`GpuArtifactHash`。曾提议的 runtime-only native
  cache digest由C13明确移出本次范围。
- **2026-08-09 / C12（D7）**：ordinary graphics/compute Explicit global RS支持合法 root binding
  fan-out；一个 `BindingHandle` 的 value write由 CPU plan展开到全部 visibility-disjoint destinations。
  DXR Local RS与SM 6.6 directly-indexed heaps不在当前范围，分别等待DXR/SBT与bindless专项设计。
- **2026-08-09 / C13（D8）**：去重只指同一 DXIL lane内相同 stage RS outputs合并为一个 optional
  artifact blob。历史 runtime `PipelineLayoutCache`不恢复，`DeviceD3D12`不新增native RS cache；跨
  artifact/Variant/package/native-object共享全由上层负责，不在本次范围。
- **2026-08-09 / C14（D8b）**：最终 Explicit artifact启用 `-Qstrip_rootsignature`，各 stage DXIL不再
  重复内嵌 `RTS0`，只保留lane-level standalone RS range。实施时先保留embedded完成GPU parity
  gate，再strip并断代goldens。
- **2026-08-09 / C15**：用户确认共享理解，允许将TODO标记为implementation-ready；随后明确要求
  开始实现。ADR-0035据此接受。

### 已确认的共享理解

- Explicit：复用 DXC现有 RS编译/校验/序列化，lane只发布一个完整
  `DXC_OUT_ROOT_SIGNATURE` range；错误fail closed；允许stable superset与合法binding fan-out。
- Implicit：range为空，DXC不生成fallback RS；D3D12 RHI保留当前自动生成算法。
- Final artifact：Explicit stage DXIL strip内嵌 `RTS0`，RS semantics通过compiler
  `RootSignatureHash`进入layout/artifact identity，carrier container本身不重复hash。
- 去重：只做lane内stage-output coalescing；所有runtime/native/package/cross-artifact共享均不在本次。
- Scope：ordinary graphics/compute global RS；Local RS与directly-indexed heaps分别留给DXR/bindless。
- Vulkan：继续使用target-specific metadata；只有Explicit policy参与declaration-identity bridge。

全部专项决策已关闭，用户已确认构成共享理解。当前实现进度：

- 已接入 schema v5 optional `RootSignature` carrier、旧 schema v4 decoder compatibility、DXIL
  `DXC_OUT_ROOT_SIGNATURE` coalescing、`-Qstrip_rootsignature` 与 explicit/implicit tests。
- 已接入 D3D12 explicit deserializer/CPU binding plan（descriptor tables、root descriptors、root
  constants、static samplers、visibility fan-out）和原有 implicit generator fallback。
- 已完成 compiler-free decoder、artifact hash 与 target-specific negative tests。
- 当前工作树已完成 Debug 全量 build 与 179/179 CTest（包含真实 D3D12 explicit layout 创建）；
  由于 `SDKs/` 是脚本填充的只读树，测试时使用 fork 新构建的 `dxcompiler.dll` 部署到 build
  output。fork Release 构建及可重定位 SDK 包
  完整性检查也已通过，验证包 SHA-256 为
  `bc38837fa8836ba0329b25181ce58a4e2d05276c3ec05bb94a054441f36bdfbf`。尚未更新
  `project_manifest.json` 或 `SDKs/`，因为该包尚未发布到 manifest 指向的远端 release；发布流程
  完成后再更新 manifest/hash，并在使用新 SDK 的干净环境执行全量 build/test 与真实 GPU
  explicit-PSO parity gate。
