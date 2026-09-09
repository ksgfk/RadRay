> - 适用: 改帧节奏、提交时序、GPU 上传；排查"GPU 对象被提前销毁"或帧同步问题
> - 权威: 本文是帧节奏与 GPU 资源上传的唯一说明。资产侧的延迟销毁契约见 [asset-system](asset-system.md)；RHI 本身见 [render-rhi](render-rhi.md)
> - 锚点: `modules/runtime/include/radray/runtime/gpu_system.h`, `modules/runtime/include/radray/runtime/flight_completion.h`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/include/radray/runtime/wait_frame.h`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/gpu_system.cpp`

# 帧节奏与 GPU 上传

## 职责边界

| 系统 | 负责 | 不负责 |
|---|---|---|
| `GpuSystem` | **何时画**。instance/factory/device/主队列/fence、flight 槽位、上传器、帧 profiler、帧边界等待表、flight 完成通知的发布与排空 | 画什么 |
| `RenderSystem` | **画什么**。pipeline、workload/output、graph pools/history、program/artifact cache、RenderPass/Framebuffer registry、game-thread Scene 与 per-flight asset refs | GPU 提交时序 |
| `WindowManager` | 窗口创建/销毁、swapchain acquire/present/recreate、事件分发 | — |
| `Application` | 固化帧序与关停顺序；游戏侧的窄扩展点 | — |

## 帧序

```
Application::StartLoop
  ├─ NativeEventPump                 收集原始窗口输入
  ├─ BeginUpdateForFlight(flight)     取得该 flight 的可写槽位；PumpFlightCompletions → PumpWaitFrame → PumpFrameUploadScheduler
  ├─ RenderSystem::BeginUpdateForFlight 清除该 flight 上一帧的 retained asset refs 和 frame plan
  ├─ ApplicationExtension::OnBeginUpdate  按安装序；ImGui 在此消费已完成 flight 的纹理反馈，释放当前槽快照
  ├─ AssetManager::Pump               提交加载结果；销毁零引用资产
  ├─ ApplicationScheduler::Pump
  ├─ ApplicationExtension::OnBeforeInput  按安装序；ImGui 在此消费原始输入、决定输入捕获并 NewFrame
  ├─ WindowManager::DispatchInput     向应用派发经过路由的输入
  ├─ Application::OnUpdate            游戏逻辑
  ├─ World::Tick
  ├─ ApplicationExtension::OnAfterWorldTick 按安装序；ImGui 在此触发 EventDraw、Render、UpdatePlatformWindows、复制拥有数据的快照
  ├─ RenderSystem::PrepareFrame        game thread 复制 pipeline input 并构造 view families；pipeline → overlays → composer
  ├─ GpuSystem::PrepareFrameUploads   game thread 恢复 BeginUpload，录制当前 flight 的 UploadCommands
  ├─ 发布当前 flight；game thread 可以开始下一可写 flight 的 Update
  ├─ GpuSystem::BeginFrameRecord      render thread Begin 主 CommandBuffer；清 targets、开始 profiler
  ├─ Application::Render              → pool/history BeginFlight → output/view resolve → pipeline graph → host finalize
  └─ GpuSystem::EndFrameRecordAndSubmit
       uploader.EndFlight → 结束共享 CB 与 per-HWND present CB → 聚合 sync object
       → UploadCommands（若仍可呈现则加上共享 CB / 应用附加 CB）
       D3D12 且多个 HWND：先 Submit 共享工作，再对每个窗口 Execute(present CB) 后立刻 Present
       单窗口与 Vulkan：一次 Submit（含各 present CB）再 Present 全部 target
```

`ApplicationExtension` 是 runtime 唯一的帧内扩展点（`modules/runtime/include/radray/runtime/application_extension.h`）：
`Application::AddExtension` 只在运行时初始化后、主循环启动前（即 `OnInit` 内）接受安装，三槽按安装序在
game thread 调用，`DestroyRuntime` 在 World 之前按逆序销毁扩展。runtime 本身不知道任何具体扩展。

