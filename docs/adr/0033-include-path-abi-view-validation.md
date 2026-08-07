# ADR-0033 include path ABI view validation

状态: 生效
日期: 2026-08
影响: RadRay DXC extension ABI、shader compiler client、runtime ShaderJit

## 背景

filesystem-backed include 需要把 caller 的有序路径数组同步交给 fork，但 ABI 不能依赖
`std::filesystem::path`、STL 容器或 NUL 结尾字符串。空路径、嵌入 NUL 和不明确的字符串生命周期
若被静默接受，会让不同 caller 得到不同的 DXC 参数或产生悬空指针。

## 决策

extension ABI 定义独立的 `RadRayDxcIncludePathListView`：

```cpp
struct RadRayDxcIncludePathListView {
  const RadRayDxcBlobView* Paths;
  uint32_t Count;
};
```

每个 `RadRayDxcBlobView` 是一项显式长度的 UTF-8 path bytes，不要求 NUL 结尾。view 只在同步
`DiscoverSourceContract` 或 `CompileVariant` 调用期间借用；fork 不保存数组指针或元素指针。
`Count == 0` 时 `Paths == nullptr` 合法；`Count > 0` 时数组指针必须有效，每项必须是非空且不含
嵌入 NUL 的 UTF-8 path。违反这些输入约束返回 `InvalidRequest`，不跳过或重排元素。

## 放弃的方案及代价

- **把路径序列化进 shader request**：会让执行上下文进入 shader identity 和 request hash。
- **使用 NUL 结尾字符串数组**：需要额外的终止符约定，并无法复用现有 blob view 的长度安全边界。
- **静默忽略空或非法路径**：会改变 caller 指定的 `-I` 顺序，隐藏配置错误。

## 必须保持为真

- path list 是 request wire 之外的独立、同步借用输入。
- 数组顺序原样传递给 DXC；空列表不隐式补充任何路径。
- high-level client/JIT 在进入 ABI 前负责把自己的路径对象转换为 UTF-8 bytes，并保持 view 生命周期
  覆盖整个同步调用。
