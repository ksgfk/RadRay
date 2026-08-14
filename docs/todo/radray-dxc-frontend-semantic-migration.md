> - 适用: 把 RadRay DXC extension 的 source contract 与 metadata 从手写扫描迁移到 Clang/DXC compiler pipeline
> - 权威: 本文是 ADR-0034 的实施与验收计划；shader 总体契约以 ADR-0016、include 边界以 ADR-0022 至 ADR-0031 和 ADR-0033 为准
> - 状态: 核心实现已落地，尚未完成全部验收（2026-08-14；Windows consumer、JIT-disabled 与 compiler-free gate 已通过；待 `.radray.4` 发布/manifest hash、fork 原生 compiler/SPIR-V/TAEF、非 Windows和 docs gate）
> - 锚点: `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/src/shader_compiler_contract.cpp`, `modules/shader_compiler`, `modules/render/tests`, `docs/architecture/shader-pipeline.md`, `tools/fetch_sdks.py`

# RadRay DXC frontend 语义迁移计划

## 目标

让 `DiscoverSourceContract` 和 `CompileVariant` 的所有 HLSL 语义都来自 Clang/DXC 已有的
preprocessor、AST/Sema 与 target codegen model。最终输入输出仍是 RadRay-owned、versioned、
fixed-width wire；DXC 内部如何组织 AST、LLVM IR 和 SPIR-V module 不暴露给 RadRay。

完成后要同时消除两处重复权威：fork `dxcradray.cpp` 中的 contract/metadata source scanners，以及
RadRay `SourceContractDiscoverer` 的本地 scanner/非 Windows fallback。filesystem include 行为保持
不变：root source 在内存，调用方提供 ordered include directory array，DXC 每次 invocation 直接从
磁盘读取 include；不生成 include closure，不做 compiler-side cache invalidation。

## 实施结果

迁移前 fork 的真实路径是：

```text
root bytes --+--> -P include validation --> root-only contract scanner
             |
             +--> IDxcCompiler3 target compile --> bytecode
             |
             +--> root-only metadata scanner --> metadata
```

迁移前的 `CleanSource`、`ParseStageAttributes`、`ParseKeywordPragma`、`DiscoverContract`、
`BuildMetadataFacts` 及其 resource/type/root-signature helpers 都在 compiler frontend 之外解释
HLSL；RadRay client 在非 Windows 分支又通过 `SourceContractDiscoverer` 实现第三份解释。它们不可能
与 Clang tokenization、Sema declaration、optimization 后的 active resources 和 target layout 长期
保持一致。

当前 fork 的真实路径是：

```text
typed request + ordered include paths
              |
              v
      shared DXC compiler invocation
        |          |             |
        |          |             +--> final DXIL/SPIR-V model --> LaneMetadata
        |          +--> AST/Sema -------------------------------> entry/type identity
        +--> preprocessor/PragmaHandler ------------------------> keyword domain
              |
      +--> syntax-only: ShaderContract
      +--> target action: bytecode + metadata (atomic result)
```

`dxcradray.cpp` 中的 source/metadata scanner、独立 `-P` include validation、RadRay
`SourceContractDiscoverer` 和非 Windows scanner fallback 已删除。frontend collector 通过
`WrapperFrontendAction`/`MultiplexConsumer` 接入 DXC；DXIL/SPIR-V metadata 来自各自最终模型，
SPIR-V selected entry 的 active resource 集合由 Sema AST 的 declaration/call graph 选择，binding/type
事实仍从 legalization 后的最终 module 读取；push constant 作为接口事实保留。SPIR-V immutable
sampler 在需要时使用不发布 DXIL lane 的辅助分析 invocation，并用作者显式 `register()` 与合并后的
DXIL static-sampler register policy 关联。当前实现仍可保留在
一个 fork-private translation unit 中，observer seam 与 owning facts 已按职责隔离；物理拆分不属于
外部契约或验收条件。

## 最终外部契约

### 1. 保留两阶段 API

- 保留 `IRadRayDxcCompiler::DiscoverSourceContract` 与 `CompileVariant`，以及 result 中 status、ABI info、
  contract blob、target lanes 和 diagnostics 的职责。
