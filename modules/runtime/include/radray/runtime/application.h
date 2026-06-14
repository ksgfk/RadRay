#pragma once

#include <chrono>
#include <concepts>
#include <string_view>

#include <radray/runtime_type.h>
#include <radray/types.h>
#include <radray/render/common.h>

namespace radray {

class Application;
class GpuSystem;
class AppWindow;
class WindowManager;
class AppFrameContext;
class AssetManager;
class World;
class AppSubsystem;
struct GpuSystemDescriptor;
struct WindowManagerDescriptor;
struct AppFrameTarget;

namespace render {
class SwapChain;
class Device;
class DXGIFactory;
}  // namespace render

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

/// 可注册到 Application 的运行时子系统。Application 只驱动这个窄接口,
/// 具体实现(例如 ImGuiSystem)由上层显式注册并按需 GetSubsystem<T>() 获取。
class AppSubsystem {
public:
    AppSubsystem() noexcept;
    AppSubsystem(const AppSubsystem&) = delete;
    AppSubsystem(AppSubsystem&&) = delete;
    AppSubsystem& operator=(const AppSubsystem&) = delete;
    AppSubsystem& operator=(AppSubsystem&&) = delete;
    virtual ~AppSubsystem() noexcept;

    virtual void OnInit(Application& app);
    virtual void OnUpdate(Application& app, const AppUpdateContext& ctx);
    /// 返回 true 表示该子系统已经处理了本帧窗口 acquire/render/present 路径。
    virtual bool OnRender(Application& app, AppFrameContext& ctx);
    virtual void OnRenderComplete(Application& app, const AppRenderCompleteContext& ctx);
    virtual void OnSwapChainRecreate(Application& app, const AppSwapChainRecreateContext& ctx);
    virtual void OnShutdown(Application& app);

    bool IsInitialized() const noexcept;

private:
    friend class Application;

    void Init(Application& app);
    void Shutdown(Application& app) noexcept;

    RuntimeTypeId _type{Guid::Empty()};
    bool _initialized{false};
};

/// 一站式运行时启动描述。Application::Run(desc) 据此创建 device(+factory)、窗口系统、
/// GpuSystem、主窗口 + swapchain、AssetManager、World,并固化帧序与 shutdown 顺序。
/// 可选能力(例如 ImGuiSystem)由上层通过 RegisterSubsystem<T>() 显式注册。
struct ApplicationRuntimeDescriptor {
    // —— 后端 / 运行模式 ——
    render::RenderBackend Backend{render::RenderBackend::Vulkan};
    bool EnableValidation{false};
    bool Multithreaded{false};
    std::string_view AppName{"RadRay Application"};
    std::string_view EngineName{"RadRay"};

    // —— 主窗口 ——
    std::string_view WindowTitle{"RadRay Application"};
    int32_t WindowWidth{1280};
    int32_t WindowHeight{720};

