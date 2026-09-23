> - 适用: 改帧节奏、提交时序、GPU 上传；排查"GPU 对象被提前销毁"或帧同步问题
> - 权威: 本文是帧节奏与 GPU 资源上传的唯一说明。资产侧的延迟销毁契约见 [asset-system](asset-system.md)；RHI 本身见 [render-rhi](render-rhi.md)
> - 锚点: `modules/runtime/include/radray/runtime/gpu_system.h`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/include/radray/runtime/wait_frame.h`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/gpu_system.cpp`

# 帧节奏与 GPU 上传

## 职责边界

| 系统 | 负责 | 不负责 |
|---|---|---|
| `GpuSystem` | **何时画**。instance/factory/device/主队列/fence、flight 槽位、上传器、帧 profiler、帧边界等待表、flight 完成消息的发布与内部状态应用 | 画什么 |
| `RenderSystem` | 多个持久 CPU RenderScene、按需对象 GPU 镜像、per-flight 场景操作包与视图、资产常驻与退休、program/artifact cache、RenderPass/Framebuffer registry | GPU 提交时序 |
| `WindowManager` | 窗口创建/销毁、swapchain acquire/present/recreate、事件分发 | — |
| `Application` | 固化帧序与关停顺序；消费 flight 完成消息；游戏侧的窄扩展点 | — |

## 对象参数上传与完成反馈

RenderSystem 的 RT 场景记录按需拥有 `SceneGpuData`，每个物理 flight buffer 独立维护去重 Pending、
已录制的 Attempt 和执行状态。Scene Apply 的最终变化登记到所有 flight 的 Pending，删除按完整 ShapeId 移除。
首次准备和扩容枚举当前全部存活 mesh，后续只访问 Pending；按槽位排序并将相邻记录合并为上传范围。
矩阵直接从 CPU Scene 打包到复用 scratch，不保存每 flight 的 CPU 世界矩阵副本。

同一 FrameSerial 最多准备一次，包括空场景和分配失败。成功录制后把此次身份移入 Attempt，记录 FrameSerial
与 BufferGeneration，清空本次 Pending；尚未提交的资源状态不视为已执行。
GT completion 只写该 flight 的 CompletedSerial/CompletedExecuted 交接字段，不修改活场景 GPU dirty 数据。
RT 下次 Consume 该 flight 时，先处理旧反馈再 Apply：成功清除对应 Attempt 并提交初始化状态；失败仅将仍有效的
完整身份合回 Pending，保留其他 flight 在此期间产生的新变化，重试读取 Scene 最新值。
未录制无需 Attempt；首次初始化被丢弃后，下轮仍完整初始化。扩容只发生在已退休 flight 的首次准备阶段，
其他 flight 的 buffer 保持独立。对象资源始终归还 ShaderRead 状态。

Scene 删除立即销毁 CPU Scene，将 GPU owner 转入删除批次的 RetiredGpu。当前有序主队列的删除批次真实 fence
覆盖先前全部使用，即使应用命令被丢弃仍可在 completion 释放；旧 scene generation 的反馈不进入新 Scene。
关停在 RT 停止、GPU drain 和真实 completion 消费之后释放剩余镜像。对象上传使用
`ResourceUploader::TryUploadBufferRanges`：先分配并写入所有暂存范围，再录制全部复制命令；对象 buffer 或暂存页
分配失败均返回空、保留 Pending，不录制部分目的资源更新。已分配暂存页仍随当前 flight 退休。
既有单范围 `UploadBuffer` 与暂存池 `Reserve` 保留失败即中止契约，需要恢复的调用方使用 Try 接口。
多范围接口要求同一目标、相同前后状态、递增且不重叠的范围。runtime 去重暂存页 barrier，整批仅录制一次前置和一次后置 barrier；
范围与 barrier 描述复用容量，RHI 的临时分配次数不随稀疏对象范围数增长。

## GpuSystem 的线程约定

`gpu_system.h` 在可调用的 `GpuSystem` 函数声明上标记线程与阶段，私有函数也遵循同一约定：

- **GT** 是 Application 所在线程，拥有游戏帧号与帧等待表。协程的启动、恢复和
  取消都必须在 GT；`GetFrameIndex` / `GetCurrentFlightIndex` 读取非原子状态，不能作为 RT 的帧号来源。
- **RT** 是当前 flight 的录制线程。GT 完成 Update 并交出槽位后，RT 独占录制与提交。
  单线程模式中 RT 与 GT 是同一线程。应用显式调用 uploader 的录制也发生在 RT。
