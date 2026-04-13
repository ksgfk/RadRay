#pragma once

#include <span>
#include <atomic>
#include <mutex>
#include <stdexcept>

#include <radray/sparse_set.h>
#include <radray/render/common.h>
#include <radray/window/native_window.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class AppException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

using AppWindowHandle = SparseSetHandle;

class AppWindow {
public:
public:
    AppWindowHandle _selfHandle;
    unique_ptr<NativeWindow> _window;
    unique_ptr<GpuSurface> _surface;
    vector<std::optional<GpuTask>> _flightTasks;
    uint32_t _nextFreeTaskSlot{0};
    bool _isPrimary{false};
    bool _pendingResize{false};
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

    AppWindowHandle CreateWindow(const NativeWindowCreateDescriptor& windowDesc, const GpuSurfaceDescriptor& surfaceDesc, bool isPrimary);
    void DispatchAllWindowEvents();
    void HandleSurfaceChanges();

protected:
    SparseSet<AppWindow> _windows;
    unique_ptr<GpuRuntime> _gpu;
    std::mutex _gpuMutex;
    std::atomic_bool _exitRequested{false};
    bool _multiThreaded{false};
    bool _allowFrameDrop{false};
};

}  // namespace radray
