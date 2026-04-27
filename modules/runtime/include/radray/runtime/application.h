#pragma once

#include <optional>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <utility>

#include <radray/render/common.h>
#include <radray/window/native_window.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class Application;

class AppException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AppWindowHandle {
    uint64_t Id{std::numeric_limits<uint64_t>::max()};

    constexpr bool IsValid() const { return Id != std::numeric_limits<uint64_t>::max(); }

    constexpr void Invalidate() { Id = std::numeric_limits<uint64_t>::max(); }

    constexpr static AppWindowHandle Invalid() { return {std::numeric_limits<uint64_t>::max()}; }
};

/**
 * flight slot 和 swapchain 的 flight frame count 对齐，用来保证同一个 swapchain frame slot 不会被重复使用
 * mailbox 解决的是“主线程准备渲染数据”和“渲染阶段消费渲染数据”的解耦问题
 * mailbox 优先覆盖 Published 但尚未进入 InRender 的最新快照
 * mailboxSlot 是 runtime 内部分配并回传的令牌，只能传入当前 window 由 AllocMailboxSlot() 分配的槽位，不能当任意外部输入使用
 *
 * window 内部所有权链条：
 * - 主线程通过 AllocMailboxSlot() -> OnPrepareRender() -> PublishPreparedMailbox() 生成最新快照
 * - 主线程通过 TryQueueLatestPublished() 将最新快照绑定到当前 GpuSurface::GetNextFrameSlotIndex()
 * - 渲染阶段通过 TryClaimQueuedRenderRequest() 取得 request，并最终通过 EndPrepareRenderTask() 记录 GpuTask
 * - mailbox 的 InRender 生命周期绑定到 GpuTask 完成，而不是绑定到 OnRender() 返回
 *
 * 多线程模式下，mailbox/flight/channel 状态只能通过这些成员函数访问；
 * 单线程模式下省略锁，但仍必须保持相同状态流转。
 */
class AppWindow {
public:
    struct RenderRequest {
        uint32_t FlightSlot;
        uint32_t MailboxSlot;
        uint64_t Generation;
    };

    AppWindow() noexcept = default;
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow(AppWindow&& other) = delete;
    AppWindow& operator=(AppWindow&& other) = delete;
    ~AppWindow() noexcept;

    void ResetMailboxes() noexcept;
    /** 预留一个 mailbox 槽位并立刻标记为 Preparing；优先覆盖最新 Published，找不到则使用 Free 槽位 */
    std::optional<uint32_t> AllocMailboxSlot() noexcept;
    /** 只有 Preparing 槽位才能正式发布为当前最新快照 */
    void PublishPreparedMailbox(uint32_t mailboxSlot) noexcept;
    /** Release 用于已 claim 的 request 不再占用 mailbox 时，使槽位重新失效并回到 Free */
    void ReleaseMailbox(RenderRequest request) noexcept;
    /** 尝试将最新发布的渲染请求加入队列 */
    std::optional<RenderRequest> TryQueueLatestPublished() noexcept;
    /** 尝试取出队列中的渲染请求, 并原子切换到正在准备渲染任务 */
    std::optional<RenderRequest> TryClaimQueuedRenderRequest() noexcept;
    /** 录制渲染任务完成, 已提交到 GPU */
    void EndPrepareRenderTask(RenderRequest request, GpuTask task) noexcept;
    void AbandonClaimedFrame(
        GpuRuntime* gpu,
        GpuRuntime::BeginFrameResult& begin,
        RenderRequest request) noexcept;

    /** 处理已完成的 flight slot */
    void CollectCompletedFlightSlots() noexcept;

    /** 当前 window 是否可渲染 */
    bool CanRender() const noexcept;

public:
    enum class MailboxState {
        Free,
        /** 主线程正在准备渲染快照，渲染阶段不可见 */
        Preparing,
        /** 快照已经提交，渲染阶段现在可以选 */
        Published,
        /** 快照已经交给渲染线程，但还没形成 flight task */
        Queued,
        /** 快照已经被渲染/GPU 真正占用，不能动 */
        InRender
    };

    enum class FlightState {
        Free,
        /** 渲染任务已排队等待执行, 也就是 MailboxState::Queued */
        Queued,
        /** 渲染线程正在准备渲染任务 */
        Preparing,
        /** 渲染任务正在执行 */
        InRender
    };

    class MailboxData {
    public:
        uint64_t _generation{0};
        MailboxState _state{MailboxState::Free};
    };

    class FlightData {
    public:
        FlightState _state{FlightState::Free};
        uint32_t _mailboxSlot{0};
        uint64_t _mailboxGeneration{0};
        GpuTask _task;
    };

