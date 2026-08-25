# ADR-0035 Optional Explicit DXIL Root Signature 与 D3D12 RHI fallback

状态: 部分被 ADR-0051 取代
日期: 2026-08
影响: RadRay DXC fork、shader artifact wire 与 identity、`radrayshader` decoder、D3D12 pipeline layout、SPIR-V policy bridge、shader authoring 与测试

## 背景

ADR-0016 确定 HLSL 与 forked RadRay DXC 是 shader 权威，并允许 standard serialized Root Signature
成为 DXIL artifact leaf。当前 fork已经从最终 DXIL module读取、校验和摘要化
`[RootSignature]`，DXC本身也已经通过 `DXC_OUT_ROOT_SIGNATURE` 产生包含 `RTS0` 的独立
serialized container；但是 RadRay extension只保留扁平 binding/root-constant/static-sampler facts，
没有把现成 blob带入 artifact。D3D12 backend随后按固定 descriptor-table规则重建 Root Signature，
因此会丢失作者选择的 root descriptor、RootConstants、parameter/range顺序、visibility、flags与完整
static sampler state。

同时，RadRay不能要求所有 HLSL作者都声明 `[RootSignature]`。现有 shader与普通 authoring路径需要在
没有显式 RS时继续根据active binding metadata自动建立D3D12 layout，但这个fallback不应被迁入DXC，
也不能用来掩盖错误的显式 RS。graphics多stage、Variant stable-superset、artifact内重复、hash边界、
CPU binding plan与Vulkan bridge因此必须作为一组决策关闭。

## 决策

### 1. Root Signature source 与 authority

`[RootSignature]`可选。一个DXIL Variant只有两种互斥source：

- **Explicit**：graphics任一entry拥有显式RS即选择Explicit；其他entry可以省略attribute，所有非空
  stage outputs必须逐字节相同。compute按自己的entry选择。fork复用DXC已有编译、validation、
  serialization与`DXC_OUT_ROOT_SIGNATURE` output，不新增RS parser、serializer或fallback generator。
- **Implicit**：所有相关entries都没有`[RootSignature]`时选择Implicit。artifact的RS range为空，
  compiler只发布active binding metadata；D3D12 RHI保留并拥有现有自动生成算法。

一旦选择Explicit，malformed、跨stage冲突、没有覆盖active resource或其他compiler contract错误
都必须编译失败，不得转入Implicit fallback；D3D12 backend不支持的合法RS形状在explicit layout
创建时fail closed，也不得转入Implicit fallback。Explicit RS可以是跨Variant稳定的declared superset：
必须覆盖全部active resources，但允许保留当前Variant未使用的合法parameters、ranges与static
samplers；compiler与backend都不得投影、裁剪或重写作者blob。

### 2. Artifact carrier、identity 与局部合并

DXIL wire增加一个optional Root Signature range。Explicit原样保存完整
`DXC_OUT_ROOT_SIGNATURE`独立container；Implicit与SPIR-V range为空。range presence唯一决定source，
不增加重复的source enum。

graphics lane merge对所有非空stage RS outputs做逐字节比较，并且只发布其中一份；hash只可快速筛选，
不能替代byte comparison。compute最多发布一份。本决策的“去重”仅指这种artifact-local
coalescing；不恢复历史runtime `PipelineLayoutCache`，不在`DeviceD3D12`增加native RS cache，也不
处理跨artifact、Variant、package或native object共享。

完整carrier container bytes不直接进入compiler hash。fork复用DXC从serialized `RTS0` payload计算的
`RootSignatureHash`，把Explicit RS semantics作为layout record纳入`PipelineLayoutHash`与
`GpuArtifactHash`。Implicit layout identity只覆盖active binding metadata并使用独立domain。hash是
compiler output identity，不是artifact integrity校验。

最终Explicit artifact对stage DXIL启用`-Qstrip_rootsignature`，只保留lane-level standalone range
这一份RS。实施必须先保留embedded `RTS0`完成direct-consumption真实GPU parity gate，再strip并
原子断代bytecode/artifact goldens。

### 3. D3D12 consumption 与支持范围

D3D12 Explicit路径直接用artifact blob创建native Root Signature，并通过versioned Root Signature
deserializer只读解析同一blob，与active binding metadata匹配后建立CPU binding plan。backend不得从
扁平records重建作者RS。Implicit路径继续从active binding metadata生成RS，并在同一构造过程建立
对应plan。

