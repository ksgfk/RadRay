# ADR-0046 PSO 缓存归 ShaderProgram 所有，不建全局缓存

状态: 生效
日期: 2026-08
影响: 新增 runtime `ShaderProgram` 与 `RenderSystem` 上的 program 缓存；
`examples/example_lambert_sphere` 的 PSO 持有方式

## 背景

`example_lambert_sphere` 自己拥有四样东西：`BackendShaderArtifact`（含 `PipelineLayout`）、
两个 `render::Shader`、一个 `GraphicsPipelineState`。它只画一个球，所以"一个 pass 一个 PSO"
够用。一旦有多个材质、多个 attachment 格式或多种 vertex layout，PSO 数量就是三者的乘积，
需要缓存。

M-1 删除的旧 `PipelineStateCache` 以 `ShaderPassProgram*` 为 key，是一个跨 program 的全局缓存。
它带来的复杂度有明确记录：旧实现专门有两个用例守 `CacheMayDieBeforeItsHolders`，也就是缓存
可能比它的持有者先死，因此需要 detach、refcount 与关停顺序保证。
`docs/todo/hlsl-radray-dxc-shader-pipeline.md` 已判定它"与新契约无法对齐，故重写而非改造"。

同一份 TODO 也定了第一期的 layout 生命周期：**layout 与 compiled Variant artifact 一对一，
不共享**，理由正是"artifact 死则 layout 死"，没有 cache detach、refcount 或关停顺序问题。
`PipelineLayoutHash` 只作为 metadata 字段用于比较，不驱动任何缓存。

## 决策

**新增 runtime `ShaderProgram`，它拥有一个 concrete Variant 的全部 GPU 侧 shader 对象，
并自持一张 PSO map。不建立跨 program 的全局 PSO 缓存。**

`ShaderProgram` 拥有：

- `render::BackendShaderArtifact`（内含 `PipelineLayout` 与 decoded artifact bytes）
- 各 stage 的 `render::Shader`
- `unordered_map<PsoKey, unique_ptr<render::GraphicsPipelineState>>`
- ADR-0045 的扁平参数名索引

`PsoKey` 由 PSO 输入中**不属于 program 自身**的部分组成：material 的固定功能状态基线
（ADR-0044）、几何的 `PrimitiveVertexLayout` 与 topology、pass 的 attachment 格式/sample count/
`CompatibleRenderPass`。program 自身的 layout 与 bytecode 不进 key —— 它们对同一个 program 恒定。

**一个 `ShaderProgram` 对应一个 concrete Variant**，即一份 artifact。Variant 的枚举与选择策略
不属于本决策；material 直接指定它要哪个 program。

program 本身由 `RenderSystem` 缓存，key 为 `(逻辑 SourceName, 规范化的 keyword assignments)`。
编译失败时**缓存负结果**，避免每帧重编译刷屏；material 取不到 program 就跳过绘制。
**不提供 fallback shader** —— 那会成为渲染结果的第二份真相，且会把"shader 编译失败"伪装成
"画出了粉色"。

生命周期因此是单向的：`RenderSystem` → `ShaderProgram` → PSO。program 析构时它的 PSO 全部
析构，与 layout/artifact 同时消失。没有任何对象需要在缓存死亡时 detach。

## 放弃的方案及代价

- **重建跨 program 的全局 PSO 缓存**。PSO 复用率更高（不同 program 若 layout 兼容可共享？
  实际上不能 —— PSO 描述包含 `PipelineLayout*`，而 layout 与 artifact 一对一，所以跨 program
  复用本来就不成立）。真正的代价是重新引入 `CacheMayDieBeforeItsHolders` 那一类生命周期问题：
  缓存的生存期与持有者无序，需要 detach 与 refcount，而收益在 layout 不共享的前提下为零。
- **PSO 挂在 pass 或 pipeline 上**（即样例现状）。最简单，但同一个 material 在两个 pass 中使用
  时会各建一份 PSO，且 pass 作者要自己写缓存逻辑。
- **PSO 挂在 material 上**。material 是"用什么 shader 加什么参数"，同一 material 在不同
  attachment 布局下需要不同 PSO，缓存 key 仍然要含 pass 事实；把 map 放 material 上只是把
  同一张表按 material 切碎，命中率更低（多个 material 共享一个 program 时不能互相复用）。
- **按 `PipelineLayoutHash` 共享 layout，进而共享 PSO**。TODO 已把它记入"后续但不阻塞"：
  等 cook 落地、真实 variant 数量已知后再做，并需要重建 refcount/detach/关停顺序机制。
  第一期提前做等于在没有数据支撑的情况下付这笔复杂度。
- **编译失败时提供 fallback shader**。画面不会消失，便于继续操作。但它让 shader 错误不再表现
  为"什么都没画"，而是表现为"画错了"，并且引入一份必须与真实 pass 保持同步的假实现 ——
  与 ADR-0002 当年拒绝"在 CLI 里 stub 掉 `Device::Create`"是同一个理由。

## 必须保持为真

- `rg PipelineStateCache` 与 `rg SharedPipelineLayout` 无命中。
- `PsoKey` 不含 `PipelineLayout*`、bytecode、shader hash 或 program 指针。
- `ShaderProgram` 析构后不存在仍可访问的 PSO；不存在 detach 或 refcount 路径。
- program 编译失败被缓存为负结果；同一 key 不会在每帧重新触发编译。
- 不存在 fallback / error shader。
- `PipelineLayoutHash` 不作为任何缓存的 key。
