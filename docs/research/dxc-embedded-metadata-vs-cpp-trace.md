> - 适用: 评估 DXC HLSL 自定义元数据、DXIL/SPIR-V 产物与 C++ trace shader 的 ABI 归属
> - 权威: 本文是一次针对 RadRay、DXC `v1.9.2607`、实际 `dxc-autobuild` 包与 UE vendored DXC `1.8.2403.0` 源码快照的只读研究记录，不改变 RadRay 契约
> - 锚点: `project_manifest.json`, `modules/render/include/radray/render/rhi.h`, `modules/runtime/include/radray/runtime/asset_manager.h`, `shaderlib/core/math.hlsli`, `docs/todo/hlsl-radray-dxc-shader-pipeline.md`, `docs/adr/0003-manifest-is-abi-authority.md`, `docs/adr/0013-vertex-stage-interface-projection.md`, `docs/adr/0014-cpp-trace-is-shader-source-of-truth.md`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\CMakeLists.txt`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\utils\hct\hctbuild.cmd`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\tools\dxcompiler\CMakeLists.txt`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\tools\dxildll\CMakeLists.txt`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\docs\SPIR-V.rst`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\include\dxc\dxcapi.h`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\include\dxc\DxilRootSignature\DxilRootSignature.h`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\include\clang\Basic\Attr.td`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\include\clang\Lex\Pragma.h`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\include\clang\Lex\Preprocessor.h`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\tools\dxcompiler\dxcapi.cpp`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\tools\dxcompiler\dxcompilerobj.cpp`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\lib\CodeGen\CGHLSLMS.cpp`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\lib\Frontend\InitPreprocessor.cpp`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\lib\Lex\Pragma.cpp`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\lib\Pragma.cpp`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\tools\clang\lib\Sema\SemaHLSL.cpp`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\lib\DxilValidation\DxilContainerValidation.cpp`, `C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607\lib\DxilRootSignature\DxilRootSignatureValidator.cpp`, `F:\cpp\UnrealEngine\Engine\Source\ThirdParty\ShaderConductor\ShaderConductor\External\DirectXShaderCompiler\tools\clang\tools\dxcompiler\dxcutil.cpp`

# DXC Sidecar 与 C++ trace 交叉研究

## 核心结论

这次交叉核对把问题分成两个目标，结论不同：

1. 如果目标只是“shader-owned ABI 元数据有一个真相源、DXIL/SPIR-V 使用同一份输出、删除人工 manifest 握手”，**small DXC frontend fork + canonical sidecar 是可行候选**。它应在 AST 仍存活的 frontend 结束边界收集 HLSL author metadata，使用现有 `DXC_OUT_EXTRA_OUTPUTS`/`IDxcExtraOutputs` 返回独立 sidecar；不新增 `DXC_OUT_KIND`，不把 sidecar 伪装成 bytecode 内嵌数据。
2. 如果目标还包括 typed C++ binding、摆脱 keyword/string variant、表达结构化变体、减少 HLSL/manifest/shader 工具链复杂度，**sidecar 不能独立达到目标**；C++ trace 才覆盖这些目标，但迁移成本更高。

因此本报告不再先验选择 C++ trace。候选选择取决于用户是否只要“单一 ABI 元数据 + 双后端 sidecar”，还是要同时改变 shader 作者模型与 variant 模型。

“删除人工 manifest”也不等于“把现有 JSON 每个字段逐字搬进 HLSL”。Shader entry/domain 与
不可推导的 binding policy 可以属于 HLSL contract；optimization、target、发布 coverage 属于
build profile；vertex buffer packing 属于 mesh schema；content used set 属于内容数据库。可行的
sidecar 方案是**把字段退回各自 owner，再删除手写聚合文件**，不是让 HLSL 冒充所有系统的配置中心。

“一次用户操作自动编译并聚合所有选定 Variant”也可行，但应由 RadRay 做 discovery、planning、N 次 single-job DXC compile 和 artifact publication，不能误称为一次标准 DXC compile。以当前 forward pass 为例，完整 256-point domain 在 DXIL/SPIR-V 双 target 下是 514 次唯一 stage compile；当前 default + fully-on coverage 只有 6 次。

当前最重要的架构分叉是：RadRay 要保持跨 variant 共享的 **稳定 superset layout**，还是采用每个 permutation 的 **exact active layout** 并把 layout identity 纳入 variant/cache。这个决定先于选择 DXC sidecar 或 C++ trace。

## 版本与调查范围

RadRay 当前工作树的 `project_manifest.json:5` 实际锁定 DXC `1.9.2607`，不是 `1.9.2602`。当前 SDK 记录为 `SDKs/dxc/v1.9.2607/.radray-sdk.json`；仓库还保留历史 `SDKs/dxc/v1.9.2602.24/.radray-sdk.json`。`modules/shader/src/dxc.cpp:3` 的 `v1.9.2602` 注释和 `docs/todo/vertex-interface-projection.md:81` 的 `1.9.2602.24` 是历史依据，不改变当前 manifest。

本次源码核对的主 DXC checkout 是官方 tag `v1.9.2607`，commit：

`0d3ee6b551b8fa768fbf825300ebab81047ef6a8`

checkout：

`C:\Users\xiaoxs\AppData\Local\Temp\radray-dxc-v1.9.2607`

对照 tag：

- `v1.9.2602` -> `21d28f727ad395b59394815ef76012e432f7e4e5`
- `v1.9.2602.24` -> `d355aa8364d34df3f0822ba0de8d1dfc75ae6f48`
- UE vendored DXC `1.8.2403.0` -> `F:\cpp\UnrealEngine` commit `260bb2e1c5610b31c63a36206eedd289409c5f11`

UE 快照的 `utils/version/version.inc:16-26` 是 `1.8.2403.0`。UE 1.8.2403、历史 DXC 1.9.2602 和当前 RadRay 目标 1.9.2607 不能互换验证 compiler、validator 或 shader artifact 行为。`DXC_OUT_EXTRA_OUTPUTS`、`IDxcExtraOutputs` 和 C++ frontend 生命周期在 UE 快照中已经存在，但 1.9.2607 的 validator/发行行为已不同；若 RadRay 要重新锁定 1.9.2602，必须对 `21d28f...` 或实际分发 binary 重新跑 ABI/validator 矩阵。

调查只读了 RadRay、UE checkout、官方 DXC checkout 与实际 `dxc-autobuild` 仓库，并使用官方
DXC/GitHub、CMake 与 Khronos 一手资料；没有实现实验 fork。额外使用未修改的官方
`v1.9.2607` checkout 完成了 CMake `add_subdirectory` 配置和 `dxcompiler`/`dxildll` 实际构建
探针。运行当前 SDK 的 `dxc.exe --version` 得到 `1.9(5402-0d3ee6b5)`，与 tag commit 一致。

## RadRay 现状

`docs/architecture/shader-pipeline.md:44-59` 和 ADR-0003 的当前模型是：manifest 声明 ABI，reflection 只做一致性核对；`BuildPipelineLayoutStorage` 可以不依赖字节码、reflection、target 或 variant 建立共享 layout。

当前反射/manifest 握手涉及：

- `modules/shader/include/radray/shader/dxc.h:17-20` 的公开输出结构实际叫 `DxcOutput`，字段是 `Data`、`Refl`、`Category`；仓库没有名为 `DxcCompileResult` 的公开结构。
- `modules/shader/src/dxc.cpp:476-495` 从内部 `IDxcResult` 只读取 primary `GetResult()` 或指定 standard output blob。
- `modules/shader/src/dxc.cpp:498-537` 只取 object data 与 `DXC_OUT_REFLECTION`，没有 `DXC_OUT_EXTRA_OUTPUTS` decoder/plumbing。
- `modules/shader/src/shader_asset_template.cpp:717-728` 仍以 reflection 构建/检查初始 binding 集合。
- manifest 的 variant domain、artifact index、schema、cache key、AOT/JIT resolver 和 runtime binding 仍由 `modules/shader`/`modules/runtime` 承担。
- `modules/render/include/radray/render/rhi.h` 的 pipeline layout、push constant、vertex input descriptor 形状可以保持不动；这不代表 `modules/shader` 的 18k 行复杂度会由 DXC fork 自动消失。

如果 sidecar 成为 ABI 真相，至少仍需改造 `DxcOutput`、sidecar schema decoder、shader asset/schema、variant resolver、artifact/cache key、AOT/JIT 读取和 runtime binding 入口。RHI descriptor 可以不动，但 shader 模块的 variant/artifact/cache machinery 不会自动删除。

## 一手源码核对

### 自定义 attribute

通用 C++11 parser 通常不需要改。`tools/clang/lib/Parse/ParseDeclCXX.cpp:3848-3921` 已能读 `[[attribute-token(...)]]`；未知 token 会被放入 `AttributeList`。`tools/clang/lib/Sema/AttributeList.cpp:111-145` 通过 TableGen 生成的 kind/spelling 表查询属性。

要让 `[[radray::...]]` 成为 HLSL 的结构化数据，需要：

- 在 `tools/clang/include/clang/Basic/Attr.td` 增加 HLSL attribute、`CXX11<"radray", ...>` spelling、subjects 和 argument/accessor 定义。
- 运行 TableGen，生成 attribute kind、parsed attribute、AST class、spelling index 等代码；`tools/clang/utils/TableGen/TableGen.cpp:160-194` 列出这些生成器。
- 在 `tools/clang/lib/Sema/SemaHLSL.cpp` 的 HLSL attribute handler 中校验参数、声明位置、重复和 stage 语义，并把已知 attribute 留在 AST。
- 增加 AST metadata collector，通常作为 `ASTConsumer::HandleTranslationUnit` 或受控 frontend action 的一部分遍历 declarations/attributes；`tools/clang/include/clang/AST/ASTConsumer.h:67-75` 规定该回调在整棵 translation unit 已解析后调用。

`tools/clang/lib/Sema/SemaDeclAttr.cpp:4569-4593` 对未知 attribute 发 `warn_unknown_attribute_ignored` 并返回；因此未知 attribute 不会自动产生 AST metadata、reflection 字段或 sidecar。

HLSL 的 GNU `__attribute__` 在 `tools/clang/lib/Parse/ParseDecl.cpp:128-150` 明确报 unsupported；HLSL 专用冒号 annotations 在 `ParseDecl.cpp:344+` 只覆盖已有 unusual annotations，不是开放 key/value registry。

**结论**: attribute 方案的 parser 改动可以很小，但 TableGen、HLSL Sema、AST collector 和 result plumbing 是必需的。它不需要假设通用 parser 要为每个 RadRay 字段写新 grammar。

### pragma

`tools/clang/lib/Lex/Pragma.cpp:82-100` 的通用 namespace handler 在没有注册 handler 时发 `warn_pragma_ignored`。HLSL 模式的 builtin 注册在 `Pragma.cpp:1430-1444`，只含 DXC diagnostic、message、region 等，不含 RadRay pragma。

真正的语言 pragma handler 在 `tools/clang/lib/Parse/ParsePragma.cpp:170-260` 的 `Parser::initializePragmaHandlers` 注册，并由 `Parse/CMakeLists.txt:17` 编入 parser。`Parser.cpp:71-86` 在 parser 创建时调用它，`Parser.cpp:431+` 负责移除 handler。

**结论**: `#pragma radray_keyword_group` 要成为 DXC 结构化输入，必须增加 ParsePragma handler、状态保存和 collector 关联；未知 pragma 默认不会进入 AST、DXIL metadata 或 sidecar。stock DXC 的预处理输出能保留未知 pragma 文本，只是 RadRay 自己解析的源侧文本，不是 DXC 的语义输出。

### 插件与内部接口

Clang 源码保留 `PluginASTAction` registry，`FrontendAction.cpp` 能把 requested plugin consumer 加入 `MultiplexConsumer`；但这是 Clang frontend 内部机制，不是 stock `dxcompiler.dll` COM API 的稳定 HLSL plugin loader。当前 `dxcompilerobj.cpp:970-978` 对普通编译直接创建 `EmitBCAction`，没有给 `IDxcCompiler3` 调用者暴露任意 plugin name/loader。

`IDxcLangExtensions` 在 `include/dxc/dxcapi.internal.h:246-290` 只支持 semantic defines、define exclusions、intrinsic tables、target triple；没有任意 attribute parser、AST visitor 或 metadata serializer callback。`IDxcContainerEvent` 在同文件 `:348-364` 只支持编译后 container handler。

