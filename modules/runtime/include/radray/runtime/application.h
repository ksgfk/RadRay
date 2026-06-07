#pragma once

#include <chrono>
#include <optional>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/window/native_window.h>

namespace radray {

class Application;
class AppWindow;
class AppWindowSystem;
class AppRenderSystem;

struct AppWindowSystemDescriptor {
    NativeWindowType Type;
};

struct AppRenderSystemDescriptor {
    render::Device* Device;
    uint32_t MainQueueIndex;
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
};

struct AppUpdateContext {
    uint32_t FlightIndex{0};
    std::chrono::duration<float> DeltaTime{};
    std::chrono::duration<float> LastFrameLatency{};
};

struct AppShutdownContext {
};

struct AppRenderContext {
    uint32_t FlightIndex{0};
    std::chrono::duration<float> DeltaTime{};
    std::chrono::duration<float> LastFrameLatency{};
    bool IsInModalLoop{false};
};

struct AppSwapChainRecreateContext {
    AppWindow* Window;
    render::SwapChain* SwapChain;
};

struct AppRenderCompleteContext {
    uint32_t FlightIndex{0};
};

struct AppUpdateResult {
    bool ShouldExit;
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
    render::TextureView* GetCurrentBackBufferView(const render::SwapChainFrame& frame) noexcept;

public:
    AppWindowSystem* _system;
    uint64_t _id;
    unique_ptr<NativeWindow> _window;
    NativeEventPump* _pump;
    Nullable<unique_ptr<render::SwapChain>> _swapchain;
    vector<BackBufferView> _backBufferViews;
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
    void CheckRecreateSwapChains() noexcept;
    void SetPresentMode(render::PresentMode presentMode) noexcept;

public:
    Application* _app;
    AppRenderSystem* _renderSystem{nullptr};
    unique_ptr<NativeEventPump> _eventPump;
    vector<unique_ptr<AppWindow>> _windows;
    uint64_t _windowIdCounter{0};
    std::optional<render::PresentMode> _presentModeOverride;
};

class AppRenderSystem {
public:
    struct FenceSignal {
        render::Fence* Fence{nullptr};
        uint64_t Value{0};

        static constexpr FenceSignal Invalid() noexcept { return FenceSignal{}; }

        constexpr bool IsValid() const noexcept { return Fence != nullptr; }
    };

    AppRenderSystem(Application* app, const AppRenderSystemDescriptor& desc);
    AppRenderSystem(const AppRenderSystem&) = delete;
    AppRenderSystem(AppRenderSystem&&) = delete;
    AppRenderSystem& operator=(const AppRenderSystem&) = delete;
    AppRenderSystem& operator=(AppRenderSystem&&) = delete;
    ~AppRenderSystem() noexcept;

    void WaitAndCleanupCompletedFlights();

public:
    struct QueueFrameTrack {
        render::CommandQueue* Queue{nullptr};
        unique_ptr<render::Fence> Fence;
        uint64_t NextFenceValue{1};
    };

    struct FlightData {
        FenceSignal Signal;
        std::chrono::steady_clock::time_point FrameStartTime{};
        vector<unique_ptr<render::RenderBase>> WaitForDestroy;
    };

    Application* _app;
    AppWindowSystem* _windowSystem{nullptr};
    render::Device* _device;
    render::CommandQueue* _mainQueue;
    const uint32_t _backBufferCount;
    const uint32_t _flightDataCount;
    QueueFrameTrack _mainQueueTrack;
    uint64_t _nowFrameIndex{0};
    std::chrono::duration<float> _lastFrameLatency{};
    vector<FlightData> _flight;
};

class Application {
public:
    Application() noexcept = default;
    Application(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(const Application&) = delete;
    Application& operator=(Application&&) = delete;
    virtual ~Application() noexcept = default;

public:
    AppWindowSystem* InitWindowSystem(const AppWindowSystemDescriptor& desc);
    void ShutdownWindowSystem();
    AppRenderSystem* InitRenderSystem(const AppRenderSystemDescriptor& desc);
    void ShutdownRenderSystem();

    void NotifyRenderComplete(const AppRenderCompleteContext& ctx);

    int StartLoop();

public:
    virtual AppUpdateResult Update(const AppUpdateContext& ctx) = 0;
    virtual int Shutdown(const AppShutdownContext& ctx) = 0;
    virtual void Render(const AppRenderContext& ctx) = 0;
    virtual void OnSwapChainRecreate(const AppSwapChainRecreateContext& ctx) = 0;
    virtual void OnRenderComplete(const AppRenderCompleteContext& ctx) = 0;

    unique_ptr<AppWindowSystem> _windowSystem;
    unique_ptr<AppRenderSystem> _renderSystem;
};

}  // namespace radray
