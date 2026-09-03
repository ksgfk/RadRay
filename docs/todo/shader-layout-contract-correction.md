> - 适用: 把 `[RootSignature]` 变成可 lower 到 D3D12/Vulkan 的 base policy，移除 group-wide `ShaderLayoutPolicy`，修正 resolved layout、cache、sampler、handle 与 dynamic offset 全链路
> - 权威: 本文是 ADR-0051 的 implementation-ready 实施与验收计划；目标语义以 ADR-0051、`CONTEXT.md` 和本文的接口/wire 检查站为准
> - 状态: 已完成（2026-08-25 契约关闭；M1 compiler policy frontend 与 schema 6 wire 在 fork 侧完成并由 `utils/radray_probe_matrix.py` 验证 15/15；M2 decoder 与 target-typed resolver、M3 Vulkan native chain、M4 D3D12 native chain、M5 runtime request 与两层 cache、M6 原子发布全部落地；`1.9.2607.radray.5` 已发布，`project_manifest.json` 按 `EnforceHash` 固定已发布归档 hash，干净 restore 后 RadRay 全量 `ctest` 240/240 通过）
> - 锚点: `docs/adr/0051-root-signature-policy-and-target-layout-resolution.md`, `CONTEXT.md`, `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/include/radray/shader/shader_artifact.h`, `modules/render/include/radray/render/backend/pipeline_layout_types.h`, `modules/render/include/radray/render/rhi.h`, `modules/render/src/shader_artifact.cpp`, `modules/render/src/d3d12/d3d12_impl.cpp`, `modules/render/src/vk/vulkan_impl.cpp`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/src/shader_jit.cpp`, `project_manifest.json`

# Shader layout contract correction

## 完成定义

完成后，RadRay 不再让一个 group-wide policy 同时决定两 backend 的 buffer placement，也不让
runtime 承担 RootSignature policy 的跨 stage/target 解释。一个 concrete Variant 的链路固定为：

```text
HLSL declarations + optional [RootSignature]
  -> compiler-owned RootSignature policy frontend
  -> DXIL base artifact / SPIR-V base artifact
  -> target artifact decode
  -> target-typed resolve + exact modifiers
  -> ResolvedD3D12Layout / ResolvedVulkanLayout
  -> native layout + opaque binding metadata
