#pragma once

#include <optional>
#include <variant>
#include <span>
#include <atomic>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <utility>

#include <radray/sparse_set.h>
#include <radray/channel.h>
#include <radray/render/common.h>
#include <radray/window/native_window.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class AppException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct StopRenderThreadCommand {};

using RenderThreadCommand = std::variant<StopRenderThreadCommand>;

using AppWindowHandle = SparseSetHandle;

/**
 * mailbox 优先拿 Free 槽位；如果没有空槽位，允许覆盖 Published 但尚未进入 InRender 的最新快照
 */
class AppWindow {
public:
    AppWindow() noexcept = default;
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow(AppWindow&& other) noexcept;
    AppWindow& operator=(AppWindow&& other) noexcept;
    ~AppWindow() noexcept;

    void ResetMailboxes() noexcept;
    /** mailboxSlot 是 runtime 内部分配并回传的令牌，只能传入当前 window 之前由 GetPrepareMailboxSlot()/GetPublishedMailboxSlot() 返回过的槽位，不能当任意外部输入使用 */
    std::optional<uint32_t> GetPublishedMailboxSlot() const noexcept;
    std::optional<uint32_t> GetPrepareMailboxSlot() const noexcept;
    /** Publish 会把准备完成的槽位切换成当前最新可读快照；允许传入 Free 槽位，或当前被覆盖的 Published 槽位，但绝不能传入 Queued 或 InRender 的槽位 */
    void PublishPreparedMailbox(uint32_t mailboxSlot) noexcept;
    /** Restore 只用于槽位刚被标记为 Queued，但 BeginFrame 在提交前失败的路径，此时要把这份最新快照恢复为 Published */
    void RestoreMailbox(uint32_t mailboxSlot) noexcept;
    /** Release 用于在槽位被新版本替代，或持有它的 in-flight render 完成后，使槽位重新失效并回到 Free */
    void ReleaseMailbox(uint32_t mailboxSlot) noexcept;
    void CollectCompletedFlightTasks() noexcept;

    friend void swap(AppWindow& a, AppWindow& b) noexcept;

public:
    enum class MailboxState {
        Free,
        /** 快照已经提交，渲染阶段现在可以选 */
        Published,
        /** 快照已经交给渲染线程，但还没形成 flight task */
        Queued,
        /** 快照已经被渲染/GPU 真正占用，不能动 */
        InRender
    };

    class MailboxData {
    public:
        uint64_t _generation{0};
        MailboxState _state{MailboxState::Free};
    };

    struct FlightData {
        std::optional<GpuTask> _task;
        std::optional<uint32_t> _mailboxSlot;
    };

    AppWindowHandle _selfHandle{};
    unique_ptr<NativeWindow> _window;
    unique_ptr<GpuSurface> _surface;
    vector<FlightData> _flights;
    vector<MailboxData> _mailboxes;
    std::optional<uint32_t> _latestPublishedMailboxSlot{};
    uint64_t _latestPublishedGeneration{0};
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
    void DispatchAllWindowEvents();
    void CheckWindowStates();
    void HandleSurfaceChanges();
    bool CanRenderWindow(AppWindowHandle window) const;
    void HandlePresentResult(AppWindow& window, const render::SwapChainPresentResult& presentResult);
    void WaitAllFlightTasks();
    void WaitAllSurfaceQueues();

    void RequestMultiThreaded(bool multiThreaded);
    void ApplyPendingThreadMode();
    void ScheduleFramesSingleThreaded();
    void ScheduleFramesMultiThreaded();

protected:
    SparseSet<AppWindow> _windows;
    unique_ptr<GpuRuntime> _gpu;
    std::mutex _renderMutex;
    std::thread _renderThread;
    UnboundedChannel<RenderThreadCommand> _renderCommandQueue;
    std::atomic_bool _exitRequested{false};
    bool _pendingMultiThreaded{false};
    bool _multiThreaded{false};
    bool _allowFrameDrop{false};
};

}  // namespace radray