- **GT/RT，retire 阶段** 表示两种线程都可能回收 flight，不表示函数内部提供互斥。
  Application 的两个 runner 都由 GT 独占 retire；RT 在 Submit/Present 返回后以 release 发布 CPU 完成计数，
  GT acquire 该计数后才读取对应 flight 的 fence。槽位通过 ready 信号量交给 RT，通过真实 fence 完成回到 GT。
  独立驱动仍可在 GT 或 RT retire，但须自行提供相同的独占和交接前提；channel 的锁只保护消息，不保护 flight 状态。
- **任意线程** 仅适用于读取固定配置、稳定对象指针或原子统计。对象必须已完成构造/装配发布，
  且尚未开始拆除；取得指针不会改变 device、queue、WindowManager 自身的线程约束。

生命周期操作仍归 GT：装配与拆除必须隔离其他读者，销毁前先停止生产者、等待 render/GPU idle，
并消费完成消息。`WaitAndRetireFlights` 自己等待 idle；`CleanupCompletedFlights` 与析构则要求
调用前已经 idle。逐函数的具体前提以头文件中的 API 契约为准。

## 帧序

```text
S0 runner::PrepareFrame：固定窗口维护批次 → writable 背压
  Application::ServiceFrameBoundaryGT
    完成消息批次 → 释放 RenderSystem/GpuSystem owner → 帧等待通知
    → OnRenderFrameComplete → AssetManager::Pump → ApplicationScheduler::Pump
BeginFrameTiming → DispatchEvents → OnUpdate → WorldManager::Tick（统一 epoch）
S1 Application::FinalizeWorldAndSealGT
  FinalizeWorldsGT → CollectRenderUpdates → OnCollectRenderViews → SealFrameGT → PublishFrameGT → ready
RT BeginFrameRecord → S2 ApplySceneUpdatesRT → 可选 OnRender
  EndFrameRecordAndSubmit：检查归还 → 上传 EndFlight → HostWrites.Flush
  → 有序 Submit/Present → 最终主队列 fence
```


单线程与双线程普通循环都遵循上述顺序。资源边界是当前 flight 上一轮的 fence 已完成、
runner 已取得槽位使用权；逻辑帧边界是完成批次与应用完成钩子处理结束后的计时点。
等待可写槽和处理完成批次放在事件派发之前，让紧接着的 Update 使用这些阶段期间到达的输入。
完成批次处理有明确终点，不会为回调新建的任务或稍后到达的 GPU 消息反复排空整个系统。

未启用 Gpu 时，单线程 CPU runner 按配置的 flight 数轮转索引，帧首完成上一帧已消费的
RenderSystem 包，再泵可选 AssetManager、scheduler 和窗口事件。Update 后若启用 RenderSystem，
同步 Publish/Consume；退出时完成最后已发布包，未发布的退出帧在 Shutdown 中 abandon。
该路径的 `LastFrameLatency` 为零，不创建 `AppFrameContext`、不调用 OnRender 或 GPU 完成钩子。

`DeltaTime` 是相邻逻辑帧开始时间之差，仍包含两个开始点之间的槽位等待与收尾耗时。
`LastFrameLatency` 从同一个逻辑帧开始时间计算到 CPU 观察到该 flight fence 完成；
包含事件派发、Update、录制、排队与 GPU 执行，不包含该帧开始前的槽位等待和完成回调。
手动驱动 GpuSystem 时，在完成调度之后、开始本帧工作之前调用 `BeginFrameTiming`。

`BeginFrameRecord` 为每次录制生成独立 FrameSerial，完成消息保留该 serial，不能以可复用
flight index 代替提交身份。RHI 的 void Submit 返回只表示 CPU 提交调用结束，真实 GPU 完成
由 fence 确认。应用通过 `OnRenderFrameComplete` 在游戏线程处理完成结果，自行持有需要活到
GPU 完成的资源 owner。

`CompleteFlight` 在 fence 完成后 resolve profiler，将
`FlightCompletion{FlightIndex, GpuWorkCompleted, FrameSerial}` 写入 `GpuSystem` 持有的
`UnboundedChannel<FlightCompletion>`，回收 staging，并发布原子 `WaitersCompleted`。
ThreadedRunner 在 GT 帧首用 `RetireRenderedFrames` 非阻塞检查已提交 flight；需要复用的槽位仍占用时，
`PrepareFlightSlot` 只等待该槽位的 CPU 提交和 fence。不访问协程等待表，也不调用应用钩子。
RT 不再回收 flight，staging 回收与 profiler resolve 发生在下一次 GT 回收边界或最终 drain。

