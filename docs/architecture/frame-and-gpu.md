> - 适用: 改帧节奏、提交时序、GPU 上传；排查"GPU 对象被提前销毁"或帧同步问题
> - 权威: 本文是帧节奏与 GPU 资源上传的唯一说明。资产侧的延迟销毁契约见 [asset-system](asset-system.md)；RHI 本身见 [render-rhi](render-rhi.md)
> - 锚点: `modules/runtime/include/radray/runtime/gpu_system.h`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/include/radray/runtime/wait_frame.h`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/gpu_system.cpp`

# 帧节奏与 GPU 上传

## 职责边界

| 系统 | 负责 | 不负责 |
|---|---|---|
| `GpuSystem` | **何时画**。instance/factory/device/主队列/fence、flight 槽位、上传器、帧 profiler、帧边界等待表、flight 完成消息的发布与内部状态应用 | 画什么 |
| `RenderSystem` | program/artifact cache、RenderPass/Framebuffer registry | GPU 提交时序 |
| `WindowManager` | 窗口创建/销毁、swapchain acquire/present/recreate、事件分发 | — |
| `Application` | 固化帧序与关停顺序；消费 flight 完成消息；游戏侧的窄扩展点 | — |

## GpuSystem 的线程约定

`gpu_system.h` 在可调用的 `GpuSystem` 函数声明上标记线程与阶段，私有函数也遵循同一约定：

- **GT** 是 Application 所在线程，拥有游戏帧号与帧等待表。协程的启动、恢复和
  取消都必须在 GT；`GetFrameIndex` / `GetCurrentFlightIndex` 读取非原子状态，不能作为 RT 的帧号来源。
- **RT** 是当前 flight 的录制线程。GT 完成 Update 并交出槽位后，RT 独占录制与提交。
  单线程模式中 RT 与 GT 是同一线程。应用显式调用 uploader 的录制也发生在 RT。
- **GT/RT，retire 阶段** 表示两种线程都可能回收 flight，不表示函数内部提供互斥。
  ThreadedRunner 用 `_retireMutex` 串行化 retire，并通过提交发布与槽位信号量隔离 Submit 和复用。
  单线程或全局 idle 路径须提供等价的无并发前提；channel 的锁只保护消息，不保护 flight 状态。
- **任意线程** 仅适用于读取固定配置、稳定对象指针或原子统计。对象必须已完成构造/装配发布，
  且尚未开始拆除；取得指针不会改变 device、queue、WindowManager 自身的线程约束。

生命周期操作仍归 GT：装配与拆除必须隔离其他读者，销毁前先停止生产者、等待 render/GPU idle，
并消费完成消息。`WaitAndRetireFlights` 自己等待 idle；`CleanupCompletedFlights` 与析构则要求
调用前已经 idle。逐函数的具体前提以头文件中的 API 契约为准。

## 帧序

```
Application::StartLoop
  ├─ runner::PrepareFrame            等待并取得当前可写 flight；处理已有交换链重建
  ├─ Application::BeginUpdateForFlight  收集完成批次 → GPU 内部调度 → OnRenderFrameComplete
  ├─ GpuSystem::BeginFrameTiming     记录逻辑帧开始时间，计算相邻帧 DeltaTime
  ├─ NativeEventPump::DispatchEvents 排空可取的窗口消息，包含等待与完成回调期间投递的输入
  ├─ CheckRecreateSwapChains         处理事件期间产生的交换链变化
  ├─ AssetManager::Pump               提交加载结果；销毁零引用资产
  ├─ ApplicationScheduler::Pump
  ├─ Application::OnUpdate            游戏逻辑
  ├─ World::Tick
  ├─ 发布当前 flight；game thread 可以开始下一可写 flight 的 Update
  ├─ GpuSystem::BeginFrameRecord      render thread Begin 主 CommandBuffer；清 targets、开始 profiler
  ├─ Application::Render              应用录制入口；默认空实现
  └─ GpuSystem::EndFrameRecordAndSubmit
       uploader.EndFlight → 结束共享 CB 与 per-HWND present CB → 聚合 sync object
       → 共享 CB / 应用附加 CB（窗口已不可呈现时跳过本轮命令）
       D3D12 且多个 HWND：先 Submit 共享工作，再对每个窗口 Execute(present CB) 后立刻 Present
       单窗口与 Vulkan：一次 Submit（含各 present CB）再 Present 全部 target
```

