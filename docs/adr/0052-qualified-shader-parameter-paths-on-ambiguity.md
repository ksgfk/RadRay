# ADR-0052 Shader 参数以声明限定路径消歧，唯一叶名保留为简写

状态: 生效
日期: 2026-09
影响: `ShaderParameterLayout` 的参数索引、`ShaderParameterStorage`/`Material` 的命名 setter、shader type tree fixture

## 背景

ADR-0045 让 CPU 参数打包只消费 compiler type tree，但要求一个 program 内所有叶成员名全局唯一；两个 cbuffer 复用同一 payload struct，或两个不同成员路径以同名叶子结尾时，整个 program 因扁平名碰撞而创建失败。schema 7 又把每个 CBuffer declaration 到 lane-local payload root 的所有权显式写入 artifact，runtime 已能稳定构造完整声明限定路径，不再需要靠根类型顺序或全局叶名唯一性维持身份。

## 决策

**Shader 参数的身份是 `Binding.Member.Path` 形式的完整声明限定路径；全局唯一的叶成员名是同一参数的正式简写，不是第二个身份。**

机制：

1. `WireBindingRecord::TypeIndex` 选择当前 lane 的 payload root；runtime 从 binding declaration name 开始，递归追加每层 struct/member/struct-array 名称，生成 canonical path。
2. struct array 的元素仍由现有 setter 的 `element` 参数选择；`[index]` 不进入路径。
3. lookup 先查 canonical exact map，再查 short-name map。叶名第一次出现时可作简写；第二次出现后该简写永久标记为 ambiguous，lookup 返回不存在，layout 本身保持有效。
4. Texture/Sampler declaration name 是 exact top-level key。若它与某个 cbuffer 叶名相同，exact resource key 优先；cbuffer 字段仍通过 qualified path 可达。
5. setter 签名不增加分段 path、binding 参数或兼容重载。`ShaderParameterStorage` 与 `Material` 继续把一个字符串交给同一 `Find` 规则。

## 放弃的方案及代价

- **延续 ADR-0045 的全局扁平名唯一性，碰撞即拒绝 program。** 实现最小，但合法的 shared payload 与 nested-root shader 无法加载；碰撞影响的是简写，不应破坏 canonical identity。
- **重复叶名按首次或最后一次声明解析。** 不增加调用方字符数，但结果依赖 metadata 发射顺序，重新排序 binding 就会静默改写别的 buffer。
- **setter 分别接收 binding name 与 member path。** 类型上更显式，但会扩张全部 setter，并建立第二套调用形态；单个 canonical string 已能无歧义表达同一身份。
- **把数组下标编码进字符串。** 允许逐元素 key，但与现有 `element` 参数重复，增加解析与分配，并让同一 array declaration 产生不定数量的身份。

## 必须保持为真

- canonical path 始终从 HLSL declaration name 开始并包含完整成员路径。
- exact canonical lookup 优先于 short-name lookup；资源 exact key 不被同名 cbuffer 叶简写遮蔽。
- 唯一叶名继续可用；重复叶名只使简写 ambiguous，不使 layout 或 program 创建失败。
- ambiguous 简写写入返回失败且不修改任何 buffer。
- binding record 重排、两个 binding 共享 root、以及一个 root 同时被 binding 拥有和被另一 root 引用，都不改变路径对应的 buffer。
- struct array 元素只由 setter 的 `element` 参数选择，路径不包含数组下标。
