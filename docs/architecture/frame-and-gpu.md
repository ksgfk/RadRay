> - 适用: 改帧节奏、提交时序、GPU 上传；排查"GPU 对象被提前销毁"或帧同步问题
> - 权威: 本文是帧节奏与 GPU 资源上传的唯一说明。资产侧的使用者保活契约见 [asset-system](asset-system.md)；RHI 本身见 [render-rhi](render-rhi.md)
> - 锚点: `modules/runtime/include/radray/runtime/frame_timeline.h`, `modules/runtime/src/frame_timeline.cpp`, `modules/runtime/include/radray/runtime/gpu_system.h`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/include/radray/runtime/vertex_data.h`, `modules/runtime/include/radray/runtime/wait_frame.h`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/gpu_system.cpp`, `modules/runtime/src/application_loop.h`, `modules/runtime/src/application_loop.cpp`, `modules/window/src/win32/win32_window.cpp`

# 帧节奏与 GPU 上传

## 职责边界

| 系统 | 负责 | 不负责 |
|---|---|---|
| `FrameTimeline` | 游戏帧号、FrameSerial、完成通道、每 flight 等待表、帧延迟。有无 GPU 都在 | 设备、命令提交、画什么 |
| `GpuSystem` | **何时画**。instance/factory/device/主队列/fence、flight 槽位、上传器、帧 profiler。fence 完成后调用 `FrameTimeline::PublishCompletion` | 帧号、等待表、完成通道、画什么 |
| `SceneManager` | 多个持久 CPU RenderScene、按需对象 GPU 镜像、per-flight 场景操作包与视图、资产常驻与退休 | 底层 shader 缓存与 GPU 提交时序 |
| `RenderSystem` | program/artifact cache、GraphicsPipelineCache | 场景、World、Pipeline 与 GPU 提交时序 |
| `WindowManager` | 窗口创建/销毁、swapchain acquire/present/recreate、事件分发 | — |
| `Application` / 内部 `ApplicationLoop` | 固化帧序与关停顺序；协调原生消息和 CPU/GPU 进度；消费 FrameTimeline 的完成通道；游戏侧的窄扩展点 | RHI 内部消息泵 |

## 资源寿命与回收条件

寿命描述的是具体资源版本或分配，而不是常驻容器。逻辑释放停止新使用；退休证明旧使用已经结束；
回收才销毁对象、归还范围或允许下一次借出。引用计数、generation 与 FrameSerial 各有职责，均不能独自代替 GPU 完成和 barrier。

| 寿命 | 当前 owner 与示例 | 允许回收的条件 |
|---|---|---|
| 持久版本 | StaticMesh/TextureAsset、MaterialGpuState、SceneGpuData 的每 flight 镜像 | 逻辑 owner 不再使用，且所有已经接纳的录制和 GPU 使用结束 |
| 帧内分配 | GpuFrameResources 的 staging、常量/storage 切片、临时参数集与 texture lease | 整个 flight 的 final fence 完成、旧 command epoch 结束，旧参数/视图借用已清理 |
| 跨帧保留的结果版本 | 调用方的 history/readback 过程 owner | 最后 producer/reader 的 GPU 使用结束，并且 CPU 消费或历史保留权结束 |

池保留物理存储不等于上一逻辑版本仍有效；新 texture lease 的内容有效性必须重新建立。
持久资产进入退休阶段仍是原来的资源版本，不因等待 fence 就变成另一种 owner。
当前跨帧过程使用显式 owner 与已有完成机制，没有通用 history/readback 管理服务。

提交前先确认所有命令已接纳或丢弃、acquire token 有合法终结路径、owner 已登记、主机写入已提交，
再建立有序提交和 final signal。完成时先核验真实 signal 与身份、Reset 覆盖的全部 command epoch，
处理 profiler 结果和帧资源，再发布完成，由 GT 释放语义 owner。取消通知不改变这两个安全边界。
RHI 独立调用方须自行提供同等证明；一个宿主的 serial/fence 不覆盖另一个宿主的使用。

## 对象参数上传与必要工作

每个 SceneGpuData 仍拥有 F 份独立的物理 buffer。Scene Apply 的最终变化登记到各 flight 的去重 Pending，删除按完整 ShapeId 移除；首次初始化和扩容枚举存活行，暖机后只访问 Pending。对象与各实例参数流独立接纳，不要求整 Scene 的上传事务。

PipelineContext 的 IRequiredGpuWork 接纳关闭后的资源更新命令；AppFrameContext 实现此窄接口。更新必须在使用它的绘制前登记为无输出目标的 batch，登记成功以后才清 Pending 并推进 Initialized。暂存或目的资源分配失败保留该流的 dirty，下一次准备读取最新值；无成功更新的首次/扩容版本不能供绘制读取。同一 FrameSerial 最多准备一次，多个 view 共享相同绑定。

本地窗口与外部表面采用相同输出政策：提交前发现目标不可呈现时仅丢弃该目标的 batch，必要资源更新和独立离屏 batch 继续执行。因此 Scene 不再维护 Attempt、BufferGeneration 的执行反馈及补传队列。可取消等待只负责通知，不撤回已经接纳的命令。

相邻且无目标、无外部 wait/signal 的必要或离屏 batch 保持命令顺序，可合为一次 native Submit；profiler 首尾也参与连续无条件命令的合并。遇到外部同步或 acquired target 必须先提交前面的工作，不能跨越这些边界合并；Vulkan 在 Present 前提交所有应用工作，最终 signal 位于呈现收尾之后。

