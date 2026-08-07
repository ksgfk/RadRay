# ADR-0023 ordered include paths follow DXC shadowing

状态: 生效
日期: 2026-08
影响: RadRay DXC include invocation、shader JIT、shader tools、`shaderlib/**`

## 背景

JIT 现在保存的是一个有序 include directory 数组，而不是单一路径。多个 directory 中可能存在
同名 header；RadRay 如果自行去重、排序或扫描，会复制并改变 DXC 的 header search 语义。

## 决策

数组顺序原样转换为 DXC `-I` 顺序。对同一个 include name，DXC 首次命中的 directory 胜出；RadRay
不做预扫描、去重、排序、shadowing 检测或额外诊断。quoted/angled/absolute include 的具体查找
规则继续由 DXC frontend 和默认 filesystem include handler 决定。

## 放弃的方案及代价

- **编译前检测重复 header**：需要第二套文件系统扫描，且检测结果可能与 DXC 实际访问时刻不同。
- **按路径排序或去重**：会改变 caller 明确提供的优先级，导致编译结果悄然变化。
- **RadRay 自己实现 fallback 搜索**：会与 DXC 的 include kind、includer directory 和 `-I` 规则
  分叉，增加维护面。

## 必须保持为真

- include path list 的顺序是可观察的编译输入，但不是 shader identity。
- 首个命中的文件由 DXC 决定；RadRay 不插入额外搜索层。
- include 文件仍在实际编译期间按需从 filesystem 读取。
