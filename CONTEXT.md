# RadRay

C++20 实时渲染器，D3D12 + Vulkan 双后端，纯光栅。

本文件是**领域词汇表**：定义术语「是什么」。不描述实现、不记录决策、不当规格书用。
实现现状见 `docs/architecture/`，决策见 `docs/adr/`。

当前实施边界：`radrayshadercompiler` 已有 source discovery、typed variant、双 target wire、
RadRay DXC fork extension client 与 extension probe；client 只用 fork extension ABI，无 stock
adapter，stock extension probe 会 fail closed。RadRay DXC fork package 已通过
`project_manifest.json` 的 `radray_dxc` 本地包接入并验证正向 ABI result；正式 artifact
publisher/index 和 install/export 层仍未实现。source/metadata scanner 已退役，contract 与 metadata
统一来自 Clang/DXC frontend 和最终 target model；剩余发布与跨平台验收边界由 ADR-0034 与
`docs/todo/radray-dxc-frontend-semantic-migration.md` 定义。

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

**Root source**:
一个 Pass source unit 在一次编译操作中提交的根 `.hlsl` 原始源码；它与物理文件路径、预处理后的
源码以及 transitive include 内容是不同概念。共享 `.hlsli` 不属于 root source。
_Avoid_: expanded source, include closure

**Logical source name**:
caller 为内存 root source 提供的非绝对、逻辑 HLSL 名称；compiler 将它作为 DXC virtual main-file
name 用于诊断和预处理上下文，不据此从 filesystem 重新读取 root。项目 authoring 的 angle include
仍由 caller 提供的 `-I` path list 解析。

**Pass asset identity**:
`PassName` 与 `AssetId` 属于 caller/资产系统，不属于 shader compiler contract。RadRay DXC
extension request/result、compiler metadata 与 artifact identity 均不包含 `PassName`；compiler
只接收 shaderlib-root-relative 的逻辑 `SourceName`，用于诊断与 virtual source context；该
逻辑身份不等于物理仓库/部署路径，且 shaderlib include 内容不属于稳定 source identity。cook/资产层在自己生成的索引中把
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

**Shader contract discovery**:
compiler 对 root source 执行的不产出 target bytecode 的 syntax-only frontend operation，按普通
filesystem include search 读取共享 include，并由 Clang/DXC preprocessor 与 AST/Sema 输出 canonical
keyword domain、entry topology 和 `ContractHash`。它与 concrete compile 使用同一份 `Defines`、
frontend policy、ordered include paths 和 DXC default include handler 规则。include 缺失、parse 或
Sema 失败直接成为该次 discovery 的 compiler diagnostics。raw source/include 内容和路径本身不属于
contract identity；只有它们改变的 canonical contract facts 进入 hash。

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
为生成 SPIR-V lane 所需的 immutable sampler metadata，compiler 可以在内部额外执行不产出
DXIL result 的 DXIL-mode RootSignature/static-sampler analysis；该辅助 lane 不改变 result 的
requested target lane 集合。

**Filesystem-backed compilation**:
一次 discovery/compile 在调用时由 DXC 的默认 filesystem include handler 从当前文件系统读取共享
HLSL 源码；include search 使用由调用方在本次调用提供的普通有序 include directories。RadRay
authoring 仍以唯一逻辑 `shaderlib` 根组织 include，但其物理目录由调用方传入。include 内容是
本次编译观察到的依赖，不属于 Pass 或 Variant 的稳定 identity；每个实际 compiler invocation 使用
自己的 default handler，编译器不负责决定外部产物何时失效。
_Avoid_: content-closed request, caller-owned include closure

**Shaderlib include root**:
authoring 使用的唯一逻辑 HLSL include 根；每次编译由 caller 将对应物理 directory 作为普通 `-I`
搜索路径提供给 DXC。compiler 不内置其名称或物理位置；它不是 shader request 的 shader 内容，
也不是 shader source identity。

