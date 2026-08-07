# ADR-0022 ShaderJit owning immutable include path list

状态: 生效
日期: 2026-08
影响: `modules/runtime` shader JIT、`modules/shader_compiler` client、RadRay DXC extension invocation

## 背景

一个 shader 工程可能需要多个有序 include directory。单一路径 API 无法表达标准 DXC 的多目录
search list，也会迫使 caller 在 compiler 外部复制搜索逻辑。与此同时，include 根仍然是 JIT 的
运行时上下文，不应随每次 shader request 改变。

## 决策

`ShaderJit` 构造函数接收一个 `vector<std::filesystem::path>` include path list，并按给定顺序保存
为不可变实例状态。所有 discovery 与 compile operation 都使用同一个列表；需要另一套搜索路径时
创建另一个 JIT，不提供 setter。

JIT/client 在每次实际 compiler invocation 时，将保存的列表逐项转换为 DXC 的标准 `-I` 参数并
传给 fork。DXC fork 不内置任何项目路径，也不保存跨调用的 include state；它使用默认 filesystem
include handler 按需读取文件。列表内容及其顺序不进入 canonical shader request、metadata 或
shader identity。

## 放弃的方案及代价

- **每次 operation 传单一路径**：无法表达标准 ordered include search，也会限制独立工具的目录
  组织；放弃后 caller 需要明确维护列表顺序。
- **在 JIT 运行期间修改列表**：会让并发 discovery/compile 看到不同的 header resolution；放弃后
  切换工程必须构造新的 JIT。
- **compiler 内置默认路径**：把工程布局绑定到 DXC binary；放弃后所有物理路径都由 JIT caller
  提供。

## 必须保持为真

- 一个 `ShaderJit` 实例的 include path list 构造后不变，discovery 与 compile 复用同一顺序。
- DXC fork 不包含任何 RadRay 项目路径或默认 include directory。
- 每个 path 按原值传给 DXC；不做预 canonicalize、存在性检查或 caller-owned include closure 收集。
- include path list、include 内容都不进入 shader identity。
