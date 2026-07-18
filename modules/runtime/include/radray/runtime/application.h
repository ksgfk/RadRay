#pragma once

#include <chrono>
#include <concepts>
#include <filesystem>
#include <string_view>

#include <radray/coroutine.h>
#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

class Application;
class ApplicationScheduler;
class SwitchToApplicationSchedulerAwaitable;
struct ApplicationSchedulerRecord;
class GpuSystem;
class AppWindow;
class WindowManager;
class AppFrameContext;
class AssetManager;
class RenderSystem;
class World;
class AppSubsystem;
struct AppFrameTarget;

namespace render {
class SwapChain;
class Device;
enum class RenderBackend;
enum class TextureFormat;
enum class PresentMode;
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
    bool GpuWorkCompleted{true};
};

struct AppUpdateResult {
    bool ShouldExit;
};

struct ApplicationSchedulerRecord : ManualCoroutineRecord {
};

class SwitchToApplicationSchedulerAwaitable {
public:
    SwitchToApplicationSchedulerAwaitable(ApplicationScheduler* scheduler, stop_token stop) noexcept
        : _scheduler(scheduler), _stop(stop) {}

    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> continuation);
    bool await_resume() noexcept;

private:
    ApplicationScheduler* _scheduler;
    stop_token _stop;
    ApplicationSchedulerRecord* _record{nullptr};
};

class ApplicationScheduler {
public:
    ApplicationScheduler() noexcept = default;
    ApplicationScheduler(const ApplicationScheduler&) = delete;
    ApplicationScheduler(ApplicationScheduler&&) = delete;
    ApplicationScheduler& operator=(const ApplicationScheduler&) = delete;
    ApplicationScheduler& operator=(ApplicationScheduler&&) = delete;
    ~ApplicationScheduler() noexcept;

    task<void> SwitchTo();
    void Pump();
    void CancelAll() noexcept;

private:
    friend class SwitchToApplicationSchedulerAwaitable;

    ApplicationSchedulerRecord* Enqueue(stop_token stop, std::coroutine_handle<> continuation);
    bool Erase(ApplicationSchedulerRecord* record) noexcept;
    bool IsAlive(ApplicationSchedulerRecord* record) const noexcept;
    void ResumeRecord(ApplicationSchedulerRecord* record) noexcept;
    void CancelRecord(ApplicationSchedulerRecord* record) noexcept;

    ManualCoroutineScheduler<ApplicationSchedulerRecord> _records;
};

/// 启动前注册到 Application 的运行时子系统。Application 只驱动这个窄接口,
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
    /// 每帧渲染开始前调用。用于上传/准备跨 target 的渲染资源。
    virtual void OnRenderBegin(AppFrameContext& ctx);
    /// 对当前 target 追加绘制。返回 true 表示该子系统向 target 写入了内容。
    virtual bool OnRender(AppFrameContext& ctx, const AppFrameTarget& target, bool contentDrawn);
    /// 每帧全部 target 渲染结束后调用。
    virtual void OnRenderEnd(AppFrameContext& ctx);
    virtual void OnRenderComplete(Application& app, const AppRenderCompleteContext& ctx);
    virtual void OnSwapChainRecreate(Application& app, const AppSwapChainRecreateContext& ctx);
    virtual void OnShutdown(Application& app);
    virtual RuntimeTypeId GetTypeId() const noexcept = 0;

    bool IsInitialized() const noexcept;

private:
    friend class Application;

    void Initialize(Application& app);
    void Teardown(Application& app) noexcept;

    bool _initialized{false};
};

/// 一站式运行时启动描述。Application::Run(desc) 据此创建 GpuSystem(由其持有 device/factory)、
/// 窗口系统、主窗口 + swapchain、AssetManager、World,并固化帧序与 shutdown 顺序。
/// 可选能力(例如 ImGuiSystem)由上层在 Run() 前通过 RegisterSubsystem<T>() 显式注册。
struct ApplicationRuntimeDescriptor {
    // —— 后端 / 运行模式 ——
    render::RenderBackend Backend;
    bool EnableValidation{false};
    bool Multithreaded{false};
    std::string_view AppName{"RadRay Application"};
    std::string_view EngineName{"RadRay"};
    /// Explicit writable directory for persistent graphics pipeline caches.
    /// Shader bytecode is loaded from cooked ShaderAsset binaries.
    std::filesystem::path RenderCachePath{};

