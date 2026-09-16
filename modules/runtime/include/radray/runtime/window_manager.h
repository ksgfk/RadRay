#pragma once

#include <atomic>
#include <optional>
#include <thread>

#include <sigslot/signal.hpp>

#include <radray/coroutine.h>
#include <radray/nullable.h>
#include <radray/runtime_type.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/window/native_window.h>

// 窗口操作的协程调度与安全阶段: docs/architecture/frame-and-gpu.md

namespace radray {

class Application;
class GpuSystem;
class RenderSystem;
class WindowManager;
struct AppRenderContext;

namespace render {
class SwapChain;
class SwapChainDescriptor;
class SwapChainFrame;
class Texture;
class TextureView;
enum class PresentMode : int32_t;
struct SwapChainAcquireResult;
struct SwapChainPresentResult;
}  // namespace render

struct WindowManagerDescriptor {
    NativeWindowType Type;
};

struct WindowHandle {
    Nullable<const WindowManager*> Owner{nullptr};
    uint64_t Id{0};

    bool operator==(const WindowHandle& other) const noexcept {
        return Owner.Get() == other.Owner.Get() && Id == other.Id;
    }
};

enum class WindowOperationStatus {
    Completed,
    Deferred,
    InvalidWindow,
    Failed
};

struct WindowCreateDescriptor {
    string Title{};
    int32_t Width{0}, Height{0}, X{0}, Y{0};
    bool Resizable{false}, StartMaximized{false}, Fullscreen{false}, StartVisible{true};
    WindowHandle OwnerWindow{};
    bool Decorated{true}, ShowInTaskbar{true}, TopMost{false}, ActivateOnShow{true};
    bool FocusOnClick{true}, InputPassthrough{false};
};

struct WindowSwapChainDescriptor {
    uint32_t Width{0}, Height{0}, BackBufferCount{0};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
};

struct WindowCreateResult {
    WindowOperationStatus Status{WindowOperationStatus::Failed};
    WindowHandle Handle{};
};

struct WindowReleaseResult {
    WindowOperationStatus Status{WindowOperationStatus::Failed};
    unique_ptr<render::SwapChain> SwapChain;
};

class AppWindow {
public:
    struct BackBufferView {
        render::Texture* BackBuffer{nullptr};
        unique_ptr<render::TextureView> View;
        // 该 backbuffer 上一次录制后遗留的状态。首次使用 / swapchain 重建后为 Undefined,
        // 一帧 RenderTarget→Present 后为 Present。供下一帧起始 barrier 的 Before 状态使用。
        render::TextureStates State{render::TextureState::Undefined};
    };

