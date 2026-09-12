# RadRay runtime 下一步性能实施计划 v2：变更驱动的绘制准备

## 1. 决策、范围与基线

下一轮的主线是“Scene 变更时编译稳定绘制描述，快照时发布已准备数据，视图阶段只选择并实例化”。执行顺序是：建立可重复的 Release 基线；修复上一轮生命周期与平方级查找问题；完成合并式变更追踪、静态 draw/value 分离、per-flight 增量快照发布和索引式列表；完成构图之前的执行计划复用；最后精简录制并做完整 CPU 总账与跨 flight 验收。

本版本完整替代 `RadRay_Next_Step_Plan_20260911.md` 和对应 v1 验收 JSON。原 M0–M5 主线及 T01–T33 保留并修订；M2 扩为 M2a–M2d，新增 T34–T60。最重要的修正是：不再将 CpuDrawStore 的 dirty 同步一律推迟到后续 profiling；本轮就实现静态部分按变化准备和发布。但旧裸写 API、未知 proxy 的必要观察，以及逐帧资产保活，不被伪装成零成本。

本次重新读取的 GitHub 最新提交仍为 `542030a906fda526d0cbf3db4a309ff9007c8c1f`。v2 是设计与验收规格更新，不是实现提交或新一轮性能测量。

这不是再建立一套 Scene/RenderWorld、通用命令虚拟机或多级缓存体系。延续当前 setup / prepare / record 边界，扩展已有 CpuDrawStore 和 FrameDrawResources，将前者归入 Scene 的唯一 CPU 编译产物，将 SnapshotBuilder 收敛为发布器；同时把当前 RenderGraph 编译结果提升为可复用的完整执行计划。

**硬边界：所有生产代码和新增测试限制在 `modules/runtime`；GpuSystem 的接口、实现、flight/fence 语义不改；RHI 和后端实现不改；不做 GPU-driven，也不把 shader 改造、GPU indirect、instancing、D3D12 bundle 或多队列录制当作本轮前提。**外部模块当前使用的 RG API 由 runtime 内适配，不能为了完成快路径要求修改 ImGui 等调用方。

源码基线为 GitHub 默认分支本次返回的 `542030a906fda526d0cbf3db4a309ff9007c8c1f`，仍与上一轮静态审查相同。性能输入是 `20260911-190150-debug-tidal-d3d12-mt-imgui-noval.csv`。该 CSV 没有写入二进制 commit、工作区 dirty 状态、完整编译选项和导出命令；函数及行号与所读源码相符，但不能据此证明运行二进制与该 commit 完全一致。[S1][C0]

本文区分三种结论：CSV 中可直接复核的统计；源码可证明的结构性重复工作；尚未测量、作为下一轮验收要求提出的目标。**本文中的性能百分比目标不是已经实现的结果或加速保证。**本次没有编译工程、运行 D3D12/Metal 测试，也没有完整 `.tracy` 时间线。

## 2. 最新 Tracy 数据怎样影响优先级

### 2.1 统一统计口径

本次有 242 次 `RenderSystem::Render`、243 次 `TickFrame`，149 个导出行。下面的“每渲染帧摊销”统一按 `total_ns / 242 / 1e6` 计算，不使用某个 per-draw scope 的单次平均代替整帧代价。它是测量区间内的摊销，不是每一帧都出现相同调用次数的证明。[S1]

按源码中的嵌套关系，这些数据应按包含子 scope 的墙钟区间理解；CSV 没有导出模式元数据，因此还应在下一份 capture manifest 中明确 inclusive/self。`PrepareGroup` 的主要调用在 `PrepareCommand` 里面，后者又在 renderer list 构建中，不能相加为三个独立热点。`ExecuteGraph` 也包含 Compile、Realize、Prepare、PlanBarriers、Record。插桩墙钟区间还可能包括线程被调度出去的时间和下层等待，不等于退休指令对应的纯 CPU 忙碌时间。[C1][C4][C7][E1]

同名 pass 要保留 `name + src_file + src_line` 键。新 CSV 中 `render_graph.cpp:2163` 的 `Forward.Opaque` 是 prepare 回调，而 `:2320` 是 record；仅按名字合并会把不同阶段揉在一起。`percentile_ns` 列没有说明百分位参数，本文不把它当成 P95 或 P99。[S1][C7]

### 2.2 核心数据

| Scope | 次数 | 每渲染帧摊销（ms） | 解释 |
|---|---:|---:|---|
| `RenderSystem::Render` | 242 | 23.678 | 外层渲染区间，不等于完整端到端帧延迟 |
| `ComposeGraph` | 242 | 13.612 | 包含场景可见列表、命令准备和图声明，不是纯 RG 算法 |
| `ExecuteGraph` | 242 | 9.318 | 包含以下多个子阶段，不能与它们相加 |
| `RenderGraph::Compile` | 242 | 1.560 | 归一化、IR、查缓存、编译结果应用与优化 |
| `RenderGraph::Realize` | 242 | 0.448 | 资源、view/framebuffer 等实例化 |
| `RenderGraph::Prepare` | 242 | 2.144 | 存活 pass 的 prepare，包含描述符和 PSO 等工作 |
| `RenderGraph::PlanBarriers` | 242 | 0.371 | 当前每帧 barrier 规划 |
| `RenderGraph::Record` | 242 | 4.758 | 包括实际 RHI 调用及其下层成本 |
| `BuildRendererLists` | 741 | 5.501 | 主视图列表构建，包含 PrepareCommand |
| `BuildRendererList` | 880 | 2.424 | 本样例主要是阴影列表构建 |
| `PrepareCommand` | 311,862 | 6.427 | 6 ms 级主线；包含大量 PrepareGroup 工作 |
| `PrepareGroup` | 250,238 | 2.901 | 包含上传/绑定准备，不能与 PrepareCommand 相加 |
| `RecordRendererList` | 2,693 | 3.714 | 包含在 RenderGraph::Record 中 |
| `PrepareRendererList` | 2,693 | 0.457 | 本次样例较小，但有独立 mesh 的复杂度风险 |
| `CompileRenderGraph` | 33 | 0.043 | 只在 miss 时执行的核心编译算法 |
| `GpuSystem::SubmitFrame` | 242 | 0.447 | 保持不改，先测其输入和帧槽反压 |

上述原始值全部来自 [S1]。`ComposeGraph` 占 `RenderSystem::Render` 聚合时长的 57.5%，而整个 Compile 约 6.6%。这不是 CSV 的 `total_perc` 列；此处明确用 RenderSystem 的总时长作分母。

当前内层编译缓存命中 209 次、miss 33 次，命中率 86.36%，但 NormalizePasses、BuildIR、Compile 都执行 242 次。`CompileRenderGraph` 核心算法仅摊销 0.043 ms。仅优化拓扑排序、或者把 33 次 miss 进一步全部变成当前形式的 hit，不足以消除 13.612 ms 的 ComposeGraph。真正的图优化要绕过重复声明、归一化、IR 重建和后续稳定规划。[S1][C6]

### 2.3 与上一份 Debug 的有限对照

以下只作相同名字 scope 和调用量的描述性对照；未确认完整运行条件相同，不当作因果加速结论，也不与旧 Release 混比。

| 指标 | 10:57 Debug 摊销 ms | 19:01 Debug 摊销 ms | 下一步含义 |
|---|---:|---:|---|
| `ComposeGraph` | 17.106 | 13.612 | 阶段总量下降，但仍是最大渲染区间 |
| `ExecuteGraph` | 8.705 | 9.318 | 部分准备工作后移，不能只看 ComposeGraph |
| `PrepareCommand` | 6.332 | 6.427 | 主要逐命令工作基本没有减少 |
| `PrepareGroup` | 2.828 | 2.901 | 上传和绑定准备仍需单独降重复量 |
| `ForwardGraph::BuildGraph` | 2.121 | 0.523 | setup 的直接负担明显变小 |
| `PrepareRendererList` | 1.163 | 0.457 | 该函数本身已不是本样例最大成本 |
| `RenderGraph::Record` | 4.473 | 4.758 | 需要新 Release 和 RHI 对照，不能保证全是框架开销 |

旧 Debug 的 PrepareCommand 为 311,870 次，新 Debug 为 311,862 次；PrepareGroup 从 250,246 次变为 250,238 次。两组调用量几乎不变。它们不能证明改动毫无收益，却能说明“setup/prepare 分离”尚未消除这条逐命令工作链。[S1][S2]

### 2.4 不应该误判的三件事

`WaitWritableSlot` 平均 21.396 ms，不代表一个锁浪费了 21 ms。源码中此路径会等待渲染完成或 GPU fence；`WaitSubmittedFlight` 就嵌在其中。不能把等待时长与另一线程的 Render 相加，再以减少 flight 安全等待为优化手段。下一轮要关联同一 frame/flight 的生产、录制、提交与退休时间线。[S1][C9]

`ForwardPipeline::PrepareFrame` 的一次最大值是 468.830 ms，占该 scope 总时长约 57.2%；`WaitReadySlot` 最大 494.039 ms，占其总时长约 95.5%。说明此采样混有非常显著的长尾，可能是启动或其他冷路径，必须由完整时间线确认。不能把含长尾的均值直接定成稳态预算，也不能在没有事件依据时机械删除最慢帧。[S1]

本次文件名标记 Debug。优化目标最终应以有符号信息的优化构建为准；关闭 RG validation 不等于编译优化已打开，也不等于 RHI/驱动验证层、报告、GPU marker 全关。Microsoft 也将 Release 构建作为性能测量的基本工作流。[E1]

## 3. 目标架构与职责约束

保留以下最小结构，不再添加另一套世界表示。

| 结构 | 基于现有模块改造 | 保存什么 | 不保存什么 |
|---|---|---|---|
| 稳定 CPU draw 目录 | Scene 所属的 CpuDrawStore / DrawRecord | 几何/材质/程序稳定身份、pass/binding recipe、版本与 dirty 范围 | 本帧 view/graph 地址、资产永久引用、逐相机完整命令 |
| 已发布 scene 数据 | 既有 SnapshotBuilder / per-flight Snapshot | 该 flight 已拥有的稳定目录与当前对象/材质值，增量补齐 | 指向 GT 正在修改的 canonical 表；每帧重新推导静态描述 |
| 本帧绑定与可见数据 | FrameDrawResources / flight 数据 | view 常量、对象常量切片、材质绑定、DrawId/排序索引、最终 PSO 引用 | 一份重新生成的逻辑图；隐式跨 flight 复用的裸地址 |
| 不可变执行计划 | RenderGraph 编译产物扩展 | live 工作、资源槽、lifetimes、raster groups、barrier 模板、输出路由 | 上一帧 backbuffer 地址、瞬时 frame payload、旧外部初始状态 |
| 录制输入 | 收敛现有 PreparedRendererList | 直接可用的 draw 引用/连续索引和批次绑定 | 新的通用命令字节码、每 draw 字符串解析和图查询 |

目标热路径：

```text
World/Renderer 编辑 → Scene dirty 合并（GT，不调用 RHI）
    → 一次 Commit：更新受影响静态 recipe / 对象与材质值
    → 当前 writable flight：增量发布 Snapshot，沿原机制保留帧资产引用

取得已有图结构版本 / 选择计划
    ├─ 失效：冻结声明 → 可选 ValidatePlanInput → 编译发布图计划
    └─ 命中：直接借用不可变图计划，不复制整份计划

读取已发布 snapshot 与 live work
    → 每个必要视图独立剔除；生成紧凑 draw 索引
    → 按唯一数据元组准备常量和绑定
    → 实例化资源槽、补齐屏障与输出路由
    → 可选 ValidateReadyFrame
    → 直接 RHI 录制
    → 原有 GpuSystem 提交、退休、资产引用释放
```

所有共享必须建立在语义相同上。不同相机的可见性与透明深度不同，不合并成一份错误的可见列表；不同 view 的 temporal previous matrix / MotionValid 不共享；不同 shader binding layout 不因为字节数相同就共用参数 set；不同 GPU 物理资源不能因为逻辑槽相同就跳过状态补丁。

## 4. M0：建立可以验收的基线与计量

### 要做什么