**结论**: stock dxcompiler COM 路径没有可依赖的稳定插件接口。可靠的 HLSL attribute/pragma 方案仍是小型 DXC fork，除非项目愿意维护一个与特定 DXC 内部 ABI 绑定的自建插件加载分支。

### AST 生命周期与 sidecar 输出

`tools/clang/lib/Frontend/FrontendAction.cpp:471-496` 的顺序是：

1. 调用 preprocessor 的 `EndSourceFile()`。
2. 调用 action 的 `EndSourceFileAction()`。
3. 清除 Sema、ASTContext、ASTConsumer。

`CodeGenAction::EndSourceFileAction` 在 `tools/clang/lib/CodeGen/CodeGenAction.cpp:655-666` 先从 backend consumer 取走 LLVM module。当前 `dxcompilerobj.cpp:975-978` 调用 `action.EndSourceFile()`，之后 `:1036` 才 `action.takeModule()`。

因此 canonical source metadata 应在 AST 清理边界前完成，具体可由 ASTConsumer 的 `HandleTranslationUnit` 收集，或由 forked action/consumer 在 `EndSourceFileAction` 的可控阶段保存 bytes。不能等 `FrontendAction::EndSourceFile()` 返回后再访问 AST；那时 ASTContext 已被清空。sidecar 的“frontend 生成”与 DXIL backend 生成必须共享一次编译的 source identity、defines、entry point、profile、optimization 和 category。

### `DXC_OUT_EXTRA_OUTPUTS` 是现有 sidecar carrier，不是 stock 自动功能

`include/dxc/dxcapi.h:724-750` 已有 `DXC_OUT_EXTRA_OUTPUTS`，`IDxcResult` 在 `:755-793` 提供 output enumeration，`IDxcExtraOutputs` 在 `:799-826` 提供每个额外 blob 的 type/name/object。

`include/dxc/Support/dxcapi.impl.h:293-372` 有内部 `DxcExtraOutputs` 实现，`DxcResult::SetOutputObject` 在 `:533-542` 可以把一个 `IDxcExtraOutputs` 对象挂到现有 output kind。但当前 `dxcompilerobj.cpp` 只设置 object、reflection、root signature、shader hash、errors/remarks 等 standard outputs；没有设置 `DXC_OUT_EXTRA_OUTPUTS` 的 custom metadata output。全仓库搜索还表明上游没有任何 `DxcExtraOutputs` producer 或对应单元测试；`tools/clang/tools/dxclib/dxc.cpp:225-270` 只有 CLI 消费并把已有 extra output 写到文件的逻辑。

因此推荐的 sidecar 形状是：

1. HLSL attribute/pragma collector 在 AST 清理前生成 canonical, versioned, backend-neutral blob。
2. forked compiler 创建一个 `DxcExtraOutputs`，将 blob 作为 `IDxcBlob`，type/name 明确标识 schema 与 source identity。
3. forked `dxcompilerobj.cpp` 调 `DxcResult::SetOutputObject(DXC_OUT_EXTRA_OUTPUTS, ...)`。
4. RadRay `DxcOutput` 增加 sidecar bytes/decoder，并在同一次 compile result 中同时取得 object、standard reflection 和 sidecar。

这条路径不新增 `DXC_OUT_KIND`，也不要求 sidecar 进入最终 bytecode。`IDxcExtraOutputs` 是现有公开接口；生成 custom output 的 frontend/compiler wiring、producer 生命周期、失败语义和回归测试仍全部属于 fork 内容。它是合适的 carrier，不是现成的 metadata feature。

### LLVM named metadata 与 container part

`HLModule::ClearHLMetadata`、`DxilModule::ClearDxilMetadata` 会按 known names 清理/重建 metadata；linker `DxilLinker.cpp:563-604` 可能复制未知 named metadata，但最终 `lib/DxilValidation/DxilValidation.cpp:4166-4195` 对未知非 `llvm.*` named metadata 发 `MetaKnown` 错误。因此 `!radray.*` 只能是中间实现细节，不能是 validated DXIL ABI。

`DxcContainerBuilder::AddPart` 在 `lib/DxilContainer/DxcContainerBuilder.cpp:57-79` 只允许 debug info/name、root signature、shader statistics 和 `DFCC_PrivateData`。`ValidateDxilContainerParts` 在 `lib/DxilValidation/DxilContainerValidation.cpp:934-1097` 对未知 FourCC 走 `default` 并发 `ContainerPartInvalid`；`DFCC_PrivateData` 被显式跳过。

所以：

- 不新增 custom FourCC。
- 不把未知 named metadata 当 sidecar 或 embedded ABI。
- `DFCC_PrivateData` 只能是 DXIL 专属 opaque carrier，不能替代跨后端 sidecar；它会把 metadata 与最终 bytecode 绑定，并增加 strip/repack/hash/读取风险。
- sidecar 与 embedded private data 必须作为两个不同设计分别评估。

### SPIR-V

DXC 的 `SpirvEmitter.cpp:681-695` 只在 debug/tool 选项组合满足时使用 `OpModuleProcessed` 写 commit/options 字符串，`EmitVisitor.cpp:719-724` 原样编码。Khronos SPIR-V specification 将 `OpModuleProcessed` 归入 module/debug annotation 的字符串信息，不是 RadRay 的 resource/layout schema。

**结论**: `OpModuleProcessed` 既非结构化资源 metadata，也不是无条件输出；其出现受选项影响。它不能作为 sidecar 的跨 backend carrier，更不能作为“metadata 嵌进 SPIR-V bytecode”的可靠契约。

### DXIL 与 SPIR-V 共用 collector 的真实边界

`dxcompilerobj.cpp:938-977` 显示两条 codegen lane 使用不同 action：SPIR-V 是
`EmitSpirvAction`，DXIL 是 `EmitBCAction`。二者都继承 `ASTFrontendAction`；
`FrontendAction::CreateWrappedASTConsumer` 已能用 `MultiplexConsumer` 把额外 consumer
并到原 consumer 后面。因此 fork 可以让两条 action 共用同一个只读 metadata collector，
而不在 DXIL LLVM lowering 与 SPIR-V emitter 中各实现一份 schema 语义。

但“共用 frontend collector”只保证 author declaration 是 backend-neutral，**不等于它能输出
post-DCE active resources**：

- DXIL 的 `DXC_OUT_REFLECTION` 在 `EmitBCAction` 完成、DXIL assembly/validation 之后才设置；
- SPIR-V lane 不设置 `DXC_OUT_REFLECTION`。RadRay 当前在 `shader_manifest.cpp:3761-3786`
  对最终 SPIR-V bytecode 调 `ReflectSpirv`；
- 因而 AST collector 产出的应是 `DeclaredContract`。逐 job 的 active resource 集合仍来自最终
  DXIL/SPIR-V reflection，或来自另一个明确实现并验证过的 entry reachability analysis；不能
  把 AST 中“声明存在”写成“最终 active”。

最小 fork 可用派生 action 覆盖 `CreateASTConsumer`，让 base consumer 与 collector 共享一个
输出 sink；`HandleTranslationUnit` 在 AST 清理前完成 canonical serialization，之后
`dxcompilerobj.cpp` 才把 blob 包装成 `DxcExtraOutputs`。这条实现仍需分别接入两个 action 的
创建点和共同的 `DxcResult` 组装点，但 schema/collector 只应有一份。

### HLSL Root Signature 是 DXIL policy 真相，不是 active projection

DXC `v1.9.2607` 已原生支持入口函数上的 `[RootSignature(...)]`：
`CGHLSLMS.cpp:2556-2559,6145-6164` 只为 entry/export 解析 attribute，编译 Root Signature
1.0/1.1，并把序列化结果写入 DXIL module/container；`dxcompilerobj.cpp` 还能通过
`DXC_OUT_ROOT_SIGNATURE` 单独返回 root signature。它已经覆盖 descriptor table、root
descriptor、root constants 与 static sampler，因此 DXIL 路线不需要再造一套
`[[radray::root_descriptor]]` policy 语法。

但 stock validator 的方向仍是 **Root Signature 覆盖 shader active resources**：
`DxilRootSignatureValidator.cpp:623-780` 对 shader PSV 中的 sampler/SRV/UAV/CBV 查找 covering
range，缺少覆盖时报错；它不会因为 Root Signature 里有 shader 未使用的额外 parameter/range
而失败。故原样序列化 HLSL Root Signature 只能保证 compatible superset，不能自动兑现
exact-permutation。

若 RadRay 选择 HLSL Root Signature + exact-permutation，fork 必须在 compiler 内把作者声明的
Root Signature 当作 DXIL `DeclaredContract`，用 Variant 所有 active stages 的最终资源并集做
projection：保留 active binding 对应的 root parameter/range/static sampler，移除空项，重新验证
后为全部 stage bytecode 和 sidecar 输出同一份 Variant-level serialized Root Signature。该
projection 是新的 DXC fork 功能，不是 stock DXC 已有行为，也不能下放给 cook/runtime。

push/root constants 的 cardinality 后续按 target 分开确认。DXC `docs/SPIR-V.rst:224-229,357-359`
说明 Vulkan shader 最多静态使用一个 `[[vk::push_constant]]` block；pipeline layout 的多个
`VkPushConstantRange` 只是同一 push-constant address space 的 offset/stage ranges。这个限制不能
反向套给 D3D12：DXIL lane 允许 HLSL Root Signature 包含多个 active `RootConstants` parameters，
每个 parameter 由 compiler 与对应 `register(b#, space#)` 上的 active HLSL constant declaration
关联并原样进入 projected Root Signature。两 target 的 inline-constant 数量、位置、名称和 byte
layout 不要求一致。当前 `PipelineLayoutDescriptor::PushConstant` 的单一 optional 形状属于待迁移
RHI 限制，不再构成 compiler contract。

RootSignature 的 target scope 也已确认：HLSL `[RootSignature(...)]` 只属于 DXIL lane，允许在
`__spirv__` 条件下排除；SPIR-V pipeline layout 只由标准 `vk::binding` / `vk::push_constant`
和 SPIR-V compiler output 构成，不要求有等价 RootSignature 字符串。DXIL graphics entries 仍
必须在同一 Variant 内解析为同一份 projected RootSignature，compute entry 单独解析自己的版本；
两 target 的 layout 不做强制同构。

runtime/RHI 的 layout 边界也已确认：compiler target result 解码后保持 D3D12 与 Vulkan 的
target-native pipeline-layout representation，直接交给对应 backend。RHI 仍可提供跨 backend 的
command operations，但不能再用统一 `PipelineLayoutDescriptor` 作为 compiler metadata 的权威
投影；它无法无损表达 RootConstants、push-constant、register/space、set/binding 与 vertex
signature/location 的差异。任何公共 binding handle 都必须在当前 target artifact 上解析。

caller-facing binding identity 已确认使用 canonical HLSL declaration name。compiler 为当前
target/Variant 的每个 active resource 输出该名称，runtime 在加载期把它解析为 artifact-local
`BindingHandle`；提交路径不再做字符串查找。handle 不保证跨 target、Variant 或重编译数值稳定，
layout 改变后必须重解析，inactive resource 查找失败。manifest/caller 不再提供 binding alias，
register/space 与 set/binding 只存在于各自 target payload 中。

identity 最终分为三层。`ContractHash` 覆盖 canonical keyword domain、entry topology 与影响
discovery 的 Defines/policy，用来证明 assignment planning 针对哪一版 contract。每个 target
lane 的 `CompileInputHash` 覆盖 `SourceName`、root bytes、该 lane/Variant 实际打开的规范化 include
path/content、canonical assignment/Defines、target/environment、resolved policy，以及 fork
commit、extension ABI、metadata schema、validator/toolchain identity。`ArtifactContentHash` 对
最终 bytecode blob 与 metadata blob 的 deterministic bytes 计算，作为内容寻址和完整性依据；
metadata envelope 保存 CompileInputHash 与配对 bytecode hash，ArtifactContentHash 由 result/
外层索引保存以避免自引用。`AssetId` / `PassName` 不进入任何 compiler hash。

runtime metadata 的 type scope 随后扩大：每个 target result 必须包含 runtime 构造 CPU buffer
data 所需的完整 target-native cbuffer/struct type tree，包括 nested member name、offset/size、
array/matrix stride 与 scalar/vector/matrix shape；它不是 inspection-only output。用户明确选择
ODR-style invariant，而不是独立 schema identity：runtime 不从 C++、其他 artifact 或 reflection
API 获取第二份 type tree，也不交叉验证；type tree 不设置 `CpuSchemaHash`，不独立缓存/寻址或
跨 Variant 复用，必须与 target result 原子交付和存活。为避免名称误导，GPU 内容身份称为
`GpuArtifactHash`，只覆盖 bytecode 与 GPU layout metadata；CPU type tree 不进入该 hash。错配
属于 publisher/loader 的系统缺陷，而不是 runtime validation case。