`BeginFrameRecord` 为每次录制生成独立 FrameSerial，收据同时校验 serial 与阶段，不能以可复用
flight index 代替提交身份。`FrameSubmission` 的 Recorded/Submitted/GpuCompleted 分别对应录制、
void Submit 返回与真实 fence 完成；未提交收据取消不发布资源状态和历史。提交后的收据由 flight
保留至 fence，graph tickets/readback/owner 随该收据完成。
成功 Submit 后立即清空 `OnSubmitted`，释放状态快照与提交阶段捕获，不再访问借用的外部资源 wrapper；
需要活到 GPU 完成的 owner 必须由 `OnCompleted` 保留。已提交收据的 Cancel 不提前释放这些 owner，
正常完成或失败完成仍由匹配 frame serial 的 fence 路径处理。

`CompleteFlight` 在 fence 完成后 resolve profiler、回收 staging、完成 submission receipts，并 `NotifyFlightComplete`
入队，同时发布原子 `WaitersCompleted`。调用点是 `RetireRenderedFrames`（渲染线程在两帧 Record 之间，
game thread 在 `WaitWritableSlot` 等已提交 fence 之后）。不访问协程等待表。
`GpuSystem::PumpFlightCompletions` 在 game thread 排空队列：先让上传调度器 `ApplyCompletedFlights`，
再按注册顺序调用 `IFlightCompletionObserver`。`Application` 与 `ImGuiSystem`（imgui 模块自行注册）都是观察者；
`Application::OnFlightsComplete` 再转到 `OnRenderFrameComplete`。线程断言留在 `Application` 一侧
（它持有 `_applicationThread`）；`GpuSystem` 全文没有 `this_thread::get_id()`，靠调用点固定在
`BeginUpdateForFlight` 与 `WaitAndCleanupCompletedFlights`。正常、跳过和 shutdown 路径保持相同线程归属。

多线程普通帧只等待当前 flight 可写，不等待上一帧 CPU record 结束，允许 `Update(n+1)` 与
`Record(n)` 重叠。槽位在 GPU fence 之后才能复用；`WaitWritableSlot` 等的是**已提交** flight 的
fence，并在等待中 retire，不把 game thread 堵到当前 `Record` 结束。同一 flight 仍必须等 fence。每个 flight 拥有独立上传命令和 uploader，
发布后 game thread 不再改它。关闭、模态丢帧和 shutdown 仍提交已经录制的上传并等待真实 fence，
但完成通知的 `GpuWorkCompleted=false`，不能据此提交图像历史。单线程/手动录制入口会补做尚未准备的上传。

`GpuSystem::SubmitFrame` 把不写 flip backbuffer 的工作录在共享 command buffer 上；每个已 acquire
的窗口有自己的 present command buffer。D3D12 在同一 flight 有多个窗口时，先提交共享工作，再对每个
HWND `Execute` 该窗的 present CB 并立刻 `Present`，不再等待 graphics queue。这样 DXGI 把每次 Present
绑到只写该 current backbuffer 的 Execute。应用附加 command buffer 与共享工作一起提交，不得写入
flip backbuffer。单窗口与 Vulkan 仍一次 Submit 再逐个 Present。同一队列上 present blit 仍排在共享
工作之后，GPU 可以接着跑；Acquire 的 waitable、FIFO Present 队列满、复用 flight 的 fence，以及
`EnsureRenderIdle` 仍会挡住 CPU 或让 GPU 等下一帧工作。
创建、销毁、resize 或修改 output 时，`WindowManager`/output registry 才调用 runner 的
`EnsureRenderIdle`，排空已发布工作、GPU 引用，并等待 present 队列（D3D12 的 `Present` 在
frame fence 之后入队，只等 fence 不够）。已挂 swapchain 的 `NativeWindow`
`SetSize` / `SetPosition` / `Show` / `SetAlpha` / `SetOwner` 同样先 idle，再改 HWND。
窗口的 Active/尺寸目录在 PrepareFrame
复制进 flight；`AcquireWindow` 与 `SubmitFrame` 会再查一次活的最小化、隐藏与客户区状态。
已经 acquire 但提交前被最小化或隐藏的窗口不再执行写入 swapchain 的 GPU 工作；D3D12 在
不可呈现的 HWND 上跳过 DXGI Present。绕过 NativeWindow 的原始 `ShowWindow(SW_MINIMIZE)`
必须先 `EnsureRenderIdle`：在 Win32 钩子里等待会让渲染线程在最小化过程中 Present，同样
`ACCESS_DENIED`。这个生命周期等待不发生在普通无变更帧。

