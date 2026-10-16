#include <radray/runtime/application.h>
#include "application_loop.h"

#include <chrono>

#include <atomic>
#include <optional>
#include <thread>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/scope_guard.h>
#include <radray/render/rhi.h>
#include <radray/runtime/asset_database.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/scene_manager.h>
#include <radray/runtime/render_framework/renderer.h>
#include <radray/runtime/world_manager.h>
#include <radray/window/native_window.h>

namespace radray {

namespace {

vector<unique_ptr<AssetImporter>> MakeDefaultAssetImporters() {
    vector<unique_ptr<AssetImporter>> importers;
    importers.push_back(make_unique<TextureImporter>());
    importers.push_back(make_unique<MeshImporter>());
    return importers;
}

}  // namespace

bool SwitchToApplicationSchedulerAwaitable::await_ready() const noexcept {
    return _scheduler == nullptr || _scheduler->IsStopping() || _stop.stop_requested();
}

bool SwitchToApplicationSchedulerAwaitable::await_suspend(std::coroutine_handle<> continuation) {
    if (_scheduler == nullptr || _scheduler->IsStopping() || _stop.stop_requested()) {
        return false;
    }
    _record = _scheduler->Enqueue(_stop, continuation);
    return true;
}

bool SwitchToApplicationSchedulerAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        return !_stop.stop_requested() && (_scheduler == nullptr || !_scheduler->IsStopping());
    }

    const bool completed = !_record->Canceled && !_record->Stop.stop_requested();
    if (_scheduler != nullptr) {
        _scheduler->Erase(_record);
    }
    _record = nullptr;
    return completed;
}

ApplicationScheduler::~ApplicationScheduler() noexcept {
    BeginStopping();
    CancelAll();
}

task<void> ApplicationScheduler::SwitchTo() {
    stop_token stop = co_await CurrentStopToken();
    bool completed = co_await SwitchToApplicationSchedulerAwaitable{this, stop};
    if (!completed) {
        co_await StopCurrentTask();
    }
}

ApplicationSchedulerRecord* ApplicationScheduler::Enqueue(stop_token stop, std::coroutine_handle<> continuation) {
    return _records.Enqueue(stop, continuation);
}

bool ApplicationScheduler::Erase(ApplicationSchedulerRecord* record) noexcept {
    return _records.Erase(record);
}

void ApplicationScheduler::Pump() {
    RADRAY_PROFILE_SCOPE_N("ApplicationScheduler::Pump");
    if (_pumping || _collecting) RADRAY_ABORT("Cannot pump scheduler during dispatch or collection");
    _pumping = true;
    auto guard = MakeScopeGuard([this]() noexcept { _pumping = false; });
    _records.DispatchReady([](const auto&) { return true; });
}

void ApplicationScheduler::CancelAll() noexcept {
    _records.CancelAll();
}

Application::Application() noexcept = default;

Application::~Application() noexcept {
    StopAndDrainRuntime();
    if (_renderer && !_renderer->Shutdown()) RADRAY_ABORT("Application renderer still owns unfinished GPU epochs after drain");
    _scheduler.CancelAll();
    DestroyRuntime();
}

void Application::OnInit() {
}

void Application::OnUpdate(const AppUpdateContext& ctx) {
    (void)ctx;
}

void Application::OnRender(AppFrameContext& ctx) {
    (void)ctx;
}

void Application::OnShutdown() {
}

void Application::OnRenderFrameComplete(const FlightCompletion& ctx) {
    (void)ctx;
}

void Application::ServiceFrameBoundaryGT(uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("Application::ServiceFrameBoundaryGT");
    PumpFlightCompletions(flightIndex);
    if (_assetManager) _assetManager->Pump();
    _scheduler.Pump();
}

bool Application::SetCollecting(bool collecting) {
    const bool previous = _scheduler._collecting;
    _scheduler._collecting = collecting;
    if (_assetManager) _assetManager->_operationsBlocked = collecting;
    return previous;
}

void Application::ApplySceneUpdatesRT(AppFrameContext& ctx) {
    if (_sceneManager != nullptr) _sceneManager->ConsumeRenderUpdates(ctx.FlightIndex(), ctx.FrameSerial());
}