单线程与双线程普通循环都遵循上述顺序。资源边界是当前 flight 上一轮的 fence 已完成、
runner 已取得槽位使用权；逻辑帧边界是完成批次与应用完成钩子处理结束后的计时点。
等待可写槽和处理完成批次放在事件派发之前，让紧接着的 Update 使用这些阶段期间到达的输入。
完成批次处理有明确终点，不会为回调新建的任务或稍后到达的 GPU 消息反复排空整个系统。

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
调用点是 `RetireRenderedFrames`（渲染线程在两帧 Record 之间，game thread 在
`WaitWritableSlot` 等已提交 fence 之后）；不访问协程等待表，也不调用应用钩子。

`Application` 是 channel 的唯一消费者。帧顶取得可写槽位后，`Application::BeginUpdateForFlight`
作为 `GpuSystem` 的 friend，直接通过私有 `_flightCompletions.TryRead` 非阻塞收集一个本地批次，
随后调用 `GpuSystem::BeginUpdateForFlight` 重置当前槽位的 HostWrites 并运行 `PumpWaitFrame`。
完成批次只由 Application 用于应用完成钩子，不再传给 GPU 内部上传调度器。
这些步骤返回后，Application 逐条调用 `OnRenderFrameComplete`，随后开始本帧计时、派发窗口事件并进入 Update。
完成消息保留 FrameSerial，不能仅用可复用的 FlightIndex 识别一帧。
`GpuWorkCompleted` 仍表示该轮渲染结果有效；false 的跳过帧也已经经过其提交的真实 fence，
通知不代表画面已显示到屏幕。

`Application::PumpFlightCompletions` 包含 GT 线程断言与覆盖内部调度、应用钩子的重入保护。
批次收集期间不执行用户代码；处理批次期间新发布的消息留到下一次消费。
channel 不拥有回调或资产引用，没有观察者注册、注销与跨线程调用应用的接口。

多线程普通帧只等待当前 flight 可写，不等待上一帧 CPU record 结束，允许 `Update(n+1)` 与
`Record(n)` 重叠。槽位在 GPU fence 之后才能复用；`WaitWritableSlot` 等的是**已提交** flight 的
fence，并在等待中 retire，不把 game thread 堵到当前 `Record` 结束。同一 flight 仍必须等 fence。
关闭与模态丢帧仍提交真实 fence，但跳过 OnRender，完成通知的 `GpuWorkCompleted=false`，
不能据此提交图像历史。当前不再录制或提交自动的资产上传前缀。

`GpuSystem::SubmitFrame` 把不写 flip backbuffer 的工作录在共享 command buffer 上；每个已 acquire
的窗口有自己的 present command buffer。D3D12 在同一 flight 有多个窗口时，先提交共享工作，再对每个
HWND `Execute` 该窗的 present CB 并立刻 `Present`，不再等待 graphics queue。这样 DXGI 把每次 Present
绑到只写该 current backbuffer 的 Execute。应用附加 command buffer 与共享工作一起提交，不得写入
flip backbuffer。单窗口与 Vulkan 仍一次 Submit 再逐个 Present。同一队列上 present blit 仍排在共享
工作之后，GPU 可以接着跑；Acquire 的 waitable、FIFO Present 队列满、复用 flight 的 fence，以及
`EnsureRenderIdle` 仍会挡住 CPU 或让 GPU 等下一帧工作。
创建、销毁或 resize 窗口时，`WindowManager` 调用 runner 的
`EnsureRenderIdle`，排空已发布工作、GPU 引用，并等待 present 队列（D3D12 的 `Present` 在
frame fence 之后入队，只等 fence 不够）。已挂 swapchain 的 `NativeWindow`
`SetSize` / `SetPosition` / `Show` / `SetAlpha` / `SetOwner` 同样先 idle，再改 HWND。
`AcquireWindow` 与 `SubmitFrame` 会检查活的最小化、隐藏与客户区状态。
已经 acquire 但提交前被最小化或隐藏的窗口不再执行写入 swapchain 的 GPU 工作；D3D12 在
不可呈现的 HWND 上跳过 DXGI Present。绕过 NativeWindow 的原始 `ShowWindow(SW_MINIMIZE)`
必须先 `EnsureRenderIdle`：在 Win32 钩子里等待会让渲染线程在最小化过程中 Present，同样
`ACCESS_DENIED`。这个生命周期等待不发生在普通无变更帧。

