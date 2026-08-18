# ADR-0044 Material 拥有完整固定功能状态基线，attachment 格式由 pass 注入

状态: 生效
日期: 2026-08
影响: `modules/runtime/include/radray/runtime/render_framework/render_pipeline.h`（删除 `MaterialRenderState`）、
新增 material 类型、PSO 构造入口、`modules/core/include/radray/vertex_data.h`（topology）

## 背景

`MaterialRenderState` 以三态 `optional` 表达"材质对 PSO 固定功能状态的覆盖"：`false` = 不覆盖，
`true` + 有值 = 覆盖为开启，`true` + `nullopt` = 强制关闭。它只覆盖 Cull / DepthWrite / Blend
三项，而且结构体自己的注释就记录了两个未决问题：**基线由谁提供尚未裁决**，以及
Topology / FrontFace / DepthCompare / target Format 无人负责。

它当前零使用点。与此同时 `example_lambert_sphere` 在建 PSO 时手填了完整的
`PrimitiveState::Default()`、`DepthStencilState::Default()` 加一次 `Format` 赋值、
`ColorTargetState::Default(kColorFormat)` 与 `MultiSampleState::Default()` —— 也就是说
"基线"这件事在样例里已经存在，只是以复制粘贴的形式存在。

三态覆盖模型的前提是"存在一个别处定义的基线"。既然那个基线从未出现，覆盖模型就没有被覆盖的
对象；而一旦为它补一个基线，同一个状态字段就会有两个可写的地方。

## 决策

**删除 `MaterialRenderState`。Material 直接持有完整的固定功能状态基线，没有三态。**

Material 拥有的状态：

| 字段 | 内容 | 理由 |
|---|---|---|
| `Primitive` | `render::PrimitiveState` 去掉 topology 之外的全部字段（FrontFace / Cull / PolygonMode / UnclippedDepth / Conservative） | 双面材质、线框、深度夹取都是材质表现 |
| `DepthStencil` | `render::DepthStencilState` 去掉 `Format` | 深度比较函数与写入开关是材质表现 |
| `Blend` / `WriteMask` | `optional<render::BlendState>` + `render::ColorWrites` | 混合模式是材质表现 |
| `Queue` | `RenderQueue` | 决定材质进不透明还是半透明队列 |

**Material 不拥有的状态，以及它们的来源：**

- **`TextureFormat`（颜色与深度）与 `SampleCount`**：由 pass 的 attachment 布局在建 PSO 时注入。
  材质不知道自己会被画进哪个 render target，这是 pass 的事实。同一材质出现在主 pass 与
  shadow pass 时格式不同，而它仍然是同一个材质。
- **`Topology`**：几何事实，归 `MeshPrimitive`（见下）。
- **`CompatibleRenderPass`**：pass 的事实，与格式同源。

**`PrimitiveTopology` 移入 `MeshPrimitive`**，默认 `TriangleList`。理由：topology 描述"索引缓冲
里的数字怎么组成图元"，改变它会改变几何含义，与材质无关。同一份 mesh 用不同材质绘制时
topology 不变；同一材质画 triangle list 与 line list 需要两个 PSO，而这个差异的来源在几何侧。
这会改动 `modules/core` 的 `vertex_data.h`，是本决策接受的代价。

由此 PSO 的完整输入被分成三个来源，各自唯一：

```
Material    → 固定功能状态基线（cull / depth compare / blend / write mask）
MeshPrimitive → topology + PrimitiveVertexLayout（vertex input state）
Pass        → color/depth format、sample count、CompatibleRenderPass
ShaderProgram → layout + stage bytecode + PSO 缓存（见 ADR-0046）
```

## 放弃的方案及代价

- **保留三态覆盖，另找一个基线来源**（例如 pipeline 或 pass 提供基线，material 覆盖）。
  这需要回答"基线放哪"，而无论放哪，一个状态字段都会有两个可写位置，读代码时必须同时看两处
  才能知道最终值。三态本身也引入了一个不自然的编码：`true + nullopt` 表示"强制关闭"，而
  `false` 表示"不管"，两者在类型上无法区分意图，只能靠注释。
- **Material 也拥有 format 与 sample count**。写起来最短（PSO 描述几乎可以整份来自 material），
  但同一材质无法同时用于不同 attachment 布局的 pass，shadow pass 与主 pass 必须各配一份材质。
- **topology 留在 material**。避免改动 `modules/core`，但会出现"同一份 mesh 配一个 line 材质就
  变成线框"这种几何含义被材质改写的情况；而真正需要线框时改的是 `PolygonMode`，不是 topology。
- **PSO 描述整份由调用方手填**（即维持样例现状）。零抽象成本，但每个 pass 作者都要重复
  一遍格式、depth 格式、multisample 与 blend 的默认值，任何一处写错只会表现为渲染异常而不是
  编译或创建失败。

## 必须保持为真

- `rg MaterialRenderState` 无命中。
- material 不含 `TextureFormat`、`SampleCount` 或 `RenderPass*` 字段。
- `MeshPrimitive` 携带 topology；material 不含 topology 字段。
- PSO 创建入口同时接收 material 状态基线、几何 vertex layout/topology 与 pass 的 attachment
  布局；任一来源缺失时不得用默认值补齐，必须创建失败。
- 同一个 material 实例可以在两个 attachment 格式不同的 pass 中使用，且不需要复制材质。
