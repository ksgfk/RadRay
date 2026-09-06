#pragma once

#include <chrono>
#include <filesystem>
#include <string_view>

#include <radray/coroutine.h>
#include <radray/types.h>
#ifdef RADRAY_ENABLE_IMGUI
#include <radray/runtime/imgui/imgui_system.h>
#endif

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
class World;
struct AppFrameTarget;

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

/// 一站式运行时启动描述。Application::Run(desc) 据此创建 GpuSystem(由其持有 device/factory)、
/// 窗口系统、主窗口 + swapchain、AssetManager、World,并固化帧序与 shutdown 顺序。
/// 所有系统(含渲染)都在运行时内部生命周期里创建与驱动,不提供外部注册钩子。
struct ApplicationRuntimeDescriptor {
    // —— 后端 / 运行模式 ——
    render::RenderBackend Backend;
    bool EnableValidation{false};
    bool Multithreaded{false};
    std::string_view AppName{"RadRay Application"};
    std::string_view EngineName{"RadRay"};
    /// 显式指定的可写目录，用于持久化图形管线缓存。
    /// shader artifact 的加载策略由 runtime/render 边界负责。
    std::filesystem::path RenderCachePath{};
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
    bool EnableSynchronizationValidation{false};
#ifdef RADRAY_ENABLE_IMGUI
    ImGuiSystemDescriptor ImGui{};
#endif
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
    World* GetWorld() noexcept { return _world.get(); }
#ifdef RADRAY_ENABLE_IMGUI
    Nullable<ImGuiSystem*> GetImGuiSystem() noexcept { return _imguiSystem.get(); }
#endif
    const World* GetWorld() const noexcept { return _world.get(); }
    /// 兼容性便捷入口；device 的所有权与生命周期由 GpuSystem 管理。
    render::Device* GetDevice() noexcept;
    const render::Device* GetDevice() const noexcept;
    const std::filesystem::path& GetRenderCachePath() const noexcept { return _renderCachePath; }
    const std::filesystem::path& GetShaderSourceRoot() const noexcept { return _shaderSourceRoot; }
    const vector<std::filesystem::path>& GetShaderIncludePaths() const noexcept {
        return _shaderIncludePaths;
    }

    // —— runner / 运行时内部系统调用的框架方法(已固化帧序,非游戏 override 点)——
    AppUpdateResult Update(const AppUpdateContext& ctx);
    void Render(AppFrameContext& ctx);
    int Shutdown(const AppShutdownContext& ctx);
    void OnRenderComplete(const AppRenderCompleteContext& ctx);
    void NotifyRenderComplete(const AppRenderCompleteContext& ctx);

    int StartLoop();

protected:
    // 游戏 override 点 (窄接口)。底层负责"何时 tick、怎么 acquire/render/present",
    // 游戏只负责"这个应用要画什么"。

    /// 运行时全部内部系统就绪后(device/window/gpu/render/asset/world 全部建好)的一次性初始化。
    /// 典型用途:加载资产、Spawn Actor、建相机。
    virtual void OnInit();
#ifdef RADRAY_ENABLE_IMGUI
    virtual void ConfigureImGui(ImGuiSystemDescriptor& descriptor);
    virtual void OnImGui();
#endif

    /// 每帧游戏逻辑(World::Tick 之前)。在 AssetManager::Pump 之后调用。
    virtual void OnUpdate(const AppUpdateContext& ctx);

    /// 关闭前的游戏侧清理(WaitAndCleanupCompletedFlights 之后、World 拆除之前)。
    /// 典型用途:释放游戏自管的 per-flight 资源、置空指向 World 的非 owning 指针。
    virtual void OnShutdown();

    /// 某个 flight 的 GPU work 完成后回调。典型用途:回收应用自管的延迟销毁 GPU 资源。
    virtual void OnRenderFrameComplete(const AppRenderCompleteContext& ctx);

    /// 是否请求退出。默认:主窗口被关闭。
    bool ShouldExit() const noexcept;

private:
    bool InitializeRuntime(const ApplicationRuntimeDescriptor& desc);
    void DestroyRuntime() noexcept;

    unique_ptr<WindowManager> _windowManager;
    unique_ptr<GpuSystem> _gpuSystem;
    unique_ptr<AssetDatabase> _assetDatabase;
    unique_ptr<AssetManager> _assetManager;
    unique_ptr<RenderSystem> _renderSystem;
    unique_ptr<World> _world;
#ifdef RADRAY_ENABLE_IMGUI
    unique_ptr<ImGuiSystem> _imguiSystem;
#endif
    ApplicationScheduler _scheduler;
    std::filesystem::path _renderCachePath;
    std::filesystem::path _shaderSourceRoot;
    vector<std::filesystem::path> _shaderIncludePaths;
    bool _multithreaded{false};
};

}  // namespace radray