### Root Signature static sampler 到 Vulkan immutable sampler 的桥接

`DxilStaticSamplerDesc`（`DxilRootSignature.h:296-308`）包含 filter、U/V/W address mode、
MipLODBias、MaxAnisotropy、ComparisonFunc、BorderColor、Min/MaxLOD、D3D shader register 与
register space。其大部分数值可映射到 `VkSamplerCreateInfo` 及
`VkDescriptorSetLayoutBinding::pImmutableSamplers`；但映射不是只复制一个 binding number：
Root Signature 的 shader visibility 在 Vulkan 归属于 descriptor binding 的 stage flags，且
RadRay 当前 `SamplerDescriptor` 尚未保留 MipLODBias 与完整 BorderColor 等字段，若要无损桥接
必须扩展 compiler metadata/RHI sampler schema。

更重要的是 target binding 数字已允许不同。Root Signature static sampler 只有 D3D
`s#/space`，而 SPIR-V sampler 使用独立的 `[[vk::binding(binding, set)]]`。因此 compiler 必须
把 static sampler 与同一 HLSL `SamplerState` declaration 关联，再从该 declaration 的 SPIR-V
attribute 取得 Vulkan binding；不能以两套数字相等作为关联。没有对应 active sampler declaration
的 static sampler，或一个 static sampler 匹配多个 SPIR-V sampler，必须 fail closed。

projection 完成后，DXIL artifact 可以携带 projected Root Signature，SPIR-V metadata 则携带由
同一 sampler declaration 反推出的 immutable sampler descriptor；runtime 只用当前 target 的
metadata 创建 native sampler，不做跨 target 反射/合并。

### 一个 Variant request 同时生成两种 target

stock `IDxcCompiler3::Compile` 一次接收一个 profile/target，`dxcompilerobj.cpp` 的 DXIL 与
SPIR-V 也走两个不同 `ASTFrontendAction`（`EmitBCAction` / `EmitSpirvAction`）。因此“单次
请求同时输出 DXIL、SPIR-V 及双方 metadata”不是现有 API 行为，需要 fork 增加 batch/variant
output contract，例如：

```text
VariantCompileResult
  Common: schema + source/assignment identity
  DXIL:   object + projected RootSignature + target metadata
  SPIR-V: object + target metadata
```

这个 batch request 可以在 compiler 内共享 source/include 读取、assignment 校验、entry/domain
discovery、canonical contract collector 和诊断汇总；但 current HLSL 的 `VULKAN` 条件宏会让
DXIL/SPIR-V 预处理结果不同，不能未经设计就假定一个 AST 同时喂给两个 backend。最稳妥的实现
是一个外部 compiler request 内运行两个 target lanes，各 lane 独立 `CompilerInstance`/codegen，
共享只读输入和 schema sink；之后再评估 target-neutral AST/consumer multiplex 是否值得。

因此 batch 能减少 RadRay/COM 启动、include 扫描与重复 discovery 的开销，但 backend lowering、
optimization、validation 仍各执行一次。AOT cook 可以请求 `{DXIL, SPIR-V}`，runtime JIT 只请求
当前 backend，避免运行时为另一 target 额外编译。

### 用独立头文件隔离 fork API

“不修改 DXC 原有对外 API”需要分成两层：

- 保持 upstream `dxcapi.h`、现有 CLSID/IID、`IDxcCompiler3` vtable 和两个 DLL export 不变：可行；
- 只增加一个头文件、完全不修改 forked `dxcompiler` 实现：不可行。compiler-internal batch、stage
  merge 与 metadata 生成必须有二进制实现，头文件本身不能给 COM object 增加行为。

DXC 现有工厂正好允许把扩展隔离出去。`dxcapi.cpp:69-107` 的
`ThreadMallocDxcCreateInstance` 按 CLSID 分派，`DxcCreateInstance` / `DxcCreateInstance2` 只是稳定
入口；`dxcompilerobj.cpp:2031-2038` 再由目标 object 的 `QueryInterface` 返回请求 IID。因此 fork
可以新增 `dxcapi_radrayext.h`，在其中声明 RadRay 自己的 `CLSID_RadRayDxcCompiler`、
`IRadRayDxcCompiler` 和 result interface，调用方仍通过原有 `DxcCreateInstance` 创建：

```text
DxcCreateInstance(
  CLSID_RadRayDxcCompiler,
  IID_IRadRayDxcCompiler,
  ...)
```

fork 只需在 factory dispatch 中识别新 CLSID 并创建独立 implementation。这样现有
`CLSID_DxcCompiler -> IDxcCompiler3` object 连 `QueryInterface` 都不需要加分支；stock DXC 对该
CLSID 返回 `REGDB_E_CLASSNOTREG`，RadRay 可以 fail closed，避免把错误版本的 DLL 当成兼容
compiler。新 header 与 forked DLL 的 extension ABI version 必须进入 toolchain/artifact identity。

可行路线的边界如下：

| 路线 | upstream API 稳定性 | 主要代价 |
|---|---|---|
| `IDxcCompiler3` + 私有 arguments + `DXC_OUT_EXTRA_OUTPUTS` | 不改 header/vtable | 把 multi-stage/multi-target batch 塞进“单 entry/单 target”语义；output 靠 type/name 字符串约定 |
| `dxcapi_radrayext.h` + 新 CLSID + 新 COM interface | 不改任何现有 interface/export，隔离最完整 | fork factory 新增一个 class；RadRay 必须随 DLL 分发匹配的 header/schema |
| 新 header + 新 IID，挂到 `CLSID_DxcCompiler` | 不改 `IDxcCompiler3` vtable | 要修改现有 `DxcCompiler::QueryInterface`，标准 object 与 fork 扩展耦合 |
| 新 header + 新 DLL export | 不改现有 interface | 必须修改 `.def`/动态符号加载，另建 ownership/calling-convention contract，没有复用现有 COM factory 的收益 |

推荐第二条。extension interface 应直接继承 `IUnknown`，不要继承或扩展 `IDxcCompiler3`；request
跨 ABI 只使用定长 POD、`DxcBuffer`、`LPCWSTR` 数组、`IDxcIncludeHandler` 和 COM pointers，不能
暴露 STL、异常或 compiler-internal LLVM/Clang 类型。result 同样应提供 target/kind 的强类型查询
并返回 `IDxcBlob`/COM object；实现内部是否复用 `DxcResult`、`IDxcExtraOutputs` 是实现细节，不能
让 `DXC_OUT_KIND` 的 upstream enum 被迫增加 RadRay 成员。

这个设计同时保留标准 DXC 路径：需要普通单 entry 编译的工具仍可创建 `CLSID_DxcCompiler`；
RadRay Pass/Variant pipeline 只创建 `CLSID_RadRayDxcCompiler`。两条 object contract 不互相冒充，
也不需要依赖“某个私有 argument 是否碰巧被当前 DLL 接受”来探测能力。

后续 grilling 已确认 result ABI 也完全分离：`IRadRayDxcResult` 直接表达 batch、target lane、
stage output 与 metadata 的层级；`IDxcResult`、`DXC_OUT_KIND`、`IDxcExtraOutputs` 只允许作为实现
细节，`IDxcBlob` 等 upstream COM object 可以继续作为叶子数据载体。这样不会把
`target + stage + output kind` 退化为 extra-output type/name 字符串协议。

持久化边界也已确认：每个 target lane 的 bytecode 与 metadata 是 forked DXC 直接生成的两个
独立 blob，metadata 使用 RadRay extension 定义的 canonical、versioned wire schema，但不嵌入
DXIL/SPIR-V container。cook 或其他 publisher 不得根据 structured getters 二次序列化 metadata；
它只能校验、内容寻址并原样发布 compiler bytes。JIT 使用完全相同的 decoder 读取同一种
metadata blob，因此 compiler 是字节级 artifact authority，而不只是内存字段值的来源。当前
实现优先级不包含 cook；首期先完成 compiler extension、wire decoder 与 runtime JIT/direct
consumption，publisher 留作后续阶段，但不能因此另设临时 JIT schema。

wire schema 不把“target-native”误解为持久化 SDK 内存布局。两条 lane 只共享包含 magic、schema
version、target kind、compiler/toolchain 与 contract/Variant identity、payload bounds 和完整性
摘要的薄 envelope；DXIL 与 SPIR-V payload 是两套独立 schema，不要求字段同构。标准 serialized
Root Signature 已经是稳定的 D3D wire representation，DXIL lane 直接复用；其他 DX reflection
facts 与 Vulkan descriptor/push-constant/vertex/immutable-sampler facts 使用 RadRay-owned、固定
宽度的 offset/index records。`D3D12_ROOT_SIGNATURE_DESC`、`VkDescriptorSetLayoutBinding` 等含
pointer 的 runtime structs，以及 `VkSamplerCreateInfo::pNext` chain，不得被 `memcpy` 进 artifact；
decoder 再据 records 构造当前 backend 所需的临时 native structures。

### Stage entry 可以复用标准 HLSL attribute

DXC v1.9.2607 已有标准 `HLSLShader` attribute：`Attr.td:866-868` 的 spelling 是
`[shader("...")]`；Sema 在 `SemaHLSL.cpp:14218-14253` 校验 stage 字符串和重复声明，现有测试也
直接覆盖 `[shader("vertex")]`、`[shader("pixel")]` 与 `[shader("compute")]`。fork 的
backend-neutral discovery action 可以遍历带 `HLSLShaderAttr` 的顶层 function，取得 entry name
与 stage，无需新增 `[[radray::stage(...)]]` 或继续依赖外部 entry list。

这里复用的是 entry 的标准 stage 语义，不等于 stock `IDxcCompiler3::Compile` 已能一次输出整个
Pass。标准 API 仍由 `-E/-T` 选择单 entry；RadRay extension 需要先 discovery，再为当前 Variant
的每个 entry 启动对应 target lane，并在 compiler 内聚合结果。若一个 source unit 没有 entry、
同一 stage 声明多个 entry，或 graphics 与 compute entry 混在同一 Pass，如何处理仍需由 RadRay
Pass contract 明确并 fail closed。

后续 grilling 已确认：Pass entry 只使用标准 `[shader("...")]`，不新增 RadRay stage attribute，
不从 `VSMain`/`PSMain` 等函数名推断，也不由 cook/runtime 传入 entry/stage list。

Pass entry cardinality 也已确认：graphics source unit 恰好一个 vertex entry、至多一个 pixel
entry（depth-only 可以省略 pixel）；compute source unit 恰好一个 compute entry。graphics 与
compute 不得混合，同一 stage 不得多 entry，当前 RHI 未支持的 stage 一律 fail closed。

entry topology 必须跨 keyword domain 稳定：entry declaration/name/stage 不能受 Variant 条件
控制，Pass 也不能通过 keyword 在 graphics 与 compute 间切换。keyword 仍可改变函数体、资源
使用、类型字段和 VS/PS interface；每个具体 Variant 的 stage interface 由 compiler 单独校验。

### Keyword domain 继续使用 compiler-recognized pragma

keyword assignment 最终控制 `#if/#ifdef`，其合法域属于 preprocessing contract，不适合依附在
一个虚构 AST declaration 上。Clang preprocessor 已提供正式扩展点：`Preprocessor.h:928-944`
允许注册/移除 `PragmaHandler`，`Pragma.h:59-68` 定义 handler interface，`Pragma.cpp:82-100`
负责 namespace dispatch 和 unknown-pragma diagnostics。因此 fork 可以直接接管现有
`#pragma radray_keyword_group`，在 token/preprocessor 层完成 typed parse、source location
diagnostics 与 contract collection，不再让 RadRay 扫描预处理后的文本。

后续 grilling 已确认保留该 pragma 作为 HLSL keyword-domain 真相。它必须位于 Variant 条件
之外；重复 group、keyword 跨组重复、非法 assignment 和语法错误都是 compiler hard error。
compiler 输出 canonical domain，cook/runtime 只提交 assignment。当前
`shader_asset_template.cpp` 的 `ParseShaderKeywordPragmas` / `StripShaderKeywordPragmas` 外部
parser 在迁移完成后删除。