ordinary graphics/compute global RS支持descriptor tables、root CBV/SRV/UAV、RootConstants、static
samplers、作者parameter/range顺序、visibility/flags、显式offsets与1.1 flags。一个active declaration
可以按互不重叠的shader visibility映射到多个合法root locations；artifact-local `BindingHandle`仍表示
一个declaration，CPU plan把一次value write展开到全部destinations。visibility重叠的register overlap
继续由DXC validation拒绝。

DXR Local RS与SM 6.6 directly-indexed descriptor heaps不属于当前contract。前者等待DXR state
object、shader record与SBT设计；后者等待跨后端bindless heap/index生命周期设计。

### 4. Vulkan 与跨target边界

DXIL Root Signature不成为Vulkan pipeline layout wire。SPIR-V继续使用`vk::binding`、
`vk::push_constant`与target-specific compiler metadata。只有Explicit RS policy需要按HLSL declaration
identity把static sampler关联到SPIR-V immutable sampler metadata；两target的binding数字不要求相等。
Implicit D3D12 fallback不运行static-sampler bridge，不推导Vulkan layout，也不成为SPIR-V-only请求的
隐藏DXIL依赖。

## 放弃的方案及代价

- **强制每个shader写`[RootSignature]`**：artifact始终自带RS，但破坏现有authoring与自动绑定路径。
- **由forked DXC生成缺省RS**：能让两种source都带blob，但把backend fallback policy塞入compiler，
  与已确认的RHI owner冲突，并要求迁移、版本化和长期维护另一套布局算法。
- **D3D12 backend重建Explicit RS**：可以继续使用扁平records，却会丢失residency、顺序、offset、flags、
  visibility与完整sampler state，形成作者policy的第二权威。
- **把错误Explicit RS当成缺失并fallback**：提高表面容错，但会静默忽略作者policy，错误难以定位。
- **把Explicit RS投影为exact active layout或拒绝inactive parameters**：减少每个Variant的参数，却破坏
  stable-superset共享并改写作者blob。选择原样保留superset的代价是单个Variant可能携带未使用参数。
- **hash完整standalone carrier**：实现直接，但wrapper/checksum变化会无意义地改变layout identity；
  选择hash serialized payload semantics需要继续维护compiler-owned `RootSignatureHash`。
- **最终保留每个stage内嵌`RTS0`**：便于单独取出stage bytecode，却在graphics artifact中重复同一RS。
  选择strip要求PSO始终显式使用standalone range，并增加真实GPU gate。
- **本次同时增加runtime/native/package去重**：可能进一步减少object与存储重复，但跨越本次artifact
  cutover边界，且会重新引入owner、lifetime与artifact-local plan共享问题。
- **立即支持Local RS与directly-indexed heaps**：能覆盖更多D3D12形状，但分别需要尚不存在的DXR与
  bindless跨后端架构。

## 必须保持为真

- fork中不存在Implicit RS generator；没有显式RS时DXIL artifact range为空且active binding metadata完整。
- Explicit路径只接入DXC已有`DXC_OUT_ROOT_SIGNATURE`；错误显式RS fail closed且永不fallback。
- graphics所有非空stage RS outputs逐字节相同，lane只发布一份；compute最多一份；SPIR-V永远为空。
- Explicit stable superset原样保留并完整覆盖active resources；compiler和backend都不做exact projection。
- final Explicit stage DXIL不含embedded `RTS0`；standalone range是artifact内唯一RS copy。
- `RootSignatureHash`覆盖serialized payload semantics；完整carrier bytes不直接进入compiler hash。
- D3D12 Explicit路径从同一blob创建RS并建立CPU plan，不根据binding records重建作者RS。
- 合法visibility-disjoint fan-out由一次binding write驱动全部destinations；Local RS与directly-indexed heaps
  fail closed。
- 本决策不增加任何runtime/native/package/cross-artifact RS cache。
- Vulkan layout继续target-specific；Implicit D3D12 policy不污染SPIR-V，Explicit static sampler只按
  declaration identity桥接。

本决策在DXIL Root Signature具体边界上澄清ADR-0016；两者冲突时以本决策为准，ADR-0016其余内容
继续生效。
