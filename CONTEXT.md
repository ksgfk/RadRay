# RadRay

C++20 实时渲染器，D3D12 + Vulkan 双后端，纯光栅。

本文件是**领域词汇表**：定义术语「是什么」。不描述实现、不记录决策、不当规格书用。
实现现状见 `docs/architecture/`，决策见 `docs/adr/`。

## Shader 管线

**Pass**:
属于同一 shader 家族的一组 stage。一次绘制所需的完整 shader 程序；同一 Pass 的不同 Variant
可以拥有不同的 active binding layout。
_Avoid_: program, effect, technique

**Pass source unit**:
一份入口 `.hlsl` 文件定义且只定义一个 Pass；共享代码放在 `.hlsli`。stage entry functions
使用标准 HLSL `[shader("...")]` attribute 声明，由 compiler 自动发现；不新增 RadRay stage
attribute，也不依赖 entry function 命名。CompileVariantRequest 不再携带作者维护的 stage/entry
列表。

**Pass asset identity**:
`PassName` 与 `AssetId` 属于 caller/资产系统，不属于 shader compiler contract。RadRay DXC
extension request/result、compiler metadata 与 artifact identity 均不包含 `PassName`；compiler
只接收用于 include、诊断与 source identity 的 `SourceName`。cook/资产层在自己生成的索引中把
外部 asset/pass identity 映射到内容寻址的 compiler artifact，多个资产 Pass 可以复用同一产物。

**Pass entry cardinality**:
graphics Pass 恰好包含一个 `[shader("vertex")]` entry，并可包含至多一个
`[shader("pixel")]` entry；缺少 pixel 的 depth-only Pass 合法。compute Pass 恰好包含一个
`[shader("compute")]` entry。一个 source unit 不得混合 graphics 与 compute，同一 stage 不得
声明多个 entry；当前不接受其他 stage。

**Stable entry topology**:
一个 Pass 的 entry names、stages 和 graphics/compute kind 在整个合法 keyword domain 内保持
不变；带 `[shader("...")]` 的声明不得受 Variant 条件编译控制。keyword 可以改变函数体、资源
使用、类型字段和 stage interface，但 compiler 必须对每个具体 Variant 校验 stage interface。
pixel entry 可以永久缺省，不能只在部分 Variant 中存在。

**Stage**:
Pass 中的一个可编程管线阶段（vertex / pixel / compute）。
_Avoid_: 单独使用的 "shader"（过于笼统，可指 stage、pass 或字节码）

**Variant**:
同一个 Pass 的一组具体 keyword assignment，是 target-independent 的逻辑身份。每个 target
各自产生一个 Compiled Variant artifact 及其实际需要的 binding layout。
_Avoid_: permutation, configuration, 变种

**Compiled Variant artifact**:
一个 logical Variant 针对一个 target category 的物理编译产物。身份至少包含 keyword
assignment、DXIL/SPIR-V target 与 compiler/toolchain identity；DXIL 与 SPIR-V 可以拥有不同的
active binding 集合和 layout identity。

**DXIL loadability gate**:
每个 pinned RadRay DXC SDK build 必须用其默认 compiler output 通过真实 D3D12 pipeline-state
创建与执行 smoke test，证明交付的 DXIL 可被目标运行时加载。external `dxil` validator 不是
compile path 的固有依赖；只有某个 toolchain build 无法通过此门槛时才需要它。
_Avoid_: external-validator presence check, nonzero-hash-only check

**Variant batch request**:
compiler-level request 携带一个确定的 keyword assignment 与 target mask。一次 request 可以同时
输出 DXIL 和 SPIR-V 及各自 metadata；每个 target lane 仍独立预处理、codegen、optimization
和 validation。AOT cook 通常请求双 target，runtime JIT 只请求当前 target。请求按 target
原子成功：任一被请求 lane 失败，整个 batch result 的 status 为 failed，所有 target lane 都不可访问，
不产生 publication/persisted blob；diagnostics 可以保留各 lane 的失败信息，但成功 lane 也不得单独交付。