- discovery 改为编码一个 typed `DiscoverSourceContractRequest`，字段为 `SourceName`、`RootSource`、
  ordinary `Defines`、non-empty `Targets` 和完整 `CompilePolicy`。include path array 继续是方法的独立
  borrowed 参数。
- discovery 不接收 `KeywordAssignments` 或 `ExpectedContractHash`。它发现合法 domain，不选择
  concrete Variant。
- `CompileVariantRequest` 的业务字段保持不变；compile 必须用同一 collector 重新发现具体 assignment
  下的 contract，而不是信任 caller 或复用 discovery 的 AST。

`Targets` 允许调用方明确当前需要验证的 frontend modes；请求 `All` 时必须比较 DXIL/SPIR-V
contract facts 后只返回一份 contract。cook、跨后端 shader suite 和发布前 discovery 使用 `All`；
单 backend 开发调用可以只请求对应 target，但不能据此宣称已证明 cross-target invariant。

### 2. ABI/schema 断代

- `DiscoverSourceContractRequest` 增加 fields，因此 discovery wire schema 必须提升。
- 提升 extension ABI version 并更换 `IRadRayDxcCompiler` IID；CLSID 可保留为同一 extension 的稳定
  discoverability identity。旧 client 查询新 compiler、或新 client 查询旧 compiler时都 fail closed。
- `IRadRayDxcResult` vtable、compile request wire、contract wire 和 metadata wire 若 binary shape 未变
  则不升 schema。内部事实来源变化由 extension ABI/toolchain identity 表达。
- 提升 toolchain identity、SDK package version/manifest hash，重新生成所有依赖 compiler output 的
  goldens。不得提供旧 discovery encoder、旧 IID 或 scanner fallback adapter；当前 package 为
  `1.9.2607.radray.4`，ABI 为 3，discovery wire 为 3，metadata wire 为 5。该 fork package
  发布并取得 zip hash 前，`project_manifest.json` 继续固定到最后一个可下载的 `.radray.3`；不得
  预先改 manifest version 而留下 404 或 hash mismatch。

### 3. Identity

`ContractHash` 只编码 canonical keyword groups、entry names/stages 和 shader kind。`Defines`、policy、
source/include bytes、source name、include directories 和 compiler-private declaration key 都不直接
进入 hash；若这些输入改变了 canonical contract facts，输出 hash 自然改变。

`BytecodeHash`、`PipelineLayoutHash` 和 `GpuArtifactHash` 继续从对应 target 的 compiler output 计算。
本迁移不恢复 `CompileInputHash`，也不新增 include content hash、dependency graph 或 cache key。

## Fork 内部架构

### 1. Shared invocation

把 DXC 当前 compile driver 中的 compiler setup、argument translation、source buffer、include handler、
diagnostic engine 和 action execution 抽成 fork-private shared invocation。标准 `IDxcCompiler3` 继续走
无 observer 的相同路径；RadRay extension 传入 invocation-local observer：

- discovery：`SyntaxOnlyAction` + RadRay frontend collector；
- DXIL stage：`EmitBCAction` + 同一 frontend collector + DXIL final-module observer；
- SPIR-V stage：`EmitSpirvAction` + 同一 frontend collector + SPIR-V final-module observer。

RadRay collector 通过 `WrapperFrontendAction`/`MultiplexConsumer` 与原 action 组合，不替换原 AST
consumer。DXIL/SPIR-V 只增加最小的 fork-private completion hook，把 final model 的 value facts 复制给
当前 invocation；不改变 upstream COM interface、标准 `IDxcResult` 或 CLI behavior。

把现有大文件按职责拆成三个单元：`dxcradray.cpp` 只保留 COM/wire/orchestration/result publication；
`dxcradrayfrontend.{h,cpp}` 承载 shared invocation、frontend action、pragma/AST collector 与 target
completion hooks；`dxcradraymetadata.{h,cpp}` 承载 canonical merge、validation 和 wire encoder。
三者都留在 fork 的 `dxcompiler` implementation 内，不安装为 SDK header。

