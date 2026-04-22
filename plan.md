# RadRay 双线程帧循环实施方案

## 一、现状与目标

当前单线程模型 (`Application::Run`, application.cpp:146-181):

```
Main Thread:
  DispatchAllWindowEvents()        // OS 消息泵
  CheckWindowStates()              // 检测 resize/close
  OnUpdate()                       // 游戏逻辑
  ProcessTasks()                   // GPU 资源回收
  HandleSurfaceChanges()           // 重建 swapchain
  ScheduleFramesSingleThreaded()   // prepare → render → submit 全串行
```

设计亮点：Mailbox 状态机逻辑上已为解耦 update/render 而设计；flight frame 管理正确；`_allowFrameDrop` + TryBeginFrame 非阻塞路径已有；错误恢复路径完整。

核心瓶颈：CPU-GPU 流水线气泡。OnUpdate 期间 GPU 空闲，GPU 执行期间 CPU 空闲，渲染负载重时帧率减半。

目标：将 prepare 和 render 分到两个线程流水线执行，消除气泡。

## 二、设计

### 2.1 回调并发契约

双线程模式下 `OnPrepareRender` 在主线程执行，`OnRender` 在渲染线程执行，两者可以并发。框架通过 mailbox slot 隔离数据：每个 slot 在同一时刻只被一个线程操作。但框架不保护用户自有的共享状态。

契约规则：
- `OnPrepareRender(window, slot)`: 主线程调用。可安全访问游戏状态。只应向 `slot` 对应的 extract 数据写入，不应读写其他 slot 的数据。
- `OnRender(window, context, slot)`: 渲染线程调用。只应读取 `slot` 对应的 extract 数据和 render-thread-owned 状态（如 RTV 缓存）。不应访问游戏状态或其他 slot。
- 用户跨回调共享的状态（计数器、标志等）必须由用户自行同步（`std::atomic`、mutex 等）。
- 通过 `GetDevice()` 等 escape hatch 获得的 borrowed 对象不在 `GpuRuntime` 线程安全保证内（gpu_system.h:63-64）。在 `OnRender` 中使用这些对象时，用户需自行确保线程安全。
- `GpuRuntime` 的公开成员函数（`CreateTextureView`、`CreateBuffer` 等）可被多线程并发调用，内部已串行化。

application.h 中回调声明需更新注释：
```cpp
/// 主线程调用。准备 mailbox slot 对应的渲染快照。
/// 多线程模式下与 OnRender 并发执行，只应写入 slot 对应的 extract 数据。
virtual void OnPrepareRender(AppWindowHandle window, uint32_t mailboxSlot) = 0;

/// 单线程模式下由主线程调用，多线程模式下由渲染线程调用。
/// 录制渲染命令并消费 slot 中的渲染快照。
/// 多线程模式下不应访问游戏状态；escape hatch (GetDevice() 等) 需用户自行同步。
virtual void OnRender(AppWindowHandle window, GpuFrameContext* context, uint32_t mailboxSlot) = 0;
```

### 2.2 总体流程

```
Game Thread (main):                      Render Thread:
  ApplyPendingThreadMode()                 while (true) {
  DispatchAllWindowEvents()                  unique_lock(_renderWakeMutex)
  CheckWindowStates()                        wait_for(16ms, pred: frame||pause||stop)
  OnUpdate()                                 if (stop) return
  for each window:                           if (pause) {
    lock(mailboxMutex)                         _renderPaused = true
      SampleWindowState → snapshot             notify _pauseAckCV
      slot = ReserveMailboxSlot()              wait(!pause || stop)
      Free/Published → Preparing               continue
                                            }
    unlock                                   hasFrame = _newFramePending
    OnPrepareRender(window, slot)            _newFramePending = false
    lock(mailboxMutex)                       unlock
      Preparing → Published                  RenderAllWindows()  // 见 2.7
    unlock                                 }
  lock(_renderWakeMutex)
    _newFramePending = true
  unlock
  notify _renderWakeCV
  ProcessTasks()  // 持 _renderMutex
  HandleSurfaceChanges()
    ↑ 需要时:
      PauseRenderThread()
      重建 swapchain
      ResumeRenderThread()
```

### 2.3 Mailbox 状态机

新增 `Preparing` 状态，防止主线程写入期间 slot 被渲染线程消费：

```
Free ──ReserveMailboxSlot──→ Preparing ──PublishPreparedMailbox──→ Published
  ↑                                                                   │
  │                                                   ConsumePublished│
  │                                                                   ↓
  ←──────────── CollectCompleted ──────────── InRender ←────────── Queued
  ↑             (flight 完成后)              (BeginFrame 成功)          │
  │                                                                   │
  ←──── RestoreOrRelease (已被超越时直接 Free) ──────────────────────────┘
                         (仍是最新时恢复 Published)
```

```cpp
enum class MailboxState { Free, Preparing, Published, Queued, InRender };
```

所有状态转换由 per-window `_mailboxMutex` 保护。

覆盖 Published 的语义：`ReserveMailboxSlot` 在单线程和多线程模式下均允许覆盖 Published。在多线程模式下，覆盖和消费都在同一把 `_mailboxMutex` 下执行，天然串行化，不存在竞争。真正的不变量是"不能覆盖 Queued 或 InRender"，这由状态机保证。禁止覆盖 Published 会在 `2×InRender + 1×Published` 场景下阻塞主线程 prepare，破坏 `_allowFrameDrop` 的 frame coalescing 能力。

### 2.4 per-window 同步协议

每个 `AppWindow` 的 `_mailboxMutex` 统一保护以下字段：

