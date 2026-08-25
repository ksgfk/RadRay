> - 适用: 把 `[RootSignature]` 变成可 lower 到 D3D12/Vulkan 的 base policy，移除 group-wide `ShaderLayoutPolicy`，修正 resolved layout、cache、sampler、handle 与 dynamic offset 全链路
> - 权威: 本文是 ADR-0051 的 implementation-ready 实施与验收计划；目标语义以 ADR-0051、`CONTEXT.md` 和本文的接口/wire 检查站为准
> - 状态: implementation-ready（2026-08-25；契约已关闭，尚未开始 compiler、wire、RHI 或 runtime 实现）
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

1. 在fork中建立target-independent RootSignature policy frontend，复用现有AST/RS parser与Variant
   merge，不新增runtime link。
2. 增加declaration identity关联和RootSignature->Vulkan lowering；保留DX carrier输出。
3. 拆分logical buffer kinds，增加push declaration identity和full immutable sampler record。
4. 将layout hash语义固定为`BasePipelineLayoutHash`，升级schema/toolchain/package constants。
5. 增加稳定diagnostic：policy未覆盖、无法关联、非法root buffer、多个active Vulkan push blocks、
   push同时写`VK_BINDING`等。

检查站：同一concrete inputs的policy frontend和各lane事实一致；Root CBV/SRV/UAV、RootConstants、
StaticSampler逐项生成预期Vulkan records；无RS请求没有隐藏DX lane或公共policy。

### M2：decoder 与 target-typed resolver

1. schema 6 decoder拒绝4/5，分别暴露DXIL/SPIR-V typed views。
2. 删除decode阶段group-wide dynamic rewrite；logical kind原样保存。
3. 定义public owning resolved layout与target-specific options/selector。
4. 实现modifier canonicalization、resolved hash和layout-local metadata table。
5. selector/handle/recipe矛盾使用debug invariant；wire安全仍fail closed。

检查站：相同resolved semantics由不同modifier顺序得到相同hash；非当前backend recipe字段不影响
program identity；artifact bytes/lifetime结束后owning resolved value仍不悬空。

### M3：Vulkan native chain

1. 只从`ResolvedVulkanLayout`建立set entries、empty set holes、dynamic order、push range与name table。
2. 从full sampler record/override创建immutable samplers，并保证引用生命周期。
3. 正确区分uniform/storage/texel/image descriptor type，保留array count与actual stage flags。
4. `vkCmdBindDescriptorSets`按resolved order打包handle offsets；不再匹配裸binding number。
5. 覆盖limits/features/extensions/native create diagnostics；禁止sampler silent downgrade。

检查站：Vk pipeline layout create info中的set layouts、push ranges、immutable sampler pointers与resolved
value逐项一致；regular/dynamic切换、sparse groups、typed/structured/raw buffers和sampler state均通过真实
Vulkan创建及dispatch/draw/readback。

### M4：D3D12 resolved layout 与 offset path

1. Explicit carrier原样进入resolved value/native create，CPU plan只生成destinations不重写blob。
2. Implicit generator接收精确buffer placement modifiers，非目标declaration保持table。
3. root CBV/SRV/UAV command bind统一从handle destination取base GPU VA并加offset，覆盖fan-out。
4. root constants进入push handle table；static sampler路径保持现有parity。

检查站：Explicit/Implicit table、root descriptors、fan-out、多个RootConstants/static sampler均通过真实
D3 PSO/draw/dispatch；同一dynamic-CB arena offset在Implicit modifier和Explicit authored root CBV上
得到相同数据结果。

### M5：runtime request、program cache 与调用迁移

1. 引入`ShaderProgramRequest`和side-by-side layout recipe，迁移pipeline调用。
2. discovery/compile共享完整policy/defines/source inputs。
3. 修复artifact/program/failure cache keys；增加active-backend hash规则。
4. 迁移`BindingHandle`公共面、dynamic offset和push API；更新parameter storage/forward executor。
5. 保留ADR-0049 arena、每(layout,flight) set和per-draw仅offset的数据路径。

检查站：只改变policy、target、toolchain或active-backend modifier会按规则命中/分离cache；改变非当前
backend recipe不重复编译/建program；per-draw不新增descriptor write或set allocation。

### M6：原子发布与回归

1. fork package发布为`1.9.2607.radray.5`，更新`project_manifest.json` URL/hash并restore。
2. 原子替换schema 6 DXIL/SPIR-V bytecode/metadata goldens；明确4/5 negative fixtures。
3. 更新SDK handshake tests、compiler-free decoder/runtime tests与raw compile CLI预期。
4. 完成Debug全量build/CTest和真实D3D12/Vulkan GPU parity gates。
5. 实现落地后移除架构文档中的“迁移待实现”提示，保持本文作为完成记录。

检查站：干净restore环境只使用manifest固定SDK即可通过全部build/test；runtime-only进程不加载compiler；
旧schema不会被误读，新artifact在两个backend均按resolved contract执行。

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
- 不删除或改写既有`ShaderParameterBindingType::Dynamic*`枚举标识符。
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

上述边界已构成共享理解，可以按M1开始实现；本文建立本身不授权开始代码改动。
