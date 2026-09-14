> - 适用: 重构前回顾旧渲染框架的职责、数据流与设计约束
> - 权威: 删除前的历史设计快照；不代表新框架方案或当前可调用 API
> - 锚点: `AGENTS.md`, `modules/runtime/`, `modules/render/`, `modules/shader/`
> - 基线: `a68237fb44d8cb3025de8f08047a6a3ec17fec92`（原 main）

# 旧渲染框架设计快照

本快照在删除代码前保存。重构分支为 `refactor/render-framework-reset`。
本文根据基线实现及所属架构文档整理；细节保留在
[渲染框架原文](render-framework-legacy.md)、[Renderer foundation 原文](renderer-foundation-legacy.md)
和 [ImGui 原文](runtime-imgui-legacy.md)。附件中的现状、代码路径和能力均只对应基线。

## 本次重构边界

上层删除以 `render_framework` 及其直接依赖为边界，包含 ImGui。用户同时确认移除 Material/MaterialTechnique、
ShaderProgram 的 PSO 缓存、primitive_vertex_layout 与 material_state；下表说明最终删除范围。

| 范围 | 处理 |
|---|---|
| `modules/runtime/{include/radray/runtime,src}/render_framework/` | 删除旧 Scene/proxy、draw/list、RenderGraph、output/workload、pool/history 等实现 |
| `forward_pipeline/`、ImGui 图适配、依赖旧框架的样例与产品 shader | 删除，以及其专用生成、验证工具和测试 |
| `RenderSystem`、Application、WindowManager | 保留基础宿主、shader/cache 与 RHI 服务，拆除旧管线装配、Scene 与 output/graph 接口 |
| `game_framework/` 与 `components/` | 保留 Actor/World、注册、tick、变换、相机、灯光和网格数据；拆除 Scene/proxy 与旧 Material 桥接 |
| Material/MaterialTechnique 及其状态、队列、cbuffer 视图 | 删除；保留独立的 ShaderParameterLayout/Storage |
| ShaderProgram、GPU 网格 | 保留 shader/artifact/layout 与 GPU 上传；移除 PSO 缓存、primitive_vertex_layout/resolver，上传校验保留 |
| GPU/flight、上传、提交完成、资产、服务系统 | 保留；只处理旧框架带来的 include 或连接 |
| runtime WindowInputRouter 及其接线和测试 | 按用户要求删除；NativeWindow 原始事件保留 |
| ApplicationExtension 及其安装入口、回调、所有权和测试 | 按用户要求删除；Application 固定帧序保留 |
| core、RHI/backend、window、shader、shader compiler 库 | 保留。shader compiler 测试中仅移除旧产品 shader 的测试条目 |
| `third_party/`、`SDKs/`、本地 assets | 不属于删除范围 |

## 总体分层与依赖

```text
Application / runner                       决定何时 update / record / submit
  ├─ GpuSystem                            device、queue、flight、上传与完成通知
  ├─ WindowManager                        native window、swapchain、输入
  ├─ AssetManager / AssetDatabase          资产寿命与加载
  ├─ World → Actor → Component            游戏侧可变状态
  └─ RenderSystem                         决定画什么、如何组织一张图
       ├─ Scene / SceneRenderState         GT 场景发布与 CPU draw 目录
       ├─ RenderOutputRegistry             呈现与外部输出身份
       ├─ RenderPipeline + overlays        产品及 UI 组件
       ├─ FrameGraphComposer               显式 ports 装配
       ├─ RenderGraphRuntime               编译计划、flight 资源、实例和模板缓存
       ├─ ViewStateRegistry                时域有效性与历史资源
       └─ ShaderProgramCache / RenderPassRegistry

ForwardPipeline → render_framework → runtime 基础服务 → render / shader / core
ImGuiSystem → ApplicationExtension + RenderGraphComponent → runtime
```

RenderSystem 拥有渲染策略与框架缓存，借用 GpuSystem。图没有自己的提交线程、flight 或 acquire/present 协议。
Forward 不依赖 ImGui；UI 通过通用 overlay/port 机制接入。CPU 绘制准备在调用线程串行执行。

## 一帧的数据流

1. runner 确认 flight 可写，GpuSystem 处理完成通知和上传调度；旧 retained asset refs 在该槽安全复用时释放。
2. GT 执行 extension begin、资产 Pump、ApplicationScheduler Pump、输入与 OnUpdate、World tick。
3. pipeline、overlays、composer 先 `CollectScenePolicies`；宿主统一 `FreezeRegisteredScenes`，形成全帧输入截止点。
4. `PrepareFrame` 将相机、设置、view families、输出请求和 RenderGraphRuntimeOptions 冻结到当前 flight；此时保留资产 owner。
5. RT 根据 frame plan 取得输出并 resolve view，composer 连接 ports，组件 `BuildGraph` 声明资源、pass 与 CPU work。
6. graph 执行 Compile → Realize → live work → upload → pass prepare → barrier/输出路由补丁 → Record。
7. host 对未写输出 fallback clear，转换到 required final state。GpuSystem 结束 CB、Submit、Present。
8. Submit 返回后发布外部状态与有效提交结果；fence 完成后完成 readback、释放 owner，再由 GT 观察 flight 退休。