改造 `modules/runtime/tests/test_runtime_profile.cpp`，明确传入 performance runtime options，而不是依赖图构造函数的默认诊断参数。把 RG validation、report、GPU markers、报告序列化拆成独立配置，测试日志打印有效值。新增一个 runtime 内的确定性综合 fixture，覆盖当前三个视图、阴影、后处理和 output routing；外部样例只用来集成验证，不要求更改它。

每次采样生成 manifest：commit、dirty 状态、编译器及优化/断言选项、平台与 CPU/GPU/驱动、场景及资产版本、视图与输出配置、窗口尺寸、VSync、flight 数、RG/驱动验证开关、Tracy 版本及导出参数。帧中记录实际 draw 数、唯一 geometry/material/layout/PSO recipe 数；CSV 中准备调用次数不能替代实际 draw 数。

保留默认轻量 frame/phase scope。将高频的 PrepareCommand/PrepareGroup 明细插桩放进专用测量模式，避免几百次到几千次每帧的微 scope 成为基线的一部分。细粒度计数用线程私有计数或由容器长度批量统计，帧末汇总；不在每次 bind 上做原子统计、动态字符串格式化或读取 profiling/validation 开关。

为跨线程关联增加 FrameSerial/FlightIndex，拆出 CPU 更新、CPU 渲染、提交、等待渲染完成、等待 GPU 退休，保留帧到提交延迟与帧退休/呈现节奏。埋点可以加在 runtime 调用边界，不需要修改 GpuSystem 实现。


**v2 额外基线要求：** authoring setter 的渲染相关额外工作、SceneCommit、SnapshotPublish（含 RetainOwners/legacy 观察）、ObjectValueUpdate 都进入测量。记录每类 dirty、唯一 material/program/policy、每 flight 未同步页/字节和模板 rebuild 次数。局部 scope 移到 GT 之前不算节省；PreparationTotal 的旧/新边界见 §6.15。tracked 与 escaped API、未知 proxy 回退分组报告。

### 测量规程

建议每个固定场景先完成资产/PSO/descriptor/page 预热，并至少跑 120 帧；之后测量 1,000 帧，重复 5 轮，旧/新顺序交替。120/1,000/5 是本计划的统一规程，不是 Tracy 固有限制。冷启动、功能切换、窗口 resize、history 重置作为单独测试集，不混入稳态均值。

同时保留三个模式：Release-Light 负责正式性能验收；Release-Full/ASan 负责正确性；Debug-Light 负责复现此次热点与复杂度。Light 与 Full 均不得改变渲染输出语义。所有性能比较保持同一机器、同一优化级别、同一效果质量、同一提交策略，不减少视图、阴影或 draw 来制造收益。

### 验收标准

M0 完成不是“得到一个更低数字”，而是：相同输入可重放；采样绑定到二进制版本；父子 scope 和 prepare/record 同名 pass 可区分；能输出每帧 P50/P95/P99，而不是从当前聚合 CSV 伪造分位数。至少有一次完整 `.tracy` 和 CPU sampling，用于拆解 PrepareCommand 与 RecordRendererList 的 self time/下层调用。

若同一固定基线五轮 P50 的变异系数超过 5%，该场景先标为不稳定，不用一次 3% 的降低判断优化成功。保留轻量插桩与关闭明细插桩的对照；明细采样影响不能算入 runtime 架构收益。[E1]

## 5. M1：先修正确性和多 mesh 扩展性门槛

### 5.1 完成延后 prepare 的数据所有权

`CopyProgramRows()` 复制 rows 与 cbuffer 字节，但 `Declaration` 仍可能只是借用的 string_view；延后 prepare 会读取它。[C10]

最小修复是在 runtime payload/arena 拥有所有绑定名，包括共享资源 rows，并保证字符地址不因容器增长或短字符串移动而失效。长期路径将名字在冷阶段转换成稳定 binding ID/recipe，热阶段只填数值。不得通过要求所有调用方永久持有名字来悄悄缩小当前 API 契约。

验收：局部短/长字符串建立 pass 后立即原位覆盖，再销毁 helper 存储；prepare 和执行结果应与字面量相同。覆盖多个参数、共享图资源参数和 vector 扩容，ASan 无 use-after-free；Off 与 Full 都通过。

### 5.2 关闭校验时彻底移除 geometry 契约去重

当前 `checked` 是线性已见表，关闭校验仍扫描维护；U 个不同 `(buffer, access)` 最坏产生 U(U-1)/2 次比较。[C5]

将“校验是否已见、校验 buffer 契约”整块移入可选边界。Full 可使用稳定 ID 的访问标记，或有界、可测量的去重实现，禁止无界增长的线性扫描。图内生成/写入后读取的 VB/IB 仍必须在图编译前声明，它们的依赖不是可删除的校验。

验收：Off 的 geometry 契约校验次数、为该校验专门维护的去重表写入次数均为 0。Full 的 unique buffer 扩展测试不出现平方级探测增长。

### 5.3 PSO recipe 改用布局语义身份

当前 recipe 匹配 `&Geometry->VertexLayout`，并线性查找 recipe vector。不同对象保存相同布局内容时无法命中这个局部缓存。[C5]

在 runtime 冷路径规范化顶点布局，生成 LayoutId；静态 recipe 至少包含 program/revision、有效 pipeline state 规则（含镜像变体）、layout、topology；最终 native PSO key 还要包含实际 render-pass/attachment 兼容信息。后者未知时不能把半成品假作最终 PSO。不要把 ProgramFrameId 当跨帧身份，也不要在逐 draw 中重新 hash 深层布局。

验收：1,000/2,000/4,000 个不同 geometry 对象，布局值相同，最终 recipe 数不能跟着对象地址数增长；真正改变顶点布局、RT/depth 格式、MSAA、镜像状态时必须正确选择不同 PSO。单独的线性准备段扣除空测试固定成本后，倍增输入的 P50 目标不超过 2.5 倍；排序不混在该线性段中。

**M1 是合并门槛，但不要将它等同于本样例最大的性能机会。当前 PrepareRendererList 仅摊销 0.457 ms；M2 中的 6.427 ms PrepareCommand 主线应尽快开始。**[S1]

## 6. M2：变更驱动的静态绘制编译、快照发布与按视图实例化

### 6.1 设计结论及其适用边界

应把与 view、flight、graph 物理资源无关的 renderer-list 准备，移到 Scene 接收变更并提交的时刻，必要时在 Renderer 注册时完成首次准备。快照应发布“已经完成静态准备的绘制目录与当前对象值”，而不是要求后续从 Material/MeshBatch 再推导完整命令。实际 renderer list 是从这份目录中筛选出的视图相关索引；它本身不是一个可对任意相机永久复用的列表。

**前移只是手段；不变时不做、变化时只做必要部分、同一份输入只准备一次，才是目标。** 不把每帧的全量 PrepareCommand 原样搬进 SceneSnapshotBuild；不在每个 setter 内做 PSO 创建、descriptor 分配或 GPU 上传；不把一份包含当前 view/flight 地址的完整 MeshDrawCommand 跨帧缓存。

Epic 的 mesh drawing 文档也将稳定 draw 的建立与逐帧选择区分开，强调依赖变化需要失效，资源引用不会自动保活。这里只借鉴 retained CPU 描述和明确依赖边界，不引入其 GPUScene、instancing 或 GPU-driven 路径。[E3]

### 6.2 本次源码核实：已经存在的能力与不足

`RenderSceneSnapshotBuilder::Build` 已按 proxy generation / RenderDataRevision 缓存 section 结构，并利用 TransformRevision 复用 bounds；材质也按 generation/revision 复用。末尾调用 `_draws.Sync(next)`，所以“快照前准备稳定 draw”并非从零开始。[C11][C12]

但当前 Sync 每次遍历全部 primitive、batch、material pass，构造 key、查 `_cache`，再复制全部 DrawRecords。其稳定性比较还包含 `ProgramFrameId`，后者是在每次 snapshot 构造中按遍历顺序分配的局部编号。它不应成为跨帧 recipe 身份或失效原因。[C3][C11][C18]

`ForwardLitMeshPassProcessor::PrepareRecord` 仍返回 PrepareCommand，后者即使命中模板也会实例化完整 command，排序三个 group 并复制它们。这部分是本次要移除的下游重复展开。[C1]

`FreezeObjectData` 每次重置整表并为全部 primitive 执行 MakeNormalToWorld，然后写入非 temporal 初值。这类只依赖对象变换的 CPU 计算可以按变更做，而 previous/motion 的按视图补丁仍不能提前固化。[C20][C21]

Scene 当前是 GT proxy 注册表，SnapshotBuilder 由 ForwardPipeline 持有，CpuDrawStore 又在 Builder 内。新方案调整这些现有对象的职责与所有权，不在 Scene 外另造一份语义相同的 World。[C8][C12][C13]

### 6.3 数据按变化频率分层

| 内容 | 默认准备时机 | 快照/后段怎样使用 |
|---|---|---|
| Geometry 语义身份、section/index range、topology、LayoutId、连续 VB 段 | 注册、mesh/section/layout/readiness 改变 | 引用稳定 geometry recipe，不逐 draw 扫布局 |
| Material pass 归属、queue 规则、pass-specific 有效 state、正反面变体 recipe | technique、pass、state、queue 或 policy 改变 | 选预编译 recipe；不再运行完整 PrepareCommand |
| 参数名字到 binding ID、group 顺序、wire 布局、资源读取契约 | program/schema/policy 注册或变化 | 直接按槽填值，不重复字符串反射 |
| 材质数值、纹理/采样器值 | 对应值或 readiness 改变 | 更新共享 material value 记录，不重建所有使用者的 draw |
| LocalToWorld、NormalToWorld、WorldBounds、mirror 标记 | transform/local bounds 改变后合并处理 | 发布当前值；普通变换不重编译几何/材质模板 |
| View culling、LOD 选择（若有）、透明深度、最终可见顺序 | 当前 live view | 生成紧凑可见索引；LOD 候选可预备，最终选择不能固化 |
| previous matrix、MotionValid、camera cut/history 状态 | 对应 view 的当前帧 | 从该 view 已提交的历史派生，不能等同于上次 setter 的值 |
| 实际 PSO、descriptor、上传 offset、backbuffer/graph view | 所需兼容信息/物理资源可用且 flight 可写后 | 按唯一 recipe/值元组实例化；native 录制只读 Ready 数据 |

模板静态不等于物体不能移动。一个每帧移动、但 mesh/material/pipeline 布局不变的对象，其 draw 静态部分仍应一直复用。所谓“快照可直接用”是“不再需要访问 World、解读材质布局或重建稳定命令”，不是“可跳过 view/flight/graph 实例化”。

### 6.4 所有权和模块边界：沿用 Scene、CpuDrawStore、SnapshotBuilder

**Scene 仍是唯一 SceneObjectId 注册和 GT 状态归属。** 将现有 CpuDrawStore 从 pipeline 私有 SnapshotBuilder 中迁为 Scene 所属的 CPU 编译产物。它只存从 proxy/material 派生的必要绘制数据与版本，不反向成为另一套可编辑场景。Scene 不 include ForwardPipeline，不创建 GPU 资源。

**SnapshotBuilder 变为发布器。** 它消费 Scene 的已提交目录，为当前可写 flight 更新拥有自己存储的 RenderSceneSnapshot。既有 per-flight snapshot 是并发隔离手段，不删除它，也不让 RT 读取 GT 可写的 canonical 表。

**Pass 规则仍由 pipeline 所有。** 从现有 MeshPassProcessor 拆出冷阶段的静态 recipe 构造和热阶段的 view/flight 填值。runtime 注册一个小型 `PassPolicyId + Revision + CompileStatic 函数` 即可；函数每个受影响 recipe 调一次，而不是每个 draw 回调。绑定方案、DepthOnly/ForwardLit/ShadowCaster 等有效 state 修正规则留在 Forward 代码内。不要再设计多层通用 policy 继承体系。

**同一 Scene 多 pipeline 的处理。** 活跃 policy 在本帧 Scene 提交截止点之前注册；`RenderPrepareContext`/runtime 准备协调器按 `(Scene, FrameSerial, Flight)` 复用同一已发布 snapshot，Forward 的 flight 保存只读引用，而不是每条 pipeline 重新 snapshot 同一 Scene。pipeline 特有的 object wire/temporal/PSO 实例化仍各自拥有；相同 policy 可共享 recipe，不同 policy 不能因 pass 名相同而混用。第一版可先在当前单 Forward 场景接入，但多消费者不重复建 catalog 是接口约束。