Scene 删除立即释放 CPU Scene，并将 GPU owner 转入删除批次的 RetiredGpu。有序主队列的真实完成覆盖之前的使用。退休某个较晚 flight 时，GpuSystem 先按 queue signal 顺序结束它覆盖的全部旧 command epoch，再发布完成；原生缓存引用和参数旧借用记录先清理，SceneManager/GT payload 后释放。

TryUploadBufferRanges 先校验并暂存完整的单流范围，再录制一对目标 barrier 及复制；失败不录制部分目的更新。既有非 Try 上传仍保留失败即诊断的契约。

## GpuSystem 的线程约定

独立宿主可用 `GpuSystem::TryAttachDevice` 共享已创建的 RHI device，保留独立的 flight、上传器、
主队列 fence 与 FrameTimeline 完成通道，不再创建 instance/factory/device。attachment descriptor
要求非空 shared device 和与 timeline 一致的非零 flight 数；主 direct queue 不存在时明确失败。
原 device owner 的 factory/environment 必须存活到 attached host 销毁之后。共享队列的 submit、
retire 和复用由调用方串行化；device 的 shared ownership 不提供这些操作的并发同步。
attached host 不关闭外部 Vulkan environment，也不取得外部窗口或主循环的所有权。

外部逻辑 producer 和 RT 可以在同一物理线程依次运行：BeginUpdateForFlight 在 GT 开域并取得 serial，
SceneWriter 修改完成后 Seal/Publish，再 Consume/Prepare/Record/Submit；使用资产的封闭 flight
在真实覆盖 fence 完成之前保活，Scene 删除、更新的 scene generation 或新的逻辑世界不会提前释放
旧 flight 的 owner。完成消息仍须先交给 SceneManager 和 GpuSystem，再复用对应 flight。
无需构造 Application、World 或 Actor 即可使用这组公共接口。

`AppFrameContext::AdoptSwapChainFrame` 接收外部 SwapChain 的真实 acquire token；表面、设备和回调
由调用方拥有：surface/device 存活到最终 GPU signal 完成，回调至少存活到 SubmitFrame 返回。
期间不得 recreate；token 必须属于传入的 SwapChain，PresentQueue 必须是宿主的主队列。
外部目标的 backbuffer 访问独占一个非空 batch，runtime 统一执行同步、Present 和最终主队列 fence。
IsPresentable 回调在提交时重新检查输出；目标不可呈现时，跳过其 backbuffer batch，独立的资源和
离屏 batch 仍执行，必要更新和离屏工作仍由最终完成覆盖。
外部宿主消费真实 completion 后先 ReleaseFrameResourcesGT，再泵 FrameTimeline/AssetManager 的等待器；
不能用 CPU Return、Present 返回或中间 batch fence 代替最后的 flight signal。

`ResourceUploader::TryUploadTextureRegions` 复用 staging pool 与 RHI region copy，先校验、暂存整批，再录制
目标修改。支持单采样的非压缩 2D/2DArray color 格式，检查 mip/layer、offset/extent、row pitch、整数溢出
和源长度；失败不录制部分目标修改。每条 barrier 只作用于该 mip/layer；调用方提供真实 Before/After。
SrcData 仅借用到暂存完成，staging 随真实 flight 完成回收。必须执行的资源更新不得放进可跳过的 backbuffer batch。

`gpu_system.h` 在可调用的 `GpuSystem` 函数声明上标记线程与阶段，私有函数也遵循同一约定：

- **GT** 是 Application 所在线程，拥有 `FrameTimeline` 上的游戏帧号与帧等待表。协程的启动、恢复和
  取消都必须在 GT；`FrameTimeline::GetFrameIndex` / `GetCurrentFlightIndex` 读取非原子状态，不能作为 RT 的帧号来源。
- **RT** 是当前 flight 的录制线程。GT 完成 Update 并交出槽位后，RT 独占录制与提交。
  单线程模式中 RT 与 GT 是同一线程。应用显式调用 uploader 的录制也发生在 RT。
- **GT/RT，retire 阶段** 表示两种线程都可能回收 flight，不表示函数内部提供互斥。
  ApplicationLoop 的单/双线程路径都由 GT 独占 retire；RT 完成 Submit/Present、最终 Signal 写入和录制收尾后，以 release 发布 CPU 完成计数，
  GT acquire 该计数后才读取对应 flight 的 fence。槽位通过 ready 信号量交给 RT，通过真实 fence 完成回到 GT。
  独立驱动仍可在 GT 或 RT retire，但须自行提供相同的独占和交接前提；channel 的锁只保护消息，不保护 flight 状态。
- **任意线程** 仅适用于读取固定配置、稳定对象指针或原子统计。对象必须已完成构造/装配发布，
  且尚未开始拆除；取得指针不会改变 device、queue、WindowManager 自身的线程约束。

生命周期操作仍归 GT：装配与拆除必须隔离其他读者，销毁前先停止生产者、等待 render/GPU idle，
并消费完成消息。`GpuSystem::WaitAndRetireFlights` 自己等待 idle；`FrameTimeline::CleanupCompletedFlights`
与 GpuSystem 析构则要求调用前已经 idle、完成消息已经消费。逐函数的具体前提以头文件中的 API 契约为准。

