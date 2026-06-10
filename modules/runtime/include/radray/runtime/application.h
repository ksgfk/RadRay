#pragma once

#include <chrono>

#include <radray/types.h>

namespace radray {

class AppRenderSystem;
class AppWindow;
class AppWindowSystem;
class AppFrameContext;
struct AppRenderSystemDescriptor;
struct AppWindowSystemDescriptor;

namespace render {
class SwapChain;
}

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

class Application {
public:
    Application() noexcept = default;
    Application(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(const Application&) = delete;
    Application& operator=(Application&&) = delete;
    virtual ~Application() noexcept;

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
    virtual void Render(AppFrameContext& ctx) = 0;
    virtual void OnSwapChainRecreate(const AppSwapChainRecreateContext& ctx) = 0;
    virtual void OnRenderComplete(const AppRenderCompleteContext& ctx) = 0;

    unique_ptr<AppWindowSystem> _windowSystem;
    unique_ptr<AppRenderSystem> _renderSystem;
    bool _multithreaded{false};
};

}  // namespace radray