- `_mailboxes`（状态数组）
- `_latestPublishedMailboxSlot` / `_latestPublishedGeneration`
- `_flights`（flight task 数组）
- `_stateSnapshot`（窗口状态镜像）

```cpp
class AppWindow {
public:
    AppWindow() noexcept
        : _mailboxMutex(make_unique<std::mutex>()) {}

    unique_ptr<std::mutex> _mailboxMutex;
    std::atomic<bool> _pendingRecreate{false};

    struct StateSnapshot {
        bool isValid{false};
        bool shouldClose{false};
        bool isMinimized{false};
        int32_t width{0};
        int32_t height{0};
    };
    StateSnapshot _stateSnapshot{};

    std::mutex& GetMailboxMutex() const { return *_mailboxMutex; }
};
```

`_mailboxMutex` 通过 `make_unique<std::mutex>()` 初始化，move 时 `unique_ptr` 自然转移所有权。

`_stateSnapshot` 纳入 mutex：主线程每帧写 snapshot，渲染线程读 snapshot，两者可并发。将 snapshot 放在 mailbox mutex 下，主线程在 Reserve 前写入，渲染线程在 Consume 时拷贝到局部变量，读写严格互斥。

多线程稳态下 `_flights` 仅由渲染线程读写：主线程 prepare 流程不再调用 `CollectCompletedFlightTasks()`。主线程只在渲染线程已 pause 的重建/销毁路径中整体重置 `_flights`。这样渲染线程可以在锁外等待 fence，而不会与主线程对 `_flights` 的 reset 竞争。

`_pendingRecreate` 用 `std::atomic<bool>`：渲染路径的 `HandlePresentResult` 和 BeginFrame 失败也会写它，这些路径不在 mailbox mutex 临界区内。

### 2.5 关键操作实现

**ReserveMailboxSlot**（主线程，持 mailbox mutex）：
```cpp
std::optional<uint32_t> AppWindow::ReserveMailboxSlot() noexcept {
    // 调用方已持 _mailboxMutex
    for (uint32_t i = 0; i < _mailboxes.size(); ++i) {
        if (_mailboxes[i]._state == MailboxState::Free) {
            _mailboxes[i]._state = MailboxState::Preparing;
            return i;
        }
    }
    // 无 Free 时覆盖 Published（单线程/多线程均安全）
    if (_latestPublishedMailboxSlot.has_value()) {
        auto slot = *_latestPublishedMailboxSlot;
        if (_mailboxes[slot]._state == MailboxState::Published) {
            _mailboxes[slot]._state = MailboxState::Preparing;
            return slot;
        }
    }
    return std::nullopt;
}
```

不再需要 `allowOverwritePublished` 参数。覆盖 Published 在 mutex 保护下对两种模式都安全。

**PublishPreparedMailbox**（主线程，持 mailbox mutex）：
```cpp
void AppWindow::PublishPreparedMailbox(uint32_t mailboxSlot) noexcept {
    // 调用方已持 _mailboxMutex
    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    auto& mailbox = _mailboxes[mailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Preparing);

    const auto superseded = this->GetPublishedMailboxSlot();
    const uint64_t nextGen = _latestPublishedGeneration + 1;
    mailbox._state = MailboxState::Published;
    mailbox._generation = nextGen;
    _latestPublishedMailboxSlot = mailboxSlot;
    _latestPublishedGeneration = nextGen;

    if (superseded.has_value() && *superseded != mailboxSlot) {
        this->ReleaseMailbox(*superseded);
    }
}
```

**ConsumePublishedMailboxSlot**（渲染线程，持 mailbox mutex）：
```cpp
std::optional<uint32_t> AppWindow::ConsumePublishedMailboxSlot() noexcept {
    // 调用方已持 _mailboxMutex
    if (!_latestPublishedMailboxSlot.has_value()) return std::nullopt;
    auto slot = *_latestPublishedMailboxSlot;
    if (_mailboxes[slot]._state != MailboxState::Published) return std::nullopt;
    _mailboxes[slot]._state = MailboxState::Queued;
    return slot;
}
```

**RestoreOrReleaseMailbox**（渲染线程，持 mailbox mutex）：

单线程下 `RestoreMailbox()` 无条件恢复 Queued → Published 是正确的。但双线程下，渲染线程从 Consume 解锁到 Restore 重新加锁之间，主线程可能已 publish 了更新的快照。无条件 restore 会回退 `_latestPublished*`，导致新快照成为孤儿 slot。

修正：restore 时对比 generation，已被超越则直接 release：

```cpp
void AppWindow::RestoreOrReleaseMailbox(uint32_t mailboxSlot) noexcept {
    // 调用方已持 _mailboxMutex
    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    auto& mailbox = _mailboxes[mailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Queued);

    if (mailbox._generation >= _latestPublishedGeneration) {
        mailbox._state = MailboxState::Published;
        _latestPublishedMailboxSlot = mailboxSlot;
        _latestPublishedGeneration = mailbox._generation;
    } else {
        mailbox._state = MailboxState::Free;
        mailbox._generation = 0;
    }
}
```

单线程路径可继续使用原始 `RestoreMailbox()`，也可统一使用 `RestoreOrReleaseMailbox()`（generation 比较在单线程下总是 `>=`，行为等价）。

### 2.6 主线程 prepare 流程（每个窗口）

```cpp
std::optional<uint32_t> reservedSlot;
{
    std::lock_guard lock{window.GetMailboxMutex()};
    window._stateSnapshot = SampleOneWindow(window);
    reservedSlot = window.ReserveMailboxSlot();
    if (!reservedSlot) continue;
}
// mutex 外执行用户回调
this->OnPrepareRender(window._selfHandle, *reservedSlot);
{
    std::lock_guard lock{window.GetMailboxMutex()};
    window.PublishPreparedMailbox(*reservedSlot);
}
```