后续 grilling 已确认删除 `stages(...)` modifier。keyword assignment 一致传给 Pass 的所有
entries；初版允许编译全部 entries 后按 content hash 去重。若后续需要避免未受影响 stage 的
编译，必须由 compiler 生成 per-stage dependency fingerprint。最终 resource stage visibility
同样来自各 stage 最终产物的 compiler merge，不能由 keyword pragma 手写。

keyword group declaration 的 source scope 也已确认收窄为 Pass 根 `.hlsl`：`.hlsli` 中出现
`#pragma radray_keyword_group` 是 compiler error。共享 include 可以消费宏，但每个需要该 feature
的 Pass 必须在自己的无条件 contract preamble 声明 group。这样 `error_pass.hlsl` 不会因为
包含 `view.hlsli` 就继承完全未使用的 shadow domain；compiler 可用 pragma `SourceLocation` 与
main-file identity 直接执行该规则。

Variant macro input 的 API 也已确认分流：`CompileVariantRequest.KeywordAssignments` 携带经过
domain 校验的 `group -> choice/off`；`CompileVariantRequest.Defines` 携带普通 `name -> value`
预处理宏。调用方不能再通过 raw compiler arguments 传 `-D`，普通 define 也不能覆盖 domain
keyword。两组输入都由 compiler 排序/canonicalize 并进入 artifact identity。常量 `PI/E` 通常
应留在 HLSL，逐帧 `TIME` 应走 buffer/push constant；只有真正的 compile-time environment 才走
普通 Defines channel。

compile policy 的 API 同样已确认结构化：RadRay extension 不提供 raw DXC arguments。
shader model、optimization/debug、warnings、resource-binding assumptions、DXIL settings 与
SPIR-V target environment 由 caller/build profile 以 typed fields 提供，compiler 将 resolved
policy 纳入 result/artifact identity。当前 `ShaderPassDesc::ShaderModel` / `IsOptimize` /
`EnableUnbounded` 从作者 manifest 字段迁出；其中 `EnableUnbounded=false` 实际对应
`-all_resources_bound`，本质是 caller 对 descriptor 完整绑定的保证，并非 HLSL resource
declaration。任意 flag 实验继续使用标准 `IDxcCompiler3`，不得发布为 RadRay artifact。

### Vertex shader interface 与 vertex buffer layout 必须拆分

标准 HLSL vertex entry 能提供 semantic/index 与逻辑输入类型；SPIR-V lane 还可读取
`[[vk::location(...)]]` 或 compiler 分配的 final location。这足以由 compiler 生成当前
target/Variant 的精确 `ShaderVertexInterface`。但 HLSL function signature 不表达以下实际 buffer
解释：vertex buffer binding、array stride、per-vertex/per-instance step mode、attribute byte
offset，以及 GPU storage format。尤其 `float4 COLOR0` 既可能来自 `FLOAT32X4`，也可能来自
`UNORM8X4`，单看 AST/最终 shader interface 无法无歧义反推。

RadRay 当前 `ShaderVertexInputDesc` 正好混合了这些 mesh/PSO 决策：
`ShaderVertexBufferDesc` 保存 binding/stride/step mode，`ShaderVertexAttributeDesc` 保存
format/buffer/offset；RHI 随后原样转成 `D3D12_INPUT_ELEMENT_DESC` 和 Vulkan
`VkVertexInput*Description`。现有 ADR-0013 也已承认 `ShaderVertexInterface` 只描述 shader
requirements，而 vertex format/slot/offset/stride 属于未来 `PrimitiveVertexLayout` 连接。

若为了删除 manifest 而给 HLSL 新增 buffer-layout attributes，会让同一个 layout 同时写在 shader
和 mesh schema，并把一个本可接受 interleaved/split、float/normalized 多种输入的 shader 锁死到
单一物理布局。这会重新制造跨资产握手。更干净的 contract 是：compiler metadata 只输出精确
shader interface；mesh/primitive 提供实际 vertex layout；PSO builder 校验兼容性并生成
`VertexInputState`，其 resolved layout identity 进入 PSO key。这样作者维护的 shader JSON 仍可
删除，但不会错误声称 mesh 字节布局能从 HLSL 推导。

后续 grilling 已确认该边界，并进一步要求 shader processing 不出现任何实际 buffer 字节布局：
compiler 只随 target result 输出最终 vertex interface/reflection；binding/stride/step mode/storage
format/offset 全部归 primitive/mesh，旧 manifest `VertexInput` 删除，也不新增对应 HLSL attribute。

vertex reflection 也不再强制归一为跨 target 同构记录。DXIL result 保留 semantic/index、signature
register、component/mask/read-mask 等 native facts；SPIR-V result 保留 final location、type 与
decorations 等 native facts。两者的 DCE 结果、parameter 集合和编号空间允许不同，consumer 只
解释当前 backend 的 compiler output。所谓“原样反射”发生在 compile/cook 并随 artifact 交付，
不是 runtime 再调用 DXIL/SPIR-V reflection API。

### SPIR-V target 条件宏

DXC v1.9.2607 `tools/clang/lib/Frontend/InitPreprocessor.cpp:400-408` 在
`LangOpts.SPIRV` 下内置定义：

```text
__spirv__
__SPIRV_MAJOR_VERSION__
__SPIRV_MINOR_VERSION__
```

`__spirv__` 是本仓库 shaderlib 应使用的 target gate；RadRay 不应继续要求调用方手工传
`-DVULKAN`。当前 `shaderlib/core/platform.hlsli:10-20` 使用的 `VULKAN` 在仓库编译路径中没有
定义，故 `VK_BINDING` / `VK_PUSH_CONSTANT` 当前实际多为空展开；迁移到 `__spirv__` 会正式
启用 HLSL `vk::binding` / `vk::push_constant`，必须作为预期的 SPIR-V ABI 行为变更配套测试
set/binding、push constant 和 Root Signature/static sampler bridge。

在 multi-target batch request 中，DXIL lane 不定义 `__spirv__`，SPIR-V lane 由 DXC 自己定义它；
RadRay 不应复制或覆盖这些保留宏。若未来需要区分其他 target，应新增各 target compiler 的
正式 predefined macro，而不是复用 `VULKAN` 这种外层 build define。

## RadRay 七类信息矩阵

下面的“作者 metadata”表示 HLSL 作者/外层 shader contract 明确声明；“可推导”表示当前 compile 的 AST、compiler arguments 或最终 reflection 能给出的事实；“逐 Variant”表示若要得到该项的当前宏配置或 exact active 结果，必须对每个实际编译 identity 处理，不能从一次预处理结果推断所有 `#ifdef` 分支。

| 类别 | 作者 metadata | 当前 compile 可推导 | 必须逐 Variant 编译/合并 | sidecar 能否覆盖 |
|---|---|---|---|---|
| 1. push constant 身份 | 是。应声明哪一个 cbuffer 是 push constant，以及 location/size/stage 约束 | 部分可从当前声明和 `vk::push_constant` 看出；DXIL reflection 不提供统一身份 | 如果声明位于 `#ifdef`，必须每个宏配置收集或把 contract 放在 guard 外；exact layout 仍按 variant 校验 | 能，但字段属于作者 ABI，不是 reflection 的自然产物 |
| 2. binding residency | 是。descriptor table/root descriptor 等是 RHI 性能意图 | 不能从 DXIL/SPIR-V resource usage 唯一推导 | 只有当作者 declaration/contract 随 variant 改变时逐 variant 收集 | 能，sidecar 是合理载体 |
| 3. immutable/static sampler | 是。是 pipeline layout/backend 决策，不是普通 shader resource reflection | 不能由 sampler declaration 唯一推导 | 若 sampler contract 随 variant/stage 改变，逐 variant 校验 | 能，但需要 sidecar schema 与 RHI sampler 描述 |
| 4. unbounded 实际容量 | 是。实际容量来自 engine/backend 限制；reflection 只能显示 unbounded 形状 | 不能从 shader 单独推导容量 | 每个 variant 的使用类型/阶段仍需编译核对；容量通常是稳定 contract 字段 | 能，但不是 compiler 自动计算值 |
| 5. DCE/`#ifdef` 后仍保留的 binding | 是。稳定 superset 时作者必须声明“即使当前 variant 没使用仍保留” | 当前 reflection 能给 exact active subset；不能给被宏排除的分支，也不能凭空给 superset | exact active 必须当前 variant 编译；稳定 superset 需要 guard 外 contract 或多轮编译 union | 能，但必须明确 sidecar 输出的是 superset、active 还是两者 |
| 6. VertexFormat/slot/offset/stride | 是。完整 vertex ABI 需要作者/mesh contract；semantic/type/mask 不足以推出 UNORM、slot、stride | semantic、component type、mask/location 等部分可推导；完整 vertex format/packing 不能 | 如果 vertex interface 随宏改变，逐 variant 生成/合并；稳定 pipeline layout 仍需固定 contract | 能，适合保存作者 ABI和当前 stage projection，但不能假称都来自 DXC |
| 7. entry point/keyword domain | entry point 名是 compile input；keyword domain/合法组合/bake set 是外层声明 | 当前 compile 可得到 entry point、defines 和当前宏配置；不能得到未启用的 `#ifdef` 语义 | 当前 artifact 必须逐 variant；domain 需 HLSL 外层 pragma/attribute 或多轮 compile union | entry point/当前 identity 能；完整 domain 不能由单次 compile 自动产生 |

### 当前 manifest 字段不能整体搬进 HLSL

对 `ShaderAssetDesc` / `ShaderPassDesc` 的实际字段逐项归属后，删除手写 manifest 的目标可分为：

| 当前字段 | 推荐 owner | DXC/HLSL 路线中的形态 |
|---|---|---|
| `FormatVersion` | generated schema | sidecar/index header；不手写 |
| asset `Name` / `Source` | asset registry 与编译请求 | 从注册路径/输入取得；不应由 shader 重复声明自身路径 |
| asset `Name` / pass `Name` | asset system / caller | caller-owned identity；仅由 cook/资产索引映射到 compiler artifact，不进入 HLSL、RadRay DXC ABI 或 artifact identity |
| stage、entry point | shader contract | 标准 `[shader("...")]` entry attribute；discovery 输出 |
| `KeywordGroups`、合法值域、stage applicability | shader contract | guard 外 pragma/attribute；discovery 输出 |
| `BakeVariants` | build profile + content usage | planner coverage policy；不能搬进 HLSL 冒充 shader fact |
| `ShaderModel`、`IsOptimize`、target category | build/toolchain profile | compile request 与 artifact identity |
| `Defines` | build/pass request | compile request；只有合法 domain 的定义属于 shader contract |
| `EnableUnbounded` | toolchain/backend policy | compile request；具体 unbounded resource 的逻辑容量另见下项 |
| binding name/group/slot/type/声明数组长度 | HLSL declaration + reflection | compiler 推导并校验，不再手写第二份 |
| unbounded 逻辑容量、residency、immutable sampler | shader/RHI contract | 声明局部 attribute；无法由普通 reflection 唯一推导 |
| binding stage visibility | aggregate active compile facts | 按 stage/job reflection 合并；稳定 superset 时再按已选规则求 union |
| push constant 身份与 D3D location | shader/RHI contract | cbuffer/global attribute；size 从类型布局推导并跨 target 校验 |
| vertex semantic/scalar shape | shader stage interface | entry signature/reflection 推导 |
| vertex `Format`、buffer、offset、stride、step mode | mesh/primitive schema | 不放进 HLSL sidecar；PSO 连接时由 mesh schema 提供并与 stage interface 校验 |

因此 sidecar 路线可以删除 `*.shader.json` 这个**手写聚合输入**，但仍会有机器生成的
`ShaderContract`、逐 job facts 与 artifact index。它们是输出/索引，不应继续叫 manifest，
也不应把 build、mesh、content policy 伪装成 compiler metadata。

### Attribute 语法应限制 fork 面积

`Attr.td` 已提供 `StringArgument`、整数参数、`EnumArgument` 与 variadic 参数，固定位置参数可由
TableGen 生成 AST class/accessor。若追求 `key = value` 形式的命名参数，则要设置
`HasCustomParsing` 并在 parser/Sema 增加自定义 grammar，显著扩大 fork 面积。

因此最小候选应只对**贴在某个 declaration 上且无法推导**的字段使用少量、固定形状 attribute，
例如 residency、unbounded capacity、push-constant identity、immutable sampler policy；
pass/domain 这类 translation-unit 级声明用 guard 外 pragma 或 function attribute。不要把完整
`SamplerDescriptor`、build profile 或 mesh layout 编成一条巨大的字符串 DSL，否则只是把 JSON
parser 搬进 compiler。

