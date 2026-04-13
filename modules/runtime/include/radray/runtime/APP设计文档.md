先确认一个设计点：`FlightTracker` 应该**放在 AppWindow 上**而非 Application 上。每个窗口有独立的 swap chain，flight 进度独立。如果放在 Application 上，当窗口 A 的 GPU 仍在忙而窗口 B 空闲时，会互相等待。

---

## 完整主循环与调度设计

### 1. 数据结构调整

```cpp
class AppWindow {
public:
    unique_ptr<NativeWindow> _window;
    unique_ptr<GpuSurface> _surface;
    bool _isPrimary{false};

    // 帧追踪
    vector<std::optional<GpuTask>> _flightSlots;  // 大小=FlightFrameCount
    uint32_t _nextFlightSlot{0};
    uint64_t _renderFrameIndex{0};
    uint64_t _droppedFrameCount{0};

    // 待定状态
    bool _pendingResize{false};
    uint32_t _latchedWidth{0};
    uint32_t _latchedHeight{0};
};
```

### 2. 主循环

```
Run(argc, argv):
    OnInitialize()
    ASSERT: _gpu 已创建，至少有一个 primary 窗口
    初始化所有窗口的 _flightSlots（resize 到 FlightFrameCount，全部 nullopt）
    _timer.Restart()

    while (!_exitRequested):
        ──── Phase 1: 系统驱动 ────
        DispatchAllWindowEvents()     // 泵消息（一次就够，Win32 是 per-thread）
        CheckWindowStates()           // 捕获关闭/resize
        if (_exitRequested) break

        ──── Phase 2: 逻辑 ────
        UpdateFrameTiming()           // _deltaTime, _totalTime, _frameIndex++
        OnUpdate()
        if (_exitRequested) break

        ──── Phase 3: GPU 回收 ────
        if (_multiThreaded) DrainCompletions()
        _gpu->ProcessTasks()

        ──── Phase 4: Surface 变更 ────
        HandlePendingSurfaceChanges() // resize/recreate

        ──── Phase 5: 渲染调度 ────
        ScheduleFrames()

    ──── 关闭 ────
    if (_multiThreaded):
        WaitRenderIdle()
        DrainCompletions()
        StopRenderThread()
    WaitAllFlightFrames()             // 所有窗口的所有 slot Wait()
    _gpu->Wait(QueueType::Direct, 0)
    _gpu->ProcessTasks()
    OnShutdown()
    清理（销毁窗口、销毁 _gpu）
```

### 3. 渲染调度——单线程

```
ScheduleFrames():
    for each (handle, window) in _windows:
        if (!CanRender(window)) continue    // 最小化、无效、pendingResize 等跳过

        auto& slot = window._flightSlots[window._nextFlightSlot]

        // ---- Flight Slot 检查 ----
        if (slot.has_value()):
            if (_allowFrameDrop):
                if (!slot->IsCompleted()):
                    window._droppedFrameCount++
                    continue                 // 跳过该窗口本帧
                // 已完成，可以复用
            else:
                slot->Wait()                 // 阻塞等 GPU
                _gpu->ProcessTasks()
            slot.reset()

        uint32_t fi = window._nextFlightSlot

        // ---- Prepare（主线程）----
        OnPrepareRender(handle, fi)

        // ---- BeginFrame ----
        auto begin = _gpu->BeginFrame(window._surface.get())

        if (begin.Status == RetryLater):
            window._droppedFrameCount++
            continue
        if (begin.Status == RequireRecreate):
            window._pendingResize = true     // 下帧处理
            continue
        if (begin.Status == Error):
            _exitRequested = true
            return

        // ---- OnRender ----
        auto* ctx = begin.Context.Get()
        OnRender(handle, ctx, fi)

        // ---- Submit ----
        if (ctx->IsEmpty()):
            auto result = _gpu->AbandonFrame(begin.Context.Release())
            slot.emplace(std::move(result.Task))
            HandlePresentResult(handle, result.Present)
        else:
            auto result = _gpu->SubmitFrame(begin.Context.Release())
            slot.emplace(std::move(result.Task))
            HandlePresentResult(handle, result.Present)

        window._nextFlightSlot = (fi + 1) % window._flightSlots.size()
        window._renderFrameIndex++
```

### 4. 渲染调度——多线程