**RadRay DXC extension ABI**:
fork extension 只在独立的 `dxcapi_radrayext.h` 中声明，使用 RadRay-owned
`CLSID_RadRayDxcCompiler`、`IRadRayDxcCompiler` 与 `IRadRayDxcResult`，并通过 upstream
`DxcCreateInstance` / `DxcCreateInstance2` 创建。upstream `dxcapi.h`、现有 CLSID/IID、interface
vtable 与 DLL exports 保持不变；stock DXC 对扩展 CLSID 返回不支持，RadRay 必须 fail closed。
extension interface 直接继承 `IUnknown`，不继承或扩展 `IDxcCompiler3`。

**RadRay DXC SDK distribution**:
RadRay 主 CMake 工程只消费独立流水线产出的、版本与内容 hash 固定的预编译 RadRay DXC SDK；
不通过 `add_subdirectory`、`FetchContent` 或 `ExternalProject` 构建 DXC 源码。SDK 必须自带
CMake config package 与 imported targets，集中表达 headers、compiler/validator binaries、
import libraries 和可部署 runtime files；RadRay 不再根据 `SDKs/dxc/v...` 目录结构手工拼接路径。
SDK package 必须支持按组件消费和部署裁剪，为未来不携带 shader compiler 的纯运行时发布构建
铺路。`dxcompiler` 属于 compiler component；external `dxil` validator 是可选 component，不构成
DXIL compile path 的无条件依赖。每个 SDK build 通过 DXIL loadability gate 证明默认产物可用。
package identity 固定为 `RadRayDXC`，不冒充 stock `dxc`；CMake namespace 为 `RadRayDXC::`。
它提供独立的 `Headers`、`Compiler`、`Validator` 与 `CLI` imported targets：`Compiler` 依赖
`Headers`，`Validator` 依赖 `Headers`，`CLI` 依赖 `Compiler`，不提供隐式引入全部组件的 umbrella。
正式构建只从 `project_manifest.json` 固定版本、triplet 与 archive hash 的 SDK prefix 发现 package，
禁止隐式回退到系统或 package manager 中的其他 DXC。fork 开发可以显式设置
`RADRAY_DXC_SDK_ROOT` 使用未发布 package，但 CI、release 与正式 cook 禁止 override；严格纯运行时
配置完全不发现 `RadRayDXC`。
SDK canonical identity 使用完整 upstream version 加 `+radray.<release>` suffix，例如
`1.9.2607+radray.1`。RadRay 的 `find_package(RadRayDXC CONFIG REQUIRED ...)` 不指定 version，
也不依赖 CMake 的 numeric ConfigVersion comparison；config package 导出完整 identity，正式的
manifest prefix 由 RadRay 直接做字符串相等检查，开发 override 则由显式 override policy 放行。

**Pure shader runtime distribution**:
严格 compiler-free 的 AOT-only 构建与发布形态。配置阶段不发现或导入 RadRay DXC SDK；构建图
不包含 DXC client、shader compilation、reflection 或 SPIRV-Cross 路径；发布包不包含 DXC
headers、CLI、import libraries、`dxcompiler`/`dxil` binaries、HLSL source。它只消费 cook/publisher
预先交付的 compiler-owned bytecode、metadata wire 与 generated artifact index，并保留 artifact
decoder、layout 构造和 backend consumption。artifact 缺失、损坏、schema/toolchain 不兼容或没有
请求的 Variant/target 时 fail closed，不允许 JIT fallback。该边界必须由 build graph 和 package
components 兑现，不能只靠部署阶段漏拷 DLL。

**Shader compiler client**:
可选的 compiler-facing adapter，只负责把 RadRay 的 source contract discovery 与 Variant compile
请求映射到 RadRay DXC extension ABI，并把 compiler-owned result 原样交还调用方。它不拥有 shader
runtime representation、RHI layout、资产身份、Variant coverage、artifact 发布或 AOT/JIT 策略。
它动态加载 `RadRayDXC::Compiler` 指向的 binary，只把 `RadRayDXC::Headers` 作为 C++ compile
dependency，不通过 import library 建立进程启动时的 loader dependency。
client 不接收或固定 compiler path，而是使用 canonical platform library name 并服从平台动态库
搜索顺序；加载到 stock 或不兼容 fork 时，由 extension CLSID、ABI 与 toolchain identity 检查
fail closed，不允许回退到 upstream compile API。
compiler capability 开启时，CMake 自动把 `RadRayDXC::Compiler` runtime artifact 放入 RadRay 公共
build output，并通过 `install(IMPORTED_RUNTIME_ARTIFACTS ...)` 安装到 `ShaderCompiler` component；
不要求每个 executable 单独声明部署。compiler capability 关闭时不存在对应 build/install rule。
`RADRAY_BUILD_SHADER_COMPILER` 独立控制该 client 与 SDK discovery；runtime JIT 和未来 shader tools
是两个分别选择、但都要求 compiler client 的能力；`RADRAY_BUILD_RENDER` 不依赖 shader compiler。
旧 `RADRAY_BUILD_SHADER`、`RADRAY_ENABLE_DXC`、`RADRAY_ENABLE_SPIRV_CROSS` 不再存在，
`shader_gen` 随手写 JSON authoring route 一起移除。
_Avoid_: shader subsystem, shader runtime, cook