## 帧序

```text
ApplicationLoop::PrepareFrame：非阻塞维护与 writable 检查；未就绪返回消息循环
S0（本帧只执行一次）
  Application::ServiceFrameBoundaryGT
    完成消息批次 → 释放 SceneManager/GpuSystem owner → 帧等待通知
    → OnRenderFrameComplete → AssetManager::Pump → ApplicationScheduler::Pump
BeginFrameTiming → PumpNativeEvents（每次最多 256 条）→ 冻结并派发游戏事件 → OnUpdate → WorldManager::Tick（统一 epoch）
S1 Application::FinalizeWorldAndSealGT
  FinalizeWorldsGT → CollectRenderUpdates → OnCollectRenderViews → SealFrameGT → PublishFrameGT → ready
RT BeginFrameRecord → S2 ApplySceneUpdatesRT → 可选 OnRender
  EndFrameRecordAndSubmit：检查关闭/接纳/丢弃 → FrameResources.SealEpoch → HostWrites.Flush
  → 有序 Submit/Present → 最终主队列 fence
```


单线程与双线程普通循环都遵循上述顺序。资源边界是当前 flight 上一轮的 fence 已完成、
ApplicationLoop 已取得槽位使用权；逻辑帧边界是完成批次与应用完成钩子处理结束后的计时点。
等待可写槽和处理完成批次放在事件派发之前，让紧接着的 Update 使用这些阶段期间到达的输入。
完成批次处理有明确终点，不会为回调新建的任务或稍后到达的 GPU 消息反复排空整个系统。

未启用 Gpu 时，同一 ApplicationLoop 在 GT 同步消费场景，使用同一时间线。S0 消费完成并服务等待，BeginFrameTiming 在 Update 前保留身份；Update 后 Scene Publish/Consume 使用此 serial，然后发布 OutputsExecuted=false 的 CPU 完成。该模式没有 AppFrameContext 或 OnRender，但仍调用 OnRenderFrameComplete。发布前退出的域在 terminal 停止时放弃，不伪造 GPU 完成；已经发布的通知在 Shutdown 排空。

`DeltaTime` 是相邻逻辑帧开始时间之差，仍包含两个开始点之间的槽位等待与收尾耗时。首帧从 OnInit 返回后的循环入口计时，不包含初始化耗时。
`LastFrameLatency` 从同一个逻辑帧的 `BeginFrameTiming` 计算到 `PublishCompletion`。GPU 路径在观察到该 flight fence 完成时发布，包含事件派发、Update、录制、排队与 GPU 执行，不包含该帧开始前的槽位等待和完成回调。CPU 路径在同一帧场景消费之后发布，因此是该帧开始到完成发布的真实间隔。
手动驱动 GpuSystem 时，在完成调度之后、开始本帧工作之前调用 `GpuSystem::BeginFrameTiming`；它要求当前槽位没有未完成的 fence，再转给 FrameTimeline。

FrameSerial 在 GT 开域时由 FrameTimeline::OpenFrameDomain 从 1 递增保留，RT BeginFrameRecord 只使用同一身份。非零 serial 不能被循环槽位索引替代。独立单线程驱动的 BeginFrameRecord 可以补开空域；分离 RT 必须先在 GT BeginUpdateForFlight。

GpuSystem::RetireSubmittedFlights 只接受 GT acquire 确认 CPU 交付的连续提交前缀，每项包含 FlightIndex/FrameSerial；仅访问给定范围，按序退休真实 GPU 完成的前缀并返回数量。不得传入尚由 RT 录制/提交或已经复用的槽位。FrameSerial 核验身份，同一主队列的 final fence 值核验提交顺序；独立宿主提前保留其他槽位的身份，不要求 serial 按提交顺序递增。每个退休先结束覆盖的 command epoch，resolve profiler、CompleteEpoch 并记录真实完成身份，然后发布 FlightCompletion。FrameTimeline 持有预分配的 F 个通知槽；mutex 保护队列与开域身份，每 flight 至多一条未消费终结通知，重复、外来身份和溢出立即诊断。完成账本的 CompletedSerial 同时用于等待恢复，不再转存等待记录的布尔完成标记。
ApplicationLoop 在 GT 帧首用 `RetireReadyFrames` 检查这个前缀；有空槽就推进下一帧，只对需要复用的最旧槽位形成背压。
CPU 交付、唤醒、超时、Present 返回都不代替 GPU fence 完成。RT 不回收 flight，staging 回收与 profiler resolve 发生在 GT。
独立宿主的 `CompleteFlight` / `CompleteFlightIfReady` 会读取其他槽位，调用时必须隔离**全部**提交生产者及 retire/复用；
`WaitAndRetireFlights` 继续提供生产者静止后的同步排空入口。`WaitMainQueueIdle` 只做主队列等待，不退休或调用游戏代码。

