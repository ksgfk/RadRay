> - 适用: 改帧节奏、提交时序、GPU 上传；排查"GPU 对象被提前销毁"或帧同步问题
> - 权威: 本文是帧节奏与 GPU 资源上传的唯一说明。资产侧的延迟销毁契约见 `architecture/asset-system.md`；RHI 本身见 `architecture/render-rhi.md`
> - 锚点: `modules/runtime/include/radray/runtime/gpu_system.h`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/include/radray/runtime/wait_frame.h`, `modules/runtime/include/radray/runtime/render_system.h`, `modules/runtime/src/gpu_system.cpp`

# 帧节奏与 GPU 上传

## 职责边界

| 系统 | 负责 | 不负责 |
|---|---|---|
| `GpuSystem` | **何时画**。instance/factory/device/主队列/fence、flight 槽位、上传器、帧 profiler、帧边界等待表 | 画什么 |
| `RenderSystem` | **画什么**。pipeline、workload/output、graph pools/history、program/artifact cache、RenderPass/Framebuffer registry、game-thread Scene 与 per-flight asset refs | GPU 提交时序 |
| `WindowManager` | 窗口创建/销毁、swapchain acquire/present/recreate、事件分发 | — |
| `Application` | 固化帧序与关停顺序；游戏侧的窄扩展点 | — |

## 帧序

```
Application::StartLoop
  ├─ BeginUpdateForFlight(flight)     取得该 flight 的可写槽位；PumpWaitFrame
  ├─ RenderSystem::BeginUpdateForFlight 清除该 flight 上一帧的 retained asset refs 和 frame plan
  ├─ AssetManager::Pump               提交加载结果；销毁零引用资产
  ├─ Application::OnUpdate            游戏逻辑
  ├─ World::Tick
  ├─ RenderSystem::PrepareFrame        game thread 复制 pipeline input 并构造 view families
  ├─ GpuSystem::BeginFrameRecord      取/建 CommandBuffer 并 Begin()；清上帧 targets
  │    ├─ upload phase                FrameUploadScheduler 恢复等待帧顶的协程
  │    └─ GpuFrameProfiler::BeginFrame
  ├─ Application::Render              → pool/history BeginFlight → output/view resolve → pipeline graph → host finalize
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

## 渲染资源的帧寿命

RenderSystem 继续拥有 ShaderJit、artifact/program cache；ShaderProgram 自持 PSO map。program 的
layout/参数 metadata 活过所有 flight，关停 GPU idle 后才销毁。

RenderSystem 的每个 flight 保存一张 StreamingAssetRefAny vector。game thread 取得可写 flight 后
清上一帧引用，再 Pump 资产；World tick 后 pipeline PrepareFrame 把本帧几何/纹理 owner 追加回来。
render thread 不操作引用计数，只读取 pipeline 私有值快照和被保活的 immutable asset payload。
Forward 的 frame-local sets 在下次录制复用时先销毁，再重置或裁减 arena；不会改写旧 backing set。

`RenderGraphRuntime` 的每个 flight 独立持有 texture/buffer/view pool。`RenderSystem::Render` 开始时，
该 flight 的 fence 已完成，才调用 pool `BeginFlight` trim/复用。`EndGraph` 不提前释放 GPU 对象。
物理 resource states 保存至下次使用，transient 逻辑内容仍从无效开始。

`ViewStateRegistry` 在 render thread 跟踪稳定 view 身份和 history generations。替换/长期闲置的
generation 进入当前 flight retire bin，到同 flight 下次安全 Begin 才销毁。view 销毁前先调用
`RemoveFramebuffersUsing`。这依赖既有单 Direct queue 提交顺序，不新增 fence 或同步协议。
精确 key、内容提交和失败恢复规则见 [Renderer foundation](renderer-foundation.md)。

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

**`AcquireWindow` 不录任何 barrier。** 只有 workload 请求的 output 会被 acquire。graph 使用 backbuffer
真实初态；RenderSystem 对未写目标 fallback clear，并从实际末态收口到 Present。`AppFrameTarget`
只在 host 内部流动，也不暴露同步对象。离屏 external output 不参与 acquire/present，末态写回 output registry。

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
| `OnRenderFrameComplete` | 帧完成通知 |
| `OnShutdown` | 关停，游戏侧清理 |

`Application::Update` / `Render` / `Shutdown` 是**框架方法**（已固化帧序），不是 override 点。