可选 UI 的完成通知只发布 flight 结果，主线程在下一次 update 消费，渲染线程不访问活的
ImGui context。窗口模态 Tick 在正在进行的帧内拒绝重入，多线程 runner 只在 render idle
时允许窗口事件触发新帧。完整 UI 快照及纹理寿命见 [Runtime ImGui](runtime-imgui.md)。

### 窗口输入

NativeWindow 保留原始事件职责，AppWindow 的 `GetInput().EventInput()` 提供应用输入。
WindowInputRouter 复制 UTF-8 文本和双轴浮点滚轮，在 Update 前统一派发；不要在游戏代码中
直接订阅 NativeWindow 的键鼠信号。每次按下记录应用/UI 归属，抬起归还原归属；应用持有的
拖动不因 UI 捕获改变而丢失。失焦、关闭和 capture 丢失产生取消，避免卡键。Auxiliary
窗口默认不派发应用输入。ImGui 始终从原始队列取事件，跨受管窗口按全局到达顺序处理。

通用窗口能力通过 `NativeDesktopCapabilities` 查询：光标形状与显隐、桌面鼠标定位、显示器
工作区和 DPI、UTF-8 clipboard、IME 候选框定位。Win32 实现这些能力及 DPI/display/capture
消息；Cocoa 的新增桌面接口明确返回能力缺失，不伪装成功。Cocoa 滚轮继续发送原始滚轮事件，
同时提供新双轴浮点事件；这里不增加 macOS runtime。

## Flight 槽位

`GpuFlightSlot` 按所有权/阶段分三组，跨阶段的访问时序由 runner 的信号量 + retire 锁保证：

| 组 | 成员 | 谁访问 |
|---|---|---|
| 上传态 | `UploadCommands`, `Uploader`, `UploadsPrepared`, `HostWrites` | game thread 准备，发布后转交 render thread；fence 后回收 |
| 录制态 | `CmdBuffer`, `HostWrites`, `Targets`, `Submitted`, `Recording` | render thread 在发布后独占，`BeginFrameRecord`→`Render`→提交期间 |
| 计时态 | `FrameStartTime` | 游戏线程在帧开头写 |
| 提交态 | `Signal` | `EndFrameRecordAndSubmit` 写；retire/`CompleteFlight` 经 `_retireMutex` 读后清 |
| 等待表 | `WaitFrame` | 见下 |

完成通知不挂在槽位上。`FlightCompletionQueue` 属于 `GpuSystem`：任意 retire 线程 `Push`，
game thread 在 `PumpFlightCompletions` 全排空并扇出。`WaitFrame` 仍用槽位上的原子位，见下一节。

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

`IWaitFrameProcessor` 从 `ServiceRegistry` 注入，写法与 `AssetManager` 一样——
在类上加一个 setter，然后：

```cpp
template <> struct ServiceTraits<MyCache> {
    using Dependencies = TypeList<Required<IWaitFrameProcessor>>;
    static void Inject(MyCache& self, IWaitFrameProcessor& frames) noexcept {
        self.SetWaitFrameProcessor(&frames);
    }
};
```

