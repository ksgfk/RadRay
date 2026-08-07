# ADR-0031 default include handler per invocation

状态: 生效
日期: 2026-08
影响: RadRay DXC fork、source contract discovery、target stage compile、shader JIT

## 背景

现有 fork 在 `CompileStage` 中把 include handler 传为 `nullptr`，并在进入 DXC 前通过
`ExpandSourceInternal` 手工展开 caller 提供的 include bytes。这样既绕过了 DXC 的 filesystem
include 规则，也无法让 compiler 直接观察调用时磁盘上的最新 include 内容。

## 决策

每个真实 DXC invocation 都执行同一套流程：以 raw root `DxcBuffer` 和逻辑 `SourceName` 为主文件，
把 caller/JIT 提供的 ordered path list 转为 `-I` 参数，通过 `IDxcUtils::BuildArguments` 构造参数，
通过 `IDxcUtils::CreateDefaultIncludeHandler` 创建 handler，并将非空 handler 传给
`IDxcCompiler3::Compile`。discovery frontend 与 target stage compile 使用相同的 path list 和
filesystem handler 规则；每个 invocation 自己创建 handler，不跨 invocation 共享 source/include
cache。

删除 fork 内的 caller-owned `IncludeSource` wire、`ExpandSourceInternal`/`ExpandSource` 和 opened
include tracking。include 缺失、解析失败与 DXC frontend diagnostics 原样进入该 invocation 的
compiler result。

## 放弃的方案及代价

- **先手工展开再传 `nullptr` handler**：会复制预处理器语义，且把即时文件读取变成 caller snapshot。
- **复用一个跨 lane/stage 的 handler/cache**：会隐藏文件读取时序，并使并发 invocation 的资源
  lifetime 和变更语义复杂化。
- **只在 concrete compile 传 handler，discovery 继续 root-only 解析**：discovery 与 compile
  可能观察不同 include 内容，`ContractHash` 不再是同一 frontend 语义下的结果。

## 必须保持为真

- raw root source 直接交给 DXC；RadRay 不预展开 include。
- discovery 与 compile 都使用 caller-provided ordered `-I` paths 和 default filesystem handler。
- 每次 invocation 独立创建 handler；fork 不保留 caller path/blob pointer 或 include snapshot。
- include diagnostics 属于 compiler result，成功/失败 lane 的原子交付规则不变。