### 单次预处理的硬边界

`IDxcCompiler3::Compile` 每次接收一组 arguments/defines；RadRay 的 `DxcCompileOptions` 也只有当前 `Defines` span。预处理器只对当前宏配置展开。`#if` 未选中的声明不会进入该次 AST，因而“compile 一次，完整解析所有 `#ifdef` variants”不可行。

RadRay 当前测试已经把这个边界写成行为：`test_shader_asset_template.cpp:577-625` 通过自动 probe 多轮 defines 恢复 `#ifdef` binding；keyword groups 仍来自 pragma，见 `:624-625` 和 `:682-723`。关闭 probe 会缩小 binding 集合，但 keyword domain 仍可单独存在。sidecar 不会自动删除这套 variant domain、合法组合、stage projection 或 bake-set machinery。

## “一次操作编译全 Variant”的可行边界

### 一次用户操作不等于一次 DXC invocation

“作者在 HLSL 声明完整 variant domain，点击一次后自动编译全部 permutation 并聚合 metadata”在产品操作层面可行，但不能实现为单次标准 `IDxcCompiler3::Compile`。`dxcapi.h:834-856` 明确把一次调用描述为编译 single entry point、library、root signature 或执行一次 preprocess；调用只有一组 arguments。`dxcompilerobj.cpp:940-977` 也只按本次 options 创建一个 SPIR-V 或 DXIL frontend action，执行一次 `BeginSourceFile` / `Execute` / `EndSourceFile`。

正确分层是：

```text
one user request
  -> discover one source contract and finite variant domain
  -> combine it with a requested coverage set
  -> project and deduplicate (variant, stage, target) jobs
  -> N independent IDxcCompiler3::Compile calls
  -> validate and aggregate per-job metadata
  -> publish artifacts and one aggregate index
```

这里的“聚合”是 RadRay build/artifact protocol，不是把 N 次编译伪装成一个 `IDxcResult`。每个 object、reflection 和 sidecar 仍绑定唯一的 source identity、defines、entry point、stage、target 和 compiler options；aggregate 只引用这些不可混淆的 job records。

### 推荐模块边界

| 模块 | 输入与输出 | 必须拥有的责任 | 不应拥有的责任 |
|---|---|---|---|
| `ShaderContractDiscovery` | HLSL/include closure -> versioned `ShaderContract`、domain、contract hash | 解析 guard 外的 keyword groups、entry points、stage applicability 和作者声明 ABI；拒绝重复/矛盾声明 | 不选择发布 coverage，不编译笛卡尔积，不读取材质数据库 |
| `ShaderVariantPlanner` | contract + build profile + content usage -> 去重后的 compile jobs | 区分 domain 与 coverage；应用合法性约束、fallback、stage projection、target 展开和 artifact-key 去重 | 不解析 HLSL AST，不调用 backend reflection，不写最终 artifact |
| `ShaderCompilerBatch` | compile jobs -> 每 job 的 object、reflection、sidecar、diagnostic | 有界调度多个独立 `IDxcCompiler3::Compile`；校验 result identity；保持 DXIL/SPIR-V lane 分离 | 不决定哪些 variant 值得发布，不把部分结果冒充完整 batch |
| `ShaderArtifactPublisher` | 完整 batch results -> blobs + aggregate index | 校验 contract/job/artifact hash，复用内容寻址缓存，在 batch 成功后发布一致 index | 不重新解释 HLSL，不从 bytecode反推作者策略 |

`ShaderContractDiscovery` 可以使用 small DXC fork 的 AST-only/discovery action，或复用每次 compile 的 canonical sidecar schema；但 domain declaration 必须位于它所声明的 keyword guards 之外。若 group/约束本身藏在 `#if` 后面，就再次出现“先知道 domain 才能发现 domain”的循环，只能增加显式 meta-domain 或多轮 probe。

后续 grilling 已确认把 discovery 固化为 RadRay DXC extension 的独立 compiler-owned operation：
`DiscoverSourceContract` 接收 source、include handler、普通 `Defines` 与相关 policy，输出 canonical
keyword domain、entry topology 和 `ContractHash`，但不编译 DXIL/SPIR-V。cook/editor 只消费这个
result 来规划 concrete assignments，不再保留自己的 pragma parser。`CompileVariant` 携带
`ExpectedContractHash`，compiler 在实际编译时重新 discovery 并校验 contract 与 assignment，
从而关闭 source 在两阶段之间变化的 TOCTOU 缺口。AOT runtime 使用 cook 保存的 contract/index，
无需加载 compiler。

contract discovery 的 target invariant 也已确认：在同一 source/include/Defines 下，DXIL mode
（不定义 `__spirv__`）和 SPIR-V mode（由 DXC 内建定义 `__spirv__`）必须给出相同的 keyword domain、
entry names/stages、graphics/compute kind 与 cardinality。extension 在 `DiscoverSourceContract`
中执行两次轻量 frontend discovery，比较一份 canonical contract hash；差异直接 hard error。
`__spirv__` 只允许影响函数体、资源/binding、RootSignature projection、类型布局和 stage
interface。普通 `Defines` 是 discovery 输入，因此替换 Defines 会形成不同 contract identity，
但同一组 Defines 仍需跨 target 一致。

后续 grilling 进一步确认了 asset/compiler 边界：`PassName` 与 `AssetId` 完全由 caller/资产系统
拥有，RadRay DXC extension 的 request/result、compiler metadata 和内容寻址 identity 都不包含
`PassName`。编译请求仍需携带 `SourceName`，但它只服务于 include、诊断、`__FILE__` 与 source
identity；cook/资产层用自己生成的索引把外部 asset/pass identity 映射到 compiler artifact。
因此多个资产 Pass 可以合法复用同一份编译产物，compiler 也不会把显示名误当成 shader 语义。

`ShaderCompilerBatch` 应保留 DXC 的 single-job contract。把 planner、线程调度、cache、partial failure、aggregate schema 和发布事务塞进 `dxcompiler.dll` 会让 compiler core 依赖 RadRay 的 build SKU、材质数据库和 artifact protocol，也会显著扩大每次跟进 DXC tag 的 fork 面积。推荐的 fork边界仍只是语言声明、Sema/collector 和单次 compile sidecar plumbing；multi-variant orchestration 属于 RadRay。

### Domain 不等于发布 coverage

令第 `i` 个 keyword group 的基数为 `d_i`；optional group 的 `off` 也算一个值。无约束 domain 的大小为：

```text
V = product(d_i)
```

HLSL 能声明这个合法值域和 shader语义约束，却不能单独决定某个 PC/mobile SKU 应发布哪些点。coverage set `C` 是 `D` 的子集，应由 planner 接受显式策略：`all-domain`、手工 sparse set，或“内容实际使用集 + 必备 fallback”。材质数据库只能提供 used set，build profile 决定 targets 和发布预算；二者都不是 shader source fact。RadRay 当前文档同样明确区分 `KeywordGroups` 的合法组合域与 `BakeVariants` 的离线烘焙范围，见 `docs/architecture/shader-pipeline.md:106-117` 和 `docs/guide/shader-authoring.md:85-101`。

如果用户明确选择 `all-domain`，planner 可以自动令 `C = D`，作者无需手列每个 combination。这个便利不应永久删除 sparse/used coverage：当 `V` 增长时，它是控制构建时间和包体积的必要输入。

考虑 stage projection 后，clean batch 的唯一 compile job 数更精确地写成：

```text
J = sum(target t, pass p, stage s) |project(p, s, C_p)|
```

最坏情况下各 stage 都受全部 group 影响，成本仍是 `O(V * S * T * C_DXC)`；planning 本身至多枚举 `V`，真正昂贵的是 N 次 frontend/backend compile。相同 stage projection、source、entry、target 和 options 必须在调 DXC 前去重；跨 target 不能去重，因为 DXIL 和 SPIR-V 是不同 bytecode lane。

### Forward pass 的实际数量级

当前 `forward_pass.hlsl:25-32` 有 6 个 Pixel-only optional binary groups，include 的 `view.hlsli:30-49` 再提供 2 个。`forward_pass.shader.json:6-86` 因此形成 8 个独立 `{off, on}` 维度：

```text
V = 2^8 = 256 variants
```

全部 group 都只影响 Pixel。对一个 target，256 个 domain points 投影成 256 个唯一 PS jobs，却只投影成 1 个唯一 VS job，所以是 `256 + 1 = 257` 次 compile。`shader_cook` 在 D3D12 与 Vulkan 都编入时默认请求 DXIL 和 SPIR-V，见 `tools/shader_cook/shader_cook.cpp:88-101`，因此完整 domain 的 clean cook 是：

```text
2 targets * (1 VS + 256 PS) = 514 unique DXC compile invocations
```

当前 manifest 没有烘完整 domain。`forward_pass.shader.json:88-104` 只显式点名 fully-on combination，而 `ExpandShaderBakeSet` 总会加入 default，因此 coverage 只有 2 个 domain points。cook loop 原始会访问 `2 targets * 2 variants * 2 stages = 8` 个候选；`CookStage` 在 `shader_manifest.cpp:3635-3655` 用 stage-specific defines 算 artifact key，并在调用 DXC 前去重两个重复 VS projections。clean cook 最终是：

```text
2 targets * (1 VS + 2 PS) = 6 unique DXC compile invocations
```

所以 full-domain 是当前 default + fully-on sparse coverage 的 `514 / 6 = 85.7x` clean compile 数；单 target 时同样是 `257 / 3 = 85.7x`。增量 artifact cache 能让后续未变化 cook 复用结果，但第一次构建、toolchain/schema失效或 clean build 仍承担这个数量级。自动 batch 解决操作和一致性问题，不解决 permutation explosion。

### 六类信息的真实 owner

| 信息类别 | 真实 owner | discovery/batch 能做什么 | 不能被“编译全 Variant”解决的部分 |
|---|---|---|---|
| shader 固有 contract | HLSL authoring contract | 发现 entry/stage、keyword domain、声明 resource/push constant ABI 和 contract hash | 声明若躲在未知 guard 后仍无法由一次 discovery 完整发现 |
| 逐 Variant active compile facts | 每个 single-job DXC invocation | 给出当前 defines 下的 AST/sidecar、DCE 后 reflection、bytecode 与诊断 | 一个 job 不能代表其他 permutation；必须按 projection 编译并聚合 |
| RHI/性能策略 | renderer/platform policy；若项目刻意公开给 shader 作者，可把约束写入 author contract | 校验 residency/static sampler/unbounded 等声明是否被目标 backend 支持 | root descriptor/table 选择、硬件容量和平台预算不是 DXC 可推导事实 |
| mesh/primitive layout | mesh schema 与 geometry/content pipeline | VS reflection 可提供 semantic、scalar shape/location 等 shader 需求 | `VertexFormat`、slot、offset、stride 和真实 packing 不能由 shader 独自决定 |
| build/package policy | build profile 与 `ShaderVariantPlanner` | 选择 targets、optimization、coverage、fallback 和失败策略 | “所有合法点”不等于“本 SKU 应发布的点”；这正是 `BakeVariants` 当前承担的职责 |
| content fact | material/content database | 提供实际请求的 variant set，供 planner 合并和验证 | shader source 不知道关卡、材质实例和 DLC 实际使用哪些组合 |

这张表也限定 sidecar schema：共同的 `DeclaredContract` 可以按 contract hash 存一次；`ActiveCompileFacts` 必须按 job 存；RHI policy、mesh layout、coverage 和 content provenance 应是 aggregate/planner 输入或独立记录，不能伪装成 DXC 自动发现的 shader metadata。

### 它真正解决什么，又把什么留在原处

这套架构真正解决：一次用户操作完成 domain discovery、自动 job 枚举、并行编译、一致诊断和 metadata aggregation；`KeywordGroups` 等 shader 固有声明可从手写 JSON 移到 HLSL contract；每个 bytecode 与 metadata 共享可核验的 compile identity；两 backend 可以消费同一 schema 而不混淆物理 lane。

它没有解决：指数级 compile/package 成本、stable-superset 与 exact-permutation ABI 的裁决、typed C++ binding、mesh/RHI/content/build policy 的归属，以及 RadRay 自身的 artifact/cache/JIT/AOT/runtime variant machinery。`BakeVariants` 不应简单搬进 HLSL；它应从手写 manifest 字段演化为 planner 的 coverage policy，仍可由 build profile 或内容数据库生成。原 manifest 可以缩成生成的 contract/index 产物，但不会因为 batch API 而完全消失。