`Application` 是 channel 的唯一消费者。帧顶取得可写槽位后，`Application::ServiceFrameBoundaryGT`
作为 `GpuSystem` 的 friend，直接通过私有 `_flightCompletions.TryRead` 非阻塞收集一个本地批次，
先对全部完成消息调用 RenderSystem::OnFlightCompletedGT 和 GpuSystem::ReleaseFrameResourcesGT，
验证 serial 并释放框架 owner，再调用 GpuSystem::BeginUpdateForFlight 重置槽位和派发等待者，最后通知应用。
不得先复用槽位再校验旧 owner。各通知服务在入口冻结本批工作，回调新建的同类通知留到下一 S0；
等待者以地址和不复用序号联合验证，前一个回调取消后一个 waiter 不会产生悬垂派发。
完成消息保留 FrameSerial，不能仅用可复用的 FlightIndex 识别一帧。
`GpuWorkCompleted` 仍表示该轮渲染结果有效；false 的跳过帧也已经经过其提交的真实 fence，
通知不代表画面已显示到屏幕。

`Application::PumpFlightCompletions` 包含 GT 线程断言与覆盖内部调度、应用钩子的重入保护。
批次收集期间不执行用户代码；处理批次期间新发布的消息留到下一次消费。
channel 不拥有回调或资产引用，没有观察者注册、注销与跨线程调用应用的接口。

多线程普通帧只等待当前 flight 可写，不等待上一帧 CPU record 结束，允许 `Update(n+1)` 与
`Record(n)` 重叠。GT 以已发布帧数减已退休帧数判断槽位容量，不维护第二份 writable 信号量。
需要复用旧槽位时，先确认该槽位已经提交，再等待其真实 fence；不等待更新的 `Record` 结束。
同一 flight 仍必须等 fence，其他槽位的 Update/Record 可继续重叠。
关闭与模态丢帧仍提交真实 fence，但跳过 OnRender，完成通知的 `GpuWorkCompleted=false`，
不能据此提交图像历史。当前不再录制或提交自动的资产上传前缀。

### 命令分配与有序批次

`GpuFlightSlot::CommandAllocator` 统一拥有本 flight 的 command buffer；没有默认共享命令或
专门的窗口命令池。`AppFrameContext::AllocateCommandBuffer()` 从池中分配并 Begin，应用完成
录制后调用 `ReturnCommandBuffers(desc, target)`，由 runtime End 并接管命令。归还不表示可立即
复用；本轮已经分配的命令都保留到最终 flight fence 完成，下一轮录制才能重置分配器。

`ReturnCommandBuffers` 复用 `render::CommandQueueSubmitDescriptor`，立即复制命令、fence 和
value 数组，不保留调用方 span。批次按归还顺序执行，项内按 `CmdBuffers` 数组顺序执行，
与分配顺序无关。仅接受当前 flight 分配且尚未归还的命令；归还后应用不得继续录制。
应用 wait/signal 作用于当前批次，允许无命令、无目标的纯同步批次。`WaitToExecute` 和
`ReadyToPresent` 必须为空，交换链同步由 runtime 根据可选呈现目标注入。

一个批次最多关联一个 `AppFrameTarget`，包含该 acquire 的 backbuffer 的全部访问及收口到
Present 的 barrier；普通批次不得访问本帧 acquire 的 backbuffer。应用自己决定录制落点，
runtime 不提供按 texture 查找命令的路由。跨批次的资源依赖仍须应用显式录制 barrier。

D3D12 对每个带目标的批次保持 `Submit → Present`，然后才处理下一批次，单窗口与多窗口
遵循相同规则。Vulkan 保留批次边界，先全部 Submit，再按登记顺序 Present。本阶段不合并批次。
每次 Submit 都追加递增的内部主队列 fence signal，并放在 signal 数组最后；Vulkan 因此把
acquire semaphore 的回收关联到 runtime 持有的 fence。最终收尾 Submit 的 fence 值才是
`flight.Signal`，中间批次完成不能退休 flight。空帧也提交真实完成 fence。

`SubmitFrame()` 封口并执行已经归还的工作。封口时未归还命令、未归还成功 acquire 的目标、
重复归还、跨 flight/跨录制使用、手填交换链同步或无效 fence 数组均属于不变量错误。
显式提交后禁止继续分配、acquire 或归还；runner 的自动收尾对已提交帧不再重复 Submit。