**Shader runtime representation**:
已编译 bytecode、compiler metadata wire 的 decoder，以及由其表达的 target-native binding/layout、
vertex interface 和 buffer type tree。它属于 render 领域，不依赖 compiler client，也不包含资产查找
或 Variant 选择策略。
_Avoid_: compiler reflection, shader compiler model

**Shader artifact orchestration**:
资产身份到 compiler artifact 的映射、Variant assignment 选择、artifact 加载与缓存，以及 AOT/JIT
可用性策略。它属于 runtime/资产领域；需要开发期 JIT 时可以调用 Shader compiler client，但不拥有
compiler ABI 或 render 的 target-native representation。
_Avoid_: shader compilation, RHI layout construction

**RadRay DXC structured result**:
`IRadRayDxcResult` 是独立于 `IDxcResult`、`DXC_OUT_KIND` 和 `IDxcExtraOutputs` 的结构化 result
ABI。它表达 batch status、common diagnostics/Variant identity，以及各 target lane 的 metadata、
projected Root Signature 和各 stage bytecode/debug outputs。upstream COM blob 类型可以作为叶子
数据载体，但 upstream result API 不构成 RadRay extension 的公开协议。

**Compiler-owned metadata wire**:
每个 target lane 由 forked DXC 直接输出彼此独立的 bytecode blob 与 canonical、versioned metadata
blob；metadata 不嵌入 DXIL/SPIR-V。持久化 wire bytes 是 compiler 产物本身，上层不得从
structured view 二次序列化。JIT 使用同一 decoder 直接消费 metadata blob；未来的 artifact
publisher 只做完整性检查、内容寻址与原样发布，不解释或重建 metadata。

**Target-specific metadata wire**:
DXIL 与 SPIR-V metadata blob 只共享包含 magic、schema/target/toolchain/Variant identity、payload
范围和完整性信息的薄 envelope，payload schema 完全分离且不要求字段同构。DXIL payload 原样
复用标准 serialized Root Signature；其他 DX facts 与所有 Vulkan facts 使用 RadRay-owned、
固定宽度的 target-specific offset/index records。不得把含 pointer、platform ABI 或 `pNext` chain
的 `D3D12_*` / `Vk*` 内存结构直接持久化；decoder 负责为当前 backend 组装临时 native structs。

**HLSL source truth**:
HLSL 是唯一 shader authoring 源真相，forked DXC 是唯一编译与 metadata 权威。不保留 C++
trace、LuisaCompute 或其他 shader authoring/codegen 路线。作者维护的 shader metadata 必须
位于 HLSL；不再存在作者手写的 `.shader.json` 或其他并行 metadata 文件。

**Generated artifact index**:
compiler/cook 为查找 compiled Variant artifacts 生成的索引。它可以采用 JSON 或其他序列化
格式，但不能由作者编辑，也不能成为 shader contract 的第二份真相。

**Binding layout**:
一个 Compiled Variant artifact 需要哪些资源、各自落在哪个 target-native group 与 slot 上的
完整描述。由编译器生成并随产物交付，是该 target binding ABI 的唯一权威；不使用的资源不占
active layout 槽位。对于 graphics Variant，layout 是所有 active stage binding 的并集；每个
entry 记录自己的 stage visibility。stage-specific projection 只服务编译与缓存，不产生独立的
RHI ABI layout。
_Avoid_: pipeline layout, root signature, descriptor set layout（这三个是后端说法）；
Property（这是 codegen 内部的表示）；binding ABI（指同一事物的契约面，不指数据本身）

**Layout identity**:
描述 binding layout 形状的身份。它区分资源集合、类型、槽位与 stage 可见性；不同 Variant
只有在 layout identity 相同的时候才能共享 layout。
_Avoid_: shader hash（shader 产物身份，不等于 layout 身份）

