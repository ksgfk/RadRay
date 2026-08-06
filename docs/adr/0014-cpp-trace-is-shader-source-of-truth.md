# ADR-0014 shader 源真相是 C++ trace，绑定由 trace 产出

状态: 已被 ADR-0016 取代
日期: 2026-08
影响: 整个 `modules/shader`；`shaderlib/**`；`tools/shader_gen`、`tools/shader_cook`；
全部 `*.shader.json`。取代 [ADR-0003](0003-manifest-is-abi-authority.md)

## 背景

ADR-0003 的核心论证是"反射不足以构建 PSO"，并列出 7 项反射拿不到、必须由作者在
`*.shader.json` 里声明的信息。那条推理在**作者手写 HLSL** 的前提下完全成立。

代价是一套双向握手：作者写 HLSL，另写一份 manifest 声明同一批绑定，cook 期用反射核对两者。
这套握手催生了 `modules/shader` 18,431 行中的大部分，服务 1,472 行 HLSL —— 比例 12.5:1。
而按归属拆开，只有约 4,600 行是 DXC 反射本身的成本（DXC 调用、两套反射 JSON codec、
为跨平台 vendored 的 `d3d12shader.h` + `d3dcommon_adapter.h`），其余约 10,700 行是
manifest / 变体 / artifact 层 —— 即握手本身的成本。

握手的失败模式也不对称：反射看不见被 DCE 或 `#ifdef` 消掉的绑定（ADR-0003 第 5 项，
其正文称这一项"是致命的"），所以校验方向只能是"声明 ⊇ 反射"，manifest 多声明一个不存在的
绑定永远不会被发现。

## 决策

**放弃反射，改用 trace。** Shader 用 C++ 编写，执行它把运算记录成 AST，再由 codegen 同时
产出 HLSL 文本与精确的 binding layout。绑定不再被"发现"，而是在 trace 期被"构造"。

于是 ADR-0003 的 7 项理由**集体失效，而非被解决**：

| ADR-0003 的理由 | 在 trace 下为何消失 |
|---|---|
| 1. push constant 身份 | trace 期即知道这是 push constant，不需要从字节码反推 |
| 2. 绑定驻留方式 | 作者意图，在 C++ 里就是 trace 调用的实参 |
| 3. immutable sampler | 同上 |
| 4. unbounded 数组容量 | 同上 |
| 5. 被 DCE / `#ifdef` 消掉的绑定 | 不存在这个问题：layout 由声明构造，与代码是否走到无关 |
| 6. VertexFormat / slot / offset / stride | 顶点属性由带 attribute 的类型显式构造 |
| 7. entry point 名与 keyword 组合域 | 就是 C++ 函数与它的参数 |

**manifest 整体删除。** 它的全部内容要么由 trace 产出（绑定、顶点接口、push constant），
要么变成 C++ 代码（entry point、变体域、bake set，见 ADR-0015）。保留一份 manifest 做
交叉校验会把刚删掉的那套握手原样请回来 —— 且这次校验对象是自己刚生成的东西。

**DXC 保留，但降为 codegen 背后不可见的后端。** 不追求消除它：D3D12 retail 驱动只接收经
`dxil.dll` 签名的 DXIL，换任何前端都跑不掉这个依赖。作者不再接触 HLSL 与 DXC，这已经拿到
本轮要的全部收益。

**第一期不做离线编译，因此第一期没有发布路径。** 变体在运行期按需 trace 并编译，DXC 是
运行期必需品。这与现有策略（`render_system.cpp:67-70`：有 DXC 即开发构建，无 DXC 即发布包
且缺产物是显式错误）直接冲突 —— 冲突是有意接受的：第一期的目标是让整条链在开发构建下跑通，
发布形态与离线产物留给后续（见 ADR-0015）。在离线编译回来之前，不得声称支持发布构建。

**RHI 契约不动。** `ShaderDescriptor` / `PipelineLayoutDescriptor` / `VertexInputState` /
`PipelineStateCache` 保持现状，成为新 shader 层必须产出的目标形状。`modules/render` 的
16,829 行本来就不消费反射 —— root signature 与 descriptor set layout 全部从
`PipelineLayoutDescriptor` 构造 —— 所以它正是 trace 结果的干净落点。

## 放弃的方案及代价

- **保留 manifest 做交叉校验**。就是本 ADR 要删掉的那套握手。而且 trace 结果已经是权威，
  校验对象变成自己刚生成的东西，两份真相仍需人工同步。
- **只删 manifest 的绑定部分，保留变体与烘焙声明**。见 ADR-0015：变体在 C++ 里就是函数参数，
  保留一份配置文件意味着需要一层 JSON → C++ 变体参数的手写映射 —— 又是一道握手。
- **换 shading language（Slang）**。Slang 的反射确实能一次编译同时给出 D3D12 register/space
  与 Vulkan set/binding，也有 link-time specialization 直接对应变体系统。但它仍是"作者写
  shader 源文件、工具反射它"的形状，握手只是变窄而非消失；且 DXIL 路径仍需外部
  `dxcompiler.dll`（Slang 不随包分发），B 类问题一并不解决。
- **整体引入 LuisaCompute 作为上游**。RadRay 已有自己的 RHI、资产系统、帧调度、
  `radray::types` 容器别名与 stdexec 协程；两套 runtime 直接冲突。
- **用 LuisaCompute 的光栅前端**。它的 C++ DSL 表达不出顶点属性（`LUISA_STRUCT` 没有
  attribute 通道，整个 `dsl/` 目录 `Attribute` 零命中），只有另一套 clang 前端能；
  且 `AppData` 顶点输入结构写死、光栅只允许 AOT、光栅 stage 进不了它自己的 XIR→SPIR-V 通路
  （`ast2xir.cpp` 对 `RASTER_STAGE` 是 `LUISA_NOT_IMPLEMENTED()`），因而 Vulkan 侧光栅仍走
  HLSL→DXC。
- **反射驱动 + 运行时反射**。发布包没有 DXC；且 `PipelineLayout` 就无法在编译前构建。

## 必须保持为真

- 不存在 `*.shader.json`，也不存在任何"声明一遍绑定再核对"的路径。
- Binding layout 的唯一来源是 trace 产物。没有第二处声明它的地方。
- `modules/render` 的 RHI descriptor 类型不因本决策改变形状。
- Binding group 编号 == D3D12 register space == Vulkan descriptor set index，全链路不重映射。
- 作者编写 shader 时不接触 HLSL 文本，也不直接调用 DXC。
- 生成的 HLSL 是中间产物：可落盘用于调试，但不是任何流程的输入真相。
- 第一期：DXC 是运行期必需品，不存在无 DXC 的可用构建。此约束在离线编译落地后解除。