GT/RT 的传递内容是值快照与明确保活的资源。RT 不回读 Scene、proxy、CameraComponent、Material 或 AssetManager。
`Recorded`、`Submitted`、`GpuCompleted` 是不同状态：RHI 的 Submit 返回 void，返回只表明调用完成，不能证明 GPU 执行成功。

## Scene、proxy 与增量发布

- 组件注册时创建 proxy，Scene 持有 proxy；对象身份为 slot + generation，避免移除复用后的 ABA。
- 变换更新原地作用于 proxy，普通移动增加 TransformRevision；瞬移、网格不连续变化使用独立 motion revision。
- Scene 持有 SceneRenderState 与 CpuDrawStore，合并本次变更，向可写 flight 增量发布不可变快照。
- 同一 Scene、prepare serial、flight 的消费者共享发布结果。失败不能推进已提交基线；重试仍需看见未提交变更。
- 快照包含 primitive 变换、bounds/layer、batch 范围、材质映射、灯光值与稳定 DrawRecord 目录；view 只筛选可见子集。
- 静态网格 proxy 持有 StreamingAssetRef；几何内部的裸 DrawData 指针依靠 owner 保活。GT 把几何和纹理 owner 复制给 flight。
- 对象扩展与材质数据采用按页/版本复用。被已发布 flight 或外部读者持有的页不会原地改写；兼容消费者可共享扩展。
- Culling 使用 world bounds 和 layer mask；无效 bounds 保守可见，避免错误剔除。

基线入口：`render_framework/scene.h`、`scene.cpp`、`render_scene_snapshot.*`、`cpu_draw_record.h`、
`cpu_draw_store.cpp`、`scene_change_set.h`、`scene_render_extension.h`。

## 材质、shader 与绘制准备

MaterialTechnique 是不可变的多 pass 表，primary pass 定义 canonical 参数布局；Material 保存数值、纹理、sampler 和逐 pass 固定功能状态。
generation/revision 将身份、结构、数值、binding 与资源就绪变化分开。GT 的 BuildRenderData 生成值快照，并保留纹理资产，
不在快照阶段创建原生 descriptor。缺资源只使消费它的 pass 无效。

ShaderProgram 拥有已解析 artifact/layout、shader、参数索引和 PSO cache。graphics key 由材质状态、几何顶点布局、topology、
attachment 格式与 sample count 构成，不含 framebuffer 尺寸、Load/Store 或 render pass 地址。
ShaderProgramCache 分离 artifact key 与 program/layout key；失败按完整 key 缓存，source revision 使后续请求重新编译，旧 program 继续存活。
JIT 是可选依赖，预编译 artifact 的消费不依赖 DXC。

通用 RendererList 根据 pass/queue/mask 分类并排序。Forward 每个 view 做一次 Cull，合并准备 DepthOnly/Opaque/Transparent 列表。
静态绘制目录发布 record 索引和 FrameDrawBindingId，未知或 view-dependent batch 才走拥有数据的动态命令适配。
所有列表共享当前 flight 的 FrameDrawResources；不跨帧复用 Rg handles 或 arena 切片。

常量走「GT 冻结 → 可见集合 gather」：对象、材质、view 数值按身份和 revision 复用，录制前准备 CBV、groups、PSO。
带时域的对象数据还必须纳入 view/history 提供者、已提交 serial 与失效 revision，不能只凭 transform clean 跳过更新。

## Output、View 与装配

RenderOutputId 是进程内单调且不复用的身份。呈现输出随 AppWindow attach/detach 注册注销，重建保留 ID；
外部输出借用 texture/view，调用方负责 GPU 安全寿命。v1 外部输出只支持单 mip/layer 的 2D color RT。
注册修改必须在 GT，并通过 runner waiter 排空已发布的渲染工作；读取已发布状态不触发等待。

RenderFramePlan 是值目录：请求输出去重，view family 引用目标与稳定 view ID。host 只 acquire 被请求的目标；
单个目标不可用不阻止其他 family，也不阻止 side-effect compute/copy。
view resolve 根据输出尺寸、scale、rect/scissor、projection 与 jitter 算实际工作尺寸，并检查 device limits。
history 身份可选；零 ID 不保留时域状态。

FrameGraph 的 typed ports 必须且只能连接一次，描述符精确匹配；依赖决定顺序，缺连接和环在 freeze 前拒绝。
无 overlay 时默认 composer 直接连接场景输出；有 overlay 时使用线性 RGBA16_FLOAT canvas，串接 scene 与 overlays，
最后统一编码到目标。PreserveContents 时先载入原内容；非 sRGB UNORM 显式编码，sRGB attachment 由硬件编码。

## RenderGraph 编译和执行

资源值携带 resource index、graph generation、content version。版本表示同一物理存储上的内容演进；
写现有资源用 NextVersion，不能借不同逻辑槽分叉同一存储。需要同时保留新旧内容时必须显式 copy。
texture 访问规范化到 mip/layer/aspect，buffer 到字节范围；Load/ReadWrite 消费前驱有效内容，Discard 不产生可读内容。

