> - 适用: 编写 workload、接入 presentation/离屏 output、声明 graph pass、使用 transient pool 或 view history
> - 权威: 本文描述 Stage A renderer foundation 的当前契约；帧同步见 `frame-and-gpu.md`，原生接口事实见 `render-rhi.md`
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_output.h`, `modules/runtime/include/radray/runtime/render_framework/render_workload.h`, `modules/runtime/include/radray/runtime/render_framework/render_view.h`, `modules/runtime/include/radray/runtime/render_framework/render_graph.h`, `modules/runtime/include/radray/runtime/render_framework/render_resource_pool.h`, `modules/runtime/include/radray/runtime/render_framework/view_state.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/src/forward_pipeline/forward_pipeline.cpp`

# Renderer foundation

Stage A 的系统都属于 `radrayruntime`。保留 game-thread `PrepareFrame` → render-thread `Render`
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

Texture/buffer/view/pass 使用类型不同且含 generation 的 handle。不能跨 graph 使用，也不能在 freeze
后追加 setup。raster/compute setup 立即执行，把数据写入 graph 拥有的 payload；执行函数为非捕获
函数指针，只能解析当前 pass 声明的 handle。payload 中借用的 program、set 等必须活过 graph 执行，
资产仍由宿主 per-flight refs 保活。execute callback 返回 void，不通过异常恢复。

Raster builder 声明 sampled read、buffer access、color/depth attachment 与 Load/Store/Clear；compute
builder 另可声明 UAV write/read-write。copy pass 用专门的 buffer/texture/texture-to-buffer API。
同 pass 的重叠 readonly access 可合并；重叠写入、attachment feedback、extent/sample 不匹配、
无效 load/store 或超出 capability 的 descriptor 在录制前失败。

`Compile` 只执行 CPU 工作，不创建原生资源。内容有效性按 mip × array layer 跟踪，depth/stencil
共用一个 aspect domain，buffer 为整资源。transient 内容初始无效；external 从显式有效位开始。
Read、Load、ReadWrite 消费当前内容版本；Clear/Discard write 产生不依赖旧内容的新版本；Store Discard
使之后读取无效。即使 pass 最后被裁掉，非法 read/Load 也会报错。

observable external 的最终 writer 与 `SetSideEffect` 是 roots。沿消费内容的依赖反向标记 live 后，
仅对 live passes 建立 RAW/WAR/WAW hazard edges。所有 edge 都指向后声明 pass，声明顺序本身就是
稳定拓扑序。被后续 Clear 完整覆盖的旧 producer 可以裁掉，Load 则会保留它；hazard 不参与 liveness。

执行前先 realize 所有 live resource/view/render pass/framebuffer，继续复用 RenderPassRegistry。
任一 realize 失败均不录 graph 命令。随后从 pool/external 的真实状态产生 pass 前 barriers；相同 UAV
state 的写后访问使用显式 UAV memory barrier。每个 live pass 独立 Begin/End，并用同名 debug group。
pass commands facade 只转发绘制、dispatch、binding 和 viewport/scissor，不能通过 RHI encoder 的
`GetCommandBuffer` 绕过 graph。静态 mesh/asset bindings 暂由原有固定状态契约约束。

若 BeginRenderPass/BeginComputePass 失败，停止后续 pass；已录 barrier 的真实状态仍提交给 storage，
失败 pass 不标记内容有效/已写，host 可据此恢复。成功 graph 才能提交 view/history。PSO 缺失时产品
callback 可以跳过 draw，attachment clear 仍算有效内容。

## Pool、history 与报告

每个 flight 独立持有 `RenderResourcePool`。key 覆盖完整 texture/buffer descriptor；view key 覆盖
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

`RenderGraphExecutionReport` 提供稳定 Text/JSON/DOT，记录 pass 类型/执行与裁剪原因、内容与 hazard
依赖、资源 descriptor/lifetime/physical ID、逐 subresource before/after、UAV 数量、pool stats 和
带 source location 的 diagnostic。ID 不使用原生地址；相同 setup 的 CPU dump 可直接比对。
`RenderSystem::GetGraphReport`、`GetFramePlan`、`GetPoolStats` 只在对应阶段安全点读取。

## Forward 范围

Forward PrepareFrame 为 active presentation outputs 提交一个 view 的 family；自定义包装 pipeline
可追加外部 output families。Render 消费通用 resolved families，为每 family 保存独立 prepared draws
和 view/object/material offsets，透明排序与光源筛选使用该 view。Stage A Forward 只支持一 view/family
和 RenderScale=1；其他 workload 由自定义 pipeline 处理或 host fallback。

Forward 的 opaque Clear、必要的 transparent Load 都进入同一张 graph；depth 是相对 family 尺寸的
transient，按 D32_FLOAT → D24_UNORM_S8_UINT → D16_UNORM 查询支持后选择。不存在 per-window depth
owner 或手工 resource barrier。ShaderProgram 的 PSO key 使用 color/depth formats + sample count 的
`GraphicsPassCompatibilityKey`；Clear/Load 和 framebuffer 尺寸不分裂 PSO，创建时仍传真实兼容 pass。

Stage A 不实现 async compute、并行录制、pass merge、heap aliasing、scene culling、renderer list、
material multipass、动态分辨率 upscale 或 temporal effect；history 提供这些功能将来需要的寿命基础。
