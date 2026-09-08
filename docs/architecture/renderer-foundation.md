> - 适用: 编写 workload、接入 presentation/离屏 output、声明 graph pass、使用 transient pool 或 view history
> - 权威: 本文描述 renderer foundation 与内置 Forward 的当前契约；帧同步见 `frame-and-gpu.md`，原生接口事实见 `render-rhi.md`
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_output.h`, `modules/runtime/include/radray/runtime/render_framework/render_workload.h`, `modules/runtime/include/radray/runtime/render_framework/render_view.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph_compiler.h`, `modules/runtime/include/radray/runtime/render_framework/frame_graph.h`, `modules/runtime/include/radray/runtime/frame_submission.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph_runtime.h`, `modules/runtime/include/radray/runtime/render_framework/render_resource_pool.h`, `modules/runtime/include/radray/runtime/render_framework/view_state.h`, `modules/runtime/include/radray/runtime/forward_pipeline/forward_graph.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/src/forward_pipeline/forward_pipeline.cpp`, `modules/runtime/include/radray/runtime/render_framework/render_scene_snapshot.h`, `modules/runtime/include/radray/runtime/render_framework/culling.h`, `modules/runtime/include/radray/runtime/material_technique.h`, `modules/runtime/include/radray/runtime/render_framework/renderer_list.h`, `modules/runtime/include/radray/runtime/render_framework/frame_draw_resources.h`

# Renderer foundation

Renderer foundation 的系统都属于 `radrayruntime`。保留 game-thread `PrepareFrame` → render-thread `Render`
和 per-flight 资产保活；一次 Render 最多执行一张 graph，使用 GpuSystem 已经 Begin 的 Direct command
buffer。graph 不提交、不等待、不 acquire/present，也不增加线程、flight 或队列同步协议。

单队列和 per-flight pool 复用已有同步边界，避免再引入跨队列/flight 的资源回收协议；代价是
暂不做 heap aliasing，可能多占显存。history 独立于 transient pool，区分物理复用与跨帧内容
有效性，才能保证 skip/失败不旋转历史。窄 commands facade 封住 raw command buffer 的
barrier/submit 旁路，让 graph 负责状态收口。每帧 graph setup/payload 的 CPU 分配需按实际负载衡量。

## Output 与 frame plan

`RenderOutputRegistry` 分配进程内单调、不复用的 `RenderOutputId`。`AppWindow` 在 attach swapchain
时注册 presentation output，在 recreate 时更新尺寸并保留 ID，在 detach 时注销。外部 color output
由调用方注册 `ExternalRenderOutputDesc`；registry 借用 texture/view，调用方负责它们的 GPU 安全寿命。
v1 只接收单 mip、单 layer 的 2D color render target，格式/view/range/usage 必须一致。
重复注册同一 texture、非法初始或最终状态会被拒绝。`PreserveContents` 是调用方声明，成功写入
只更新真实 `CurrentState`，不会擅自改变下一帧的保留策略。

注册、更新和注销只能在 game thread；runner 安装的 idle waiter 在实际修改前排空 render/GPU work，
再开放 mutation gate。独立使用 registry 时由调用者确保 idle。外部资源仍由调用方拥有，注销前后的
原生资源销毁必须遵守 fence 寿命，并先摘除缓存 framebuffer。只读取已发布 output 状态不会触发生命周期等待。

`RenderPrepareContext` 提供 AppUpdateContext、无原生指针的 output catalog、`RenderWorkloadBuilder`
和 retained asset vector。builder 向当前 flight 的 `RenderFramePlan` 写入值类型 view families；
未知 output、重复 primary output、重复稳定 view ID、非法 view/scale 会记录 diagnostic 并拒绝整条
family。`BeginUpdateForFlight` 只清理当前可写 flight 的 plan 和 retained refs。

`RequestOutput(id)` 可以不创建 view family，供纯 UI 等无相机工作量使用；AddViewFamily 自动
请求其 output。plan 内请求去重，host 对每个 ID acquire 一次。`RenderOutputUsage::Scene`
与 `Auxiliary` 区分场景窗口和工具窗口，Forward/Tidal 的默认场景枚举跳过 Auxiliary。

host 只 acquire plan 请求的 output。acquire 失败的 family 保留身份，`OutputAvailable=false`；其他
family 正常执行。没有 presentation 或所有 output 都不可用时仍调用 pipeline，允许 side-effect
compute/copy。acquire 不录 barrier。graph 从 surface 的真实状态导入 output，成功写入标记由 executor
产生。pipeline 返回后 host 对未写 surface 做 fallback clear，再从实际状态转换到 RequiredFinalState；
presentation 为 Present，external 为注册时声明的状态。录制阶段只保存预测状态；队列 Submit 返回后外部 registry/AppWindow 才发布实际已提交状态。

## View resolve

`RenderViewDesc` 保存矩阵、世界位置、projection variant、normalized view/scissor rect、pixel jitter、
layer mask、LOD bias、camera cut 和可选稳定 `ViewStateId`，不保存 CameraComponent。ID 由
`AllocateViewStateId` 分配，0 表示不保留跨帧状态。

`ResolveRenderViewFamily` 根据取得的 output descriptor 计算 `OutputSize` 和
`RenderSize = max(1, ceil(OutputSize * RenderScale))`，检查 device limits。rect 最小边 floor、最大边
ceil；非法越界输入会失败，不静默裁正。Perspective/Ortho 的 aspect 来自实际 view rect；Explicit
matrix 保持原值，再应用约定的 jitter。pixel jitter 转为 NDC `(2*x/width, -2*y/height)`，使用 LH
projection。viewport 的 Vulkan Y 翻转仍统一经 `MakeViewport`。

`RuntimeTextureDesc` 支持绝对、family render size 相对、family output size 相对三种 extent。相对值
先 ceil，再 minimum clamp，再向上对齐，最后用 DeviceCapabilities/TextureSupport 验证完整 descriptor。
有序 format candidates 由 runtime policy 选择；RHI 不做降级或偷偷替换格式。

## Graph 声明与编译

`RenderPipelineContext` 提供 flight/serial、capabilities、HostWrites、resolved families、output import、
history 与 `CreateRenderGraph`/`ExecuteGraph`。AppFrameContext 和 surface 实现细节为 private。
不能从 pipeline context 取得窗口、swapchain、command buffer 或直接进行 barrier/submit。
每个 context 只允许创建、执行一次自身 generation 的 graph。

`OutputSurfaces()` 给装配器提供已取得的输出。`ImportOutputTarget` 导入真实目标；组件通过
`RenderGraphOutputBinding` 收到显式输入值，并返回显式输出值。`RenderPipeline` 是
`RenderGraphComponent`，提供 `PrepareFrame`、`BuildGraph`、`GraphRecorded`；RenderSystem 负责
创建唯一一张图、调用装配器、展开组件、执行，再分发录制结果。Forward 不引用 ImGui。

`FrameGraphComposer` 是应用装配策略。`FrameGraph::AddComponent` 按描述符预先声明 typed input/output
ports，`Connect` 连接指定生产值，`Expand` 调用组件的 BuildGraph 并连接其返回值。所有端口必须且
只能连接一次，描述符精确匹配。消费者可以先声明，依赖决定执行顺序；端口循环、缺失连接、非法
版本在 freeze 前诊断。默认装配器连接 renderer 与输出；注册了 overlay 时在线性 canvas 上串接
Scene 与 overlays 再编码导出（见 [Render framework](render-framework.md#overlay-与默认装配)）。
没有全局 latest output、固定 UI 阶段或隐式输出重定向。

Texture/Buffer 使用 `RgTextureValue` / `RgBufferValue`，携带资源 index、graph generation 和内容
version；view/pass/port/program/parameter/indirect handle 具有独立类型和 generation。跨图、错误类型、
越界版本及 freeze 后修改都会被拒绝。`CreateTexture` / `CreateBuffer` 返回预留的首个写版本；
`ImportTexture` / `ImportBuffer` 返回入口 version 0。写已有资源先调用 `NextVersion`，每个被写范围
只能有一个 producer。Read 消费指定版本，Load/ReadWrite 消费后继的前驱内容；Clear/Discard write
不消费旧内容。版本表达内容，所有版本仍共享一个原生存储，旧版本的读者通过 WAR 边排在覆盖前。
需要同时保留旧、新内容时显式 copy 到另一资源，不能分叉一个存储的版本链。

纯 CPU `CompileRenderGraph` 接收资源版本节点、执行节点和导出 roots；前端先解析 ports，再把
访问归一化成 texture mip × layer × aspect 或 Buffer 字节区间。depth 与 stencil 有独立内容有效位。
未写区间继承前驱，transient 初始无效；external 明确提供初始状态与有效位。即使最终被裁剪，非法
read/Load/ReadWrite 仍拒绝。Store Discard 产生无效内容，不能随后读取。

图为二分有向图：资源版本 → 消费 pass，生产 pass → 新资源版本。编译器从精确导出版本、
ObservableOutput 的最终内容和 `SetSideEffect` 反向标记 live，只沿内容依赖保留生产者；再为 live
passes 加 RAW/WAR/WAW 存储约束，并做稳定拓扑排序、环检测和最终生命周期分析。覆盖写可裁掉
旧 producer，hazard 不参与 liveness，也不要求声明顺序是执行顺序。

Raster builder 声明 attachment、Load/Store/Clear、采样、Buffer 或 UAV 访问；compute builder 声明
compute program 与读写。`CreateParameterSet` 用 canonical declaration name 和数组元素绑定图内
资源、sampler 或立即复制的 cbuffer bytes，反射决定参数类型；可写 binding 必须声明
Read/Write/ReadWrite。常量、上传数据和 payload 均由图/flight 持有。program 与 RendererList 借用至
录制结束；底层 native material sets 只允许 flight 保活的不可变资源，不能隐藏图内写资源。
`PrepareRendererList` 显式登记持久 geometry 的只读 usage；`ReadImmutableBuffer` 接受固定读取状态
和可选 owner，未传 owner 时由宿主 flight refs 保活。需要图内生成 geometry 时先声明其精确版本读取。

`ReadIndirectArguments` 把 Draw/DrawIndexed/Dispatch、offset 和固定 count 固化为 pass-local handle。
copy/resolve 为专用执行节点；区域 texture upload 的部分覆盖需要前驱该 mip/layer 内容有效。
纹理直接复制沿用 RHI 的 color-only 契约；depth/stencil 数据可采样后写入 Buffer 再读回。
`AddResolveTexturePass` 只接受相同格式、尺寸的 MSAA color → single-sample color，不做格式转换。
`AddRenderGraphBlit` 提供普通 raster 格式/尺寸与颜色编码转换，内置 artifact 独立于 ImGui 和 JIT。

`UploadBuffer` 复制源 bytes，声明上传执行节点；只有 live upload 才分配、map、写入和 flush。
`ReadbackBuffer` / `ReadbackTexture` 创建拥有 readback 存储的 ticket，并添加 copy 与 HostRead export；
只有匹配 frame serial 的 fence 完成后 `Read` 才复制 bytes。`ExportTexture` / `ExportBuffer` 是明确的
边界执行节点，声明根与最终状态，无需空 compute pass。Texture readback 当前为一个非 MSAA 2D
color 子资源；跨帧 history 仍通过专用 registry 导入。

同一原生外部资源在图中只有一个身份。重复 import 返回同一个 version 0，并验证描述符、初始
状态和有效位一致；冲突拒绝，写权限合并，所有登记的状态接收者在提交时一起更新。wrapper 与
其状态 span 必须活到 Submit，可通过 `Owner` / `Retain` 保留包装和 GPU owner；owner 随执行收据
保留到 fence 完成。原生指针仅用于本帧身份匹配，不作为报告或跨帧持久 ID。

执行次序为 setup → ports/freeze → pure compile → storage/attachment plan → realize → prepare →
barrier plan → record。任何 allocation/参数/PSO 准备失败都发生在图内命令录制前。图只录制，不提交。
逻辑内容依赖与物理执行访问分开保存：storage plan 确定物理对象后，按 pass/物理 cell 聚合
state、write 与 shader stages；barrier 规划和录制状态收口消费这一执行计划，不再逐 cell 重扫逻辑访问。
同 mip/layer 的 depth/stencil 采用共同原生 layout，Buffer 采用整资源原生状态；内容依赖仍保持
aspect/字节精度。不兼容的同 pass 原生状态组合被拒绝，保守扩大范围的原因写入报告。

`RenderGraphCompileOptions` 可独立关闭 culling、compatible resource reuse、attachment store 裁剪、
raster merge、barrier elimination 与 batch。无 live consumer 的 transient attachment store 可丢弃；
Load 不会凭空改为 discard。相邻 raster 只有相同 attachment/view、后续 Load、前序 Store，且没有
非 attachment 访问或 UAV 时合并；各逻辑 pass 保留各自 callback、票据与报告，一个 group 只 Begin/End
一次。屏障批处理沿用 RHI span，同状态读可消除，写后访问保留 memory dependency。

`RenderGraphExecutionResult::Submission` 区分 Declared、Recorded、Submitted、GpuCompleted 和
Cancelled。Recorded 仅说明命令进入 command buffer；当前 RHI Submit 返回 void，Submitted 只说明
调用返回，不能证明设备接受或执行成功。宿主将收据放入当前 flight，Submit 后提交已录前缀的状态，
fence 后完成 tickets。未提交的收据析构或取消会取消其操作。encoder/回调失败时停止后续 pass，
失败写内容失效，已录 barrier 状态仍作为 host fallback 的起点；整体失败不能推进 view history。
完整提交和线程边界见 [帧与 GPU](frame-and-gpu.md)。

Graph 独占 setup、编译结果和执行计划，完成收据不再保留整个 Graph 实现。提交阶段只保存最终状态
与有效位的写回快照，成功 Submit 后释放；完成阶段保存 tickets、显式 retained owners、readback 存储
以及可能自持 GPU 资源的泛型 pass payload，保留到 fence 完成。Graph 析构不会提前销毁这些 GPU owner。

## Per-flight Graph 资源、pool、history 与报告

`RenderGraphRuntime` 为每个 flight 持有一个 `RenderGraphFrameResources`，聚合
`RenderResourcePool`、Graph parameter sets/cache、`DynamicCBufferArena` 与 CPU 编译工作空间。工作空间
复用 version/cell 映射、读者和消费者集合、拓扑遍历暂存的容量；编译结果不借用工作空间，下一次编译
不会改写前一张图的依赖或执行顺序。共享工作空间的调用必须串行，Clear 时释放其容量，不缓存图拓扑。
安全复用时先清 parameter
sets/cache，再 reset 上传 arena，最后让 pool BeginFlight trim/复用；Graph 析构不会释放 GPU 仍引用的
descriptor 或上传页。parameter-set cache key 覆盖 layout/group、完整 binding 身份、数组元素、资源、
静态 offset/range、stride/format 与 sampler；dynamic offset 不进入物理 set key，命中后不改写 descriptor。

pool key 覆盖完整 texture/buffer descriptor；view key 覆盖
dimension、format、归一化 range/aspects、usage。pool 一次 flight cycle 租出的对象由图内 storage planner
分配给描述符完全相同且最终生命周期不重叠的 transient；这是兼容对象复用，不是 placed-resource heap aliasing。`EndGraph` 只结束租约；下次安全 `BeginFlight` 才允许复用。物理状态跨帧
保存，但新 transient 的内容有效位仍从 false 开始。

trim 默认删除超过三个未使用 flight cycles 的 entry，只在安全 BeginFlight 运行；先从
RenderPassRegistry 删除引用 view 的 framebuffers，再释放 view/texture。普通 external 临时 view 由
当前 flight 保存至下次安全 Begin；history view 缓存在所属 generation，避免逐帧创建。
pool stats 区分累计 hits/misses/created/views-created/trimmed 与当前数量、估算字节和历史峰值。
`SetResourceView` 标记随后创建的 transient 归属，0 表示共享资源；每 flight 的 `MemoryByView` 按
颜色、深度、存储纹理与 buffer 分组，并报告本 cycle 未使用的字节。Forward HDR 为各 view 自动标记。
history 单独报告 active/retired 字节、view 归属和各 flight retire bin 字节。估算只覆盖描述符数据量，
不含驱动对齐、压缩元数据、heap 空洞或实际 residency；不能据此宣称真实显存峰值或推导 heap aliasing 收益。

`ViewStateRegistry` 是 render-thread-owned。resolve 读取最后成功提交的 previous matrix，第一次、
camera cut、extent/format/sample 改变时 previous 无效。输出不可用、跳过、graph 失败不会推进。
`RegisterViewCompletion` 把 view、graph generation、frame serial、末端 pass 和确切输出版本绑定为不透明 token。
`CommitView(view, token, requiredDrawsSucceeded)` 同时验证 graph 成功、该 pass 实际执行且写入对应
output、必需 draw 成功；共享 output 的另一 view 写入不能代替本 view 的完成证明。该检查只排队提交，
view matrix、WithView 与 Independent history 都在对应 Submission 的 OnSubmitted 中推进，取消录制不推进。

history 以稳定 view ID + string key 标识，descriptor 精确匹配，允许 2–4 buffers。一个 key 每 frame
只能 acquire 一次；Previous 是最后成功提交的 image，Current 是下一写入 image。首次/失效时
PreviousValid=false。token 含 generation/serial/index，重复或过期提交失败；只有成功 graph 中实际执行
并有效写完 Current 的全部 subresources 才旋转，不因 acquire、被裁掉或失败而推进。

`HistoryCommitMode::Independent` 保持独立反馈纹理的提交行为；`WithView` 的所有已申请 key、view
矩阵和 primitive history 必须原子提交。颜色或深度任一未写、旧 token、重复提交、必需 draw 失败时
整组保持上次成功状态。旧的无 completion `CommitView` 入口不能推进 `WithView`。
`InvalidateView` 只失效时域组，不旋转或抹掉 Independent history；效果关闭再开启及 view rect 改变
由 pipeline 显式失效，不能仅比较纹理尺寸。

`PreparePrimitiveHistory` 从 immutable snapshot 准备该 view 的 pending 变换，以现有 proxy generation
为身份、MotionRevision 为连续性版本。上次成功提交的矩阵才是 previous；跳帧、其他 view 提交、
新建/重建/瞬移及非有限矩阵都不能伪造连续运动。所有 active primitive 进入 pending，不能只记录
当前可见列表。提交前不访问 Actor、Component 或 proxy，也不消费一次性的 reset 标志。

resize/descriptor 变化先成功创建新 generation，旧 generation 进入当前 flight retire bin，到该 flight
下一次安全复用再销毁。长期未使用的 view 同样先 retire 后释放。沿用单 Direct queue 的提交顺序，
不为 history 增加 fence。关停先 GPU idle，再按 pipeline → graph pools → view states → registry 顺序清理。

`RenderGraphExecutionReport` 按调用请求生成 Text/JSON/DOT，`Resolve` 是独立 pass 类型；报告记录执行与裁剪
原因、二分版本节点/read-write edges、内容与 hazard
依赖、最终执行序、资源 descriptor/lifetime/physical slot/ID、访问范围/stage、raster group、优化决定、
逐 subresource before/after、UAV 数量、pool stats 和
带 source location 及可选 binding/resource 的 diagnostic。ID 不使用原生地址。宿主图直接写入对应 flight
的 report，不在 ExecuteGraph 返回时整份复制。普通 Forward 帧不生成 JSON/DOT，capture 才序列化；
失败 diagnostic 始终保留。报告还记录 graphics PSO 请求/准备/新建次数。
`RenderSystem::GetGraphReport`、`GetFramePlan`、`GetPoolStats` 只在对应阶段安全点读取。
history 统计通过 `ViewStateRegistry::GetStats` 汇总跨 flight 的 generation 与估算显存，必须在 render thread 或全局 render idle 读取。

## 场景快照与剔除

`RenderSceneSnapshotBuilder::Build` 只在 game thread 调用，每 flight/frame 构建一次，与输出和视图数量无关。
Forward 持久复用 builder 的去重表和连续 primitive 结构记录，以及当前 flight 的变换、包围盒、材质和向量存储。
结构记录按场景发布顺序保存，稳定槽位直接比较 generation，只有成员或顺序变化才临时建立索引重排；
常见一到两个 section 随记录内联存储，更多 section 可溢出。变换与世界包围盒只保留在各 flight 的物化值中。
primitive 缓存以 generation 标识实例，RenderDataRevision 表示 section/geometry/range/local bounds
的结构变化，TransformRevision 表示变换变化；稳定结构不重复读取和验证 draw 范围，稳定变换不重算
世界包围盒。自定义 proxy 默认 revision 为 0，保持逐帧刷新；声明非零版本时必须覆盖相应数据和
异步几何就绪变化。StaticMesh proxy 的不可变 mesh payload 与基类变换版本满足这一契约。
一次性调用可使用 `BuildRenderSceneSnapshot`，跨调用结构缓存需持久持有 builder。
它物化 primitive generation、MotionRevision、变换、世界 AABB、layer mask、禁用剔除标志、MeshBatch 范围及 light 参数，按首次遇到
的 Material 去重并生成 pass 值快照。geometry/texture 仅借用指针，几何 owner 必须由 proxy 的
`CollectAssetReferences` 先追加到宿主 retained refs。快照不保存 game object 或 asset ref；发布后只读。
缺几何、空 draw、越界 index range 或不可用材质会跳过对应 section，并计入 `RenderSceneSnapshotStats`。
索引溢出拒绝整次构建，输出为空。primitive 值由 builder 管理，不得原地修改后继续复用版本；
`ResetForReuse` 清逻辑内容及物化状态，保留 vector 容量及以元素计的容量高水位。
每个 flight 仍持有独立值快照，builder 缓存只在 game thread 使用。材质按 generation 找回所属 flight 的
物化值，不依赖场景遍历顺序；未就绪材质暂存于 builder，不覆盖公开快照中的有效材质槽。generation/revision 命中时
复用该 flight 已物化的 pass 参数，并重新保活当前 ready 资源；新 flight、材质值/状态/资源就绪变化
重新物化。`MaterialBytesCopied` 只计本次实际复制字节；结构、包围盒与材质另报 rebuilt/reused 计数。

AABB 由局部中心/半长经过 affine transform 的绝对线性部分变换，支持旋转、非均匀和负缩放。
非法或非有限 bounds 不参与视锥拒绝，统计并保守保留；mask 仍然有效，Forward 只警告一次。
`Cull` 消费 snapshot 和一个 `ResolvedRenderView`，输出可见 primitive 索引与 view-space Z、可见光索引
与 distance squared。primitive、view 和额外 mask 逐位相交；禁用剔除标志只绕过视锥测试。
`PrimitiveSceneProxy::ResetMotion` 单调增加 MotionRevision；正常移动保留 MotionRevision，并增加 TransformRevision。重建 proxy 使用新
generation，不能复用地址充当身份。光源快照也复制 CastShadow。

视锥从实际 `ViewProjection` 提取，使用 D3D/Vulkan 公共的 zero-to-one clip depth。Perspective、Ortho、
旋转视图与无限远平面都按矩阵处理；无限 far 的退化平面停用，其余无效 view 拒绝剔除并清空结果。
非有限 bounds 不产生 NaN 排序值。方向光仅受 mask 约束；点光按有效世界球界限测试，负 radius、
非有限 position/radius 与不支持的 light type 计入拒绝统计。Spot 使用 normalized direction、正 radius
与 inner/outer cone cosine，要求 `0 <= inner < outer < pi/2`；Cull 用以灯为中心的 radius 球保守包住
锥体，不再把 Spot 当作 unsupported light。负数/非有限 radius setter 保留原值，零 radius 表示禁用
并由 Cull 拒绝；非有限光参数、无效方向和锥角计入 `InvalidLightParameters`，不进入 GPU 上传。
`CullingStats` 区分各拒绝原因并记录 CPU 时间。

每个相机 view 的主视锥剔除结果可供任意数量的 lists 消费；阴影 cascade 使用独立的 light view
剔除，不能从相机可见集挑选投影者。不在 pass 内重复遍历 Scene。
这些数组是帧局部 CPU 数据；增量物化缓存不改变 Scene 的 game-thread 所有权，也不引入 BVH 或 GPU-driven culling。

## 材质 technique

`MaterialTechnique::Create` 验证唯一且非空的 pass 名、非空 program、存在的 primary pass，以及每个
非空 material anchor 所在组恰有一个非数组 cbuffer。material group 只接受该 cbuffer、texture 与
sampler，不接收其他 buffer/UAV。空 anchor 表示这个 pass 完全不消费材质组，DepthOnly 使用此形式。

primary cbuffer 定义 canonical 相对字段路径。其他消费材质的 pass 必须匹配完整数值布局：字段集合、
kind、byte offset、size、stride、element count 及 cbuffer 总大小；物理组号、binding handle 和 cbuffer
声明前缀可以不同。各 pass 保留自己的 storage/layout，只有验证成功后才复制 canonical bytes。

**当前 metadata 限制**：schema 7 的 runtime 参数信息不包含标量类型或矩阵行列数。这里按现有 metadata
可表达的布局事实校验，不能区分布局相同的 float/int、矩阵形状或其他缺失的类型语义；这不是完整类型
等价验证。编写 technique 时必须保持这些语义一致，完整验证需要将来扩展编译器与 runtime 的公共契约。

secondary pass 的 texture/sampler 必须是 primary 声明的子集，按名称、kind 和数组数量一致匹配；不得
新增只在 secondary 存在的材质资源。数值字段不允许做子集。运行时缺资源仅使实际消费它的 pass
无效，因此缺纹理的 ForwardLit 仍可保留不消费材质的 DepthOnly。缺少/无效 pass 时 list 跳过对应 batch，
不回退到 primary 或其他 pass。固定功能状态可用 `SetPassPipelineState` 逐 pass 覆盖，RenderQueue 属于材质。

Material 实例拥有不可复用的 generation 与单调内容 revision。数值 setter 只在字节实际变化时增加
revision，纹理/sampler/queue 变化也使快照失效；game-thread `GetRevision` 还观察资源就绪状态以及
通过可变 `GetPipelineState` 引用写入的状态。`BuildRenderData` 是已物化材质快照的更新入口，消费方
不得原地修改其值后继续把原 generation/revision 当作有效缓存；手工修改前调用 `Invalidate`，下一次
Build 完整恢复 authoring 值。ProgramFrameId 由 builder 每帧分配，不影响材质内容版本。已发布 flight 的快照保持只读。

## Renderer lists 与帧内绘制资源

`RendererListDesc` 指定所需 pass、闭区间 queue 范围、额外 layer mask、view/culling 和排序方式。
通用 builder 验证结果与 view 的身份及 snapshot 索引，筛选候选 batch，再交给 `MeshPassProcessor`。
processor 每 batch 最多输出一条 command，拒绝原因汇总进 `RendererListStats`；无效描述会清空旧 commands。
默认 opaque 范围为 queue < 2500，transparent 为 queue >= 2500。
`RequireMaterialPass` 使产品必需 pass 的缺失单独计入 `MissingRequiredPass`，与可选 pass 跳过区分。

排序只使用 queue、按快照首次出现分配的 ProgramFrameId、material 索引、view depth、primitive/batch
索引。StateThenFrontToBack 按 queue/program/material 聚簇后从近到远；FrontToBack 与 BackToFront 按
queue 后的深度顺序排列。primitive/batch 为稳定的最终 tie-breaker，不使用资源地址决定绘制顺序。
`Commands` 保存发布顺序的完整 payload，紧凑 `RendererListItem` 保存排序值与 command 索引；排序仅
移动 Items。按执行顺序读取使用 `GetCommand`，不得把 Commands 的物理顺序当作绘制顺序。手工装配
且 Items 为空的列表按发布顺序执行；非空 Items 必须完整且唯一地引用所有 commands，在 graph setup
时验证。所有过滤后的 commands 及其 view/group offsets 必须保存至 graph 执行完毕。

`FrameDrawResources` 持有每 flight 的 `DynamicCBufferArena` 与 `ShaderParameterSet`。`PrepareGroup`
在 graph 执行前上传 bytes、按实际 binding number 排列 dynamic offsets，并解析纹理 subview/sampler。
buffer 排序、dynamic 标志和资源反射形成 `ShaderParameterGroupRecipe`，由拥有 layout 的 ShaderProgram
按 group 惰性创建并保持到 program 销毁。准备调用仍归 render thread，共享 program 的准备不能并发
修改其缓存。新 program 自带新 recipe 身份，不通过借用指针维持跨 program 的缓存。
临时绑定与 set key 复用容量；flight 安全复用仍清理 native sets 并重置 arena，不清 program recipe。
Forward 在同一 processor 内按 program、primitive 复用对象参数，同一物体多个 section 不重复上传；
HDR 同一 view 的列表和同一主视图的四个阴影级联已共享 processor，view-dependent motion
仍单独准备。processor 的 material/primitive 准备以 snapshot 索引为键，因此一个 processor 只服务
同一帧、同一 snapshot 的列表。`FrameDrawResourceStats::RecipeBuilds` 计实际首次构建，warm flight 为 0；另报组准备、
set 命中/创建和常量复制字节数。set 命中不代表本次参数上传被省略。
set cache 精确 key 为 pipeline layout、group、所有 buffer target/静态 offset/range、解析后的 texture view
和 sampler（含绑定身份/数组元素）。dynamic offset 不属于 key，相同 backing page 上的切片可复用 set；
spill 或静态 range/资源变化创建新 set。缓存命中后绝不改写已发布 descriptor，执行阶段不上传或写 set。
只含 dynamic constant buffer 的组（view/object）另有快速路径：set 仅由 layout、group 与各 buffer 所在
arena block 决定，用线性小表命中，不构造通用 key；语义与精确 key 缓存一致。

复用顺序为清空 renderer lists/借用 command → 清 set cache 与 sets → reset/裁减 arena，全部依赖既有
flight fence 安全边界。`MeshDrawDescription` 保存与视图无关的 program、PSO 输入、geometry 和 draw range；
`MeshDrawCommand` 加入帧内已准备的 groups，均不拥有 RHI 资源或资产。setup 中 `PrepareRendererList`
验证执行索引、几何/有序唯一 groups，
合并 graph 组并声明各 PSO，得到借用原 list 与 bindings 的 `PreparedRendererList`。同一 (buffer, range,
access) 的持久 geometry 读取在一次 prepare 内只向 pass 声明一次；重复声明只会线性放大 access 列表
与之后每个编译步骤，不改变语义。二者必须保持不变
直到图执行完毕。`SubmitRendererList` 只接受 prepared list，以 pass-local handle 绑定已准备的 PSO，
再执行 bind/draw；record 不逐 draw 查找 PSO、分配校验容器或重建参数。graph 命令包装对本 pass 内
已通过声明检查的 vertex/index buffer 做少量缓存，相邻 draw 共享几何时不重复查表；后端 encoder 亦跳过
与当前状态完全相同的 vertex/index 重绑定，pso 切换时全量重发。

`RendererListPassBindings::Create/Build` 把 graph parameter set 与当前 pass、program、真实 group 关联，
供同一 `SubmitRendererList` draw loop 合并 native per-view/object/material 组。按 program 逐 draw 绑定，
不会沿用上一 program 的组；native/graph 冲突、缺组、数组洞、错误 layout 或跨图/跨 pass set 在
执行前拒绝。不把 Shadow/AO/light-list 等产品字段写入通用 mesh draw executor。
`DrawExecutionStats::Succeeded` 是产品判定必需绘制完成的入口；只读深度 attachment 的 PSO 禁止
depth/stencil 写入，这一访问检查不扩大兼容 PSO key。
D3D12 在 encoder 结束时绑定 command-buffer-owned 的空 root signature，结束旧 static-data 参数
的使用期。使用有效空签名使后续 GBV barrier 注入仍可恢复状态；其寿命随原 command buffer，
不增加每帧 descriptor 分配，也不修改已发布 parameter set。

## Forward 范围

现有 `ForwardPipeline(app, scene, camera)` 保持默认的基础 Forward 用法；同一类的 `SetSettings`
开启 HDR 与效果，`Temporal()` / `Msaa()` 提供互斥 AA 配置。`SetViews` 支持 presentation/外部 output、
多个独立或不重叠的 view rect；空列表恢复构造时相机。设置和 view 在 game thread 写入，PrepareFrame
复制到当前可写 flight。稳定 ViewStateId 保持跨帧身份，切换设置不能修改已发布 flight。

HDR 工作尺寸按 RenderScale 解析，view 使用各自局部 attachments，最后合成到 output rect；每 view
独立保存剔除、列表、光照与 histories。`SetOutputOverlays` 在所有 view family 后把本帧产生的 SDR
离屏 output 采样进目标 rect，仍在同一张图中。调用方负责 output 的借用寿命，不能形成反馈环。

`SetOutputSurfaces` 为 HDR 路径提供本帧输出到世界空间屏幕的映射。每条描述包含 source/destination
output、单位 XY 平面的 LocalToWorld、layer mask 和亮度。管线按输出依赖排序各 family，拒绝环和
缺失的相机输出；数据复制到 flight，输出所有者必须保活到 fence。屏幕采样单采样 2D SDR 输出，
先解码到线性亮度，在 TAA 后、透明与折射取样前绘制。屏幕使用只读深度，不进入时域历史或投射阴影。
普通材质纹理仍由 TextureAsset 提供；动态显示通过此产品接口声明，不从样例注入渲染回调。

内置 PBR 的 Surface 是 metallic/roughness/alpha-cutoff/emission；Transmission.y 选择 unlit。
UVTransform.xy/zw 指定纹理缩放/偏移，xy 全零表示单位缩放。三个材质 pass 共享这些字段，
保持 cutout、阴影和颜色采样一致。HDR 材质输出线性值，不自行进行 tone mapping 或 sRGB 编码。
RenderScale 的范围为 0.25 到 1，基础非 HDR 配置固定为 1。

产品 ForwardObject 同时保存 `LocalToWorld` 与 `NormalToWorld`。CPU 按对象计算后者，作为
线性变换逆转置的正比例矩阵，shader 使用后再归一化，支持非均匀缩放、shear 与镜像。
实现用缩放后的余子式矩阵和行列式符号避免除以零；退化为平面时保留可定义的法线方向，
完全退化或无效变换产生零矩阵，由 shader 的 `safe_normalize` 选择有限的默认方向。
没有声明 `NormalToWorld` 的自定义 Forward program 继续只接收其实际声明的字段。

`ForwardGraph::BuildGraph(graph, stage, inputs)` 是只向调用方同一张图声明阶段的组合模块。输入复制
resolved view 值并借用 RendererList，携带 color/depth handles、attachment Load/Clear 和执行统计；输出
返回资源 handles、成功状态与实际 pass handle。没有 command 的 Depth/Transparent 成功但不加 pass，
Opaque 即使为空仍加 pass 定义输出。模块不创建/执行子图，不 acquire/present，也不提交 view/history。
ForwardPipeline 的基础与 HDR 配置共用该模块及提交循环。Tidal Atrium 和 Pipeline Probe 均直接
使用该管线，样例不再组织 Graph Pass。

存在有效 DepthOnly command 时声明 `Forward.DepthPrepass`（深度 Clear/Store）；没有则省略。
`Forward.Opaque` 总是存在，color Clear/Store；有预通道时 depth Load，否则 depth Clear。缺 DepthOnly
的 opaque 材质仍在 opaque pass 正常绘制。opaque 启用 LessEqual 与深度写入；透明材质不进入预通道，
`Forward.Transparent` 在有 command 时 Load color/depth，深度 attachment 只读且 PSO 禁用 depth/stencil 写入。
所有 family 的 passes 进入同一张 graph；即使列表为空，opaque clear 仍定义输出内容。

depth 为 family 相对尺寸的 transient，按 D32_FLOAT → D24_UNORM_S8_UINT → D16_UNORM 选择支持格式。
PSO key 使用 color/depth formats + sample count 的 `GraphicsPassCompatibilityKey`，Clear/Load、只读标志
和 framebuffer 尺寸不分裂兼容 PSO；原生 render pass 的完整 key 仍包含这些访问事实。
HDR 每个 view 注册末端 output pass 的 completion token，并合并必需 list/draw 的失败状态。
剔除、必需材质 pass、PSO、参数、graph 或末端合成失败均不推进时域组。

HDR 的两个配置组合如下；效果 shader 只属于产品层，基础图/RHI 数值验收使用独立最小 shader：

| 阶段 | Temporal | Msaa4 |
|---|---|---|
| 阴影 | 主方向光四 cascade、独立 light-view Cull、稳定正交投影、深度数组与 PCF | 相同 |
| 深度 | depth/normal/刚体 motion 预通道，包含 alpha cutout | 4x depth；不采样 MSAA 深度 |
| 光照 | 16x16 tile compute、固定全 near/far 区间；opaque 与 transparent 共用完整局部灯 | 相同 |
| AO | 线性深度、多 mip 金字塔、半分辨率 AO 与 bilateral 合成 | 关闭 |
| HDR | PBR opaque + sky，opaque history → TAA → 独立 transparent → indirect fireflies | 4x opaque/sky/transparent/fireflies → color resolve |
| 输出 | Bloom、曝光、tone map、SDR 合成 | 相同 |

局部灯最多 256、每 tile 默认 64；溢出 tile 回退遍历完整灯列表，不能静默丢灯。Spot 与 Point 通过
同一固定大小 GPU 记录传输。主方向光启用 CastShadow 时，级联阴影请求可见 opaque 的 ShadowCaster 材质 pass；主相机 cull 与 tile frustum 额外
覆盖一个像素，避免 jitter 边缘漏物体。history color/depth 用三图环，TAA 只处理 opaque/sky；sky
按相机旋转重投影，运动只包含刚体变换。effect signature 改变、cut、尺寸/rect/AA 变化先失效。

depth pyramid 是一张带 mip 的 R32_FLOAT texture，pass 按精确 subresource 声明依赖；没有消费者的
mip 会裁剪。CurrentHdr 在时域和透明之前保留独立副本，避免读写同一附件。SDR 的线性/sRGB 编码
依据 output 格式在正确边界完成一次；离屏叠加先解码再按目标格式编码。

设置拒绝非有限或越界参数、MSAA 与 AO/TAA 的组合以及依赖不存在输入的 debug mode。能力验证或
必需 shader 失败使 `Failed()` 为 true，不能以黑屏、清屏或跳过伪造成功。debug 支持线性深度、法线、
motion、AO、tile occupancy/overflow、Bloom、cascade、当前/历史 HDR 和深度金字塔末级。

`ForwardPipeline::GetSceneSnapshot` / `GetStageBStats` 只在所属 flight 的阶段安全点读取；统计包括快照
构建数、剔除调用/失败、三类 command 数量与执行失败。场景规模统计来自 snapshot，视图筛选来自 Cull，
候选分类来自 list，避免重复计数。

`RequestCapture` 只申请下一次 prepared frame；readback 由 graph 声明，`CompleteCaptures(flight)`
必须在所属 fence 完成后调用，生成 PNG 与 graph JSON/DOT。截图读取最终输出目标，包含启用的
ImGui；默认选择 workload 中首个可用 view family。正常帧不增加全队列等待。
展示宿主与回归命令见 [构建与测试](../guide/build-test.md#样例与专项验证)。

当前不实现 async compute、并行录制、heap aliasing、常驻场景、GPUScene、GPU count buffer、
depth resolve、骨骼/形变运动、透明时域重投影或跨分辨率 history 重建。缩放使用产品合成采样，
不声称实现生产级时域超分；pool/history 沿用既有 flight 同步。