### 2. Invocation-local facts

内部只在一次同步 operation 中流转三层 value data：

| 事实 | 唯一来源 | 去向 |
|---|---|---|
| keyword groups、声明位置与条件深度 | Clang preprocessor `PragmaHandler`、`PPCallbacks`、`SourceManager` | `SourceContractFacts` |
| entry name/stage/kind | Sema 完成后的 `FunctionDecl`、`HLSLShaderAttr` | `SourceContractFacts` |
| source declaration association | canonical declaration/USR-like key | stage merge、static sampler bridge；不序列化 |
| DXIL active binding、RootSignature、type/layout | final DXIL module、resource/type/root-signature model | DXIL `StageSemanticFacts` |
| SPIR-V active binding、push constant、location、type/layout | legalization 后 final `SpirvModule` | SPIR-V `StageSemanticFacts` |
| stage visibility、graphics union、target hashes | canonical fork merge/encoder | target `LaneMetadata` |

表中的类型名是实施时的工作名。它们必须是 owning value types；AST/LLVM/SPIR-V object pointer 只能在
observer callback 内短暂使用。跨 stage invocation 的 declaration association 使用 compiler canonical
symbol identity，不能使用 pointer 或物理 include path；冲突或无法唯一关联时 hard error。

### 3. Contract collection

- 注册 `radray_keyword_group` pragma handler；按 compiler token 规则解析 name/value，拒绝 malformed、
  duplicate、include-origin 和 conditional pragma。root-only 按 main-file spelling location 判定，不按
  文件名字符串猜测。
- AST consumer 在 translation unit 完成后枚举带 `HLSLShaderAttr` 的有效 function declaration，使用
  Sema 归并后的 declaration/definition，处理 redeclaration、attribute placement 和 overload diagnostics。
  entry 可以定义在 include 中。
- canonicalize keyword groups 与 entries，执行 graphics/compute topology 规则。discovery 的 requested
  target contracts 不一致时 `InvalidRequest`。
- concrete compile 用当前 assignment macros 再采集 topology，并与 expected contract 比较。只验证
  实际请求编译的 assignments；本阶段不穷举整个 keyword domain。

### 4. Target metadata collection

- 每个 entry 单独执行 target action；只采集该 bytecode 中最终 active 的 resources/interface，不从 AST
  中把“声明过但被优化掉”的资源算为 active。
- DXIL observer 在 module/root-signature processing 完成后提取 register/space、resource class、array
  count、root constants/static samplers、cbuffer type tree、vertex input 和 target-native offsets/strides。
- SPIR-V observer 在 binding/location assignment 与 legalization 完成后提取 descriptor set/binding、
  storage class、push constant、decorations、type tree、vertex locations 和 target-native offsets/strides。
- graphics 的 VS/PS facts 在 lane 内合并，按 declaration key 合并同一 binding 并累计 visibility；类型、
  count、binding 或 immutable-sampler association 冲突即失败。compute 不走 graphics merge。
- SPIR-V immutable sampler 按作者显式 DXIL register 关联合并后的 RootSignature static sampler policy；
  只请求 SPIR-V 时允许执行不发布 DXIL bytecode 的辅助 analysis invocation。
- metadata encoder 只接受已经完成校验的 `LaneMetadata`。先完成所有 requested lanes，再一次性填充
  result；中途不得把成功 lane 暴露给 caller。

### 5. Include 与并发

删除 discovery 的独立 `-P` validation。syntax-only/target action 本身使用 raw root `DxcBuffer`、逻辑
`SourceName`、caller ordered `-I` 和每次 invocation 新建的 default filesystem include handler，缺失
或错误 include 直接产生正常 compiler diagnostics。

extension 对 caller path array 只做 ABI view、UTF-8/NUL/长度与计数安全校验，不内置 shaderlib、CWD
或平台路径。相对/绝对路径、quote/angle search 和 shadowing 由 DXC 处理。不同 operation 之间无 AST、
include bytes 或 semantic facts cache，因此并发只共享 DXC 原本线程安全的只读设施。

## Diagnostics 与状态映射