    AppWindow(const AppWindow&) = delete;
    AppWindow(AppWindow&&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow& operator=(AppWindow&&) = delete;
    ~AppWindow() noexcept;

    WindowHandle GetHandle() const noexcept { return {_manager, _id}; }
    render::SwapChainAcquireResult AcquireNextSwapChainFrame(const AppRenderContext& ctx) noexcept;
    render::SwapChainPresentResult PresentSwapChainFrame(render::SwapChainFrame&& frame) noexcept;
    NativeWindow* GetNativeWindow() const noexcept;
    render::SwapChain* GetSwapChain() const noexcept;
    bool IsMainWindow() const noexcept;
    render::TextureView* GetOrCreateBackBufferView(const render::SwapChainFrame& frame) noexcept;
    /// 读 / 写指定 backbuffer 索引的遗留状态(供起始/收尾 barrier 使用)。
    render::TextureStates GetBackBufferState(uint32_t backBufferIndex) const noexcept;
    void SetBackBufferState(uint32_t backBufferIndex, render::TextureStates state) noexcept;

    bool IsMinimized() const noexcept;
    Eigen::Vector2i GetSize() const noexcept;
    /// Live HWND/client-size check. Safe from the render thread; used at acquire and submit.
    bool IsSwapChainPresentable() const noexcept;
    bool NeedsSwapChainRecreate(std::optional<render::PresentMode> desiredPresentMode) const noexcept;

private:
    friend class WindowManager;
    AppWindow(WindowManager* manager, unique_ptr<NativeWindow> window, NativeEventPump* pump, bool isMain, uint64_t id) noexcept;
    WindowOperationStatus AttachSwapChain(const WindowSwapChainDescriptor& desc) noexcept;
    unique_ptr<render::SwapChain> ReleaseSwapChain() noexcept;
    void DetachSwapChain() noexcept;
    WindowOperationStatus UpdateSwapChain(std::optional<render::PresentMode> desiredMode) noexcept;

    void ReleaseBackBufferViews() noexcept;

    WindowManager* _manager;
    const uint64_t _id;
    WindowHandle _ownerWindow{};
    std::optional<WindowSwapChainDescriptor> _swapChainDescriptor;
    bool _swapChainUsable{false};
    unique_ptr<NativeWindow> _window;
    sigslot::scoped_connection _beforeSurfaceChange;
    NativeEventPump* _pump;
    Nullable<unique_ptr<render::SwapChain>> _swapchain;
    vector<BackBufferView> _backBufferViews;
    std::atomic_bool _requestRecreateSwapChain{false};
    bool _isMain{false};
};

class WindowManager {
public:
    explicit WindowManager(const WindowManagerDescriptor& desc);
    WindowManager(const WindowManager&) = delete;
    WindowManager(WindowManager&&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;
    WindowManager& operator=(WindowManager&&) = delete;
    ~WindowManager() noexcept;

    /// [GT] Start/cancel/await on the application thread. Tasks must finish before this manager dies.
    task<WindowCreateResult> CreateWindow(WindowCreateDescriptor desc, bool isMain = false);
    task<WindowOperationStatus> DestroyWindow(WindowHandle window);
    task<WindowOperationStatus> AttachSwapChain(WindowHandle window, WindowSwapChainDescriptor desc);
    task<WindowOperationStatus> DetachSwapChain(WindowHandle window);
    task<WindowReleaseResult> ReleaseSwapChain(WindowHandle window);
    task<WindowOperationStatus> SetPresentMode(render::PresentMode mode);
    task<WindowOperationStatus> SetSize(WindowHandle window, int width, int height);
    task<WindowOperationStatus> SetPosition(WindowHandle window, int x, int y);
    task<WindowOperationStatus> Show(WindowHandle window);
    task<WindowOperationStatus> Show(WindowHandle window, NativeWindowShowMode mode);
    task<WindowOperationStatus> SetAlpha(WindowHandle window, float alpha);
    task<WindowOperationStatus> SetOwner(WindowHandle window, WindowHandle owner);
    task<WindowOperationStatus> SetDecorated(WindowHandle window, bool value);
    task<WindowOperationStatus> SetShowInTaskbar(WindowHandle window, bool value);
    task<WindowOperationStatus> SetTopMost(WindowHandle window, bool value);
    /// [GT] Borrowed pointer; do not retain across suspension or a maintenance phase.
    Nullable<AppWindow*> ResolveWindow(WindowHandle handle) const noexcept;
    /// [GT] Runner captures the sequence boundary before waiting for render/GPU completion.
    bool NeedsMaintenance() const noexcept;
    uint64_t GetOperationBoundary() const noexcept;
    /// [GT, render/GPU idle] Execute one bounded batch, then deliver its results outside the mutation phase.
    void ProcessOperations(uint64_t boundary);
    /// [GT, outside ProcessOperations] Stop admission and cancel pending operations before teardown.
    void CloseOperations() noexcept;
    /// [GT] Native surface mutation contract; performs no waits.
    void AssertMutationAllowed() const noexcept;
    size_t GetWindowCount() const noexcept;
    AppWindow* GetWindow(size_t index) noexcept;
    const AppWindow* GetWindow(size_t index) const noexcept;
    AppWindow* GetMainWindow() noexcept;
    const AppWindow* GetMainWindow() const noexcept;
    bool ShouldExit() const noexcept;
    render::TextureFormat GetMainBackBufferFormat(render::TextureFormat fallback = render::TextureFormat::BGRA8_UNORM) const noexcept;
    render::PresentMode GetMainPresentMode(render::PresentMode fallback = render::PresentMode::FIFO) const noexcept;
    bool NeedsRecreateSwapChain(AppWindow* window) const noexcept;
    bool HasSwapChainToRecreate() const noexcept;
    void DispatchEvents() noexcept;
    sigslot::signal<NativeWindow*>& EventModalLoopTick() noexcept;
    void SetGpuSystem(Nullable<GpuSystem*> gpuSystem) noexcept { _gpuSystem = gpuSystem.Get(); }
    GpuSystem* GetGpuSystem() const noexcept { return _gpuSystem; }
    /// 注入渲染系统(非拥有)。窗口在 backbuffer view 失效时需要淘汰其上的 Framebuffer 缓存。
    void SetRenderSystem(Nullable<RenderSystem*> renderSystem) noexcept { _renderSystem = renderSystem.Get(); }
    RenderSystem* GetRenderSystem() const noexcept { return _renderSystem; }
    NativeWindow* FindMainNativeWindow(NativeWindowType type) const noexcept;
    NativeWindow* FindFirstNativeWindow(NativeWindowType type) const noexcept;

private:
    friend class Application;

    enum class WindowOperationPhase {
        WaitingForSafety,
        Executing,
        WaitingForDelivery
    };

    struct WindowOperationRecord : ManualCoroutineRecord {
        uint64_t Sequence{0};
        WindowOperationPhase Phase{WindowOperationPhase::WaitingForSafety};
    };

    /// Must be consumed by FinishOperation before the operation coroutine returns or unwinds.
    struct WindowOperationPermission {
        explicit WindowOperationPermission(WindowOperationRecord* record) noexcept;
        WindowOperationPermission(WindowOperationPermission&& other) noexcept;
        WindowOperationPermission(const WindowOperationPermission&) = delete;
        WindowOperationPermission& operator=(const WindowOperationPermission&) = delete;
        WindowOperationPermission& operator=(WindowOperationPermission&&) = delete;
        ~WindowOperationPermission() noexcept;

        Nullable<WindowOperationRecord*> Record;
    };

    class WaitSafeAwaitable {
    public:
        WaitSafeAwaitable(WindowManager* manager, stop_token stop) noexcept;
        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> continuation);
        Nullable<WindowOperationRecord*> await_resume() noexcept;

    private:
        WindowManager* _manager;
        stop_token _stop;
        Nullable<WindowOperationRecord*> _record{nullptr};
    };

