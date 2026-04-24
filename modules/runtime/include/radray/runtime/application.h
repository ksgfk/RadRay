#pragma once

#include <optional>
#include <span>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <utility>

#include <radray/sparse_set.h>
#include <radray/render/common.h>
#include <radray/window/native_window.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class AppException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

using AppWindowHandle = SparseSetHandle;

/**
 * flight slot 和 swapchain 的 flight frame count 对齐，用来保证同一个 swapchain frame slot 不会被重复使用
 * mailbox 解决的是“主线程准备渲染数据”和“渲染阶段消费渲染数据”的解耦问题
 * mailbox 优先拿 Free 槽位；如果没有空槽位，允许覆盖 Published 但尚未进入 InRender 的最新快照
 * mailboxSlot 是 runtime 内部分配并回传的令牌，只能传入当前 window 之前由 ReserveMailboxSlot()/GetPublishedMailboxSlot() 返回过的槽位，不能当任意外部输入使用
 */
class AppWindow {
public:
    struct RenderRequest {
        uint32_t FlightSlot{0};
        uint32_t MailboxSlot{0};
        uint64_t Generation{0};
    };

    AppWindow() noexcept = default;
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow(AppWindow&& other) noexcept;
    AppWindow& operator=(AppWindow&& other) noexcept;
    ~AppWindow() noexcept;

    /** 重置所有 mailbox 状态, 会等待所有任务完成 */
    void ResetMailboxes() noexcept;
    /** 最新已发布的 mailbox 槽位 */
    std::optional<uint32_t> GetPublishedMailboxSlot() const noexcept;
    /** 预留一个 mailbox 槽位并立刻标记为 Preparing；优先使用 Free，找不到则覆盖当前 Published */
    std::optional<uint32_t> AllocMailboxSlot() noexcept;
    /** 只有 Preparing 槽位才能正式发布为当前最新快照 */
    void PublishPreparedMailbox(uint32_t mailboxSlot) noexcept;
    /** 若 Queued 槽位已经落后于更新 generation 直接释放，否则恢复为 Published */
    void RestoreOrReleaseMailbox(uint32_t mailboxSlot) noexcept;
    /** Release 用于在槽位被新版本替代，或持有它的 in-flight render 完成后，使槽位重新失效并回到 Free */
    void ReleaseMailbox(uint32_t mailboxSlot) noexcept;

    /** 处理已完成的 flight slot */
    void CollectCompletedFlightSlots() noexcept;

    /** 当前 window 是否可渲染 */
    bool CanRender() const noexcept;

    friend void swap(AppWindow& a, AppWindow& b) noexcept;

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
        /** 渲染线程正在准备渲染任务 */
        Preparing,
        /** 渲染任务已排队等待执行 */
        Queued,
        /** 渲染任务正在执行 */
        InRender
    };

    class MailboxData {
    public:
        uint64_t _generation{0};
        MailboxState _state{MailboxState::Free};
    };

    struct FlightData {
        std::optional<GpuTask> _task;
        uint32_t _mailboxSlot{0};
        FlightState _state{FlightState::Free};
    };

    struct RenderDataChannel {
        mutable std::mutex _mutex;
        deque<RenderRequest> _queue;
        size_t _queueCapacity{0};
        bool _completed{false};
    };

    AppWindowHandle _selfHandle{};
    unique_ptr<NativeWindow> _window;
    unique_ptr<GpuSurface> _surface;
    vector<FlightData> _flights;
    vector<MailboxData> _mailboxes;
    std::optional<RenderRequest> _latestRequest;
    Nullable<std::unique_ptr<RenderDataChannel>> _channel;
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

protected:
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

protected:
    SparseSet<AppWindow> _windows;
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