    struct MailboxSnapshot {
        uint32_t Slot;
        MailboxState State;
        uint64_t Generation;
    };

    class RenderDataChannel {
    public:
        /**
         * 关闭入队端。Stop() 只阻止新的 TryQueueLatestPublished() 成功，不中断正在执行的 OnRender()。
         * 已经 queued 或 in-flight 的 request 由 StopRenderThread() / ResetMailboxes() 负责收口。
         */
        void Stop() noexcept;

        mutable std::mutex _mutex;
        deque<RenderRequest> _queue;
        size_t _queueCapacity{0};
        bool _completed{false};
    };

    Application* _app{nullptr};
    AppWindowHandle _selfHandle{};
    unique_ptr<NativeWindow> _window;
    unique_ptr<GpuSurface> _surface;
    vector<FlightData> _flights;
    vector<MailboxData> _mailboxes;
    /** 最近一次已发布但尚未被渲染线程消费的渲染请求 */
    std::optional<MailboxSnapshot> _latestPublished;
    mutable std::mutex _stateMutex;
    RenderDataChannel _channel;
    bool _isPrimary{false};
    bool _pendingRecreate{false};
};

/**
 * Application 线程和帧循环约定：
 *
 * 主线程所有权：
 * - Run()、OnInitialize()、OnShutdown()、OnUpdate()、OnPrepareRender()、窗口事件分发、
 *   window/surface 创建销毁、资源释放调度和线程模式切换都属于主线程。
 * - _windows 只能由主线程修改；多线程模式下，修改 window 列表或 surface 前必须先进入 safe point。
 * - NativeWindow 查询和事件处理属于主线程；渲染线程不直接调用 NativeWindow 状态查询。
 *
 * 渲染阶段所有权：
 * - 单线程模式下，渲染阶段由主线程执行。
 * - 多线程模式下，渲染阶段由 RenderThreadImpl() 执行。
 * - 渲染阶段负责 acquire-present 帧、调用 OnRender() 录制命令、提交 GPU，并把 GpuTask 写回 flight。
 * - 渲染阶段不修改 window 列表，不重建 surface，不调度主线程资源释放，也不切换线程模式。
 *
 * safe point 定义：
 * - 渲染线程尚未启动，或已经响应 PauseRenderThread() 并确认没有正在执行的 OnRender()。
 * - 当前没有未被 SubmitFrame()/AbandonFrame() 消费的 GpuFrameContext。
 * - 主线程可在 safe point 独占修改 _windows、AppWindow::_surface、_pendingRecreate、
 *   _multiThreaded 和 _allowFrameDrop。
 * - safe point 不代表 GPU idle；销毁或重建 surface 前仍必须 WaitWindowTasks()、ProcessTasks()，
 *   并通过 ResetMailboxes() 清理 window-local mailbox/flight 状态。
 *
 * acquire/claim 顺序约定：
 * - TryQueueLatestPublished() 已经把 queued request 绑定到当前 surface frame slot。
 * - BeginFrame()/TryBeginFrame() 只能在确认同一个 window 已经存在 queued request 后调用。
 * - 渲染阶段允许先 BeginFrame()/TryBeginFrame()，再 TryClaimQueuedRenderRequest()，以避免 acquire 失败后已经持有 mailbox。
 * - acquire 成功后必须立即 claim 同一个 window 的 queued request；claim 为空或 request.FlightSlot 不等于
 *   GpuFrameContext::_frameSlotIndex 都是调度错误，通过 RADRAY_ASSERT 在 debug 阶段暴露。
 * - _allowFrameDrop=false 时使用 BeginFrame()，不会因为 RetryLater 丢弃 queued request。
 * - _allowFrameDrop=true 时使用 TryBeginFrame()；RetryLater 表示 acquire 侧暂不可用，可以 claim 后
 *   ReleaseMailbox() 丢弃队首 request，从而允许后续最新快照覆盖旧帧。
 *
 * surface recreate 约定：
 * - RequireRecreate 可由渲染阶段发现，但只能在 AppWindow::_stateMutex 保护下设置 _pendingRecreate。
 * - surface 重建只由主线程在 HandleWindowChanges() 中进入 safe point 后执行。
 *
 * 异常处理约定：
 * - 任意回调或 GPU 路径异常传播到 Application 层后都视为致命错误。
 * - 主线程异常由 Run() 捕获；渲染线程异常会交还给 Run()，请求停止所有 frame 调度。
 * - 如果异常发生在 acquire 成功之后，Application 会先尝试 AbandonFrame() 收口 frame 生命周期。
 * - Run() 会停止渲染线程、等待 window queue、调用 OnShutdown()，并返回非 0。
 */
