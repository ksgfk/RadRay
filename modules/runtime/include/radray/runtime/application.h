#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <limits>
#include <variant>

#include <fmt/format.h>
#include <radray/types.h>
#include <radray/stopwatch.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class NativeWindow;

struct AppSurfaceId {
    uint64_t Handle{std::numeric_limits<uint64_t>::max()};

    constexpr bool IsValid() const { return Handle != std::numeric_limits<uint64_t>::max(); }

    constexpr void Invalidate() { Handle = std::numeric_limits<uint64_t>::max(); }

    constexpr static AppSurfaceId Invalid() { return {std::numeric_limits<uint64_t>::max()}; }

    constexpr auto operator<=>(const AppSurfaceId&) const noexcept = default;
};

struct AppSurfaceConfig {
    // 外部创建好的窗口，Application 借用不拥有
    NativeWindow* Window{nullptr};

    render::TextureFormat SurfaceFormat{render::TextureFormat::BGRA8_UNORM};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
    uint32_t BackBufferCount{3};
    uint32_t FlightFrameCount{2};
    bool IsPrimary{false};
};

struct AppConfig {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    vector<AppSurfaceConfig> InitialSurfaces{};

    bool MultiThreadedRender{false};
    bool AllowFrameDrop{false};
};

struct AppFrameInfo {
    uint64_t LogicFrameIndex{0};
    float DeltaTime{0};
    float TotalTime{0};
};

struct AppSurfaceFrameInfo {
    uint64_t RenderFrameIndex{0};
    uint64_t DroppedFrameCount{0};
};

class FramePacket {
public:
    virtual ~FramePacket() noexcept = default;
};

class Application;

class IAppCallbacks {
public:
    virtual ~IAppCallbacks() noexcept = default;

    // 主线程。GpuRuntime 和所有 initial surfaces 已创建
    virtual void OnStartup(Application* app) {}

    // 主线程。GpuRuntime 仍存活，surfaces 可能已销毁
    virtual void OnShutdown(Application* app) {}

    // 主线程。每轮主循环必调用
    virtual void OnUpdate(Application* app, float dt) {}

    // 主线程。通常仅在某个 surface 本帧将进入渲染管线时调用。
    // 多线程 + AllowFrameDrop 模式下，提取后的 packet 仍可能在渲染线程消费前被丢弃。
    virtual unique_ptr<FramePacket> OnExtractRenderData(Application* app, AppSurfaceId surfaceId, const AppFrameInfo& appInfo, const AppSurfaceFrameInfo& surfaceInfo) { return nullptr; }

    // 单线程模式：主线程。多线程模式：渲染线程。
    // packet 是 OnExtractRenderData 的返回值，可能为 nullptr。
    virtual void OnRender(Application* app, AppSurfaceId surfaceId, GpuFrameContext* frameCtx, const AppFrameInfo& appInfo, const AppSurfaceFrameInfo& surfaceInfo, FramePacket* packet) {}

    // 主线程。仅运行时新增 surface 成功后调用
    virtual void OnSurfaceAdded(Application* app, AppSurfaceId surfaceId, GpuSurface* surface) {}

    // 主线程。surface 已从 Application 注销，window 仍由外部拥有
    virtual void OnSurfaceRemoved(Application* app, AppSurfaceId surfaceId, NativeWindow* window) {}

    // 主线程。surface 已重建完成（resize / present mode 切换）
    virtual void OnSurfaceRecreated(Application* app, AppSurfaceId surfaceId, GpuSurface* surface) {}
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
    AppSurfaceId RequestAddSurface(AppSurfaceConfig config);
    void RequestRemoveSurface(AppSurfaceId surfaceId);
    void RequestPresentModeChange(render::PresentMode mode);
    void RequestPresentModeChange(AppSurfaceId surfaceId, render::PresentMode mode);
    void RequestExit();

    // ─── 只读查询 ───