void Application::PumpFlightCompletions(std::optional<uint32_t> flightIndex) {
    RADRAY_ASSERT(std::this_thread::get_id() == _applicationThread);
    if (_frameTimeline == nullptr || _processingFlightCompletions) return;
    _processingFlightCompletions = true;
    auto scope = MakeScopeGuard([this]() noexcept { _processingFlightCompletions = false; });

    vector<FlightCompletion> completions;
    FlightCompletion completion;
    while (_frameTimeline->TryReadCompletion(completion)) {
        completions.push_back(completion);
    }
    for (const auto& c : completions) {
        if (_gpuSystem != nullptr) _gpuSystem->ReleaseFrameResourcesGT(c);
        if (_sceneManager != nullptr) _sceneManager->OnFlightCompletedGT(c);
    }
    if (_sceneManager) _sceneManager->PumpAssetExitsGT();
    if (flightIndex) {
        _frameTimeline->PumpWaitFrame(*flightIndex);
        if (_gpuSystem != nullptr) _gpuSystem->BeginUpdateForFlight(*flightIndex);
    } else {
        _frameTimeline->CleanupCompletedFlights();
    }
    for (const auto& c : completions) {
        OnRenderFrameComplete(c);
    }
}

// ════════════════════════════════════════════════════════════════
//  固化的帧序 / 生命周期(框架驱动,非游戏 override 点)
// ════════════════════════════════════════════════════════════════

AppUpdateResult Application::Update(const AppUpdateContext& ctx) {
    // 2) 游戏逻辑。
    OnUpdate(ctx);
    // 3) World Tick、延迟销毁与渲染收集。
    if (_worldManager != nullptr) {
        _worldManager->Tick(ctx.DeltaTime.count());
    }
    FinalizeWorldAndSealGT(ctx.FlightIndex);
    return AppUpdateResult{ShouldExit()};
}

void Application::FinalizeWorldAndSealGT(uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("Application::FinalizeWorldAndSealGT");
    if (_worldManager) {
        _worldManager->FinalizeWorldsGT();
        _worldManager->CollectRenderUpdates();
    }
    if (_sceneManager) {
        auto& frame = _sceneManager->GetFrameUpdates(flightIndex);
        if (frame.Phase != SceneManager::SceneFlightPhase::Writable) RADRAY_ABORT("View collection requires writable flight");
        frame.Views.clear();
        {
            _sceneManager->SetCollecting(true);
            const bool wasCollecting = SetCollecting(true);
            const bool worldsWereCollecting = _worldManager && _worldManager->_collecting;
            if (_worldManager) _worldManager->_collecting = true;
            auto guard = MakeScopeGuard([this, wasCollecting, worldsWereCollecting]() noexcept {
                SetCollecting(wasCollecting);
                if (_worldManager) _worldManager->_collecting = worldsWereCollecting;
                _sceneManager->SetCollecting(false);
            });
            SceneViewCollector collector{frame.Views};
            OnCollectRenderViews(collector);
        }
        _sceneManager->SealFrameGT(flightIndex);
    }
}

void Application::OnCollectRenderViews(SceneViewCollector& collector) { (void)collector; }

void Application::Render(AppFrameContext& ctx) {
    this->OnRender(ctx);
}

bool Application::ShouldExit() const noexcept {
    return _exitRequested.load(std::memory_order_relaxed) || (_windowManager != nullptr && _windowManager->ShouldExit());
}

void Application::RequestExit() noexcept {
    _exitRequested.store(true, std::memory_order_release);
    if (_loop) _loop->Wake();
}

void Application::StopAndDrainRuntime() {
    if (_loop)
        _loop->Drain();
    else
        BeginStoppingRuntime();
}

void Application::BeginStoppingRuntime() {
    _scheduler.BeginStopping();
    if (_frameTimeline != nullptr) _frameTimeline->BeginStopping();
    if (_worldManager != nullptr) _worldManager->BeginStopping();
    if (_sceneManager != nullptr) _sceneManager->BeginStoppingGT();
    if (_assetManager) _assetManager->BeginStopping();
}

int Application::Shutdown(const AppShutdownContext& ctx) {
    (void)ctx;
    if (_shuttingDown) return 0;
    if (_loop && !_loop->CanFinalize()) {
        RequestExit();
        return 0;
    }
    _shuttingDown = true;
    StopAndDrainRuntime();
    if (_renderer && !_renderer->Shutdown()) RADRAY_ABORT("Application renderer still owns unfinished GPU epochs after drain");
    if (_windowManager && _windowManager->GetMainWindow())
        _windowManager->_eventPump->DispatchCloseRequest(_windowManager->GetMainWindow()->GetNativeWindow());
    // 游戏侧清理:释放自管 per-flight 资源、置空指向 World 的非 owning 指针。
    OnShutdown();
    _scheduler.CancelAll();
    DestroyRuntime();
    return 0;
}