**条件化结论**：对“一次操作自动编译并聚合所选 coverage”是 **Go**，实现位置是 RadRay 的四模块 orchestration，DXC fork 只提供 discovery/单 job sidecar。对“在 DXC compiler core 内新增一个 monolithic compile-all-variants 调用，并让 HLSL 独自拥有 coverage、RHI、mesh 和 content policy”是 **No-go**。

## Active binding：纯 RGB、纹理和两个 ABI 语义

仅给资源 declaration 加 attribute 不等于当前 permutation 的 active resource 集合。DXC `lib/HLSL/DxilCondenseResources.cpp:514-568` 会清理 `llvm.used` 并调用 `RemoveResourcesWithUnusedSymbols()`；DXC 自己的 `HLSLFileCheck/validation/unused_input.hlsl:1-23` 是 unused resource 在 `-Od` 也会被清理、避免 validation 失败的回归测试。

因此必须区分两种可实现语义：

### 稳定 superset ABI

- sidecar 保存作者声明的完整 binding contract，包括被某些 `#ifdef`/DCE 隐藏但 layout 必须保留的项。
- 当前 variant 的 reflection/usage 只作为 active subset 校验，不能删除 superset 项。
- variant domain/合法组合来自 HLSL 外层 pragma/attribute 或多轮 compile 的显式 union。
- 这最接近 RadRay 当前 `SharedPipelineLayout` 与 ADR-0003 的声明集合模型。

### Exact-permutation ABI

- 每次 compile 的 sidecar 保存当前预处理 AST 中的 author metadata，reflection 保存当前 codegen/DCE 后的 active resource usage。
- 纯 RGB 只产生 numeric uniform；active texture sample 需要 texture/sampler resource contract。这个结论与 UE 对照研究的 `ue5-material-resource-layout.md:279-311` 一致。
- Static Switch 的 A/B 纹理会产生不同 active 集合；如果 layout 形状不同，layout identity 必须进入 variant/cache key；同形状 variant 才能共享 layout。
- 这不允许继续声称所有 variant 共用一个不变 layout，除非另加 stable superset 层。

同一个 RadRay artifact/index schema 可以同时保存 `DeclaredContract` 与 `ActiveReflectionSummary`，
但两者必须分字段、分来源、分校验方向：前者可由 DXC frontend sidecar 直接给出，后者应由
逐 backend 的最终 reflection 聚合得到。把 declaration metadata 叫成 active resource reflection
会重新制造错误的单向握手。

## 侧写与 artifact/cache

“同次编译返回 sidecar”不会使请求 identity 消失。至少这些字段仍必须进入 sidecar header、RadRay schema 或 cache key：source identity、include closure、entry point、stage/profile、defines、optimization、SPIR-V/DXIL category、compiler/validator/toolchain version、schema version 以及 variant key。

sidecar 方案对 AOT/JIT 的影响是：

- JIT 必须从同一个 `IDxcResult` 取得 object、standard reflection 和 extra sidecar，并运行相同 decoder/ABI validation。
- AOT 必须把 sidecar 与 artifact/index 一起保存，或把 canonical sidecar落入 RadRay artifact payload；不应在 runtime 重新解析 HLSL。
- artifact/cache invalidation 仍需同时考虑 shader source、sidecar schema、compiler version、backend lane 和 variant identity。
- hot reload 仍是“source changed -> preprocess/compile/sidecar decode -> layout/cache invalidation”；sidecar减少手写同步，不消除 reload machinery。
- sidecar 不嵌入 bytecode，因而 DXIL validator 只需要验证标准 DXIL；但 sidecar 与 bytecode 的 source/variant hash 必须由 RadRay 校验。

### Discovery、身份与 fail-closed

变体 planning 需要在第一份 stage artifact 之前知道 domain、pass 和 entry point。仅靠每次
`IDxcCompiler3::Compile` 的 sidecar 不够：它一次只接收一组 defines，且 `#if` 未选中的声明
不会进入该次 AST。可行边界有两个：

1. 保留一个极小的 source-side contract reader，只读 guard 外的 domain/pass declaration；或
2. 在 fork 中增加一次 backend-neutral discovery action（例如 library/syntax-only mode），只
   输出 contract，不发布 bytecode，然后由 RadRay planner 展开 N 个独立 stage jobs。

无论选哪条，contract declaration 必须在自身 guard 之外；否则会重新出现“先知道 keyword 才能
发现 keyword”的循环。多轮编译可以求 active resource union，但它不是一次 discovery 的免费
副作用。

stock DXC 对未知 `[[radray::...]]` attribute 会走 unknown-attribute warning/ignore 路径。
所以 RadRay 不能把“编译成功但 sidecar 缺失”当成兼容模式：

- fork 应提供明确的 metadata schema/version 标记（可由预定义宏让错误的 compiler 直接失败）；
- `DxcOutput` 必须要求 sidecar 存在、magic/schema/fork identity 正确，并把解析失败当 hard error；
- artifact content hash 应覆盖 bytecode 与 sidecar，JIT/AOT 必须从同一 `IDxcResult` 或同一 blob
  恢复两者；
- 当前 `GetShaderToolchainHash()` 只把 `RADRAY_DXC_VERSION` 与 artifact format 纳入 key
  （`shader_manifest.cpp:3260-3270`），不会查询 `IDxcVersionInfo2/3` 的 commit/custom version。
  fork/schema revision 必须显式加入 toolchain hash，否则同版本号替换 compiler 会复用旧产物。

sidecar header 可以保存 compile identity 的摘要，但 include closure、content coverage 与 mesh
schema 不应伪装成 DXC 自己推导的事实；RadRay 外层 artifact key/index 仍是这些输入的 owner。

### fork 维护与实际发布链

用官方 tag 快照做的 `v1.9.2602..v1.9.2607` 文件级 diff 显示，正好是预期 fork 热点在变化：

```text
tools/clang/include/clang/Basic/Attr.td          +31 -1
tools/clang/lib/Sema/SemaHLSL.cpp                +779 -591
tools/clang/tools/dxcompiler/dxcompilerobj.cpp   +14 -1
```

这不是说每次升级都会重复同样数量，而是说明 attribute/Sema/compiler-output 三处都不是稳定
插件 ABI。patch/fork 必须带自己的 compiler tests，并在每次升级 tag 后重跑 DXIL、SPIR-V、
reflection、validator 与 sidecar schema 矩阵。

RadRay 当前 `dxc-autobuild` workflow 直接 checkout `microsoft/DirectXShaderCompiler` 的
输入 ref，并以 upstream tag 名发布 zip；当前 SDK 的 `dxc.exe --version` 是
`1.9(5402-0d3ee6b5)`，包内 `dxcompiler.dll`/`dxil.dll` 也未做 Authenticode 签名。引入 fork
后必须把 repository/ref、fork commit、metadata schema 与包 hash 一并区分，不能继续覆盖同名
upstream release asset。sidecar 本身不要求修改 `dxil.dll` validator，但实际 D3D12 运行与
retail-ready DXIL 仍需在目标设备/官方 validator 组合上单独验收。

### 作为 CMake 子工程的实测

**结论：可以。** 未修改的官方 DXC `v1.9.2607` 能通过
`add_subdirectory(<dxc-source> dxc EXCLUDE_FROM_ALL)` 进入一个已有 CMake 工程，并在同一个
Ninja build graph 中实际生成 `dxcompiler.dll` 和 `dxil.dll`。这不是开箱即用的一行集成；DXC
仓库根 `CMakeLists.txt` 假定调用方复制其官方构建脚本中的 cache contract。

Windows 探针使用 CMake `4.4.0`、Ninja `1.13.2`、MSVC `19.51.36252.0`，父工程先设 C++20，
随后以 `EXCLUDE_FROM_ALL` 加入未修改的 commit `0d3ee6b...`。必须至少对齐这些官方
`utils/hct/hctbuild.cmd:346-370` 参数：

```cmake
set(LLVM_TARGETS_TO_BUILD None CACHE STRING "" FORCE)
set(LLVM_ENABLE_EH ON CACHE BOOL "" FORCE)
set(LLVM_ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(LLVM_DEFAULT_TARGET_TRIPLE dxil-ms-dx CACHE STRING "" FORCE)

set(HLSL_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(CLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HLSL_BUILD_DXILCONV OFF CACHE BOOL "" FORCE)
set(CLANG_ENABLE_ARCMT OFF CACHE BOOL "" FORCE)
set(CLANG_ENABLE_STATIC_ANALYZER OFF CACHE BOOL "" FORCE)
set(LIBCLANG_BUILD_STATIC ON CACHE BOOL "" FORCE)
```

这里前四项不是普通裁剪项：

- DXC 根工程把 `LLVM_TARGETS_TO_BUILD` 默认设为 `all`，再展开成 `AMDGPU;NVPTX`，但同一 HLSL
  fork 的 `lib/Target/LLVMBuild.txt` 已移除这两个 component；未设 `None` 的 fresh configure
  会在 `llvm-build` 阶段失败。官方 `hctbuild.cmd:355` 同样显式设置 `None`。
- 未打开 `LLVM_ENABLE_EH`/`LLVM_ENABLE_RTTI` 时，DXC 会给多数 target 追加 `/EHs-c- /GR-`，
  而 `LLVMSupport` 又单独要求 `/EHsc`；在本机 MSVC 下产生 C4530，并因 `/WX` 失败。官方
  `hctbuild.cmd:362-364` 明确同时打开两者。

完整探针结果：fresh configure/generate 约 `23s`；显式构建 `dxcompiler` 执行 `1111` 个 Ninja
动作并在约 `165s` 完成，Debug `dxcompiler.dll` 为 `62,650,368` bytes；随后单独构建
`dxildll` 增量执行 `9` 个动作，得到 `10,716,672` bytes 的 `dxil.dll`。两份 DLL 都通过本机
动态加载检查。`dxcompiler` target 不依赖 `dxildll`，所以 RadRay source mode 必须显式依赖
二者，不能只依赖 `$<TARGET_FILE:dxcompiler>`。

隔离边界也经过生成图核对：DXC 子目录返回后，父工程 target 仍使用 C++20 和原有 `/EHsc`；
DXC 设置的 C++17、`CMAKE_RUNTIME_OUTPUT_DIRECTORY`、directory-level definitions 和 link options
没有回流到父目录。原因是 CMake 为每个子目录建立变量 binding scope。以下内容仍共享，不能称为
完全隔离：

- DXC 注册约 3700 个 Ninja targets/utility targets，所有真实 target 名处在全局命名空间；
- `LLVM_*`、`CLANG_*`、`HLSL_*`、`SPIRV_*`、`DXC_*` 前缀在探针 cache 中增加约 110 项；
- `USE_FOLDERS`、job pools、`LLVM_EXPORTS` 等 global properties 会写入同一 configure；
- 同一个 build tree 共享 generator、toolchain 和 configuration。RadRay Debug 只能直接构建
  Debug DXC，不能让这个子目录独立选择 Release；
- `EXCLUDE_FROM_ALL` 只让 DXC 不进入父目录默认 build，并忽略其父级 install/IDE inclusion；
  DXC 的 configure、cache 与全局 target 注册仍会立即发生。

用户后续选择继续采用 pinned prebuilt SDK，并关闭 RadRay 主工程中的 DXC source mode。fork
源码只由独立 autobuild/superbuild 流水线构建和发布；RadRay 不使用 `add_subdirectory`、
`FetchContent` 或 `ExternalProject` 构建 DXC。上面的探针保留为可行性与风险证据，不再是推荐
接入路线。

预编译 SDK 不能继续只是一个由 RadRay 根 `CMakeLists.txt:230-260` 手拼目录的 zip。它应自带
CMake config package 与 imported targets，描述 headers、`dxcompiler`、`dxil`、import libraries
和 runtime deployment files，并允许调用方按组件选择。当前 Windows 包实际有 9 个文件：
`dxc.exe` 约 1.09 MB、`dxcompiler.dll` 约 21.54 MB、`dxil.dll` 约 1.73 MB、4 个 headers 和 2 个
import libraries；没有 CMake package。当前 `CreateDxc()` 又要求两份 DLL 同时可加载，
`modules/shader/CMakeLists.txt:32-43` 在 JIT 开启时手工复制它们。

