#pragma once

#include <atomic>
#include <optional>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/window/native_window.h>
#include <radray/runtime/service_registry.h>

namespace radray {

class Application;
class GpuSystem;
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

class AppWindow {
public:
    struct BackBufferView {
        render::Texture* BackBuffer{nullptr};
        unique_ptr<render::TextureView> View;
        // 该 backbuffer 上一次录制后遗留的状态。首次使用 / swapchain 重建后为 Undefined,
        // 一帧 RenderTarget→Present 后为 Present。供下一帧起始 barrier 的 Before 状态使用。
        render::TextureStates State{render::TextureState::Undefined};
    };

    AppWindow(WindowManager* manager, unique_ptr<NativeWindow> window, NativeEventPump* pump, bool isMain) noexcept;
    AppWindow(const AppWindow&) = delete;
    AppWindow(AppWindow&&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow& operator=(AppWindow&&) = delete;
    ~AppWindow() noexcept;

    render::SwapChain* AttachSwapChain(const render::SwapChainDescriptor& desc) noexcept;
    unique_ptr<render::SwapChain> ReleaseSwapChain() noexcept;
    void DetachSwapChain() noexcept;
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
    bool NeedsSwapChainRecreate(std::optional<render::PresentMode> desiredPresentMode) const noexcept;
    void ResetSwapChainRecreateRequest() noexcept;
    bool RecreateSwapChain(uint32_t width, uint32_t height, render::PresentMode presentMode) noexcept;

private:
    friend class WindowManager;

    void ReleaseBackBufferViews() noexcept;

    WindowManager* _manager;
    unique_ptr<NativeWindow> _window;
    NativeEventPump* _pump;
    Nullable<unique_ptr<render::SwapChain>> _swapchain;
    vector<BackBufferView> _backBufferViews;
    std::atomic_bool _requestRecreateSwapChain{false};
    bool _isMain{false};
};

class WindowManager {
public:
    WindowManager(Application* app, const WindowManagerDescriptor& desc);
    WindowManager(const WindowManager&) = delete;
    WindowManager(WindowManager&&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;
    WindowManager& operator=(WindowManager&&) = delete;
    ~WindowManager() noexcept;

    AppWindow* CreateWindow(const NativeWindowCreateDescriptor& desc, bool isMain);
    void DestroyWindow(AppWindow* window) noexcept;
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
    void CheckRecreateSwapChains() noexcept;
    void SetPresentMode(render::PresentMode presentMode) noexcept;
    void DispatchEvents() noexcept;
    sigslot::signal<NativeWindow*>& EventModalLoopTick() noexcept;
    void SetGpuSystem(GpuSystem* gpuSystem) noexcept { _gpuSystem = gpuSystem; }
    GpuSystem* GetGpuSystem() const noexcept { return _gpuSystem; }
    void DetachAllSwapChains() noexcept;
    NativeWindow* FindMainNativeWindow(NativeWindowType type) const noexcept;
    NativeWindow* FindFirstNativeWindow(NativeWindowType type) const noexcept;

private:
    Application* _app;
    GpuSystem* _gpuSystem{nullptr};
    unique_ptr<NativeEventPump> _eventPump;
    vector<unique_ptr<AppWindow>> _windows;
    AppWindow* _mainWindow{nullptr};
    std::optional<render::PresentMode> _desiredPresentMode;
};

/// 依赖声明(非侵入,类外特化):WindowManager 需要 GpuSystem,复用已有 public setter。
template <>
struct ServiceTraits<WindowManager> {
    static constexpr auto Inject = std::tuple{&WindowManager::SetGpuSystem};
};

template <>
struct RuntimeTypeTrait<WindowManager> {
    static constexpr RuntimeTypeId value{0x4cdaef6b, 0x5df0, 0x4c55, 0xa0, 0x8a, 0x8b, 0xe6, 0x54, 0xd6, 0x6c, 0x12};
    using Bases = std::tuple<>;
};

}  // namespace radray
