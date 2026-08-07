# ADR-0020 caller-supplied filesystem include paths

状态: 部分被 ADR-0021 取代
日期: 2026-08
影响: RadRay DXC fork extension、`radrayshadercompiler`、shader JIT、shader tools、`shaderlib/**`

## 背景

ADR-0019 试图把相对 `shaderlib` 作为 compiler 内建的唯一 `-I` 路径，并依赖进程 CWD。这个约定
把项目部署布局和 compiler binary 绑定在一起，也无法让不同调用方在自己的工程、测试 fixture 或
安装布局中选择 include root。include directory 是一次编译的外部输入，应由发起编译的调用方决定。

## 决策

- 每次 `DiscoverSourceContract` 与 `CompileVariant` 调用都由 caller 提供 filesystem include
  directories；compiler 将它们按调用方给定的顺序转换为标准 DXC `-I` 参数。
- RadRay DXC fork 不内置 `shaderlib`、工程根、CWD 或任何物理 include 路径，也不自动向上查找
  repository root。相对路径和绝对路径均由 caller 选择，并按 DXC/操作系统的普通路径规则解释。
- compiler 使用 DXC 默认 filesystem include handler。它在预处理需要 include 时才打开文件；
  caller 不递归收集、不读取、不提交 include 内容。
- RadRay HLSL authoring 仍使用相对于逻辑 shaderlib 根的尖括号 include，例如
  `#include <core/platform.hlsli>`。caller 负责把该逻辑根映射为本次调用的物理 `-I` directory。
- include directories 属于调用上下文，不进入 canonical shader request 或 shader identity。若
  discovery 与随后 compile 要求同一语义，caller 必须向两次调用提供同一组路径；compiler 不从
  request 或全局状态推断它们。
- 该决策取代 ADR-0019 关于 compiler 内建路径和 CWD 约定的全部内容；ADR-0018 中关于
  filesystem-backed、移除 `IncludeSource`/`CompileInputHash` 和 ABI/schema 断代的结论继续有效。

## 放弃的方案及代价

- **compiler 内建 `shaderlib` 相对路径**：简单但绑定项目布局，不能服务独立工具、测试目录和安装
  后的 shader 项目；放弃后调用方必须显式管理 include root。
- **在 compiler 内部自动搜索相对路径再搜索绝对路径**：会隐藏调用方的路径配置错误，并可能在
  多个工作区存在同名 header 时产生不可见的 shadowing；标准 `-I` 顺序已经足够表达搜索策略。
- **把 include directory 编入 shader identity**：会把物理部署环境污染到 shader 内容身份；include
  内容和路径的失效策略仍由外部构建/资产系统负责。

## 必须保持为真

- DXC fork 不携带任何项目特定的默认 include path。
- discovery 与 compile 都明确接收 caller-provided include directories，并使用标准 `-I` 顺序。
- include 文件在编译过程中按需从 filesystem 读取；没有 caller-owned include closure。
- include directory、include 内容都不进入 `ContractHash` 或稳定 shader input identity。
- shaderlib 仍是 authoring 的逻辑根；物理路径由 caller 显式映射。