class Application {
public:
    Application() noexcept = default;
    virtual ~Application() noexcept = default;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    int32_t Run(int argc, char* argv[]);

public:
    /** Run 启动后初始化 app 系统, 应该在函数中准备好 GpuRuntime, 和至少一个窗口 */
    virtual void OnInitialize() = 0;
    /** Run 结束前清理, 默认会将 GpuRuntime 和窗口清理；清理函数不允许抛异常 */
    virtual void OnShutdown() noexcept;
    /** 游戏逻辑帧调度, 主线程调度；可以请求创建窗口、切换线程模式和调度资源生命周期 */
    virtual void OnUpdate() = 0;
    /** 主线程通知可以安全地准备 mailbox slot 对应的最新渲染快照；只写入该 mailbox slot 的 CPU 渲染数据 */
    virtual void OnPrepareRender(AppWindowHandle window, uint32_t mailboxSlot) = 0;
    /**
     * 录制渲染命令，并消费指定 mailbox slot 中的渲染快照。
     * mailbox slot 会一直保留到对应的 flight task 完成后才重新变为 Free。
     * 单线程模式由主线程调用，多线程模式由渲染线程调用；实现中不要修改窗口列表、重建 surface 或切换线程模式。
     */
    virtual void OnRender(AppWindowHandle window, GpuFrameContext* context, uint32_t mailboxSlot) = 0;

    void CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, std::optional<render::VulkanInstanceDescriptor> vkInsDesc);
    void CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, unique_ptr<render::InstanceVulkan> vkIns);

    /** 创建窗口只能在主线程调用；多线程运行中调用必须先进入 safe point */
    AppWindowHandle CreateWindow(const NativeWindowCreateDescriptor& windowDesc, const GpuSurfaceDescriptor& surfaceDesc, bool isPrimary, uint32_t mailboxCount = 3);
    /** 分发窗口事件, 主线程调度 */
    void DispatchWindowEvents();
    /** 刷新窗口状态, 例如是否需要重建交换链, 是否请求退出应用, 主线程调度 */
    void CheckWindowStates();
    /** 处理窗口状态更改, 例如重建交换链, 如果发生重建, 会暂停渲染线程进入 safe point, 主线程调度 */
    void HandleWindowChanges();
    /** 等待窗口关联的 cmd queue 完成, 主线程调度 */
    void WaitWindowTasks() noexcept;

    /** 主线程直接执行 prepare、acquire、render、submit、present 全流程 */
    void ScheduleFramesSingleThreaded();
    /** 主线程只 prepare/queue 最新快照并唤醒渲染线程 */
    void ScheduleFramesMultiThreaded();

    void RenderThreadImpl();
    /** 请求渲染线程停在 safe point；返回时没有正在执行的 OnRender() */
    void PauseRenderThread();
    /** 解除 PauseRenderThread() 设置的暂停请求 */
    void ResumeRenderThread();
    /** 关闭各 window channel，唤醒渲染线程，并等待渲染线程退出；不中断正在执行的 OnRender() */
    void StopRenderThread() noexcept;
    /** 主线程请求切换线程模式；实际切换延迟到 ApplyPendingThreadMode() 的 safe point */
    void RequestMultiThreaded(bool multiThreaded);
    /** 在 safe point 切换多线程模式, 只能在主线程调用 */
    void ApplyPendingThreadMode();

    void RequestFatalExit(std::exception_ptr exception) noexcept;
    std::exception_ptr GetFatalException() noexcept;
    void ShutdownAfterRun() noexcept;

public:
    /** 主线程拥有 window 列表；多线程模式下修改前必须进入 safe point */
    vector<unique_ptr<AppWindow>> _windows;
    uint64_t _windowIdCounter{0};
    unique_ptr<GpuRuntime> _gpu;
    /**
     * 保护 _renderPauseRequested、_renderPaused、_renderStopRequested 和渲染线程唤醒/暂停握手。
     * 持有该锁时不能调用用户回调、GPU acquire/submit，也不能执行长时间 AppWindow 状态操作。
     */
    std::mutex _renderWakeMutex;
    std::condition_variable _renderWakeCV;
    std::condition_variable _pauseAckCV;
    std::thread _renderThread;
    std::atomic_bool _exitRequested{false};
    std::atomic_bool _fatalExitRequested{false};
    std::mutex _fatalExceptionMutex;
    std::exception_ptr _fatalException{nullptr};
    bool _renderPauseRequested{false};
    bool _renderPaused{false};
    bool _renderStopRequested{false};
    bool _pendingMultiThreaded{false};
    /** 约定只在 safe point 修改, 多线程可安全读 */
    bool _multiThreaded{false};
    /** 约定只在 safe point 修改, 多线程可安全读 */
    bool _allowFrameDrop{false};
};

}  // namespace radray
