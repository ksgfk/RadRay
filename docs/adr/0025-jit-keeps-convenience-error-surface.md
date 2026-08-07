# ADR-0025 JIT keeps a stateless convenience error surface

状态: 生效
日期: 2026-08
影响: `modules/shader_compiler` client、`modules/runtime` shader JIT、shader JIT example/tests

## 背景

filesystem-backed include 失败、DXC frontend error 和 contract mismatch 都需要在 compiler client
层保留诊断。runtime `ShaderJit` 当前是把 compiler result 转换成 artifact/contract 便捷接口，
并以 `optional` 表达失败；为了暴露额外错误而在 JIT 对象上放置 `LastDiagnostics` 会引入共享可变
状态，尤其不适合并发 JIT 调用。

## 决策

`shader_compiler::Client` 继续原样返回 `Status` 与 `Diagnostics`。`ShaderJit` 维持现有无状态的
optional convenience API，不保存上一次调用的诊断，也不通过全局 logger 替代 result。需要检查
具体 include/DXC 诊断的测试和工具直接使用 client result；runtime/example 的 JIT 失败只需按现有
调用约定处理失败。

## 放弃的方案及代价

- **在 ShaderJit 上增加 `LastDiagnostics`**：会产生并发竞态和生命周期歧义；调用方还可能读取到
  上一次 operation 的错误。
- **让 ShaderJit 直接打印 compiler diagnostics**：把 library API 与日志策略耦合，并妨碍上层选择
  自己的错误呈现方式。
- **让底层 client 丢弃 diagnostics**：会使缺失 include、路径 shadowing 和 frontend 错误难以定位。

## 必须保持为真

- compiler client result 总是拥有该次 operation 的 status/diagnostics。
- `ShaderJit` 不拥有跨调用的错误状态。
- 失败不会触发 include fallback、另一 target fallback 或 runtime reflection fallback。