**Declared contract**:
shader 作者声明的资源与接口约束。它说明允许或要求什么，不等于某个 Variant 编译后实际
使用了哪些资源。
_Avoid_: reflection（编译结果事实，不是作者声明）

**Stable binding identity**:
shader 源使用 target 已有的 HLSL 语义声明 binding：DXIL 以 `register(..., space...)` 为真相，
SPIR-V 以 `[[vk::binding(...)]]` / `[[vk::push_constant]]` 为真相。RadRay 不新增 binding
attribute，也不要求两套数字相等。同一资源在同一 target 的不同 Variant 中保持 binding
稳定，只会处于 active 或 inactive；compiler 负责验证并输出当前 artifact 的 active subset。

**DXIL Root Signature contract**:
DXIL 直接复用 HLSL `[RootSignature(...)]` 作为 descriptor table、root descriptor、root
constant、static sampler 与 visibility 的声明真相。forked DXC 在 compiler 内按当前 Variant
所有 active stage 的资源并集投影出精确 Root Signature，移除 inactive parameter/range；不新增
RadRay residency attribute。graphics Pass 的 stage entries 必须解析为同一份 Root Signature，
否则编译失败。

**Target-specific inline constants**:
SPIR-V target 遵守 DXC/Vulkan 规则，每个 shader source 最多一个 active
`[[vk::push_constant]]` block；多个 `VkPushConstantRange` 只是同一 address space 的 offset/stage
ranges，不代表多个独立 blocks。DXIL target 不套用这个限制，允许最终 Root Signature 包含多个
active `RootConstants` parameters，并逐个与相应 `register(b#, space#)` 上的 active HLSL constant
declaration 关联。两 target 的数量、位置、名称与 byte layout 不要求一致。旧 RHI 的单一
`optional<PushConstantDescriptor>` 必须迁移，不能反向限制 compiler output。

**RootSignature target scope**:
HLSL `[RootSignature(...)]` 只属于 DXIL lane；可以在 `__spirv__` 条件下排除。SPIR-V layout
完全由标准 `vk::binding` / `vk::push_constant` 与 SPIR-V compiler output 决定，不要求存在等价
RootSignature 字符串或跨 target 的 descriptor/push-constant projection。DXIL graphics entries
仍必须解析为同一份 variant-projected RootSignature，compute entry 单独解析自己的版本。

**Static sampler bridge**:
compiler 从 DXIL Root Signature 解析 static sampler，与同一 HLSL `SamplerState` declaration
关联，再读取该 declaration 的 SPIR-V `vk::binding` 生成 Vulkan immutable sampler metadata。
两套 binding 数字不要求相等；无法唯一关联或 sampler schema 无法无损表达时编译失败。

**SPIR-V target gate**:
shaderlib 使用 DXC 内置的 `__spirv__`（以及版本宏）判断 SPIR-V lane，不再依赖手工
`VULKAN` define。DXIL lane 不定义 `__spirv__`；RadRay 不复制或覆盖这些 compiler-reserved
macros。

**Compiler-generated binding metadata**:
编译器根据最终产物生成的 active binding、stage visibility、类型与 layout identity。它与
shader bytecode 一起构成 Variant artifact；运行时直接信任它，不再通过 DXIL/SPIR-V 反射做
二次校验。

**Vertex input ownership**:
shader compiler 只输出当前 target/Variant 的最终 vertex entry interface/reflection，不接触实际
vertex buffer 的 binding、stride、step mode、attribute storage format 或 byte offset。物理字节
布局完全属于 primitive/mesh vertex schema；PSO builder 用 compiler output 与
`PrimitiveVertexLayout` 解析出 native vertex input state。HLSL 不新增物理 vertex-layout
attributes，旧 manifest `VertexInput` 删除。semantic/location、shape/format、slot/offset/stride/step
mode 不兼容时，PSO builder 必须在创建任何 native PSO 前 fail closed。

**Target-native vertex reflection**:
DXIL 与 SPIR-V target result 各自保留 native vertex interface facts，不构造跨 target 的统一
register/location schema，也不要求两边 DCE 后的 parameter 集合相同。D3D12 consumer 按
semantic/index/signature facts 解析，Vulkan consumer 按 final location/type/decorations 解析；
runtime 不重新调用 reflection API。