### 6.5 Renderer 变更：setter 标记，提交点合并编译

当前 `StaticMeshComponent::SetMaterial/SetStaticMesh` 调用 MarkRenderStateDirty，后者即时 Destroy/Create proxy。transform 则原地 SetLocalToWorld。这会让同一帧连续材质编辑反复改变注册身份，并阻碍稳定 draw/history 复用。[C14][C15]

建议对内建 StaticMeshComponent/StaticMeshSceneProxy 提供原地更新或 Scene 内同类型替换路径，保留 SceneObjectId；分离 renderer 注册身份、结构版本、对象值版本和 MotionRevision。普通 material 编辑不等同于对象移除重建；真正注销时才释放 slot 并更新 generation。mesh/topology 变化若打断运动连续性，明确重置 motion，而不是用 material 变更无意触发 history reset。[C13][C19][C21]

setter 的快路径只更新 authoring 值、必要的轻量 proxy 输入、OR dirty bits，并且每个 scene slot 每次提交最多加入队列一次。保持现有 property getter 与注册/注销的可观察语义；延后的是派生编译，不是让读取者在 setter 返回后看到错误的 authoring 值。无值变化可直接返回。已有资产替换所需的引用操作保留，但不调用 RHI。重计算在现有 GT frame prepare 的 Scene 提交点统一完成，不在本轮改变 GpuSystem 的可写 flight 等待顺序。

建议 dirty 类别：`Structure`（mesh/range/layout）、`MaterialAssignment`、`TransformOrBounds`、`Filter`（layer/visibility）、`MotionReset`。Material 自身另有 `Values`、`Bindings`、`StateOrTechnique`；program/policy 有自己的 revision。标记数量保持小型位集，不采用一个万物 Dirty 后全部重建的接口。

```cpp
// 设计示意；不是当前仓库已有 API。
void Scene::MarkRenderDirty(SceneObjectId id, DirtyMask bits) {
    auto& slot = Slots[id.Slot];   // GT 的有效注册身份，不是 RT 热路径查找
    slot.Pending |= bits;
    if (!slot.Enqueued) {
        slot.Enqueued = true;
        DirtySlots.push_back(id);
    }
}

bool Scene::CommitRenderChanges(FrameSerial serial) {
    if (serial == LastCommittedSerial) return true;
    ObserveLegacyAndPendingResources(); // §6.8：不可见写入/readiness 的必要兼容
    ApplyChangedMaterialAndProgramRecipes();
    for (SceneObjectId id : DirtySlots) {
        // 有效删除/替换事件按 generation 路由；失效事件不访问已析构 proxy。
        UpdateOnlyChangedInputs(id);
        CompileOnlyAffectedStaticRecipes(id);
        MarkAllFlightCopiesDirty(ChangedRanges(id));
    }
    // 成功提交后才清该批标记；失败不发布半个 epoch。
    return CommitCpuEpoch(serial);
}
```

这段伪码不承诺任意非法输入可不经检查直接索引。身份解析、容量和真实资源错误走正常构造/失败路径；可选诊断仍在 M4 约定的两个边界，不给每个 setter 增加 Full/Off 分支。

同一 frame 的 late mutation 在冻结截止点之后发生，进入下个 epoch；异步通知先进入 GT 输入队列，不能从加载线程改 canonical 表。无渲染消费者时可延后静态编译，dirty 位只保留最终状态，不累计每次 setter 的完整历史。

### 6.6 失效不是只有 Renderer 变更：依赖与传播矩阵

编译阶段实际读取的输入就是缓存依赖。既要避免漏掉 material、program、policy、readiness 变化，也要避免 value-only 修改把数千个 draw 全部变脏。

| 变化 | 更新对象/值数据 | 重建静态 draw/recipe | 重新完整图编译 |
|---|---|---|---|
| 普通平移/旋转/非镜像缩放 | 该 primitive 的矩阵、normal、bounds | 否 | 否 |
| mirror 符号改变 | 当前标记、bounds/normal | 选已有正反面 recipe；缺失时补一个变体 | 通常否 |
| 显隐/layer/剔除策略改变 | filter 数据 | 通常否 | 通常否，除非改变真实资源访问契约 |
| 材质颜色/数值改变 | 共享 material payload | 否，不向使用者广播全命令失效 | 否 |
| 纹理/采样器替换但 layout 不变 | binding value、readiness/owner 集合 | 绑定实例失效；draw/PSO recipe 通常不变 | 仅图依赖改变时 |
| queue、blend/depth state、pass enable 改变 | material state/filter | 相关 pass 选择和有效 state recipe | 仅 pass/资源依赖拓扑改变时 |
| mesh/section/range/layout 改变 | geometry/primitive membership | 相关 section/policy | 仅图内依赖改变时 |
| program/schema/pipeline policy 改变 | recipe 与 binding schema | 使用该版本的相关 recipe | 依赖改变时 |
| 资源 Ready/不可用/重新实例化 | 有效性、物理绑定与所有者 | 按实际改变的几何/绑定层级失效 | 按资源契约判断 |
| 相机或 history 改变 | view/temporal 数据 | 否 | 仅历史初始化等结构变体需要 |
| 删除/复用 Scene slot | membership/generation | 删除旧、建立新记录 | 通常否 |

增加轻量反向依赖表，如 `MaterialId -> 使用的 section/draw ranges`、`ProgramId -> PassRecipeIds`。它们是 CpuDrawStore 内的 ID 列表，不是额外的 Scene 图。value-only 更新只改共享记录，不遍历所有使用者；只有改变静态选择的输入才沿反向关系失效。冷路径 hash 必须配合完整 key 相等判断；pass 名 hash 不是全局无碰撞 ID。

使用稳定 `ProgramId/Revision`、`MaterialGeneration`、`LayoutId`、`PassPolicyId/Revision`；`ProgramFrameId` 和 snapshot material/primitive 的遍历编号只允许作帧内映射。program 的稳定 ID/revision 如当前接口未提供，由 runtime 的注册/重载边界分配，不能编造现有接口已经具备热重载通知。

### 6.7 缓存应停在什么结构上

以下是字段示意；不要求一字段一类，也不强制改变现有命名。

```cpp
struct StaticDrawRecord {
    DrawId Id;                    // 稳定 ID + generation
    SceneObjectId Primitive;
    GeometryId Geometry;
    MaterialId Material;
    PassRecipeId PassRecipe;
    DrawRange Range;               // firstIndex / indexCount / vertexOffset
    ResourceContractId Access;    // 已知只读/图内动态读取等声明身份
    uint64_t StaticSortPrefix;
};

struct PassRecipe {
    ProgramId Program;
    uint64_t ProgramRevision;
    LayoutId VertexLayout;
    PassPolicyId Policy;
    EffectiveStateId NormalState, MirroredState;
    BindingRecipeId Bindings;      // group 顺序、binding IDs、wire 解释
};

struct VisibleDrawRef {
    uint32_t SnapshotDrawSlot;     // 指向本 snapshot，不访问 GT store
    uint32_t ObjectBindingIndex;
    uint64_t SortKey;              // 结合本 view 深度得到
};
```

结构中不包含本帧上传 offset、`RendererList*`、World/Component 指针、`RenderGraph*`、临时字符串、backbuffer 或当前 encoder。snapshot 发布后 RT 只能解引用 snapshot 自己的表或沿原机制有足够生命周期的资源。

同一 shader/layout/policy 的 binding recipe 是共享的，不能按每个 Renderer 复制一套字符串和反射数据。geometry 描述和 material 值也单独共享。PSO 的最后选择是 `静态 recipe + 实际 attachment signature + 必要 pass state`；例如 depth bias 若由本 cascade 动态决定就属于后段输入，不能错误固化。实际签名未知时只保存 recipe。已知且允许的冷期可预热 PSO，但不是每个 setter 的必做工作。

`Forward_ObjectData` 不进入通用 Scene 头文件。通用变换派生值或 pipeline 自己的 CPU wire 行按相关 revision 更新；`FreezeObjectData` 改为消费 changed primitive ranges，而不是每次 Reset 全表。MakeNormalToWorld 的数值/退化行为保持现有实现，只减少调用次数。[C20]

### 6.8 材质裸写接口：事件驱动必须保留一个诚实的兼容边界

当前 `Material::As<T>()`、可写 `NumericBytes()` 和非 const `GetPipelineState()` 返回可长期持有的存储。`GetRevision()` 依靠比较 observed bytes/state 和读取 texture readiness 发现变化。只拦截 SetFloat/SetMaterial 不足以得到完整变更集；在取得指针时 MarkDirty 一次，也无法覆盖之后很多帧通过同一指针写入。[C16][C17]

新快路径增加显式 setter 或作用域编辑事务（如 `EditNumeric()/EditPipelineState()`）。事务结束时提交 dirty 类别，合并到本 epoch；typed setter 值未变不产生工作。事务不得把可写存储带出作用域。它可在一次编辑提交中比较相关字节，但不能让所有干净材质每帧再比较一遍。

**原有外部调用方不强制修改。** legacy 可写入口一旦使用，runtime 将该 Material 标记为“存在逃逸写入”；在每次 Scene 提交点对每个这样的活跃唯一材质最多观察一次。此标记不能在一次干净帧之后清掉，因为旧指针仍可能被写入。只观察一次的结果同时供多个 pipeline/flight 发布使用。

这段比较是维持旧 API 语义所必需的变更发现，不是 validation，Off 下仍运行。日志/counters 明确区分 `TrackedMaterialChanges` 与 `LegacyMaterialsObserved/BytesCompared`。在不改外部调用方的约束下，不能承诺所有材质都自动达到“零轮询”。对于外部旧接口，该边界是有意保留的退化路径，不是假装 dirty 通知已经覆盖所有写入。

资源 readiness 同理：优先在现有 runtime 可观测完成/注册边界标记；没有事件能力时，对 pending 资源或必要的唯一依赖做 GT 检测，不能为了零轮询要求改写资产/GpuSystem。当前 StaticMesh proxy 的 readiness revision 与材料纹理观察必须被覆盖。版本为零、没有可靠通知的自定义 proxy 继续每 epoch 保守刷新，不能错误进入永久静态缓存。[C17][C19][C22]

### 6.9 快照发布：复用 per-flight 存储，只补该 flight 尚未收到的变化

默认采用**现有 per-flight 自有数组 + dirty range/page 发布**，不用新增逐 draw shared_ptr、任意 epoch 的持久数据结构或一套 GPU 退休机制。Scene 的 canonical 编译产物只由 GT 写；每个 flight 的 snapshot 只在原协议允许写入时修改，之后供 RT 只读。

稳定元数据与当前对象值分开存。geometry/recipe/material 的逻辑 ID 稳定；membership 变化用带 generation 的 slot/range 表表达。剔除仍遍历紧凑 active primitive 数据。swap-remove、range 重分配或压缩时，所有被移动的字段与索引页一起标脏；不能只更新最后一次编辑的原位置。

发布元数据可采用每 flight 的 touched-page list + 去重位集。一个 canonical 页变化时，把该页加入所有 flight 的 pending 集合；同一 flight 中已经 pending 的页不重复追加。提交当前可写 flight 时只遍历它的 pending 列表，不扫描整个大表猜测哪里变化。页大小由数据密度和 sparse/dense 测试选择；4 KiB 可以是试验值，不是强制最优常数。

重要例子：三个 flight 轮转，对象在 F0 所属帧改变。发布 F0 后，只清 F0 的 pending；F1/F2 下次变为 writable 时仍需接收该改变，即使那个时候 World 已经没有新的 dirty 事件。跳帧或暂时停用的 flight 不能丢累计变化。每个目标 flight 复制 canonical 的最新完整页，不逐事件回放中间版本。