`AcquireWindow` 与提交前检查保留实时的最小化、隐藏和客户区判断。任一已 acquire 的窗口
不可呈现时，整帧应用命令均跳过，完成通知的 `GpuWorkCompleted=false`，但保留各批次的
wait/signal 及交换链收尾。D3D12 由既有 Present 路径消费 frame；Vulkan 仍消费 acquire
semaphore、signal present semaphore 并 Present，必要时从统一命令池录制单独的 Present 状态
转换。只发布实际执行的 backbuffer 状态，不发布被跳过的应用命令预期状态。
成功 acquire 的帧不能通过析构取消；当前没有通用取消协议。原始平台 API 不受协程调度器
管理，不能绕过 runtime 接口在渲染并发期间修改 HWND/NSWindow。

runner 在准备槽位、完成回调、Update 与录制期间拒绝模态 Tick 重入，事件派发期间允许模态 Tick。
普通循环在 DispatchEvents 前保留一个已准备的逻辑帧；模态 Tick 优先消费它，不重复领取 writable
槽位、重置 HostWrites、恢复完成批次或采样时间。事件派发期间出现模态活动后，外层跳过普通 Tick，
不会再提交已被模态路径消费的帧；尚未消费的准备状态保留供后续 Tick 使用。
同一次系统模态循环中的后续帧自行取得可写槽、收尾并开始计时，沿用系统已派发的输入，
不递归调用 DispatchEvents。双线程模态路径仍先等待已发布的 CPU 录制结束，槽位不可写时跳过本次 Tick。

### 窗口修改协程

运行期间由 `WindowManager` 提供 `CreateWindow`、`DestroyWindow`、`AttachSwapChain`、
`DetachSwapChain`、`ReleaseSwapChain`、`SetPresentMode`，以及尺寸、位置、显示、透明度、owner、
装饰、任务栏和置顶属性的协程接口。调用方在自己的 `TaskScope` 启动任务，通过 `co_await` 等待操作结果。
所有启动、取消和恢复均在应用线程；任务及其 scope 必须先于 manager 结束。应用线程不能同步等待
仍需 runner 推进的任务，原生事件回调中也不能阻塞等待正在执行的操作退出。

`WindowCreateDescriptor` 拥有标题字符串，owner 使用 `WindowHandle`。句柄包含所属 manager 和
实例内不复用的递增编号，不延长窗口或 manager 寿命。`ResolveWindow` 返回借用指针，不得跨挂起点或
维护阶段保存；操作执行前重新解析目标和 owner。销毁 owner 前解除存活子窗口的 owner 关系，
销毁主窗口则锁存退出请求。交换链描述不接受原生窗口指针或队列；实际缓冲尺寸跟随当前客户区，
`BackBufferCount == 0` 使用 GpuSystem 配置。

窗口接口本身是协程：先 `co_await WaitSafe()` 取得执行许可，再顺序执行原生修改和交换链处理，
最后 `co_await FinishOperation(permission)` 等待结果交付，之后才返回结果或通过停止通道结束。
这两个调度接口均为 WindowManager 私有接口；获得许可后只允许同步操作，不能提前返回或等待其他任务。
许可必须经过 FinishOperation 消耗，遗漏交付屏障或执行期间意外挂起属于契约违反。

等待表是 `ManualCoroutineScheduler<WindowOperationRecord>`：参数和结果由操作协程帧拥有，
记录只保存入队序号、阶段、协程 continuation 和停止状态，不保存操作回调或另一份命令参数。
同一条记录从等待安全点转为执行，再转为等待交付，保留最初的入队序号与取消注册。
runner 在等待前截取入队序号上界，再停止发布新帧、等待旧 CPU
录制/提交/Present 调用结束、退休 flight 并等待主队列 idle。整个批次只在 runtime 层发起一次主队列
排空，窗口函数不再嵌套等待。D3D12 的帧 fence 在 Present 前入队，不能用它代替最后的队列排空。