    // —— GPU / 呈现 ——
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
    render::TextureFormat BackBufferFormat{render::TextureFormat::BGRA8_UNORM};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
};

class Application {
public:
    Application() noexcept;
    Application(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(const Application&) = delete;
    Application& operator=(Application&&) = delete;
    virtual ~Application() noexcept;

    /// 一行启动:创建运行时 → OnInit → 进主循环 → 退出后固化 Shutdown。返回进程退出码。
    int Run(const ApplicationRuntimeDescriptor& desc);

    template <class T, class... Args>
    requires std::derived_from<T, AppSubsystem>
    T* RegisterSubsystem(Args&&... args);

    template <class T>
    requires std::derived_from<T, AppSubsystem>
    T* GetSubsystem() noexcept;

    template <class T>
    requires std::derived_from<T, AppSubsystem>
    const T* GetSubsystem() const noexcept;

    /// 非模板注册入口。用于运行时工厂、插件、脚本绑定等不方便走模板的位置。
    /// type 由调用方负责保持唯一；重复注册同 type 时返回已存在实例。
    AppSubsystem* RegisterSubsystem(RuntimeTypeId type, unique_ptr<AppSubsystem> subsystem);
    AppSubsystem* GetSubsystem(RuntimeTypeId type) noexcept;
    const AppSubsystem* GetSubsystem(RuntimeTypeId type) const noexcept;

    WindowManager* GetWindowManager() noexcept { return _windowManager.get(); }
    const WindowManager* GetWindowManager() const noexcept { return _windowManager.get(); }
    GpuSystem* GetGpuSystem() noexcept { return _gpuSystem.get(); }
    const GpuSystem* GetGpuSystem() const noexcept { return _gpuSystem.get(); }
    AssetManager* GetAssetManager() noexcept { return _assetManager.get(); }
    const AssetManager* GetAssetManager() const noexcept { return _assetManager.get(); }
    World* GetWorld() noexcept { return _world.get(); }
    const World* GetWorld() const noexcept { return _world.get(); }
    render::Device* GetDevice() noexcept { return _device.get(); }
    const render::Device* GetDevice() const noexcept { return _device.get(); }

    // —— 低层子系统(高级用法仍可单独调;Run 内部即用这些)——
    WindowManager* InitWindowManager(const WindowManagerDescriptor& desc);
    void ShutdownWindowManager();
    GpuSystem* InitGpuSystem(const GpuSystemDescriptor& desc);
    void ShutdownGpuSystem();

    /// 引擎级资产仓库,进程内唯一、跨 World 共享。对应 UE5 挂在 UEngine 上的 UAssetManager。
    AssetManager* InitAssetManager();
    void ShutdownAssetManager();
    /// 当前运行时世界容器。对应 UE5 由 FWorldContext 持有的当前 UWorld。
    World* InitWorld();
    void ShutdownWorld();

    // —— runner / 子系统调用的框架方法(已固化帧序,非游戏 override 点)——
    AppUpdateResult Update(const AppUpdateContext& ctx);
    void Render(AppFrameContext& ctx);
    int Shutdown(const AppShutdownContext& ctx);
    void OnSwapChainRecreate(const AppSwapChainRecreateContext& ctx);
    void OnRenderComplete(const AppRenderCompleteContext& ctx);
    void NotifyRenderComplete(const AppRenderCompleteContext& ctx);
    bool RenderViewContent(AppFrameContext& ctx, const AppFrameTarget& target);

    int StartLoop();

protected:
    // ════════════════════════════════════════════════════════════════
    //  游戏 override 点(窄接口)。底层负责"何时 tick、怎么 acquire/render/present";
    //  游戏只负责"这个应用要画什么 / 做什么"。
    // ════════════════════════════════════════════════════════════════

    /// 运行时与已注册子系统就绪后(device/window/gpu/asset/world 全部建好)的一次性初始化。
    /// 典型用途:加载资产、Spawn Actor、建相机。
    virtual void OnInit();

    /// 每帧游戏逻辑(World::Tick 之前)。在 AssetManager::Pump 之后调用。
    virtual void OnUpdate(const AppUpdateContext& ctx);

    /// 任意 view/window 场景内容录制。返回 true 表示已向 backbuffer 写入内容,
    /// false 则框架或子系统可选择 Clear。默认不画任何东西。
    virtual bool OnRenderView(AppFrameContext& ctx, const AppFrameTarget& target);

    /// 关闭前的游戏侧清理(WaitAndCleanupCompletedFlights 之后、ShutdownWorld 之前)。
    /// 典型用途:释放游戏自管的 per-flight 资源、置空指向 World 的非 owning 指针。
    virtual void OnShutdown();

    /// 是否请求退出。默认:主窗口被关闭。
    bool ShouldExit() const noexcept;

private:
    void InitRuntime(const ApplicationRuntimeDescriptor& desc);
    void InitSubsystems();
    void ShutdownSubsystems() noexcept;
    void InitSubsystemAt(size_t index);

    /// 没有子系统接管窗口渲染时的窗口直接渲染路径(acquire/barrier/可选 clear/present-barrier)。
    void RenderWindow(AppFrameContext& ctx, AppWindow* window);
    void RenderWindows(AppFrameContext& ctx);

    unique_ptr<WindowManager> _windowManager;
    unique_ptr<GpuSystem> _gpuSystem;
    unique_ptr<AssetManager> _assetManager;
    unique_ptr<World> _world;
    vector<unique_ptr<AppSubsystem>> _subsystems;
    shared_ptr<render::Device> _device;
    unique_ptr<render::DXGIFactory> _dxgiFactory;
    bool _multithreaded{false};
    bool _runtimeInitialized{false};
};

template <class T, class... Args>
requires std::derived_from<T, AppSubsystem>
T* Application::RegisterSubsystem(Args&&... args) {
    return static_cast<T*>(RegisterSubsystem(runtime_type_id_v<T>, make_unique<T>(std::forward<Args>(args)...)));
}

template <class T>
requires std::derived_from<T, AppSubsystem>
T* Application::GetSubsystem() noexcept {
    return static_cast<T*>(GetSubsystem(runtime_type_id_v<T>));
}

template <class T>
requires std::derived_from<T, AppSubsystem>
const T* Application::GetSubsystem() const noexcept {
    return static_cast<const T*>(GetSubsystem(runtime_type_id_v<T>));
}

}  // namespace radray