    GpuRuntime* GetGpuRuntime() const;
    NativeWindow* GetWindow() const;
    NativeWindow* GetWindow(AppSurfaceId surfaceId) const;
    GpuSurface* GetSurface() const;
    GpuSurface* GetSurface(AppSurfaceId surfaceId) const;
    AppSurfaceId GetPrimarySurfaceId() const;
    vector<AppSurfaceId> GetSurfaceIds() const;
    bool HasSurface(AppSurfaceId surfaceId) const;
    const AppFrameInfo& GetFrameInfo() const;
    bool IsMultiThreadedRender() const;
    bool IsFrameDropEnabled() const;
    render::PresentMode GetCurrentPresentMode() const;

private:
    struct AddSurfaceCommand {
        AppSurfaceId SurfaceId{AppSurfaceId::Invalid()};
        AppSurfaceConfig Config{};
        bool Cancelled{false};
    };

    struct RemoveSurfaceCommand {
        AppSurfaceId SurfaceId{AppSurfaceId::Invalid()};
    };

    struct ChangePresentModeCommand {
        AppSurfaceId SurfaceId{AppSurfaceId::Invalid()};
        render::PresentMode PresentMode{render::PresentMode::FIFO};
    };

    struct SwitchThreadModeCommand {
        bool Enable{false};
    };

    struct RequestExitCommand {};

    using MainThreadCommand = std::variant<
        AddSurfaceCommand,
        RemoveSurfaceCommand,
        ChangePresentModeCommand,
        SwitchThreadModeCommand,
        RequestExitCommand>;

    struct SurfaceState {
        NativeWindow* Window{nullptr};
        unique_ptr<GpuSurface> Surface{};

        render::TextureFormat SurfaceFormat{render::TextureFormat::BGRA8_UNORM};
        render::PresentMode DesiredPresentMode{render::PresentMode::FIFO};
        uint32_t BackBufferCount{3};
        uint32_t FlightFrameCount{2};
        uint32_t LatchedWidth{0};
        uint32_t LatchedHeight{0};
        uint64_t RenderFrameIndex{0};
        uint64_t DroppedFrameCount{0};

        bool Active{false};
        bool IsPrimary{false};
        bool PendingResize{false};
        bool PendingRecreate{false};
        bool PendingRemoval{false};
        bool Closing{false};
        uint32_t OutstandingFrameCount{0};
    };

    struct RenderSurfaceFrameCommand {
        AppSurfaceId SurfaceId{AppSurfaceId::Invalid()};
        GpuSurface* Surface{nullptr};
        unique_ptr<GpuFrameContext> Ctx{};
        AppFrameInfo AppInfo{};
        AppSurfaceFrameInfo SurfaceInfo{};
        unique_ptr<FramePacket> Packet{};
    };

    struct StopRenderThreadCommand {};

    using RenderThreadCommand = std::variant<RenderSurfaceFrameCommand, StopRenderThreadCommand>;

    struct RenderCompletion {
        AppSurfaceId SurfaceId{AppSurfaceId::Invalid()};
        render::SwapChainStatus BeginStatus{render::SwapChainStatus::Success};
        render::SwapChainPresentResult PresentResult{};
        bool CallbackFailed{false};
        bool FatalError{false};
    };

    struct RenderThreadCommandResult {
        std::optional<RenderCompletion> Completion{};
        bool ShouldStop{false};
    };

    bool BuildInitialSurfaceConfigs(vector<AppSurfaceConfig>& configs);
    bool CreateRuntime();
    bool CreateInitialSurfaces(const vector<AppSurfaceConfig>& configs);
    bool CreateSurfaceFromConfig(AppSurfaceId surfaceId, const AppSurfaceConfig& config, bool notifyAdded);
    AppSurfaceId AllocateSurfaceId() const;