**JIT include paths**:
`ShaderJit` 构造时接收并在生命周期内固定的有序物理 include directory 数组；discovery 与
compile 都按相同顺序使用它。需要切换 shader 工程或 include 根时创建新的 `ShaderJit`，不在运行
中修改现有实例。JIT 原样保存并传给 DXC，不做绝对化、canonicalize 或存在性检查；相对路径在
实际 compiler invocation 时按当时的进程 CWD 解析。数组顺序就是 DXC `-I` 顺序；同名 include
由首个命中的目录提供。它在 extension ABI 中作为独立的 borrowed per-call path-list view 传递，
不属于 shader request 或 shader identity。数组可以为空；此时不提供 `-I`，include 缺失只在
实际需要读取时由 DXC 报告。ABI view 的每项是显式长度的 UTF-8 path bytes，不要求 NUL 结尾；
空列表可用 `Count == 0` 与空指针表示，非空列表中的空 path 或嵌入 NUL 直接作为 invalid request
拒绝。

`radray_shader_compile` 作为 filesystem-backed 编译的命令行 caller，保留 `--shader-root` 用于
定位 root source，并将其作为第一个 include directory；可重复的 `--include-path` 按命令行顺序
追加到路径数组。工具不递归读取、预展开或验证 include closure。

**JIT diagnostics surface**:
compiler client 的 discovery/compile result 保留完整的 status 与 diagnostics；runtime `ShaderJit`
只向上提供无状态的 optional convenience result，不保存 `LastDiagnostics` 或其他可变错误状态。

**Explicit JIT include configuration**:
`ShaderJit` 构造函数必须显式接收 include path list；无 include 的实例显式传空数组。不存在隐含
默认路径或 compiler 内建路径。

**Immutable JIT include ownership**:
JIT 按值接收并持有 include path list；construction 完成后没有 setter，const discovery/compile 可以
并发读取同一份列表。每次 ABI 调用只借用临时 view，不把 caller 内存交给异步 compiler 保存。

**Include tree stability window**:
一次 discovery、concrete compile 或 multi-target batch 的 filesystem 读取期间，caller/build system
负责保持 include tree 不变。RadRay/DXC 不建立跨 invocation 的 snapshot、锁或 include cache；文件变更
后的行为由实际读取时序决定。

**RadRay DXC extension ABI**:
fork extension 只在独立的 `dxcapi_radrayext.h` 中声明，使用 RadRay-owned
`CLSID_RadRayDxcCompiler`、`IRadRayDxcCompiler` 与 `IRadRayDxcResult`，并通过 upstream
`DxcCreateInstance` / `DxcCreateInstance2` 创建。upstream `dxcapi.h`、现有 CLSID/IID、interface
vtable 与 DLL exports 保持不变；stock DXC 对扩展 CLSID 返回不支持，RadRay 必须 fail closed。
extension interface 直接继承 `IUnknown`，不继承或扩展 `IDxcCompiler3`。