`Application` 是完成通道的唯一消费者。帧顶取得可写槽位后，`Application::ServiceFrameBoundaryGT`
通过 `FrameTimeline::TryReadCompletion` 非阻塞收集一个本地批次，
先对全部完成消息调用可选的 SceneManager::OnFlightCompletedGT 和 GpuSystem::ReleaseFrameResourcesGT，
验证 serial 并释放框架 owner，再 `PumpWaitFrame`；有 GPU 时 `BeginUpdateForFlight` 为新域保留身份并开放 GT 保留阶段。
关停排空不指定 flight，改为 `CleanupCompletedFlights`。最后通知应用。
不得先复用槽位再校验旧 owner。各通知服务在入口冻结本批工作，回调新建的同类通知留到下一 S0；
等待者以地址和不复用序号联合验证，前一个回调取消后一个 waiter 不会产生悬垂派发。
完成消息保留 FrameSerial，不能仅用可复用的 FlightIndex 识别一帧。
OutputsExecuted 仅描述该帧可选输出是否全部执行；false 仍有真实完成，必要更新可能已经执行。历史有效性由对应 producer 的接纳与完成确定。
CPU 路径的完成进入同一通道，但 OutputsExecuted 恒为 false，没有 fence。
通知不代表画面已显示到屏幕。

`Application::PumpFlightCompletions` 包含 GT 线程断言与覆盖内部调度、应用钩子的重入保护。
批次收集期间不执行用户代码；处理批次期间新发布的消息留到下一次消费。
channel 不拥有回调或资产引用，没有观察者注册、注销与跨线程调用应用的接口。

多线程普通帧只等待当前 flight 可写，不等待上一帧 CPU record 结束，允许 `Update(n+1)` 与
`Record(n)` 重叠。GT 以已发布帧数减已退休帧数判断槽位容量，不维护第二份 writable 信号量。
需要复用旧槽位时，先确认该槽位已经提交，再等待其真实 fence；不等待更新的 `Record` 结束。
同一 flight 仍必须等 fence，其他槽位的 Update/Record 可继续重叠。
关闭与模态丢帧仍提交真实 fence，但跳过 OnRender，完成通知的 `OutputsExecuted=false`，
不能据此提交图像历史。当前不再录制或提交自动的资产上传前缀。

### 命令分配与有序批次

`GpuFlightSlot::CommandAllocator` 实现窄接口 `IGpuCmdAllocator`，拥有当前 flight/main queue 的 RHI
CommandAllocator 分组池；每组持有一个 allocator、独占 command wrappers、本轮分配 cursor 与打开录制数量。
初始化创建一组；按 `GetBackend()`，D3D12 选择第一个没有打开录制的组，全部占用时新增一组；Vulkan 始终使用第一组。
`Allocate()` 从选中组 Begin 一个本 epoch 尚未使用的 wrapper，不足时创建；wrapper 固定绑定所属组的 allocator。
`Return(span)` 只 End 并关闭借用，不登记、不提交、不重用 wrapper，也不决定执行顺序。
`RegisterClosedCommandBuffers(desc, target)` 登记已归还命令，复制数组，批次按登记顺序、项内按数组顺序执行。
同一个执行用 command 必须归还并登记一次；未接受的 Recording/Closed command 可 Discard，之后本 epoch 不再借出。
Return 或 Discard 仅在结束仍打开的录制时释放组占用。批量操作先跨组检查全部命令，再修改状态；封口拒绝未处理命令。
结果数组可以与 Allocate、Return 的顺序不同。

旧 `AppFrameContext::AllocateCommandBuffer/ReturnCommandBuffers` 保留；后者是 Return + Register 的适配入口。
内部 profiler 与 late-drop cleanup 通过同一分组池的私有续录入口追加命令；公开录制封口后不能再调用。
所有提交及内部续录仍由原 final flight fence 覆盖。观察到真实完成后，RetireFlight 逐组 Reset 本轮用过的 storage、清零 cursor，
清理全部 child 临时状态，清除参数旧借用并重置参数/常量 cursor，然后发布 completion。
GT 消费完成并释放 owner 后才允许下一次 BeginFrameRecord；Begin 不再二次 Reset，Allocate/Return/Submit 也不 Reset。
终止清理会先释放借用 program layout 的 GpuFrameResources，再拆 RenderSystem caches。
Reset 不新增 GPU 等待；D3D12 组数按同时 open 数的高水位保留，Vulkan 每 flight 一个 pool。
每组 wrapper 容量按该组使用高水位保留，全部 children 先于组内 allocator 销毁。

应用 wait/signal 作用于当前批次，允许无命令、无目标的纯同步批次。`WaitToExecute` 和
`ReadyToPresent` 必须为空，交换链同步由 runtime 根据可选呈现目标注入。

一个批次最多关联一个 `AppFrameTarget`，包含该 acquire 的 backbuffer 的全部访问及收口到
Present 的 barrier；普通批次不得访问本帧 acquire 的 backbuffer。应用自己决定录制落点，
runtime 不提供按 texture 查找命令的路由。跨批次的资源依赖仍须应用显式录制 barrier。

D3D12 对每个带目标的批次保持 `Submit → Present`，然后才处理下一批次，单窗口与多窗口
遵循相同规则。Vulkan 保留目标及外部同步边界，先提交全部应用工作，再按登记顺序 Present；连续无条件命令按上文规则合并。
每次 Submit 都追加递增的内部主队列 fence signal，并放在 signal 数组最后；Vulkan 因此把
acquire semaphore 的回收关联到 runtime 持有的 fence。最终收尾 Submit 的 fence 值才是
`flight.Signal`，中间批次完成不能退休 flight。空帧也提交真实完成 fence。

`SubmitFrame()` 封口并执行已经归还的工作。封口时未归还或未登记命令、未归还成功 acquire 的目标、
重复归还、跨 flight/跨录制使用、手填交换链同步或无效 fence 数组均属于不变量错误。
显式提交后禁止继续分配、acquire 或归还；runner 的自动收尾对已提交帧不再重复 Submit。