`ServiceTraits<GpuSystem>::Provides` 暴露 `IWaitFrameProcessor`，编译期确定唯一提供者并生成
直接注入调用。装配细节见「服务装配」一节。

目前仓库里只有资产走这条路，所以没有现成的非资产调用点可参照。

## 完成通知与三条恢复路径

retire 线程观察到 fence 之后，有三条互不替代的恢复路径：

| 路径 | 载体 | 排空 |
|---|---|---|
| 上传协程（`AwaitingFence` → `FenceComplete`） | `GpuSystem` 的 `FlightCompletionQueue` | `PumpFlightCompletions` 全排空，再 `PumpFrameUploadScheduler` 恢复 |
| 游戏 / ImGui 观察者 | 同一条队列 | 同上，apply 之后按注册顺序扇出 |
| 帧边界等待（`IWaitFrameProcessor::Wait`） | per-flight 原子 `WaitersCompleted` | `PumpWaitFrame` **只泵当前独占 flight** |

前两条曾各有一份「retire 发布 / game thread 排空」队列，现已合成 `FlightCompletionQueue`。
`WaitFrame` 不并入：记录本身挂在 flight 上，通知只需一个 bit，且排空必须只碰当前独占的那个
flight，与队列的全排空语义相反。

`BeginUpdateForFlight` 里三者的顺序是固定的：`PumpFlightCompletions` → `PumpWaitFrame` →
`PumpFrameUploadScheduler`。不要对调后两步——`PumpWaitFrame` 用 `WaitersCompleted.exchange(false)`
把当时表里的记录全部标成 `FlightComplete`；若先恢复上传协程，新注册的 `Wait()` 会被同一轮零等待恢复。

两条跨子系统前提支撑这个形状，不能从单个函数看出来：

1. **观察者回调不会产生同轮 `Wait()` 记录。** `DeferDestroy` 只 `_pendingDeferred.push_back`；
   `co_await Wait()` 发生在 `RunDeferredDestroy`，由 `FlushDeferredBatch` spawn，而后者的唯一调用点
   是 `AssetManager::Pump` → `Application::Update`，发生在 `BeginUpdateForFlight` 返回之后。
   因此 `PumpFlightCompletions` 排在 `PumpWaitFrame` 之前是安全的。上传协程恢复后直接执行游戏代码，
   不适用这条两层间接，所以 `PumpFrameUploadScheduler` 仍在 `PumpWaitFrame` 之后。
2. **当前 flight 的完成不可能在帧顶排空之后到达。** `TickFrame` 先取得可写槽位
   才算出 `flightIndex` 并 `BeginUpdateForFlight`；`RetireRenderedFrames` 只在 `CompleteFlightIfReady`
   成功后才 `release()`。`WaitWritableSlot` 在 `acquire` 前会 retire 已完成的 submitted flight
  （必要时等它的 fence），因此 `CompleteFlight(N)` 仍严格早于 game thread 取得 flight N 的可写槽位，
   帧顶一次排空就够，不必在 `PrepareFrameUploads` 再排一次。

## 上传

三层，按"数据从哪来"选：

| 设施 | 用途 |
|---|---|
| `ResourceUploader` | 经 staging buffer 上传到 device-local 资源。`UploadBuffer` / `UploadTexture` / `UploadMeshResource` |
| `DynamicCBufferArena` | 帧内常量缓冲切片。从映射上传页线性子分配，按 flight reset |
| `HostWriteBatch` + `ScopedBufferMap` | 直接写持久映射缓冲，收集写入范围后统一 flush |

`StagingBufferPool` 按 flight 分池，`CollectFlight` 在 fence 完成后回收。
`MappedUploadPage` 持久映射，`Reservation` 是仅可移动的映射切片，提交时记录实际写入范围。

### 从加载协程上传

`FrameUploadScheduler` 让加载协程能挂到帧顶的 upload phase：

