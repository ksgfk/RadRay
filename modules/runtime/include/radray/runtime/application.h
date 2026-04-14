#pragma once

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

class AppWindow {
public:
    AppWindow() noexcept = default;
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow(AppWindow&& other) noexcept;
    AppWindow& operator=(AppWindow&& other) noexcept;
    ~AppWindow() noexcept;

    friend void swap(AppWindow& a, AppWindow& b) noexcept;

public:
    enum class FlightState {
        Free,
        Queued,
        Submitted
    };

    class FlightData {
    public:
        std::optional<GpuTask> _task{};
        FlightState _state{FlightState::Free};
    };

    AppWindowHandle _selfHandle;
    unique_ptr<NativeWindow> _window;
    unique_ptr<GpuSurface> _surface;
    vector<FlightData> _flightTasks;
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
    /** 主线程通知可以安全的为渲染准备数据 */
    virtual void OnPrepareRender(AppWindowHandle window, uint32_t flightIndex) = 0;
    /** 录制渲染命令 */
    virtual void OnRender(AppWindowHandle window, GpuFrameContext* context, uint32_t flightIndex) = 0;

    void CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, std::optional<render::VulkanInstanceDescriptor> vkInsDesc);
    void CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, unique_ptr<render::InstanceVulkan> vkIns);

    AppWindowHandle CreateWindow(const NativeWindowCreateDescriptor& windowDesc, const GpuSurfaceDescriptor& surfaceDesc, bool isPrimary);
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
