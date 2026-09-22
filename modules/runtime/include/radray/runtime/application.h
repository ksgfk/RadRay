#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>
#include <thread>

#include <radray/coroutine.h>
#include <radray/nullable.h>
#include <radray/types.h>

namespace radray {

class Application;
class ApplicationScheduler;
class SwitchToApplicationSchedulerAwaitable;
struct ApplicationSchedulerRecord;
class GpuSystem;
class WindowManager;
class AppFrameContext;
class AssetDatabase;
class AssetManager;
class RenderSystem;
class WorldManager;
struct AppFrameTarget;
struct FlightCompletion;

namespace render {
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
    void BeginStopping() noexcept { _stopping = true; }
    bool IsStopping() const noexcept { return _stopping; }
    void CancelAll() noexcept;

private:
    friend class SwitchToApplicationSchedulerAwaitable;
    friend class Application;

    ApplicationSchedulerRecord* Enqueue(stop_token stop, std::coroutine_handle<> continuation);
    bool Erase(ApplicationSchedulerRecord* record) noexcept;

    ManualCoroutineScheduler<ApplicationSchedulerRecord> _records;
    bool _pumping{false};
    bool _collecting{false};
    bool _stopping{false};
};

/// 一站式运行时启动描述。Application::Run(desc) 据此创建 GpuSystem(由其持有 device/factory)、
/// 窗口系统、主窗口 + swapchain、AssetManager,并固化帧序与 shutdown 顺序。
/// 核心系统都在运行时内部生命周期里创建与驱动。
struct ApplicationRuntimeDescriptor {
    // —— 后端 / 运行模式 ——
    render::RenderBackend Backend;
    bool EnableValidation{false};
    bool Multithreaded{false};
    bool EnableSynchronizationValidation{false};
    std::string_view AppName{"RadRay Application"};
    std::string_view EngineName{"RadRay"};
    /// 开发时资产根；清单固定为 `<AssetRoot>/assets.json`。空路径不启用 AssetDatabase。
    std::filesystem::path AssetRoot{};
    /// 开发时 shader 逻辑源名的文件系统根。空路径会让 program 请求明确失败。
    std::filesystem::path ShaderSourceRoot{};
    /// 传给 shader compiler 的 HLSL include roots。
    vector<std::filesystem::path> ShaderIncludePaths{};

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
    /// Available from OnInit through OnShutdown; null outside the runtime lifetime.
    Nullable<WorldManager*> GetWorldManager() noexcept { return _worldManager.get(); }
    Nullable<const WorldManager*> GetWorldManager() const noexcept { return _worldManager.get(); }
    const std::filesystem::path& GetShaderSourceRoot() const noexcept { return _shaderSourceRoot; }
    const vector<std::filesystem::path>& GetShaderIncludePaths() const noexcept { return _shaderIncludePaths; }

    AppUpdateResult Update(const AppUpdateContext& ctx);
    void Render(AppFrameContext& ctx);
    int Shutdown(const AppShutdownContext& ctx);
    int StartLoop();

protected:
    // 游戏 override 点 (窄接口)。底层负责"何时 tick、怎么 acquire/render/present",
    // 游戏只负责"这个应用要画什么"。

    /// 运行时全部内部系统就绪后(device/window/gpu/render/asset 全部建好，World 由应用显式创建)的一次性初始化。
    /// 典型用途:加载资产、Spawn Actor、建相机。
    virtual void OnInit();

    /// 每帧游戏逻辑(World::Tick 之前)。在 AssetManager::Pump 之后调用。
    virtual void OnUpdate(const AppUpdateContext& ctx);

    /// 在渲染线程（或单线程模式下的主线程）上录制应用程序命令。
    /// runner 调用开始/结束/提交；资源必须在帧处理过程(flight)时保持存活。
    virtual void OnRender(AppFrameContext& ctx);

    /// 关闭前的游戏侧清理(WaitAndCleanupCompletedFlights 之后、World 拆除之前)。
    /// 典型用途:释放游戏自管的 per-flight 资源、置空指向 World 的非 owning 指针。
    virtual void OnShutdown();

    /// 主线程, 帧 flight 重用或销毁时调用. 丢弃的帧用 GpuWorkCompleted 检查.
    virtual void OnRenderFrameComplete(const FlightCompletion& ctx);

    /// 是否请求退出。默认:主窗口被关闭。
    bool ShouldExit() const noexcept;

private:
    friend class SingleThreadRunner;
    friend class ThreadedRunner;
    friend class RenderSystem;
    void SetCollecting(bool collecting);

    bool InitializeRuntime(const ApplicationRuntimeDescriptor& desc);
    void StopAndDrainRuntime();
    void DestroyRuntime() noexcept;
    void WaitAndCleanupCompletedFlights();
    /// 有 flightIndex 时推进该可写槽位；空值仅用于 GPU idle 后的全量清理。
    void PumpFlightCompletions(std::optional<uint32_t> flightIndex);
    /// 上一帧完成后，在主线程完成回调，Runner 取得可写槽位后调用；消费 GPU 完成消息并推进本帧的 GT 调度。
    /// 调用阶段在本帧计时与窗口事件派发之前。
    void ServiceFrameBoundaryGT(uint32_t flightIndex);
    void FinalizeWorldAndSealGT(uint32_t flightIndex);
    /// RT: runner calls once per published frame, before and independently of optional drawing.
    void ApplySceneUpdatesRT(AppFrameContext& ctx);

    unique_ptr<WindowManager> _windowManager;
    unique_ptr<GpuSystem> _gpuSystem;
    unique_ptr<AssetDatabase> _assetDatabase;
    unique_ptr<AssetManager> _assetManager;
    unique_ptr<RenderSystem> _renderSystem;
    unique_ptr<WorldManager> _worldManager;
    ApplicationScheduler _scheduler;
    std::filesystem::path _shaderSourceRoot;
    vector<std::filesystem::path> _shaderIncludePaths;
    bool _multithreaded{false};
    bool _processingFlightCompletions{false};
    const std::thread::id _applicationThread{std::this_thread::get_id()};
};

}  // namespace radray
