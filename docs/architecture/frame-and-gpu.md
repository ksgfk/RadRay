> - 适用: 改帧节奏、提交时序、GPU 上传；排查"GPU 对象被提前销毁"或帧同步问题
> - 权威: 本文是帧节奏与 GPU 资源上传的唯一说明。资产侧的延迟销毁契约见 `architecture/asset-system.md`；RHI 本身见 `architecture/render-rhi.md`
> - 锚点: `modules/runtime/include/radray/runtime/gpu_system.h`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/include/radray/runtime/wait_frame.h`, `modules/runtime/src/gpu_system.cpp`

# 帧节奏与 GPU 上传

## 职责边界

| 系统 | 负责 | 不负责 |
|---|---|---|
| `GpuSystem` | **何时画**。instance/factory/device/主队列/fence、flight 槽位、上传器、帧 profiler、帧边界等待表 | 画什么 |
| `RenderSystem` | **画什么**。RenderPass/Framebuffer 缓存、PSO 缓存、PipelineLayout 缓存、Scene 列表、DXC | 帧时序 |
| `WindowManager` | 窗口创建/销毁、swapchain acquire/present/recreate、事件分发 | — |
| `Application` | 固化帧序与关停顺序；游戏侧的窄扩展点 | — |

## 帧序

```
Application::StartLoop
  ├─ BeginUpdateForFlight(flight)     取得该 flight 的可写槽位；PumpWaitFrame
  ├─ AssetManager::Pump               提交加载结果；销毁零引用资产
  ├─ Application::OnUpdate            游戏逻辑
  ├─ World::Tick
  ├─ GpuSystem::BeginFrameRecord      取/建 CommandBuffer 并 Begin()；清上帧 targets
  │    ├─ upload phase                FrameUploadScheduler 恢复等待帧顶的协程
  │    └─ GpuFrameProfiler::BeginFrame
  ├─ Application::Render              → RenderSystem::Render → RenderPipeline::Render
  └─ GpuSystem::EndFrameRecordAndSubmit
       uploader.EndFlight → CmdBuffer.End → 聚合 sync object → Submit
       → 写 flight.Signal → Present 全部 target
```

`CompleteFlight` 在 fence 完成后跑：resolve profiler、`CollectFlight` 回收 staging、
标记该 flight 上的 `WaitFrameRecord`。多线程模式下它在渲染线程
（`ThreadedRunner::RetireRenderedFrames`）。

## Flight 槽位

`GpuFlightSlot` 按所有权/阶段分三组，跨阶段的访问时序由 runner 的信号量 + retire 锁保证：

| 组 | 成员 | 谁访问 |
|---|---|---|
| 录制态 | `CmdBuffer`, `HostWrites`, `Targets`, `Submitted`, `Recording` | 渲染线程独占（单线程模式即主线程），`BeginFrameRecord`→`Render`→`EndFrameRecordAndSubmit` 期间 |
| 计时态 | `FrameStartTime` | 游戏线程在帧开头写 |
| 提交态 | `Signal` | `EndFrameRecordAndSubmit` 写；retire/`CompleteFlight` 经 `_retireMutex` 读后清 |
| 等待表 | `WaitFrame` | 见下 |

`_flights` 是 `vector<unique_ptr<FlightSlot>>` 而不是 `vector<FlightSlot>`：`FlightSlot`
内含 `ManualCoroutineScheduler`，它不可拷贝也不可移动——挂起的协程记录里存着回指调度器的
指针（stop callback），搬动槽位会让那些指针指向旧地址。数量在构造时定下且此后不变，
故间接一层不带来任何代价。

## 帧边界等待

这是 GPU 资源延迟销毁的机制。核心设计（不交对象，交出挂起点）见
[ADR-0009](../adr/0009-deferred-destroy-hands-over-suspension.md)，使用侧见
`architecture/asset-system.md`。实现侧三条：

**等待表挂在 per-flight 槽位上**（`GpuFlightSlot::WaitFrame`），不是全局表。flight 的
fence 就是完成条件，记在槽位上便无需另存 fence 值再逐个比较。

**两段式恢复。** `CompleteFlight` 只把记录标记 `FlightComplete`（它可能跑在渲染线程），
真正的 resume 由主线程的 `PumpWaitFrame` 做。

**`Wait()` 挂进当前 flight，不是"上一次提交的 fence 值"。** 调用点在帧顶 Update 期间，
此刻当前 flight 还没开始录制，故"已录制的 work"全都属于更早的 flight——等当前 flight 的
fence 必然晚于它们完成。这样就不必记录并比较 fence 值，代价是最多多等一轮
（口径本就允许多等）。

**`PumpWaitFrame` 只泵一个 flight，且必须是调用线程当前独占的那个。** 多线程模式下渲染
线程会在 `CompleteFlight` 里标记别的 flight 的记录，若在此扫全表就与之竞争。调用点固定在
`BeginUpdateForFlight`——那一刻 runner 刚拿到该 flight 的可写槽位，该 flight 上一轮的
fence 必然已完成、记录必然已被标记，且此后到下一次 `BeginUpdateForFlight` 之间只有本线程
访问它。

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
    static constexpr auto Inject = std::tuple{&MyCache::SetWaitFrameProcessor};
};
```