**RadRay DXC SDK distribution**:
目标形态是 RadRay 主 CMake 工程只消费独立流水线产出的、版本与内容 hash 固定的预编译
RadRay DXC SDK；不通过 `add_subdirectory`、`FetchContent` 或 `ExternalProject` 构建 DXC 源码。
当前工作树通过 `project_manifest.json` 的 `radray_dxc` 本地包消费真实 fork package：它由 fork
仓库 `utils/package_radray_sdk.py` 打包为 relocatable archive，包含
CMake config package 与 imported targets，集中表达 headers、compiler/validator binaries、
import libraries 和可部署 runtime files；RadRay 只按 `Name` 使用固定 SDK prefix，不手工解析包内
部文件布局，也不设任何
env/cache override。SDK package 必须支持按组件消费和部署裁剪，为未来不携带 shader compiler
的纯运行时发布构建铺路。`dxcompiler` 属于 compiler component；external `dxil` validator 是
可选 component，不构成 DXIL compile path 的无条件依赖。每个 SDK build 通过 DXIL loadability
gate 证明默认产物可用。打包产物含完整 dxc 发行集：`bin/dxc.exe`、`bin/dxcompiler.dll`、
`bin/dxil.dll`、`lib/dxcompiler.lib`、`lib/dxil.lib`、`include/dxc/*` 与
`lib/cmake/RadRayDXC/*`。
package identity 固定为 `RadRayDXC`，不冒充 stock `dxc`；CMake namespace 为 `RadRayDXC::`。
它提供独立的 `Headers`、`Compiler`、`Validator` 与 `CLI` imported targets：`Compiler` 依赖
`Headers`，`Validator` 依赖 `Headers`，`CLI` 依赖 `Compiler`，不提供隐式引入全部组件的 umbrella。
`tools/fetch_sdks.py` 按 `project_manifest.json` 固定版本、triplet 与 archive hash 准备
`SDKs/radray_dxc` extracted prefix；RadRay `CMakeLists.txt` 使用固定的
`Name` 路径发现 package，配置阶段不再解析
`.radray-sdk.json` 或 archive hash。禁止隐式回退到系统或 package manager 中的其他 DXC，
也不提供 `RADRAY_DXC_SDK_ROOT`/`RADRAY_DXC_FORK_PACKAGE_ROOT` 之类的开发 override。严格
纯运行时配置完全不发现 `RadRayDXC`。
SDK canonical identity 使用完整 upstream version 加 `.radray.<release>` suffix，例如
`1.9.2607.radray.1`。RadRay 的 `find_package(RadRayDXC CONFIG REQUIRED ...)` 不指定 version，
也不依赖 CMake 的 numeric ConfigVersion comparison；config package 导出完整 identity。fork
重新打包后用新的 archive hash 更新 manifest，其余机器由 `fetch_sdks.py restore` 按本地包复制
和校验；CMake 只消费按 `Name` 派生出的 package prefix。

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
目标 client 动态加载 `RadRayDXC::Compiler` 指向的 binary，只把 `RadRayDXC::Headers` 作为 C++
compile dependency，不通过 import library 建立进程启动时的 loader dependency。client 只用
RadRay DXC extension ABI（`dxcapi_radrayext.h`），无 upstream `IDxcCompiler3` fallback；
`RADRAY_SHADER_COMPILER_FORK` 在 compiler 构建中恒定义，stock 编译路径已删除。stock DXC 的
extension CLSID 探测仍 fail closed，不能把任何 stock 产物当作 fork ABI。
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
optional DXIL serialized Root Signature 和各 stage bytecode/debug outputs。upstream COM blob 类型可以作为叶子
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
一个 Compiled Variant artifact 需要哪些 logical resources、各自落在哪个 target-native group 与
slot 上，以及可选 RootSignature policy lowering 得到的 base placement。由编译器生成并随产物
交付，是该 target base binding ABI 的唯一权威；不使用的资源不占 active Vulkan layout 槽位。
对于 graphics Variant，layout 是所有 active stage binding 的并集；每个 entry 记录自己的实际
stage visibility。pipeline 的 Target layout modifier 不属于 Binding layout，它只参与后续
Resolved target layout。
_Avoid_: pipeline layout, root signature, descriptor set layout（这三个是后端说法）；
Property（这是 codegen 内部的表示）；binding ABI（指同一事物的契约面，不指数据本身）

**Layout identity**:
描述 layout 语义的身份。compiler 的 Base layout identity 区分资源集合、logical kind、槽位、
stage 可见性与 RootSignature policy lowering；render 的 Resolved layout identity 再加入当前
backend 的 canonical modifier 结果。只有对应层 identity 相同的对象才能共享。
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

**Explicit DXIL Root Signature**:
RootSignature policy 在 DXIL lane 的 serialized carrier。它包括 descriptor table、root descriptor、
root constants、static sampler、parameter order、visibility 与 flags；forked DXC 校验并把标准
serialized form 作为 optional DXIL artifact range 发布，D3D12 RHI 直接消费该 blob，不根据 binding
metadata 重建或改写。只要任一相关 entry 声明了 RootSignature policy，malformed、跨 stage 冲突或
与 active resources 不兼容都必须编译失败，不能改走 Implicit fallback。carrier 可以是跨 Variant
稳定的 declared superset；这不要求 Vulkan 实例化 inactive declarations。
_Avoid_: authored layout, manual Root Signature

**Implicit D3D12 Root Signature**:
DXIL Variant 未选择 Explicit DXIL Root Signature 时，由 D3D12 RHI 按 backend fallback policy 从
artifact 的 active binding metadata 生成的 Root Signature。此时 DXIL artifact 的 serialized Root
Signature range 为空；forked DXC 不实现或发布默认 Root Signature。当前 fallback 使用 descriptor
tables，不推断 root descriptor、RootConstants 或 static sampler。
_Avoid_: Generated DXIL Root Signature, compiler-generated Root Signature