runner 在准备槽位、完成回调、Update 与录制期间拒绝模态 Tick 重入，事件派发期间允许模态 Tick。
普通循环在 DispatchEvents 前保留一个已准备的逻辑帧；模态 Tick 优先消费它，不重复领取 writable
信号量、重置 HostWrites、恢复完成批次或采样时间。事件派发期间出现模态活动后，外层跳过普通 Tick，
不会再提交已被模态路径消费的帧；尚未消费的准备状态保留供后续 Tick 使用。
同一次系统模态循环中的后续帧自行取得可写槽、收尾并开始计时，沿用系统已派发的输入，
不递归调用 DispatchEvents。双线程模态路径仍先等待已发布的 CPU 录制结束，槽位不可写时跳过本次 Tick。

### 窗口输入

NativeWindow 保留原始键鼠、文本、滚轮与焦点事件，调用方可直接订阅其信号。
runtime 的 WindowInputRouter、AppWindow::GetInput、统一 DispatchInput 与 OnBeforeInput 扩展槽已移除。
应用自行处理输入状态、捕获与失焦取消；宿主不再提供应用/UI 输入归属路由。

通用窗口能力通过 `NativeDesktopCapabilities` 查询：光标形状与显隐、桌面鼠标定位、显示器
工作区和 DPI、UTF-8 clipboard、IME 候选框定位。Win32 实现这些能力及 DPI/display/capture
消息；Cocoa 的新增桌面接口明确返回能力缺失，不伪装成功。Cocoa 滚轮继续发送原始滚轮事件，
同时提供新双轴浮点事件；这里不增加 macOS runtime。

## Flight 槽位

`GpuFlightSlot` 按所有权/阶段分三组，跨阶段的访问时序由 runner 的信号量 + retire 锁保证：

| 组 | 成员 | 谁访问 |
|---|---|---|
| 录制态 | `CmdBuffer`, `Uploader`, `HostWrites`, `Targets`, `Submitted`, `Recording` | GT 帧顶重置 HostWrites；RT 接管后录制，应用显式上传的 staging 在 fence 后回收 |
| 计时态 | `FrameStartTime` | 游戏线程在帧开头写 |
| 提交态 | `Signal` | `EndFrameRecordAndSubmit` 写；retire/`CompleteFlight` 经 `_retireMutex` 读后清 |
| 等待表 | `WaitFrame` | 见下 |

完成消息不挂在槽位上。`UnboundedChannel<FlightCompletion>` 属于 `GpuSystem`，由 retire 线程
写入、Application 在 GT 消费。`WaitFrame` 仍用槽位上的原子位，见下一节。

`_flights` 是 `vector<unique_ptr<FlightSlot>>` 而不是 `vector<FlightSlot>`：`FlightSlot`
内含 `ManualCoroutineScheduler`，它不可拷贝也不可移动——挂起的协程记录里存着回指调度器的
指针（stop callback），搬动槽位会让那些指针指向旧地址。数量在构造时定下且此后不变，
故间接一层不带来任何代价。

## 帧边界等待

这是 GPU 资源延迟销毁的机制：持有者等待帧边界，在恢复后的作用域内析构整包 payload，
使资源依赖通过包内成员声明顺序表达，不依赖逐对象回收队列的入队顺序。
资产侧入口见[资产系统](asset-system.md)。实现约束如下：

**等待表挂在 per-flight 槽位上**（`GpuFlightSlot::WaitFrame`），不是全局表。flight 的
fence 就是完成条件，记在槽位上便无需另存 fence 值再逐个比较。

**两段式恢复。** `CompleteFlight` 只发布槽位完成的原子标记；主线程的 `PumpWaitFrame`
消费标记，更新该 flight 的等待记录并 resume。

**`Wait()` 挂进当前 flight，不是"上一次提交的 fence 值"。** 调用点在帧顶 Update 期间，
此刻当前 flight 还没开始录制，故"已录制的 work"全都属于更早的 flight——等当前 flight 的
fence 必然晚于它们完成。这样就不必记录并比较 fence 值，代价是最多多等一轮
（口径本就允许多等）。

**`PumpWaitFrame` 只泵当前可写 flight。** 调用点固定在 `BeginUpdateForFlight`，关停时则在
排空工作后逐 flight 泵送。等待表始终由 game thread 修改，render thread 只写原子完成标记。