```

DX 与 Vulkan 保留各自的数字、native topology、capability 和 public option types；共同层只保留
HLSL declaration identity、logical resource kind、base policy authority 和薄的 command operations。

## 已确认的现状与问题

### P1：program cache identity 不完整

`RenderSystem::ProgramKey` 当前只含 source name 与 assignments；`GetOrCreateShaderProgram` 虽接收
`CompilePolicy` 与 `ShaderLayoutPolicy`，两者没有进入 key。相同源码/assignment 的后续请求可能
错误复用第一次创建的 program。`ShaderJit` 的 runtime discovery 也没有传 compile request 的完整
`CompilePolicy`，使 discovery 与 compile 可以观察到不同的 shader model/HLSL version/policy。

修复要求：runtime 使用显式 `ShaderProgramRequest`；compiler input identity 至少覆盖 source input、
canonical assignments、完整 `CompilePolicy`、target 和 toolchain。layout recipe 不进入 compiler
artifact identity，但当前 backend resolve 后的 `ResolvedLayoutHash` 进入 program/layout identity。

### P2：Vulkan static sampler 有对象，缺完整语义

D3D12 Explicit 路径已经从 serialized Root Signature 保留并消费完整 native static sampler state，
不是缺陷。Vulkan 当前也会创建 `VkSampler`、把它作为 `pImmutableSamplers` 交给 descriptor set layout，
并保持 set-layout/native-layout 生命周期；问题发生得更早：schema 5 wire 只有 immutable bit，decoder
只能构造默认 `SamplerDescriptor`，所以作者的 filter/address/LOD/compare 等 state 已在进入 Vulkan
backend 前丢失。

修复要求：schema 6 增加 SPIR-V/Vulkan-specific fixed-width immutable sampler record，覆盖 filter、
address U/V/W、mipmap/LOD range、LOD bias、anisotropy、compare、border color 与 reduction mode。
record 不能持久化 `VkSamplerCreateInfo` 或 `pNext`。resolver 从 canonical record生成 sampler recipe；
native backend再构造 Vulkan structs。device不支持时返回 capability diagnostic/failure，不静默改 state；
pipeline可用 sampler replacement modifier显式替换。
这条 wire/public option 都保持 Vulkan target-typed，不扩张公共 `SamplerDescriptor` 来复制 D3 static
sampler contract。

### P3：wire 把不同 buffer kind 合并，placement 又反写 logical type

当前 compiler wire把 typed `Buffer<T>/RWBuffer<T>` 与 structured/raw buffer压成同类 code，Vulkan
因而可能把本应是 texel-buffer descriptor 的资源当成 storage buffer。decode阶段又把 group policy
选中的 `CBuffer` 改成 `DynamicCBuffer`，把 native placement伪装成逻辑资源类别。

修复要求：schema 6 明确区分 cbuffer、typed read/write buffer、structured read/write buffer、raw
read/write buffer、sampled/storage texture 与 sampler；resolved D3D12/Vulkan records另存 placement。
既有 `ShaderParameterBindingType::Dynamic*` enum identifiers保留但新 artifact path不生产。

### P4：pure push/root constants 没有完整 handle 路径

当前 name table只从 ordinary bindings生成，root/push record不带可供 `FindBinding` 使用的 declaration
identity；`SetPushConstants` 还要求 caller 再传 group。pure push shader因而不能走与 descriptor
一致的 lookup/ownership 契约。

修复要求：compiler为 root/push record保留 canonical declaration name/identity；resolved layout为它
建立 push metadata table record；`FindBinding(name)`返回普通 opaque handle；调用改为
`SetPushConstants(BindingHandle, bytes)`。当前合法 producer 的 root/push offset均为 0，这不是需要
单独修复的问题；公共写入刻意只支持从 0 开始的 prefix。

### P5：D3 Explicit root descriptor 没有消费公共 dynamic offset

D3D12 Explicit CPU plan能识别 root CBV/SRV/UAV，但当前命令路径直接提交 set中记录的 base GPU VA；
只有 logical type被改成 `Dynamic*` 的旧 implicit路径才遍历 `dynamicOffsets`。因此 authored root
descriptor虽然 layout可创建，却不能完整复用 ADR-0049 的 per-draw offset data path。

修复要求：dynamic offset由 resolved native destination而非 `Dynamic*` enum触发。一个
`BindingHandle + Offset` 在 D3D12 对所有 root destinations提交 `base GPU VA + offset`，在 Vulkan
按 resolved dynamic descriptor order打包。visibility-disjoint D3 fan-out仍由同一个 handle驱动。

### 已撤回的“问题”

- **D3 static sampler 不可用**：不成立；Explicit serialized RS 已保留、反序列化并原样创建它。
- **需要新增 backend 交叉校验/恢复路径**：不成立；selector、kind、handle、offset等矛盾是 framework
  invariant，Debug assert即可，不增加生产恢复 API。
- **`BindingHandle` 缺 group**：不成立；它本来就是 handle，不是公开地址。正确修复是进一步隐藏
  generation/namespace/index，由 layout metadata table解释。
- **`WireRootConstantRecord::Offset` 当前错误**：不成立；所有合法 producer都发射 0，新公共 push API
  也只承诺 prefix write。
- **Vulkan `VkPipelineLayout` 没有把 immutable sampler接上**：不成立；native链路已接上。真正缺陷是
  schema 5 丢失 sampler state，以及 dynamic offsets仍以裸 binding number与临时顺序关联。

## 最终 contract

### 1. compiler policy frontend

每个 concrete Variant恰好执行一次 compiler-owned RootSignature policy frontend。输入必须与所有
requested lanes使用相同的 root source、ordered include paths、structured `Defines`、canonical
assignments和完整 `CompilePolicy`。frontend负责：

1. 解析 graphics/compute Variant 的 RootSignature source并在 compiler内合并 stage facts；
2. 以 DX register/space定位 policy parameter，再以 canonical declaration identity关联到当前 target
   的 active declaration；
3. 检查标准D3 active-resource coverage，并检查已关联Vulkan declaration的policy visibility覆盖actual
   active stages；target-only declaration保持标准Vulkan attribute语义；
4. 为 DXIL发布 serialized carrier，为SPIR-V发布 Vulkan-specific base records；
5. 保证 runtime/cook不执行额外 DXIL分析、stage link或 declaration pairing。

普通资源需要 `register` + `VK_BINDING`。push declaration需要 `register` + `VK_PUSH_CONSTANT`，不得
同时写 `VK_BINDING`；DXC 对 push+binding 的拒绝保持为 authoring diagnostic。跨 target数字可不同。

有 `[RootSignature]` 时按 ADR-0051 映射表 lower。Vulkan只发布当前 Variant active declarations及
actual stage flags；能关联policy的declaration应用映射，target-only declaration继续使用标准
Vulkan attribute语义；D3 serialized RS继续允许stable superset。没有 `[RootSignature]` 时不发布公共
base policy：D3 artifact保持空 carrier并走 implicit tables，SPIR-V保持普通 descriptors。

### 2. schema 6 wire

schema 6 原子替换 schema 5，不做 4/5 compatibility decode。extension interface/vtable不变，所以
extension ABI保持 3；toolchain/package identity升级到 `1.9.2607.radray.5`，manifest、SDK hash和全部
raw goldens同批切换。

DXIL payload：

- active logical declaration records保留准确 kind；
- optional serialized Root Signature carrier继续作为D3唯一explicit topology权威；
- root constants/declaration identity足以建立push handles；
- `BasePipelineLayoutHash`覆盖carrier semantics与active logical facts。

SPIR-V payload：

- descriptor records区分 uniform、typed texel、structured/raw storage、texture/image和sampler；
- base descriptor placement包含 ordinary/dynamic，但不借用公共 `Dynamic*` logical kind；
- immutable sampler引用完整 fixed-width sampler state record；
- push block记录canonical declaration identity、size和actual active stages；
- `BasePipelineLayoutHash`覆盖上述canonical Vulkan base semantics。

full sampler record 与 replacement option 使用 Vulkan-semantic target type；D3 full static sampler
仍只以 serialized Root Signature 为权威，不新增公共 expanded `SamplerDescriptor`。

所有 record继续使用offset/index和固定宽度值，不持久化STL/native structs/pointers。compiler-owned
hash命名从“最终 layout”窄化为base layout；modifier与native capability不进入它。

M1 落地时补齐的契约细节（已由 probe 矩阵验证）：

- `Placement` 是一个独立字段（0 `Table`、1 `RootDescriptor`、2 `StaticSampler`），取代旧的 immutable
  bit。D3D12 的 static sampler 记为 `StaticSampler` 且不占 table slot；Vulkan 的 immutable sampler 记为
  `Table` 加 `SamplerIndex`，两者是同一份 policy 在两个 target 的不同落法。`SamplerIndex` 无值时为
  `0xffffffff`。
- root constants 由 policy 推导，不由 liveness 推导。policy 中每个对该 stage 可见、且能匹配到 CBuffer
  declaration 的 `RootConstants` parameter，在两条 lane 上发布完全相同的记录（`Size` =
  `num32BitConstants * 4`），避免一侧死代码剥除导致两 lane 记录不一致。root constants declaration 在两条
  lane 都不产生 binding record：`Bindings` 只放 descriptor，push handle 走 root constant record 加 name。
- sampler record 只在 SPIR-V lane 发布，按 RS static sampler 槽位顺序排列（与 stage 无关，可直接跨 stage
  比对），且不论 liveness 全量发布以保证确定性。serialized RS carrier 只在 DXIL lane 发布。
- `RootSignatureHash` 在两条 lane 都取 frontend 原始 serialized bytes 的摘要，因此它标识 policy 本身而不是
  某个 target 的 carrier 形态，可用于跨 lane 一致性检查。
- DXIL carrier 的忠实性不变量：`DXC_OUT_ROOT_SIGNATURE` 是包裹 raw serialized RS 的 DXBC container。
  编译器解开 container 的 `RTS0` part 与 frontend bytes 逐字节比对，只有一致才把 **container** 存为 wire
  carrier（保持历史上 D3D12 `CreateRootSignature` 消费的形态）。
- `BasePipelineLayoutDigest` 的 salt 含 target，所以它是 per-target 的确定性摘要，不是跨 lane 相等性检查。
- descriptor/push 的 `StageMask` 始终是实际 active stage；policy visibility 只用来校验覆盖（不覆盖报
  2123），不会把 stable superset 原样传播到 wire。

### 3. public target options 与 selector

目标 public shape：

```text
ShaderProgramLayoutRecipe
  D3D12TargetLayoutOptions
    D3D12BufferPlacementModifier[]
  VulkanTargetLayoutOptions
    VulkanBufferDescriptorModifier[]
    VulkanImmutableSamplerModifier[]

