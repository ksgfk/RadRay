# ADR-0027 JIT include path list is an explicit construction input

状态: 生效
日期: 2026-08
影响: `modules/runtime/include/radray/runtime/shader_jit.h`、shader JIT callers、example/tests

## 背景

JIT 的 include search 依赖 caller 的工程布局，而 DXC fork 不应猜测路径。保留无参数构造会让
调用方无法区分“有意使用空 search list”和“忘记配置 shader project root”，并隐藏 JIT 的关键
运行时输入。

## 决策

`ShaderJit` 的构造函数显式接收 include path list；不提供默认 include path，也不从 CWD、可执行文件
目录或 `ApplicationRuntimeDescriptor` 推导路径。无 include 的 shader 工程仍可创建 JIT，但必须
显式传入空数组。路径列表随后由 JIT 持有并在生命周期内保持不变。

## 放弃的方案及代价

- **无参数构造并隐式使用 `shaderlib`**：耦合项目布局且无法服务独立工程。
- **无参数构造等价于空列表**：调用方遗漏配置时不会在 API 层暴露意图，错误会推迟到 include 失败。
- **从 CWD/可执行文件目录自动推导**：把部署环境策略偷偷写入 runtime JIT，且不适用于多工程进程。

## 必须保持为真

- 每个 `ShaderJit` construction 都明确给出 include path list，包括空列表。
- JIT 不会替 caller 发现或修正 include root。
- DXC fork 不内置 RadRay 项目路径。