**Root Signature source**:
一个 DXIL Variant 在 D3D12 创建 pipeline layout 时采用的互斥来源。`Explicit` 表示 artifact 带有
作者声明的非空 serialized Root Signature range；该 range 原样保存完整
`DXC_OUT_ROOT_SIGNATURE` 独立 container，不抽取或重新包装 `RTS0`。`Implicit` 表示该 range
为空，由 D3D12 RHI 根据 active binding metadata 生成 Root Signature。wire 不另存重复的 source
enum；range presence 唯一决定 source。source 不改变 binding facts 的 compiler authority，但明确
区分作者 policy 与 backend fallback policy。
_Avoid_: Root Signature provenance, Canonical DXIL Root Signature

**Target-specific inline constants**:
SPIR-V target 遵守 DXC/Vulkan 规则，每个 shader source 最多一个 active
`[[vk::push_constant]]` block；多个 `VkPushConstantRange` 只是同一 address space 的 offset/stage
ranges，不代表多个独立 blocks。DXIL target 不套用这个限制，允许最终 Root Signature 包含多个
active `RootConstants` parameters，并逐个与相应 `register(b#, space#)` 上的 active HLSL constant
declaration 关联。RootSignature policy 中覆盖 SPIR-V active push declaration 的 `RootConstants`
lower 为该 `VK_PUSH_CONSTANT` block；push declaration 同时写 DX register 与 `VK_PUSH_CONSTANT`，
不得再写 `VK_BINDING`。D3-only declarations仍可形成多个RootConstants，但一个Vulkan Variant不能
形成多个active logical push blocks。

**RootSignature policy**:
HLSL `[RootSignature(...)]` 声明的跨 target base layout policy。每个 concrete Variant 先由
compiler-owned、与输出 target无关的frontend按相同source/include、Defines、assignments和完整
CompilePolicy解析，再分别lower：DXIL得到standard serialized carrier；SPIR-V得到Vulkan-specific
fixed-width records。普通declaration的DX register/space与`VK_BINDING`数字可以不同，compiler只按
canonical HLSL declaration identity关联。能关联policy parameter的active declaration中，
DescriptorTable lower为普通Vulkan descriptor；root
CBV为dynamic uniform buffer；root SRV/UAV buffer为dynamic storage buffer；RootConstants为作者写的
`VK_PUSH_CONSTANT`；StaticSampler为full-state immutable sampler。D3-only topology/flags不强造
Vulkan语义；target-only declaration保持标准Vulkan attribute语义，Vulkan stage flags取实际active
stages。没有attribute时不合成公共policy：D3走Implicit Root Signature，Vulkan走普通descriptors。
_Avoid_: DXIL-only RootSignature policy, Vulkan RootSignature blob, runtime RootSignature link

**Static sampler bridge**:
compiler 从 DXIL Root Signature 解析 static sampler，与同一 HLSL `SamplerState` declaration
关联，再读取该 declaration 的 SPIR-V `vk::binding` 生成 Vulkan immutable sampler metadata。
metadata 是 Vulkan-specific fixed-width full-state record，覆盖filter/address/LOD/bias/anisotropy/
compare/border/reduction，不只是immutable bit。两套 binding数字不要求相等；无法唯一关联或无法
无损表达时编译失败。D3仍直接消费serialized RS中的native static sampler state。

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

**PrimitiveVertexLayout**:
一个 mesh primitive 的 geometry-owned 物理顶点布局：buffer binding、stride、step mode、semantic/
index、storage format 与 byte offset。它不含 shader location；location 只在与 concrete Variant 的
target-native vertex reflection 合并时确定。
_Avoid_: vertex declaration（易与 shader interface 混淆）, input layout（后端对象名）

**Material**:
一次 surface draw 对 concrete `ShaderProgram` 的选择，加上命名参数值、纹理/sampler 引用、完整固定
功能状态基线与 render queue。Material 不拥有 geometry topology，也不拥有 pass attachment format、
sample count 或 compatible render pass。
_Avoid_: material instance（当前没有两级继承）, effect

**MeshDrawItem**:
collector 为单个 camera/frame 从一个 primitive section 展平出的绘制事实：geometry、material、
local-to-world、index range、section identity、view depth 与稳定顺序。它是临时排序输入，不是持久
GPU command 或资产所有者。
_Avoid_: draw command, mesh batch cache