SDK CMake package 的目标不仅是更整洁的导入，还要让 compiler headers/client objects、CLI、
compiler/validator binaries 与纯 artifact runtime 形成可裁剪边界。用户已确认未来纯运行时是
严格 compiler-free AOT-only：配置阶段完全不发现 DXC package；构建和发布均不包含 compiler
client、reflection/SPIRV-Cross、DXC headers/CLI/import libraries/binaries 或 HLSL source；只保留
compiler artifact decoder、layout/backend consumption。artifact 缺失、损坏、不兼容或没有请求的
Variant/target 时直接失败，不允许 JIT fallback。这必须是 build graph/package component 边界，
不能退化成部署脚本少复制文件。

### 已确认的 RadRay 模块所有权

现有 `radrayshader` 同时包含 runtime 数据模型、DXC client、reflection、manifest 与 cook 支撑，已经
不符合新 contract。用户确认最终移除这个独立模块，但“合并到 `radrayrender`”只适用于 shader 的
runtime representation，并不把 compiler 与资产编排一起塞进 render：

- `radrayrender` 拥有运行时可消费的 DXIL/SPIR-V bytecode view、compiler metadata wire decoder、
  target-native binding/layout representation、vertex interface、cbuffer/struct type tree、binding
  handle 解析，以及由这些数据驱动的 backend shader/pipeline-layout 创建。
- `radrayruntime` 拥有 generated artifact index、asset/pass identity 映射、Variant assignment 选择、
  artifact 加载与缓存，以及 AOT/JIT/fail-closed policy。它不解析 HLSL，也不重建 compiler metadata。
- 可选的 `radrayshadercompiler` 只是 RadRay DXC SDK 的 C++ client adapter。输入是 source/include、
  `Defines`、`KeywordAssignments`、`CompilePolicy`、target mask 与 expected contract hash；操作只有
  `DiscoverSourceContract` 和 `CompileVariant`；输出是 diagnostics、identity/hash，以及每个 target
  的原始 bytecode/metadata/result blobs。它负责 DXC 动态加载、COM/blob/include-provider 生命周期、
  request/result 映射和 ABI 错误传播，但不拥有 RHI layout、Pass/Asset identity、Variant coverage、
  cook 调度、artifact 发布、AOT/JIT 决策或 GPU object 创建。

因此离线 shader tools 只依赖 `radrayshadercompiler` 与 RadRay DXC SDK，不依赖 render/runtime 或任一
GPU backend；开发期 JIT 的 runtime 可以可选依赖该 client，再把 compiler blobs 交给 render decoder。
严格纯运行时构建则完全不生成 `radrayshadercompiler` target，也不发现 SDK。目标依赖形状为：

```text
shader_cook (future) -> radrayshadercompiler -> RadRay DXC SDK
runtime (development JIT only) -> radrayshadercompiler
runtime -> render -> core
```

这也意味着模块名不代表新的“shader 业务层”：`radrayshadercompiler` 是可裁剪的 compiler boundary
adapter；所有不需要 compiler 才能消费的产物语义必须留在 render/runtime 一侧。

SDK 的 CMake package surface 随后确认如下：package 名为 `RadRayDXC`，target namespace 为
`RadRayDXC::`，避免带专有 extension ABI 的 fork 被误认为 stock `dxc`。config package 提供：

- `RadRayDXC::Headers`：upstream DXC headers 与 `dxcapi_radrayext.h`；
- `RadRayDXC::Compiler`：forked `dxcompiler` runtime/import library，并依赖 `Headers`；
- `RadRayDXC::Validator`：可选 external `dxil` validator，并依赖 `Headers`；
- `RadRayDXC::CLI`：可选 `dxc` executable，并依赖 `Compiler`。

不提供会隐式拉入全部 SDK 内容的 umbrella target。`radrayshadercompiler` 使用动态加载，所以只把
`Headers` 作为正常 compile dependency；构建/部署规则通过 `$<TARGET_FILE:RadRayDXC::Compiler>`
引用 compiler binary，不把其 import library 链入最终进程。CLI、Validator 和纯运行时部署都因此
可以在 build graph 中独立裁剪，而不是靠发布脚本碰巧漏拷文件。

package discovery policy 也已确认。默认 prefix 只能由 `project_manifest.json` 中匹配当前 triplet 的
固定 version/archive SHA-256 推导，CMake 使用不带 version argument 的
`find_package(RadRayDXC CONFIG REQUIRED COMPONENTS Compiler PATHS <prefix> NO_DEFAULT_PATH)`，
不得静默命中系统安装、registry、vcpkg 或 stock DXC。`tools/fetch_sdks.py` 继续负责下载、hash 校验
与解压，SDK 自带的 relocatable config package 负责解释其内部文件布局。

SDK 的 canonical identity 保留完整 upstream version，并附加 `.radray.<release>`，例如
`1.9.2607.radray.1`。RadRay 不要求 CMake 解析这个 suffix，也不围绕 `ConfigVersion.cmake` 建立
兼容层；config package 导出完整的 `RadRayDXC_SDK_VERSION`，RadRay 对 manifest-pinned prefix 做
简单字符串相等检查。upstream version、fork release、extension ABI、metadata schema、fork commit
和 content hash 仍是独立的 toolchain identity fields。

fork 开发者可以显式设置 `RADRAY_DXC_SDK_ROOT` 指向一个未发布 package；它仍须导出非空的
canonical identity，并通过 extension ABI handshake 与 toolchain identity 检查，所生成 artifact
也拥有不同 identity。CI、release 和正式 cook preset 禁止该 override，只接受 manifest-pinned SDK。
严格纯运行时配置则完全不执行 `find_package(RadRayDXC)`，而不是先导入 package 再选择不复制 DLL。

RadRay build capability 也不再沿用当前 `RADRAY_BUILD_SHADER` / `RADRAY_ENABLE_DXC`。独立
`radrayshader` 被移除后，`RADRAY_BUILD_SHADER_COMPILER` 显式控制 compiler client target 与
`find_package(RadRayDXC)`；`RADRAY_ENABLE_SHADER_JIT` 和未来的 `RADRAY_BUILD_SHADER_TOOLS` 是两个
独立 consumer capability，并分别要求 compiler client。这样 compiler contract tests、只构建离线
工具、开发期 JIT 和纯 AOT runtime 都有可表达的 build graph，而不把“有 DXC”误当成 shader
runtime 的固有属性。

当前 `shader_gen` 的唯一职责是从 reflection 生成作者再编辑的 `.shader.json` 起始模板，和已确认的
HLSL-only/no-handwritten-JSON contract 直接冲突，因此删除而不迁移。cook 当前不急于实施，
`RADRAY_BUILD_SHADER_TOOLS` 可以先默认关闭；严格纯运行时 preset 同时关闭 compiler、JIT 与 tools，
从 configure 开始就不接触 SDK。

runtime loader policy 选择服从平台搜索顺序。`radrayshadercompiler` 不接收 explicit/absolute compiler
path，也不把 SDK build-machine path 编译进 binary；它按平台 canonical name 加载 `dxcompiler`。
这意味着环境可能先命中 system、`PATH`、working-directory 或 application-local 的另一份 binary，
该风险由用户明确接受。正确性边界放在加载后的 `CLSID_RadRayDxcCompiler`、extension ABI 与
toolchain identity 检查：stock DXC、旧 fork 或 identity 不兼容都必须 fail closed，不能回退到
`IDxcCompiler3` 继续生成缺少 RadRay metadata 的产物。

compiler runtime deployment 采用 CMake target/component 规则，而不是 per-executable opt-in。只要
`RADRAY_BUILD_SHADER_COMPILER` 开启，build graph 就把 `$<TARGET_FILE:RadRayDXC::Compiler>` 自动复制
到 RadRay 统一的 `${RADRAY_BUILD_PATH}/$<CONFIG>` runtime output；因此按裸名称加载时 application、
tools 与 tests 共享同一个 application-local compiler。install tree 使用 CMake 3.21 已提供的
`install(IMPORTED_RUNTIME_ARTIFACTS RadRayDXC::Compiler ...)`，归入独立 `ShaderCompiler` component，
不手写 DLL/.so/.dylib 文件路径清单。`Validator` 与 `CLI` 依旧属于各自可选 install component；
compiler capability 关闭时不生成任何 compiler copy/install rule，纯运行时不会被部署阶段重新污染。

### 最小原型门槛

在选择 sidecar 还是 C++ trace 前，至少应完成一个不改 RadRay RHI 的 compiler-only 原型：

1. `[[radray::...]]` 的 TableGen/Sema 诊断、重复/缺失字段和错误类型都 fail；stock DXC 或旧
   fork 不产生可被 RadRay 接受的结果。
2. 同一份 source/defines/entry 在 DXIL 与 SPIR-V action 上都返回相同 schema 的
   `DeclaredContract`；sidecar 与 object/reflection 的 identity 可校验配对。
3. forward 的 RGB、texture、shadow 三个 active set 经过 DXIL reflection 与 SPIRV-Cross
   反射后，能明确验证 stable superset 或 exact-permutation 其中一种语义；error pass 不会
   因 include 中未使用的 shadow declaration 获得错误 binding。
4. `IDxcVersionInfo`/toolchain hash、schema bump、sidecar 缺失和部分失败都有测试；完成前不应
   把“删除 manifest”写成既定 ADR 或实施 TODO。

## Validator、hash 与部署边界

这个问题必须区分改 `dxcompiler.dll` frontend 和改/自建 `dxil.dll` validator：

- `include/dxc/dxcapi.h:1058-1062` 定义 `DxcVersionInfoFlags_Internal = 2`，注释明确是 `Internal Validator (non-signing)`。
- 当前 `tools/clang/tools/dxcompiler/dxcvalidator.cpp:141-149` 的 locally-linked validator 设置该 flag；`dxcompiler/dxcutil.cpp:133-176` 直接调用它完成 compile 后验证。
- DXC `1.9.2607` ReleaseNotes 的 `1.8.2505` 条目说明正常 compiler 改为始终使用 internal validator，不再自动搜索外部 `DXIL.dll`。
- `tools/clang/tools/dxcompiler/dxcapi.extval.cpp:169-173,395-429,442-464` 仍保留显式 external-validation wrapper：`DXC_DXIL_DLL_PATH`/loader 可以把 compiler 包装为使用外部 validator，但这不是 RadRay 当前 `IDxcCompiler3` 直连路径自动启用的行为。
- UE vendored 1.8.2403 的 `tools/clang/tools/dxcompiler/dxcutil.cpp:52-68,160-187` 明确在外部 `dxil.dll` 缺失时警告“Resulting DXIL will not be signed for use in release environments”，并区分 external 与 internal validator。
- 当前 checkout 的 `tools/clang/tools/dxildll/dxcvalidator.cpp:120-128` 的独立 `dxil` target 没有设置 `DxcVersionInfoFlags_Internal`；`dxildll/CMakeLists.txt:6-20,55-59` 链接 `LLVMDxilHash`、`LLVMDxilValidation`，而 `tools/clang/tools/dxcvalidator/dxcvalidator.cpp:36-50` 实现 retail hash 更新。

所以源码支持的精确结论是：**source-built `dxcompiler.dll` 的内嵌 validator 明确是 internal/non-signing；不能把这句话无条件扩大为所有单独构建的 `dxil.dll` target。** 官方 README 又把发布包中的 `dxil.dll` 称为 signing binary，并说明 Windows SDK 提供 supported compiler/validator。没有运行驱动实验时，不应把 locally built `dxil.dll` 宣称为 Microsoft-supported retail signing path。

对 RadRay 的实际风险：`CMakeLists.txt:230-260` 和 `modules/shader/src/dxc.cpp:815-867` 会加载 `dxcompiler` 与 `dxil` DLL，但 RadRay 直接调用 `DxcCreateInstance(CLSID_DxcCompiler)`，没有使用 `DxcDllExtValidationLoader` external wrapper。sidecar 本身原则上不需要修改 validator；fork 的 compiler 仍必须和目标 official validator/DXIL version 兼容，并在发布/cook 处明确由哪个 validator 负责 retail-ready DXIL。不要为了 sidecar 去 fork `dxil.dll` validator。

用户随后提出正式 D3D12 拒绝未签名 shader，因此认为 `dxil.dll` 必须是硬依赖。D3D12 可加载性
确实必须是发布门槛，但针对 pinned `v1.9.2607` 的后续源码与运行探针否定了“没有外部
`dxil.dll` 就一定得到未签名 bytes”这个前提，故 validator 部署决策重新打开，不能先写成 contract。

