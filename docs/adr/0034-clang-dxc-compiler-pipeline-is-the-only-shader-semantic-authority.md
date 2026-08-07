# ADR-0034 Clang/DXC compiler pipeline 是唯一 shader 语义权威

状态: 生效
日期: 2026-08
影响: RadRay DXC fork、source contract discovery、typed Variant compile、target metadata、shader compiler client 与测试

## 背景

ADR-0016 已确定 HLSL 与 forked RadRay DXC 是 shader 权威。迁移前 extension 曾在 DXC frontend
之外扫描 root source 文本：`DiscoverSourceContract` 先用 `-P` 验证 include，再由 root-only scanner
识别 keyword pragma 和 stage attribute；`CompileVariant` 又由另一组 scanner 推断 resource、type、
vertex input、RootSignature 与 SPIR-V metadata。真实 bytecode 则由 Clang/DXC preprocessor、AST/Sema
和 target codegen 生成；该历史实现现已删除。

这使同一次编译拥有两套语义解释。手写扫描无法完整复现 tokenization、宏展开、条件编译、声明
合并、overload、include source origin、HLSL type/layout 和 target legalization；即使先扫描 `-P`
输出，也只是把错误边界从字符扫描移到预处理文本扫描，仍没有使用 compiler 已经建立的语义模型。

## 决策

### 1. 单一 compiler pipeline

所有会进入 source contract、metadata 或 shader identity 的 HLSL 事实必须来自同一次
Clang/DXC compiler pipeline。保留 `IRadRayDxcCompiler` 的两阶段外部形状：
`DiscoverSourceContract` 不生成 bytecode，`CompileVariant` 生成 requested target lanes；两者在
fork 内复用同一个 frontend invocation 与 semantic collector，不再调用任何源文本或预处理文本
scanner。

fork-private 实现以 `WrapperFrontendAction` 和 `MultiplexConsumer` 组合 RadRay collector 与 DXC
原 action。discovery 包装 syntax-only action；DXIL compile 包装 `EmitBCAction`；SPIR-V compile
包装 `EmitSpirvAction`。需要最终 target layout 的事实由 DXIL/SPIR-V codegen 在最终 module 已完成、
尚未序列化输出时交给同一次 invocation 的 observer。不得使用 global registry、TLS、plugin argument
中的 pointer、旁路文件或跨 invocation AST cache。

### 2. 事实来源

`#pragma radray_keyword_group` 由 compiler-owned `PragmaHandler` 在 preprocessor 中解析，并用
`SourceManager` 验证其 spelling location 位于 root main file 且不在 Variant conditional 内。
entry name、stage 与 shader kind 来自已完成 Sema 的 `FunctionDecl` 及 `HLSLShaderAttr`；entry declaration
可以来自 include，只有 keyword domain declaration 保持 root-only。

DXIL 的 active resources、register/space、RootSignature、static sampler 和 target-native type/layout
来自最终 DXIL compiler model。SPIR-V 的 set/binding、push constant、location、decoration 和
target-native type/layout 来自 legalization 后的最终 SPIR-V module；selected entry 的 active resource
集合由同一次 invocation 中 Sema 已解析的 AST declaration/call graph 选择，再以最终 module variable
读取 target facts。这样既遵守 target codegen 的 binding/type 结果，也不会把未被该 entry 使用的 module
global 发布到 stage metadata。AST declaration identity 只在一次 operation 内桥接 source declaration
与 target facts；物理路径、AST pointer 和该桥接 identity 均不得进入 public wire 或 hash。

compiler 内部先形成 target-independent `SourceContractFacts`，再形成每个 stage 的
`StageSemanticFacts`，最后合并为 target-specific lane metadata。这些都是 fork-private value data；
任何 Clang/LLVM 类型或借用生命周期都不得越过 extension ABI。

### 3. Discovery 与 identity

discovery request 改为 typed request，包含 `SourceName`、memory-backed `RootSource`、普通 `Defines`、
明确的 target selection 和 frontend-relevant `CompilePolicy`。ordered include paths 继续作为独立的
同步 borrowed ABI context；request 不接收 include closure，也不接收 keyword assignment。

每个 requested target 都执行真实 syntax-only frontend。请求两个 target 时，canonical keyword
domain、entry topology 和 shader kind 必须完全相同，才发布一份公共 `ShaderContract` 和
`ContractHash`。`CompileVariant` 使用具体 assignment 重新执行同一 contract collection，并与
`ExpectedContractHash` 及 discovery topology 比较；条件编译导致的 concrete topology 漂移必须失败。