### 2.7 渲染线程消费流程（完整锁边界）

这是对原方案 P1-3 问题的完整修正。每个窗口的处理流程，标注了每个关键步骤的锁状态：

```cpp
// ═══ Step 1: 消费 Published，拷贝 snapshot ═══
AppWindow::StateSnapshot localSnapshot;
std::optional<uint32_t> mailboxSlot;
uint32_t flightSlot;
{
    std::lock_guard lock{window.GetMailboxMutex()};           // ← 加锁
    window.CollectCompletedFlightTasks();
    mailboxSlot = window.ConsumePublishedMailboxSlot();       // Published → Queued
    if (!mailboxSlot.has_value()) continue;
    localSnapshot = window._stateSnapshot;
    flightSlot = static_cast<uint32_t>(window._surface->_nextFrameSlotIndex);
}                                                             // ← 解锁

// ═══ Step 2: CanRender 判断（无锁，使用局部变量）═══
if (!CanRender(localSnapshot, window._pendingRecreate.load())) {
    std::lock_guard lock{window.GetMailboxMutex()};           // ← 加锁
    window.RestoreOrReleaseMailbox(*mailboxSlot);
    continue;                                                 // ← 解锁
}

// ═══ Step 3~8: flight 等待 / BeginFrame / OnRender / Submit / Present（统一异常收口）═══
enum class RenderStage {
    QueuedNoFrame,        // mailbox = Queued, 尚未成功 BeginFrame
    FrameAcquired,        // BeginFrame 成功, frameContext 已持有
    MailboxInRender,      // mailbox = InRender, 尚未 submit
    SubmitRecorded        // submit 成功且已写回 flight, 等待 HandlePresentResult
};

RenderStage stage = RenderStage::QueuedNoFrame;
unique_ptr<GpuFrameContext> frameContext;

try {
    // Step 3: flight 等待/跳过
    // 先在锁内提取稳定的 wait token，解锁后再 Wait，避免持锁阻塞主线程 prepare
    bool flightBusy = false;
    bool flightCompleted = false;
    render::Fence* waitFence = nullptr;
    uint64_t waitSignalValue = 0;
    {
        std::lock_guard lock{window.GetMailboxMutex()};       // ← 加锁
        auto& flightData = window._flights[flightSlot];
        if (flightData._task.has_value()) {
            flightBusy = true;
            flightCompleted = flightData._task->IsCompleted();
            if (!flightCompleted) {
                waitFence = flightData._task->_fence;
                waitSignalValue = flightData._task->_signalValue;
            }
            if (_allowFrameDrop && !flightCompleted) {
                window.RestoreOrReleaseMailbox(*mailboxSlot);
                continue;                                     // ← 解锁, 跳过
            }
        }
    }                                                         // ← 解锁

    if (flightBusy && !flightCompleted) {
        // !allowFrameDrop: 在锁外等待 GPU 完成，不阻塞主线程
        RADRAY_ASSERT(waitFence != nullptr && waitSignalValue != 0);
        waitFence->Wait(waitSignalValue);
        {
            std::lock_guard renderLock{_renderMutex};
            _gpu->ProcessTasks();
        }
    }

    if (flightBusy) {
        std::lock_guard lock{window.GetMailboxMutex()};       // ← 加锁
        auto& flightData = window._flights[flightSlot];
        RADRAY_ASSERT(!flightData._task.has_value() || flightData._task->IsCompleted());
        if (flightData._mailboxSlot.has_value()) {
            window.ReleaseMailbox(*flightData._mailboxSlot);
            flightData._mailboxSlot.reset();
        }
        flightData._task.reset();
    }                                                         // ← 解锁

    // Step 4: BeginFrame（无锁，GPU 操作）
    GpuRuntime::BeginFrameResult begin{};
    if (_allowFrameDrop) {
        begin = _gpu->TryBeginFrame(window._surface.get());
    } else {
        begin = _gpu->BeginFrame(window._surface.get());
    }

    if (begin.Status != SwapChainStatus::Success) {
        std::lock_guard lock{window.GetMailboxMutex()};       // ← 加锁
        window.RestoreOrReleaseMailbox(*mailboxSlot);
        if (begin.Status == SwapChainStatus::RequireRecreate) {
            window._pendingRecreate.store(true);
        }
        continue;                                             // ← 解锁
    }

    stage = RenderStage::FrameAcquired;
    frameContext = begin.Context.Release();

    // Step 5: Queued → InRender（加锁）
    {
        std::lock_guard lock{window.GetMailboxMutex()};       // ← 加锁
        window._mailboxes[*mailboxSlot]._state = MailboxState::InRender;
    }                                                         // ← 解锁
    stage = RenderStage::MailboxInRender;

    // Step 6: OnRender（无锁，用户回调）
    this->OnRender(window._selfHandle, frameContext.get(), *mailboxSlot);

    // Step 7: Submit（无锁，GPU 操作）
    auto submit = frameContext->IsEmpty()
                      ? _gpu->AbandonFrame(std::move(frameContext))
                      : _gpu->SubmitFrame(std::move(frameContext));

    // Step 8: 写回 flight 数据（加锁）
    {
        std::lock_guard lock{window.GetMailboxMutex()};       // ← 加锁
        window._flights[flightSlot]._task.emplace(std::move(submit.Task));
        window._flights[flightSlot]._mailboxSlot = *mailboxSlot;
    }                                                         // ← 解锁
    stage = RenderStage::SubmitRecorded;

    this->HandlePresentResult(window, submit.Present);
} catch (...) {
    auto ex = std::current_exception();

    {
        std::lock_guard lock{window.GetMailboxMutex()};
        switch (stage) {
            case RenderStage::QueuedNoFrame:
                // Wait / ProcessTasks / BeginFrame 抛异常时, 当前 mailbox 仍只是 Queued
                window.RestoreOrReleaseMailbox(*mailboxSlot);
                break;
            case RenderStage::FrameAcquired:
            case RenderStage::MailboxInRender:
                // frame 已 acquire 但尚未把所有权写回 flight, 仍由当前线程负责收口
                window.ReleaseMailbox(*mailboxSlot);
                break;
            case RenderStage::SubmitRecorded:
                // submit.Task 已写回 flight, mailbox 生命周期已转移给 flight slot
                break;
        }
    }

    if (frameContext != nullptr) {
        try {
            auto abandon = _gpu->AbandonFrame(std::move(frameContext));
            this->HandlePresentResult(window, abandon.Present);
            if (abandon.Task.IsValid()) {
                abandon.Task.Wait();
                std::lock_guard renderLock{_renderMutex};
                _gpu->ProcessTasks();
            }
        } catch (...) {
            // cleanup 失败时保留原始异常；渲染线程将整体停止，由主线程重新抛出首个异常
        }
    }

    this->ReportRenderThreadException(ex);
    return;  // 终止本轮 RenderAllWindows，回到 RenderThreadMain 检查 stop
}
```