```cpp
auto scope = co_await frameUploads.BeginUpload();   // 恢复点在帧顶 upload phase
uploader.UploadTexture(scope->GetCommandBuffer(), request);
co_await scope->WaitGpu();                          // 恢复点在该 flight fence 完成后
// 此处 GPU 已读完 staging，可以构造资产
```

`TextureAsset` 与 `StaticMesh` 的 loader 就走这条路。这样"构造即完整"得以兑现：
资产一出生即可被采样绑定。纹理解码、RGBA 转换和 mip 生成发生在 `BeginUpload` 之前，
upload phase 只分配 GPU 对象、复制已准备的 mip 数据并录制命令。CPU 准备仍同步发生在加载调用线程；
这里没有后台解码线程，不能将其误称为异步 I/O 或并行 mip 生成。
`FrameUploadScheduler::IsRecordingUploads` 标识当前阶段，纹理读取、解码和像素准备入口在 Debug
断言其为 false；上传协程在 BeginUpload 恢复后为 true，GPU 完成后的主线程恢复已离开此阶段。

取消发生在 upload phase 之前时，等待者可以立即退出；一旦开始录制，取消只标记请求，
`WaitGpu` 必须等对应 flight fence 完成后才终止加载协程。loader 的局部 GPU payload 因此会
活过所有已录制的复制命令。`UploadMeshResource` 先分配全部目标 buffer，再开始录制；任一
目标分配失败时，不留下引用局部资源的命令。

`FrameUploadScheduler` 不自持完成队列。它是 `GpuSystem` 共享队列的消费方：`PumpFlightCompletions`
先 `ApplyCompletedFlights`（只读 `.FlightIndex`，把对应 `AwaitingFence` 记录标成 `FenceComplete`），
`PumpCompletedUploads` 再恢复这些协程。`RunUploadPhase` 不再自排空。

帧顶一次排空足够：`TickFrame` 先取得可写槽位才算出 `flightIndex` 并
`BeginUpdateForFlight`；`RetireRenderedFrames` 只在 `CompleteFlightIfReady` 成功后才 `release()`。
因此 `CompleteFlight(N)` 严格早于 game thread 取得 flight N 的可写槽位，挂在 flight N 上的
`AwaitingFence` 记录在 `RunUploadPhase(N)` 之前已被 apply 并恢复。中途到达的其他 flight 完成
推迟到下一帧顶 apply；它们也只在下一帧的 `PumpFrameUploadScheduler` 才恢复，没有额外延迟。关停的 `WaitAndCleanupCompletedFlights` 在 GPU idle 后
也会 `PumpFlightCompletions` 再 pump 上传调度器，然后才能销毁 AssetManager 及其 task scope。

## 渲染资源的帧寿命

RenderSystem 通过同一 runtime 库内的私有 ShaderProgramCache 拥有 JIT、artifact/program cache；ShaderProgram 自持 PSO map。program 的
layout/参数 metadata 活过所有 flight，关停 GPU idle 后才销毁。

