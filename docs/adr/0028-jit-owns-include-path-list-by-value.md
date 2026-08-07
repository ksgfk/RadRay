# ADR-0028 JIT owns include path list by value

状态: 生效
日期: 2026-08
影响: `modules/runtime` shader JIT、`modules/shader_compiler` client、RadRay DXC ABI call boundary

## 背景

include path list 是 JIT 的固定运行时上下文。若 JIT 只保存 caller 的外部引用，caller 在并发
编译期间修改或销毁容器，就会使 ABI view 悬空或改变解析顺序。

## 决策

`ShaderJit` 按值接收 `vector<std::filesystem::path>` 并在 construction 时 move/own。构造后不提供
setter；所有 discovery/compile 入口保持 const，可以并发读取同一份 immutable list。每次同步 ABI
调用由 JIT/client 根据该 list 创建短生命周期的 borrowed UTF-8 view；fork 不异步保存任何 caller
pointer。

## 放弃的方案及代价

- **保存 `span`/引用**：生命周期和并发修改由 caller 负责，容易产生悬空 view 或非确定的 search
  order。
- **允许 setter**：同一个 JIT 的 discovery 与 compile 可能看到不同 include 根，且需要额外锁和
  operation generation 规则。
- **每次调用复制 caller list**：不能解决 JIT API 的固定上下文语义，也把 ownership 责任继续泄漏
  到调用点。

## 必须保持为真

- JIT 生命周期内 include path list 的值和顺序不变。
- const JIT operation 不修改路径状态，可以安全并发读取。
- ABI borrowed view 只在同步调用期间有效，compiler 不保存其地址。