关键设计决策说明：

- `_surface->_nextFrameSlotIndex` 在 Step 1 的 mutex 内读取。只有渲染线程调用 BeginFrame 会推进它，主线程不访问，但读取时恰好在 mutex 内（与 Render 线程的 collect 同一临界区），无额外开销。
- 多线程稳态下，flight 的 collect/回收只由渲染线程执行；主线程 prepare 不再调用 `CollectCompletedFlightTasks()`。这避免了主线程与渲染线程在 `!allowFrameDrop` 锁外 Wait 路径上竞争同一个 flight slot。
- Step 3 的 `!allowFrameDrop` 路径将 GPU Wait 移到锁外执行，但不会在锁外直接解引用 `std::optional<GpuTask>`。锁内先提取稳定的 wait token（`Fence* + signalValue`），解锁后只等待 fence，再重新加锁回收 flight。这避免了持锁等 GPU 阻塞主线程 prepare，也避免了 optional 被 reset 后的悬空访问。
- Step 5 (Queued→InRender) 和 Step 8 (flight 写回) 分别加锁，而非合并为一次长锁，是为了让 OnRender 和 Submit 这两个耗时操作在无锁状态下执行。
- Step 4~8 的外层 try-catch 覆盖 `Wait`、`ProcessTasks`、`BeginFrame/TryBeginFrame`、`OnRender`、`SubmitFrame/AbandonFrame` 和 `HandlePresentResult`。catch 路径按 `RenderStage` 做 stage-aware 收口，再统一走 `_renderThreadException` 回传（见 2.14）。
- `HandlePresentResult` 写 `_pendingRecreate`（atomic），不需要 mutex。

### 2.8 渲染线程生命周期与唤醒

所有线程间控制状态由 `_renderWakeMutex` 统一保护，用普通 bool，不用 atomic。消除 atomic+condvar 混用导致的 lost wakeup：

```cpp
class Application {
    std::mutex _renderWakeMutex;
    std::condition_variable _renderWakeCV;
    std::condition_variable _pauseAckCV;
    bool _newFramePending{false};
    bool _renderPauseRequested{false};
    bool _renderPaused{false};
    bool _renderStopRequested{false};
    std::exception_ptr _renderThreadException{};  // 渲染线程异常传播（见 2.14）
    std::thread _renderThread;
};
```

**渲染线程主循环**：
```cpp
void Application::ReportRenderThreadException(std::exception_ptr ex) {
    {
        std::lock_guard lock{_renderWakeMutex};
        if (!_renderThreadException) _renderThreadException = ex;
        _renderStopRequested = true;
    }
    _pauseAckCV.notify_all();  // 唤醒等待 pause ack 的主线程
    _renderWakeCV.notify_one();
}

void Application::RenderThreadMain() {
    try {
        while (true) {
            std::unique_lock lock{_renderWakeMutex};
            _renderWakeCV.wait_for(lock, std::chrono::milliseconds(16), [this]() {
                return _newFramePending || _renderPauseRequested || _renderStopRequested;
            });

            if (_renderStopRequested) return;

            if (_renderPauseRequested) {
                _renderPaused = true;
                lock.unlock();
                _pauseAckCV.notify_one();
                lock.lock();
                _renderWakeCV.wait(lock, [this]() {
                    return !_renderPauseRequested || _renderStopRequested;
                });
                _renderPaused = false;
                if (_renderStopRequested) return;
                continue;
            }

            bool hasFrame = _newFramePending;
            _newFramePending = false;
            lock.unlock();

            // hasFrame 为 false 时（超时唤醒），仍执行一轮 collect 防止自阻塞
            this->RenderAllWindows();
        }
    } catch (...) {
        this->ReportRenderThreadException(std::current_exception());  // last-resort catch
    }
}
```

`wait_for` 16ms 超时兜底：即使没有新帧通知，也会周期性唤醒执行 `CollectCompletedFlightTasks()`，防止全 slot 占满时的自阻塞死锁。