`GpuSystem` 已经用 `Bases = std::tuple<IWaitFrameProcessor>` 声明了这个基类，
所以 `Wire()` 能解析到它。装配细节见「服务装配」一节。

目前仓库里只有资产走这条路，所以没有现成的非资产调用点可参照。

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
资产一出生即可被采样绑定。

## PSO 缓存

`PipelineStateCache` 的 key 是 `GraphicsPipelineStateKey`：

```
Program(ShaderPassProgram*) + CompatibleRenderPass + Primitive + DepthStencil
+ MultiSample + ColorTargets
```

加上 program 内解析出的各 stage `ShaderHash`。key 比字节码宽——同一份字节码会喂给多个 PSO
（不同 blend/cull/RT 格式）。这是 PSO 归 `RenderSystem` 而字节码归 `ShaderPassProgram`
的原因，见 `architecture/shader-pipeline.md`。

条目只持一个 `StreamingAssetRefAny`，同时保住资产 + program + layout。`Ref` 必须声明在
`Object` 之前（析构逆序保证 PSO 先死）。理由见 `architecture/asset-system.md`。

## 帧 profiler

`GpuFrameProfiler` 对应 UE5 的 `FGPUTiming`（最小化）：per-flight timestamp pool + readback。
由 `GpuSystem` 在 `BeginFrameRecord`/`EndFrameRecordAndSubmit` 自动包裹本帧录制，
`CompleteFlight` 时 resolve。应用只读 `GetLastGpuTimeMs()`。后端 readback barrier 差异
内部隐藏。

## 呈现

`AppFrameContext::AcquireWindow(window)` 内部 `AcquireNextSwapChainFrame`：
`RequireRecreate` / `RetryLater` / `Error` / 最小化 → `nullopt`（应用跳过该窗口）。
成功时把 `SwapChainFrame` 收进本帧 `FlightSlot`，返回 `AppFrameTarget`
（backbuffer + view + index）。

**`AcquireWindow` 不录任何 barrier。** backbuffer 的初始翻转与 →Present 收尾全部由应用
显式录。`AppFrameTarget` 也**不暴露同步对象**——sync object 是提交细节，由 runtime 独占。

交换链尺寸变化时后备缓冲 view 会重建，此时必须调
`RenderPassRegistry::RemoveFramebuffersUsing(oldView)`：framebuffer 存的是 `TextureView`
裸指针。见 `architecture/render-rhi.md`。

## 关停顺序

`Application::Shutdown` 的顺序是固化的，每一步都有理由：

```cpp
_gpuSystem->WaitAndCleanupCompletedFlights();  // 等 GPU 静默
OnShutdown();                                  // 游戏侧释放自管 per-flight 资源
_scheduler.CancelAll();
_world.reset();                    // Actor → SceneProxy → drop StreamingAssetRef
_windowManager->SetRenderSystem(nullptr);  // RenderPassRegistry 即将销毁，先断引用
_renderSystem.reset();             // 持有 Scene，须长于 World 的拆解
_assetManager.reset();             // 放开全部资产，GPU buffer 须在 device 前释放
_windowManager->DetachAllSwapChains();
_windowManager->SetGpuSystem(nullptr);
_gpuSystem->SetWindowManager(nullptr);
_gpuSystem.reset();                // device 最后死
_windowManager.reset();
```

关键约束：**`AssetManager` 必须在 `GpuSystem` 之前销毁**（GPU 资源必须在 device 之前交出），
而 `PipelineLayoutCache`（宿主 `RenderSystem`）会因此先于它的持有者销毁——那是常规路径，
缓存的析构会把残留条目的所有权交还给它们自己。

## 服务装配

三阶段（实例化 → 装配 → 初始化），由 `ServiceRegistry` 驱动。构造函数只做平凡/自身初始化，
不碰兄弟系统，故 phase 1 的顺序任意；互相引用（`WindowManager` ↔ `GpuSystem`）在 phase 2
装配时全部实例已存在，天然可解。

```cpp
template <> struct ServiceTraits<GpuSystem> {
    static constexpr auto Inject = std::tuple{&GpuSystem::SetWindowManager};
};
template <> struct ServiceTraits<AssetManager> {
    static constexpr auto Inject = std::tuple{&AssetManager::SetWaitFrameProcessor};
};
```

`AssetManager` 要的是 `IWaitFrameProcessor` 接口，由 `ServiceRegistry` 通过
`RuntimeTypeTrait<GpuSystem>::Bases` 解析到 `GpuSystem`。所以给 `GpuSystem` 的
`RuntimeTypeTrait` 加/删基类会影响装配能否解析。

## 游戏侧扩展点

底层负责"何时 tick、怎么 acquire/render/present"；游戏只负责"这个应用要画什么"。

| 钩子 | 时机 |
|---|---|
| `OnInit` | 全部内部系统就绪后一次。加载资产、Spawn Actor、建相机 |
| `OnUpdate` | 每帧，`AssetManager::Pump` 之后、`World::Tick` 之前 |
| `OnRenderView` | 录制 view/window 场景内容。返回 true 表示已写 backbuffer |
| `OnRenderFrameComplete` | 帧完成通知 |
| `OnShutdown` | 关停，游戏侧清理 |

`Application::Update` / `Render` / `Shutdown` 是**框架方法**（已固化帧序），不是 override 点。