**Target-native runtime layout**:
compiler metadata decode 后，D3D12 与 Vulkan 的 pipeline layout 继续保持各自的 target-native
表示并直接交给对应 backend；RHI 只保留薄的公共操作层，不以统一 `PipelineLayoutDescriptor`
重新表达 compiler output。runtime binding handles 必须在当前 target artifact 上解析，不能假设
DXIL register/space、SPIR-V set/binding、RootConstants 或 push-constant ranges 可以互换。

**Binding lookup identity**:
active resource 的 canonical HLSL declaration name 是 caller 唯一的加载期 lookup key；不再由
manifest/caller 提供 binding alias。runtime 在当前 target/Variant artifact 上把名称解析成
artifact-local `BindingHandle`，提交路径只使用 handle。handle 不跨 target、Variant 或重编译保持
数值稳定，layout 改变后必须重解析；inactive declaration 的查找明确失败。DXIL register/space 与
SPIR-V set/binding 只属于 handle 背后的 target payload，不构成公共身份。

**Variant-level compiler metadata**:
graphics Variant 的各 stage 必须在同一个 compiler-level request 中完成 metadata 合并，直接
生成 Variant 级 metadata。合并发生在编译器内部，不交给 shader_cook 或 runtime；最终交付的
metadata 与各 stage bytecode 一起由这次 compiler request 产生。一个 request 代表一个确定的
Variant，不得把不同 keyword assignment 的结果混在同一个 metadata 中。

**Variant assignment ownership**:
合法组合域由 shader contract 定义；具体 assignment 由调用方传入。shader_cook 为 AOT
逐个传入要烘焙的 assignment，runtime 在允许 JIT 时为当前请求传入 assignment。编译器负责
校验并编译该 assignment，不负责枚举全部 permutation。

**Compile-time macro inputs**:
`CompileVariantRequest` 以两个独立的 structured channels 接收 `KeywordAssignments` 与普通
`Defines`，不允许调用方在 raw compiler arguments 中传 `-D`。keyword assignment 使用
`group -> choice/off` 并按 HLSL domain 校验；普通 define 使用 `name -> value`，不得覆盖任何
domain keyword。两组输入都由 compiler canonicalize 并进入 artifact identity。

**Structured compile policy**:
RadRay DXC extension 不接受 raw DXC arguments。shader model、optimization/debug、warnings、
resource-binding assumptions、DXIL settings 与 SPIR-V target environment 由 caller/build profile
通过 typed `CompilePolicy` 提供；compiler 根据标准 stage attribute 生成 profiles，并把所有
resolved policy 纳入 result/artifact identity。任意 DXC flag 实验只能走标准 `IDxcCompiler3`，
不进入 RadRay artifact pipeline。

**Keyword domain declaration**:
Pass 的合法 keyword groups 使用 HLSL `#pragma radray_keyword_group` 声明，并且必须位于所有
Variant 条件之外。forked DXC 在 preprocessor 内正式解析、校验并输出 canonical domain；
cook/runtime 不解析 pragma，RadRay 不再保留外部预处理文本扫描器。重复 group、keyword 跨组
重复、非法 assignment 或 pragma 语法错误都是 compiler hard error。pragma 不接受作者声明的
`stages(...)`；stage dependency、resource visibility 与 stage projection 都由 compiler 生成。
只有 Pass 根 `.hlsl` 可以声明 group；`.hlsli` 只能消费 keyword 宏，不能隐式扩张 include 它的
Pass domain。

**Source contract discovery**:
RadRay DXC extension 提供独立的 compiler-owned `DiscoverSourceContract` 操作，根据 source、
include closure、普通 `Defines` 与相关 policy 输出 canonical keyword domain、entry topology 和
`ContractHash`，但不生成 target bytecode。cook/editor 不自行解析 pragma；它们基于 discovery
result 规划具体 assignments。`CompileVariant` 接收 `ExpectedContractHash` 并在 compiler 内重新
发现、校验 contract 与 assignment，拒绝 discovery 后发生的 source/contract 漂移。AOT runtime
直接消费 cook 产出的 contract/index，不需要 compiler。

