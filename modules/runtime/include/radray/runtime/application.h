#pragma once

#include <chrono>

#include <radray/types.h>

namespace radray {

class AppRenderSystem;
class AppWindow;
class AppWindowSystem;
class AppFrameContext;
class AssetManager;
class World;
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

    /// 引擎级资产仓库,进程内唯一、跨 World 共享。对应 UE5 挂在 UEngine 上的 UAssetManager。
    AssetManager* InitAssetManager();
    void ShutdownAssetManager();
    /// 当前运行时世界容器。对应 UE5 由 FWorldContext 持有的当前 UWorld。
    /// 当前为单 World;未来需要多 World 时再抽出独立的 WorldContext 层。
    World* InitWorld();
    void ShutdownWorld();

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
    unique_ptr<AssetManager> _assetManager;
    unique_ptr<World> _world;
    bool _multithreaded{false};
};

}  // namespace radray