void Application::DestroyRuntime() noexcept {
    _loop.reset();
    if (_windowManager != nullptr) _windowManager->CloseOperations();
    // 拆 World:销毁 Actor / Component，释放其持有的 StreamingAssetRef。
    if (_worldManager != nullptr) _worldManager->Shutdown();
    _worldManager.reset();
    _sceneManager.reset();
    _renderer.reset();
    if (_gpuSystem) _gpuSystem->AbandonUnpublishedResourcesTerminalGT();
    // Disconnect borrowed render services before destroying them.
    if (_windowManager != nullptr) {
        _windowManager->SetRenderSystem(nullptr);
    }
    // AssetManager 析构会 force-unload 全部资产,释放 GPU buffer(须在 device 销毁前)。
    _assetManager.reset();
    _renderSystem.reset();
    // 先取消帧等待，再拆 device。等待者恢复时 device 仍在。
    _frameTimeline.reset();
    // importer 与 settings 必须活到全部在飞加载协程被 AssetManager 收束之后。
    _assetDatabase.reset();
    if (_windowManager != nullptr) {
        _windowManager->DetachAllSwapChains();
        _windowManager->SetGpuSystem(nullptr);
    }
    if (_gpuSystem != nullptr) {
        _gpuSystem->SetWindowManager(nullptr);
    }
    _gpuSystem.reset();
    _windowManager.reset();
}