编译从 export、observable output 和 side-effect 反向标记 live，只用内容依赖做存活分析；
随后为 live passes 添加 RAW/WAR/WAW 存储约束、稳定拓扑排序、环检查、生命周期与物理资源计划。
hazard 不参与 liveness，因此覆盖写可以裁掉旧 producer。非法内容读取即使最终被裁剪也会拒绝。

执行前统一完成 allocation、参数/PSO 和 Ready 校验，失败时不进入图内 Record。窄 commands facade 不暴露任意 barrier/submit 旁路。
prepare 只运行 live work/pass，work 最多一次并合并消费者 mask；work 之间不依赖注册顺序。
写入 flip backbuffer 的 pass 与 Present export 路由到对应窗口的 CB，其余进入共享 Direct CB。

可选优化包括 compatible resource reuse、attachment store 裁剪、raster merge、barrier elimination/batch。
相邻 raster 合并要求 attachment/view、Load/Store 与访问兼容；逻辑回调和票据仍各自保留。
同一原生资源重复 import 会合并身份并验证状态一致，不能通过别名绕过访问与有效性检查。

## 复用、资源寿命与时域

- CompiledFramePlan 缓存结构化结果；hash 命中后仍字段比较。native 地址、初始状态和帧 payload 不属于跨帧身份。
- RenderGraphTemplate 保存不可变 CPU recipe 和 typed frame slots，禁止冻结本帧 native import、即时数据、readback 或 owner。
- flight graph 实例池和 payload 池保留容器容量；每次借用获得新 generation，同时存活的 graph 不共享可写实例。
- transient pool 沿既有 flight fence 复用兼容资源；没有 heap aliasing。history 与 transient 分离，物理可复用不等于内容有效。
- history 只有成功提交的生产结果才前进；失败、跳帧、cut、尺寸/AA/结构变化不能误旋转历史。Forward TAA color/depth 使用三图环。
- FrameSubmission 持有提交状态写回与完成后释放的 owner/ticket/payload；Graph 析构不会提前销毁仍在飞行中的 GPU 资源。

## Forward 与 ImGui 产品层

Forward 的基础路径是可选 DepthPrepass、必有 Opaque、按需 Transparent。透明不进入预通道，深度只读。
HDR 扩展包括每帧共享的四 cascade 阴影、16×16 Forward+ tile lights、depth/normal/刚体 motion、深度金字塔、AO、
opaque/sky TAA、透明、indirect fireflies、Bloom、曝光/tone map 和 SDR 合成；MSAA4 与 AO/TAA 互斥。
辅助 view 跳过 TAA/AO/Bloom/Fireflies 等主视图效果，仍支持局部灯与共享阴影。局部灯或 tile 溢出不能静默丢灯。
输出还可作为 overlay 或世界空间屏幕，依赖必须无环。必需 pass/PSO/参数或末端输出失败不能推进 history。

ImGuiSystem 是可选模块，兼任 ApplicationExtension、flight completion observer 和图 overlay；负责 UI、平台窗口与 frame 数据。
图内图片用显式资源访问接入，UI 不旁路图的资源状态。其内置 shader artifact 可在 JIT 关闭时使用。
UI 的平台窗口/input capture、字体、绘制数据与 GPU 资源回收细节见历史附件。

## 诊断、测试与已知限制

Validation、Report、GpuMarkers 独立，并按 flight 冻结。性能默认 Off/Minimal/false；Full 校验在准备边界执行，
不替代必要的执行失败处理。HasFailed/FirstErrorCode 才是成败依据，不能用诊断数组是否为空判断。
Full 可输出 JSON/DOT、内容与 hazard 边、执行顺序、物理资源、barrier、culling 原因和缓存/分配统计。

旧测试覆盖纯 CPU 图编译、版本与子资源负例、pool/history、Scene 增量发布失败重试、renderer list、Ready/PSO/descriptor、
双后端图/compute/readback、Forward 时域与模板、overlay 编码和 ImGui。runtime profile/record harness 属于旧框架，
其 GT/RT CPU 观察时间不能当作 GPU 时间，也不能把历史运行结果当作本次验收。

基线没有 async compute、并行录制、heap aliasing、GPUScene、GPU count buffer、depth resolve、骨骼/形变 motion、
透明时域重投影或跨分辨率 history 重建。Scene/proxy 在 GT 常驻，但没有常驻 GPU 场景系统。
当前工作是拆除旧实现，不在本快照中预定新框架 API。

## 查阅基线

路径以仓库根为基准，例如：

```powershell
git show a68237fb44d8cb3025de8f08047a6a3ec17fec92:modules/runtime/src/render_system.cpp
git show a68237fb44d8cb3025de8f08047a6a3ec17fec92:modules/runtime/include/radray/runtime/render_framework/render_graph.h
git show a68237fb44d8cb3025de8f08047a6a3ec17fec92:modules/runtime/tests/CMakeLists.txt
```