**关停必须 `CancelAllWaitFrames`。** 挂在未提交 flight 上的记录永远等不到 fence，
不取消就是协程帧连同它捕获的 GPU 对象一起泄漏。

### 非资产的持有者怎么延迟销毁

`AssetManager::DeferDestroy` 是给 `Asset::OnUnload` 用的，不要在别处调它。
非资产的持有者直接 `co_await` `IWaitFrameProcessor::Wait()`（实现方是 `GpuSystem`）：

```cpp
// 在自己的 TaskScope 里
scope.Spawn([this, payload = std::move(gpuStuff)]() -> task<void> mutable {
    co_await _waitFrame->Wait();
    // payload 在此处析构，声明顺序即销毁顺序
}());
```

两条约束：**自己的 `TaskScope` 必须在 `GpuSystem` 之前析构**（否则取消时的析构会碰到已死的
device），且**恢复点在主线程**，所以析构里可以安全动 GPU 对象。

`Application::InitializeRuntime` 直接通过 setter 连接帧等待接口：

```cpp
_assetManager->SetWaitFrameProcessor(_gpuSystem.get());
```

其他需要等待帧的系统也由 Application 显式连接到 GpuSystem 提供的 `IWaitFrameProcessor`，
并在 `DestroyRuntime` 中安排其先于 GpuSystem 清理。装配细节见「服务装配」一节。

目前仓库里只有资产走这条路，所以没有现成的非资产调用点可参照。

## 完成通知与帧等待恢复

retire 阶段观察到 fence 后发布两种通知：

| 路径 | 载体 | 消费与恢复 |
|---|---|---|
| 应用完成钩子 | `UnboundedChannel<FlightCompletion>` 的本地批次 | Application 在主线程逐条调用 `OnRenderFrameComplete` |
| 帧边界等待 | per-flight 原子 `WaitersCompleted` | `PumpWaitFrame` 只泵当前独占 flight |

普通帧顺序固定为：收集消息 → `PumpWaitFrame` → `OnRenderFrameComplete`。
先消费旧 WaitersCompleted，再调用应用钩子，避免钩子新建的 Wait 被误认为已经完成。
`PumpWaitFrame` 在恢复现有等待者前统一标记旧等待记录，恢复期间新建的等待同样不会提前完成。
上传专用的等待表、状态机和恢复路径已删除，帧等待协议保留。

当前 flight 取得可写槽位之前，它上一轮的 fence 完成消息和 WaitersCompleted 已发布。
ThreadedRunner 的 `RetireRenderedFrames` 只在 `CompleteFlightIfReady` 成功后 release writable
信号量，因而帧顶不需要再次收集当前槽位的完成消息。其他 flight 稍后发布的消息留到下一次消费。

窗口重建、detach 使用 `GpuSystem::WaitAndRetireFlights`：等待 render/GPU idle、退休所有已提交
flight 并发布消息，不消费 channel、不恢复协程。消息留给下一次 Application 帧顶或关停消费，
避免窗口操作中途改变应用完成钩子与协程的执行阶段。

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
未在此次删除中修改。`IWaitFrameProcessor::Wait` 继续负责帧边界等待与延迟销毁。

## 渲染资源的帧寿命

RenderSystem 通过私有 ShaderProgramCache 拥有 JIT、artifact/program cache；PSO 由调用方通过 RHI 创建和持有。
program 的 layout/参数 metadata 活过所有 flight，关停 GPU idle 后才销毁。

旧框架的自动 per-flight asset refs、pool、descriptor arena 和 history 已移除。应用录制方负责
让输入数据、原生资源与资产 owners 活到对应工作完成。StreamingAssetRef 的复制和释放仍只在 GT；
可在 OnUpdate/OnRenderFrameComplete 的安全点处理，或沿既有资产延迟销毁协议回收。
retire 阶段仅发布帧完成消息；应用完成钩子在 GT 消费消息时执行。

## 帧 profiler

`GpuFrameProfiler` 对应 UE5 的 `FGPUTiming`（最小化）：per-flight timestamp pool + readback。
由 `GpuSystem` 在 `BeginFrameRecord`/`EndFrameRecordAndSubmit` 自动包裹本帧录制，
`CompleteFlight` 时 resolve。应用只读 `GetLastGpuTimeMs()`。后端 readback barrier 差异
内部隐藏。
最近一次 GPU 耗时和 frame latency 通过原子标量发布，允许 game thread 在另一 flight 回收时读取。

