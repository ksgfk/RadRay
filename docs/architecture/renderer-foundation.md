> - 适用: 编写 workload、接入 presentation/离屏 output、声明 graph pass、使用 transient pool 或 view history
> - 权威: 本文描述 renderer foundation 与内置 Forward 的当前契约；帧同步见 `frame-and-gpu.md`，原生接口事实见 `render-rhi.md`
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_output.h`, `modules/runtime/include/radray/runtime/render_framework/render_workload.h`, `modules/runtime/include/radray/runtime/render_framework/render_view.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph.h`, `modules/runtime/src/render_framework/render_graph.cpp`, `modules/runtime/include/radray/runtime/render_framework/render_graph_compiler.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph_runtime_options.h`, `modules/runtime/include/radray/runtime/render_framework/frame_graph.h`, `modules/runtime/include/radray/runtime/frame_submission.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph_runtime.h`, `modules/runtime/include/radray/runtime/render_framework/render_resource_pool.h`, `modules/runtime/include/radray/runtime/render_framework/view_state.h`, `modules/runtime/include/radray/runtime/forward_pipeline/forward_pipeline.h`, `modules/runtime/include/radray/runtime/forward_pipeline/forward_graph.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/src/forward_pipeline/forward_pipeline.cpp`, `modules/runtime/include/radray/runtime/render_framework/render_scene_snapshot.h`, `modules/runtime/include/radray/runtime/render_framework/culling.h`, `modules/runtime/include/radray/runtime/material_technique.h`, `modules/runtime/include/radray/runtime/render_framework/renderer_list.h`, `modules/runtime/src/render_framework/renderer_list.cpp`, `modules/runtime/include/radray/runtime/render_framework/mesh_draw_command.h`, `modules/runtime/src/render_framework/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/render_framework/renderer_list_pass_sets.h`, `modules/runtime/src/render_framework/renderer_list_pass_sets.cpp`, `modules/runtime/include/radray/runtime/render_framework/frame_draw_resources.h`, `modules/runtime/include/radray/runtime/render_framework/cpu_draw_record.h`, `modules/runtime/include/radray/runtime/render_framework/scene.h`, `modules/runtime/src/render_framework/cpu_draw_store.cpp`

# Renderer foundation

Renderer foundation 的系统都属于 `radrayruntime`。保留 game-thread `PrepareFrame` → render-thread `Render`
和 per-flight 资产保活；一次 Render 最多执行一张 graph。GpuSystem 已 Begin 共享 Direct command
buffer；每个已 acquire 的窗口另有一条 present command buffer，graph 把写入该 flip backbuffer 的
pass 以及把它收到 Present 的 Export 录进对应 CB。graph 不提交、不等待、不 acquire/present，也不增加线程、flight 或队列同步协议。
CPU 绘制准备在调用线程串行执行，不为 view 或 primitive 新开 worker；既有 `ThreadedRunner` 的 game/render
两线程边界不变。

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
`RenderGraphPlanCache` 在访问规范化前按结构 key 查找不可变 `CompiledFramePlan`；
hash 命中后仍逐字段比较，key 包含资源描述符、内容版本与有效位、访问范围、attachment、work mask、
copy/upload 和导出结构。native 地址、初始状态及 work payload 属于本帧数据。计划保存编译结果、
物理槽与生命周期、raster groups、live work、屏障模板及输出路由资源槽；命中时借用这些数组，
只填写本帧实例状态。普通 AddPass 路径在 ports 解析后查找；含模板的组合在解析 ports 前查找，
命中后借用已解析声明，未命中才物化组合并解析。Forward 的 LDR family、共享阴影、HDR view、
output overlay 与默认 composer 使用模板入口；动态组件仍逐帧调用 `BuildGraph`，其声明参与组合 key。
`RenderGraphCompileOptions::ReuseCompiledPlan` 关闭时走同一编译入口。
`RenderGraph::CompilePlanHit` / `RenderGraph::CompilePlanMiss` 与 report 的 `CompilePlanReused` 区分复用和重编译。

`RenderGraphTemplate` 在所属 device 上拥有不可变的 CPU 声明。普通未编译 graph 可通过
`DeclareTemplateSlot<T>`、`AddTemplateRasterPass` / `AddTemplateComputePass` / `AddTemplateWork`
描述稳定 recipe 与 typed frame data 的边界，随后 `FreezeTemplate` 转移声明所有权。冻结拒绝
legacy payload capture、native import、readback、即时 upload bytes 和 frame owner，防止把本帧资源留进模板。
`Instantiate` 借用模板声明；`RenderGraphTemplateInstance::Bind` 按类型和 builder generation 一次绑定非空数据，
只为 live work/pass 实例化 callback。被裁剪分支可不绑定 slot。固定大小的 upload slot 借用本帧 `RgUploadData`，
绑定 payload 按既有提交寿命保留。`Value` 将模板资源、port 与 pass 句柄映射到指定实例；
typed callback 内的模板 view/buffer/indirect 引用按所属实例映射，不能借另一个实例的索引。
同一模板最多保留四个位置变体，旧 graph 继续共享其不可变版本。新增版本、端口连接与动态 AddPass
片段参与组合 key；clear 值由本帧覆盖，资源 native 地址与初始状态在执行准备时填写。
`TemplatePlacementBuilds`、`TemplateMaterializations` 与资源/pass/ports 声明计数区分冷构建、动态声明和复用。

模板的 external texture/buffer slot 只保存描述符；每次实例化通过 `Bind` 提供本帧 native 资源、
状态、内容有效位与 owner。同一原生资源绑定多个逻辑槽时仍共享物理存储，别名关系与内容访问进入
组合结构，不能通过逻辑槽分叉同一存储的写版本。Forward 把矩阵、jitter、曝光、AO 半径、阴影距离、
灯光数据和 history 资源放入 typed frame data；描述符、feature、shader schema、history 分支与输出
连接决定结构变体。capture/readback 继续追加动态片段，不存入稳定模板。

`RenderGraphRuntimeOptions` 按 flight 冻结，不是全局服务。`Application::SetRenderGraphRuntimeOptions` 只影响尚未在 `PrepareFrame` 冻结的后续帧；`RenderSystem` 把该副本写入 `RenderPrepareContext` 与 `RenderPipelineContext`，本帧创建的所有 graph component 共用一份。默认性能路径是 `Validation=Off`、`Report=Minimal`、`GpuMarkers=false`；独立 graph 测试默认 `Full` 校验与完整报告。`Off` 的 draw/record 内循环不读 `Validation`，准备入口只决定是否登记统一边界的诊断，仍执行必要的 ports/依赖/barrier/PSO/上传与真实失败处理，归一化与 IR 构建在计划未命中时执行。`Full` 把契约检查收到阶段边界上的具名函数，仍在 Record 前拒绝负例：冻结输入边界的 `ValidatePlanInput`（未命中时在归一化后运行，命中时检查借用的 canonical 数据）、所有存活 pass 准备结束后的 `ValidateReadyFrame`（含 `ValidatePreparedDraws`、pass-set 的 required-group/碰撞，以及 `CreateParameterSet` 的缺 binding、重复数组元素和资源访问范围/stage 检查）。Full 的参数准备保留拥有诊断数据的请求与尚未写入的新 set，全部 Ready 检查通过后才执行 Set/Flush，全部写入成功后才发布到 flight cache；失败草稿不会进入缓存。`ResolveView` / `ResolveBuffer` 在所有模式下查询本帧每个 pass 的 native 访问表，错 generation 或无访问权属于不可执行的输入；录制时不扫描声明列表或读取 validation 模式。访问表在 Realize 后建立并复用 flight 容量；几何的声明检查在 Ready 边界通过 `ValidateGeometryBuffer` 执行：null buffer 任何等级都拒绝，图内 geometry 缺声明的 `UndeclaredGeometryRead` 只在 `Full` 生效，因为 `NativeBuffers` 仅 Full 登记。缺 attachment、参数类型/尺寸/group 不匹配、PSO/OOM 等无法形成可执行结果的失败留在原算法里。报告等级不改变合法执行计划；`HasFailed()` / `GetFirstErrorCode()` 判定成败，不再用 `Report.Diagnostics.empty()`。`Minimal`/`Counters` 不填充 `Report.Passes` / `Report.Resources` 明细（名字、依赖、Accesses、PhysicalId），只保留聚合计数与首个错误码；完整 pass/resource 表仅 `Report=Full`。GPU marker 独立于校验。编译缓存的字段级 equality 两种模式都保留；Full 在缓存命中后仍验证当前输入。驱动验证层不由该开关热切换。

CPU 阶段在 Tracy 上拆开：`ComposeGraph` 声明图与轻量工作请求；内置 Forward 的剔除与列表准备
由 `ExecuteGraph` / `RenderGraph::Execute` 编译后的存活集合驱动。执行顺序是 Compile、Realize、
Prepare、屏障与输出路由补丁、Record。`RenderGraph::Prepare` 先运行所有 live work，再执行
`PrepareUploads` 和 `PreparePasses`；后者逐个 live pass 运行其 prepare 回调，
`PrepareRendererList`、PSO 与参数 set 解析都在这一层的 pass zone 里。
`Record` 内每个 live pass 用 `RADRAY_PROFILE_SCOPE_DYN(pass.Name)`，CSV/GUI 看到的是 pass 名。
把 live pass 写入共享 Direct command buffer，或写入该 pass 所写 flip backbuffer 对应的 present command buffer；GPU 实际执行在随后的 `Submit` 与 GPU 时间线。

图为二分有向图：资源版本 → 消费 pass，生产 pass → 新资源版本。编译器从精确导出版本、
ObservableOutput 的最终内容和 `SetSideEffect` 反向标记 live，只沿内容依赖保留生产者；再为 live
passes 加 RAW/WAR/WAW 存储约束，并做稳定拓扑排序、环检测和最终生命周期分析。覆盖写可裁掉
旧 producer，hazard 不参与 liveness，也不要求声明顺序是执行顺序。

Raster builder 声明 attachment、Load/Store/Clear、采样、Buffer 或 UAV 访问；compute builder 声明存储
纹理与 buffer 的读写。声明期只产出访问与 attachment：不创建 descriptor、不解析 PSO、不组装绘制数据。
`AddRasterPass` / `AddComputePass` 的四参重载在 setup 与 execute 之间接一个
`bool (*)(Data&, RenderGraphPrepareContext&)` prepare 回调，三参重载表示没有 prepare 阶段。prepare 与
execute 都是不能捕获的函数指针，它们需要的一切只能放进 pass `Data`，包括 prepare 期才用来构造 span 的
cbuffer 常量字节。

`RenderGraphPrepareContext` 在 realize 之后、PlanBarriers 与 Record 之前只对 live pass 运行。
`ResolveGraphicsPipeline` / `ResolveComputePipeline` 以 realize 后的真实 `PassState` 命中 `ShaderProgram`
自己的 PSO 缓存，图不再登记 program。`CreateParameterSet` 用 canonical declaration name 和数组元素绑定
本 pass 已声明的 view handle、buffer 值、sampler 或立即复制的 cbuffer bytes，反射决定参数类型，产出录制期
唯一消费形式 `PreparedShaderGroup`。纹理经声明返回的 `RgTextureViewHandle` 绑定，读写语义由产出该 handle
的声明 API 决定；buffer 绑定要求本 pass 已声明同资源同版本、状态匹配且字节范围与 shader stages 覆盖该
绑定的访问。跨图 handle、未声明的 view 或 buffer 以 `ParameterUndeclared` 拒绝。类型、尺寸、group 不匹配
当场失败，否则无法形成 set；`Off` 只做 kind/类型、cbuffer 尺寸与 view 已 realize 的表示检查，缺 binding 与
重复数组元素的完整性扫描属于 `Full`。prepare 返回 false 使整张图失败，不进入录制。常量、上传数据和
payload 均由图/flight 持有。program 与 RendererList 借用至录制结束；底层 native material sets 只允许
flight 保活的不可变资源，不能隐藏图内写资源。

持久 geometry 不再逐 draw 导入图：prepare 用 `ValidateGeometryBuffer` 对每个 distinct buffer 检查一次，
接受由宿主 flight refs 保活的只读资产；`Full` 下图内 geometry 缺少本 pass 对应的 Vertex/Index 读声明时以
`UndeclaredGeometryRead` 拒绝，匹配按 native buffer 指针并合并本 pass 全部读声明的状态，因此别名共享同一
native buffer 的资源不会误报。需要图内生成 geometry 时仍先声明其精确版本读取。录制期
`BindShaderParameterSet` 接受持久 set 或 prepare 产出的 `PreparedShaderGroup`，
`BindGraphicsPipelineState` / `BindComputePipelineState` 绑定已解析的 native PSO，Record 不再回查图。

`ReadIndirectArguments` 把 Draw/DrawIndexed/Dispatch、offset 和固定 count 固化为 pass-local handle。
copy/resolve 为专用执行节点；区域 texture upload 的部分覆盖需要前驱该 mip/layer 内容有效。
纹理直接复制沿用 RHI 的 color-only 契约；depth/stencil 数据可采样后写入 Buffer 再读回。
`AddResolveTexturePass` 只接受相同格式、尺寸的 MSAA color → single-sample color，不做格式转换。
`AddRenderGraphBlit` 提供普通 raster 格式/尺寸与颜色编码转换，内置 artifact 独立于 ImGui 和 JIT。

`AddWork` 持有 typed CPU payload 与不捕获的 prepare 函数；pass 通过 `RequireWork(handle, mask)`
声明消费者。编译只合并 live pass 的 mask，每个 work 最多执行一次；被裁剪消费者不触发该 work。
work 之间不声明依赖，不能依靠注册次序消费另一 work 的结果；全部 work 完成后 pass prepare 才
消费它们的输出。work 失败时停止准备，不进入录制。

`UploadBuffer` 的即时重载复制源 bytes；deferred 重载声明固定字节数并借用 `RgUploadData`，
由相应 live work 填充源 span。该容器和源 bytes 必须覆盖上传准备期；短数据在录制前失败。
只有 live upload 才分配、map、写入和 flush。
`ReadbackBuffer` / `ReadbackTexture` 创建拥有 readback 存储的 ticket，并添加 copy 与 HostRead export；
只有匹配 frame serial 的 fence 完成后 `Read` 才复制 bytes。`ExportTexture` / `ExportBuffer` 是明确的
边界执行节点，声明根与最终状态，无需空 compute pass。Texture readback 当前为一个非 MSAA 2D
color 子资源；跨帧 history 仍通过专用 registry 导入。

同一原生外部资源在图中只有一个身份。重复 import 返回同一个 version 0，并验证描述符、初始
状态和有效位一致；冲突拒绝，写权限合并，所有登记的状态接收者在提交时一起更新。wrapper 与
其状态 span 必须活到 Submit，可通过 `Owner` / `Retain` 保留包装和 GPU owner；owner 随执行收据
保留到 fence 完成。原生指针仅用于本帧身份匹配，不作为报告或跨帧持久 ID。

执行次序为 setup → ports/freeze → 选择或编译计划 → realize → live work → upload → pass prepare →
屏障及输出路由补丁 → record。任何 allocation/参数/PSO 准备失败都发生在图内命令录制前。
图只录制，不提交；外部最终状态仍沿成功提交路径写回。
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

Graph 独占 setup 与本帧实例数据，借用不可变编译计划；完成收据不保留整个 Graph 实现。提交阶段只保存最终状态
与有效位的写回快照，成功 Submit 后释放；完成阶段保存 tickets、显式 retained owners、readback 存储
以及可能自持 GPU 资源的泛型 pass payload，保留到 fence 完成。Graph 析构不会提前销毁这些 GPU owner。

## Per-flight Graph 资源、pool、history 与报告

`RenderGraphRuntime` 为每个 flight 持有一个 `RenderGraphFrameResources`，聚合
`RenderResourcePool`、Graph parameter sets/cache、`DynamicCBufferArena` 与 CPU 编译工作空间。工作空间
复用 version/cell 映射、读者和消费者集合、拓扑遍历暂存的容量；编译结果不借用工作空间，下一次编译
不会改写前一张图的依赖或执行顺序。共享工作空间的调用必须串行，Clear 时释放其容量与计划缓存。
`RenderGraphRuntime` 的各 flight 共享有界 `RenderGraphPlanCache`，默认保留四个结构变体。
启用 `ReuseCompiledPlan` 时，命中跳过访问规范化、IR 构建、物理槽/raster/屏障模板和路由规划，
仍执行当前资源校验以及 Full 下的 canonical 契约检查。淘汰只移除缓存引用，不改写仍被 Graph
使用的不可变计划；真实资源、descriptor、初始状态与目标 command buffer 每帧重新实例化。
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
pass 范围用 graph 的核心 pass 计数，不依赖是否生成详细报告。
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

`RenderGraphExecutionReport` 按调用请求生成 Text/JSON/DOT，`Resolve` 是独立 pass 类型；`Report=Full` 时记录执行与裁剪
原因、二分版本节点/read-write edges、内容与 hazard
依赖、最终执行序、资源 descriptor/lifetime/physical slot/ID、访问范围/stage、raster group、优化决定、
逐 subresource before/after、UAV 数量、pool stats、本帧是否复用 compile plan，以及
带 source location 及可选 binding/resource 的 diagnostic。ID 不使用原生地址。宿主图直接写入对应 flight
的 report，不在 ExecuteGraph 返回时整份复制。普通 Forward 帧使用 `Report=Minimal`，不生成 JSON/DOT、不填充 `Passes`/`Resources` 明细；
`Counters` 只保留聚合统计。失败时 `HasFailed()` 与 `FirstErrorCode` 始终可用，`ToText()` 也会打印该码。报告还记录 graphics PSO 请求/准备/新建次数。
`RenderSystem::GetGraphReport`、`GetFramePlan`、`GetPoolStats` 只在对应阶段安全点读取。
history 统计通过 `ViewStateRegistry::GetStats` 汇总跨 flight 的 generation 与估算显存，必须在 render thread 或全局 render idle 读取。

## 场景快照与剔除

`Scene` 是 game thread 的唯一长期身份表：`SceneObjectId` 将稀疏 slot/generation 与稠密 packed 下标分开。
删除后复用 slot 必须换 generation，旧 ID 不能改到新对象。World/proxy 保持 authoring 值的即时可见性；
内建 proxy 的 setter 合并 dirty 位，同一 slot 每次提交最多入队一次。材质 assignment 原地更新
StaticMesh proxy，保留注册身份及 MotionRevision；真正替换 mesh 才重置运动连续性。

Scene 拥有 `CpuDrawStore` 与 `SceneRenderState`。后者在 `BeginRenderCommit` 截止点消费变更，
更新 canonical primitive、batch、material、light 与 draw 表；`RenderSceneSnapshotBuilder` 是发布器 facade，
不再私有持有另一份场景缓存。临时 builder 与 `BuildRenderSceneSnapshot` 同样复用所属 Scene 的缓存。
所有 pipeline、overlay 与 composer 先在 `CollectScenePolicies` 中登记 Scene 与本帧活跃 policy，
`RenderSystem` 随后调用 `FreezeRegisteredScenes`，在任何 `PrepareFrame` 回调之前完成统一截止点。
`RenderPrepareContext::PrepareScene` 按 Scene/PrepareSerial/flight 共享一份只读 snapshot；PrepareSerial 是
GT 输入 epoch，与之后 GPU 录制分配的 FrameSerial 分开。多个消费者不得按各自请求次数重复累计实际提交工作。
同一显式 epoch 属于同一个发布目标与原 retained owner 容器；独立目标需使用新 epoch，不能为旧数据补取已替换资产的新 owner。

primitive 的 RenderDataRevision 覆盖 section/geometry/range/local bounds，TransformRevision 覆盖变换。
受控 proxy 用通知传播变更；未知自定义 proxy 或零版本数据保守观察，pending mesh readiness 继续检查，
就绪后退出 pending 集合。世界 bounds 仅在相关输入变化时重算。Material 使用 generation 对应唯一共享值，
数值变化不向其所有 draw 使用者广播静态重建；结构或有效性变化才沿 material 使用关系更新相关 batch/draw。

包装 `StaticMeshSceneProxy` 的自定义 proxy 必须转发结构、变换版本和 pending readiness；若声明支持变更通知，
修改时还须由注册在 Scene 的外层 proxy 发送 dirty，未注册的内部 proxy 无法代发。Tidal 的 `DisplayProxy`
在变换版本实际改变后标记外层 `TransformOrBounds`，等值写入不入队，固定的 mesh/material assignment 保持不变。

`CpuDrawStore::SyncChanged` 在成员与范围不变时只访问已变 primitive；无变化时不遍历整表。
成员、section 或 pass 数量改变需重新排布范围。记录身份以 primitive generation、section、完整 pass 名与 PassPolicyId 区分，
hash 仅加速查找；DrawId/generation 与 RecipeRevision 分开，材质数值和 ProgramFrameId 不进入静态失效条件。
ShaderProgram 的不可复用 generation 与按完整值驻留的 LayoutId 提供稳定身份；实际 native PSO 仍在附件签名已知后解析。
Scene 的布局驻留由活跃 recipe 的 `Acquire` / `Release` 引用计数管理，替换时先获取新布局再释放旧布局，
注销后不保留无人使用的历史布局。旧 flight 保留数值 ID 与自身资源引用，释放驻留项不复用这个 ID。
手工绘制的局部 `PrimitiveVertexLayoutRegistry::Intern` 仍将布局固定到该 registry 的 `Clear` 或析构。
镜像改变记录当前手性选择，普通变换不重建 geometry/program/state/range recipe。
缺几何或不可用 pass 由状态与统计说明；IB/VB 范围诊断在 Full 的 `ValidatePreparedDraws` 执行。
容量、索引和真实资源失败仍走正常错误返回，不依赖诊断开关。

pipeline 用 `PassPolicyId + Revision + PassName + CompileStatic` 声明有限的静态规则，Scene 不依赖 Forward。
收集器合并同 Scene 的一致注册，拒绝同 ID 的冲突规则和截止点之后的注册；`SetActivePolicies` 接受本 epoch 的
完整活跃集合，稳定重复注册不重新编译。policy revision 改变沿使用者关系更新相关 draw，新增或移除 policy
触发范围重排；发布器使用 `ChangedDrawRanges/ChangedBindingRanges`，因此没有 primitive dirty 的 policy 变化也会到达所有 flight。

`StaticBindingRecipe` 是 snapshot 独立拥有的共享 POD 表，缓存身份包含 program generation、LayoutId、policy ID/revision。
它保存 group、buffer 索引和既定组顺序，不保存上传 offset 或 native set。回调的 binding 结果只能依赖这些身份；
normal/mirrored state 另可依赖静态 pass state 与 queue。Forward 注册 Lit、只读 depth 的 Lit、DepthOnly、
DepthNormalsMotion、ShadowCaster 五种 policy；两个 processor 的 record 路径读取已发布 schema 和有效 state。
没有注册 policy 的快照与 policy=0 的旧消费者仍有 batch fallback。view/material/native 准备由帧内 processor
执行，语义一致的数值和绑定元组在所属 FrameDrawResources 共享；状态不同的 policy 不制造重复上传。

每个可写 flight 独立拥有 snapshot 容器及 touched-page 去重集合。canonical 改变时标记所有已登记 publication 的
待同步页；发布只复制该目标尚未收到的最新完整页，成功后只清它自己的 pending，跳过多个 epoch 也能补齐。
拥有 vector/string 的材质页使用正常对象复制。删除、压缩、扩容和范围移动一并发布相关索引，旧 flight 数据不被覆盖。
零 dirty 帧不复制静态页，但仍从 proxy 与唯一材质重新获取本帧资产 owner，追加到现有 retained vector，沿原 flight 协议退休。
CPU catalog 不持有额外长期 StreamingAssetRef；geometry/texture/program 的借用必须受现有 owner 生命周期保证。

`PublicationId` 标识目标存储，`PublicationRevision` 每次成功发布递增，`ChangedFromPublicationRevision`
标明本次 changed ranges 所基于的成功版本。消费者只有连续消费时才能仅看 ranges；漏过一次发布须通过对象版本补齐。
复制 snapshot 会脱离 publication 身份，移动则保留。`ResetForReuse` 清逻辑内容和身份，保留容器容量。
发布开始先置 Valid=false；保活或页复制失败时回滚本次新增 owner、保留 pending 和上次成功版本，不能读取半更新内容。
页入队成功后才设置去重标记，目标成功登记后才取得 publication 身份；失败不能留下未入队的已标记页或未登记的目标。
复制阶段的 C++ 分配异常继续向调用方传播，owner 回滚由局部 RAII 执行，不将异常转换成普通成功或空快照。
待同步页只在所有表及变长字段复制完成后清除，页内中断与材质字段的部分赋值同样允许重试。
在相同最终 canonical 输入下重试才能发布；不可在冻结同一 epoch 后更换 authoring 资产，再为旧 canonical 获取新 owner。

`MaterialBytesCopied` 计 canonical 材质实际复制字节，`PublishedPages/PublishedBytes/PublishedMaterialBytes` 计目标发布量；
commit、draw 访问、结构/包围盒更新、legacy 观察与 pending readiness 各自计数。
`CpuSceneBytes/DrawRecordBytes` 目前是公开表的容量估算，不代表包含全部依赖边、内部缓存及变长 payload 的完整内存账。

显式内存盘点使用 `Scene`、`SceneRenderState`、`CpuDrawStore` 和 `PrimitiveVertexLayoutRegistry` 的
`GetMemoryStats`，独立 snapshot 使用 `MeasureRenderSceneSnapshot`。查询在串行观察点遍历当前数据，
不分配存储，也不作为每帧热路径的一部分。`SceneRenderStateMemoryStats` 分开记录 catalog、canonical、
publication 元数据及 shared flight 存储；dirty、待同步页、依赖边和 owner 引用另计数量。
`RenderMemoryStats::KnownBytes()` 包含对象、容器容量、字符串堆容量及 map 元素本体，排除 allocator/node
管理开销、hash bucket 存储、shared_ptr 控制块、借用的 authoring 对象和 GPU 资源；bucket/node 数量独立提供。
变量 payload 的实际长度和容量与上述存储分类重叠，不能再加进总字节数。发布统计中的
`PublishedVariablePayloadBytes` 计复制到 snapshot 的嵌套内容；`MaterialPayloadMoves/MovedMaterialPayloadBytes`
单独计 canonical 材质内容的所有权搬移，不把搬移视作复制字节或测得的耗时。

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

Create 只从 primary program 取该 cbuffer 的**字节数**，用来分配 Material 的数值存储；这是构造
信息，不是类型校验。secondary pass 的数值布局是否与 primary 相同由调用方保证——产品路径的
两个 pass include 同一份 HLSL cbuffer ABI，CPU 侧看的又是同一个生成 POD，逐字段扫描只会在
热路径上重做一遍生成器已经做对的事。物理组号、binding handle 和 cbuffer 声明前缀可以不同。

之所以不做运行时校验：唯一能证明布局的事实来自 DXC type tree，而它已经在 AOT 生成 POD 时被
用过一次，且两个 target lane 不一致时编译就会失败（见
[shader pipeline](shader-pipeline.md)）。运行时再验一遍既不能发现新问题，也挡不住调用方写错
`As<T>()` 的类型——那是调用方 bug。

secondary pass 的 texture/sampler 必须是 primary 声明的子集，按名称、kind 和数组数量一致匹配；不得
新增只在 secondary 存在的材质资源。运行时缺资源仅使实际消费它的 pass
无效，因此缺纹理的 ForwardLit 仍可保留不消费材质的 DepthOnly。缺少/无效 pass 时 list 跳过对应 batch，
不回退到 primary 或其他 pass。固定功能状态可用 `SetPassPipelineState` 逐 pass 覆盖，RenderQueue 属于材质。

Material 的 canonical 数值存储是一段 GPU 布局 `byte[]`（空 anchor 时长度为 0），有两个写入面：
产品路径 `As<Forward_MaterialData>()` 直接把它当成生成 POD 写，JIT 与测试用按名 setter 经
`ShaderParameterStorage` 盖在同一块 bytes 上。`Material` 与 `MaterialTechnique` 的头文件不出现
`Forward_*`，方向是调用方选类型而不是 runtime 依赖 Forward。

Material 实例拥有不可复用的 generation，以及内容、结构、数值、binding 四类 revision。
`SetNumeric`、按名 setter 与 `SetPassPipelineState` 等 tracked 写入在提交时只比较对应输入，等值写不产生变更；
未逃逸的干净材质不会每 epoch 扫描 numeric/state。`As<T>()`、可写 `NumericBytes` 与非 const
`GetPipelineState` 一旦返回可写地址，就永久标记 escaped；旧地址可能跨多帧写入，因此每个活跃唯一 escaped
材质每 epoch 仍观察一次，并单独记录比较字节。pending texture readiness 的必要观察同样不受 validation 控制。

`GetRevisions(epoch)` 冻结该 epoch 的观察结果，晚于截止点的 authoring 编辑进入下一 epoch。
`BuildRenderData` 是 canonical 材质快照的更新入口，保留各 flight 的独立副本；消费方不得原地修改后
继续沿用原 generation/revision，手工修改前调用 `Invalidate`。ProgramFrameId 只作 snapshot 内映射，
不会引起材质内容版本或稳定 recipe 失效。所有已发布 flight 保持只读。

## Renderer lists 与帧内绘制资源

`RendererListDesc` 指定所需 pass、闭区间 queue 范围、额外 layer mask、view/culling 和排序方式。
snapshot 含对齐的 `DrawRecord` 表时，通用 builder 按可见 primitive 的记录范围筛选 pass/queue/layer，再交给
`MeshPassProcessor::PrepareRecord`；缺几何仍以 `CpuDrawStore::Sync` 写入的 `DrawRecord::Status` 为准，IB/VB 范围改在 Ready 边界的 `ValidatePreparedDraws` 检查，不再在 processor 或 Sync 热路径插桩。没有记录表时仍走 `AddMeshBatch`，遍历时无条件检查将要使用的 primitive、batch 和 material 索引关系，不再追加全量诊断遍历。同一 `CullingResults` 与 view 的多个 desc 可通过 `BuildRendererLists` 一次遍历后按 pass 顺序写出，保持 processor 的 program 局部性；单列表 `BuildRendererList` 是它的薄封装。processor 每 batch 最多输出一条 command，拒绝原因汇总进 `RendererListStats`；无效描述会清空旧 commands。
`MeshPassDrawListContext::AddRecord` 发布 snapshot 记录与成功的帧绑定 ID；动态 `AddCommand` 消费右值候选。
`AppendTo` 只消费一次，重复发布或显式拒绝会丢弃未消费候选，不暴露内部 command 引用。
默认 opaque 范围为 queue < 2500，transparent 为 queue >= 2500。
`RequireMaterialPass` 使产品必需 pass 的缺失单独计入 `MissingRequiredPass`，与可选 pass 跳过区分。
`DrawRecord` 不含当前 CB offset 或 graph handle；镜像物体只翻转 `FaceClockwise`，不重建布局。

排序只使用 queue、按快照首次出现分配的 ProgramFrameId、material 索引、view depth、primitive/batch
索引。StateThenFrontToBack 按 queue/program/material 聚簇后从近到远；FrontToBack 与 BackToFront 按
queue 后的深度顺序排列。primitive/batch 为稳定的最终 tie-breaker，不使用资源地址决定绘制顺序。
静态列表仅保存 snapshot draw-record 索引与 `FrameDrawBindingId`，`Commands` 只拥有动态候选或手工装配数据。
紧凑 `RendererListItem` 保存排序值与发布索引，排序仅移动 Items。按执行顺序使用 `GetDrawCount`、
`GetDescription`、`GetPipelineState` 与 `GetGroups`；镜像状态由 `GetPipelineState` 选择，不能直接取普通 Description 的 state。
`GetCommand` 只适用于动态项。Items 为空时按发布顺序执行；Full 在危险索引访问前检查非空 Items 是完整置换。
列表借用的 snapshot 与帧资源必须保持有效，所有源数据在准备后保持不变直到录制结束。
列表捕获 PublicationId/SceneEpoch/PublicationRevision 和资源 epoch；reset、追加、赋值使 build revision 前进，
move 同时使源列表失效。旧列表不能混入新 publication 或新帧的数据。

`FrameDrawResources` 持有每 flight 的 `DynamicCBufferArena` 与 `ShaderParameterSet`。`PrepareGroup`
在 graph 执行前上传 bytes、按实际 binding number 排列 dynamic offsets，并解析纹理 subview/sampler。
buffer 排序、dynamic 标志和资源反射形成 `ShaderParameterGroupRecipe`，由拥有 layout 的 ShaderProgram
按 group 惰性创建并保持到 program 销毁。准备调用仍归 render thread，共享 program 的准备不能并发
修改其缓存。新 program 自带新 recipe 身份，不通过借用指针维持跨 program 的缓存。
临时绑定与 set key 复用容量；flight 安全复用仍清理 native sets 并重置 arena，不清 program recipe。
Forward 的多个 processor、view 和 state-only policy 共用本帧元组表，同一对象多个 section 不重复上传。
阴影使用非 temporal 的 object 值；带时域的 lit view 按各自历史身份准备。processor 不另存完整 command 模板，
`ResetView` 仅重置其 view 数值槽。processor 的 material/primitive 准备仍以 snapshot 索引为键，
一个 processor 只服务同一帧、同一 snapshot 的列表。`FrameDrawResourceStats::RecipeBuilds` 计实际首次构建，warm flight 为 0；另报组准备、
set 命中/创建和常量复制字节数。set 命中不代表本次参数上传被省略。
set cache 精确 key 为 pipeline layout、group、所有 buffer target/静态 offset/range、解析后的 texture view
和 sampler（含绑定身份/数组元素）。dynamic offset 不属于 key，相同 backing page 上的切片可复用 set；
spill 或静态 range/资源变化创建新 set。缓存命中后绝不改写已发布 descriptor，执行阶段不上传或写 set。
只含 dynamic constant buffer 的组（view/object）另有快速路径：set 仅由 layout、group 与各 buffer 所在
arena block 决定，先检查上一次命中的条目，再回退到线性小表，不构造通用 key；
所有命中仍比较 layout、group、buffer 与 buffer index，语义与精确 key 缓存一致。

`PrepareGroupId` 按 source/wire ABI/context/generation/revision/history/row 共享同帧上传切片，
并以 program generation/group 区分原生组；material 的值和资源版本同时参与身份。
不同 program 可共享 Offset，但 BindingHandle 必须各自匹配真实 layout。失败不发布 group 或 tuple ID；
已成功上传的切片可供同帧重试复用。`InternBinding` 将有序 group ID 组合驻留成 `FrameDrawBindingId`。
`FrameShaderGroupId` 是本 flight 当前帧的稠密索引，表扩容不改变 ID；组引用仅能在不再扩容时长期借用。
`PrepareSharedCBufferGroupId` 保留单 cbuffer 的入口，返回值接口用于旧调用方。
非 temporal object 使用同一 context；temporal 身份包含 ViewStateId、历史提供者、已提交 serial、失效 revision 和 PreviousViewValid。
wire 身份由调用方显式保证，
相同字节数不能代替兼容契约。`SharedBufferUploads/SharedBufferHits` 与 `SharedGroupHits` 分别计上传及原生组复用，
重复取得同一组不再增加 GroupPreparations。BeginFrame/ClearSets 使旧 ID 失效；开放寻址索引按 epoch 重置，
owner 表复用容量，不保留历史 key 的无限链条。


复用顺序为清空 renderer lists/借用 command → 清 set cache 与 sets → reset/裁减 arena，全部依赖既有
flight fence 安全边界。`MeshDrawDescription` 保存与视图无关的 program、PSO 输入、geometry 和 draw range；
`MeshDrawCommand` 加入帧内已准备的 `PreparedShaderGroup`，均不拥有 RHI 资源或资产。
`PrepareRendererList` 在所属 pass 的 prepare 阶段装配 `PreparedRendererList`。基本索引范围、资源存在性和
source epoch 在所有模式下提前检查；`Full` 注册拥有诊断数据的 `DeferReadyValidation` 回调，等全部存活
pass 准备完成后在 `ValidateReadyFrame` 边界执行 `ValidatePreparedDraws`（Items 置换、几何结构/IB 范围、组序）。
回调先检查列表 revision 与帧资源身份，避免遍历已经失效的来源。临时 pass-set 包装可以在 prepare 返回时
析构，诊断保有自己的组信息。装配循环不读校验开关，`Off` 不分配或运行这些诊断数据和容器。
内部封装的 `PreparedRendererList` 只能由准备函数构造，
每个 Draw 保存最终 PSO、必要 group 绑定范围、geometry 索引和三个 indexed draw 参数，不复制完整稳定描述。
录制直接从连续 Ready 行读取 IndexCount、FirstIndex 与 VertexOffset，不再为这些标量逐 draw 跳转到源描述。
`FrameDrawResources` 拥有当前 flight 的准备工作区，复用 recipe/geometry/group 索引和中间数组容量。
各个存活 Ready 持有独立输出存储的共享 lease；工作区只复用无人借用的存储，因此同帧多列表、池扩容和
跨 epoch 保留旧 Ready 都不会覆写封存数组。lease 只保护这些数组，来源列表、snapshot 与 native 资源仍受
原生命周期和 epoch 约束；旧 epoch 或被移动走的 Ready 不能录制。动态 offset 超出 inline 容量时，
复用外层 group 行和嵌套 offset 容量；`GetCacheCapacityBytes` 包含工作区、索引及各输出存储的容量。
没有 FrameDrawResources 的旧调用使用本次准备独立拥有的工作区，不引入跨帧缓存。
已发布的 LayoutId 直接参与准备；只有缺少该身份的旧描述才按本次调用惰性创建布局注册表，不长期保留旧布局。
静态 group 保留帧表 ID，pass/dynamic group 的唯一原生元组在 Ready 内拥有；录制入口一次取得当前帧表基址，
因此后续 pass 在同 epoch 扩容组表不使旧 Ready 悬空。持久 geometry 不进图，只由
`ValidateGeometryBuffer` 按 distinct buffer 检查一次，因此 draw 数不再放大 access 列表与之后每个编译步骤。
静态 program recipe 以 `(ShaderProgram, effective state ID, PrimitiveVertexLayoutId, PrimitiveTopology)` 为完整
key，动态无状态 ID 时比较完整 MaterialPipelineState。state ID 在 Scene 冷编译时驻留普通与镜像 state，
不重用已释放 ID；geometry 连续绑定段也在冷路径发布，手工 Commands 仅在 prepare 按唯一 geometry 补齐。
非相邻相同 recipe 同样复用，所以 `GraphicsPipelinePreparations` 统计 prepare 期实际
解析 PSO 的次数，可小于 draw 数；实际提交的绘制数量由 `DrawExecutionStats::Draws` 统计。
`RecordRendererList` 只接受 prepared list，绑定已解析的 native PSO 与已准备的 set 再 draw；record 不回查图、
不逐 draw 解析 PSO、不分配校验容器也不重建参数。prepare 按最终执行顺序合并组并计算必要绑定范围，
Record 不再做组 merge、offset 深比较或连续顶点段发现。首 draw 与 PSO 切换强制绑定全部组和几何，
同状态后续 draw 只发必要变化；每次 Record 独立恢复首 draw 状态，外部回调不能污染下一次调用。
入口检查 pass、列表 revision、资源 owner/epoch 和 publication；公开旧 Commands 的任意原地字段修改仍受不可变契约约束。
graph 命令包装不再持有校验状态；
后端 encoder 亦跳过与当前状态完全相同的 vertex/index 重绑定，pso 切换时全量重发。

`RendererListPassSets::Create` 在 pass 的 prepare 阶段按 `RendererListProgramParameters` 为每个
(program, group) 建一个 `PreparedShaderGroup`，`Find(program)` 返回按 group 升序的 span，供
`PrepareRendererList` 与该 program 的 native per-view/object/material 组合并。按 program 逐 draw 绑定，
不会沿用上一 program 的组；`Validation=Full` 时在 Ready 边界检查 required-group 覆盖与 native/graph 组碰撞。set 是本 pass prepare 现场产出的，
`Create` 记下 `GetPass()`，`PrepareRendererList` 拒绝属于另一个 pass 的 sets（它们的 view 在那里才有声明）；
不缓存跨帧 handle，program 与 list 必须活到图执行结束。
不把 Shadow/AO/light-list 等产品字段写入通用 mesh draw executor。
`DrawExecutionStats::Succeeded` 是产品判定必需绘制完成的入口；只读深度 attachment 的 PSO 禁止
depth/stencil 写入，这一访问检查不扩大兼容 PSO key。
D3D12 在 encoder 结束时绑定 command-buffer-owned 的空 root signature，结束旧 static-data 参数
的使用期。使用有效空签名使后续 GBV barrier 注入仍可恢复状态；其寿命随原 command buffer，
不增加每帧 descriptor 分配，也不修改已发布 parameter set。

## Forward 范围

现有 `ForwardPipeline(app, scene, camera)` 保持默认的基础 Forward 用法；同一类的 `SetSettings`
开启 HDR 与效果，`Temporal()` / `Msaa()` 提供互斥 AA 配置。`SetViews` 支持 presentation/外部 output、
多个独立或不重叠的 view rect；空列表恢复构造时相机。`ForwardViewSource::Auxiliary` 标记观察相机：
仍做主视锥 Cull、Opaque/Transparent 列表、Forward+ tiles 以及自己的 opaque/sky/tonemap；
不写 DepthNormals、TAA history、AO、Bloom 或 Fireflies，opaque 内写深度。
本帧只从第一个可用的非 Auxiliary 相机（若全是 Auxiliary 则退回第一个可用 view）声明一次四 cascade 阴影图集，
所有 HDR view 采样该图集。`SetOutputSurfaces` 会把 observer family 排到主相机之前，因此阴影必须在
per-view 循环之前声明，否则 CSM 会按正交观察相机裁剪。设置和 view 在 game thread 写入，PrepareFrame
复制到当前可写 flight。稳定 ViewStateId 保持跨帧身份，切换设置或 Auxiliary 不能修改已发布 flight。

HDR 工作尺寸按 RenderScale 解析，view 使用各自局部 attachments，最后合成到 output rect；每 Full view
独立保存剔除、列表、光照与 histories，Auxiliary view 不持有 history。`SetOutputOverlays` 在所有 view family 后把本帧产生的 SDR
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
| 阴影 | 每帧一次：主相机四 cascade、独立 light-view Cull、稳定正交投影、深度数组与 PCF；所有 view 采样同一 atlas | 相同 |
| 深度 | depth/normal/刚体 motion 预通道，包含 alpha cutout | 4x depth；不采样 MSAA 深度 |
| 光照 | 16x16 tile compute、固定全 near/far 区间；opaque 与 transparent 共用完整局部灯 | 相同 |
| AO | 线性深度、多 mip 金字塔、半分辨率 AO 与 bilateral 合成 | 关闭 |
| HDR | PBR opaque + sky，opaque history → TAA → 独立 transparent → indirect fireflies | 4x opaque/sky/transparent/fireflies → color resolve |
| 输出 | Bloom、曝光、tone map、SDR 合成 | 相同 |

上表描述 Full 相机。Auxiliary view 跳过预通道、TAA、AO、Bloom 与 Fireflies，仍采样本帧共享阴影图集。

局部灯最多 256、每 tile 默认 64；溢出 tile 回退遍历完整灯列表，不能静默丢灯。Spot 与 Point 通过
同一固定大小 GPU 记录传输。主方向光启用 CastShadow 时，级联阴影请求可见 opaque 的 ShadowCaster 材质 pass；主相机 cull 与 tile frustum 额外
覆盖一个像素，避免 jitter 边缘漏物体。history color/depth 用三图环，TAA 只处理 opaque/sky；sky
按相机旋转重投影，运动只包含刚体变换。effect signature 的结构变化、cut、尺寸/rect/AA 或 Auxiliary
变化先失效。曝光、AO 半径、阴影距离等普通数值以及 TAA 后的 Bloom/Fireflies 不重置 opaque history；
它们仍更新当前帧输入或选择相应的后处理图变体。

depth pyramid 是一张带 mip 的 R32_FLOAT texture，pass 按精确 subresource 声明依赖；没有消费者的
mip 会裁剪。CurrentHdr 在时域和透明之前保留独立副本，避免读写同一附件。SDR 的线性/sRGB 编码
依据 output 格式在正确边界完成一次；离屏叠加先解码再按目标格式编码。

设置拒绝非有限或越界参数、MSAA 与 AO/TAA 的组合以及依赖不存在输入的 debug mode。能力验证或
必需 shader 失败使 `Failed()` 为 true，不能以黑屏、清屏或跳过伪造成功。debug 支持线性深度、法线、
motion、AO、tile occupancy/overflow、Bloom、cascade、当前/历史 HDR 和深度金字塔末级。

`ForwardPipeline::GetSceneSnapshot` / `GetStageBStats` 只在所属 flight 的阶段安全点读取；统计包括快照
构建数、剔除调用/失败、三类 command 数量与执行失败。`TemporalViews` 与 `ValidTemporalHistories`
分别统计当前时域 view 数、实际具有有效颜色/深度和前帧 view 数据的数量。场景规模统计来自 snapshot，视图筛选来自 Cull，
候选分类来自 list，避免重复计数。

`RequestCapture` 只申请下一次 prepared frame；readback 由 graph 声明，`CompleteCaptures(flight)`
必须在所属 fence 完成后调用，生成 PNG 与 graph JSON/DOT。截图读取最终输出目标，包含启用的
ImGui；默认选择 workload 中首个可用 view family。正常帧不增加全队列等待。
展示宿主与回归命令见 [构建与测试](../guide/build-test.md#样例与专项验证)。

当前不实现 async compute、并行录制、heap aliasing、常驻场景、GPUScene、GPU count buffer、
depth resolve、骨骼/形变运动、透明时域重投影或跨分辨率 history 重建。CPU 绘制准备保持串行，不为
view 分块新开线程。缩放使用产品合成采样，
不声称实现生产级时域超分；pool/history 沿用既有 flight 同步。
