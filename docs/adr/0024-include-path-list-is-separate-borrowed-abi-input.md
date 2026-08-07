# ADR-0024 include path list is a separate borrowed ABI input

状态: 生效
日期: 2026-08
影响: RadRay DXC extension ABI、`radrayshadercompiler` client、shader JIT、shader request wire

## 背景

filesystem-backed include 需要让 DXC 在每次调用时知道 caller 的 include directories，但这些路径
不是 shader 内容，也不应污染 canonical request、metadata 或 hash。把它们放进 request wire 会把
编译上下文和 shader identity 混在同一份序列化数据中；让 DXC 自己保存路径又会产生隐藏的可变状态。

## 决策

`IRadRayDxcCompiler::DiscoverSourceContract` 与 `CompileVariant` 都接收独立的 include-path-list
ABI view。view 由 caller 借用，仅在同步调用期间有效；fork 不保存其中的 pointer，也不把路径
写入 result。每一项是 caller 提供的 UTF-8 path bytes，顺序原样对应 DXC `-I` 参数顺序。

`ShaderJit` 以 `vector<std::filesystem::path>` 保存不可变路径列表，并在每次调用 client 时构造
临时 ABI view。request wire 仍只表达 root source、defines、assignments、targets、policy 和
expected contract；include path list 不参与 canonicalization、`ContractHash` 或 output identity。

## 放弃的方案及代价

- **把路径列表编码进 CompileVariantRequest**：会混淆 shader request 与外部编译上下文，并需要
  在 canonical wire 中额外定义路径语义。
- **让 fork compiler 对象保存 include paths**：会产生跨调用的 mutable state，破坏同一 compiler
  object 在不同工程间复用和并发调用的可解释性。
- **让 caller-owned view 在异步编译后继续存活**：ABI 不需要异步持有外部内存；调用完成前由
  caller 保证 view 和每项 path bytes 有效，结果只返回 compiler-owned blobs。

## 必须保持为真

- include path list 是独立的 per-call ABI input，不是 shader request wire 字段。
- view 只在同步调用期间借用；fork 不保存指针。
- JIT 实例内路径列表不可变，discovery 与 compile 使用同一顺序。
- DXC 仍使用默认 filesystem include handler，按需读取当前文件系统内容。