ShaderLayoutSelector
  DeclarationName
  ExpectedLogicalResourceKind
```

具体命名可按 C++ convention微调，但不能退回 target-erased modifier variant或 group集合。两 backend
options并列存在是为了让同一个 pipeline配方显式表达差异；D3运行时只读取/哈希D3字段，Vulkan同理。

D3 buffer placement modifier只对Implicit source生效，目标declaration必须count=1且原生规则允许
root descriptor。Vulkan buffer modifier只在 uniform/storage Regular<->Dynamic之间切换并保持count；
sampler modifier为selected sampler指定完整immutable state，base已有时整体替换。modifier list在
resolve前按selector canonicalize，因此输入
顺序不改变resolved hash；重复selector不是“last wins”，而是invariant violation。

### 4. resolved target layouts

`ResolvedD3D12Layout`与`ResolvedVulkanLayout`必须是public owning value：可复制/移动策略按实现成本
选择，但其比较与hash不依赖borrowed span、artifact地址、allocation地址或native handle。它们是
native layout创建的唯一输入。

`ResolvedD3D12Layout`至少拥有：

- Explicit carrier或Implicit canonical root topology（二者互斥）；
- 每个active declaration的logical kind与Table/RootDescriptor destinations；
- root constants/push metadata与visibility-disjoint fan-out；
- D3 static sampler semantics；
- canonical `ResolvedLayoutHash`。

`ResolvedVulkanLayout`至少拥有：

- 按set/binding排序的active descriptor declarations与Regular/Dynamic placement；
- 从set 0到max active set的完整set-layout序列，空洞用有效empty layout recipe占位；
- 完整immutable sampler recipes及其descriptor引用；
- 唯一active logical push block的range与actual stage flags；
- Vulkan规范要求的dynamic offset packing order（set、binding、array element）；
- name到内部metadata table record的映射与canonical `ResolvedLayoutHash`。

native Vulkan创建顺序为 sampler objects -> descriptor set layouts -> `VkPipelineLayout`；owning backend
layout保持sampler/set-layout引用至少覆盖pipeline layout与parameter sets的使用期。hash只吃sampler
semantics，不吃`VkSampler` handles。descriptor limits、dynamic-buffer limits、push size/alignment、
sampler feature/extension与native create result在这里形成明确 diagnostic/failure。

### 5. handle 与 command semantics

`BindingHandle`公共API只保留default-invalid、validity和equality；不公开group/binding/namespace/index，
也不允许caller工厂构造。内部token由layout generation/namespace/table index组成，但其位布局不是ABI。

metadata table record为两类：

- Descriptor：logical kind、group/binding、array facts，以及D3/Vulkan的一个或多个native destinations；
- Push：declaration name、resolved size/stages、D3 root destinations和/或Vulkan push ranges。

`ShaderParameterDynamicOffset`变成 `{ BindingHandle Binding; uint32_t Offset; }`。offset必须对应当前
bound layout、当前group中的dynamic/root destination，并满足backend alignment；错误handle、重复/缺失
offset、错误group和未对齐值都是framework invariant。Vulkan以resolved order打包所有必需offset，
不依赖caller输入顺序；D3把同一offset应用到handle的全部root destinations。

`SetPushConstants(BindingHandle, bytes)`不再接收group。写入范围恒为`[0, bytes.size())`，size非零、
不超过resolved block且4-byte aligned；block剩余内容不变。一个D3 declaration可fan-out到多个
RootConstants destinations；Vulkan整个Variant最多一个active logicalpush block。

### 6. runtime request 与 cache

目标入口：

```text
GetOrCreateShaderProgram(const ShaderProgramRequest&)
```

request显式拥有source compile input（logical source与structured `Defines`）、canonicalizable
assignments、完整 `CompilePolicy` 与layout recipe。runtime discovery和compile从同一request构造
输入；不得让discovery使用默认policy而compile使用caller policy。

缓存分两层理解并测试：

- compiler artifact key：source input/identity与structured `Defines`、canonical assignments、完整compile policy、target、
  toolchain；不含layout recipe；
- program/layout key：artifact identity + 当前backend canonical `ResolvedLayoutHash`；不含非当前backend
  options、modifier原始顺序或native handles。

失败缓存也必须按完整对应key隔离。继续由 `ShaderProgram` 拥有自己的PSO map；不增加跨program、
跨device或全局native layout cache。

## 实施阶段与检查站

### M0：契约与历史决策对齐

- [x] 接受ADR-0051，部分取代ADR-0035/0049并更新ADR索引。
- [x] 更新`CONTEXT.md`、authoring、shader pipeline、shaderlib、render RHI和历史计划的supersession说明。
- [x] 明确P1-P5、撤回的非问题、错误边界、schema/package cutover与non-goals。

检查站：全仓库当前契约不再把`[RootSignature]`称为DXIL-only policy，也不把group-wide
`ShaderLayoutPolicy`当成目标设计；历史ADR/research只通过status/后续说明保留原始记录。

### M1：compiler policy frontend 与 schema 6

状态：已完成（fork 侧）。fork 工作区为外部 checkout `F:\cpp\DirectXShaderCompiler`（非本仓库跟踪，
不受本仓库可移植性检查约束）。

1. 在fork中建立target-independent RootSignature policy frontend，复用现有AST/RS parser与Variant
   merge，不新增runtime link。
2. 增加declaration identity关联和RootSignature->Vulkan lowering；保留DX carrier输出。
3. 拆分logical buffer kinds，增加push declaration identity和full immutable sampler record。
4. 将layout hash语义固定为`BasePipelineLayoutHash`，升级schema/toolchain/package constants。
5. 增加稳定diagnostic：policy未覆盖、无法关联、非法root buffer、多个active Vulkan push blocks、
   push同时写`VK_BINDING`等。

检查站：同一concrete inputs的policy frontend和各lane事实一致；Root CBV/SRV/UAV、RootConstants、
StaticSampler逐项生成预期Vulkan records；无RS请求没有隐藏DX lane或公共policy。

fork 原先没有 RadRay 自有测试（只有 `dxcradray.cpp` 与 ext header），M1 因此在 fork 内补了一套
编译器侧回归载体，不依赖 RadRay checkout：

- `utils/radray_wire_probe.cpp`：直接驱动 `DiscoverSourceContract` + `CompileVariant` 的独立 probe，
  解码并打印两条 lane 的 schema 6 wire。第三个参数是 target mask（1 DXIL、2 SPIR-V、3 both），
  用于隔离只在单 target 出现的诊断。
- `utils/radray_probe_tests/*.hlsl`：正负 fixture。
- `utils/radray_probe_matrix.py`：用 vcvars + `cl` 构建 probe 并跑完整 fixture 矩阵，逐条断言
  placement、logical kind、count、stage mask、sampler state 与 diagnostic code；payload 尺寸在比对前
  归一化掉，所以无关 codegen 变化不会误报。fixture 没有对应期望时直接失败。

M1 验证结果：`dxcompiler` 构建干净，probe 矩阵 15/15 通过，覆盖完整 placement 矩阵、无 policy 的
implicit topology（含 `RawBuffer`/`RWTypedBuffer` 分类）、D3D→Vulkan sampler 翻译（含
`COMPARISON_ANISOTROPIC` 与 `MINIMUM` reduction）、compute stage 折叠，以及 2105/2111/2117/2118/
2120/2121/2122/2123/2124。2119 是 declaration kind 与 lane lowering 的防御性交叉校验，合法 HLSL 无法
触发，不造 fixture。

RadRay 侧的 M1 验收与 M2 decoder 同批生效：schema/ABI handshake 两边必须一致，schema 6 的 DLL 会被
现有 schema 5 client 直接拒绝。本仓库的 M2-M5 不等待官方包，`modules/render/tests` 已经在测试内手造
`WireMetadataEnvelope` 字节，schema 6 fixture 同样手造；需要真实编译器的 `modules/shader_compiler/tests`
靠本地 staging 包（本地构建产物 + 临时 `project_manifest.json` hash）推进，正式 URL/hash 在 M6 一次性
固定，不得把 staging hash 提交到主线 manifest。

### M2：decoder 与 target-typed resolver

1. schema 6 decoder拒绝4/5，分别暴露DXIL/SPIR-V typed views。
2. 删除decode阶段group-wide dynamic rewrite；logical kind原样保存。
3. 定义public owning resolved layout与target-specific options/selector。
4. 实现modifier canonicalization、resolved hash和layout-local metadata table。
5. selector/handle/recipe矛盾报告并fail closed；wire安全同样fail closed。

检查站：相同resolved semantics由不同modifier顺序得到相同hash；非当前backend recipe字段不影响
program identity；artifact bytes/lifetime结束后owning resolved value仍不悬空。

M2 落地时补齐的实施细节：

1. resolve 失败只 log + 返回 `std::nullopt`，不 `RADRAY_ASSERT(false)`。同一条路径也校验 decode
   后的 wire 数据，abort 会把被篡改的 artifact 变成进程终止；且 fail-closed 行为必须在实际跑测试
   的 Debug 配置里可观察。
2. `BackendPipelineLayoutInput` 作为显式 adapter 保留一个 milestone：它把 logical kind 与 resolved
   placement 融合回 `ShaderParameterBindingType::Dynamic*`，让两个 backend 在 M3/M4 重接之前保持
   可用。融合是 adapter 自身的属性，resolved layout 里两者仍分开。
3. Vulkan immutable sampler 的 native 创建从 M3 提前到 M2：把完整 wire state 降级成公共
   `SamplerDescriptor` 是被禁止的静默降级，而让 layout 创建失败等于留洞。`VkSampler` 由
   `PipelineLayoutVulkan` 拥有，vector 在 group loop 之前 reserve，因为
   `VkDescriptorSetLayoutBinding::pImmutableSamplers` 借用其中的指针。
4. `ShaderProgram::IsBufferGroupDynamic(group)` 变为 `IsBufferDynamic(declarationName)`；forward
   pipeline 的三个 group 变成 `ForwardPipeline::GetLayoutRecipe()` 里三个具名 declaration modifier。
5. `modules/runtime/src/shader_parameters.cpp` 原先按裸 wire 数值 1/4/6 和已废弃的 immutable bit
   判定 parameter kind，M2 改为按 `ShaderBindingKind` 判定，并把 policy 已固定的 sampler
   （DXIL `StaticSampler` placement 或 SPIR-V 已带 `SamplerIndex`）排除在 material parameter 之外。
6. fixture/test 消费顺序按 C11：本地 staging package（`package_radray_sdk.py --manifest`）驱动
   `radray_shader_fixture_generator` 重新生成 `shader_artifacts/*.bin` golden 与
   `kExpectedGpuArtifacts`；staging hash 不进入正式 manifest，正式 URL/hash 在 M6 一次性写入。
7. policy 必须写在每个 entry 上（见 `docs/guide/shader-authoring.md`）。只写在部分 entry 上时，缺少
   attribute 的 stage 编出不含 serialized RS 的 DXIL，与 frontend 观察到的 policy 不一致，以 2106
   失败。M2 按此修正了 `modules/render/tests/data/shader_sources/` 与
   `test_radray_dxc_metadata.cpp` 中的相关 fixture。
8. `RootConstants` policy parameter 只有在能关联到一条真实 declaration 时才发布 push handle，因此
   只写 policy、不写对应 `ConstantBuffer` 声明的 fixture 会得到 0 条 root constant record。
9. cross-stage DXIL register drift 重新纳入检查：fork 的 `MetadataBindingFact` 恢复记录 declaration
   的 D3 register（不上 wire），两个 lane 都记录，merge 时比较，漂移报 2109。否则 SPIR-V-only 编译
   会静默接受一个 register 随 stage 变化的 declaration。

### M3：Vulkan native chain

1. 只从`ResolvedVulkanLayout`建立set entries、empty set holes、dynamic order、push range与name table。
2. 从full sampler record/override创建immutable samplers，并保证引用生命周期。
3. 正确区分uniform/storage/texel/image descriptor type，保留array count与actual stage flags。
4. `vkCmdBindDescriptorSets`按resolved order打包handle offsets；不再匹配裸binding number。
5. 覆盖limits/features/extensions/native create diagnostics；禁止sampler silent downgrade。

检查站：Vk pipeline layout create info中的set layouts、push ranges、immutable sampler pointers与resolved
value逐项一致；regular/dynamic切换、sparse groups、typed/structured/raw buffers和sampler state均通过真实
Vulkan创建及dispatch/draw/readback。

M3 落地时补齐的实施细节：

1. `DeviceVulkan::CreatePipelineLayoutInternal` 改为直接接受 `ResolvedVulkanLayout`，Vulkan 侧的
   `MakeBackendPipelineLayoutInput` overload 一并删除，因此 Vulkan layout 只剩一种描述方式。D3D12
   仍走过渡 adapter，M4 一并去掉。
2. backend 内部 entry 换成 `ShaderParameterSetLayoutEntryVulkan`：保留 logical kind 与 placement 两个
   独立轴，`VkDescriptorType` 由二者推导。融合成单一枚举会让 uniform/storage、texel/image 的区分靠
   命名巧合成立，而一个非法 dynamic placement 会被静默改判成邻近的 descriptor type。
3. value 兼容性、required buffer usage、alignment limit 与 view usage 全部由 logical kind 推导
   （`IsUniformBufferKind`/`IsWritableKind`/`IsTexelBufferKind`/`IsImageKind` 落在 contract header 里，
   因为它们是 wire kind 自身的性质）。
4. resolved bindings 已按 (set, binding) 排序且去重，backend 只确认该 invariant，不再自己重排，否则
   backend 会在 resolver 背后改变顺序。
5. immutable sampler 按 resolved 数组下标一次性创建并与之对齐，`_immutableSamplers` 先 reserve 再填，
   因为 `VkDescriptorSetLayoutBinding::pImmutableSamplers` 只借用其中的指针。设备能力
   （`samplerAnisotropy`、`maxSamplerAnisotropy`、`maxSamplerLodBias`、`samplerFilterMinmax`、
   `samplerMirrorClampToEdge`）在这里检查，因为 resolved layout 与设备无关。
6. `PipelineLayoutVulkan::_dynamicEntryOrder` 由 `ResolvedVulkanLayout::DynamicOffsetOrder` 投影到每个
   set 得到，不再重新推导；`vkCmdBindDescriptorSets` 按该顺序为每个 slot 反查 caller 值，缺失或重复
   一律失败。旧实现遍历 entries 挑 dynamic 项并忽略缺失值，会静默移位。
7. wire sampler enum 边界改为 contract header 里的具名常量，radrayrender 用 `static_assert` 把它们和
   volk 的真实枚举值绑在一起；wire stage bit 与 `ShaderStage` 的对应也同样 static_assert。
8. 新增 `modules/render/tests/test_radray_render_vulkan_layout.cpp`（真 Vulkan device，3 个测试）：
   descriptor type 按 logical kind 区分 + set hole 保号 + dynamic order 投影 + name table；非法 dynamic
   placement 与不一致的 dynamic order 一律 fail closed；`shadow_static_sampler` 的 policy sampler 在
   set 4 落成 immutable sampler，set 0..3 是空洞，且该 slot 不能被 caller 写入。已用 mutation 验证
   dynamic order 相关断言非空泛。

### M4：D3D12 resolved layout 与 offset path

1. Explicit carrier原样进入resolved value/native create，CPU plan只生成destinations不重写blob。
2. Implicit generator接收精确buffer placement modifiers，非目标declaration保持table。
3. root CBV/SRV/UAV command bind统一从handle destination取base GPU VA并加offset，覆盖fan-out。
4. root constants进入push handle table；static sampler路径保持现有parity。

检查站：Explicit/Implicit table、root descriptors、fan-out、多个RootConstants/static sampler均通过真实
D3 PSO/draw/dispatch；同一dynamic-CB arena offset在Implicit modifier和Explicit authored root CBV上
得到相同数据结果。

M4 落地时补齐的实施细节：

1. `DeviceD3D12::CreateRootSignatureInternal` / `CreateExplicitRootSignatureInternal` 改为直接接受
   `ResolvedD3D12Layout`，两条路径共用同一份 group 构建；`MakeBackendPipelineLayoutInput`、
   `BackendPipelineLayoutInput`、`PipelineLayoutDescriptor`、`ShaderParameterSetLayoutDescriptor`、
   `ShaderParameterSetLayoutEntryDescriptor`、`PushConstantDescriptor` 与
   `GetShaderBindingNamespace(ShaderParameterBindingType)` 一并删除，公共面上的
   `ShaderParameterBindingType` 与 `IsDynamicShaderParameterBindingType` 也随之删除。
2. backend 内部 entry 换成 `ShaderParameterSetLayoutEntryD3D12`：logical kind、placement 与 register
   class（namespace）分开存放，descriptor range type、root parameter type、required buffer usage 与
   value 兼容性全部由 logical kind 推导，placement 只决定它落在 table 还是 root parameter。唯一读
   placement 的校验是 table CBV 的 view size 上限，root CBV 没有这个限制。
3. group 0..max 全部物化，空 group 保留，否则 caller 的 group index 不再等于 shader 编译时的
   register space。resolved bindings 已按 (group, namespace, binding) 排序去重，backend 只确认该
   invariant；每个查找都以 register class 为 key，旧的按裸 binding number 做 `lower_bound` 在一个
   group 同时有 b0/t0/u0 时会取错项。
4. static sampler 现在保留在 `Entries` 里（旧 adapter 会丢掉它们），所以 explicit carrier 的
   coverage 检查是可达的：覆盖来自 carrier 的 static sampler 数组而非 table/root destination，
   parameter set 也不能写它。Implicit builder 直接拒绝 static sampler，因为它只能来自 carrier。
5. root descriptor 统一：`ShaderParameterBindingLayoutD3D12::RootParameterIndex` 删除，Implicit
   builder 与 authored carrier 都记 `RootDescriptorDestinations`，command 期只有一个循环按
   `RootDescriptorOrder` 绑定。地址 = buffer GPU VA + bound range offset + dynamic offset，两种
   topology 一致；旧实现里 explicit 路径完全忽略 dynamic offset，Implicit 路径走另一条没有 fan-out
   的单 index 路径。
6. dynamic offset 与 Vulkan 同样严格：一个 group 里每个 root descriptor 恰好一个 offset，缺失、
   重复或多余都报告失败；另外校验位移后的窗口仍在资源内，constant buffer 还要求 256-byte 对齐。
7. push handle table 补齐：push block 的 declaration name 进入两个后端的 name 表，
   `FindBinding("ObjectConstants")` 现在能拿到可用于 `SetPushConstants` 的 handle——这在两个后端上
   之前都不成立。
8. 新增 `modules/render/tests/test_radray_render_d3d12_layout.cpp`（真 D3D12 device，debug layer，
   5 个测试）：table 与 root descriptor 由 placement modifier 决定；policy static sampler 来自
   carrier 且不可写；authored root constants 全部进入 push handle table（并有 carrier 未覆盖时
   fail closed 的反例）；非法 placement 与乱序 binding 一律 fail closed；同一个 256 字节 arena
   offset 在 Implicit modifier 与测试用 `D3D12SerializeVersionedRootSignature` 现场 author 的
   root UAV carrier 上 dispatch 后读回相同数据，且 offset 0 处未被写。已用 mutation 验证非空泛。

已知限制（M5 处理）：`BindShaderParameterSet` 返回 void，因此 dynamic offset 的 arity 失败在公共
API 上不可观测（Vulkan 自 M3 起同样如此）；`ShaderParameterDynamicOffset` 仍是裸 binding number，
同一 group 里两个不同 register class 的 root descriptor 撞号时无法区分。

### M5：runtime request、program cache 与调用迁移

1. 引入`ShaderProgramRequest`和side-by-side layout recipe，迁移pipeline调用。
2. discovery/compile共享完整policy/defines/source inputs。
3. 修复artifact/program/failure cache keys；增加active-backend hash规则。
4. 迁移`BindingHandle`公共面、dynamic offset和push API；更新parameter storage/forward executor。
5. 保留ADR-0049 arena、每(layout,flight) set和per-draw仅offset的数据路径。

检查站：只改变policy、target、toolchain或active-backend modifier会按规则命中/分离cache；改变非当前
backend recipe不重复编译/建program；per-draw不新增descriptor write或set allocation。

M5 落地时补齐的实施细节：

1. `RenderSystem::GetOrCreateShaderProgram(const ShaderProgramRequest&)` 是唯一入口，request 拥有
   source name、结构化 defines、keyword assignments、完整 `CompilePolicy` 与 side-by-side layout
   recipe；旧的 (sourceName, assignments, recipe, policy) 重载删除，`test_forward_pipeline` 与
   `example_lambert_sphere` 一起迁移。
2. 缓存分两层。artifact key = source + canonical defines + canonical assignments + 完整 policy +
   target + toolchain identity（不含 layout recipe）；program key = artifact identity + 当前 backend
   的 `ResolvedLayoutHash`。defines/assignments 排序后入 key，所以 caller 顺序不是身份；重名直接
   报错。policy 按原始字节参与 hash，后续加字段不会被漏掉。
3. 失败改成显式失败记录，按同一完整 key 隔离。旧实现在编译前先插入一个空 program 且失败时不删，
   既分不清"还没编译"和"编译失败"，也让一次失败永久污染这个 key。
4. toolchain identity 从 `IRadRayDxcCompiler::GetAbiInfo` 读出，经 `Client::GetToolchainIdentity`
   与 `ShaderJit::GetToolchainIdentity` 暴露，因此它在任何编译之前就可以进入 key。
5. discovery 与 compile 现在共享同一份 source/defines/policy：新增
   `ShaderJit::DiscoverContractHash(const shader::SourceContractRequest&)`，旧的三参重载保留为
   default-policy 便利入口。此前 discovery 固定用 default policy 且丢掉 defines。
6. 新增 `render::ResolveBackendLayoutHash(backend, blob, options, recipe, error)`：只 decode + resolve，
   不创建任何 native 对象，这样 program key 可以在 pipeline layout 存在之前算出来，而不是为了读
   hash 每次多建一个 native layout。
7. `BindingHandle` 公共面收缩为 default-invalid/validity/equality；内部 token 变成 layout generation
   加该 layout metadata table 的 record index（原来是 generation + namespace + 裸 binding number），
   只有后端通过 `BindingHandleAccess` 拆开。record 带 `Descriptor`/`Push` kind，因此 push handle 不能
   写 parameter set、descriptor handle 不能写 push，错 group 的 handle 也会被拒绝。
8. `ShaderParameterDynamicOffset` 变成 `{BindingHandle, Offset}`，两个后端都先把 caller 的 offset 经
   destination layout 的 record 表解析（拒绝 foreign generation 与错 group），再按 resolved order 反查。
   `SetPushConstants(BindingHandle, bytes)` 去掉 group；D3D12 用完整 `ShaderBindingLocation` 匹配
   `_pushConstantBindings`，所以不同 register space 的同号 RootConstants 仍可区分。
9. handle 校验分支里的 `RADRAY_ASSERT` 删除（与 M2 `Reject()` 同一理由）：Debug abort 会让 fail-closed
   路径在唯一会跑测试的配置里无法验证，返回值加日志才是可观测契约。`test_radray_render_pso_smoke`
   里原本的 `EXPECT_DEATH` 改为 `EXPECT_FALSE`。
10. `ShaderProgram::Create` 去掉未使用的 `layoutRecipe` 参数——artifact 已经带着 resolved layout。

检查站结果：`RadRayRuntimeForwardPipeline.{D3D12,Vulkan}` 内新增 cache 规则检查并通过：同一 request
命中；modifier 顺序颠倒仍命中；改非当前 backend recipe 仍命中且 artifact/program 数都还是 1；改当前
backend recipe 复用 artifact 但新建 program（1 artifact / 2 programs）；`ShaderModel` 61 分离 artifact；
不存在的 source 连续两次失败只留一条失败记录且不影响好的 key；重复 assignment 报错且不入缓存。
两次 mutation 验证非空泛：program key 去掉 layout hash、artifact key 去掉 policy 各自让对应断言变红。
新增 `D3D12DeviceFixture.PushHandleWritesRootConstantsAndRejectsMisuse` 用真实 dispatch 跑通
name -> handle -> root parameter，并钉住五种误用拒绝；去掉 generation 比较会让它变红。

### M6：原子发布与回归

1. fork package发布为`1.9.2607.radray.5`，更新`project_manifest.json` URL/hash并restore。
2. 原子替换schema 6 DXIL/SPIR-V bytecode/metadata goldens；明确4/5 negative fixtures。
3. 更新SDK handshake tests、compiler-free decoder/runtime tests与raw compile CLI预期。
4. 完成Debug全量build/CTest和真实D3D12/Vulkan GPU parity gates。
5. 实现落地后移除架构文档中的“迁移待实现”提示，保持本文作为完成记录。

检查站：干净restore环境只使用manifest固定SDK即可通过全部build/test；runtime-only进程不加载compiler；
旧schema不会被误读，新artifact在两个backend均按resolved contract执行。

M6 落地时补齐的实施细节：

1. fork 侧先落一次提交（`276ef523`：跨 stage register drift 诊断恢复、`RADRAY_DXC_PACKAGE_VERSION`
   提升到 `.5`、probe matrix 覆盖新用例、删除 `.build_probe.bat`），再由 `ksgfk/dxc-autobuild` 的
   `build_win_x64.yaml` 以 `dxc_ref=codex/radray-dxc-1.9.2607`、`package_version=1.9.2607.radray.5`
   构建并发布到 rolling `latest` release。sidecar manifest 记录的 `dxc_commit` 就是这次提交。
2. `project_manifest.json` 的 hash 换成已发布归档的
   `c2d023fc4103451fe5b5beb84ab999012f6dbef7652c9596418f52f8459b5009`，之前用于本地验证的 staging
   hash 没有进入任何提交。清掉 `SDKs/radray_dxc` 的 `.done`、`extracted` 与本地 staged zip 之后
   `python tools/fetch_sdks.py restore` 真的从 manifest URL 下载并在 `EnforceHash` 下校验通过。
3. goldens 在装上 CI package 之后重新生成：12 个 DXIL blob 全部改变、12 个 SPIR-V blob 逐字节不变
   （DXIL 容器随 compiler 二进制变化，SPIR-V 不随），`kExpectedGpuArtifacts` 整块重写；toolchain
   identity 仍是 `0x0000000001090211`，说明它绑的是版本而不是构建。
4. 4/5 拒绝、ABI 3 handshake 与 raw compile CLI 都已经有覆盖，不需要新增：
   `test_radray_shader_contract` 把 retired 4、retired 5 与 schema+1 都断言成
   `UnsupportedSchemaVersion`；`client.cpp` 的 `IsValidForkAbi` 同时校验 ABI 3、schema 6、toolchain
   1.9 与非零 identity，`test_radray_shader_compiler_client` 通过真实 DLL 断言同一组事实；
   `radray_shader_compile` 对 `pipelines/forward/forward.hlsl` 双 lane 输出正常。
5. 补上 Vulkan push 端到端测试 `VulkanDeviceFixture.PushHandleWritesPushConstantsAndRejectsMisuse`，
   在 `compute` fixture 上加一个 push block，再加一个 set 0 binding 0 的未使用 descriptor——它和 push
   的 (space 0, register 0) 撞位，因此拒绝只能来自 record kind 而不是 location 不匹配。mutation 验证：
   去掉 `Kind != Push` 判断后这条断言变红。

检查站结果：干净 restore 只用 manifest 固定的 SDK，全量 Debug build rc 0 且无 `error` 行，
`ctest` 240/240 通过（含真实 D3D12 与 Vulkan 设备测试，validation/debug layer 打开）。

## 测试矩阵

| 维度 | 必测事实 |
|---|---|
| RootSignature source | graphics/compute有无RS；stable superset；policy未覆盖；跨stage冲突 |
| Vulkan lowering | table->regular；root CBV->dynamic uniform；root SRV/UAV->dynamic storage；constants->push；static sampler->full immutable state |
| target binding | DX register与VK binding数字不同仍按declaration identity关联；push没有VK binding |
| resource kind | typed、structured、raw、uniform、texture/image、sampler不互相折叠 |
| modifier | 精确selector；D3 implicit table/root；Vk regular/dynamic；Vk sampler replacement；非法组合assert fixture |
| resolved hash | input顺序不敏感；active backend敏感；非active backend不敏感；不含native handles |
| Vulkan chain | sparse sets、descriptor arrays、dynamic order、actual stage flags、push size、immutable sampler lifetime/capability |
| D3 chain | explicit carrier parity、implicit generation、root fan-out、full static sampler、root GPU VA + offset |
| handle | descriptor/push name lookup、跨layout、错误group、duplicate/missing offset、prefix push write |
| cache | fullCompilePolicy、target、toolchain、source/assignment、failure isolation、recipe artifact exclusion |
| cutover | schema6成功；4/5拒绝；ABI3 handshake；`.radray.5` clean restore |

framework invariant tests可以使用death/assert形式，不要求生产API返回可恢复错误。compiler authoring
diagnostics、device capability与native create failures必须保留可定位的信息。

## 非目标

- 不支持DXR Local Root Signature或directly-indexed heaps。
- 不支持一个Vulkan Variant多个active logical push blocks，也不增加任意push destination offset。
- 不新增第二套HLSL metadata DSL，不移除`VK_BINDING`/`VK_PUSH_CONSTANT` target gate。
- 不要求DX register/space与Vulkan set/binding相同，不把两套native layout压成公共结构。
- 不为RootSignature的D3-only parameter order/range offsets/flags制造虚假的Vulkan modifier。
- 不增加全局native layout/cache，也不把layout recipe加入compiler artifact identity。
- 不增加backend恢复校验、selector忽略或sampler silent downgrade。
- 不改写既有`ShaderParameterBindingType::Dynamic*`枚举标识符。**落地偏离**：M4 删掉整个
  `ShaderParameterBindingType` 枚举而不是只停止生产它。它把 logical kind 和 placement 融在一个
  值里，而 M4 的整个 D3D12 adapter 层（`BackendPipelineLayoutInput` 家族）连同它一起消失，留下一个
  没有任何生产者也没有任何消费者的公共枚举，只会让 caller 以为还存在第二条 binding 描述路径。
  没有成员被重命名，因此 AGENTS.md 的"不重命名枚举成员"约束没有被破坏。
- 不改用per-object StructuredBuffer；dynamic constant-buffer arena/data path继续保留。

## 对齐记录

- **2026-08-25 / C1**：`[RootSignature]`应直接成为可lower到Vulkan layout DSL/records的base policy，
  但不抹平两target数字与native差异。
- **2026-08-25 / C2**：无RS时不禁止dynamic cbuffer；两backend使用各自默认layout，pipeline可施加
  精确target modifier。
- **2026-08-25 / C3**：modifier作用域必须是canonical declaration + expected kind，且每类slot只能
  接受列举的target-specific修改。
- **2026-08-25 / C4**：D3 static sampler不是问题；Vulkan问题是wire只携带immutable bit而丢完整state。
- **2026-08-25 / C5**：selector/handle/offset矛盾属于framework invariant，不扩张backend验证API；
  `BindingHandle`只是opaque handle，group缺失不是问题。
- **2026-08-25 / C6**：确认修复program cache/policy identity、buffer kind、push handle和D3 explicit
  root-descriptor offset；`WireRootConstantRecord::Offset == 0`不是缺陷。
- **2026-08-25 / C7**：schema6拒绝4/5，SDK package升级`.radray.5`，extension ABI保持3。
- **2026-08-25 / C8**：dynamic/root placement 的 declaration count 恒为 1。public
  `ShaderParameterDynamicOffset{BindingHandle, Offset}` 没有 array-element 维度，所以 count>1 落到
  dynamic/root 目标是 framework invariant violation，不为此新增 `ArrayElement` 字段。
- **2026-08-25 / C9**：buffer kind 只增成员、不改名。typed 复用 `TexelBuffer`/`RWTexelBuffer`，structured
  复用 `Buffer`/`RWBuffer`（语义收窄），raw 新增 `RawBuffer`/`RWRawBuffer`。enum 成员名是
  magic_enum/序列化的 public identity，改名等于破坏兼容。D3D12 的 raw-vs-structured view 形态本来由 caller
  的 `StructureByteStride` 决定，logical kind 只驱动 Debug 校验与 root descriptor 合法性。
- **2026-08-25 / C10**：artifact cache 是新增工作，不是修 bug。runtime 现在只有 program map 与 PSO map，
  需要新增 artifact 层，并把 sticky negative cache（编译前插 null、失败不 erase）换成显式 failure record。
- **2026-08-25 / C11**：消费顺序。M2-M5 用手造 schema 6 wire fixture 推进；需要真实编译器的测试用
  `package_radray_sdk.py --manifest` 产生的本地 staging 包，正式 URL/hash 在 M6 一次性固定，staging hash
  不进主线 manifest。

上述边界已构成共享理解；M1（fork 侧）已按此实现并验证，见「实施阶段与检查站」。

## 2026-09-03：schema 7 declaration owner 后续

schema 6 的 GPU layout 修正完成记录保持不变；本次后续只修 CPU payload 所有权和参数寻址：

1. RadRay DXC fork 提交 `8a5cf97cab3728ab2c5272d36e272e130a1c0543` 把 extension ABI 升为 4、
   metadata schema 升为 7、toolchain identity 升为 `0x0000000001090212`。44-byte
   `WireBindingRecord` 与 36-byte `WireRootConstantRecord` 追加 lane-local payload `TypeIndex`；
   declaration owner 不进入 GPU layout/artifact hash。fork Release build 与 17/17 probe matrix 通过。
2. 按发布要求直接把本地验证过的 package 上传到 `ksgfk/dxc-autobuild` rolling release；不使用远端
   CI 重新打包，已取消 run `33770586624`。正式 asset 为
   `dxc-windows-x64-1.9.2607.radray.6.zip`，SHA-256 为
   `92db0ac511a124d019697ce72284f69b192dfa9c79dbff2c6cf315e6f1e8c613`；相邻 provenance manifest
   固定上述 fork commit。`project_manifest.json` 使用同一 version/hash，clean forced restore 成功。
3. decoder 在 type tree 校验之后 fail closed 校验 owner：CBuffer 必须指向顶层 `Struct`，非
   CBuffer 必须为 sentinel；concrete root-constant owner 还受 payload size 约束。runtime 删除
   unreferenced-root/发射位置配对，只跟随 binding owner。canonical CPU 参数名改为
   `Binding.Member.Path`；唯一 leaf 可作简写，重复 leaf 只令简写 ambiguous，exact resource name
   优先。push-only artifact 保持合法空 CPU parameter layout。
4. 正式 package 重新生成 14 个 fixture 的 DXIL/SPIR-V 两条 lane，共 28 个 raw artifact；
   shared payload 与 direct+nested root fixture 钉住多 declaration 共用 root、owned root 同时被
   嵌套引用和双 target program creation。Debug 全量 build 通过；changed-contract suites 93/93、
   compiler-free runtime suites 36/36、全量 CTest 246/246 通过。runtime-only build tree 不含
   compiler target/binary，CMake cache 不含 RadRay DXC package 路径。
   D3D12 与启用 validation layer 的 Vulkan declaration-owner program creation 均在 NVIDIA
   GeForce RTX 4080 上实际执行并通过，没有走 device-unavailable skip。