**Target-native vertex reflection**:
DXIL 与 SPIR-V target result 各自保留 native vertex interface facts，不构造跨 target 的统一
register/location schema，也不要求两边 DCE 后的 parameter 集合相同。D3D12 consumer 按
semantic/index/signature facts 解析，Vulkan consumer 按 final location/type/decorations 解析；
runtime 不重新调用 reflection API。

**Resolved target layout**:
target artifact decode后，把base records与当前backend的Target layout modifiers canonical resolve
得到的owning、可比较、可hash value。D3D12与Vulkan分别使用`ResolvedD3D12Layout`和
`ResolvedVulkanLayout`，它们是native layout creation的唯一输入；不以统一
`PipelineLayoutDescriptor`重新表达。resolved value拥有descriptor/push/sampler/dynamic-order与
name table facts，不借用artifact span，也不包含native handles。
_Avoid_: Target-native runtime layout, common resolved layout descriptor

**Binding lookup identity**:
active descriptor或push declaration的canonical HLSL name是caller唯一的加载期lookup key；不再由
manifest/caller提供alias。resolved layout把名称解析成artifact-local `BindingHandle`，提交路径只用
handle。公共handle只暴露validity/equality，factory、generation、namespace、table index和native
destinations均为layout内部。handle不跨target、Variant或重编译稳定，layout改变后必须重解析；
inactive declaration查找失败。DX register/space与SPIR-V set/binding不构成公共身份。

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
compiler-owned `PragmaHandler` 按 compiler token 规则读取 active directive，并用 `SourceManager`
验证 main-file spelling location 与 conditional depth；cook/runtime 不解析 pragma。重复 group、keyword
跨组重复、非法 assignment 或 pragma 语法错误都是 compiler hard error。pragma 不接受作者声明的
`stages(...)`；stage dependency、resource visibility 与 stage projection 都由 compiler 生成。只有
Pass 根 `.hlsl` 可以声明 group；`.hlsli` 只能消费 keyword 宏，不能隐式扩张 include 它的 Pass
domain。

**Source contract discovery**:
RadRay DXC extension 提供独立的 compiler-owned `DiscoverSourceContract` 操作，根据 typed request
中的 root source、普通 `Defines`、requested targets、`CompilePolicy` 和独立的 caller-provided
filesystem include search 输出 canonical keyword domain、entry topology 和 `ContractHash`，但不生成
target bytecode。cook/editor 不自行解析 pragma；它们基于 discovery result 规划具体 assignments。
`CompileVariant` 接收 `ExpectedContractHash` 并用同一 frontend collector 重新发现、校验 concrete
contract 与 assignment，拒绝 discovery 后发生的 source/contract 漂移。AOT runtime 直接消费 cook
产出的 contract/index，不需要 compiler。

**Cross-target source contract invariant**:
对同一组 source/include/Defines，DXIL lane（不定义 `__spirv__`）与 SPIR-V lane（由 DXC 内建定义
`__spirv__`）必须输出相同的 keyword domain、entry names/stages、graphics/compute kind 与
cardinality。`DiscoverSourceContract` 请求两个 target 时各做一次轻量 frontend discovery，比较
canonical contract 并在不一致时 hard error；只返回一份公共 `ContractHash`。只请求一个 target 的
结果不能证明 cross-target invariant，cook 与跨后端发布 gate 必须请求两者。`__spirv__` 可以改变
函数体、资源/binding、RootSignature projection、类型布局和 stage interface，但不能改变上述
Variant contract。普通 `Defines` 是 discovery 输入，所以不同 Defines 集合可以形成不同 contract。

**AOT bake set**:
一次 cook 要预编译的 Variant assignments，是调用方根据项目、平台和内容使用情况提供的
build input，不属于 shader metadata。HLSL 只声明合法 keyword domain；不再存在作者维护的
`BakeVariants` 字段或文件。

**Artifact trust**:
运行时只接受 schema/version、工具链身份与 wire 安全解析通过的 Variant artifact，并把其中的
compiler-generated binding metadata 视为事实；不通过 runtime reflection 或第二份 schema 交叉核对
它。decoder 可以把 metadata 中的 compiler-produced `GpuArtifactHash` 与 caller 提供的独立可信
expected hash 做相等比较，但不在 RadRay 侧重算 hash，也不把该比较扩张为完整 artifact integrity
校验。type tree 的语义错配属于 compiler/ODR 系统缺陷；wire 越界、非法 record、identity mismatch
或无法安全构造 CPU 数据时仍必须 fail closed。