**主线程控制接口**：
```cpp
void Application::NotifyNewFrame() {
    { std::lock_guard lock{_renderWakeMutex}; _newFramePending = true; }
    _renderWakeCV.notify_one();
}

void Application::PauseRenderThread() {
    {
        std::lock_guard lock{_renderWakeMutex};
        if (_renderStopRequested || _renderThreadException != nullptr) return;
        _renderPauseRequested = true;
    }
    _renderWakeCV.notify_one();
    std::unique_lock lock{_renderWakeMutex};
    _pauseAckCV.wait(lock, [this]() {
        return _renderPaused || _renderStopRequested || _renderThreadException != nullptr;
    });
}

void Application::ResumeRenderThread() {
    { std::lock_guard lock{_renderWakeMutex}; _renderPauseRequested = false; _renderPaused = false; }
    _renderWakeCV.notify_one();
}

void Application::StopRenderThread() {
    { std::lock_guard lock{_renderWakeMutex}; _renderStopRequested = true; }
    _renderWakeCV.notify_one();
    if (_renderThread.joinable()) _renderThread.join();
}
```

### 2.9 窗口集合拓扑约束

`SparseSet<AppWindow>` 不是线程安全容器。`Emplace()`/`Destroy()` 会移动 dense array 元素、可能触发扩容，渲染线程持有的引用立刻失效。

硬性约束：所有 `_windows` 拓扑变更必须在渲染线程暂停期间执行。

但仅靠 `CreateWindow()` 里的 assert 还不够。框架还需要提供 app-facing helper，把 pause / exception-check / resume 的配对封装起来，避免子类手写不完整的 pause-resume 协议：

```cpp
template <class F>
decltype(auto) Application::WithRenderThreadPaused(F&& fn) {
    const bool needPause = _multiThreaded;
    if (needPause) {
        this->PauseRenderThread();
        this->CheckRenderThreadException();
    }

    // 只在成功路径 resume。
    // 若 fn 抛异常，说明窗口拓扑/surface 重建可能只完成了一半；
    // 此时必须保持渲染线程停住，由 Run() 顶层 catch 执行 stop/join，
    // 不能先 resume 再把半完成状态暴露给渲染线程。
    if constexpr (std::is_void_v<std::invoke_result_t<F&>>) {
        std::invoke(std::forward<F>(fn));
        if (needPause) this->ResumeRenderThread();
    } else {
        auto result = std::invoke(std::forward<F>(fn));
        if (needPause) this->ResumeRenderThread();
        return result;
    }
}
```

```cpp
AppWindowHandle Application::CreateWindow(...) {
    RADRAY_ASSERT(!_multiThreaded || _renderPaused);
    // ... 原有逻辑
}
```

`CreateWindow()` 中的 assert 仍然保留，它表达的是底层不变量；app 侧在运行时新增窗口时，应该走：

```cpp
auto handle = this->WithRenderThreadPaused([&]() {
    return this->CreateWindow(windowDesc, surfaceDesc, false);
});
```

`OnInitialize()` 中创建窗口不受此约束，因为渲染线程尚未启动。

`PauseRenderThread()` 的返回语义是"渲染线程已暂停，或渲染线程已因 stop/exception 退出"。因此 helper 在 pause 返回后，必须先 `CheckRenderThreadException()`，再假设窗口集合已被安全独占。

### 2.10 HandleSurfaceChanges

swapchain 重建也要走同一个 pause helper，不能手写 pause/resume；这样重建路径抛异常时也不会把渲染线程永远卡在 paused 状态：

```cpp
void Application::HandleSurfaceChanges() {
    bool needRecreate = false;
    for (auto& w : _windows.Values()) {
        if (w._pendingRecreate.load(std::memory_order_acquire)) {
            needRecreate = true; break;
        }
    }
    if (!needRecreate) return;
    this->WithRenderThreadPaused([&]() {
        // ... 重建 swapchain（原有逻辑）
    });
}
```

`WithRenderThreadPaused()` 只负责成功路径 resume；若重建代码抛异常，它会刻意保持渲染线程停在 pause/stop 边界，把异常继续交给 `Run()` 顶层收口（见 2.11）。这是为了避免重建一半就把不一致状态重新暴露给渲染线程。

### 2.11 线程模式切换与 Run() 集成

`ApplyPendingThreadMode` 在 `Run()` 主循环顶部调用，确保模式切换在帧边界的干净时机生效。同时 `Run()` 需要顶层异常收口：主线程任意异常都必须先 stop/join 渲染线程，再向上传播。

这里还要顺手修正 shutdown 责任边界：framework 自己持有的 `_windows` / `_gpu` 不能继续只靠虚函数 `OnShutdown()` 间接清理。原因有两个：

- `OnInitialize()` 可能在创建出 `_gpu` / `window` 后半路抛异常；这时 `OnShutdown()` 不一定适合被调用，但 framework 资源仍必须按 `_windows` → `_gpu` 的顺序回收。
- `Application` 的成员析构顺序是 `_gpu` 先于 `_windows`，如果不显式清理，`GpuRuntime` 会先死于 `GpuSurface` 之前，违反 `gpu_system.h` 的生命周期约束。

因此需要拆成两层：

- `OnShutdown()`：用户覆写点，只负责 app-owned 清理；只在 `OnInitialize()` 成功返回后调用。
- `ShutdownFrameworkStateNoThrow()`：framework 内部非虚 helper，负责 `_windows.Clear()` 后再 `_gpu.reset()`；在正常路径和异常路径都由 `Run()` 保证执行。