```cpp
// 设计示意：所有调用在 GT 的既有 writable-flight 边界内。
bool PublishSnapshot(Scene& scene, WritableFlight& flight) {
    auto& pending = scene.PendingPages(flight.Index());
    auto& snapshot = flight.SceneSnapshot();

    // 沿原机制重新获取本帧需要的资产所有者；无静态变化不等于可不保活。
    auto owners = RetainCurrentFrameOwners(scene);
    if (!owners) return false;

    // 可以写的只是当前已安全回收的 flight。失败时不把半更新 snapshot 发布。
    if (!ApplyChangedLayoutAndPages(scene, pending, snapshot))
        return false; // pending 保留，目标标为未发布，下次重试

    snapshot.SceneEpoch = scene.CommittedEpoch();
    snapshot.Valid = true;
    AttachOwnersToExistingFlightRetirement(flight, std::move(owners));
    pending.ClearAfterSuccessfulPublication();
    return true;
}
```

页复制不是对 `MaterialRenderData` 的 vector/string/shared ownership 对象做裸 memcpy。只有 POD 表、offset 表和 arena 字节区可以按字节复制；可变长度 payload 使用已有容器/arena 的所有权正确复制或替换，并在本 flight 内解析 offset。地址不稳定的 vector 元素指针不能作为跨发布存储。结构增长、删除、count/root 元数据与变量区搬移必须在同一发布事务里更新。

兼顾工程复杂度，M2a/b 可以先按 dirty entry 更新原容器；M2c 再加成批范围复制与 touched-page 索引。最终验收要求避免静态全表反复重建，但不要求一步引入新的通用分页 allocator。

### 6.10 资产保活和 CPU 数据版本严格分开

CPU catalog 不新增长期 `StreamingAssetRef`；proxy/material 现有 authoring 所有权不改。发布每个 flight 时，继续在 GT 收集该帧引用的资产 owner，GPU 安全退休后依照原路径释放。即使 snapshot 页没有变化，也不能跳过这次帧保活。可以按唯一 owner 去重，但不是把帧引用延长成缓存引用。[C11][C16]

旧 F0 的 material bytes、geometry 地址、recipe 和 descriptor 不能在准备 F1 时原地覆盖。per-flight 自有 CPU 表确保前者；GPU payload/descriptor/上传页仍需要原 flight 机制确保后者。CPU 编译表的旧版本不必设计成无限存活：旧 frame 已拥有自己的已发布表；只有它引用的资源必须仍有效。

注册删除时先移除 catalog/dependency 索引，不允许后续事件通过旧 proxy 指针找数据。资产重新实例化要更新 native 绑定并固定本帧 owner。如果外部资产实现允许原地销毁已被 flight 引用的 payload，单靠缓存版本不能修复它；本轮继续要求现有不可变发布/在途保活契约，不发明额外的资产系统保证。

### 6.11 RendererList 变为视图索引，而非命令工厂

图阶段先确定 live work；随后对需要的 view 独立剔除，从 snapshot 的 pass 分类/range 中发出 `VisibleDrawRef`，补本 view 排序键。稳定 metadata 的解读不再次发生。Opaque 可按有效 state 与深度组织；Transparent 保持本 view 的背到前顺序。

每个相同语义的 material/object/view 数据元组最多准备一次。沿用原 M2 的复用粒度：非 temporal object 可共享同帧兼容数据；temporal 数据包含 ViewStateId/history epoch；dynamic set 包含 layout/group/backing page/descriptor shape。第一版仍允许每个 flight 每帧上传一次必要对象常量，不要求跨帧永久映射和不上传；本改动首先解决 CPU 重编译/重打包与同帧重复上传。

`PrepareRendererList` 变成有限的 native binding/PSO 解析或融合进 FrameDrawResources 的批量准备。它不再通过 `DrawRecord -> MeshBatch -> MaterialPass -> PrepareCommand` 重建完整 command。Record 直接遍历已选引用和 Ready binding 表，仍调用现有 RHI 的逐 draw 接口。

### 6.12 与 RG 分离计划的关系：两种静态计划互补

Scene draw catalog 回答“这个对象在某个 pass policy 下如何绘制”；CompiledFramePlan 回答“本帧需要哪些工作、资源和执行关系”。两者不合成一个巨大的场景图缓存。

scene epoch 的数值变化不能自动使 graph plan 失效；graph 编译也不能逐 draw 重建资源发现。已缓存 geometry/material 的读取契约预先保存，图内写后读仍必须显式入图。graph topology/attachment signature 真正变化时，选择正确图计划并刷新必要 PSO/binding 实例，而不是重编译整个 scene catalog。

“裁掉的 pass 不做完整准备”仍成立，但要明确区分：注册/变更时可构造它的轻量、view-independent CPU recipe；没有 live 消费者时，不进行该 pass 专用的本帧剔除、完整动态列表、GPU 描述符、上传或 native PSO 实例化。只为当前活跃的有限 policy 集准备，不穷举未来所有管线/功能排列。

对于真正 view-dependent 的动态几何/参数，保留直接动态路径；view-independent 但每帧变的部分至多每 epoch 做一次，views 共享结果。100% 高频变化场景采用同一 typed batch 更新/发布路径，避免昂贵 hash、逐事件对象分配和重复回放；不能强迫它经过多层静态缓存才能绘制。

### 6.13 默认不采用的方案及原因

不在任意 setter 里立即完整准备：同帧十次编辑只需要最终版本；RHI 调用时机/资源有效性也未必满足。不把所有资源和所有参数都早绑定：view、temporal、attachment signature 和 flight offsets 未确定。不引用 GT canonical 数组供 RT 直接读：下一帧会修改它。不使用无限 epoch/COW 链保活整个 Scene：已有固定 flight 快照足够，先用 dirty 范围发布。

不为每个 Renderer 复制一份所有 pass 的庞大 native command。不通过每帧全量 hash/比较再声明“事件驱动”，除非属于明确列出的 legacy/未知 proxy 兼容路径。不把临时 ProgramFrameId 或裸地址当稳定身份。不根据 dirty 数为零就跳过 temporal、asset owner 或 GPU 同步处理。

### 6.14 M2a–M2d 提交范围和依赖

| 子阶段 | 主要文件/职责 | 合并门槛 |
|---|---|---|
| M2a 变更来源与稳定身份 | `components/primitive_component.*`、`static_mesh_component.*`、`render_framework/scene.*`、`primitive/static_mesh_scene_proxy.*`、`material.*` | setter 合并；material-only 不换 SceneId；escape 写入/readiness/自定义 proxy 不漏变化 |
| M2b 静态编译与值分离 | `cpu_draw_record.h`、`cpu_draw_store.cpp`、`mesh_pass_processor.*`、Forward lit/depth processors、binding caches | 稳定模板按变化构造；value-only 不放大为全 draw 失效；ProgramFrameId 不进 key |
| M2c 快照增量发布 | `render_scene_snapshot.*`、runtime prepare context/coordinator、`forward_pipeline.cpp`、`forward_frame.cpp` | 所有 flight 接收各自未同步变更；静态页不全拷；owner/失败事务正确 |
| M2d 索引式列表与 Ready 绑定 | `renderer_list.*`、`frame_draw_resources.*`、`mesh_draw_command.*`、`forward_effects.*` | 后段不再调用完整 PrepareCommand；每语义元组一次；状态/排序/temporal 与参考一致 |

表中都是现有文件族上的实施范围；新 ID/接口/测试名均是建议，不能当成已存在符号。M2a/b 不等待完整 M3；M2d 可先用当前 Compile 的 live 结果运行，M3 随后提供零重建图计划。M1 的所有权与复杂度修复仍先过关。最终不长期保留两套互相转换的生产 draw 系统。

### 6.15 性能计量：禁止只降低 RenderSystem scope

新增低频 scope：`Scene.CommitChanges`、`Scene.CompileStaticDraws`、`Scene.PublishSnapshot`（其中区分 Copy/RetainOwners）、`Scene.ObserveLegacy`、`ObjectValueUpdate`，与 `DrawWorkBuild`、GraphMaintenance、Record 相接。显式计算同一逻辑帧的准备总账：

```text
PreparationTotal = authoring/setter 的渲染相关额外工作
                 + SceneCommit + SnapshotPublish（含保活与兼容发现）
                 + ObjectValueUpdate + DrawWorkBuild
                 + 独立 worker 上尚未计入的上述工作
```

相加的是互不包含的 CPU 工作区间/采样 CPU 时间；不能把父子墙钟 scope 和线程等待加进来充当总 CPU 时间。另用 frame serial 时间线测关键路径和 input-to-submit 延迟。旧基线也覆盖原 SnapshotBuilder/FreezeObjectData/PrepareCommand，不能只为新路径加前段成本而拿旧局部 Render 比较。

工作量模型用 `N` 表示 scene primitives，`K_s` 表示本次提交真正改变的静态记录，`K_v` 表示变化的值，`P_f` 表示目标 flight 尚未收到的页，`D_v` 表示各 live view 的可见 draws：

```text
稳定编译/发布成本 ≈ O(K_s + K_v + 受影响依赖 + P_f × 页载荷)
仍需的视图工作   ≈ O(各 view 剔除范围 + 各 view 排序/录制的 D_v)
兼容/保活工作    = 逃逸材质观察 + pending readiness + 本帧 owner 获取
```

这不是宣称整个渲染或整个 snapshot 都能降到 O(变化量)。相机剔除、逐 draw native 录制、flight 保活，以及没有事件的 legacy 接口都仍有成本。

### 6.16 M2 验收硬指标

**稳态：** 活跃 policy/material/layout/geometry 固定且全部 flight 预热后，仅移动相机连续 1,000 帧。静态 recipe/command 模板重建为 0；对象 normal/bounds 重计算为 0；静态 catalog 全表重建/深比较/复制为 0；所有 flight 收到同一版本后静态页复制为 0。view/temporal 常量、排序、保活与必要 GPU 上传允许非零并单独计数。

**局部变化：** 一个 Renderer 在提交前连续写十次，只处理最终值一次；普通 transform 只更新自己的值/派生值，不重编译模板。共享材质被 10,000 Renderer 使用时，纯 numeric 修改只更新该唯一 material value 记录，不触发 10,000 次 draw 重建。静态 state/technique 改变允许处理相关依赖，但不能遍历无关 Scene。

**发布：** 三 flight，至少一个跳过多个 epoch，再次变为 writable 时拿到最新完整数据；变化不能在第一个 flight 发布后从其他 flight 丢失。对象删除、slot 重用、payload range 搬移、材质/geometry 替换、失败重试时，旧在途 snapshot 与新 snapshot 各自一致。

**后段：** 稳定 record 的 renderer list 路径不再调用原完整 PrepareCommand/instantiate；每语义元组最多准备一次；DrawIndexed 数量和各 draw 的有效状态/排序保持。预热后无逐 draw group 排序、字符串解析、深 layout 比较和堆分配。

**回退与成本：** 对 0%、1%、10%、100% transform/material/structure 修改分别采样；将 repeated-mesh 与 unique-mesh、tracked API 与 legacy escaped API、static 与 view-dependent dynamic 分开。变化路径的工作量不乘 view 数；100% dirty 时不得变成平方级，也不得遗漏变更以达标。

保留 v1 的 DrawWorkBuild P50 ≤50%、P95 ≤65% 基线作为后段目标，但新增不可省略的总账门槛：同等静态/低变化多视图 fixture 的 PreparationTotal 建议 P50 ≤70%、P95 ≤80% 对应旧完整准备基线；GT 渲染准备关键区间的 P95 不应比旧值高超过 10%，超限需给出端到端改善证据并调整方案，不能仅报 RT 下降。所有百分比均为待实测目标，不是当前 CSV 能保证的加速。

如果原有模板和常量复用已使这些目标的主要收益被其他阶段获得，仍以相同完整边界的联合对照验收，不机械累计各阶段百分比。非目标/dynamic 场景遵守原 M5 的 5%/10% 回归保护线；无法满足时先定位实际成本，不把热路径推给未测线程。

### 6.17 Full 模式怎样核实缓存而不污染 Off

仍只有 M4 两个可选诊断边界。缓存依赖和 dirty 消费属于运行逻辑，不受 validation 控制；不会因为 Off 而停止检测必要变化。静态编译/发布使用受控的 typed 数据和安全容量/资源错误处理，不能先越界再指望事后检查。

