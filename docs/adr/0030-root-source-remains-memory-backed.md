# ADR-0030 root source remains memory-backed

状态: 生效
日期: 2026-08
影响: source contract discovery、Variant compile、RadRay DXC extension、shader JIT

## 背景

filesystem-backed include 不等于要求 compiler 从 root source name 再打开一个物理 `.hlsl` 文件。
runtime JIT、editor 和测试可能已经拥有 root bytes，且 `SourceName` 还需要保持逻辑、可移植的
诊断名称。把 root 重新绑定到磁盘会造成双重 source authority，并使内存修改无法直接测试。

## 决策

caller 继续提交 root `.hlsl` 的原始 bytes 作为 DXC 的 main `DxcBuffer`，同时提交逻辑、非绝对的
`SourceName` 作为 virtual main-file name。DXC 不因 `SourceName` 重新读取 root 文件。

`<...>` include 按 caller 提供的有序 `-I` path list 搜索；若使用标准 `"..."` include，则 DXC
可以按 virtual main-file/includer directory 执行其既有相对查找，再继续标准 include search。RadRay
不在外部预展开或重写这些规则。

## 放弃的方案及代价

- **把 RootSource 改成 root file path**：会让 runtime JIT 失去内存 source 输入，并改变现有 ABI
  的 ownership；也不能解决 shared include 的即时读取问题。
- **把 root 与 include 一起预读成 closure**：恢复 caller-owned include snapshot，偏离标准 DXC
  frontend。
- **用 SourceName 作为物理路径**：将部署路径写进逻辑 source contract，并使诊断/复用不可移植。

## 必须保持为真

- RootSource bytes 是本次 operation 的 root source authority。
- SourceName 是 virtual/logical name，不触发 root 文件读取。
- include 文件仍由 DXC 在 filesystem-backed invocation 中按需读取。