```cpp
void Application::ShutdownFrameworkStateNoThrow() noexcept {
    try {
        _windows.Clear();
    } catch (...) {
    }
    _gpu.reset();
}

int32_t Application::Run(int argc, char* argv[]) {
    bool initialized = false;
    bool userShutdownDone = false;
    bool frameworkShutdownDone = false;
    bool teardownDone = false;

    auto stopRenderThreadNoThrow = [&]() noexcept {
        if (!_renderThread.joinable()) return;
        try {
            this->StopRenderThread();
        } catch (...) {
        }
    };

    auto shutdownFrameworkNoThrow = [&]() noexcept {
        if (frameworkShutdownDone) return;
        this->ShutdownFrameworkStateNoThrow();
        frameworkShutdownDone = true;
    };

    auto teardownNoThrow = [&]() noexcept {
        if (teardownDone) return;

        stopRenderThreadNoThrow();

        if (_gpu != nullptr) {
            try {
                this->WaitAllFlightTasks();
            } catch (...) {
            }
            try {
                this->WaitAllSurfaceQueues();
            } catch (...) {
            }
        }

        if (initialized && !userShutdownDone) {
            try {
                this->OnShutdown();
            } catch (...) {
            }
            userShutdownDone = true;
        }

        shutdownFrameworkNoThrow();
        teardownDone = true;
    };

    try {
        this->OnInitialize();
        initialized = true;
        // ...

        while (!_exitRequested) {
            this->ApplyPendingThreadMode();           // ← 帧边界切换
            this->CheckRenderThreadException();       // ← 渲染线程异常回传（见 2.14）
            this->DispatchAllWindowEvents();
            this->CheckWindowStates();
            if (_exitRequested) break;
            this->OnUpdate();
            if (_exitRequested) break;
            {
                std::unique_lock<std::mutex> l{_renderMutex, std::defer_lock};
                if (_multiThreaded) l.lock();
                _gpu->ProcessTasks();
            }
            this->HandleSurfaceChanges();
            if (_multiThreaded) {
                this->ScheduleFramesMultiThreaded();   // prepare only
                this->NotifyNewFrame();
            } else {
                this->ScheduleFramesSingleThreaded();  // prepare + render + submit
            }
        }

        if (_renderThread.joinable()) {
            this->StopRenderThread();
        }
        this->WaitAllFlightTasks();
        this->WaitAllSurfaceQueues();
        this->OnShutdown();
        userShutdownDone = true;
        shutdownFrameworkNoThrow();
        teardownDone = true;
        return 0;
    } catch (...) {
        teardownNoThrow();
        throw;
    }
}
```

这层顶层 catch 是必须的；否则 `OnUpdate()`、`OnPrepareRender()`、`ProcessTasks()` 或 pause 后的 surface recreate 只要在主线程抛异常，就可能绕过 `StopRenderThread()`，把一个仍然 joinable 的渲染线程留到析构阶段。这里还要注意：

- teardown 只能执行一次，不能在 `OnShutdown()` 自己抛异常后又重复 wait/shutdown 一轮。
- framework-owned `_windows` / `_gpu` 清理必须是非虚、固定顺序的内部逻辑，不能完全寄托在用户 override 是否记得调 base。

**ApplyPendingThreadMode**：
```cpp
void Application::ApplyPendingThreadMode() {
    if (_pendingMultiThreaded == _multiThreaded) return;
    if (_pendingMultiThreaded) {
        this->WaitAllFlightTasks();
        {
            std::lock_guard lock{_renderWakeMutex};
            _newFramePending = false;
            _renderPauseRequested = false;
            _renderPaused = false;
            _renderStopRequested = false;
        }
        _renderThread = std::thread([this]() { this->RenderThreadMain(); });
    } else {
        this->StopRenderThread();
        this->WaitAllFlightTasks();
        this->WaitAllSurfaceQueues();
    }
    _multiThreaded = _pendingMultiThreaded;
}
```

### 2.12 SparseSet 兼容性

`std::mutex` 不可移动，`std::atomic<bool>` 默认 move 语义不适合搬运。用 `unique_ptr<std::mutex>` 包装：

```cpp
AppWindow::AppWindow(AppWindow&& other) noexcept
    : // ... 其他字段
      _mailboxMutex(std::move(other._mailboxMutex)),
      _pendingRecreate(other._pendingRecreate.load(std::memory_order_relaxed))
{
    other._pendingRecreate.store(false, std::memory_order_relaxed);
}

void swap(AppWindow& a, AppWindow& b) noexcept {
    using std::swap;
    // ... 其他字段
    swap(a._mailboxMutex, b._mailboxMutex);
    bool tmp = a._pendingRecreate.load(std::memory_order_relaxed);
    a._pendingRecreate.store(b._pendingRecreate.load(std::memory_order_relaxed), std::memory_order_relaxed);
    b._pendingRecreate.store(tmp, std::memory_order_relaxed);
}
```

### 2.13 _renderMutex 保留策略

原有 `_renderMutex` 的 mailbox 保护职责已迁移到 per-window `_mailboxMutex`，线程控制职责已迁移到 `_renderWakeMutex`。但 `_renderMutex` 仍需保留，原因：

1. `ProcessTasks()` 在主线程和渲染线程都可能调用（渲染线程 Step 3 中 `!_allowFrameDrop` 路径的 `Wait` + `ProcessTasks`）。虽然 `GpuRuntime` 公开 API 内部有 `_runtimeMutex` 串行化，但 `_renderMutex` 提供了更粗粒度的保护，避免主线程和渲染线程同时进入 `ProcessTasks` 导致的不必要竞争。

精确覆盖范围：多线程模式下，`_renderMutex` 只保护 `ProcessTasks()` 调用本身，包括主线程 `Run()` 中的 `ProcessTasks()`，以及渲染线程 Step 3 中 Wait 后的 `ProcessTasks()`。渲染线程的 OnRender / Submit 步骤（2.7 Step 6-7）不持 `_renderMutex`，是无锁执行的。