Full 的测试模式可在 `ValidateReadyFrame` 做“完全从输入重建的只读参考结果 vs 增量 snapshot/Ready”的差分，包括可见 draw 状态、material values、normal/bounds、temporal 和资源访问；图合法性仍在 ValidatePlanInput。差分参考只在测试/Full 中构造，不保留第二套生产 pipeline，也不在每个 Renderer/bind 加校验开关。

## 7. M3：完整执行计划复用，并由存活工作驱动准备

### 7.1 先确定可复用单元，而不是继续扩大旧 hash cache

现有 cache 命中发生在 BuildIR 之后，且通过值赋值复制 CompiledGraph；PlanStorage、OptimizeRaster、BuildExecutionPlan 仍会执行。[C6] 新的 `CompiledFramePlan` 应当有稳定身份，命中时借用它，不再通过复制大容器获得一个“本帧计划”。

建议首先实现单一 CPU 录制队列/现有 command buffer 组织的计划，不扩展 async compute、多队列调度或 RHI heap aliasing。复用当前 RHI 可表达的资源复用与 barrier 语义，不为新的 memory alias 优化去修改后端。

计划应保存：最终 live pass/work 序列；逻辑资源到物理槽的映射及兼容描述；生命周期/复用安排；raster group、load/store/clear 结构；内部资源状态转换模板；外部资源首用/末用补丁槽；记录批次和 presentation 输出路由；参数/binding recipe 布局。每帧数据只填真实资源、constants、draw ranges、操作票据与状态补丁。

### 7.2 结构 key 和数据 key 分开

第一版采用显式结构版本和紧凑 key，不设计通用的自动变更追踪框架。下表是必须覆盖的失效规则；不要仅用 PassCount 或 feature mask 作 key。

| 输入变化 | 是否应使核心图结构失效 | 处理 |
|---|---|---|
| 相机矩阵、jitter、普通物体变换 | 否 | 更新帧数据、剔除和必要 temporal 数据 |
| draw 数量、可见性、排序变化 | 通常否 | 更新索引；前提是资源访问契约未改变 |
| 同一外部逻辑资源槽更换 native 地址 | 通常否 | 按本帧身份解析，descriptor/barrier 跟着更新 |
| 材质数值/只读纹理值变化 | 不应自动触发全图编译 | 局部 binding 更新；若改变图内资源依赖则另行失效 |
| view 数量/结构、输出路由变化 | 是，或选择已缓存变体 | 不跨不同组合误复用 |
| HDR/TAA/AO/Bloom/阴影 cascade 等使 pass 或访问集合变化 | 是，或选择变体 | 状态切换与 warm steady-state 分开计量 |
| resource dimensions、mip/array/aspect、range partition、format、sample count、usage 变化 | 第一版保守失效相关整体计划 | 不能仅修改地址后沿用不兼容资源复用或 barrier |
| shader/binding schema 更新 | 绑定 recipe 必须失效；图依赖变了则计划失效 | 旧 in-flight 版本保持有效至退休 |
| history 有效性改变了某个消费者的结构 | 选择历史初始化/正常状态变体 | 不能采样未初始化资源 |
| side effect、readback、upload、capture 或 external access 契约改变 | 是，或单独动态 fragment | 不为 cache hit 隐藏副作用 |

仅当同一结构 key 的全部输入不变量成立时复用。减少一次 hash 比较不值得换取一个错误计划。查询成本应与紧凑结构元数据有关，不与逐 draw 数量有关。

使用有界的小型变体缓存，至少容纳测试中的 A/B 两个常用结构，不因 flight 轮转或每帧切换在两个模式间反复编译。缓存淘汰不可销毁仍被当前 flight 使用的计划；通过原 flight 生命周期保留必要的不可变计划版本即可，不另造 GPU fence 体系。

### 7.3 命中计划也要实例化，但只做本帧必需工作

内部 barrier 的资源槽、前后状态和相对位置预编译；每帧填 native 资源引用。导入资源的初始状态、实际物理资源重用状态和 history 状态必须根据本帧输入补齐。GPU 最终状态只能沿现有成功提交路径提交，不能在仅录制成功或准备失败时提前更新外部状态。

RenderGraph 当前在 Record 中遍历 pass 的访问，再逐 presentation target 查找目的 command buffer。新计划把输出路由编码为 role/slot，实例化阶段填 command buffer，Record 不再扫描整个访问集合寻找目标。[C7]

尺寸、格式或资源池分配结果不满足编译时兼容条件时必须重做相关布局/计划。不能把“缓存整个执行计划”解释成缓存上一帧的 framebuffer、descriptor 内容、backbuffer 指针和 initial state 然后无限复用。

### 7.4 存活性真正控制完整绘制准备

M2 完成以后，Forward pass 构图应引用轻量 WorkRequest、已发布静态 draw 目录及资源依赖描述，不再为了 AddRasterPass 首先生成 RendererList。变更时允许准备有限、与 view 无关的 CPU recipe；本节“无用分支零准备”指本帧完整 view/flight/native 准备，不把必要的静态 recipe 编译误判成违反裁剪原则。由计划的 live work 集合启动可见性与完整 draw work 准备。

多个存活 pass 共用同一个视图剔除任务时，只运行一次；一个 pass 被裁掉但另一个仍需要相同 work，不能一起删除。图内动态 VB/IB、copy/compute 生产者与 consumer 的访问必须在编译前声明；若资源依赖确实取决于本帧可见集合，只提前执行最小依赖发现，不提前完整组装 draw command。

对于满足“上传已经就绪、调度范围内无写入、正确只读状态、生命周期覆盖整帧”的外部 mesh/material 资源，使用显式只读资源契约或共享资源集合，避免逐 draw 重建资源访问。新出现的上传或图内写入不能被这个 fast path 吞掉。

对 draw count 为零的 pass 保持拓扑稳定，必要 clear/load/store/side effect 仍执行。不要因为每帧可见数改变就在 empty/nonempty 间重编译整图。需要按空工作动态跳过纯 draw 部分时，让执行数据表达零范围，而不是取消有效输出。

### 7.5 外部现有 API 与动态图的处理

`modules/imgui` 等外部模块的 AddPass 接口不作为本轮修改对象。runtime 内保留通用声明入口，并把它提交的工作整理成动态 fragment。Forward 的稳定主体拥有显式计划；外部 fragment 按其实际资源/side-effect 结构参与组合。

只要 fragment 结构稳定，复用已编译的组合计划；payload、顶点/索引数据和真实资源地址属于帧实例。fragment 结构变化时走明确的组合重编译或动态慢路径，并记录原因，不允许静默忽略依赖。第一版不必实现复杂的局部图增量编译。

因此，“稳定主体 Declare/Normalize/BuildIR 为零”与“任意外部调用方完全没有每帧声明成本”是两个不同目标。后者在现有通用 API 下不能无条件保证。外部 fragment 的适配费用必须计入 GraphMaintenance，不可为好看的数据排除。完整综合场景的 stable-frame 计划复用必须实际验证，不能只展示一个没有 ImGui/输出路由的微型测试。

### 验收标准

预热后，固定结构的 runtime fixture 连续 1,000 帧：完整声明、归一化、IR 重建、拓扑/存活编译、PlanStorage、OptimizeRaster、BuildExecutionPlan 的重跑次数为 0；允许 per-frame 资源实例化、必要 binding 补丁和外部初始状态补丁。计划命中不复制整份计划；只更新矩阵和 draw 数不重建。

A/B 结构交替测试在两个变体预热后不持续 miss。变更 RT 格式/MSAA/mip/range/输出路由/依赖时，必须选择正确变体或重编译。强制重编译与计划复用两条路径的可观察输出、资源访问顺序、屏障语义、readback/present 结果一致。

被裁剪分支完整 DrawWorkBuild、descriptor 准备、上传和 record 次数均为 0；必要共享 work 和 clear pass 不被误删。动态上传、readback、capture 和窗口恢复覆盖真实组合路径，不能让旧指针混入新 flight。

性能目标：新定义的 `GraphMaintenance = 声明/fragment 适配 + 计划选择/编译 + 资源实现 + barrier/路由补丁`，不含 draw work 和实际 RHI record。稳态 P50 ≤ 同口径基线的 30%，P95 ≤ 50%。这里不拿整个 ComposeGraph 作为“纯图基线”，也不靠把 DrawWorkBuild 换名字塞到另一个线程获得达标。

## 8. M4：将录制输入收敛到直接 RHI 热循环

### 8.1 先测剩余成本，再决定批次粒度

本次 RecordRendererList 摊销 3.714 ms，包含在 4.758 ms 的 RG Record 中；没有更低层计时，无法把这些时间全部归因于 runtime。需要在 runtime test 内构造使用同样 PSO、set、VB/IB、draw 参数的手写 RHI 录制对照，保持输出工作量、命令缓冲组织和热身条件一致。[S1][C5]

手写对照不绕过 RHI、不调用原生 D3D12，也不实现 bundle。它用来估计“现有 RHI 下这批工作需要的成本”，而不是声称得到了可移植的硬件理论下限。

### 8.2 要做的重构

让录制消费内部封装的 Ready 数据。保留唯一的一份稳定 draw 记录与必要的本帧索引/绑定视图，删除 `MeshDrawCommand → PreparedDraw → 另一份 packet → 通用 opcode` 这类多次展开，不新造命令虚拟机。

稳定 group 的顺序、动态 binding 槽、连续 vertex buffer 的绑定范围在 recipe 或 geometry 冷路径计算。按合法的状态 run 绑定 PSO、view/pass/material 组；逐 draw 主要更新对象动态偏移与 DrawIndexed。若在运行时仍需要状态比较，应是小型 ID/指针/offset 比较，不再反射参数、排序 groups 或查询图结构。

状态跳过必须遵守现有 RHI 契约。不能假定 PSO/layout 改变永远不会影响后续绑定；只有测试证明相同绑定有效时才跳过。不同 pass、外部回调、自定义命令对 encoder 的状态影响要设置明确的 cache reset 边界。透明绘制不按 material 重新排序；raster 合并也不能跨不兼容的 attachment/load/store/clear/viewport 语义。

每 pass 一次函数调用/回调可以保留。RHI 现有 virtual/API 调用也可以保留，不为了消掉一层调用去修改后端。优先删除逐 draw 容器组装、merge 元数据解释和无必要参数绑定。

### 8.3 校验只在两个边界可选启用

两个逻辑入口是：**冻结图声明后、编译前的 ValidatePlanInput；本帧资源和绘制准备完成后、Record 前的 ValidateReadyFrame。**前者对新/失效结构检查输入契约；从 Off 切到 Full 时，可在同一图边界一次性补建诊断描述或验证已有描述，不增加热循环入口。

所有资产/geometry/binding/ready range 的可选契约检查都归入这两处之一。运行时模式在帧开始时快照，整段开关，不在每次 draw/bind 读取 validation。已有通用 AddPass 输入在边界归一化和验证；内部 Ready 结构不再任由任意调用方伪造后交给内建 fast recorder。

这里不是取消真实错误处理。分配失败、设备丢失、encoder 创建失败、提交失败，以及实际完成图算法所必需的依赖/循环/资源生产者语义，仍需处理。Full 主要增加诊断契约，不改变 barrier、culling、upload 或提交行为。编译器的非法输入测试要保证 Full 在危险索引解引用之前报告问题，不是先越界再在计划发布处检查。

### 验收标准

预热的 draw/record 内循环：0 堆分配、0 字符串解析、0 图 handle 解析、0 layout 深比较、0 validation 分支；必要的每 draw offset 和 RHI 调用保留。PSO/material/view 绑定次数与有效状态变化相关，而非每 draw 无条件全绑定。实际 DrawIndexed 次数与参考工作一致，不能把减少 draw 数当作本轮优化。

runtime-only fake encoder 记录语义轨迹，核对每个 draw 生效的 PSO、参数、VB/IB、offset、索引范围、viewport/scissor、attachment 与 barrier 顺序；仅允许消除等价冗余绑定。另在真实 D3D12、Metal 上做对应的基础正确性回归，不用 fake 轨迹代替 GPU 验证。

性能目标：对至少 1,000 draw 的稳定代表场景，Release 下新 Record P50 不超过同配置手写 RHI 对照的 115%，并且 P95 无明确退化。若原路径已经接近此对照，不再通过新增复杂层次追逐微小收益。若仍相差很大，则用 sampling 区分 runtime 数组/绑定解释与 RHI/驱动成本，不据此修改 GpuSystem 或 RHI。