先处理已有的 OS/后端重建需求，再按 FIFO 恢复截止序号内等待 WaitSafe 的协程；每条协程完成必要的
交换链处理和 framebuffer 缓存淘汰后，在 FinishOperation 再次挂起。全部修改结束、退出修改阶段后，
才恢复等待交付的操作并继续调用者。原生回调和完成
续体产生的新请求留给下一批；连续 resize 不合并，较早请求返回时可能已经应用了同批的较晚请求。
批次执行期间的取消只标记记录，离开修改阶段后才允许展开协程帧。执行前取消不产生修改；执行后取消
不回滚已发生的修改，取消通过停止通道传递，不伪装为 `Completed`。
FinishOperation 使用直接 awaitable，无论是否已收到取消都必须先挂起到交付阶段；不能替换为普通
子 task，否则 task 启动时的取消检查可能在修改阶段内提前展开协程。

| 结果 | 契约 |
|---|---|
| `Completed` | 原生调用及当前需要的交换链处理结束；不承诺画面已显示，也不保证对象不会被后续操作销毁 |
| `Deferred` | 原生修改已执行，但窗口隐藏、最小化或客户区为空；保留交换链需求，恢复可呈现后再处理 |
| `InvalidWindow` | 目标或非空 owner 句柄已经失效或不属于该 manager |
| `Failed` | 参数被拒绝、窗口/交换链创建或重建失败；不回滚已发生的原生修改 |

交换链重建失败后暂停该窗口呈现，后续维护阶段重试；隐藏/最小化的窗口本身不触发重复排空。
创建结果额外携带句柄，释放交换链结果携带所有权。未挂接交换链的窗口可以正常存在。
`NativeWindow` 的同步接口仍供独立 window 模块使用；runtime 管理的窗口通过修改前通知检查阶段，
该通知只校验契约，不等待线程或 GPU。尺寸、位置、显示、样式、owner、alpha 和直接销毁都受此保护；
标题、输入和桌面能力接口保持原有语义。

窗口维护只在 S0 PrepareFrame 检查一次；事件或 Update 提出的请求等下一 S0，模态循环复用相同入口。
维护只等待 last-published packet 的 CPU 边界，绝不等待 GT 正持有的未发布包；实时 acquire/present 防御仍保留。
维护与结果交付期间拒绝模态 Tick 重入。启动主窗口和最终拆除走宿主私有的立即执行路径；
`OnInit` 中启动的操作在首个维护阶段执行。关停先关闭请求入口并取消等待者，关闭后新任务直接停止。

### 窗口输入

NativeWindow 保留原始键鼠、文本、滚轮与焦点事件，调用方可直接订阅其信号。
runtime 的 WindowInputRouter、AppWindow::GetInput、统一 DispatchInput 与 OnBeforeInput 扩展槽已移除。
应用自行处理输入状态、捕获与失焦取消；宿主不再提供应用/UI 输入归属路由。

通用窗口能力通过 `NativeDesktopCapabilities` 查询：光标形状与显隐、桌面鼠标定位、显示器
工作区和 DPI、UTF-8 clipboard、IME 候选框定位。Win32 实现这些能力及 DPI/display/capture
消息；Cocoa 的新增桌面接口明确返回能力缺失，不伪装成功。Cocoa 滚轮继续发送原始滚轮事件，
同时提供新双轴浮点事件；这里不增加 macOS runtime。

## Flight 槽位

`GpuFlightSlot` 按所有权/阶段分组，跨阶段的访问时序由 ready 信号量、CPU 完成计数与 GPU fence 保证：

| 组 | 成员 | 谁访问 |
|---|---|---|
| 录制态 | `CommandAllocator`, `Batches`, `Acquisitions`, `Uploader`, `HostWrites`, `Recording` | GT 帧顶重置 HostWrites；RT 接管后录制，应用显式上传的 staging 在 fence 后回收 |
| 计时态 | `FrameStartTime` | 游戏线程在帧开头写 |
| 保活态 | `Payloads`, `FrameSerial` | GT 登记/释放 payload；RT 在 BeginFrameRecord 分配编号，GT 匹配真实完成后清零 |
| 提交态 | `Signal` | RT 在 `EndFrameRecordAndSubmit` 写；GT 观察 CPU 提交完成后在 retire/`CompleteFlight` 读后清 |
| 等待表 | `WaitFrame` | 见下 |

`Recording` 从 BeginFrameRecord 到提交封口前为 true；AppFrameContext 同时校验保存的 FrameSerial。
封口后禁止继续录制，runner 的 EndFrameRecordAndSubmit 可重复收尾，不另存 Submitted 标志。

完成消息不挂在槽位上。`UnboundedChannel<FlightCompletion>` 属于 `GpuSystem`，由 retire 线程
写入、Application 在 GT 消费。`WaitFrame` 仍用槽位上的原子位，见下一节。