这意味着：通过 `GetDevice()` 等 escape hatch 获得的 borrowed 对象（gpu_system.h:63-64 明确排除在线程安全保证外）在多线程模式的 `OnRender` 中使用时，`_renderMutex` 不提供保护。用户必须自行同步，或迁移到 `GpuRuntime` 的公开线程安全 API（如 `GpuRuntime::CreateTextureView` 替代 `GetDevice()->CreateTextureView`）。

在 swapchain backbuffer RTV 这种当前还没有 `GpuRuntime` 公开封装的场景里，测试 app 若必须调用 `GetDevice()->CreateTextureView`，则必须在 cache miss 分支显式持有 `_renderMutex`（或等价同步原语）把该 escape hatch 与主线程 `ProcessTasks()` 串行化；否则该 smoke test 不能作为多线程正确性的证据。

`RenderThreadCommand` variant 和 `UnboundedChannel` 不再用于渲染线程通信，可在 Phase 1 完成后移除。

### 2.14 渲染线程异常传播与主线程收口

单线程模式下，`OnRender`/`SubmitFrame`/`HandlePresentResult` 的异常在 `Run()` 调用链上直接 rethrow（application.cpp:535）。独立渲染线程没有天然回传通道，未捕获异常会导致 `std::terminate`。

机制分三层：

- 第一层：`RenderAllWindows()` 的 per-window 外层 try-catch 覆盖 `Wait`、`ProcessTasks`、`BeginFrame/TryBeginFrame`、`OnRender`、`SubmitFrame/AbandonFrame`、`HandlePresentResult`。catch 路径按 `RenderStage` 做 mailbox/frame 的 stage-aware 收口。
- 第二层：`RenderThreadMain()` 自身还有 last-resort try-catch。任何意外逃逸出 `RenderAllWindows()` 的异常，都会被 `ReportRenderThreadException()` 兜底捕获，避免 `std::terminate`。
- 第三层：`Run()` 顶层 try-catch 负责主线程异常收口。即使异常来自 `OnUpdate()`、`OnPrepareRender()`、`HandleSurfaceChanges()` 或其他主线程路径，也必须先 stop/join 渲染线程，再 rethrow（见 2.11）。

统一上报 helper：

```cpp
void Application::ReportRenderThreadException(std::exception_ptr ex) {
    {
        std::lock_guard lock{_renderWakeMutex};
        if (!_renderThreadException) _renderThreadException = ex;
        _renderStopRequested = true;
    }
    _pauseAckCV.notify_all();  // 避免 PauseRenderThread 永久等待
    _renderWakeCV.notify_one();
}
```

```cpp
// Run() 主循环帧边界（ApplyPendingThreadMode 之后）：
void Application::CheckRenderThreadException() {
    std::exception_ptr ex;
    {
        std::lock_guard lock{_renderWakeMutex};
        ex = std::exchange(_renderThreadException, nullptr);
    }
    if (ex) {
        // 渲染线程已因异常设置 stop，等待其退出
        if (_renderThread.joinable()) _renderThread.join();
        _multiThreaded = false;
        std::rethrow_exception(ex);
    }
}
```

`_renderThreadException` 纳入 `_renderWakeMutex` 保护，与 `_renderStopRequested` 同一同步域，无需额外原子操作。

pause 相关的死锁规避：`PauseRenderThread()` 的 wait 谓词同时接受 `_renderPaused`、`_renderStopRequested` 和 `_renderThreadException != nullptr`；`ReportRenderThreadException()` 还会 `notify_all(_pauseAckCV)` 唤醒等待 pause ack 的主线程。随后调用方通过 `CheckRenderThreadException()` 立即 rethrow，而不是误以为 pause 已成功。

---

## 三、同步原语总结

| 资源 | 保护方式 | 说明 |
|------|---------|------|
| mailbox 状态 + snapshot + flights | per-window `_mailboxMutex` | 所有 mailbox 状态转换、flight 读写、snapshot 读写 |
| 正在写入的 slot | `MailboxState::Preparing` | 对渲染线程不可见 |
| `_pendingRecreate` | `std::atomic<bool>` | 两个线程都可能写 |
| 帧通知 + 暂停 + 停止 + 异常 | `_renderWakeMutex` + condvar | 统一同步域，含 `_renderThreadException` |
| 暂停确认 | `_renderPaused` + `_pauseAckCV` | 同一把锁保护 |
| 窗口集合拓扑 | 渲染线程暂停期间独占 | CreateWindow 必须 pause 后执行 |
| ProcessTasks | `_renderMutex` | 主线程与渲染线程调用时均持有；不覆盖 OnRender |
| GPU 资源（公开 API） | `GpuRuntime::_runtimeMutex` | 已有 |
| escape hatch (GetDevice 等) | 用户自行同步 | 不在框架线程安全保证内 |

---

## 四、实施步骤

### Phase 1: API 契约与 Mailbox 基础设施

**Step 1.1** — 更新 application.h 回调注释，明确 `OnPrepareRender` / `OnRender` 的线程归属和数据访问约束（见 2.1）。

**Step 1.2** — `AppWindow` 添加 `Preparing` 状态到 `MailboxState` 枚举。

**Step 1.3** — 将 `GetPrepareMailboxSlot()` 重构为 `ReserveMailboxSlot()`：找到 Free 或 Published slot 后立即设为 `Preparing` 并返回。不再需要 `allowOverwritePublished` 参数（见 2.5）。

**Step 1.4** — 修改 `PublishPreparedMailbox()` 的 assert：`state == Preparing`（原为 `Free || Published`）。

**Step 1.5** — 实现 `RestoreOrReleaseMailbox()`，基于 generation 比较决定恢复还是释放（见 2.5）。原 `RestoreMailbox()` 保留供单线程路径使用，或统一替换。

