# ADR-0021 ShaderJit owning immutable include path

状态: 已被 ADR-0022 取代
日期: 2026-08
影响: `modules/runtime` shader JIT、`modules/shader_compiler` client、RadRay DXC extension invocation

## 背景

include directory 不能由 DXC fork 内置，也不应在每次 shader operation 由不同调用点临时拼接。
同一个 JIT 实例需要保证 contract discovery 与 concrete compile 看到相同的 include 文件集合和
解析规则，同时允许不同 shader 工程分别创建自己的 JIT。

## 决策

`ShaderJit` 构造函数接收 include path，并将其作为不可变实例状态保存。其所有 discovery 与 compile
操作都使用该值；不提供 setter，也不从 `ApplicationRuntimeDescriptor` 或进程全局状态读取。
JIT 需要切换 include 根时销毁并重新构造。

底层 shader compiler client/DXC fork 不拥有项目路径，也不设置默认路径。JIT/client 在每次真实
compiler invocation 时把已保存的路径作为普通 `-I` 传给 DXC；DXC 仍使用默认 filesystem include
handler 按需读取文件。路径不进入 canonical shader request、metadata 或 shader identity。

## 放弃的方案及代价

- **每个 operation 都重新传 include path**：容易使 discovery 与 compile 使用不同目录，且调用点
  需要重复管理同一不变量。
- **DXC fork 内置 `shaderlib` 或 CWD 约定**：把项目布局耦合进通用 compiler binary，独立工具和
  多工程进程无法复用。
- **提供运行时 setter**：会使同一个 JIT 实例在并发 operation 中产生不一致的 include 语义。

## 必须保持为真

- 一个 `ShaderJit` 实例的 include path 构造后不变。
- discovery 和 compile 使用同一个 JIT-owned path。
- DXC fork 不包含任何 RadRay 项目路径或默认 include directory。
- include path 与 include 内容都不进入 shader identity。