- wire header、reserved field、target mask、policy、source name、include path view 或 UTF-8 非法：
  `InvalidRequest`，使用稳定 RadRay wire diagnostic code。
- preprocessor、pragma、parse、Sema、contract topology 或 cross-target invariant 错误：discovery 和
  compile 的 contract phase 均为 `InvalidRequest`，保留 compiler source location/message。
- assignment 不属于 domain：`InvalidRequest`；`ExpectedContractHash` 或 concrete topology 与 discovery
  contract 不同：`ContractMismatch`。
- DXIL/SPIR-V codegen、validation、final-fact extraction、stage merge 或 metadata encoding 失败：
  `TargetFailure`。
- 任意失败均返回可读取 diagnostics，但 contract/lane publication 遵守现有 result contract；不得以
  warning 降级 semantic conflict，也不得回退 scanner。

## 测试迁移

### DXC fork

在现有 Clang/LLVM/DXC harness 内覆盖 compiler extension：

- 在现有 `ClangHLSLTests` 的 `CompilerTest` 中增加 RadRay extension unit test，直接通过
  `DxcCreateInstance` 获取 interface，验证 ABI info、typed discovery、ordinary define、contract
  blob 和 result lifetime；更大的 metadata/atomicity 矩阵由 RadRay consumer suite 覆盖。
- 能由标准 compiler driver 表达的 parser/Sema/DXIL/SPIR-V 行为进入现有 lit + FileCheck/`-verify`
  suites；extension result 内容和 private observer 由 `ClangHLSLTests` 断言，不新增专用 CLI/probe。
- Windows 的真实 DXIL load/PSO/draw gate 并入现有 HLSL TAEF/D3D12 harness，并使用 extension-produced
  DXIL；非 Windows 运行相同 semantic/API unit tests 与 SPIR-V tests，不构建 D3D12 case。
- 删除 `radray_abi_probe.cpp`、`radray_dxc_d3d12_smoke.cpp`、
  `radray_wrong_abi_fixture.cpp`、对应 CMake targets/dependencies 和 `HLSL_IGNORE_SOURCES` entries。

最少测试矩阵：

- comments、strings、line continuation、macro-expanded tokens、nested includes、ordered shadowing、缺失
  include、active/inactive conditional；
- root/include/conditional/duplicate/malformed keyword pragma；
- entry in root/include、redecl、overload、attribute placement、conditional concrete topology、graphics/
  compute cardinality；
- active/inactive resources、same declaration across stages、arrays、nested structs、matrices、16-bit types、
  cbuffer/root constants/static sampler/push constant；
- DXIL register/space 与 SPIR-V set/binding/location 分歧、vertex input、multi-stage visibility merge；
- one-lane/two-lane success、one lane failure 后无 partial output、expected contract mismatch、diagnostic
  source locations、cross-platform ABI layout。

### RadRay

- 删除 `SourceContractDiscoverer`、其 scanner implementation 和 scanner-specific tests；
  `contract_discovery` 只保留 public result/contract blob decode，或按职责重命名。
- 删除 `client.cpp` 的 `_WIN32` source scanner fallback。client 通过 DXC cross-platform ABI 和
  `DynamicLibrary` 在所有支持平台调用同一 extension；不可用时报告 unavailable，不自行发现 contract。
- compiler ABI/package gate 已折叠进 `test_radray_shader_compiler_client`，不再由独立
  `test_radray_dxc_abi_probe` target 重复 fork tests；错误 ABI 继续由 client handshake fail closed，
  不部署错误 DLL fixture。
- 保留 `RadRayDxcMetadata`、shaderlib pass、runtime JIT 和 render backend tests，验证 client decode、
  public artifact contract 和真实消费；不在 RadRay 重测 pragma/AST/resource collector 的内部组合。

## 实施阶段与检查站

### M0：冻结 I/O 与 golden expectations（已完成）

1. 在 fork unit tests 中先编码 typed discovery request、status mapping、canonical contract 和 representative
   DXIL/SPIR-V metadata 的期望输出。
2. 在 RadRay 定义 `DiscoverSourceContractRequest` 和新 discovery wire schema；同步 fork header，但暂不
   发布 SDK。