## 9. M5：整体验收、flight 资源与容量稳定性

生命周期规则从 M2 开始实现，M5 是整体验证，不是最后才补保活。默认使用固定 flight 自有 CPU 存储与待发布 dirty 范围，旧 snapshot 不指向 GT 可写 canonical 表。每个 flight 独立接收累计变更；发布失败不清 pending；零静态 dirty 帧仍沿原途径重新保活资产。稳定 CPU 记录、计划与材质 recipe 的旧版本，必须能够被仍在使用它的 frame/flight 安全读取；不得原地覆盖 render thread 还在消费的数据。CPU 记录不需要一直活到 GPU 完成，但它引用的 GPU 资源、descriptor 和上传页必须沿现有机制活到安全退休。

三 flight 测试中反复切换材质、删除并复用 primitive 索引、替换 geometry、热重载 shader、销毁并恢复 output；把 mock GPU completion 故意延迟。每一个 in-flight frame 必须看到自己对应的数据版本。资产帧引用仍在原来的 game thread/retire 边界释放；计划与 recipe cache 不能额外长期锁住资产。

使用现有 flight arena、resource pool 和上传页增长机制，高水位预热后只重置游标/长度。新热数组必须复用容量，结构稳定场景下 runtime draw/graph 临时内存分配归零；RHI 内部或真实新资源需求带来的分配单独统计，不设一个无法在 runtime 内兑现的“全进程 malloc=0”。

至少运行 10,000 帧的稳定与 A/B 切换压力测试，观察计划数、ready cache 容量、parameter set/page 数和保留资产数；这些量在有界工作集下应收敛，而不是随帧数增长。缓存失效和容量溢出允许慢路径，但必须可见且无 use-after-free、错帧或状态提前提交。

### 并行化只作为有证据的后续分支

M2–M4 的默认交付是单 render-thread 直接录制的最低冗余路径。只有新 Release profiling 仍显示可并行的 CPU prepare/cull 占主导，且已有任务能力能在 runtime 内安全提供分块执行时，才做 job 化。需要独立 scratch、只读 snapshot、明确的 PSO/descriptor 准备线程安全边界；不能直接把带共享 `_bindingScratch` / cache / arena 的 PrepareGroup 丢进 parallel_for。[C2]

无新增 RHI/GpuSystem 支持时，不承诺 parallel native recording。D3D12 命令 allocator 的并发和复用有约束，不能把 CPU 图依赖转换成对同一个 allocator 的并发调用。[E2]

任何并行版本都同时比较关键路径 wall time 与各 worker CPU 工作量，不能只把 RenderSystem 的等待点移动到外面；不能增加 frames in flight、延迟提交或漏计 worker 时间换取更低的局部 scope。

## 10. 最终性能门槛与结果判定

### 10.1 三层验收

第一层是正确性：所有回归场景通过，资源访问与提交语义保持，Off/Full 输出一致，无新增 RHI/GpuSystem 变更。

第二层是结构性硬指标：所有线程上的稳定模板重建为零；所有 flight 已同步后 camera-only 帧静态目录发布字节和对象 normal/bounds 重算为零；每语义元组只准备一次；稳态核心图完整重建为零；被裁剪分支完整工作为零；record 热循环无分配/字符串/图查询/validation 开关。名称或计时区移动不能满足这些指标。

第三层是 Release 耗时：使用 M0 建立的同口径基线，报告新旧五轮的 P50/P95/P99、总 CPU 工作和实际 draw/bind/上传字节计数。只有同场景、同输出质量、同提交策略的数据可用于结论。

| 项目 | 建议性能达标线 | 适用条件 |
|---|---|---|
| M2 DrawWorkBuild | P50 ≤ 50% 基线；P95 ≤ 65% | 后段同口径；不得作为单独成功证据 |
| M2 PreparationTotal（v2 新增） | P50 ≤ 70%；P95 ≤ 80% 旧完整准备基线 | 含 GT setter-extra/Commit/Publish/Retain、对象值、RT 准备及 worker；静态/低变化 |
| GT 准备保护（v2 新增） | P95 不增超过 10%，否则须复审端到端证据 | 不通过把重工作全部前移达标 |
| M3 GraphMaintenance | 稳态 P50 ≤ 30%；P95 ≤ 50% | 同样的完整 graph/fragment 功能，不含 draw/record |
| M4 Record | P50 ≤ 手写同等 RHI 对照的 115% | ≥1,000 draw；相同有效调用与 command buffer 布局 |
| 综合 runtime CPU 渲染 | 建议 P50 ≤ 70%；P95 ≤ 80% 基线 | 固定当前 Tidal 等价配置与全部效果，Release-Light |
| 非目标场景保护 | P50 不超过基线 105%；P95 不超过 110% | 重复测试噪声已经受控；超限要解释并修复 |

30% 综合降低是建议达标目标，不是从当前 Debug 数字推导出的保证。若手写 RHI 对照证明不可变的 RHI/API 成本已经高到使该目标不可能在本轮约束下达到，必须提交实测下限和成本分解，重新定量协商这一总目标；不能用旧 Debug/Release 比例估算，也不能降低画面质量或改动不允许的系统来达标。

局部性能比对不加父子 scope 均值，也不将不同线程的墙钟时长相加当作端到端延迟。必要时在固定开始/结束点新增“整段工作完成”的 measurement，保证把被移走或并行执行的工作计入。

### 10.2 必须提交的成果

每个里程碑提交源码 diff、针对性单元/压力测试、Release capture manifest、每帧统计和关键 counters。最终提交 runtime 范围审计结果、旧新图与录制语义对照、完整场景新旧测量、cold/dynamic/steady 分开统计，以及已知不支持的动态 fragment 快路径清单。

不能以“源码看起来可缓存”“CompilePlanHit 很高”“ComposeGraph 更小”“帧率显示更高”单独验收。改变输出数量、关闭阴影/后处理、减少 draws、增加 flight 深度、隐藏等待或把活跃工作挪到未测线程，均不计作完成本计划。


## 11. 可直接拆成测试任务的验收矩阵

以下 60 项均是待实施测试，未执行。T01–T33 保留原 ID 并扩充 v2 边界；T34–T60 新增。表与同版本 JSON 由同一测试条目生成。所有 C++/GPU/性能通过状态须由实际执行填写，文档生成不代表验收通过。


### 原有验收 T01–T33（已整合 v2 要求）

| ID / 阶段 / 用例 | 输入与操作 | 明确通过条件 |
|---|---|---|
| T01 · M0 · 二进制与配置可追溯 | 固定 runtime 综合 fixture；Release-Light。<br>生成 manifest 和每帧采样。 | 包含 commit/dirty/优化选项/资产与场景版本/有效 RG 与驱动诊断值/flight/VSync/Tracy 导出配置；不能只凭文件名判断。；v2 另记录 tracked/escaped 材质数、各类 dirty 数、实际发布页/字节和各 flight epoch。 |
| T02 · M0 · 聚合口径与重复名字 | 同一 pass 有 prepare 和 record，存在嵌套 zone。<br>输出带 source line 的统计，并对照时间线。 | 两个阶段不合并；inclusive 与 self 显式标注；不把父子总时间相加。 |
| T03 · M0 · 稳态与冷路径分离 | 预热全部 flight 与必要 pipeline；另有首次加载/resize。<br>分别采集五轮稳态和冷路径事件。 | 稳态逐帧分位数可复算；冷路径保留而不冒充稳态均值；对超过 5% 的 P50 轮间变异先标不稳定。 |
| T04 · M1 · 绑定名字生命周期 | 使用局部短/长 string、共享资源 rows 和局部 cbuffer。<br>BuildGraph 后覆盖名字、扩容容器、销毁 helper，再 prepare/record。 | 绑定结果不变；Off/Full/ASan 均无悬空读，所有可延后使用的名字由正确生命周期拥有。 |
| T05 · M1 · Off 零 geometry 校验工作 | N 个独立 VB/IB；合法只读资源契约。<br>N=1k/2k/4k，分别 Off 和 Full。 | Off 专用校验调用/去重表维护为零；Full 不出现 U(U-1)/2 线性已见表比较。 |
| T06 · M1 · 相同布局不同地址 | 不同 DrawData，各有值相同的 VertexLayout；共享 VB/IB 排除 T05。<br>增大 geometry 对象数量。 | 有效 recipe 数不随布局对象地址增长；pipeline 解析按唯一有效 recipe 计费。 |
| T07 · M1 · 真实 PSO 变化 | 相同几何、material，依次修改 layout、RT/depth 格式、MSAA、镜像。<br>选择并录制各变体。 | 不同有效状态获得正确 PSO；普通变换不重建稳定模板；镜像改变选择正确正反面变体。 |
| T08 · M2 · 静态命令不重建 | 稳定场景、program/material/geometry，全部 flight 预热。<br>运行 1,000 帧，可见性保持。 | 完整静态 command/group 模板构造为零；允许 DrawId/排序索引写入与必要常量更新。；v2 覆盖 GT 静态编译与所有 flight：仅移动相机不能以把重建挪到 snapshot 前来达标。 |
| T09 · M2 · 多视图非 temporal 对象复用 | 相同非 temporal object wire layout，1/3/6 个视图，多 pass。<br>改变视图数，记录对象复制字节和绑定元组。 | 每 flight 每帧每唯一对象数据元组最多准备一次，不按 view/pass 重复复制；各 view culling 仍独立正确。 |
| T10 · M2 · Temporal 隔离 | 两个 camera 不同 history、previous matrix、MotionValid。<br>移动对象，camera cut，关闭后重开 TAA。 | 不同 view 的必要 temporal 数据不错误共享；history epoch 改变按语义更新，没有旧视图运动数据。 |
| T11 · M2 · 材质布局兼容 | 相同材质值，兼容/不兼容 binding layout、不同纹理子 view。<br>比较材质准备和 set 使用。 | 仅兼容元组共享；不以相同字节数或相同 group 数判断兼容；修改一材质只失效相关元组。；纯 numeric 值更新不重建共享它的全部 draw/pipeline recipes。 |
| T12 · M2 · 删除与 ID 复用 | 多个稳定 record，包含同索引不同 generation 的对象。<br>删除、复用索引、替换 geometry、修改 queue/pass mask。 | 旧缓存不误命中；只有相关模板失效；DrawId 与 generation 在引用阶段一致。；material-only 原地更新应保留 Renderer 的注册身份与运动连续性。 |
| T13 · M2 · 透明排序与不透明状态 | 同一批透明物体由两个反向相机观察；另有 opaque。<br>生成并录制列表，比较参考输出语义。 | 透明顺序按各 view 深度；不透明合法优化不改变材质/几何/可观察结果；不为 state locality 改透明语义。 |
| T14 · M2 · 上传页 rollover | 大量唯一 object 元组，跨多个 backing page，至少两个布局。<br>准备 bindings，再录制。 | 页更换时 descriptor 正确切换；dynamic offset、range 和对齐正确；每页对应的 set 不错配。 |
| T15 · M2 · 线性数据路径 | 重复实例、独立 geometry、独立 material 三个单因素场景。<br>1k/2k/4k；CPU/fake 测试可增至 100k；剔除 sort 和固定成本。 | 线性准备段倍增输入的 P50 目标 ≤2.5 倍；没有逐 draw 深 hash、group 排序和堆分配；总体 DrawWorkBuild 达到阶段目标。；v2 再测 K/N=0/1/10/100% 的事件、编译、发布成本，不能只测 RT 后段。 |
| T16 · M3 · 稳定计划零重建 | 固定 runtime 完整配置与资源形状，history 预热。<br>1,000 帧仅变矩阵、jitter、可见索引和普通参数。 | 核心 Declare/Normalize/BuildIR/Compile/PlanStorage/OptimizeRaster/BuildExecutionPlan 重跑为零；不复制整个 CompiledFramePlan。 |
| T17 · M3 · A/B 变体缓存 | 两种有限结构组合已预热；至少三个 flight。<br>每帧交替 A/B。 | 两个计划可复用，不因 flight 轮转持续 miss；淘汰不破坏 still-in-flight 版本。 |
| T18 · M3 · 结构失效矩阵 | 可切换 format/MSAA/mip/layer/aspect/range/feature/view 数/输出路由。<br>每次只改变一个真正影响计划的变量。 | 正确选择变体或重编译；不沿用不兼容生命周期、barrier 或 raster group。 |
| T19 · M3 · 外部地址及初始状态补丁 | 同一逻辑槽绑定不同 backbuffer/pool 资源；改变初始状态。<br>运行复用与强制重编译两条路径。 | 使用本帧真实地址和正确 Before/After；不误复用上一帧 descriptor 或状态。 |
| T20 · M3 · 动态 fragment 与副作用 | 现有通用 AddPass 形式提交 UI、upload、readback、capture。<br>保持 payload 变化但结构稳定，再变更结构。 | 稳定主体不重建；组合结构改变有显式失效/慢路径；副作用不漏执行，费用纳入 GraphMaintenance。 |
| T21 · M3 · 裁剪完整工作 | 无可观察消费者的分支，以及一个被其他 live pass 共用的 work。<br>编译、准备、录制。 | 无用分支完整 list/binding/upload/record 为零；共享 work 仍恰好执行一次。；允许变更时准备有限的轻量静态 CPU recipe，不允许无 live 消费者时进行该 pass 的 native PSO/descriptor/upload 准备。 |
| T22 · M3 · 空 draw 与必要 clear | 零 draw 但需要 clear 的 target；另有纯 draw 工作。<br>可见数在 0/非0 之间变化。 | 清屏和有效输出保留；单纯 draw count 不导致核心结构重复编译。 |
| T23 · M3 · 图内动态 geometry 依赖 | Compute/Copy 写 buffer，Raster 作为 VB/IB 读；子 range 重叠。<br>编译并复用计划。 | 写→读与必要屏障都存在；不能走静态只读外部 geometry 的免声明路径。 |
| T24 · M3 · 资源复用与 raster 合并 | 生命周期可复用资源、耦合 depth/stencil、不同 load/store/clear。<br>比较缓存/强制编译及优化开关对照。 | 无重叠生命期复用、无错误合并、无漏 stencil/depth 访问；结果一致。 |
| T25 · M4 · 录制有效状态轨迹 | runtime fake encoder；代表性不透明/透明/自定义 pass。<br>记录每个 draw 生效状态，对照参考路径。 | PSO、bindings、VB/IB、offset、draw range、viewport/scissor 与附件语义一致；仅允许省略冗余状态设置。 |
| T26 · M4 · 状态 run 绑定次数 | 单个兼容 pass，同 PSO/material/geometry，D 个对象。<br>分别用新 recorder 和参考直接 RHI 循环。 | 稳定 PSO/material/view 按 run 绑定；必要对象 offset 与 D 个 draw 保留；不伪称减少 draw 数。 |
| T27 · M4 · 两个诊断边界 | 合法图、故意无效图/Ready 输入；运行时切 Off→Full→Off。<br>在帧边界切模式，监视验证调用与错误位置。 | 只有冻结图输入/ReadyFrame 两个逻辑诊断入口；Full 在危险解引用前拒绝；Off 不执行诊断内循环；合法输出相同。 |
| T28 · M4 · 直接 RHI 成本对照 | ≥1,000 draw，优化构建，相同 RHI/PSO/set/geometry/CB 布局。<br>五轮交替测量手写循环和新 recorder。 | Record P50 目标 ≤对照 115%；P95 无明确退化；不把 RHI/backend/driver 成本全部称作 runtime 开销。 |
| T29 · M5 · 准备失败和状态提交 | 分配失败、set 写入失败或 encoder 创建失败的 runtime mock。<br>注入失败，随后重试下一合法帧。 | 无错误提交；外部状态只沿原成功提交路径更新；下一帧可恢复，保留必要运行错误处理。 |
| T30 · M5 · 在途多版本安全 | 三个 flight；延迟 mock GPU completion；各帧修改材质/geometry/shader。<br>录制、提交、延迟退休，删除并复用对象。 | 各 frame 看到自己的版本；descriptor/page 不提前重写；资产引用沿原路径释放，无 UAF/错帧。；每个 flight 独立累计待发布变更；GT canonical 修改不影响旧 snapshot。 |
| T31 · M5 · 稳定内存与缓存有界 | 稳定和 A/B 工作集，预热最大容量。<br>运行 10,000 帧并观察 plan/page/set/asset 引用数。 | 数量收敛；runtime 核心 draw/graph 热路径无逐帧/逐 draw 堆分配；RHI 与真实新增资源分开统计。；CPU catalog、flight 副本、dirty 元数据、pending readiness 和反向依赖表均计入内存账。 |
| T32 · M5 · 集成质量与作用域 | 当前 Tidal 等价 runtime fixture 与未改外部模块；D3D12/Metal。<br>窗口最小化/恢复、resize、output route、camera cut、TAA、阴影、透明等回归。 | 效果和资源状态正常；生产 diff 无 GpuSystem/RHI/backend/非 runtime 修改；真实平台结果单独报告。 |
| T33 · M5 · 总工作与关键路径 | Release-Light 同配置旧新实现；必要时有 worker。<br>关联 frame/flight、active work、submit、retire，采五轮。 | 满足总性能目标或提交可复核 RHI 下限；不改变质量、draw 数、flight 深度或提交策略，不漏计 worker/等待。；v2 必须同时报告 PreparationTotal、GT RenderPreparation、RT 后段和帧到提交关键路径，前移不得漏计。 |