**Cross-target source contract invariant**:
对同一组 source/include/Defines，DXIL lane（不定义 `__spirv__`）与 SPIR-V lane（由 DXC 内建定义
`__spirv__`）必须输出相同的 keyword domain、entry names/stages、graphics/compute kind 与
cardinality。`DiscoverSourceContract` 在两个 target mode 中各做一次轻量 frontend discovery，比较
canonical contract 并在不一致时 hard error；只返回一份公共 `ContractHash`。`__spirv__` 可以改变
函数体、资源/binding、RootSignature projection、类型布局和 stage interface，但不能改变上述
Variant contract。普通 `Defines` 是 discovery 输入，所以不同 Defines 集合可以形成不同 contract。

**AOT bake set**:
一次 cook 要预编译的 Variant assignments，是调用方根据项目、平台和内容使用情况提供的
build input，不属于 shader metadata。HLSL 只声明合法 keyword domain；不再存在作者维护的
`BakeVariants` 字段或文件。

**Artifact trust**:
运行时只接受 schema/version、工具链身份与 wire 安全解析通过的 Variant artifact，并把其中的
compiler-generated binding metadata 视为事实；不通过 runtime reflection、第二份 schema 或 hash
交叉核对它。type tree 的语义错配属于 compiler/ODR 系统缺陷；wire 越界、非法 record 或无法安全
构造 CPU 数据时仍必须 fail closed。

**Shader identity hashes**:
`ContractHash` 覆盖 canonical keyword domain、entry topology 及影响 discovery 的 Defines/policy，
用于绑定 assignment planning。每个 target lane 的 `CompileInputHash` 覆盖 `SourceName`、root bytes、
该 lane/Variant 实际打开的规范化 include path/content、canonical assignment/Defines、target、
resolved policy、fork/ABI/schema/validator/toolchain identity；它不额外读取 compiler 输出的 runtime type
tree（改变 source/include bytes 仍会按 CompileInputHash 的正常规则改变该 hash）。
`GpuArtifactHash` 只覆盖 bytecode 与 GPU layout metadata，用于 GPU artifact/layout identity；第一期
不定义 `ArtifactContentHash`、content-address publisher 或 `CpuSchemaHash`。compiler 输出的 type tree
不作为独立 hash 输入，也不独立缓存、寻址或兼容性校验。`AssetId` / `PassName` 不进入任何 compiler hash。

**Runtime CPU type schema**:
compiler 为每个 target result 输出 runtime 构造 CPU buffer data 所需的完整 target-native
cbuffer/struct type tree，包括 nested members、names、offset/size、array/matrix stride 与 scalar/
vector/matrix shape。runtime 不从 C++ declaration、其他 artifact 或 reflection API 获取第二份
schema，也不做交叉校验。type tree 遵守 ODR-style invariant：不设置独立 `CpuSchemaHash`，不独立
缓存、寻址或跨 Variant 复用，必须与所属 target result 原子交付和存活。GPU layout 使用
`PipelineLayoutHash`；`GpuArtifactHash` 覆盖 bytecode 与 GPU layout metadata，不覆盖 CPU type
tree。runtime 只检查 type tree 的 wire bounds、record kind、offset/size、stride 和 CPU 构造安全性；
语义错配被视为系统缺陷，而不是 runtime validation case。

**Binding group**:
一组资源绑定的集合。同时是 D3D12 的 register space 与 Vulkan 的 descriptor set index ——
这是后端已硬化的不变量，任何一层都不做重映射。
_Avoid_: descriptor set, register space, space, table

**Residency**:
一个绑定是经 descriptor table 访问，还是作为 root descriptor 直接绑定。属于性能决策，
不是 shader 的属性。
_Avoid_: binding mode, access path

**Artifact**:
离线编译产出的、可在无编译器环境下加载的 shader 产物。内容寻址。
_Avoid_: blob（指承载 artifact 的单个文件）, cache（cache 可弃，artifact 是交付物）

### 离线编译（术语待定）

第一期不做离线编译，"cook" 与 "bake" 两个词一并搁置 —— 旧代码里它们的边界要靠注释解释
（`shader_manifest.h:150`），属于需要重新命名的遗留。等第一期跑通、真要做离线产物时再定名。
在此之前不要在新代码里引入这两个词。

## 资产

**Asset**:
由路径标识、引用计数管理生命周期的可加载资源。
_Avoid_: resource（resource 指 GPU 侧对象）

**AssetId**:
由归一化路径派生的资产标识。
_Avoid_: asset key, path hash