3. 固定 schema bump 矩阵：discovery + ABI/IID/toolchain 必升；compile/contract/metadata shape 不变则不升。

检查站：新旧 request 均有 fail-closed test；golden 只描述 public output，不复制旧 scanner 实现。

### M1：建立 shared invocation 与 frontend contract collector（已完成）

1. 重构 DXC internal compile driver，增加 invocation-local observer seam。
2. 实现 pragma handler、PP callbacks、AST consumer、canonical contract/diagnostics。
3. discovery 切到 syntax-only action，验证 filesystem include；删除该路径的 `-P` 与 contract scanner。

检查站：contract matrix 在 Windows/非 Windows unit tests 通过；discovery 不创建 bytecode，编译器诊断
含正确 root/include source location。

### M2：迁移 DXIL metadata（已完成）

1. 在实际 `EmitBCAction` invocation 接入 final DXIL observer。
2. 覆盖 active resource、RootSignature/static sampler、type/layout、root constants 和 vertex input。
3. 完成 stage merge 与 DXIL metadata encoder parity。

检查站：representative DXIL metadata 与 public golden 相符；从 source scanner 删除的每类 fact 都有
final-module test，真实 D3D12 harness 使用 extension bytecode 成功执行。

### M3：迁移 SPIR-V metadata 与 cross-target merge（已完成）

1. 在实际 `EmitSpirvAction` invocation 的 legalization/finalization 后接入 observer。
2. 覆盖 active descriptor、push constant、type/layout、location 与 immutable sampler association。
3. 完成双 target contract compare、lane atomicity 和 target-specific metadata tests。

检查站：SPIR-V validation 与 Vulkan consumer tests 通过；DXIL/SPIR-V 可有不同 layout，但 public contract
一致；任一 lane 故障时 result 无 lane。

### M4：原子 cutover 与删除旧路径（已完成；TAEF gate 待环境）

1. extension 正式切到 compiler facts，提升 ABI/IID/discovery schema/toolchain identity。
2. 删除 fork 所有 source/metadata scanner helpers、`-P` validation 和三个 standalone targets/files。
3. 把 fork extension tests 全部落到 Clang/LLVM/DXC harness；移除临时 comparison code/feature flag。

检查站：active fork source 中不存在 scanner symbol；标准 `IDxcCompiler3` behavior 未变；
`check-clang`、unit tests、SPIR-V tests 和 Windows TAEF gate 全部通过。

### M5：RadRay client、SDK 与全量验收（Windows consumer/可选构建 gate 已完成）

1. package 新 fork SDK，更新 manifest/hash；迁移 RadRay encoder/client 到 typed discovery request。
2. 删除 RadRay 本地 discoverer 和平台 fallback，整理 consumer tests 与 goldens。
3. 更新 `CONTEXT.md`、shader architecture/build guide 与完成状态；执行 compiler-enabled、JIT-disabled、
   compiler-free、D3D12、Vulkan 和跨平台 gates。

检查站：旧 package/IID/wire 全部 fail closed；RadRay client/metadata/include/consumer gates 已通过；
2026-08-14 的 compiler-enabled、JIT-disabled 与 compiler-free 全量 CTest 分别为 219/219、209/209、
179/179，compiler-free build tree 不含 DXC 文件或 cache 项。`.radray.4` SDK 已在临时目录成功打包；
发布 archive/hash 尚未写入 manifest。DXC 的 Windows TAEF gate 需在提供 TAEF 开发头与库的环境补跑；
当前机器只有 TAEF runtime。runtime/render 不新增 DXC SDK 依赖。