RenderSystem 的每个 flight 保存一张 StreamingAssetRefAny vector。game thread 取得可写 flight 后
清上一帧引用，再 Pump 资产；World tick 后 pipeline PrepareFrame 把本帧几何/纹理 owner 追加回来。
render thread 不操作引用计数，只读取 pipeline 私有值快照和被保活的 immutable asset payload。
Forward 的 `FrameDrawResources` 在下次安全复用时，先清空借用它的 renderer lists，再销毁 sets，
最后重置或裁减 arena；不会改写已发布的 backing set。精确缓存 key 与命令边界见
[Renderer foundation](renderer-foundation.md#renderer-lists-与帧内绘制资源)。

`RenderGraphRuntime` 的每个 flight 独立持有一个 `RenderGraphFrameResources`，其中聚合
texture/buffer/view pool、Graph parameter sets/cache 与 `DynamicCBufferArena`。`RenderSystem::Render`
开始时该 flight 的 fence 已完成，才按 parameter sets/cache → arena reset → pool BeginFlight trim/复用
的顺序清理。Graph setup 对象可以先析构；已准备 descriptor 与上传页仍活到 flight 安全复用。
CPU 编译工作空间也归各 flight 资源对象所有，复用其容量；编译结果独立于该工作空间。Graph 的
提交收据分别持有提交时状态写回数据与完成时资源，具体契约见 [Renderer foundation](renderer-foundation.md)。
`EndGraph` 不提前释放 GPU 对象。物理 resource states 保存至下次使用，transient 逻辑内容仍从无效开始。

`ViewStateRegistry` 在 render thread 跟踪稳定 view 身份和 history generations。替换/长期闲置的
generation 进入当前 flight retire bin，到同 flight 下次安全 Begin 才销毁。view 销毁前先调用
`RemoveFramebuffersUsing`。这依赖既有单 Direct queue 提交顺序，不新增 fence 或同步协议。
精确 key、内容提交和失败恢复规则见 [Renderer foundation](renderer-foundation.md)。

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

**`AcquireWindow` 不录任何 barrier。** 只有 workload 请求的 output 会被 acquire。graph 使用 backbuffer
真实初态；RenderSystem 对未写目标 fallback clear，并从实际末态收口到 Present。`AppFrameTarget`
只在 host 内部流动，也不暴露同步对象。离屏 external output 不参与 acquire/present，末态写回 output registry。

交换链尺寸变化时后备缓冲 view 会重建，此时必须调
`RenderPassRegistry::RemoveFramebuffersUsing(oldView)`：framebuffer 存的是 `TextureView`
裸指针。见 [render-rhi](render-rhi.md)。

## 关停顺序

`Application::Shutdown` 的顺序是固化的，每一步都有理由：

```cpp
_gpuSystem->WaitAndCleanupCompletedFlights();  // 等 GPU 静默
OnShutdown();                                  // 游戏侧释放自管 per-flight 资源
_scheduler.CancelAll();
_world.reset();                    // Actor → SceneProxy → drop StreamingAssetRef
_windowManager->SetRenderSystem(nullptr);  // RenderPassRegistry 即将销毁，先断引用
_renderSystem.reset();             // pipeline → graph pools → view states → refs/program → registry
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

`Application` 析构也复用幂等的内部 teardown，作为正常 `Shutdown` 被异常绕过时的保底；该路径
不调用派生类的 `OnShutdown`，但仍会 wait GPU、取消 scheduler、断开窗口引用并保持同一销毁顺序。

## 服务装配

Application 先创建对象，再把它们绑定到 `ServiceRegistry<...>` 的固定槽位。各系统通过
`ServiceTraits` 声明接口、依赖和静态钩子；编译期完成接口匹配与启动排序。
WindowManager 用 `Link` 保存 GPU/Render 引用，GpuSystem 对窗口使用 `Required`，
所以双向引用不会形成启动环。设备仍由 GpuSystem 构造函数创建，构造顺序不能任意交换。

可选实例、错误回滚、显式 Shutdown 与 Application 的所有权边界统一见
[ServiceRegistry](render-framework.md#serviceregistry)。GUID 与 RTTI 不参与这条装配路径。

## 游戏侧扩展点

底层负责"何时 tick、怎么 acquire/render/present"；游戏只负责"这个应用要画什么"。

| 钩子 | 时机 |
|---|---|
| `OnInit` | 全部内部系统就绪后一次。加载资产、Spawn Actor、建相机 |
| `OnUpdate` | 每帧，`AssetManager::Pump` 之后、`World::Tick` 之前 |
| `OnRenderFrameComplete` | game thread 消费 `FlightCompletion`，包括跳过和 shutdown；允许释放 GT 资产引用 |
| `OnShutdown` | 关停，游戏侧清理 |

`Application::Update` / `Render` / `Shutdown` 是**框架方法**（已固化帧序），不是 override 点。