`ContractHash` 只覆盖 canonical compiler-produced contract facts。`Defines`、policy、root/include
bytes、source name 与 include paths 不按原始输入字节进入 hash；它们只有在改变 contract facts 时
才间接改变 hash。include 依旧在 invocation 时从 filesystem 读取，compiler 不承担依赖快照、缓存
失效或内容寻址。

### 4. Compile、诊断与发布

`CompileVariant` 对每个 requested target 和 discovered entry 执行真实 target compile，并在产生
bytecode 的同一次 invocation 中采集 active semantic facts。graphics stage facts 在 fork 内按
declaration identity 和 stage visibility 合并；SPIR-V immutable sampler 所需的 DXIL static-sampler
分析可以作为不发布 bytecode 的辅助 lane，但不能增加 result 中的 requested target 集合。

malformed wire/path view、pragma、frontend contract 或非法 assignment 返回 `InvalidRequest`；expected
contract 漂移返回 `ContractMismatch`；target codegen、validation 或 target-fact extraction 失败返回
`TargetFailure`。语法和语义错误优先保留 Clang/DXC 的 source location diagnostics，只有 cross-stage、
cross-target 和 wire policy 使用 RadRay-owned stable diagnostics。任一 requested lane 失败时清空全部
lane output，只保留 diagnostics，继续遵守 batch atomicity。

### 5. 断代与测试归属

迁移采用一次性 ABI cutover，不提供 old scanner fallback 或兼容 adapter。新增 discovery fields 时
提升 discovery wire schema；extension ABI、compiler IID、toolchain/package identity 同步断代。compile
request、contract 和 metadata wire 只有在 binary shape 改变时才提升各自 schema，不能仅因内部实现
迁移而制造无意义格式版本。

compiler extension 的语义、wire 和 ABI 测试进入现有 Clang/LLVM/DXC lit、FileCheck、unit/TAEF
框架。删除 fork 中独立的 `radray_dxc_abi_probe`、`radray_dxc_d3d12_smoke` 和
`radray_wrong_abi_fixture` targets/files。RadRay 仓库只保留 client、decoder、JIT、render/runtime 和
package integration 的 consumer tests，不复制 compiler 内部语义测试。

本决策取代 ADR-0032。ADR-0032 的 `-P` include validation 加 root-only scanner 只记录已实现过的
过渡方案，不再是迁移完成后的合法设计。

## 放弃的方案及代价

- **继续维护 root-source scanner**：实现简单，但 comments、strings、宏、include、条件分支和真实
  declaration semantics 会持续偏离 compiler。
- **扫描 `-P` 的预处理输出**：能看到 active include，却丢失或扭曲 source origin，仍需重写 HLSL
  parser、Sema 和 target layout，并不能消除双语义权威。
- **编译后只反射 DXIL/SPIR-V bytecode**：可以得到部分 active binding，却拿不到 root-only pragma、
  完整 source declaration identity 和所有 policy diagnostics，也难以在 bytecode 与 metadata 之间
  保证同一次编译原子性。
- **通过 plugin/global callback 把事实带出 `IDxcCompiler3`**：改动点少，但引入进程级隐式状态、并发
  污染和生命周期风险。选择 fork-private invocation hook 的代价是要重构 DXC 内部 compile driver。
- **跨调用缓存 AST 或 include closure**：可能减少重复 frontend 成本，但会把 filesystem 稳定性和
  cache invalidation 重新塞回 compiler，并使 ABI 带上 compiler-private lifetime。
- **保留 scanner 作为 fallback 或发布期双跑比对**：会继续存在第二语义权威。迁移中可在开发分支
  临时对照 golden output，最终产品路径必须原子切换并删除 scanner。

## 必须保持为真

- active code 中不存在用于 shader contract 或 metadata 的手写 source/preprocessed-text scanner。
- discovery 与 concrete compile 使用同一套 Clang/DXC preprocessor、AST/Sema 配置、ordered include
  paths 和每次 invocation 新建的 default filesystem include handler。
- contract facts 来自 preprocessor/AST/Sema；target metadata 来自产生对应 bytecode 的最终 target
  compiler model，不从 root source 猜测。
- AST/LLVM pointers、physical include paths 和 compiler-private declaration keys 不进入 extension ABI、
  artifact wire 或 identity hash。
- 双 target contract 不一致、concrete assignment topology 漂移和任一 lane 失败都 fail closed，且不
  发布部分成功结果。
- extension 在 Windows 与非 Windows 构建使用同一语义实现；平台专属 D3D12 execution gate 只存在于
  现有 Windows test harness。
- fork 不再生成三个 RadRay 专用 standalone probe/smoke/fixture executable；compiler 语义回归由
  Clang/LLVM/DXC 自带测试框架承载。
