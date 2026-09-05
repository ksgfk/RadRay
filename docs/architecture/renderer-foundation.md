> - 适用: 编写 workload、接入 presentation/离屏 output、声明 graph pass、使用 transient pool 或 view history
> - 权威: 本文描述 Stage A/B renderer foundation 的当前契约；帧同步见 `frame-and-gpu.md`，原生接口事实见 `render-rhi.md`
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_output.h`, `modules/runtime/include/radray/runtime/render_framework/render_workload.h`, `modules/runtime/include/radray/runtime/render_framework/render_view.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph_runtime.h`, `modules/runtime/include/radray/runtime/render_framework/render_resource_pool.h`, `modules/runtime/include/radray/runtime/render_framework/view_state.h`, `modules/runtime/include/radray/runtime/forward_pipeline/forward_graph.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/src/forward_pipeline/forward_pipeline.cpp`, `modules/runtime/include/radray/runtime/render_framework/render_scene_snapshot.h`, `modules/runtime/include/radray/runtime/render_framework/culling.h`, `modules/runtime/include/radray/runtime/material_technique.h`, `modules/runtime/include/radray/runtime/render_framework/renderer_list.h`, `modules/runtime/include/radray/runtime/render_framework/frame_draw_resources.h`

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

注册、更新和注销只能在 game thread 且 render idle 时进行，debug 下由 registry 检查。runner 在
发布 render work 前关闭 mutation gate，待既有 render idle 同步完成后再开放。CPU render idle
不等于 GPU idle；销毁外部 texture/view 仍需等待引用它的 GPU work 完成，并先摘除缓存 framebuffer。

`RenderPrepareContext` 提供 AppUpdateContext、无原生指针的 output catalog、`RenderWorkloadBuilder`
和 retained asset vector。builder 向当前 flight 的 `RenderFramePlan` 写入值类型 view families；
未知 output、重复 primary output、重复稳定 view ID、非法 view/scale 会记录 diagnostic 并拒绝整条
family。`BeginUpdateForFlight` 只清理当前可写 flight 的 plan 和 retained refs。

host 只 acquire plan 请求的 output。acquire 失败的 family 保留身份，`OutputAvailable=false`；其他
family 正常执行。没有 presentation 或所有 output 都不可用时仍调用 pipeline，允许 side-effect
compute/copy。acquire 不录 barrier。graph 从 surface 的真实状态导入 output，成功写入标记由 executor
产生。pipeline 返回后 host 对未写 surface 做 fallback clear，再从实际状态转换到 RequiredFinalState；
presentation 为 Present，external 为注册时声明的状态。finalize 后外部 registry/AppWindow 保存真实状态。

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

Texture/buffer/view/pass、indirect arguments、compute program 与 parameter set 使用类型不同且含
generation 的 handle。不能跨 graph 使用，也不能在 freeze 后追加 setup；indirect/program/set handle
还绑定声明它的 pass 和命令种类。raster/compute setup 立即执行，把数据写入 graph 拥有的 payload；
执行函数为非捕获函数指针，只能解析当前 pass 声明的 handle。program 与 RendererList 继续借用并须
活过 graph 执行；Graph 参数的 CPU 常量在 setup 时复制，原生 set 与上传页由所属 flight 保活。
资产仍由宿主 per-flight refs 保活。execute callback 返回 void，不通过异常恢复。

Raster builder 声明 sampled read、buffer access、color/depth attachment 与 Load/Store/Clear；compute
builder 另可声明 UAV write/read-write 和 `UseComputeProgram`。两类 pass 都可用
`CreateParameterSet(program, group, bindings)`，以 canonical declaration name 和数组元素绑定 Graph
texture/buffer、sampler 或 setup 时复制的 cbuffer bytes。SRV/cbuffer 自动成为只读访问；可写声明必须
显式选择 Read/Write/ReadWrite，Graph 据此生成依赖与 barrier。immutable/static sampler 来自 resolved
layout，不由调用方重复提供。

`ReadIndirectArguments` 把带 `Indirect` usage 的 buffer、Draw/DrawIndexed/Dispatch 种类、4-byte aligned
offset 与固定 count 固化为 pass-local handle；没有 GPU count buffer。graphics/compute facade 只接受该
handle，不再接收任意原生 buffer。copy pass 使用专门的 buffer/texture/texture-to-buffer API；
`AddResolveTexturePass` 处理同格式、同尺寸的 2D/2D-array color 子资源，从 MSAA source resolve 到
single-sample destination，一次选择一个 mip 与连续 layers，不做 depth/stencil、缩放或格式转换。

Raster pass 可用参数 binding 写 buffer/texture UAV，也可直接调用带显式 stage mask 的
`WriteTexture`/`ReadWriteTexture`。它仍须至少一个 attachment；Graph 汇总实际写阶段并在分配资源前与
`UavWriteStages` 比较，能力不足即失败。D3D12 pass begin 的 `AllowUavWrites` 由 live declaration 推导，
不进入 graphics PSO compatibility key。同 pass 的重叠 readonly access 可合并；重叠写入、attachment
feedback、extent/sample 不匹配、无效 load/store 或超出 capability 的 descriptor 在录制前失败。

`Compile` 只执行 CPU 工作，不创建原生资源。内容有效性按 mip × array layer 跟踪，depth/stencil
共用一个 aspect domain，buffer 为整资源。transient 内容初始无效；external 从显式有效位开始。
Read、Load、ReadWrite 消费当前内容版本；Clear/Discard write 产生不依赖旧内容的新版本；Store Discard
使之后读取无效。即使 pass 最后被裁掉，非法 read/Load 也会报错。

observable external 的最终 writer 与 `SetSideEffect` 是 roots。沿消费内容的依赖反向标记 live 后，
仅对 live passes 建立 RAW/WAR/WAW hazard edges。所有 edge 都指向后声明 pass，声明顺序本身就是
稳定拓扑序。被后续 Clear 完整覆盖的旧 producer 可以裁掉，Load 则会保留它；hazard 不参与 liveness。

执行顺序固定为 setup → compile → realize → prepare parameters/compute PSOs → plan barriers → execute。
`Compile` 后先 realize 所有 live resource/view/render pass/framebuffer，继续复用 RenderPassRegistry；
prepare 只处理 live pass，创建 Graph parameter sets、上传复制的常量并取得每个 compute program 的
缓存 PSO。任一步失败均不录 graph 命令，diagnostic 携带 pass/binding/resource，history 不推进。
随后从 pool/external 的真实状态产生 pass 前 barriers；相同 UAV
state 的写后访问使用显式 UAV memory barrier。初始 UAV state 保守视作可能由前一图写入，
因此首次只读 UAV 访问也有屏障；同队列提交顺序不代替跨图的内存依赖。
每个 live pass 独立 Begin/End，并用同名 debug group。
pass commands facade 只转发绘制、dispatch、binding 和 viewport/scissor，不能通过 RHI encoder 的
`GetCommandBuffer` 绕过 graph。静态 mesh/asset bindings 暂由原有固定状态契约约束。

若 BeginRenderPass/BeginComputePass 失败，停止后续 pass；已录 barrier 的真实状态仍提交给 storage，
失败 pass 不标记内容有效/已写，host 可据此恢复。成功 graph 才能提交 view/history。PSO 缺失时产品
callback 可以跳过 draw，attachment clear 仍算有效内容。

## Per-flight Graph 资源、pool、history 与报告

`RenderGraphRuntime` 为每个 flight 持有一个 `RenderGraphFrameResources`，聚合
`RenderResourcePool`、Graph parameter sets/cache 与 `DynamicCBufferArena`。安全复用时先清 parameter
sets/cache，再 reset 上传 arena，最后让 pool BeginFlight trim/复用；Graph 析构不会释放 GPU 仍引用的
descriptor 或上传页。parameter-set cache key 覆盖 layout/group、完整 binding 身份、数组元素、资源、
静态 offset/range、stride/format 与 sampler；dynamic offset 不进入物理 set key，命中后不改写 descriptor。

pool key 覆盖完整 texture/buffer descriptor；view key 覆盖
dimension、format、归一化 range、usage。一个对象在一次 flight cycle 最多租出一次，不做同 graph
资源复用或 heap aliasing。`EndGraph` 只结束租约；下次安全 `BeginFlight` 才允许复用。物理状态跨帧
保存，但新 transient 的内容有效位仍从 false 开始。

trim 默认删除超过三个未使用 flight cycles 的 entry，只在安全 BeginFlight 运行；先从
RenderPassRegistry 删除引用 view 的 framebuffers，再释放 view/texture。普通 external 临时 view 由
当前 flight 保存至下次安全 Begin；history view 缓存在所属 generation，避免逐帧创建。
pool stats 区分累计 hits/misses/created/views-created/trimmed 与当前数量、估算字节数；不是驱动真实显存占用。

`ViewStateRegistry` 是 render-thread-owned。resolve 读取最后成功提交的 previous matrix，第一次、
camera cut、extent/format/sample 改变时 previous 无效。输出不可用、跳过、graph 失败不会推进。
`CommitView` 仅在 context 成功执行且对应 output 已写时有效。

history 以稳定 view ID + string key 标识，descriptor 精确匹配，允许 2–4 buffers。一个 key 每 frame
只能 acquire 一次；Previous 是最后成功提交的 image，Current 是下一写入 image。首次/失效时
PreviousValid=false。token 含 generation/serial/index，重复或过期提交失败；只有成功 graph 中实际执行
并有效写完 Current 的全部 subresources 才旋转，不因 acquire、被裁掉或失败而推进。

resize/descriptor 变化先成功创建新 generation，旧 generation 进入当前 flight retire bin，到该 flight
下一次安全复用再销毁。长期未使用的 view 同样先 retire 后释放。沿用单 Direct queue 的提交顺序，
不为 history 增加 fence。关停先 GPU idle，再按 pipeline → graph pools → view states → registry 顺序清理。

`RenderGraphExecutionReport` 提供稳定 Text/JSON/DOT，`Resolve` 是独立 pass 类型；报告记录执行与裁剪
原因、内容与 hazard
依赖、资源 descriptor/lifetime/physical ID、逐 subresource before/after、UAV 数量、pool stats 和
带 source location 及可选 binding/resource 的 diagnostic。ID 不使用原生地址；相同 setup 的 CPU dump 可直接比对。
`RenderSystem::GetGraphReport`、`GetFramePlan`、`GetPoolStats` 只在对应阶段安全点读取。

## 场景快照与剔除

`BuildRenderSceneSnapshot` 只在 game thread 调用，每 flight/frame 构建一次，与输出和视图数量无关。
它复制 primitive 变换、世界 AABB、layer mask、禁用剔除标志、MeshBatch 范围及 light 参数，按首次遇到
的 Material 去重并生成 pass 值快照。geometry/texture 仅借用指针，几何 owner 必须由 proxy 的
`CollectAssetReferences` 先追加到宿主 retained refs。快照不保存 game object 或 asset ref；发布后只读。
缺几何、空 draw、越界 index range 或不可用材质会跳过对应 section，并计入 `RenderSceneSnapshotStats`。
索引溢出拒绝整次构建，输出为空。`ResetForReuse` 清逻辑内容并保留 vector 容量及以元素计的容量高水位。

AABB 由局部中心/半长经过 affine transform 的绝对线性部分变换，支持旋转、非均匀和负缩放。
非法或非有限 bounds 不参与视锥拒绝，统计并保守保留；mask 仍然有效，Forward 只警告一次。
`Cull` 消费 snapshot 和一个 `ResolvedRenderView`，输出可见 primitive 索引与 view-space Z、可见光索引
与 distance squared。primitive、view 和额外 mask 逐位相交；禁用剔除标志只绕过视锥测试。

视锥从实际 `ViewProjection` 提取，使用 D3D/Vulkan 公共的 zero-to-one clip depth。Perspective、Ortho、
旋转视图与无限远平面都按矩阵处理；无限 far 的退化平面停用，其余无效 view 拒绝剔除并清空结果。
非有限 bounds 不产生 NaN 排序值。方向光仅受 mask 约束；点光按有效世界球界限测试，负 radius、
非有限 position/radius 与不支持的 light type 计入拒绝统计。`CullingStats` 区分各拒绝原因并记录 CPU 时间。

每个 resolved view 在一帧只剔除一次，结果可供任意数量的 lists 消费，不在 pass 内重复遍历 Scene。
这些数组是帧局部 CPU 数据；不实现常驻 render scene、BVH、增量同步或 GPU-driven culling。

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

## Renderer lists 与帧内绘制资源

`RendererListDesc` 指定所需 pass、闭区间 queue 范围、额外 layer mask、view/culling 和排序方式。
通用 builder 验证结果与 view 的身份及 snapshot 索引，筛选候选 batch，再交给 `MeshPassProcessor`。
processor 每 batch 最多输出一条 command，拒绝原因汇总进 `RendererListStats`；无效描述会清空旧 commands。
默认 opaque 范围为 queue < 2500，transparent 为 queue >= 2500。

排序只使用 queue、按快照首次出现分配的 ProgramFrameId、material 索引、view depth、primitive/batch
索引。StateThenFrontToBack 按 queue/program/material 聚簇后从近到远；FrontToBack 与 BackToFront 按
queue 后的深度顺序排列。primitive/batch 为稳定的最终 tie-breaker，不使用资源地址决定绘制顺序。
所有过滤后的 commands 及其 view/group offsets 必须保存至 graph 执行完毕。

`FrameDrawResources` 持有每 flight 的 `DynamicCBufferArena` 与 `ShaderParameterSet`。`PrepareGroup`
在 graph 执行前上传 bytes、按实际 binding number 排列 dynamic offsets，并解析纹理 subview/sampler。
set cache 精确 key 为 pipeline layout、group、所有 buffer target/静态 offset/range、解析后的 texture view
和 sampler（含绑定身份/数组元素）。dynamic offset 不属于 key，相同 backing page 上的切片可复用 set；
spill 或静态 range/资源变化创建新 set。缓存命中后绝不改写已发布 descriptor，执行阶段不上传或写 set。

复用顺序为清空 renderer lists/借用 command → 清 set cache 与 sets → reset/裁减 arena，全部依赖既有
flight fence 安全边界。`MeshDrawCommand` 不拥有 RHI 资源或资产，只保存 program、PSO 输入、geometry、
draw range、已准备的 groups 和排序值。`SubmitRendererList` 验证几何/有序唯一 groups，取得实际 pass
的 PSO，再 bind/draw；失败跳过单条 draw 并计入 `DrawExecutionStats`，不重建数据或返回 game thread。

## Forward 范围

Forward PrepareFrame 为 active presentation outputs 提交一个 view 的 family；包装 pipeline 可以追加
外部 output families。Render 对每个 resolved view 保存独立 CullingResults 和 DepthOnly/Opaque/Transparent
lists。每 family 支持多个不重叠 view，共享 attachments，通过各自 viewport/scissor 隔离写入。
当前调用方负责提供不重叠视图，Forward 仍只支持 RenderScale=1，不提供 upscale。

产品 ForwardObject 同时保存 `LocalToWorld` 与 `NormalToWorld`。CPU 按对象计算后者，作为
线性变换逆转置的正比例矩阵，shader 使用后再归一化，支持非均匀缩放、shear 与镜像。
实现用缩放后的余子式矩阵和行列式符号避免除以零；退化为平面时保留可定义的法线方向，
完全退化或无效变换产生零矩阵，由 shader 的 `safe_normalize` 选择有限的默认方向。
没有声明 `NormalToWorld` 的自定义 Forward program 继续只接收其实际声明的字段。

`ForwardGraph::BuildGraph(graph, stage, inputs)` 是只向调用方同一张图声明阶段的组合模块。输入复制
resolved view 值并借用 RendererList，携带 color/depth handles、attachment Load/Clear 和执行统计；输出
返回资源 handles、成功状态与实际 pass handle。没有 command 的 Depth/Transparent 成功但不加 pass，
Opaque 即使为空仍加 pass 定义输出。模块不创建/执行子图，不 acquire/present，也不提交 view/history。
ForwardPipeline 使用它声明三个标准阶段；Tidal Atrium 在同一张图中保持
Sky → Depth → Opaque → scene screens → Transparent → downsample/HUD/present 的显式组合。

存在有效 DepthOnly command 时声明 `Forward.DepthPrepass`（深度 Clear/Store）；没有则省略。
`Forward.Opaque` 总是存在，color Clear/Store；有预通道时 depth Load，否则 depth Clear。缺 DepthOnly
的 opaque 材质仍在 opaque pass 正常绘制。opaque 启用 LessEqual 与深度写入；透明材质不进入预通道，
`Forward.Transparent` 在有 command 时 Load color/depth，深度 attachment 只读且 PSO 禁用 depth/stencil 写入。
所有 family 的 passes 进入同一张 graph；即使列表为空，opaque clear 仍定义输出内容。

depth 为 family 相对尺寸的 transient，按 D32_FLOAT → D24_UNORM_S8_UINT → D16_UNORM 选择支持格式。
PSO key 使用 color/depth formats + sample count 的 `GraphicsPassCompatibilityKey`，Clear/Load、只读标志
和 framebuffer 尺寸不分裂兼容 PSO；原生 render pass 的完整 key 仍包含这些访问事实。
只有 graph 成功且对应 output 已写入才提交 view；剔除失败、不可用 output 或 graph 失败不推进 view state。

`ForwardPipeline::GetSceneSnapshot` / `GetStageBStats` 只在所属 flight 的阶段安全点读取；统计包括快照
构建数、剔除调用/失败、三类 command 数量与执行失败。场景规模统计来自 snapshot，视图筛选来自 Cull，
候选分类来自 list，避免重复计数。

当前不实现 async compute、并行录制、pass merge、heap aliasing、常驻场景、GPUScene、GPU count buffer、
动态分辨率 upscale 或 temporal effect；pool/history 继续提供既有资源寿命基础。