AcquireWindow 与提交前检查保留实时的最小化、隐藏和客户区判断。不可呈现只丢弃对应目标 batch，保持必要和独立 batch 的顺序、wait/signal 与最终 fence。D3D12 由 Present 消费 token；Vulkan 消费 acquire semaphore、signal present semaphore 并 Present，必要时单独录制 Present 状态收尾。只发布实际执行的 backbuffer 状态。成功 acquire 不能通过析构取消，原始平台 API 不能绕过 runtime 在并发录制时修改窗口。

普通循环与窗口拖动/菜单 tick 共用同一非阻塞推进协议。已准备帧用唯一游戏帧号标识，
在原生消息采集期间可由模态 tick 消费；外层只执行自己持有且仍有效的身份，不能重复 S0、计时、输入、Update 或发布。
OnInit 返回并进入驱动循环前，模态入口不能提前推进帧。准备、完成回调、普通事件派发、Update、录制、维护修改和结果交付期间禁止重入。模态入口未就绪立即返回，
不等待 CPU 或 GPU。每个模态帧准备后仍再次采集消息，完成回调投递的消息可进入紧接着的 Update。
双线程丢弃过期非模态绘制但仍消费已发布场景包；最多保留一个尚未 CPU 交付的模态帧。
Win32 在系统移动、缩放和菜单循环内通过窗口的 WM_TIMER 触发 tick，单/双线程使用相同入口，
不创建 VBlank 辅助线程或隐藏消息窗口。定时器使用 USER_TIMER_MINIMUM（10 ms），WM_TIMER 优先级低；
此入口用于维持模态期间的帧推进，不承诺按显示器刷新率稳定出帧，持续高优先级消息可能推迟 tick。
最终宿主拆除必须回到外层循环，不能发生在原生消息或模态回调栈内。

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
ApplicationLoop 在尚未开启下一逻辑帧时截取操作序号上界和最后已发布帧号，停止发布新帧，
非阻塞等待该帧的 CPU 交付。生产者静止后启动临时排空线程，先等待已提交 final fence，再执行一次主队列 `Wait()`。
该线程只等待，不 retire、释放资源或派发回调；整个排空期间禁止其他队列提交、Present 和队列相关资源修改。
GT 继续采集原生消息，观察线程实际退出后才 join，随后 GT 退休 flight 并执行冻结批次。普通帧不创建排空线程。
最终 flight signal 位于 Present 调用之后，但不证明显示引擎停止使用图像或 present semaphore；
也不能覆盖其他独立队列尾部工作。交换链维护仍走统一的队列排空和平台同步路径。

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

Application 创建 WindowManager 时显式启用 `NativeEventDelivery::Deferred`。独立 NativeWindow/WindowManager 默认
`Immediate`，原生回调时机保持不变。托管 manager 的公共 `DispatchEvents` 拒绝越过 Application 帧边界自行派发。
Cocoa 继续即时模式，工厂明确拒绝尚未实现的延迟配置；Application 窗口模式目前只支持 Win32。

WndProc 立即完成系统回复、原生状态更新和文本解码；键鼠、重复键、UTF-8 文本、滚轮、焦点、捕获丢失、移动、
尺寸、显示变化与关闭通知保存到可增长 FIFO，不覆盖旧输入或额外合并事件。文本由队列拥有，回调的 string_view
只在当前 signal 发射期间有效。事件绑定不复用的窗口注册身份；注销或销毁取消该身份的积压事件。
每批在入口冻结截止位置，回调产生的事件留到下一批；失焦与捕获丢失按原生接收顺序交付，客户端负责清理输入状态。
框架保证已经接收事件的顺序与完整性，不恢复操作系统已经合并的鼠标移动。`EventBeforeSurfaceChange` 仍是同步契约检查，
模态 tick 仍是框架控制入口，两者不进入普通事件 FIFO。