`FrameSerial` 标识一次帧录制，由 `GpuSystem` 实例的计数器在 `BeginFrameRecord` 中从 1
递增分配，同时标识槽位上保活资源所属的帧。非零表示该帧尚未由 GT 清理；
GT 在 `ReleaseFrameResourcesGT` 匹配真实完成、释放 Payloads 后清零，随后槽位才可复用。
空帧同样要完成此步骤。`AppFrameContext` 和完成消息各自保存分配时的编号，不受清零影响。
编号只在同一 `GpuSystem` 生命周期内唯一，不同实例可以重复，不作为进程级身份。
`FlightIndex` 则是循环复用的槽位索引。

`_flights` 是 `vector<unique_ptr<FlightSlot>>` 而不是 `vector<FlightSlot>`：`FlightSlot`
内含 `ManualCoroutineScheduler`，它不可拷贝也不可移动——挂起的协程记录里存着回指调度器的
指针（stop callback），搬动槽位会让那些指针指向旧地址。数量在构造时定下且此后不变，
故间接一层不带来任何代价。

## 帧边界等待

`IWaitFrameProcessor::Wait()` 提供 GT 通知，等待表属于当前 flight。CompleteFlight 只发布原子完成事实；
GT PumpWaitFrame 通过 ManualCoroutineScheduler 冻结已就绪等待记录，再按地址和序号验证后恢复。
正常只泵当前 writable flight；终止排空先捕获每个 flight 等待表的序号截止，再处理全部 flight，
前一个 flight 的回调为后一个 flight 新建的等待不会混入本批。
Wait 以调用时的当前 flight 为保守覆盖点，可能多等一轮；取消经停止通道结束任务，不继续执行正常完成续体。
取消不是 GPU 完成证据，不能用普通可取消协程的捕获变量作为唯一在途资源 owner。

### 不可取消的框架 owner

GT 拥有 writable flight 时调用 `GpuSystem::RetainForFrameGT(flight, payload)`，payload 可为资产 ref 或 raw RHI owner。
记录直接保存在对应 `GpuFlightSlot::Payloads` 中，与录制共用槽位的 FrameSerial。
真实完成的 serial 与槽位 FrameSerial 和 CompletedFrameSerial 原子值都匹配后，
S0 ReleaseFrameResourcesGT 才在 GT 析构 payload。即使通知被取消或 GpuWorkCompleted=false，记录也不能提前释放。