bool Application::InitializeRuntime(const ApplicationRuntimeDescriptor& desc) {
    _startup = {};
    const auto fail = [this](RuntimeStartupStatus status, std::string_view reason) {
        _startup.Status = status;
        _startup.Reason = reason;
        RADRAY_ERR_LOG("Application startup failed: {}", reason);
        return false;
    };
    if (desc.FlightDataCount == 0)
        return fail(RuntimeStartupStatus::InvalidDescriptor, "FlightDataCount must be positive");

    if (desc.Render && !desc.Gpu)
        return fail(RuntimeStartupStatus::InvalidDescriptor, "Render requires Gpu; use Scene for CPU scene delivery");

    _multithreaded = desc.Gpu && desc.Gpu->Multithreaded;
    _exitRequested.store(false, std::memory_order_relaxed);
    if (desc.Render) {
        _shaderSourceRoot = desc.Render->ShaderSourceRoot;
        _shaderIncludePaths = desc.Render->ShaderIncludePaths;
    } else {
        _shaderSourceRoot.clear();
        _shaderIncludePaths.clear();
    }
    _frameTimeline = make_unique<FrameTimeline>(desc.FlightDataCount);

    if (desc.Window) {
        WindowManagerDescriptor windowManagerDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
        windowManagerDesc.Type = NativeWindowType::Win32HWND;
        windowManagerDesc.EventDelivery = NativeEventDelivery::Deferred;
#else
        DestroyRuntime();
        return fail(RuntimeStartupStatus::InitializationFailed, "Application deferred window events require Win32");
#endif
        _windowManager = make_unique<WindowManager>(windowManagerDesc);
    }

    if (desc.Gpu) {
        const GpuOptions& gpu = *desc.Gpu;
        render::VulkanInstanceDescriptor instanceDesc{
            .AppName = desc.AppName,
            .EngineName = desc.EngineName,
            .IsEnableDebugLayer = gpu.EnableValidation,
            .IsEnableGpuBasedValid = false,
            .IsEnableSynchronizationValidation = gpu.EnableSynchronizationValidation};
        render::DXGIFactoryDescriptor factoryDesc{
            .IsEnableDebugLayer = gpu.EnableValidation,
            .IsEnableGpuBasedValid = false};
        render::VulkanCommandQueueDescriptor queueDesc{render::QueueType::Direct, 1};
        render::DeviceDescriptor deviceDesc{};
        if (gpu.Backend == render::RenderBackend::Vulkan) {
            render::VulkanDeviceDescriptor vulkanDeviceDesc{};
            vulkanDeviceDesc.Queues = std::span{&queueDesc, 1};
            deviceDesc = vulkanDeviceDesc;
        } else if (gpu.Backend == render::RenderBackend::D3D12) {
            deviceDesc = render::D3D12DeviceDescriptor{};
        } else {
            DestroyRuntime();
            return fail(RuntimeStartupStatus::InvalidDescriptor, "Unsupported render backend");
        }

        GpuSystemDescriptor gpuSysDesc{
            .VulkanInstance = instanceDesc,
            .DXGIFactory = factoryDesc,
            .Device = deviceDesc,
            .RhiValidation = gpu.RhiValidation,
            .MainQueueIndex = 0,
            .BackBufferCount = gpu.BackBufferCount,
            .FlightDataCount = desc.FlightDataCount,
            .EnableFrameProfiler = gpu.EnableFrameProfiler};
        _gpuSystem = GpuSystem::TryCreate(gpuSysDesc, *_frameTimeline, _startup);
        if (_gpuSystem == nullptr) {
            RADRAY_ERR_LOG("Application startup failed: {}", _startup.Reason);
            DestroyRuntime();
            return false;
        }
    }
    if (desc.Render) {
        _renderSystem = make_unique<RenderSystem>(_gpuSystem->GetDevice(), _shaderSourceRoot, _shaderIncludePaths);
    }
    if (desc.Scene) {
        _sceneManager = make_unique<SceneManager>(desc.FlightDataCount);
    }
    if (desc.World) {
        _worldManager = make_unique<WorldManager>(this, _sceneManager.get());
    }
    if (desc.Asset) {
        _assetManager = make_unique<AssetManager>();
        if (!desc.Asset->AssetRoot.empty()) {
            string error;
            _assetDatabase = AssetDatabase::Open(
                desc.Asset->AssetRoot,
                MakeDefaultAssetImporters(),
                error);
            if (_assetDatabase == nullptr) {
                RADRAY_ERR_LOG("open asset database failed: {}", error);
            }
        }
    }

    if (_windowManager != nullptr) {
        _windowManager->SetGpuSystem(_gpuSystem.get());
        _windowManager->SetRenderSystem(_renderSystem.get());
    }
    if (_gpuSystem != nullptr) _gpuSystem->SetWindowManager(_windowManager.get());
    if (_assetManager != nullptr) {
        _assetManager->SetAssetSource(_assetDatabase.get());
    }

    if (_renderSystem != nullptr && !_renderSystem->OnInitialize()) {
        DestroyRuntime();
        return fail(RuntimeStartupStatus::InitializationFailed, "RenderSystem initialization failed");
    }
    if (_renderSystem) {
        _renderer = make_unique<Renderer>(*_gpuSystem, *_renderSystem);
        if (_sceneManager) _sceneManager->AttachRenderer(*_renderer);
    }

    if (_windowManager != nullptr) {
        WindowCreateDescriptor wndDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
        wndDesc.Title = desc.Window->Title;
        wndDesc.Width = desc.Window->Width;
        wndDesc.Height = desc.Window->Height;
        wndDesc.Resizable = true;
        wndDesc.StartVisible = true;
#else
        DestroyRuntime();
        return fail(RuntimeStartupStatus::InitializationFailed, "Unsupported window platform");
#endif
        std::optional<WindowSwapChainDescriptor> swapchainDesc;
        if (_gpuSystem != nullptr) {
            swapchainDesc = WindowSwapChainDescriptor{};
            swapchainDesc->Width = static_cast<uint32_t>(desc.Window->Width);
            swapchainDesc->Height = static_cast<uint32_t>(desc.Window->Height);
            swapchainDesc->Format = desc.Gpu->BackBufferFormat;
            swapchainDesc->PresentMode = desc.Gpu->PresentMode;
        }
        if (!_windowManager->InitializeMainWindow(wndDesc, swapchainDesc)) {
            DestroyRuntime();
            return fail(RuntimeStartupStatus::InitializationFailed, "Main window or swapchain initialization failed");
        }
    }
    _loop = make_unique<ApplicationLoop>(this);
    _startup.Status = RuntimeStartupStatus::Started;
    _startup.Reason.clear();
    return true;
}

int Application::Run(const ApplicationRuntimeDescriptor& desc) {
    if (!InitializeRuntime(desc)) return 1;
    // End borrowed-thread access before unwinding can destroy the derived application's members.
    auto drain = MakeScopeGuard([this]() noexcept {
        if (_loop) _loop->Drain();
    });
    OnInit();
    if (!_frameTimeline) return 0;
    if (_worldManager != nullptr) _worldManager->FinalizeWorldsGT();
    if (ShouldExit()) return Shutdown(AppShutdownContext{});
    return StartLoop();
}

int Application::StartLoop() {
    _loop->Run();
    return Shutdown(AppShutdownContext{});
}

}  // namespace radray