## 呈现

`AppFrameContext::AcquireWindow(window)` 内部 `AcquireNextSwapChainFrame`：
`RequireRecreate` / `RetryLater` / `Error` / 最小化 → `nullopt`（应用跳过该窗口）。
成功时把 `SwapChainFrame` 收进本帧 `FlightSlot`，返回 `AppFrameTarget`
（backbuffer + view + index）。

**`AcquireWindow` 不录任何 barrier。** 应用显式取得目标后，负责使用 backbuffer 真实初态、
录制目标内容并收口到 Present；只有显式 acquire 的窗口参与本次呈现。`AppFrameTarget` 不暴露同步对象。
写 flip backbuffer 的命令使用 `AppFrameContext::GetCommandBufferForTexture`；`GpuSystem` 在对应窗口
命令提交返回后、Present 前将 backbuffer 状态记为 Present。若本轮跳过窗口 GPU 工作，则保留原状态。
默认 Application::Render 为空，没有自动 acquire、清屏或离屏输出管理。

交换链尺寸变化时后备缓冲 view 会重建，此时必须调
`RenderPassRegistry::RemoveFramebuffersUsing(oldView)`：framebuffer 存的是 `TextureView`
裸指针。见 [render-rhi](render-rhi.md)。

## 关停顺序

`Application::Shutdown` 的顺序是固化的，每一步都有理由：

```cpp
WaitAndCleanupCompletedFlights();             // Application 等 GPU、消费完成消息并恢复协程
OnShutdown();                                  // 游戏侧释放自管 per-flight 资源
_scheduler.CancelAll();
_world.reset();                    // Actor / Component → drop StreamingAssetRef
_windowManager->SetRenderSystem(nullptr);  // RenderPassRegistry 即将销毁，先断引用
_renderSystem.reset();             // shader/program → registry
_assetManager.reset();             // 放开全部资产，GPU buffer 须在 device 前释放
_assetDatabase.reset();            // importer/settings 活过 manager 的在飞 task
_windowManager->DetachAllSwapChains();
_windowManager->SetGpuSystem(nullptr);
_gpuSystem->SetWindowManager(nullptr);
_gpuSystem.reset();                // device 最后死
_windowManager.reset();
```

关键约束：**`AssetManager` 必须在 `AssetDatabase` 之前销毁**（在飞 task 可能持有 importer），
且两者都必须在 `GpuSystem` 之前销毁（GPU 资源必须在 device 之前交出）。

channel 生命周期跟随 GpuSystem；关停期间保持可写，直到生产者停止且最终消息已消费后随宿主销毁。
无需调用 `Complete()`；若使用该操作，它只禁止新写入，积压消息仍能读完。

`Application` 析构也复用幂等的内部 teardown，作为正常 `Shutdown` 被异常绕过时的保底；该路径
不调用派生类的 `OnShutdown`，但仍会 wait GPU、取消 scheduler、断开窗口引用并保持同一销毁顺序。

## 服务装配

Application 直接创建对象、连接借用引用并调用初始化函数；依赖关系和拆除顺序均由普通代码明确表达。
设备由 GpuSystem 构造函数创建，WindowManager 与 GpuSystem 的双向引用在对象就位后连接。
可选资产来源、失败清理与所有权边界见
[Application 直接装配](render-framework.md#application-直接装配)。

## 游戏侧扩展点

底层负责"何时 tick、怎么 acquire/render/present"；游戏只负责"这个应用要画什么"。

| 钩子 | 时机 |
|---|---|
| `OnInit` | 全部内部系统就绪后一次。加载资产、Spawn Actor、建相机 |
| `OnRender` | 在 runner 开始录制后由 Render 调用；默认空，可覆盖以记录 RHI 命令 |
| `OnUpdate` | 每帧，`AssetManager::Pump` 之后、`World::Tick` 之前 |
| `OnRenderFrameComplete` | game thread 消费 `FlightCompletion`，包括跳过和 shutdown；允许释放 GT 资产引用 |
| `OnShutdown` | 关停，游戏侧清理 |

`Application::Update` / `Shutdown` 固化宿主时序；Render 只负责应用命令内容，不接管 runner 的提交协议。
