# ADR-0019 使用 DXC 默认 filesystem include search

状态: 已被 ADR-0020 取代
日期: 2026-08
影响: RadRay DXC fork extension、`radrayshadercompiler`、shader JIT、shader tools、`shaderlib/**`

## 背景

ADR-0018 已确定编译必须由文件系统即时读取 include，并删除 caller-owned `IncludeSource`。
随后对路径环境的进一步抽象引入了不必要的 `ShaderlibRoot` ABI 参数、client 生命周期状态和路径
预处理。DXC 本身已经提供标准的 include search 和默认 filesystem include handler；RadRay 的
authoring 约定也已经固定了工程根目录下的 `shaderlib` 布局。

## 决策

仅保留标准 DXC include 语义，不再引入独立 compile-environment API：

- extension 的 discovery/compile 调用都使用 DXC 默认 filesystem include handler。
- compiler invocation 以普通 `-I shaderlib` 作为唯一 include directory；路径按进程当前工作目录
  解释。样例、shader tools 与测试在工程根目录作为工作目录运行。
- 根 `.hlsl` 仍以调用方提供的原始 `RootSource` 和逻辑 `SourceName` 编译；共享 `.hlsli` 由 DXC
  在预处理过程中按需打开。调用方不递归扫描、不读取、不提交 include closure。
- 不实现“先试相对路径、再把同一路径转换为绝对路径”的自定义 fallback。绝对 include 本身由
  DXC 直接打开；需要绝对 include directory 时，应由调用方/启动约定提供绝对 `-I`，但当前 RadRay
  contract 不暴露第二套路径配置。
- 该回退只取代 ADR-0018 关于 compile environment/`ShaderlibRoot` ABI 的部分；删除
  `IncludeSource`、删除 `CompileInputHash` 以及 ABI/schema breaking upgrade 仍然有效。

## 放弃的方案及代价

- **新增 `ShaderlibRoot` ABI 参数**：会扩大 extension surface 和 request orchestration，却只是
  重复 DXC 已有的 `-I` 机制；放弃后工具必须遵守统一工作目录约定。
- **在 include handler 中实现相对路径到绝对路径的二次搜索**：会绕过 DXC 的 include-kind、
  includer-directory 和 ordered-search 语义，且同一目录的相对/绝对候选会造成无意义的重复。
- **继续递归收集 include**：仍然会把 compiler frontend 的职责复制到 caller，并阻止编译时读取
  当前磁盘内容。

## 必须保持为真

- discovery 与 compile 都使用同一套 DXC 默认 filesystem include 规则。
- 当前 shaderlib 根只有一个：工程根目录下的相对 `shaderlib`，以 `-I` 提供。
- root source 由调用方提交，transitive include 由 compiler 在调用期间读取。
- caller-owned `IncludeSource`、手工 include expansion 和 `CompileInputHash` 不得恢复。
- include 字节不进入 `ContractHash` 或其他稳定 shader input identity。