没有对应提交的 owner 只可在 RT 停止、GPU 排空后的 AbandonUnpublishedResourcesTerminalGT 释放，
之后拒绝新 retain。GpuSystem 析构发现尚存 owner 会诊断，不能借析构隐藏完成义务。
Scene 绑定使用自己的长期 owner 与最后解绑 flight，细节见[场景交付](render-framework.md#交付退出与资产保活)。
原有 TextureAsset 保守 DeferDestroy 保留到其外部使用者全部迁移，见[资产系统](asset-system.md)。

## 完成通知与帧等待恢复

retire 阶段观察到 fence 后发布两种通知：

| 路径 | 载体 | 消费与恢复 |
|---|---|---|
| 应用完成钩子 | `UnboundedChannel<FlightCompletion>` 的本地批次 | Application 在主线程逐条调用 `OnRenderFrameComplete` |
| 帧边界等待 | per-flight 原子 `WaitersCompleted` | `PumpWaitFrame` 只泵当前独占 flight |

普通帧顺序固定为：收集消息 → 清理匹配的 RenderSystem/GpuSystem owner → `PumpWaitFrame` → `OnRenderFrameComplete`。
先消费旧 WaitersCompleted，再调用应用钩子，避免钩子新建的 Wait 被误认为已经完成。
`PumpWaitFrame` 在恢复现有等待者前统一标记旧等待记录，恢复期间新建的等待同样不会提前完成。
上传专用的等待表、状态机和恢复路径已删除，帧等待协议保留。

当前 flight 取得可写槽位之前，它上一轮的 fence 完成消息和 WaitersCompleted 已发布。
ThreadedRunner 只在 `CompleteFlightIfReady` 成功后推进 GT 独占的退休游标，再服务帧边界和复用槽位。
回收与完成消息消费都位于 GT；独立驱动在批次期间新发布的消息仍留到下一次消费。

窗口维护由 runner 先排空 CPU 渲染工作，再调用 `GpuSystem::WaitAndRetireFlights` 等待主队列、
退休已提交 flight 并发布消息；GpuSystem 不再反向请求 WindowManager 等待 runner。
ThreadedRunner 在这次全量退休后直接推进退休游标，不再先逐 flight 等待一次 fence。
此 GT 路径同时消费 flight 完成标记，将现有帧等待记录标为完成，但不恢复协程、不消费 channel。
窗口操作续体随后新建的 `Wait()` 因而不会消费旧完成通知。恢复与应用完成钩子仍留给正常帧顶或关停阶段。

## 上传

三层，按"数据从哪来"选：

| 设施 | 用途 |
|---|---|
| `ResourceUploader` | 经 staging buffer 上传到 device-local 资源。`UploadBuffer` / `UploadTexture` / `UploadMeshResource` |
| `DynamicCBufferArena` | 帧内常量缓冲切片。从映射上传页线性子分配，按 flight reset |
| `HostWriteBatch` + `ScopedBufferMap` | 直接写持久映射缓冲，收集写入范围后统一 flush |

`StagingBufferPool` 按 flight 分池，`CollectFlight` 在 fence 完成后回收。
`MappedUploadPage` 持久映射，`Reservation` 是仅可移动的映射切片，提交时记录实际写入范围。

### 资产 GPU 上传待设计

专用帧上传协程、自动 upload phase 和上传命令前缀已删除，不提供替代上传调度器。
网格与纹理的内置 GPU 上传加载工厂同时移除；`MeshImporter`、`TextureImporter` 暂时保留
类型、扩展名和 settings 登记，加载时明确返回 `GPU upload is not implemented`。

TODO：待后续上层 GPU 调度设计确定后恢复资产上传，包括复制命令组织、barrier、提交依赖、
完成后的资产发布和取消期间资源寿命。本阶段不预设替代 API。

`ResourceUploader`、staging pool、动态常量缓冲和映射写入工具仍可显式使用。
`AppFrameContext::GetUploader` 提供当前录制 flight 的上传器，其 Begin/End/Collect 由 GpuSystem
按录制和 fence 生命周期驱动。复制命令的位置由应用录制方决定；现有低层 helper 的 barrier 行为
未在此次删除中修改。`IWaitFrameProcessor::Wait` 负责通知；已提交上传的目标 owner 使用不可取消的框架保活。

## 渲染资源的帧寿命

RenderSystem 通过私有 ShaderProgramCache 拥有 JIT、artifact/program cache；PSO 由调用方通过 RHI 创建和持有。
program 的 layout/参数 metadata 活过所有 flight，关停 GPU idle 后才销毁。

旧框架的自动 per-flight asset refs、pool、descriptor arena 和 history 已移除。应用录制方负责
让输入数据、原生资源与资产 owners 活到对应工作完成。StreamingAssetRef 的复制和释放仍只在 GT；
在 OnUpdate 的 writable 阶段以 RetainForFrameGT 交给框架；场景绑定由 RenderAssetLifetime 管理。
retire 阶段仅发布帧完成消息；应用完成钩子在 GT 消费消息时执行。

## 帧 profiler

`GpuFrameProfiler` 是定义和实现在 `gpu_resource.h/.cpp` 中的可选组件，
由 `GpuSystemDescriptor::EnableFrameProfiler` 控制创建。它对应 UE5 的 `FGPUTiming`（最小化）：
per-flight timestamp pool + readback。
`GpuSystem` 提交时从同一命令分配器取得普通 buffer，在应用批次前后分别录制开始 timestamp
和结束 timestamp / resolve，不保留独立的计时命令 buffer 成员，也不打断 D3D12 的
Submit → Present 配对。计时范围覆盖整段应用提交序列，可能包含组间 GPU 等待。
跳过应用工作的帧不录制计时，不发布旧 query 结果。`CompleteFlight` 时读取实际提交的
query 结果；应用只读 `GetLastGpuTimeMs()`，后端 readback barrier 差异内部隐藏。
最近一次 GPU 耗时和 frame latency 通过原子标量发布，允许 game thread 在另一 flight 回收时读取。

## 呈现

`AppFrameContext::AcquireWindow(window)` 内部 `AcquireNextSwapChainFrame`：
`RequireRecreate` / `RetryLater` / `Error` / 最小化 → `nullopt`（应用跳过该窗口）。
成功时返回仅可移动的 `AppFrameTarget`，包含 backbuffer、view、index，以及私有的
`SwapChainFrame` 和本轮录制的归属信息；flight 登记待归还目标，不创建 command buffer。
目标必须随本帧一个非空命令批次移动归还一次；成功 acquire 后 view 创建失败属于不可继续的
错误，不会返回空值并遗留 outstanding frame。

**`AcquireWindow` 不录任何 barrier。** 应用通过 `AllocateCommandBuffer` 显式选择录制资源，
根据 backbuffer 真实初态录制目标内容并收口到 Present，再将命令组和目标一起归还。
`GpuSystem` 在实际提交对应命令后更新 backbuffer 状态，完成提交与呈现；
窗口不可呈现的整帧跳过和 Vulkan 收尾规则见前述命令批次契约。
默认 Application::Render 为空，没有自动 acquire、清屏或离屏输出管理。

交换链尺寸变化时后备缓冲 view 会重建，此时必须调
`RenderPassRegistry::RemoveFramebuffersUsing(oldView)`：framebuffer 存的是 `TextureView`
裸指针。见 [render-rhi](render-rhi.md)。

## 关停顺序

`Application::Shutdown` 的顺序是固化的，每一步都有理由：

```text
runner 停止新帧、关闭窗口操作 → 消费并 join 已发布包
进入 Shutdown / Stopping：停止 World/Scene 新业务和 scheduler 新任务，请求加载生产者停止
WaitAndCleanupCompletedFlights：真实 GPU drain → 按 FrameSerial 发布完成 → S0 释放 owner
terminal abandon：仅释放未发布 Scene 包和 raw frame owner，不生成成功 completion
OnShutdown → 取消剩余通知 → WorldManager 显式 Shutdown
RenderSystem → AssetManager → CPU 等待器 → AssetDatabase → GpuSystem → WindowManager
```

加载任务与保守退休任务使用独立 scope；取消加载不会取消退休。GPU 模式下 AssetManager 的退休 scope 只在 GPU drain 后终止；CPU 模式的等待器无需等待 GPU。
普通窗口维护通过 WaitAndRetireFlights 等待 GPU 并发布完成，随后在帧边界释放 owner，
不 abandon writer 状态；abandon 后不能恢复正常发布。


关键约束：**`AssetManager` 必须在 `AssetDatabase` 之前销毁**（在飞 task 可能持有 importer），
且两者都必须在 `GpuSystem` 之前销毁（GPU 资源必须在 device 之前交出）。

channel 生命周期跟随 GpuSystem；关停期间保持可写，直到生产者停止且最终消息已消费后随宿主销毁。
无需调用 `Complete()`；若使用该操作，它只禁止新写入，积压消息仍能读完。

ThreadedRunner 在 join 前消费全部已发布 Scene batch，即使退出已请求，也只跳过应用绘制。
没有发布的 batch 及其退休引用在 RT 停止、GPU idle 后于 GT 清理。RenderScene::Apply 不表示 GPU 完成；CPU 增量交付沿用现有
ready 交接、fence 与 flight 退休语义。

`Application::Shutdown` 和析构共用 `StopAndDrainRuntime` 停止生产者、排空真实完成并释放未发布 owner，
随后取消通知并通过 `DestroyRuntime` 按固定顺序拆除。析构作为正常 `Shutdown` 被异常绕过时的保底；该路径
不调用派生类的 `OnShutdown`，但仍会 wait GPU、取消 scheduler、断开窗口引用并保持同一销毁顺序。

## 服务装配

Application 直接创建对象、连接借用引用并调用初始化函数；依赖关系和拆除顺序均由普通代码明确表达。
设备由 GpuSystem 创建，WindowManager 与 GpuSystem 同时启用时才建立双向引用。
可选资产来源、失败清理与所有权边界见
[Application 直接装配](render-framework.md#application-直接装配)。

## 游戏侧扩展点

底层负责"何时 tick、怎么 acquire/render/present"；游戏只负责"这个应用要画什么"。

| 钩子 | 时机 |
|---|---|
| `OnInit` | 所选系统就绪后一次。加载资产、Spawn Actor、建相机 |
| `OnRender` | GPU runner 开始录制后由 Render 调用；默认空，可覆盖以记录 RHI 命令 |
| `OnUpdate` | 每帧，`AssetManager::Pump` 之后、`World::Tick` 之前 |
| `OnRenderFrameComplete` | GPU 模式下 game thread 消费 `FlightCompletion`，包括跳过和 shutdown；允许释放 GT 资产引用 |
| `OnShutdown` | 关停，游戏侧清理 |

`Application::Update` / `Shutdown` 固化宿主时序；Render 只负责应用命令内容，不接管 runner 的提交协议。