    // —— 主窗口 ——
    std::string_view WindowTitle{"RadRay Application"};
    int32_t WindowWidth{1280};
    int32_t WindowHeight{720};

    // —— GPU / 呈现 ——
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
    render::TextureFormat BackBufferFormat;
    render::PresentMode PresentMode;
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

    /// 非模板注册入口。用于类型擦除后的配置路径。只能在 Run() 前调用。
    /// type 由 subsystem->GetTypeId() 提供；重复注册同 type 时返回已存在实例。
    AppSubsystem* RegisterSubsystem(unique_ptr<AppSubsystem> subsystem);
    AppSubsystem* GetSubsystem(RuntimeTypeId type) noexcept;
    const AppSubsystem* GetSubsystem(RuntimeTypeId type) const noexcept;
    vector<AppSubsystem*> GetSubsystems() noexcept;

    WindowManager* GetWindowManager() noexcept { return _windowManager.get(); }
    const WindowManager* GetWindowManager() const noexcept { return _windowManager.get(); }
    GpuSystem* GetGpuSystem() noexcept { return _gpuSystem.get(); }
    const GpuSystem* GetGpuSystem() const noexcept { return _gpuSystem.get(); }
    AssetManager* GetAssetManager() noexcept { return _assetManager.get(); }
    const AssetManager* GetAssetManager() const noexcept { return _assetManager.get(); }
    RenderSystem* GetRenderSystem() noexcept { return _renderSystem.get(); }
    const RenderSystem* GetRenderSystem() const noexcept { return _renderSystem.get(); }
    ApplicationScheduler& GetScheduler() noexcept { return _scheduler; }
    const ApplicationScheduler& GetScheduler() const noexcept { return _scheduler; }
    World* GetWorld() noexcept { return _world.get(); }
    const World* GetWorld() const noexcept { return _world.get(); }
    /// 兼容性便捷入口；device 的所有权与生命周期由 GpuSystem 管理。
    render::Device* GetDevice() noexcept;
    const render::Device* GetDevice() const noexcept;
    const std::filesystem::path& GetRenderCachePath() const noexcept { return _renderCachePath; }

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

    /// 关闭前的游戏侧清理(WaitAndCleanupCompletedFlights 之后、World 拆除之前)。
    /// 典型用途:释放游戏自管的 per-flight 资源、置空指向 World 的非 owning 指针。
    virtual void OnShutdown();

    /// 某个 flight 的 GPU work 完成后回调。典型用途:回收应用自管的延迟销毁 GPU 资源。
    virtual void OnRenderFrameComplete(const AppRenderCompleteContext& ctx);

    /// 是否请求退出。默认:主窗口被关闭。
    bool ShouldExit() const noexcept;

private:
    void InitializeRuntime(const ApplicationRuntimeDescriptor& desc);
    void InitializeSubsystems();
    void TeardownSubsystems() noexcept;

    unique_ptr<WindowManager> _windowManager;
    unique_ptr<GpuSystem> _gpuSystem;
    unique_ptr<RenderSystem> _renderSystem;
    unique_ptr<AssetManager> _assetManager;
    unique_ptr<World> _world;
    vector<unique_ptr<AppSubsystem>> _subsystems;
    ApplicationScheduler _scheduler;
    std::filesystem::path _renderCachePath;
    bool _multithreaded{false};
};

template <class T, class... Args>
requires std::derived_from<T, AppSubsystem>
T* Application::RegisterSubsystem(Args&&... args) {
    return static_cast<T*>(RegisterSubsystem(make_unique<T>(std::forward<Args>(args)...)));
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

template <>
struct RuntimeTypeTrait<AppSubsystem> {
    static constexpr RuntimeTypeId value{0xd124a773, 0xbe6f, 0x4202, 0x87, 0xa3, 0xc1, 0x3d, 0x44, 0x28, 0xab, 0xa1};
    using Bases = std::tuple<>;
};

}  // namespace radray