**测试所用 DLL 的必要前置**：`radray_dxc_runtime_deploy` 从 `SDKs/radray_dxc/extracted` 拷贝 DLL，
而 manifest 仍固定已发布的 `.radray.3`。因此任何 `cmake --build` 都会把手工部署到
`build_debug/_build/<Config>/` 的工作树 fork DLL 覆盖回 `.radray.3`。要验证未发布的 fork 改动，
必须在 build 之后、ctest 之前重新拷贝 fork 的 `dxcompiler.dll`，否则测得的是旧编译器。
`.radray.3` 上 `MergeTypeFacts` 按下标逐项比较 per-stage type 数组，两个 stage 使用不同 cbuffer
集合即误报 2107；工作里的 name-keyed union 已修复该缺陷。同一源在两个 DLL 下产出的 22 个
`shader_artifacts/*.bin` golden 内容全部不同，因此 fixture 必须由 fork DLL 生成。

## 验证门槛

DXC fork 必须依次通过其现有 `check-clang`、Clang HLSL unit、SPIR-V 与 Windows TAEF/D3D12 targets；
不得以三个被删除 executable 作为验收入口。RadRay 按 `docs/guide/build-test.md` 执行：

```powershell
python tools/fetch_sdks.py restore
cmake --preset win-x64-debug
cmake --build build_debug --parallel 24
ctest --test-dir build_debug -C Debug -R "RadRayShaderCompilerClient|RadRayDxcMetadata|RadRayShaderLibPass|RadRayRuntimeShaderJit|RadRayRenderPsoSmoke" --output-on-failure
python tools/check_docs.py
git diff --check
```

build 与 test 不并行。另做非 Windows compiler-client/semantic suite、JIT-disabled 和
`RADRAY_BUILD_SHADER_COMPILER=OFF` 配置；用 build commands/link map 核对依赖边界，不用运行期 DLL
枚举推断 Vulkan/SDK link edge。

## 完成定义

- fork 与 RadRay active code 都不再解析 HLSL source/preprocessed text 来推断 contract 或 metadata。
- discovery、DXIL、SPIR-V 使用同一 shared compiler setup；include、defines、policy 和 diagnostics 行为一致。
- metadata 只描述对应 bytecode 的最终 active target facts；public wire/hash 不含 AST pointer、物理路径或
  compiler-private identity。
- 新 ABI/package 跨平台可用，旧版本 fail closed；无 fallback、partial lane publication 或隐藏路径。
- 三个 fork standalone targets/files 和 RadRay 的本地 scanner/独立 ABI probe 已删除，测试职责落在
  DXC 自有 harness 与 RadRay consumer suites 的正确边界；当前环境缺 DXC Windows TAEF 开发头与库，
  非 Windows gate 也不能在本机执行。
- 全量 compiler、client、JIT、D3D12/Vulkan、compiler-free 与 docs gates 通过后，才能把本计划标记完成。

### 未修复缺陷

- `CollectContractWithFrontend` 把 discovery profile 硬编码为 `lib_6_1`，忽略了请求里的
  `Policy.ShaderModel`。SM 6.2+ 才有的语言特性会在 discovery 阶段就报错，例如 SM 6.6 的
  `ResourceDescriptorHeap` 得到 `use of undeclared identifier`。concrete compile 已按
  `ProfileForStage` 使用请求的 shader model，所以只有 discovery 这一条路径落后。
  `SupportedNonDefaultCompilePolicyCompiles` 用 `ShaderModel = 65` 但源码只有 SM 6.0 特性，
  因此测不到。修复方案是让 discovery 也从请求的 shader model 推导 `lib_6_x`。
- `kMaxEntryCount` 在 `dxcradray.cpp` 中声明后从未使用，entry 数量实际由 contract 的
  stage 唯一性规则约束。要么用它给 `EntryPoints` 设上界，要么删除。

## 非目标

- 不扩展 vertex/pixel/compute 之外的新 shader stage；本轮只收紧既有 binding ABI，要求 live 资源同时
  显式写 `VK_BINDING` 与 `register()`，不新增 numbered binding wrapper 或 sidecar metadata。
- 不实现 include dependency graph、compiler cache invalidation、hermetic source bundle、cook publisher 或
  content-addressed artifact storage。
- 不公开 AST/reflection API，不跨 call 缓存 AST，不把 include bytes/path 加入 shader identity。
- 不要求穷举完整 keyword domain；每个实际 discovery/compile request 对其 requested targets 与 concrete
  assignment 负责，AOT coverage 仍由 caller/cook 决定。