    class FinishOperationAwaitable {
    public:
        FinishOperationAwaitable(WindowManager* manager, WindowOperationPermission* permission) noexcept;
        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> continuation) noexcept;
        bool await_resume() noexcept;

    private:
        WindowManager* _manager;
        WindowOperationPermission* _permission;
    };

    WindowCreateResult CreateWindowImmediate(const WindowCreateDescriptor& desc, bool isMain);
    WindowOperationStatus DestroyWindowImmediate(WindowHandle window) noexcept;
    bool InitializeMainWindow(const WindowCreateDescriptor& desc, const WindowSwapChainDescriptor& swapchain);
    void DetachAllSwapChains() noexcept;
    Nullable<WindowOperationRecord*> EnqueueOperation(stop_token stop, std::coroutine_handle<> continuation);
    /// [GT] Grants execution in the runner's mutation phase; no further suspension until FinishOperation.
    task<WindowOperationPermission> WaitSafe();
    /// Always suspends, including after cancellation. Returns false outside the mutation phase if stopped.
    FinishOperationAwaitable FinishOperation(WindowOperationPermission& permission) noexcept;

    GpuSystem* _gpuSystem{nullptr};
    RenderSystem* _renderSystem{nullptr};
    unique_ptr<NativeEventPump> _eventPump;
    vector<unique_ptr<AppWindow>> _windows;
    AppWindow* _mainWindow{nullptr};
    std::optional<render::PresentMode> _desiredPresentMode;
    const std::thread::id _gameThread{std::this_thread::get_id()};
    NativeWindowType _type;
    ManualCoroutineScheduler<WindowOperationRecord> _operations;
    uint64_t _nextOperationSequence{1};
    uint64_t _nextWindowId{1};
    bool _acceptOperations{true};
    bool _applyingOperations{false};
    bool _processingOperations{false};
    bool _mainWindowClosed{false};
};

template <>
struct RuntimeTypeTrait<WindowManager> {
    static constexpr RuntimeTypeId value{0x4cdaef6b, 0x5df0, 0x4c55, 0xa0, 0x8a, 0x8b, 0xe6, 0x54, 0xd6, 0x6c, 0x12};
};

}  // namespace radray
