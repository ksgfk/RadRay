#pragma once

#include <optional>
#include <atomic>
#include <condition_variable>
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
 * mailbox 优先拿 Free 槽位；如果没有空槽位，允许覆盖 Published 但尚未进入 InRender 的最新快照
 * mailboxSlot 是 runtime 内部分配并回传的令牌，只能传入当前 window 由 AllocMailboxSlot() 分配的槽位，不能当任意外部输入使用
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

    /** 重置所有 mailbox 状态, 会等待所有任务完成 */
    void ResetMailboxes() noexcept;
    /** 预留一个 mailbox 槽位并立刻标记为 Preparing；优先覆盖最新 Published，找不到则使用 Free 槽位 */
    std::optional<uint32_t> AllocMailboxSlot() noexcept;
    /** 只有 Preparing 槽位才能正式发布为当前最新快照 */
    void PublishPreparedMailbox(uint32_t mailboxSlot) noexcept;
    /** Release 用于在槽位被新版本替代，或持有它的 in-flight render 完成后，使槽位重新失效并回到 Free */
    void ReleaseMailbox(uint32_t mailboxSlot) noexcept;
    /** 尝试将最新发布的渲染请求加入队列 */
    std::optional<RenderRequest> TryQueueLatestPublished() noexcept;
    /** 开始录制渲染任务 */
    void BeginPrepareRenderTask(RenderRequest request) noexcept;
    /** 录制渲染任务完成, 已提交到 GPU */
    void EndPrepareRenderTask(RenderRequest request, GpuTask task) noexcept;

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
        uint32_t _mailboxSlot{0};
        FlightState _state{FlightState::Free};
        GpuTask _task;
    };

    struct MailboxSnapshot {
        uint32_t Slot;
        MailboxState State;
        uint64_t Generation;
    };

    class RenderDataChannel {
    public:
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
    /** Run 结束前清理, 默认会将 GpuRuntime 和窗口清理 */
    virtual void OnShutdown();
    /** 游戏逻辑帧调度 */
    virtual void OnUpdate() = 0;
    /** 主线程通知可以安全地准备 mailbox slot 对应的最新渲染快照 */
    virtual void OnPrepareRender(AppWindowHandle window, uint32_t mailboxSlot) = 0;
    /** 录制渲染命令，并消费指定 mailbox slot 中的渲染快照, mailbox slot 会一直保留到对应的 flight task 完成后才重新变为 Free */
    virtual void OnRender(AppWindowHandle window, GpuFrameContext* context, uint32_t mailboxSlot) = 0;

    void CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, std::optional<render::VulkanInstanceDescriptor> vkInsDesc);
    void CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, unique_ptr<render::InstanceVulkan> vkIns);

    AppWindowHandle CreateWindow(const NativeWindowCreateDescriptor& windowDesc, const GpuSurfaceDescriptor& surfaceDesc, bool isPrimary, uint32_t mailboxCount = 3);
    void DispatchWindowEvents();
    /** 刷新窗口状态, 例如是否需要重建交换链, 是否请求退出应用 */
    void CheckWindowStates();
    /** 处理窗口状态更改, 例如重建交换链 */
    void HandleWindowChanges();
    /** 等待窗口关联的 cmd queue 完成 */
    void WaitWindowTasks();

    void ScheduleFramesSingleThreaded();
    void ScheduleFramesMultiThreaded();

    void RenderThreadImpl();
    void PauseRenderThread();
    void ResumeRenderThread();
    void StopRenderThread();
    void RequestMultiThreaded(bool multiThreaded);
    void ApplyPendingThreadMode();

public:
    vector<unique_ptr<AppWindow>> _windows;
    uint64_t _windowIdCounter{0};
    unique_ptr<GpuRuntime> _gpu;
    std::mutex _renderWakeMutex;
    std::condition_variable _renderWakeCV;
    std::condition_variable _pauseAckCV;
    std::thread _renderThread;
    std::atomic_bool _exitRequested{false};
    bool _renderPauseRequested{false};
    bool _renderPaused{false};
    bool _renderStopRequested{false};
    bool _pendingMultiThreaded{false};
    bool _multiThreaded{false};
    bool _allowFrameDrop{false};
};

}  // namespace radray