### 新增验收 T34–T60

| ID / 阶段 / 用例 | 输入与操作 | 明确通过条件 |
|---|---|---|
| T34 · M2a · 同帧多次编辑合并 | 一个已注册 Renderer，已预热；十次变换/材质 assignment 编辑发生在一次 Scene 提交前。<br>分别验证 property getter 的即时 authoring 值，再提交、发布并渲染。 | 最终 authoring 值正确；同一 slot 一次入队，每类必要派生/静态更新至多一次；setter 不调用 RHI/分配 descriptor/上传 GPU。 |
| T35 · M2a · 材质编辑保留渲染身份 | StaticMeshComponent 已有有效 SceneObjectId、DrawId 和连续的 history。<br>只替换 material assignment 或改变 material numeric，随后另做真正 unregister/register。 | 材质修改不销毁重建注册身份、不错误重置 motion；真实注销后旧 generation 不再命中新注册对象。 |
| T36 · M2b · 对象值与模板失效分离 | 相同 geometry/material/policy；准备平移、非均匀缩放、mirror、奇异矩阵输入。<br>每帧仅修改指定 transform，另运行 camera-only 帧。 | 普通 transform 仅更新该对象矩阵/normal/bounds；无模板重建；mirror 选择正确变体；数值退化行为与参考一致；camera-only 派生重计算为零。 |
| T37 · M2b · 一万使用者的共享材质数值更新 | 10,000 Renderer 共享一个同布局材质；全部 flight 已预热。<br>只改 color/roughness 等非结构 numeric 值。 | 只更新唯一 material value payload，静态 draw/PSO recipe 重建为零，不遍历一万使用者来重建命令；所有消费者看到新值。 |
| T38 · M2b · 结构型材质依赖的局部失效 | 两个互不相关材质族及 program；部分对象使用待修改材质。<br>依次改变 queue、blend/depth state、pass enable、technique/schema。 | 正确重建实际受影响的 pass/filter/state/binding recipes，无关材质族不重建；图只在真实资源依赖或拓扑改变时失效。 |
| T39 · M2a · 逃逸裸指针跨多帧写入 | 保存 Material::As<T>()、NumericBytes 或 GetPipelineState 返回的可写地址/引用。<br>跨越多次无变化 frame 后通过旧地址写入，不再调用任何 setter。 | GT 提交仍发现变化；每唯一 escaped 材质每 epoch 最多一次观察；不能在干净帧后清除 escape 标记；Off 同样正确。 |
| T40 · M2a · 显式编辑快路径不全量轮询 | 只使用新 tracked setter/edit transaction 的材质；未获取 legacy 可写入口。<br>连续无变化帧、重复等值写和一次实际编辑。 | 无变化帧不扫描该材质全部 numeric/state；等值提交不重建；实际编辑按对应 dirty 类别传播；编辑事务语义在测试中明确。 |
| T41 · M2a · 没有 Renderer setter 的资源就绪变化 | mesh/texture 处于 Loading 或暂不可用；使用现有 runtime readiness 观察能力。<br>使资源变成 Ready，再测试失效/重新实例化或替换；不修改 Renderer。 | 新数据在对应 epoch 可见；不会永久停在空 record；只检查 pending/必要唯一依赖；不引入资产/GpuSystem API 改动或加载线程直接写 Scene。 |
| T42 · M2a · 未知自定义 proxy 的保守路径 | GetRenderDataRevision/TransformRevision 返回 0 的自定义 proxy；另有受控静态 proxy。<br>不发事件而改变自定义 geometry/transform。 | 自定义路径继续按 epoch 刷新且正确；不能错误永久缓存；受控静态对象不会因此全部退回全量重建。 |
| T43 · M2b · ProgramFrameId 和遍历编号扰动 | 两个 program 和多个材质/primitive；稳定语义和资源不变。<br>改变加入顺序、删除前置对象，令帧内 material/program 索引重新编号。 | 稳定 ProgramId/recipe 不失效；所有 frame-local 映射更新正确；不以 PassNameHash 单独判唯一。 |
| T44 · M2b · 策略版本及附件后绑定 | 同一 geometry/material 用两个不同 pass policy；切换 depth/RT format/MSAA 与必要动态 pass state。<br>注册/更新 policy，然后在多个图 attachment signature 下渲染。 | 不硬编码 Forward 到 Scene；不同 policy 不混用；静态 recipe 与最后 PSO 分开；未知签名不提前错误创建/绑定 native PSO。 |
| T45 · M2c · 跳过 epoch 的 flight 补齐 | 三个 flight；先修改一次对象，F0 发布后多个 epoch 不再改该对象，F1/F2 延迟可写。<br>按不连续次序再次发布 F1/F2；再让某页在等待期间修改多次。 | 每个 flight 收到最新完整页；只清目标 flight pending；不回放无用中间版本、不因全局 dirty 已清而丢变更。 |
| T46 · M2c · 全 flight 稳态零静态复制 | 固定成员、recipe/value 版本，预热所有 flight，tracked API，无 pending readiness。<br>1,000 帧只移动相机。 | 静态 catalog 页/深容器复制字节为零，normal/bounds 派生为零；view/temporal/culling/owner 获取允许非零并分开计数。 |
| T47 · M2c · slot/range 搬移与旧帧隔离 | 多个 section、稀疏/紧凑槽，F0 已发布，F1 可写。<br>删除、swap-remove、扩容、改变 section 数、复用 slot/range；在这些步骤间保留旧帧读取。 | 所有被移动的索引与数据一同发布；旧 F0 仍是旧映射；新 generation/record range 不混用；无裸 memcpy 含 vector/string 的拥有型对象。 |
| T48 · M2c · 资产帧保活不随脏页省略 | 所有静态页保持不变；旧 flight 持有其独立资源版本。<br>发布下一帧并在 GT 修改/清空 proxy 材质与 mesh，延迟旧帧 GPU 退休。 | 每帧仍沿原途径保留必要 owner；缓存不持有额外永久 StreamingAssetRef；旧帧资源有效，退休后引用可释放。 |
| T49 · M2c · 发布失败保留待同步集合 | 目标 flight 已可写；pending 包含多页和变量 payload。<br>在保活/分配/复制中间注入失败，再用相同最终 canonical 输入重试。 | 失败 snapshot 不发布；已清/半拷状态不会骗过下一次重试；pending 只在完整成功后清；frame owner 无泄漏。 |
| T50 · M2a · 冻结截止点与异步通知一致性 | GT 正在提交/publish；加载完成通知或下一次 authoring 变更晚于 cutoff。<br>在受控队列中插入事件，同时 RT 消费旧 snapshot。 | 当前 snapshot 单一 epoch，不混用新旧值；晚事件进入下一 epoch；后台线程不写 GT canonical 或 RT 已发布存储。 |
| T51 · M2d · 没有对象 dirty 的 temporal 更新 | 两个 view 有不同提交历史；对象此帧不变，其中一个 view skip/fail 或 camera cut。<br>发布多个 frame，并在之后恢复该 view。 | MotionValid/previous 数据按每 view 已提交历史变化，不能因对象 clean 跳过 temporal；材质编辑不制造错误 motion reset。 |
| T52 · M3 · 静态缓存不吞图内 geometry 依赖 | 已缓存 draw recipe 引用由 copy/compute 写入的 VB/IB，含重叠 range。<br>让几何内容变化而 draw range/recipe 不变；复用图计划。 | cached recipe 保留读取契约；写→读依赖和 barrier 不消失；只读外部 fast path 不错误适用。 |
| T53 · M3 · 有限静态策略与无用分支 | 大量注册对象、有限活跃 policies、被裁剪 pass 和隐藏场景分支。<br>预建或更新轻量 static recipe，再执行无 live 消费者的 frame；之后启用消费者。 | 允许必要轻量静态编译但不穷举所有 shader/pipeline 组合；被裁剪分支 native PSO/descriptor/upload/完整动态 work 为零；首次启用补齐并正确渲染。 |
| T54 · M2d · 全动态与 view-dependent 回退 | 100% 结构变更或真正 view-dependent 的自定义 geometry；另有静态对象。<br>采用 typed 批处理路径，与等效直接生成参考交替测量。 | 每 epoch/view 的必要工作正确且不经多层重复转换；无平方级去重或逐事件分配；遵守 M5 5%/10% 回归保护或提交可复核修正。 |
| T55 · M2b · 变化率与多视图扩展性 | N=1k/10k/100k CPU fixture，K/N=0/1/10/100%，1/3/6 view；父级批量变换及共享材质。<br>分别记录变更事件、受影响 recipe、派生值、pending 页与 visible draws，分开 sort/native。 | 结构更新按实际受影响记录而非 N×view/pass；页复制按目标 flight 未同步页计费；共享 value 修改不扩散到全部使用者。 |
| T56 · M0 · 前移工作总账与关键路径 | 同一二进制配置的旧完整路径与 v2；采 GT、RT 和必要 worker。<br>记录 authoring/render-extra、Commit、Publish/Retain、Freeze/ValueUpdate、DrawWorkBuild 与 frame submit。 | PreparationTotal 边界两侧一致且无父子/等待重复计数；同时检验 GT P95 与端到端延迟；不能只凭 PrepareCommand scope 消失通过。 |
| T57 · M5 · CPU 目录/flight 副本总内存 | 三个 flight，有限 Scene/material/policy 工作集，结构 A/B 与删除/添加。<br>运行 10,000 帧并记录 canonical、flight bytes、dirty lists、依赖边和 payload 容量。 | 内存数量收敛；无无限 epoch/逐 draw shared_ptr 链；固定容量后无逐 draw 分配；变量区搬移成本单独报告。 |
| T58 · M5 · 增量与完全重建差分 | 固定随机种子，混合 transform/material/schema/streaming/删除/多 flight 操作；Full 测试模式。<br>每个发布 epoch 生成独立只读完全重建参考，比较 Ready 语义与 fake encoder 轨迹。 | geometry/material/state/normal/bounds/排序/访问/历史一致；ASan/可用线程检查无生命周期错误；参考仅在测试/Full，不成为第二条生产管线。 |
| T59 · M2c · 多 Scene/多 pipeline 的唯一提交 | 一个 Scene 被两个 pipeline 使用，另有独立 Scene；policy 相同与不同两组。<br>同 FrameSerial 请求 snapshot、多次提交同 Scene，并变更一个 Scene 的材质。 | 同一 Scene/FrameSerial/flight 只提交并发布一次；policy 兼容时共享 recipe；无关 Scene 不失效；各消费者只读当前 snapshot。 |
| T60 · M5 · 失效风暴与停用输出有界 | 大量材质/program 批量改变，输出暂停数帧，pending readiness 资源随后删除。<br>重复编辑同对象、注册移除 policies、恢复输出并渲染。 | dirty/touched lists 按最终对象/页去重，不存无界事件回放；已移除依赖无悬空回调；恢复时一次性发布最新值，冷路径成本可见且不静默丢工作。 |

