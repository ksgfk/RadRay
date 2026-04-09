#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>

#include <radray/types.h>
#include <radray/stopwatch.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class NativeWindow;

struct AppConfig {
    // 外部创建好的窗口，Application 借用不拥有
    NativeWindow* Window{nullptr};

    render::RenderBackend Backend{render::RenderBackend::D3D12};
    render::TextureFormat SurfaceFormat{render::TextureFormat::BGRA8_UNORM};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
    uint32_t BackBufferCount{3};
    uint32_t FlightFrameCount{2};

    bool MultiThreadedRender{false};
    bool AllowFrameDrop{false};
};

struct AppFrameInfo {
    uint64_t LogicFrameIndex{0};
    uint64_t RenderFrameIndex{0};
    uint64_t DroppedFrameCount{0};
    float DeltaTime{0};
    float TotalTime{0};
};

class FramePacket {
public:
    virtual ~FramePacket() noexcept = default;
};

class Application;

class IAppCallbacks {
public:
    virtual ~IAppCallbacks() noexcept = default;

    // 主线程。GpuRuntime 和 Surface 已创建
    virtual void OnStartup(Application* app) {}

    // 主线程。GpuRuntime 仍存活，Surface 可能已销毁
    virtual void OnShutdown(Application* app) {}

    // 主线程。每轮主循环必调用
    virtual void OnUpdate(Application* app, float dt) {}

    // 主线程。保证此刻渲染线程空闲。
    // 仅在确定本帧要渲染时调用（丢帧时不调用）。
    // 返回 nullptr 表示不需要额外数据。
    virtual unique_ptr<FramePacket> OnExtractRenderData(Application* app, const AppFrameInfo& info) { return nullptr; }

    // 单线程模式：主线程。多线程模式：渲染线程。
    // packet 是 OnExtractRenderData 的返回值，可能为 nullptr。
    // 用户在此录制命令（CreateCommandBuffer + Begin/End），不调用 SubmitFrame。
    virtual void OnRender(Application* app, GpuFrameContext* frameCtx, const AppFrameInfo& info, FramePacket* packet) {}

    // 主线程。Surface 已重建完成（resize / present mode 切换）
    virtual void OnSurfaceRecreated(Application* app, GpuSurface* surface) {}
};

class Application {
public:
    explicit Application(AppConfig config, IAppCallbacks* callbacks);
    ~Application() noexcept;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 进入主循环，阻塞直到退出。返回退出码
    int Run();

    // ─── 运行时控制（主线程调用，下一帧生效）───

    void SetMultiThreadedRender(bool enable);
    void SetAllowFrameDrop(bool enable);
    void RequestPresentModeChange(render::PresentMode mode);
    void RequestExit();

    // ─── 只读查询 ───

    GpuRuntime* GetGpuRuntime() const;
    NativeWindow* GetWindow() const;
    GpuSurface* GetSurface() const;
    const AppFrameInfo& GetFrameInfo() const;
    bool IsMultiThreadedRender() const;
    bool IsFrameDropEnabled() const;
    render::PresentMode GetCurrentPresentMode() const;

    // ─── 主循环内部 ───

    void HandlePendingChanges();
    void RecreateSurface(uint32_t width, uint32_t height, render::PresentMode mode);
    void SwitchThreadMode(bool enableMultiThread);

    // ─── 渲染线程 ───

    void RenderThreadMain();
    void KickRenderThread(unique_ptr<GpuFrameContext> ctx, AppFrameInfo info, unique_ptr<FramePacket> packet);
    void WaitRenderComplete();
    void StopRenderThread();
    void HandlePresentResult(render::SwapChainPresentResult result);

    // ─── 子系统 ───

    AppConfig _config;
    IAppCallbacks* _callbacks{nullptr};

    NativeWindow* _window{nullptr};
    unique_ptr<GpuRuntime> _gpu;
    unique_ptr<GpuSurface> _surface;

    // ─── 帧状态 ───

    AppFrameInfo _frameInfo{};
    Stopwatch _timer{};

    // ─── 运行时控制 flags（仅主线程读写）───

    bool _exitRequested{false};
    bool _multiThreaded{false};
    bool _allowFrameDrop{false};

    bool _pendingSurfaceRecreate{false};
    std::optional<bool> _pendingThreadModeSwitch{};
    bool _pendingResize{false};
    render::PresentMode _pendingPresentMode{render::PresentMode::FIFO};
    uint32_t _pendingWidth{0};
    uint32_t _pendingHeight{0};

    // ─── 渲染线程同步 ───

    struct RenderWork {
        unique_ptr<GpuFrameContext> Ctx;
        AppFrameInfo Info;
        unique_ptr<FramePacket> Packet;
    };

    std::thread _renderThread;
    std::mutex _renderMutex;
    std::condition_variable _renderKickCV;
    std::condition_variable _renderDoneCV;
    RenderWork _renderWork;
    render::SwapChainPresentResult _lastPresentResult;
    bool _renderHasWork{false};
    bool _renderDone{true};
    bool _renderThreadStop{false};
};

}  // namespace radray
