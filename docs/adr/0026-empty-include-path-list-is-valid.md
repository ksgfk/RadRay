# ADR-0026 empty include path list is valid

状态: 生效
日期: 2026-08
影响: shader JIT construction、RadRay DXC include invocation、shader compiler tests/tools

## 背景

并非所有 root `.hlsl` 都引用共享 `.hlsli`。如果把 include directory 缺失或为空当成 JIT 构造
错误，就会把 filesystem 资源预检和实际编译混在一起，也会阻止不含 include 的最小 shader 编译。

## 决策

include path list 允许为空。JIT/client 在该情况下不向 DXC 添加 `-I`；构造和 capability
`IsAvailable()` 不读取目录、不检查文件。只有 frontend 实际解析 include 时，DXC 才按标准规则
尝试打开文件并在失败时返回 diagnostics。

## 放弃的方案及代价

- **构造时强制要求至少一个 include root**：会禁止 include-free shader，并引入额外 filesystem
  preflight。
- **构造时验证所有目录存在**：不能保证编译时文件仍存在，也与“编译时即时读取”语义不一致。

## 必须保持为真

- 空列表不影响 compiler capability；include-free root source 仍可提交编译。
- 缺失 include 在实际 discovery/compile 中由 DXC 报告，不由 JIT 自己模拟。
