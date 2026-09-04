# ADR-0047 binding group 分配属于具体 pipeline，runtime 提供内置 forward pipeline

状态: 部分被 ADR-0053 取代
日期: 2026-08
影响: 新增 `modules/runtime/{include/radray/runtime,src}/forward_pipeline/`、
新增 `shaderlib/pipelines/forward/`、删除 `shaderlib/passes/forward.hlsl`、
`docs/architecture/render-framework.md`、`docs/guide/shader-authoring.md`、
`modules/shader_compiler/tests/test_shaderlib_passes.cpp`、`modules/runtime/tests/test_runtime_shader_jit.cpp`

## 背景

binding group 同时是 D3D12 的 register space 与 Vulkan 的 descriptor set index，这是后端已硬化的
不变量（CONTEXT.md `Binding group`），任何一层都不重映射。`BindShaderParameterSet` 的
`groupIndex` 因此是唯一保留在公共层的数字。

但"group 0 装什么、group 1 装什么"从未定义。`example_lambert_sphere.hlsl` 把 Frame、Light、
AlbedoTexture、LinearSampler 四个 binding 全放在 group 0；`shaderlib/passes/forward.hlsl` 用的是
`VK_BINDING(6, 2)` + `register(t0)`（space 0），即 SPIR-V set 2 与 DXIL space 0 不一致 —— 这在
ADR-0016 下是合法的（两 target 数字不要求相等），但也说明当前没有任何约定。

一旦要区分 per-view / per-material / per-object 的更新频率（每帧一次 / 变更时 / 每 draw），
就必须给它们分配不同的 group：同一个 set 里混着这三类数据时，改一个 object 变换会迫使整个 set
重新写入。

问题是这个分配该不该成为引擎级硬约定。不同渲染管线的资源分层本来就不同 —— forward 需要
view/material/object 三层，deferred 的 lighting pass 只有 view + gbuffer，shadow pass 只有
light view + object。把一套编号钉在引擎上会让每条新管线都被迫遵守一个不适合它的分层。

## 决策

**binding group 的语义分配属于具体 `RenderPipeline`，不是引擎级约定。**

具体机制：

1. pipeline 声明自己的 group 分配，以**值结构**（`BindingGroupPlan{ViewGroup, MaterialGroup,
   ObjectGroup}`）传给它的执行器与 material。执行器与 material **不硬编码 group 数字**，
   也不假设 view 一定是 0。
2. HLSL 侧的 `register(bN, spaceM)` 与 `VK_BINDING(binding, set)` 仍由作者写死
   （ADR-0016 §2：作者声明是唯一 binding 真相，引擎不分配编号）。作者写的数字**必须与目标
   pipeline 的 plan 一致**；不一致时 layout 里查不到对应 group，pipeline 初始化 fail closed。
3. 引擎不校验"这个 pass 属于哪条 pipeline" —— 那不是可判定的事实。它只校验 plan 引用的 group
   在当前 artifact layout 中存在。

**runtime 提供一条内置 `ForwardPipeline`**，代码位于 `modules/runtime/.../forward_pipeline/`，
它的 plan 是 `group 0 = per-view、group 1 = per-material、group 2 = per-object`。
这推翻 `docs/architecture/render-framework.md` 现在的"runtime 不提供默认的具体管线"，
以及 ADR-0017 的"example 注入自己的 `LambertPipeline`"—— 样例改为使用内置 pipeline。
理由：`RenderPipeline` 框架已存在但十四个 `RenderPassEvent` 只有零个内置实现，
`Scene::Primitives()` / `Lights()` 零消费者。没有一条内置管线，"如何把场景画出来"就没有
可执行的答案，每个应用都要从 `BeginRenderPass` 开始重写。

**HLSL 侧对应地切成两层：**

- `shaderlib/lighting/lights.hlsli` **保留**在共享库：`DirectionalLight` / `PointLight` 结构与
  辐照度求值函数是"一个光源长什么样、怎么求值"，与管线无关。
- **新增** `shaderlib/pipelines/forward/`：view/lights 的 **cbuffer 布局**（有哪些光、装在哪个
  group 的哪个 binding、数量上限多少）与 forward 的产品 pass。这些是"这条管线怎么组织资源"。
- **删除** `shaderlib/passes/forward.hlsl`：它是 M7 建的 contract 回归样本，与内置管线的约定
  冲突，留着就是第二份 forward 定义。`test_shaderlib_passes.cpp` 与
  `test_runtime_shader_jit.cpp` 的 forward 用例改指向新的产品 pass —— 回归测试因此覆盖真实
  产品 pass，比覆盖一个合成样本更有价值。
- `shaderlib/passes/depth.hlsl` 与 `shaderlib/passes/compute.hlsl` **保留**：compute 是 JIT
  compute smoke 的输入，depth 是 CLI raw-lane 的证据，两者都不与管线约定冲突。

## 放弃的方案及代价

- **引擎级硬约定 group 0/1/2 = view/material/object**。执行器可以直接写常量，material 不需要
  接收 plan，shader 作者只要背一套编号。代价是每条新管线都要么接受这个分层，要么违反引擎约定；
  deferred lighting pass 与 shadow pass 都不是三层结构。
- **pipeline 用虚函数暴露 group 号**（`GetViewGroupIndex()`）。比值结构更"面向对象"，但会让
  执行器在录制热路径上做虚调用取常量，且 plan 的三个字段必须分三次取、无法整体校验。
- **引擎为 shader 分配 group，用 codegen 或 wrapper 宏保证一致**。彻底消除不一致的可能。
  但 ADR-0016 已明确禁止：不新增 binding attribute、不建 numbered binding wrapper、不维护
  sidecar metadata；`shaderlib/core/platform.hlsli` 只做 target gate，不分配 RadRay 自己的编号。
- **内置 pipeline 留在 `examples/`，runtime 只提供执行器与 material**。runtime 的公共面更小。
  但"怎么画"这件事就永远没有仓库内的权威答案，每个样例各写一份，且 `Scene` 与光照投影的
  消费者仍然在 runtime 之外，无法进 runtime 的回归测试。
- **保留 `passes/forward.hlsl` 作为 contract 样本，新产品 pass 另起名字**。不动现有测试期望值，
  改动最小。代价是 shaderlib 里有两条叫 forward 的东西，需要文档解释各自身份 ——
  而"需要注释解释边界"正是 CONTEXT.md 判定 cook/bake 命名为遗留的理由。

## 必须保持为真

- 执行器与 material 代码中不出现 group 数字字面量；group 号只来自 pipeline 的 plan。
- plan 引用的 group 在当前 artifact layout 中不存在时，pipeline 初始化失败，不回退到其他 group。
- `shaderlib/` 中不存在 RadRay 自己的 binding 编号分配器、numbered binding wrapper 或
  sidecar binding metadata；`register()` 与 `VK_BINDING` 仍由作者直接书写。
- `shaderlib/passes/forward.hlsl` 不存在；`shaderlib/lighting/lights.hlsli` 仍在共享库中且不含
  cbuffer 布局或 group/binding 号。
- `shaderlib/pipelines/forward/` 之外没有第二处声明 forward 的 view/lights cbuffer 布局。
- 内置 `ForwardPipeline` 不成为 `RenderSystem` 的默认值：`SetPipeline` 仍是显式注入，
  `_pipeline` 初始为 null。
