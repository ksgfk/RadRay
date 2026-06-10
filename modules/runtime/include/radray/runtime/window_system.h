#pragma once

#include <atomic>
#include <optional>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/window/native_window.h>

namespace radray {

class Application;
class AppRenderSystem;
class AppWindowSystem;
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

struct AppWindowSystemDescriptor {
    NativeWindowType Type;
};

class AppWindow {
public:
    struct BackBufferView {
        render::Texture* BackBuffer{nullptr};
        unique_ptr<render::TextureView> View;
        uint32_t FlightIndex{0};
    };

    AppWindow(AppWindowSystem* system, uint64_t id, unique_ptr<NativeWindow> window, NativeEventPump* pump, bool isMain) noexcept;
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
    render::TextureView* GetOrCreateBackBufferView(const render::SwapChainFrame& frame, uint32_t ownerFlightIndex) noexcept;

public:
    AppWindowSystem* _system;
    uint64_t _id;
    unique_ptr<NativeWindow> _window;
    NativeEventPump* _pump;
    Nullable<unique_ptr<render::SwapChain>> _swapchain;
    vector<BackBufferView> _backBufferViews;
    std::atomic_bool _requestRecreateSwapChain{false};
    bool _isMain{false};
};

class AppWindowSystem {
public:
    AppWindowSystem(Application* app, const AppWindowSystemDescriptor& desc);
    AppWindowSystem(const AppWindowSystem&) = delete;
    AppWindowSystem(AppWindowSystem&&) = delete;
    AppWindowSystem& operator=(const AppWindowSystem&) = delete;
    AppWindowSystem& operator=(AppWindowSystem&&) = delete;
    ~AppWindowSystem() noexcept;

    AppWindow* CreateWindow(const NativeWindowCreateDescriptor& desc, bool isMain);
    void DestroyWindow(AppWindow* window) noexcept;
    bool NeedsRecreateSwapChain(AppWindow* window) const noexcept;
    bool HasSwapChainToRecreate() const noexcept;
    void CheckRecreateSwapChains() noexcept;
    void SetPresentMode(render::PresentMode presentMode) noexcept;

public:
    Application* _app;
    AppRenderSystem* _renderSystem{nullptr};
    unique_ptr<NativeEventPump> _eventPump;
    vector<unique_ptr<AppWindow>> _windows;
    uint64_t _windowIdCounter{0};
    std::optional<render::PresentMode> _desiredPresentMode;
};

}  // namespace radray