```
主线程 ScheduleFrames():
    for each (handle, window) in _windows:
        if (!CanRender(window)) continue

        auto& slot = window._flightSlots[window._nextFlightSlot]

        if (slot.has_value()):
            if (_allowFrameDrop):
                if (!slot->IsCompleted()):
                    window._droppedFrameCount++
                    continue
            else:
                // 等渲染线程完成 → completion 回来 → ProcessTasks → slot 可能完成
                WaitForRenderProgress()
                DrainCompletions()
                _gpu->ProcessTasks()
                if (slot.has_value() && !slot->IsCompleted()):
                    slot->Wait()
                    _gpu->ProcessTasks()
            slot.reset()

        uint32_t fi = window._nextFlightSlot

        // ---- 主线程准备数据 ----
        OnPrepareRender(handle, fi)

        // ---- 投递给渲染线程 ----
        KickRenderThread(RenderFrameCommand{
            .WindowHandle = handle,
            .Surface = window._surface.get(),
            .FlightIndex = fi,
        })

        window._nextFlightSlot = (fi + 1) % window._flightSlots.size()
        window._renderFrameIndex++


渲染线程 RenderThreadMain():
    while (true):
        auto cmd = WaitForCommand()        // wait on _renderKickCV
        if (cmd is StopCommand) break

        GpuRuntime::BeginFrameResult begin
        {
            lock(_gpuMutex)
            begin = _gpu->BeginFrame(cmd.Surface)   // 或 TryBeginFrame
        }

        RenderCompletion completion{.WindowHandle = cmd.WindowHandle, .FlightIndex = cmd.FlightIndex}

        if (begin.Status != Success):
            completion.BeginStatus = begin.Status
            PushCompletion(completion)
            continue

        // OnRender 在渲染线程执行，不需要 _gpuMutex
        OnRender(cmd.WindowHandle, begin.Context.Get(), cmd.FlightIndex)

        {
            lock(_gpuMutex)
            if (begin.Context.Get()->IsEmpty()):
                auto result = _gpu->AbandonFrame(begin.Context.Release())
                completion.Task.emplace(std::move(result.Task))
                completion.PresentResult = result.Present
            else:
                auto result = _gpu->SubmitFrame(begin.Context.Release())
                completion.Task.emplace(std::move(result.Task))
                completion.PresentResult = result.Present
        }

        completion.BeginStatus = SwapChainStatus::Success
        PushCompletion(completion)


主线程 DrainCompletions():
    lock(_renderMutex)
    swap completions out

    for each completion:
        auto* window = _windows.TryGet(completion.WindowHandle)
        if (!window) continue

        switch (completion.BeginStatus):
            Success:
                window->_flightSlots[completion.FlightIndex] = std::move(*completion.Task)
                HandlePresentResult(completion.WindowHandle, completion.PresentResult)
            RetryLater:
                window->_droppedFrameCount++
            RequireRecreate:
                window->_pendingResize = true
            Error:
                _exitRequested = true
```

### 5. 多线程切换

```
SetMultiThreaded(bool enable):
    if (enable == _multiThreaded) return
    _pendingSwitchThread = enable    // 标记，不立即执行

// 在主循环 Phase 4 (HandlePendingSurfaceChanges) 之后检查：
if (_pendingSwitchThread.has_value()):
    bool enable = *_pendingSwitchThread
    _pendingSwitchThread.reset()

    if (enable && !_multiThreaded):
        // 启动
        _multiThreaded = true
        _renderThread = std::thread(&Application::RenderThreadMain, this)

    else if (!enable && _multiThreaded):
        // 停止
        WaitRenderIdle()
        DrainCompletions()
        StopRenderThread()          // KickRenderThread(StopCommand) + join
        _multiThreaded = false
```

**安全约束**：切换时机在 Phase 4 之后、Phase 5 之前，此时没有 in-flight 的渲染命令。

### 6. Frame Drop 完整决策树

```
                     Flight Slot 是否被占用？
                    /                       \
                  否                         是
                  │                          │
            正常渲染                  _allowFrameDrop?
                                   /              \
                                 true             false
                                  │                │
                          IsCompleted()?      Wait() 阻塞
                          /          \             │
                        true        false       复用 slot
                         │            │         正常渲染
                    复用 slot      跳过(drop)
                    正常渲染      DroppedCount++
```

然后 BeginFrame 还有第二层丢帧：

```
            BeginFrame 返回状态
            /     |      |       \
        Success  Retry  Recreate  Error
           │      │       │        │
        继续   drop++   标记      退出
        渲染            resize
```

### 7. 关键时序图

**单线程 2 in-flight（_allowFrameDrop=false）**：
```
GPU:    [==Render F0==]  [==Render F1==]  [==Render F0==]
CPU:  [Prep0][Render0]  [Prep1][Render1]  [Wait F0][Prep0][Render0]
      ─────────────────────────────────────────────────────────▶
      flight0            flight1           flight0(复用)
```

**多线程 2 in-flight**：
```
主线程:   [Update][Prep0][Kick]  [Update][Prep1][Kick]  [Update][Prep0]...
渲染线程:               [Begin0][Render0][Submit0]  [Begin1][Render1][Submit1]
          ───────────────────────────────────────────────────────────▶
```

**多线程 + Frame Drop**：
```
主线程:   [Update][Prep0][Kick]  [Update]  slot1 occupied→skip  [Update][Prep1]...
渲染线程:               [======长渲染 F0======][Submit0]  [Begin1]...
                                    ↑ 主线程不等，直接跳过
```

### 8. _gpuMutex 加锁范围

| 操作 | 线程 | 需要 _gpuMutex |
|---|---|---|
| `BeginFrame` / `TryBeginFrame` | 渲染线程 | ✅ |
| `SubmitFrame` / `AbandonFrame` | 渲染线程 | ✅ |
| `ProcessTasks` | 主线程 | ✅ |
| `CreateSurface` | 主线程 | ✅ |
| `Wait` | 主线程 | ✅ |
| `OnRender`（命令录制） | 渲染线程 | ❌（不需要） |
| `OnPrepareRender` | 主线程 | ❌ |

单线程模式下**不需要 _gpuMutex**，省去所有锁开销。