**Step 1.6** — `AppWindow` 添加 `unique_ptr<std::mutex> _mailboxMutex`、`std::atomic<bool> _pendingRecreate`、`StateSnapshot _stateSnapshot`。更新默认构造函数、move 构造、swap（见 2.4、2.12）。

**Step 1.7** — 更新 `ScheduleFramesSingleThreaded()` 使用新的 `ReserveMailboxSlot()` + `PublishPreparedMailbox()` 流程。单线程模式下不需要加 mutex（无并发），但可选择统一加锁以简化代码路径。

**Step 1.8** — 更新现有 mailbox 单元测试适配 `Preparing` 状态和新 API。补充测试：
- `ReserveMailboxSlot` 在 `2×InRender + 1×Published` 时仍能覆盖 Published
- `PublishPreparedMailbox` 只接受 `Preparing` 状态
- `RestoreOrReleaseMailbox` 的 generation 比较逻辑

### Phase 2: 渲染线程核心

**Step 2.1** — `Application` 添加 `_renderWakeMutex`、`_renderWakeCV`、`_pauseAckCV`、四个 bool 控制状态及 `_renderThreadException`（见 2.8）。

**Step 2.2** — 实现 `RenderThreadMain()`（见 2.8）。

**Step 2.3** — 实现 `RenderAllWindows()`：遍历所有窗口，按 2.7 的完整 8 步锁协议执行消费流程。Step 3 的 `!allowFrameDrop` 路径在锁外 Wait GPU。所有异常路径存入 `_renderThreadException` 并设置 stop（见 2.14）。

**Step 2.4** — 实现 `ScheduleFramesMultiThreaded()`：遍历所有窗口，按 2.6 的流程执行 prepare。

**Step 2.5** — 实现控制接口：`NotifyNewFrame()`、`PauseRenderThread()`、`ResumeRenderThread()`、`StopRenderThread()`，并补上 app-facing 的 `WithRenderThreadPaused()` 包装（见 2.8、2.9）。

**Step 2.6** — 实现 `CheckRenderThreadException()`：主线程帧边界检查渲染线程异常并 rethrow（见 2.14）。

### Phase 3: 集成与切换

**Step 3.1** — 完善 `ApplyPendingThreadMode()` 的启动分支（见 2.11）。

**Step 3.2** — 修改 `Run()` 主循环：顶部调用 `ApplyPendingThreadMode()` + `CheckRenderThreadException()`，多线程分支调用 `ScheduleFramesMultiThreaded()` + `NotifyNewFrame()`；外层再包一层顶层 try-catch，确保主线程异常时也会 stop/join render thread 再 rethrow。同步拆分 `OnShutdown()` 与 framework-owned `ShutdownFrameworkStateNoThrow()`，保证正常路径、异常路径和 partial-init 失败都按 `_windows` → `_gpu` 的顺序清理（见 2.11）。

**Step 3.3** — 修改 `HandleSurfaceChanges()` 使用 `WithRenderThreadPaused()` 替代手写 pause/resume（见 2.10）。

**Step 3.4** — `CreateWindow()` 保留 `RADRAY_ASSERT(!_multiThreaded || _renderPaused)` 作为底层不变量；对 app/runtime 侧新增窗口场景，统一通过 `WithRenderThreadPaused([&] { CreateWindow(...); })` 调用（见 2.9）。

**Step 3.5** — `ProcessTasks()` 调用保持 `_renderMutex` 保护（见 2.13）。

### Phase 4: 测试

**Step 4.1** — 多线程 smoke test：在 `ApplicationSmokeApp` 基础上添加多线程变体，`OnInitialize` 中 `RequestMultiThreaded(true)`，验证基本渲染流程。除将 `_renderedFrameCount` 改为 `std::atomic<uint32_t>` 外，还必须处理 `EnsureBackBufferRtv()` 的 escape hatch 路径：当前 `GetDevice()->CreateTextureView` 不在框架线程安全保证内，因此要么改成框架提供的受支持 helper，要么至少在 cache miss 分支显式持有 `_renderMutex`（或等价同步原语）后再创建 RTV。

**Step 4.2** — `_allowFrameDrop` 语义保持测试：验证多线程模式下 CPU 快于 GPU 时仍能丢弃中间帧、覆盖 Published，行为与单线程一致。

**Step 4.3** — resize/recreate 与 pause 交互测试：模拟渲染线程运行中触发 `_pendingRecreate`，验证 pause 握手正确完成 swapchain 重建。

**Step 4.4** — 运行中创建窗口测试：验证 `WithRenderThreadPaused([&] { CreateWindow(...); })` 流程不会导致 crash 或 data race。当前 `Application` 没有 `DestroyWindow()` API，销毁窗口测试推迟到该 API 补齐后。

**Step 4.5** — 渲染线程异常传播测试：`OnRender` 中抛出异常，验证主线程在下一帧边界收到 rethrow，渲染线程已 join，`_multiThreaded` 已复位。

### Phase 5: 清理

**Step 5.1** — 移除 `RenderThreadCommand` variant 和 `UnboundedChannel`（不再使用）。

**Step 5.2** — 评估 `_renderMutex` 是否可移除：检查所有 escape hatch 使用点，如果 app 代码已迁移到 `GpuRuntime` 公开 API，则可移除。

---

## 五、后续阶段（不在本次实施范围）

- Phase 6: 渲染线程内部用 job system 并行录制命令（多窗口并行，或单窗口内多 pass 并行）
- Phase 7: 如果需要，加 RHI Thread（对 Vulkan 意义不大，对 DX12 有帮助）