**Shader identity hashes**:
`ContractHash` 覆盖 compiler 产出的 canonical keyword domain、entry topology 和 shader kind，用于
绑定 assignment planning。Defines、policy、root/include bytes、source name 与 include paths 不按原始
输入进入 hash；它们只有在改变 canonical contract facts 时才间接改变 hash。include 是
filesystem-backed compilation 在调用时读取的外部源码依赖。每个 target lane 的 `BytecodeHash` 覆盖完整 target bytecode，
`BasePipelineLayoutHash` 覆盖 canonical target-native base GPU layout records，`GpuArtifactHash` 覆盖 bytecode
与 GPU layout metadata。Explicit DXIL Root Signature 使用 DXC 已从 serialized `RTS0` payload 计算的
`RootSignatureHash` 作为一项 layout record进入 `BasePipelineLayoutHash`；完整
`DXC_OUT_ROOT_SIGNATURE` carrier container不直接进入 compiler hash。Implicit source没有
compiler-produced RS semantics，其 layout hash只覆盖 active binding metadata并使用独立 domain。
这些是 compiler output identity，不是输入或缓存失效标识。
三者使用统一的 128 位固定字节序表示；render resolver另算`ResolvedLayoutHash`，覆盖current
backend canonical resolved semantics，不覆盖modifier原始顺序或native handles。第一期不定义
`ArtifactContentHash`、content-address
publisher 或 `CpuSchemaHash`。compiler 输出的 type tree
不作为独立 hash 输入，也不独立缓存、寻址或兼容性校验。`AssetId` / `PassName` 不进入任何 compiler hash。

**Root Signature artifact coalescing**:
一次 DXIL lane最多发布一份 optional Explicit Root Signature range。graphics各 stage 的非空
`DXC_OUT_ROOT_SIGNATURE` outputs必须逐字节相同，lane merge只保留其中一份；compute最多有一份。
最终 artifact对Explicit stage DXIL启用 `-Qstrip_rootsignature`，不再在各 bytecode中重复内嵌
`RTS0`；实施中允许先保留 embedded RS完成direct-consumption GPU parity gate，再原子切换并断代
bytecode/artifact goldens。
本契约不定义跨 artifact/package 的 RS content table，也不定义 runtime `PipelineLayout` 或
`ID3D12RootSignature` cache；这些上层共享策略不属于 compiler artifact cutover。
_Avoid_: Root Signature cache（本术语只描述 artifact内合并）

**Root binding fan-out**:
一个 active DXIL declaration在 Explicit global Root Signature 中按互不重叠的 shader visibility映射到
多个合法 root locations。artifact-local `BindingHandle` 仍表示一个 shader declaration；D3D12 CPU
binding plan把一次 value write展开到全部 destinations，例如同时写 vertex-visible和pixel-visible的
两个 root CBV parameters。visibility重叠的 register overlap仍由 DXC validation拒绝。
_Avoid_: duplicate binding（合法 fan-out不是重复声明错误）

**Unsupported Root Signature architecture**:
当前 ordinary graphics/compute contract不接受 DXR `LOCAL_ROOT_SIGNATURE`，也不接受 SM 6.6
`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` / `SAMPLER_HEAP_DIRECTLY_INDEXED`。前者依赖 DXR state object、
shader record与SBT，后者依赖独立的跨后端 bindless heap/index生命周期模型；它们不是普通 Explicit
global Root Signature direct-consumption 的附带能力。

**Runtime CPU type schema**:
compiler 为每个 target result 输出 runtime 构造 CPU buffer data 所需的完整 target-native
cbuffer/struct type tree，包括 nested members、names、offset/size、array/matrix stride 与 scalar/
vector/matrix shape。每个 CBuffer declaration 还显式拥有一个 lane-local payload root；多个 declaration
可以共享 root，一个 owned root 也可以被另一 root 引用。runtime 不从 type 发射顺序、C++ declaration、
其他 artifact 或 reflection API 推断这条关系，也不做交叉校验。type tree 遵守 ODR-style invariant：
不设置独立 `CpuSchemaHash`，不独立缓存、寻址或跨 Variant 复用，必须与所属 target result 原子交付和
存活。GPU layout 使用 `BasePipelineLayoutHash`；`GpuArtifactHash` 覆盖 bytecode 与 GPU layout
metadata，不覆盖 CPU type tree 或 declaration owner。runtime 只检查 owner、type tree 的 wire bounds、
record kind、offset/size、stride 和 CPU 构造安全性；语义错配被视为系统缺陷。