CPU 交付、排空线程返回和 RequestExit 发出进度唤醒。Win32 使用 `MsgWaitForMultipleObjectsEx` 同时等待进度事件、
实际线程退出句柄与 `QS_ALLINPUT`；通知可以提前、合并或虚假到达，始终重新检查状态。每次原生采集最多 256 条，
随后检查帧进度。GPU 背压在外层使用 `Fence::WaitFor(value, 1)` 有限片段，每片后重新采集消息；30 秒截止绑定同一帧，
消息或模态 tick 不会重置它。准备未完成前不派发普通游戏事件或 Update。
该边界遵循 [DXGI 消息泵要求](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/dxgi-best-practices#multithreading-and-dxgi)
及 [Vulkan Win32 WSI 契约](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html#_win32_platform)；
同步 OnRender、Acquire/Present 或驱动调用本身仍可能耗时，RHI 不承担窗口消息服务。

NativeWindow 的信号仍是应用输入入口；runtime 不提供统一输入状态或应用/UI 路由。收到退出请求后取消积压输入，
停止普通派发并持续服务原生消息。Win32 消息泵记住 WM_QUIT，WindowManager 将它纳入退出条件。真实主窗口关闭请求在安全关停点通知一次，再调用 OnShutdown；RequestExit 或 WM_QUIT 不伪造关闭事件。

通用窗口能力通过 `NativeDesktopCapabilities` 查询：光标形状与显隐、桌面鼠标定位、显示器
工作区和 DPI、UTF-8 clipboard、IME 候选框定位。Win32 实现这些能力及 DPI/display/capture
消息；Cocoa 的新增桌面接口明确返回能力缺失，不伪装成功。Cocoa 滚轮继续发送原始滚轮事件，
同时提供新双轴浮点事件；这里不增加 macOS runtime。

## Flight 槽位

`GpuFlightSlot` 按所有权/阶段分组，跨阶段的访问时序由 ready 信号量、CPU 完成计数与 GPU fence 保证：

| 组 | 成员 | 谁访问 |
|---|---|---|
| 录制态 | `CommandAllocator`, `FrameResources`, `Batches`, `Acquisitions`, `Recording` | GT 帧顶重置 HostWrites；RT 接管后录制，应用显式上传的 staging 在 fence 后回收 |
| 保活态 | `Payloads`, `GpuPayloads`, `FrameSerial` | GT 开域、登记/释放 payload；RT 使用已保留编号，GT 匹配真实完成后清零 |
| 提交态 | `Signal` | RT 在 `EndFrameRecordAndSubmit` 写；GT acquire 确认 CPU 交付后在 `RetireSubmittedFlights` 读，真实退休时清空 |

`Recording` 从 BeginFrameRecord 到提交封口前为 true；AppFrameContext 同时校验保存的 FrameSerial。
封口后禁止继续录制，runner 的 EndFrameRecordAndSubmit 可重复收尾，不另存 Submitted 标志。

完成消息、帧开始时间和等待表都不挂在槽位上，它们属于 `FrameTimeline`。完成通道由 retire 阶段的 `PublishCompletion` 写入、Application 在 GT 消费。

FrameSerial 在 GT OpenFrameDomain/BeginUpdateForFlight 保留，同时标识当前更新、录制和全部保活资源。GT ReleaseFrameResourcesGT 验证槽位身份及 FrameTimeline::IsCompleted 后释放 owner 并清零，随后才能复用。RT 不生成另一份身份。
空帧同样要完成此步骤。`AppFrameContext` 和完成消息各自保存分配时的编号，不受清零影响。
编号只在同一 `FrameTimeline` 生命周期内唯一，不同实例可以重复，不作为进程级身份。
`FlightIndex` 则是循环复用的槽位索引。

`_flights` 是 `vector<unique_ptr<FlightSlot>>`：槽位及其不可移动的帧内分配器地址在录制期间保持稳定。
完成原子量与帧等待表都在 FrameTimeline，不在 GPU 槽位里。数量在构造时定下且此后不变。

## 帧边界等待

`IWaitFrameProcessor::Wait()` 由 `FrameTimeline` 实现，GpuSystem 不再提供该接口。等待表属于调用时的当前 flight。
`PublishCompletion` 只发布原子完成事实，GPU 的 RetireFlight 与 CPU-only 路径都走这里；
GT PumpWaitFrame 通过 ManualCoroutineScheduler 冻结已就绪等待记录，再按地址和序号验证后恢复。
等待表是 `vector<unique_ptr<Flight>>`：调度器不可移动，挂起记录的 stop callback 回指调度器。
正常只泵当前 writable flight；终止排空先捕获每个 flight 等待表的序号截止，再处理全部 flight，
前一个 flight 的回调为后一个 flight 新建的等待不会混入本批。
Wait 以调用时的当前 flight 为保守覆盖点，可能多等一轮；取消经停止通道结束任务，不继续执行正常完成续体。
取消不是 GPU 完成证据，不能用普通可取消协程的捕获变量作为唯一在途资源 owner。

### 不可取消的框架 owner

GT 拥有 writable flight 时调用 `GpuSystem::RetainForFrameGT(flight, payload)`，payload 可为资产 ref 或 raw RHI owner。
记录直接保存在对应 `GpuFlightSlot::Payloads` 中，与录制共用槽位的 FrameSerial。
真实完成的 serial 与槽位 FrameSerial 匹配，且 FrameTimeline 的 CompletedSerial 已覆盖该身份后，
S0 ReleaseFrameResourcesGT 才在 GT 析构 payload。即使通知被取消或 OutputsExecuted=false，记录也不能提前释放。

没有对应提交的 owner 只可在 RT 停止、GPU 排空后的 AbandonUnpublishedResourcesTerminalGT 释放，
之后拒绝新 retain。GpuSystem 析构发现尚存 owner 会诊断，不能借析构隐藏完成义务。
Scene 绑定使用自己的长期 owner 与最后解绑 flight，细节见[场景交付](render-framework.md#交付退出与资产保活)。
TextureAsset 在所有使用者完成后直接卸载，使用者协议见[资产系统](asset-system.md)。

## 完成通知与帧等待恢复

PublishCompletion 的同一 CompletedSerial 账本负责完成与等待。WaitFrameRecord 保存 FlightIndex 和 TargetSerial；登记时取得当前开放或下一域的身份。PumpWaitFrame 在固定批次截止内仅恢复已覆盖的目标身份与取消记录，恢复期间产生的新等待不会消费旧域完成。CleanupCompletedFlights 冻结所有槽位的入口截止，保留停止、取消其他等待者和重入保护。

普通边界：TryReadCompletion 收集固定批次 → 对全部消息释放 SceneManager/GpuSystem owner → PumpWaitFrame → BeginUpdateForFlight 开域 → OnRenderFrameComplete。窗口维护先停止发布、等 CPU 提交与真实主队列完成；完成账本允许窗口续体直接登记新的身份，不需要中间布尔标记。独立宿主必须消费通知并释放对应 owner 后复用槽位。

取消不是完成凭据。GT 的 RetainForFrameGT 和 RT 的 RetainGpuForFrameRT 包由框架保留到真实 epoch 结束；RT payload 只允许 GPU owner，不得包含任何嵌套的非原子 StreamingAssetRef。未发布包只在停止生产者、drain 并 terminal abandon 后释放。

## 上传

三层，按"数据从哪来"选：

| 设施 | 用途 |
|---|---|
| `ResourceUploader` | 经 staging buffer 上传到 device-local 资源。`UploadBuffer` / `UploadTexture` / `UploadMeshResource`。CPU 网格输入是 `vertex_data.h` 的 `MeshResource` |
| `DynamicCBufferArena` | 帧内常量缓冲切片。从映射上传页线性子分配，按 flight reset |
| `HostWriteBatch` + `ScopedBufferMap` | 直接写持久映射缓冲，收集写入范围后统一 flush |

GpuFrameResources 每 flight 拥有一份 HostWriteBatch、单 epoch StagingBufferPool、借用它的 ResourceUploader、constants/storage 页与参数池。SealEpoch 要求所有 reservation 已 Commit，包括 Commit(0)；完成后先清参数旧借用，再归还 active staging。Uploader 不再内嵌 F 个 pending 容器。
`MappedUploadPage` 持久映射，`Reservation` 是仅可移动的映射切片，提交时记录实际写入范围。
`HostWriteBatch::Flush` 在 runtime 按设备后端处理收集的范围：实现文件条件引入 Vulkan 后端头，
原生 Vulkan 设备和 buffer 直接收集非 coherent allocation，并通过一次 `vmaFlushAllocations`
批量刷新，由 VMA 处理 atom 对齐；RHI 校验或自定义包装对象保留逐范围 `Buffer::FlushMappedRange`
路径，不强转或绕过包装；D3D12 跳过刷新。
范围记录及相邻合并属于 runtime，公共 RHI 不提供设备级批量刷新接口。

### 资产 GPU 上传待设计

专用帧上传协程、自动 upload phase 和上传命令前缀已删除，不提供替代上传调度器。
网格与纹理的内置 GPU 上传加载工厂同时移除；`MeshImporter`、`TextureImporter` 暂时保留
类型、扩展名和 settings 登记，加载时明确返回 `GPU upload is not implemented`。

TODO：待后续上层 GPU 调度设计确定后恢复资产上传，包括复制命令组织、barrier、提交依赖、
完成后的资产发布和取消期间资源寿命。本阶段不预设替代 API。

`ResourceUploader`、staging pool、动态常量缓冲和映射写入工具仍可显式使用。
`AppFrameContext::GetUploader` 提供当前录制 flight 的上传器，epoch 由 GpuFrameResources 按录制和真实完成驱动。复制命令的位置由应用录制方决定；现有低层 helper 的 barrier 行为
未在此次删除中修改。`IWaitFrameProcessor::Wait` 负责通知；已提交上传的目标 owner 使用不可取消的框架保活。

## 渲染资源的帧寿命

RenderSystem 通过私有 ShaderProgramCache 拥有 JIT、artifact/program cache，并提供通用 GraphicsPipelineCache；调用方也可直接通过 RHI 创建和持有 PSO。
program 的 layout/参数 metadata 活过所有 flight，关停 GPU idle 后才销毁。

帧常量、storage、参数版本仅借用至当前工作域完成。开启 [RHI 校验](render-rhi.md#可选-rhi-校验)时，参数集第一次 command binding 后由包装层封存，后续 Set 被拒绝；安全 ResetBindingsForReuse 清理 CPU 旧借用记录后才重新填写。Material 纯默认组保持持久版本，混合组和 override 走帧参数池。PersistentCBufferArena 的 Trim 只在 RT 分配边界执行。

AppFrameTarget::OnOutput 在 Submit/Present 收尾时交付每个输出的 CommandsSubmitted 与 Presentation；命令执行安排、呈现返回与最后的 GPU completion 分别使用各自的事实。

GpuFrameResources::BorrowTexture 按尺寸、格式、采样、mip/layers、用途及 view role 精确匹配物理 texture/view；不接纳 external backbuffer。默认预算为 32 个目标、256 MiB 估计纹理字节及 8 个闲置目标。预算无法满足时返回空；已使用版本不会被驱逐。lease 仅在开放 epoch 有效，seal 后不能修改。相同物理 view 的新 lease 逻辑 Initialized=false；InitialState 描述物理状态，调用者在必要命令被接纳后显式登记 FinalState。无 aliasing 或跨帧切片升级。

跨帧 readback/history 从开始就使用独立 owner，记录 generation、producer serial、有效性、消费者和过程预算。CPU 未消费的 readback 在 GPU 完成后仍归过程持有；CPU 取消仍保留 GPU 写 owner 至完成。停止选择旧历史后，已接纳读者继续保活旧代。GpuRetainedResult 用两个历史版本和原生 gate 验证这些边界；不另设全局保留资源管理器或第二套 fence 轮询。

## 帧 profiler

`GpuFrameProfiler` 是定义和实现在 `gpu_resource.h/.cpp` 中的可选组件，
由 `GpuSystemDescriptor::EnableFrameProfiler` 控制创建。它对应 UE5 的 `FGPUTiming`（最小化）：
per-flight timestamp pool + readback。
`GpuSystem` 提交时从同一命令分配器取得普通 buffer，在应用批次前后分别录制开始 timestamp
和结束 timestamp / resolve，不保留独立的计时命令 buffer 成员，也不打断 D3D12 的
Submit → Present 配对。计时范围覆盖整段应用提交序列，可能包含组间 GPU 等待。
跳过应用工作的帧不录制计时，不发布旧 query 结果。flight 真实退休时读取实际提交的
query 结果；应用只读 `GetLastGpuTimeMs()`，后端 readback barrier 差异内部隐藏。
最近一次 GPU 耗时通过原子标量发布，允许 game thread 在另一 flight 回收时读取。帧延迟由 FrameTimeline 在 `PublishCompletion` 时发布。

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
窗口不可呈现的局部输出丢弃和 Vulkan 收尾规则见前述命令批次契约。
默认 Application::Render 为空，没有自动 acquire、清屏或离屏输出管理。

交换链尺寸变化时后备缓冲 view 会重建。view 销毁由 Vulkan 后端在驱动销毁前同步清理该 view 对应的闲置 framebuffer；仍被 command epoch 引用时立即诊断。上层不再维护 framebuffer registry。

## 关停顺序

`Application::Shutdown` 的顺序是固化的，每一步都有理由：

```text
ApplicationLoop 停止发布和输入派发、关闭窗口操作 → RT 消费已发布包
继续泵消息 → 观察 RT 实际退出并 join → 系统 BeginStopping（World/Scene/scheduler/加载）
临时线程等待 final fence + 主队列 idle → GT 观察实际退出并 join → GT 退休并消费完成、释放 owner
terminal abandon：释放未发布 Scene 包，不生成成功 completion
安全点交付真实主窗口关闭一次 → OnShutdown → 取消剩余通知 → WorldManager 显式 Shutdown → SceneManager
释放 GPU 帧参数资源和未发布 raw frame owner → AssetManager → RenderSystem → FrameTimeline → AssetDatabase → GpuSystem → WindowManager
```

AssetManager 只拥有加载 scope；取消加载不会释放框架已接纳的 GPU owner。
Material 的持久 SPS 与常量 allocation 必须在 program/layout 服务退出前释放，因此 GPU drain 后先拆资产，再拆 RenderSystem。
FrameTimeline 在 AssetManager 之后、GpuSystem 之前销毁：在飞 CPU/加载等待先在时间线仍存活时收束，剩余等待者取消时 device 仍在。
普通窗口维护通过临时线程等待 GPU/队列，再由 GT 退休并发布完成，随后在帧边界释放 owner，
不 abandon writer 状态；abandon 后不能恢复正常发布。


关键约束：**`AssetManager` 必须在 `AssetDatabase` 之前销毁**（在飞 task 可能持有 importer），
且两者都必须在 `GpuSystem` 之前销毁（GPU 资源必须在 device 之前交出）。

完成通道的生命周期跟随 FrameTimeline；关停期间保持可写，直到生产者停止且最终消息已消费后随时间线销毁。
固定队列随时间线销毁，停止前已经发布的消息必须先消费。

ApplicationLoop 的 RT 在退出前消费全部已发布 Scene batch，即使退出已请求，也只跳过应用绘制。
没有发布的 batch 及其退休引用在 RT 停止、GPU idle 后于 GT 清理。RenderScene::Apply 不表示 GPU 完成；CPU 增量交付沿用现有
ready 交接、fence 与 flight 退休语义。

`Application::Shutdown` 和析构共用 `StopAndDrainRuntime` 停止生产者、排空真实完成并释放未发布 owner，
随后取消通知并通过 `DestroyRuntime` 按固定顺序拆除。析构作为正常 `Shutdown` 被异常绕过时的保底；该路径
不派发延迟游戏事件，也不调用派生类的 `OnShutdown`，复用相同排空协议。Run 的作用域清理先排空借用线程，再允许调用栈展开至派生对象成员析构。停止派发时向仍处于系统模态循环的托管窗口发送 WM_CANCELMODE，使菜单等原生状态退出，最终拆除仍回到外层循环。所有线程退出后才销毁设备、窗口和唤醒对象。
退出请求、超时和线程结束都不能伪造 GPU 完成或授权提前释放在途资源。

## 服务装配

Application 先创建 FrameTimeline，再按可选配置创建其余对象、连接借用引用并调用初始化函数。GpuSystem 借用这条时间线发布完成和分配 FrameSerial。依赖关系和拆除顺序均由普通代码明确表达。
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
| `OnCollectRenderViews` | S1 完成 World 收集后、Seal 前；只提交按值视图，不修改 World 或泵 scheduler/asset |
| `OnRenderFrameComplete` | game thread 消费 `FlightCompletion`，包括 GPU 跳过帧、无 GPU 的完成（`OutputsExecuted` 为 false）和 shutdown；允许释放 GT 资产引用 |
| `OnShutdown` | 关停，游戏侧清理 |

`Application::Update` / `Shutdown` 固化宿主时序；Render 只负责应用命令内容，不接管 runner 的提交协议。