    SurfaceState* FindSurfaceState(AppSurfaceId surfaceId);
    const SurfaceState* FindSurfaceState(AppSurfaceId surfaceId) const;
    AddSurfaceCommand* FindQueuedAddCommand(AppSurfaceId surfaceId);
    bool HasQueuedAddForWindow(NativeWindow* window) const;
    bool HasQueuedRemoveForSurface(AppSurfaceId surfaceId) const;
    bool IsSurfaceIdReservedByQueuedAdd(AppSurfaceId surfaceId) const;
    SwitchThreadModeCommand* FindQueuedThreadModeSwitchCommand();
    NativeWindow* GetEventDispatchWindow() const;
    bool HasActiveSurfaces() const;

    void CaptureWindowState();
    bool ScheduleSurfaceFrames();
    bool HasOutstandingSurfaceJobs() const;

    void HandlePendingChanges();
    void DrainMainThreadCommands();
    bool ExecuteMainThreadCommand(MainThreadCommand& command);
    bool ExecuteMainThreadCommand(AddSurfaceCommand& command);
    bool ExecuteMainThreadCommand(RemoveSurfaceCommand& command);
    bool ExecuteMainThreadCommand(ChangePresentModeCommand& command);
    bool ExecuteMainThreadCommand(SwitchThreadModeCommand& command);
    bool ExecuteMainThreadCommand(RequestExitCommand& command);
    void FinalizeSurfaceRemoval(AppSurfaceId surfaceId, SurfaceState& surface);
    void HandlePendingSurfaceRemovals();
    void TrimInactiveSurfaceTail();
    void HandlePendingSurfaceRecreates();
    void RecreateSurface(AppSurfaceId surfaceId, SurfaceState& surface, uint32_t width, uint32_t height, render::PresentMode mode);

    void SwitchThreadMode(bool enableMultiThread);

    void RenderThreadMain();
    void KickRenderThread(RenderThreadCommand command);
    RenderThreadCommandResult ExecuteRenderThreadCommand(RenderThreadCommand& command);
    RenderThreadCommandResult ExecuteRenderThreadCommand(RenderSurfaceFrameCommand& command);
    RenderThreadCommandResult ExecuteRenderThreadCommand(StopRenderThreadCommand& command);
    void DrainRenderCompletions();
    void WaitRenderIdle();
    void WaitForRenderProgress();
    void StopRenderThread();
    void HandlePresentResult(AppSurfaceId surfaceId, render::SwapChainPresentResult result);

    AppConfig _config;
    IAppCallbacks* _callbacks{nullptr};
    unique_ptr<GpuRuntime> _gpu;
    vector<SurfaceState> _surfaces;
    deque<MainThreadCommand> _mainThreadCommands;

    AppFrameInfo _frameInfo{};
    Stopwatch _timer{};

    bool _exitRequested{false};
    bool _callbackFailed{false};
    bool _multiThreaded{false};
    bool _allowFrameDrop{false};
    AppSurfaceId _primarySurfaceId{AppSurfaceId::Invalid()};
    size_t _scheduleCursor{0};
    render::PresentMode _primaryConfiguredPresentMode{render::PresentMode::FIFO};

    std::thread _renderThread;
    std::mutex _renderMutex;
    std::mutex _gpuMutex;
    std::condition_variable _renderKickCV;
    std::condition_variable _renderDoneCV;
    deque<RenderThreadCommand> _renderQueue;
    deque<RenderCompletion> _renderCompletions;
    bool _renderWorkerBusy{false};
};

}  // namespace radray

namespace std {

template <>
struct hash<radray::AppSurfaceId> {
    size_t operator()(const radray::AppSurfaceId& value) const noexcept {
        return hash<radray::uint64_t>{}(value.Handle);
    }
};

}  // namespace std

namespace fmt {

template <typename CharT>
struct formatter<radray::AppSurfaceId, CharT> : formatter<radray::uint64_t, CharT> {
    template <typename FormatContext>
    auto format(const radray::AppSurfaceId& value, FormatContext& ctx) const {
        return formatter<radray::uint64_t, CharT>::format(value.Handle, ctx);
    }
};

}  // namespace fmt