**Shader parameter path**:
CPU 参数在一个 program 内的身份是从 HLSL CBuffer declaration 开始的完整成员路径，例如
`ForwardMaterial.BaseColor`。全 program 唯一的 leaf name 可以作为同一参数的简写；重复 leaf
只令简写 ambiguous，不构成身份，也不使 program 创建失败。struct array element 由 setter 的
element 参数选择，不编码进 path。Texture/Sampler declaration name 是 exact top-level identity，
优先于同名 leaf 简写。
_Avoid_: flat parameter name（叶名只可能是简写）, binding/type emission order

**Binding group**:
一组资源绑定的集合。同时是 D3D12 的 register space 与 Vulkan 的 descriptor set index ——
这是后端已硬化的不变量，任何一层都不做重映射。
_Avoid_: descriptor set, register space, space, table

**Binding group plan**:
一条 concrete render pipeline 对 binding group 更新频率/角色的值映射，例如 view、material、object。
group 数字仍由 HLSL author 声明；plan 只把这些数字传给 material 与执行器，不是全引擎编号表。
_Avoid_: global binding convention, group remap

**Logical shader resource kind**:
HLSL declaration在artifact中的资源类别，例如CBuffer、typed/structured/raw read/write buffer、
texture/storage texture和sampler。它决定可接受的value与descriptor class，但不编码D3
Table/RootDescriptor或Vulkan Regular/Dynamic placement。typed、structured与raw buffer必须保持可区分。
_Avoid_: binding type（容易把logical kind与native placement混在一起）

**Native binding placement**:
同一logical declaration在某backend resolved layout中的访问位置。D3D12为descriptor table或合法
buffer root descriptor；Vulkan为ordinary/dynamic descriptor或immutable sampler。placement不是
compiler-independent资源类型，也不通过`ShaderParameterBindingType::Dynamic*`反向修改logical kind。
_Avoid_: Residency, binding mode, access path

**Target layout modifier**:
pipeline在layout creation时对一个target artifact的精确placement override。selector由canonical HLSL
declaration name + expected logical kind组成；D3D12 Implicit只允许合法single-buffer Table<->
RootDescriptor，Vulkan只允许uniform/storage Regular<->Dynamic或为sampler指定/整体替换full immutable
sampler state。
它不改变compiler artifact、resource kind/count/visibility/push range；D3D12 Explicit carrier不接受
重写。missing/inactive selector、kind mismatch和duplicate target都是framework invariant，不是恢复分支。
_Avoid_: ShaderLayoutPolicy, Residency policy, group-wide dynamic policy, layout override

**Shader program layout recipe**:
program request中并列持有的`D3D12TargetLayoutOptions`和`VulkanTargetLayoutOptions`。两字段不强行
同构；只有current backend字段参与resolve和program cache identity，任一字段都不进入compiler
artifact identity。

**Resolved layout hash**:
render resolver对Resolved target layout canonical semantics计算的identity。modifier输入顺序、
显式默认值、borrowed地址与native handles不进入hash。program/layout key由artifact identity加current
backend的Resolved layout hash组成；本术语不承诺global native layout cache。

**Shader program request**:
runtime向`GetOrCreateShaderProgram`提交的完整值，显式包含source compile input（logical source与
structured Defines）、keyword assignments、完整CompilePolicy与Shader program layout recipe。
discovery与compile必须从同一request取得完整compile inputs；compiler artifact cache区分source/
Defines/assignments/policy/target/toolchain，不能只按source name与assignments命中。

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
资产的持久标识，也是元数据存储的 key。入库资产由 GUID 标识（登记时分配一次、永不改变），
散文件由归一化路径派生。
_Avoid_: asset key, path hash

**Asset metadata**:
资产的持久化描述：类型、相对工程路径与参数数据。
_Avoid_: asset record

**Asset path**:
资产相对工程的全局唯一路径。
_Avoid_: bundle-relative path