探针把 SDK 的 `dxc.exe` 与 `dxcompiler.dll` 复制到一个确认没有 `dxil.dll` 的隔离目录，使用
`VSMain`/`vs_6_0` 编译 `shaderlib/forward_pipeline/error_pass.hlsl`：默认内部 validator 编译成功，
得到 5,028 bytes 的 DXIL container，header hash 为非零
`AF B6 57 AD 97 B3 C0 2E AC 0C FB 76 31 66 6A A9`。随后把
`DXC_DXIL_DLL_PATH` 显式指向 SDK 的 `dxil.dll`，外部 validation wrapper 也成功，输出与默认产物
逐字节相同，SHA-256 都是
`002C73D6D5CD8D9A552739A1023B656D59304D85D42F0EB8972DDE8BE98A04A1`。把变量改为不存在的
absolute path 时 `dxc.exe` 以 `-2147024809` 失败并报告 path not found，证明前一轮确实启用了
external-validator loader，而不是环境变量设置失效。

这与源码一致但暴露了术语歧义：ReleaseNotes 仍说 `1.8.2505` 起 compiler 总是使用 internal
validator，`dxcapi.h` 也把 internal 标成 non-signing；但当前 internal 与 external 路径共享的
`tools/clang/tools/dxcvalidator/dxcvalidator.cpp:36-85` 都会计算并写入 retail container hash。
至少对该官方 SDK 和这个 shader，internal 输出不是 zero-hash/preview-bypass container，外部
`dxil.dll` 没有进一步改变 bytes。

还完成了 GPU 侧交叉检查：当前 RadRay `CreateDxc()` 虽加载 `dxil.dll`，却直接从
`dxcompiler.dll` 创建 `IDxcCompiler3`，没有创建 `IDxcValidator` 或使用
`DxcDllExtValidationLoader`，所以其 JIT compile 仍是 internal-validator 路径。
`Backends/VerticalSliceTest.ManifestToPixels/D3D12_Jit` 在 NVIDIA GeForce RTX 3060、driver
`32.0.15.9144` 上通过，覆盖 D3D12 PSO 创建、执行与像素回读。这证明 pinned SDK 的默认 DXIL
在当前正式 D3D12 路径可加载，不能以“进程中恰好 LoadLibrary 了 dxil”解释为 external signing。

因此正确的发布 contract 应先定义为“DXIL artifact 必须通过真实 D3D12 load/PSO smoke test”，而
不是直接等同于“必须部署并调用 external `dxil.dll`”。fork SDK CI 还应比较 internal/external
产物、container hash 与 validator identity；只有 fork 的 internal output 无法满足 load gate 时，
才把 external validator 纳入强制 compile 闭包。用户根据该实测确认最终 contract 不再无条件依赖
external `dxil.dll`。RadRay DXC SDK 的默认 compiler output 必须在 SDK CI 中完成 D3D12 pipeline-state
创建与执行 smoke test；只检查 DLL 是否存在或 container hash 非零都不够。`dxcompiler` 属于
compiler deployment closure，external `dxil` validator 可以作为独立可选 component；若未来某个
pinned fork 的默认输出无法通过 load gate，则该 SDK build 本身不合格，或必须明确切换为经过实测的
external-validation pipeline，不能由 RadRay runtime 猜测和补救。

## 公平候选比较

| 维度 | A. stock DXC + 现状/外部 parser | B. small DXC frontend fork + sidecar | C. C++ trace |
|---|---|---|---|
| 单一 ABI 真相 | 否；manifest 与 HLSL 仍握手。外部 parser 只能自定义源协议 | 可以；HLSL attribute/pragma 是作者声明，sidecar 是同次 compile 的 canonical 输出 | 可以；trace 直接构造 binding/layout |
| 双后端一致输出 | 需继续分别 reflection/manifest lane | 可以让 frontend 输出同一 backend-neutral sidecar，DXIL/SPIR-V bytecode 仍分 lane | 同一 trace descriptor 分别生成两 lane |
| 删除人工 manifest 握手 | No-go；除非引入另一套不稳定 parser | Go 条件：全部 ABI fields 都有 HLSL contract，variant semantics 也有明确 owner | Go 条件：trace 覆盖现有 manifest/variant/artifact contract |
| typed C++ binding/结构变体 | No | No；HLSL attribute 可有参数检查，但不是 C++ 类型系统 | Yes，目标本身是 C++ type/trace system |
| keyword/string variant | 保留 | 保留，sidecar不自动理解所有合法组合或 bake set | 可重构为 C++ 函数/参数，但仍需定义 variant/cache 语义 |
| pure RGB/texture active set | 当前 reflection/manifest 语义 | sidecar必须区分 declaration/superset 与 current reflection/active | trace 可先构造 superset，再由 backend codegen产生 active code |
| 迁移成本 | 最低；继续现有 schema、resolver、cache、cook | 中高；fork、TableGen/Sema/AST、sidecar result decoder、schema、AOT/JIT/cache、validator部署 | 高；新 frontend/codegen、typed attributes、trace API、HLSL生成和迁移测试 |
| 作者体验 | HLSL + 手写 JSON/外部规则 | HLSL + typed `[[radray::...]]`/pragma；不再手写 JSON，但仍要懂 variant contract | C++ shader DSL；HLSL成为中间产物，需学习新的 type/trace model |
| hot reload | 现有 HLSL/manifest流程 | HLSL + sidecar一起失效，少同步错误但 fork ABI需稳定 | C++ module/trace变更，reload与运行期编译更复杂，取决于前端设计 |
| 编译/分发 | 官方 DXC；当前 RadRay已有 DXIL/SPIR-V lane | 分发 forked dxcompiler、sidecar decoder、匹配 official validator；DXIL release signing仍需单独处理 | 可继续使用官方 DXC 后端；第一期仍有 ADR-0014 的运行期 DXC依赖 |
| fork维护 | 无 compiler fork；外部 parser需自行跟进 HLSL | 持续跟进 DXC tag、Attr.td生成、Sema、AST/COM output和跨后端变化 | 不维护 DXC fork，但维护 RadRay 自有 frontend/codegen |
| 跨平台 | 现有 backend lanes，reflection形状不同 | sidecar可统一，但 DXIL validator/deployment 仍是 Windows/DXIL问题 | descriptor model可统一，codegen/bytecode仍按 lane 分开 |
| 适合目标 | 只求低风险延续 | 只求 HLSL ABI single-source + sidecar | 还要 typed C++、结构化变体和作者模型重构 |

### 目标条件化回答

若初始目标严格限定为“ABI 元数据单一真相 + 双后端一致输出 + 删除人工 manifest 握手”，B 可以达到，但必须同时满足：

- 所有七类 ABI 字段都有 HLSL attribute/pragma 或外层 contract 的明确 owner。
- sidecar header 包含 schema/fork identity；RadRay 外层 artifact key 绑定 source/include、entry、stage、defines、target、options 与 variant。
- stable superset 或 exact-permutation 语义先选定，不能由 reflection 结果隐式决定。
- 对 `#ifdef` variant domain 采用 guard 外声明、多轮编译 union 或明确的外层 metadata；不能把一次 compile 当成全量 variant parse。
- `DxcOutput`、AOT/JIT、artifact/cache 和 runtime binding 完成 sidecar decoder/plumbing。
- DXIL/SPIR-V sidecar 使用同一 canonical schema，但 bytecode/reflection 仍承认两条 backend lane 的物理差异。

若目标还包括 typed C++ binding、摆脱 keyword/string variant、表达结构体/类型变体、删除大量 HLSL/manifest工具链复杂度，B 只能解决“手写 manifest握手”一部分，不能达成完整目标；C 的价值才成立。C 的代价是把 frontend、type system、hot reload、compile/cache 和第一阶段 DXC部署风险全部转移到 RadRay，而不是通过一个 sidecar自动消除。

## 三个候选的 go/no-go

### A. stock DXC + 现状/外部 parser

**Go**：当前优先级是稳定交付、接受 manifest 与 HLSL 双份声明，或只需要一个诊断工具解析现有 pragma/预处理文本。

**No-go**：要求 stock DXC COM 在不 fork 的情况下把任意 HLSL attribute 变成结构化 AST/sidecar；当前没有该稳定接口。外部 parser 也不能自动拥有 DXC Sema、DCE、resource usage 和 variant union 的语义。

### B. small DXC frontend fork + `DXC_OUT_EXTRA_OUTPUTS` sidecar

**Go**：目标限于 HLSL 作者声明的单一 ABI、双 backend canonical `DeclaredContract`、删除人工
manifest；愿意维护 DXC fork，并接受 active facts 仍由每条 backend lane 的最终 reflection
聚合，分发匹配的 decoder/validator 策略。

**No-go**：不愿维护 compiler fork；或要求 metadata 自己推导完整 variant domain、当前 active resource、immutable sampler、mesh packing 和 pipeline policy；或要求 sidecar 自动成为 bytecode内嵌且跨 DXIL/SPIR-V同构。

### C. C++ trace

**Go**：目标明确包含 typed C++ shader API、结构变体、C++ binding/layout同点构造、重做 keyword/variant 作者模型；团队接受建设 frontend、HLSL生成、热重载与运行期/AOT编译链。

**No-go**：当前只想消除 manifest手写同步、仍希望保留 HLSL 作者体验，且不准备承担新的 DSL、C++编译/部署与迁移成本。这个场景应先评估 B，而不是把更大迁移强加给窄目标。

## 未决设计与首个 grilling 问题

最关键的未决设计不是“attribute 还是 pragma”，而是 ABI 的变体语义：

**RadRay 是否必须保持一个跨所有 keyword/target 共享的 stable superset `PipelineLayout`，还是允许 exact-permutation binding 集合并把 layout identity 纳入 variant、artifact 和 cache key？**

建议 grilling 首先只问用户这一题。它会决定 sidecar 是保存单一声明 superset，还是必须为每次 compile 分离 declaration/active reflection，并会直接改变 C++ trace 的 trace/layout API 形状。

## Sources

- [DirectXShaderCompiler `dxcapi.h`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/include/dxc/dxcapi.h)
- [DirectXShaderCompiler `dxcapi.h`, tag `v1.9.2602`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2602/include/dxc/dxcapi.h)
- [DirectXShaderCompiler `dxcompilerobj.cpp`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/tools/clang/tools/dxcompiler/dxcompilerobj.cpp)
- [DirectXShaderCompiler `FrontendAction.cpp`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/tools/clang/lib/Frontend/FrontendAction.cpp)
- [DirectXShaderCompiler `Attr.td`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/tools/clang/include/clang/Basic/Attr.td)
- [DirectXShaderCompiler `ParsePragma.cpp`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/tools/clang/lib/Parse/ParsePragma.cpp)
- [DirectXShaderCompiler `DxilContainerValidation.cpp`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/lib/DxilValidation/DxilContainerValidation.cpp)
- [DirectXShaderCompiler `dxcvalidator.cpp`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/tools/clang/tools/dxcompiler/dxcvalidator.cpp)
- [DirectXShaderCompiler README, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/README.md)
- [DirectXShaderCompiler ReleaseNotes, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/docs/ReleaseNotes.md)
- [DirectXShaderCompiler root `CMakeLists.txt`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/CMakeLists.txt)
- [DirectXShaderCompiler `hctbuild.cmd`, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/utils/hct/hctbuild.cmd)
- [DirectXShaderCompiler `dxcompiler` CMake target, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/tools/clang/tools/dxcompiler/CMakeLists.txt)
- [DirectXShaderCompiler `dxildll` CMake target, tag `v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/tools/clang/tools/dxildll/CMakeLists.txt)
- [DirectXShaderCompiler `v1.9.2602` commit](https://github.com/microsoft/DirectXShaderCompiler/commit/21d28f727ad395b59394815ef76012e432f7e4e5)
- [RadRay DXC autobuild workflow](https://github.com/ksgfk/dxc-autobuild)
- [CMake `add_subdirectory`](https://cmake.org/cmake/help/latest/command/add_subdirectory.html)
- [CMake `EXCLUDE_FROM_ALL` directory property](https://cmake.org/cmake/help/latest/prop_dir/EXCLUDE_FROM_ALL.html)
- [CMake directory variable scope](https://cmake.org/cmake/help/latest/manual/cmake-language.7.html#variables)
- [SPIR-V Specification 1.6, `OpModuleProcessed`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpModuleProcessed)
- [SPIR-V Specification 1.6, Debug Information](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_debug_information)