## 12. 建议提交顺序与每次交付范围

| 提交组 | 范围 | 完成后必须能证明什么 |
|---|---|---|
| 1 | M0 的 fixture/manifest/options/phase/counter 测试工具 | 新旧同条件能测；不再把 Debug、诊断模式和稳态混比 |
| 2 | M1 的名字所有权、Off geometry 检查移出热路径、LayoutId/recipe 修复 | R1/R2/R3 回归通过，独立 geometry 不平方级退化 |
| 3a | M2a：Scene 身份/dirty、内建 proxy 原地更新、Material tracked/escape/readiness | 变更来源完整；一次提交处理最终值；只改材质不反复销毁 Renderer |
| 3b | M2b：CpuDrawStore Scene 归属、pass 静态 recipe、共享 material/object 值 | 不变时不编译，value-only 不广播全 draw 重建；pipeline 规则不混进 Scene |
| 3c | M2c：per-flight 增量快照、统一准备协调、对象值增量冻结 | 跳帧 flight 能补齐；旧帧不被覆盖；相机移动不重建/全拷静态目录 |
| 3d | M2d：索引式 renderer list、唯一绑定元组和 Ready 接入 | 后段直接用 snapshot；原完整 PrepareCommand 链移除；总账证明不是前移 |
| 4 | M3 的不可变完整计划、结构 key、资源/输出状态补丁 | 稳定主体完整图重建为零，动态变化正确失效 |
| 5 | M3 的 live work 接入 + M4 的 Ready recorder/两处诊断入口 | 被裁剪工作不准备；record 直接消费最终数据，无新增命令解释层 |
| 6 | M5 的 in-flight/失败恢复/容量/平台/综合性能验收 | 高性能没有依赖错帧、漏同步、资产不释放或改变禁止修改的系统 |

这些是可以顺序合并的功能组，不是要求每组只做一个巨型提交。M1 回归测试与 M2 的数据结构设计可交叉推进；但在 M3 发布可复用计划前，必须明确它与 flight 数据的所有权。每组都维持可运行的直接路径；最终不是长期维护两个互相复制数据的 production pipeline。

建议下一次评审提交以下一张表：每个 fixture 的有效 draw 数、唯一 geometry/layout/material/recipe 数、各 dirty 类别/逃逸材质观察数、每 flight 发布页/字节、完整模板构造数、对象/材质上传字节、整图重建次数、Record 的 RHI 调用数、P50/P95/P99、runtime 分配数，以及代码范围审计。这样能直接判断问题是否被删除，而不是被挪走。

## 13. 证据与复算说明

### 原始输入

[S1] `20260911-190150-debug-tidal-d3d12-mt-imgui-noval.csv`，本轮上传，149 行统计记录；每渲染帧归一化分母为 242。原文件 SHA-256：`e301a73b6a05f793958a65187d64d1fb79ec9fdccca838d915be8a1ace1135f9`。

[S2] `20260911-105714-debug-tidal-d3d12-mt-imgui-noval.csv`，前一轮上传，仅用于同名 scope 和调用量描述性对照；不视作受控 A/B 实验。

[R1] `RadRay_Static_Review_542030a9.md`，上一轮静态审查报告，用于对照 R1–R7；本计划重读了主要生产热路径。没有以测试名称或接口注释替代具体实现分析。

### 固定版本源码

本节全部 RadRay 源码固定到 `542030a906fda526d0cbf3db4a309ff9007c8c1f`。行范围是源码范围，不是 CSV 的行号。

[C0] 默认分支最新提交查询。

`https://api.github.com/repos/ksgfk/RadRay/commits?per_page=5`

[C1] PrepareCommand / ProgramState / instantiate / ResetView。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/forward_pipeline/forward_lit_mesh_pass_processor.cpp`

[C2] PrepareGroup、UploadBuffer、dynamic-only set 与共享 scratch。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/frame_draw_resources.cpp`

[C3] CpuDrawStore::Sync、记录版本/缓存扫描与 snapshot 写入。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/cpu_draw_store.cpp`

[C4] BuildRendererLists / EmitTargets / EmitFromRecords / 排序。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/renderer_list.cpp`

[C5] PrepareRendererList / RecordRendererList，75–245 行。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/mesh_draw_command.cpp#L75-L245`

[C6] BuildIR、late cache、Compile、Optimize，1710–1915 行。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/render_graph.cpp#L1710-L1915`

[C7] Prepare、PlanBarriers、Record 与目标路由，2130–2330 行。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/render_graph.cpp#L2130-L2330`

[C8] PrepareFrame、SceneSnapshotBuild、BuildGraph，220–360 行。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/forward_pipeline/forward_pipeline.cpp#L220-L360`

[C9] RenderThread / TickFrame / WaitForWritableFlightSlot，630–815 行。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/application.cpp#L630-L815`

[C10] CopyProgramRows、pass payload 和延后 prepare，10–101 行。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/forward_pipeline/forward_graph.cpp#L10-L101`

[E1] Microsoft：Overview of the profiling tools，Release 测量、Total/Self 和插桩墙钟区分。

`https://learn.microsoft.com/en-us/visualstudio/profiling/profiling-feature-tour?view=vs-2022`

[E2] Microsoft：Creating and recording command lists and bundles，allocator 使用/复用约束。

`https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles`


### v2 新增核实依据

[R2] `RadRay_Next_Step_Plan_20260911.md` 与 `RadRay_Next_Step_Acceptance_20260911.json`，上一版设计输入，内容完整读入后修订；并非实现证据。

[C11] Snapshot builder 的结构/transform/material 复用、全量循环、asset 保活与 DrawStore Sync。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/render_scene_snapshot.cpp`

[C12] Snapshot/Builder 的线程、per-flight 所有权及现有 CpuDrawStore 成员。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/include/radray/runtime/render_framework/render_scene_snapshot.h`

[C13] Scene 注册身份及 slot 管理职责。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/include/radray/runtime/render_framework/scene.h`

[C14] PrimitiveComponent 的 MarkRenderStateDirty 与 transform 路径。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/components/primitive_component.cpp`

[C15] StaticMeshComponent 的材质/mesh setter 与 readiness tick。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/components/static_mesh_component.cpp`

[C16] Material 的 escaped 可写 API 与材质帧数据。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/include/radray/runtime/material.h`

[C17] Material GetRevision 的 observed bytes/state 与 texture readiness。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/material.cpp`

[C18] SceneObjectId、DrawRecord 与 CpuDrawStore 身份/存储。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/include/radray/runtime/render_framework/cpu_draw_record.h`

[C19] StaticMeshSceneProxy 的 payload/readiness/保活约定。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/static_mesh_scene_proxy.cpp`

[C20] FreezeObjectData/MakeNormalToWorld 与逐 view 参数。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/forward_pipeline/forward_frame.cpp`

[C21] PrimitiveHistory 的 committed/pending 版本和 MotionRevision。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/src/render_framework/primitive_history.cpp`

[C22] 自定义 proxy revision 为零时保守刷新、transform/motion 语义。

`https://github.com/ksgfk/RadRay/blob/542030a906fda526d0cbf3db4a309ff9007c8c1f/modules/runtime/include/radray/runtime/render_framework/primitive_scene_proxy.h`

[E3] Epic Games，Mesh Drawing Pipeline，Cached Mesh Draw Commands / Cache Invalidation / Resource Lifetime Management。仅用作 retained CPU draw 的设计依据，不据此推断 RadRay 的性能数字或要求其 GPUScene。

`https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine`

### 复算公式

```text
amortized_ms_per_render(scope) = sum(total_ns for exact scope key) / 242 / 1,000,000
calls_per_render(scope) = counts / 242
inner_compile_hit_rate = 209 / (209 + 33) = 86.36%
render_share(scope) = total_ns(scope) / total_ns(RenderSystem::Render)
```

可按 name 合并的指标，必须先确认不存在不同来源或不同阶段的同名 scope。Prepare/Record pass 的同名项必须保留 source line。当前聚合数据不能重建完整帧分布、线程关键路径、GPU 执行时长，也不能从一个 scope 的 percentile 列计算整帧 P95。
