#include <radray/runtime/application.h>

#include <chrono>

#include <atomic>
#include <optional>
#include <semaphore>
#include <span>
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
#include <radray/runtime/wait_frame.h>
#include <radray/runtime/world_manager.h>
#include <radray/window/native_window.h>

#if defined(RADRAY_PLATFORM_WINDOWS) && (defined(RADRAY_ENABLE_D3D12) || defined(RADRAY_ENABLE_VULKAN))
#define RADRAY_APP_IMPL_ENABLE_VBLANK_TICK

#include <radray/platform/win32_headers.h>

#include <dxgi1_6.h>
#include <wrl.h>

#ifdef CreateWindow
#undef CreateWindow
#endif
#endif

namespace radray {

namespace {

vector<unique_ptr<AssetImporter>> MakeDefaultAssetImporters() {
    vector<unique_ptr<AssetImporter>> importers;
    importers.push_back(make_unique<TextureImporter>());
    importers.push_back(make_unique<MeshImporter>());
    return importers;
}

class CpuWaitFrameProcessor final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};

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

class SingleThreadRunner;

#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
class Win32ModalLoopVBlankRenderer {
public:
    Win32ModalLoopVBlankRenderer(
        Application* app,
        SingleThreadRunner* runner) noexcept
        : _app(app),
          _runner(runner) {}

    Win32ModalLoopVBlankRenderer(const Win32ModalLoopVBlankRenderer&) = delete;
    Win32ModalLoopVBlankRenderer(Win32ModalLoopVBlankRenderer&&) = delete;
    Win32ModalLoopVBlankRenderer& operator=(const Win32ModalLoopVBlankRenderer&) = delete;
    Win32ModalLoopVBlankRenderer& operator=(Win32ModalLoopVBlankRenderer&&) = delete;

    ~Win32ModalLoopVBlankRenderer() noexcept {
        Stop();
        DestroyMessageWindow();
    }

    static bool IsSupported(const Application* app) noexcept {
        const GpuSystem* gpuSystem = app->GetGpuSystem().Get();
        if (gpuSystem == nullptr || gpuSystem->GetDevice() == nullptr) {
            return false;
        }
        const render::RenderBackend backend = gpuSystem->GetDevice()->GetBackend();
        return backend == render::RenderBackend::D3D12 || backend == render::RenderBackend::Vulkan;
    }

    bool OnModalLoopTick(Nullable<NativeWindow*> modalWindow) {
        _lastModalLoopTick = std::chrono::steady_clock::now();
        _modalHwnd = nullptr;
        if (modalWindow && modalWindow->GetType() == NativeWindowType::Win32HWND) {
            _modalHwnd = static_cast<HWND>(modalWindow->GetNativeHandler());
        }
        return Start();
    }

public:
    struct VBlankOutput {
        Microsoft::WRL::ComPtr<IDXGIOutput> Output;
        HMONITOR Monitor{nullptr};
    };

    static constexpr UINT VBlankRenderMessage = WM_APP + 0x5242;

    static const wchar_t* MessageWindowClassName() noexcept {
        return L"RADRAY_MODAL_VBLANK_RENDER_TICK";
    }

    bool Start() {
        if (!IsSupported(_app) || !EnsureMessageWindow() || !EnsureDXGIFactory()) {
            return false;
        }

        const VBlankOutput output = ResolveVBlankOutput();
        if (output.Output == nullptr) {
            return false;
        }

        if (_thread.joinable()) {
            if (output.Monitor == _monitor) {
                return true;
            }
            Stop();
        }

        _stop.store(false, std::memory_order_release);
        _renderPosted.store(false, std::memory_order_release);
        _output = output.Output;
        _monitor = output.Monitor;

        HWND messageWindow = _messageWindow;
        Microsoft::WRL::ComPtr<IDXGIOutput> outputForThread = _output;
        _thread = std::thread([this, messageWindow, outputForThread]() {
            while (!_stop.load(std::memory_order_acquire)) {
                const HRESULT hr = outputForThread->WaitForVBlank();
                if (_stop.load(std::memory_order_acquire)) {
                    break;
                }
                if (FAILED(hr)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{16});
                }
                if (!_renderPosted.exchange(true, std::memory_order_acq_rel)) {
                    if (::PostMessageW(messageWindow, VBlankRenderMessage, 0, 0) == 0) {
                        _renderPosted.store(false, std::memory_order_release);
                    }
                }
            }
        });
        return true;
    }

    void Stop() noexcept {
        _stop.store(true, std::memory_order_release);
        if (_thread.joinable()) {
            _thread.join();
        }
        _renderPosted.store(false, std::memory_order_release);
        _output.Reset();
        _monitor = nullptr;
    }

    void DestroyMessageWindow() noexcept {
        if (_messageWindow != nullptr) {
            ::DestroyWindow(_messageWindow);
            _messageWindow = nullptr;
        }
    }

    bool EnsureMessageWindow() noexcept {
        if (_messageWindow != nullptr) {
            return true;
        }

        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &Win32ModalLoopVBlankRenderer::MessageWindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = MessageWindowClassName();
        if (::RegisterClassW(&windowClass) == 0) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                RADRAY_WARN_LOG("RegisterClassW failed for modal vblank render window: {}", err);
                return false;
            }
        }

        _messageWindow = ::CreateWindowExW(
            0,
            MessageWindowClassName(),
            MessageWindowClassName(),
            0,
            0, 0, 0, 0,
            HWND_MESSAGE,
            nullptr,
            instance,
            this);
        if (_messageWindow == nullptr) {
            RADRAY_WARN_LOG("CreateWindowExW failed for modal vblank render window: {}", ::GetLastError());
            return false;
        }
        return true;
    }

    bool EnsureDXGIFactory() noexcept {
        if (_factory != nullptr) {
            return true;
        }

        if (const HRESULT hr = ::CreateDXGIFactory1(IID_PPV_ARGS(_factory.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_WARN_LOG("CreateDXGIFactory1 failed for modal vblank render: {}", hr);
            return false;
        }
        return true;
    }

    HWND ResolveTargetWindow() const noexcept {
        if (_modalHwnd != nullptr && ::IsWindow(_modalHwnd)) {
            return _modalHwnd;
        }

        const WindowManager* windowManager = _app->GetWindowManager().Get();
        if (windowManager == nullptr) {
            return nullptr;
        }
        if (NativeWindow* window = windowManager->FindMainNativeWindow(NativeWindowType::Win32HWND)) {
            return static_cast<HWND>(window->GetNativeHandler());
        }
        if (NativeWindow* window = windowManager->FindFirstNativeWindow(NativeWindowType::Win32HWND)) {
            return static_cast<HWND>(window->GetNativeHandler());
        }
        return nullptr;
    }

    VBlankOutput ResolveVBlankOutput() const noexcept {
        HWND hwnd = ResolveTargetWindow();
        if (hwnd == nullptr || _factory == nullptr) {
            return {};
        }

        HMONITOR targetMonitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        Microsoft::WRL::ComPtr<IDXGIOutput> firstOutput;
        HMONITOR firstMonitor = nullptr;

        for (UINT adapterIndex = 0;; ++adapterIndex) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapterResult = _factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
            if (adapterResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(adapterResult)) {
                RADRAY_WARN_LOG("IDXGIFactory::EnumAdapters1 failed while resolving modal vblank output: {}", adapterResult);
                break;
            }

            for (UINT outputIndex = 0;; ++outputIndex) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                const HRESULT outputResult = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
                if (outputResult == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(outputResult)) {
                    RADRAY_WARN_LOG("IDXGIAdapter::EnumOutputs failed while resolving modal vblank output: {}", outputResult);
                    break;
                }

                DXGI_OUTPUT_DESC desc{};
                if (FAILED(output->GetDesc(&desc))) {
                    continue;
                }
                if (firstOutput == nullptr) {
                    firstOutput = output;
                    firstMonitor = desc.Monitor;
                }
                if (desc.Monitor == targetMonitor) {
                    return VBlankOutput{std::move(output), desc.Monitor};
                }
            }
        }

        if (firstOutput != nullptr) {
            return VBlankOutput{std::move(firstOutput), firstMonitor};
        }
        return {};
    }

    bool IsActive() const noexcept {
        if (_lastModalLoopTick.time_since_epoch().count() == 0) {
            return false;
        }
        return std::chrono::steady_clock::now() - _lastModalLoopTick <= std::chrono::milliseconds{100};
    }

    void OnVBlankRenderTick();

    static LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }

        auto* renderer = reinterpret_cast<Win32ModalLoopVBlankRenderer*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == VBlankRenderMessage) {
            if (renderer != nullptr) {
                renderer->OnVBlankRenderTick();
            }
            return 0;
        }
        if (message == WM_NCDESTROY) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    Application* _app;
    SingleThreadRunner* _runner{nullptr};
    HWND _messageWindow{nullptr};
    std::thread _thread;
    std::atomic_bool _stop{true};
    std::atomic_bool _renderPosted{false};
    Microsoft::WRL::ComPtr<IDXGIFactory1> _factory;
    Microsoft::WRL::ComPtr<IDXGIOutput> _output;
    HMONITOR _monitor{nullptr};
    std::chrono::steady_clock::time_point _lastModalLoopTick{};
    HWND _modalHwnd{nullptr};
};
#endif

class SingleThreadRunner {
public:
    explicit SingleThreadRunner(Application* app)
        : _app(app) {
        if (auto windows = _app->GetWindowManager())
            _modalLoopTickConnection = windows->EventModalLoopTick().connect(&SingleThreadRunner::OnModalLoopTick, this);
    }

    int Run() {
        while (true) {
#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
            StopWin32ModalVBlank();
#endif
            PrepareFrame(false);
            _hasModalLoopActivityDuringDispatch = false;
            _isDispatchingEvents = true;
            if (auto windows = _app->GetWindowManager()) windows->DispatchEvents();
            _isDispatchingEvents = false;
            if (_reqExit) {
                break;
            }
            if (_hasModalLoopActivityDuringDispatch) {
                continue;
            }
            TickFrame(false);
            if (_reqExit) {
                break;
            }
        }
        return AppShutdown();
    }

    int AppShutdown() {
        _modalLoopTickConnection.disconnect();
#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
        StopWin32ModalVBlank();
#endif
        AppShutdownContext ctx{};
        return _app->Shutdown(ctx);
    }

    void OnModalLoopTick(NativeWindow* modalWindow) {
        if (_ticking || _reqExit) return;
        MarkModalLoopActivityDuringDispatch();
#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
        StartWin32ModalVBlank(modalWindow);
#endif
        TickFrame(true);
    }

    void MaintainWindows() {
        auto* windows = _app->GetWindowManager().Get();
        if (windows == nullptr) return;
        if (!windows->NeedsMaintenance()) return;
        const uint64_t boundary = windows->GetOperationBoundary();
        _app->GetGpuSystem()->WaitAndRetireFlights();
        windows->ProcessOperations(boundary);
        if (windows->ShouldExit()) _reqExit = true;
    }

    bool PrepareFrame(bool isInModalLoop) {
        if (_ticking || _reqExit) return false;
        if (_framePrepared) return true;
        RADRAY_PROFILE_SCOPE_N("PrepareFrame");
        _ticking = true;
        auto scope = MakeScopeGuard([this]() noexcept { _ticking = false; });
        auto* gpuSystem = _app->GetGpuSystem().Get();
        MaintainWindows();
        if (_reqExit) return false;
        const uint32_t flightIndex = gpuSystem->GetCurrentFlightIndex();
        if (!gpuSystem->CompleteFlightIfReady(flightIndex, !isInModalLoop)) return false;
        _app->ServiceFrameBoundaryGT(flightIndex);
        const auto now = gpuSystem->BeginFrameTiming(flightIndex);
        _deltaTime = now - _lastFrameTime;
        _lastFrameTime = now;
        _framePrepared = true;
        return true;
    }

    void TickFrame(bool isInModalLoop) {
        if (!PrepareFrame(isInModalLoop)) return;
        _ticking = true;
        auto scope = MakeScopeGuard([this]() noexcept { _ticking = false; });
        _framePrepared = false;
        RADRAY_PROFILE_SCOPE_N("TickFrame");
        if (isInModalLoop) {
            MarkModalLoopActivityDuringDispatch();
        }
        auto* gpuSystem = _app->GetGpuSystem().Get();
        const uint32_t flightIndex = gpuSystem->GetCurrentFlightIndex();
        const auto deltaTime = _deltaTime;

        AppUpdateResult result{};
        {
            RADRAY_PROFILE_SCOPE_N("Update");
            result = _app->Update(AppUpdateContext{
                .FlightIndex = flightIndex,
                .DeltaTime = deltaTime,
                .LastFrameLatency = gpuSystem->GetLastFrameLatency()});
        }
        _reqExit = result.ShouldExit;
        if (_reqExit) {
            return;
        }

        if (auto renderSystem = _app->GetRenderSystem()) renderSystem->PublishFrameGT(flightIndex);
        AppFrameContext frameCtx = gpuSystem->BeginFrameRecord(
            flightIndex,
            deltaTime,
            gpuSystem->GetLastFrameLatency(),
            isInModalLoop);
        _app->ApplySceneUpdatesRT(frameCtx);
        {
            RADRAY_PROFILE_SCOPE_N("Render");
            _app->Render(frameCtx);
        }
        {
            RADRAY_PROFILE_SCOPE_N("Submit");
            gpuSystem->EndFrameRecordAndSubmit(flightIndex);
        }
        gpuSystem->AdvanceFrameIndex();
        RADRAY_PROFILE_FRAME();
    }

    bool IsExitRequested() const noexcept {
        return _reqExit;
    }

    void MarkModalLoopActivityDuringDispatch() noexcept {
        if (_isDispatchingEvents) {
            _hasModalLoopActivityDuringDispatch = true;
        }
    }

    Application* _app;
    sigslot::scoped_connection _modalLoopTickConnection;
    std::chrono::steady_clock::time_point _lastFrameTime{std::chrono::steady_clock::now()};
    std::chrono::duration<float> _deltaTime{};
    bool _framePrepared{false};
    bool _reqExit{false};
    bool _isDispatchingEvents{false};
    bool _ticking{false};
    bool _hasModalLoopActivityDuringDispatch{false};

private:
#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
    bool StartWin32ModalVBlank(NativeWindow* modalWindow) {
        if (Win32ModalLoopVBlankRenderer::IsSupported(_app)) {
            if (_modalVBlankRenderer == nullptr) {
                _modalVBlankRenderer = make_unique<Win32ModalLoopVBlankRenderer>(_app, this);
            }
            return _modalVBlankRenderer->OnModalLoopTick(modalWindow);
        }
        return false;
    }

    void StopWin32ModalVBlank() noexcept {
        _modalVBlankRenderer.reset();
    }

    unique_ptr<Win32ModalLoopVBlankRenderer> _modalVBlankRenderer;
#endif
};

#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
void Win32ModalLoopVBlankRenderer::OnVBlankRenderTick() {
    _renderPosted.store(false, std::memory_order_release);
    if (!_thread.joinable() || _runner == nullptr || _runner->IsExitRequested() || !IsActive()) {
        return;
    }

    _runner->TickFrame(true);
}
#endif

class ThreadedRunner {
public:
    explicit ThreadedRunner(Application* app)
        : _app(app),
          _readySlotsSemaphore(0),
          _runnerFrameDatas(_app->GetGpuSystem()->GetFlightDataCount()),
          _renderThread(&ThreadedRunner::RenderThread, this) {
        if (auto windows = _app->GetWindowManager())
            _modalLoopTickConnection = windows->EventModalLoopTick().connect(&ThreadedRunner::OnModalLoopTick, this);
    }

    int Run() {
        while (true) {
            PrepareFrame(true);
            _hasModalLoopActivityDuringDispatch = false;
            if (auto windows = _app->GetWindowManager()) windows->DispatchEvents();

            if (_reqExit) {
                break;
            }
            if (_hasModalLoopActivityDuringDispatch) {
                continue;
            }
            TickFrame(false, true);
            if (_reqExit) {
                break;
            }
        }

        if (auto windows = _app->GetWindowManager()) windows->CloseOperations();
        _readySlotsSemaphore.release();

        if (_renderThread.joinable()) {
            _renderThread.join();
        }

        _modalLoopTickConnection.disconnect();

        AppShutdownContext ctx{};
        return _app->Shutdown(ctx);
    }

    void RenderThread() {
        RADRAY_PROFILE_THREAD("RadRay Render");
        while (true) {
            auto* gpuSystem = _app->GetGpuSystem().Get();
            {
                RADRAY_PROFILE_SCOPE_N("WaitReadySlot");
                _readySlotsSemaphore.acquire();
            }

            if (_reqExit && _renderFrameIndex == _publishedFrameCount.load(std::memory_order_acquire)) {
                break;
            }

            RADRAY_PROFILE_SCOPE_N("RenderFrame");
            uint32_t flightIndex = static_cast<uint32_t>(_renderFrameIndex % gpuSystem->GetFlightDataCount());
            auto& runnerFrameData = _runnerFrameDatas[flightIndex];
            const bool discard = _reqExit || (!runnerFrameData.IsInModalLoop &&
                                              _renderFrameIndex < _discardNonModalFramesBefore.load(std::memory_order_acquire));
            AppFrameContext frameCtx = gpuSystem->BeginFrameRecord(
                flightIndex,
                runnerFrameData.DeltaTime,
                gpuSystem->GetLastFrameLatency(),
                runnerFrameData.IsInModalLoop, !discard);
            _app->ApplySceneUpdatesRT(frameCtx);
            if (!discard) {
                RADRAY_PROFILE_SCOPE_N("Render");
                _app->Render(frameCtx);
            }
            {
                RADRAY_PROFILE_SCOPE_N("Submit");
                gpuSystem->EndFrameRecordAndSubmit(flightIndex);
            }

            _renderFrameIndex++;
            NotifyRenderFrameComplete(_renderFrameIndex);
            RADRAY_PROFILE_FRAME();
        }
    }

    void OnModalLoopTick(NativeWindow*) {
        if (_ticking || _reqExit) {
            return;
        }
        _hasModalLoopActivityDuringDispatch = true;

        const uint64_t frameIndex = _app->GetGpuSystem()->GetFrameIndex();
        _discardNonModalFramesBefore.store(frameIndex, std::memory_order_release);
        WaitRenderFrameComplete(frameIndex);
        RetireRenderedFrames();
        if (auto renderedFrameCount = TickFrame(true, false)) {
            WaitRenderFrameComplete(renderedFrameCount.value());
        }
    }

    void MaintainWindows() {
        auto* windows = _app->GetWindowManager().Get();
        if (windows == nullptr) return;
        if (!windows->NeedsMaintenance()) return;
        const uint64_t boundary = windows->GetOperationBoundary();
        const uint64_t published = _app->GetGpuSystem()->GetFrameIndex();
        WaitRenderFrameComplete(published);
        _app->GetGpuSystem()->WaitAndRetireFlights();
        _retireFrameIndex = published;
        windows->ProcessOperations(boundary);
        if (windows->ShouldExit()) _reqExit = true;
    }

    bool PrepareFrame(bool waitForWritableSlot) {
        if (_ticking || _reqExit) return false;
        if (_framePrepared) return true;
        _ticking = true;
        auto scope = MakeScopeGuard([this]() noexcept { _ticking = false; });
        auto* gpuSystem = _app->GetGpuSystem().Get();
        MaintainWindows();
        if (_reqExit) return false;
        if (!waitForWritableSlot && _renderedFrameCount.load(std::memory_order_acquire) < gpuSystem->GetFrameIndex()) return false;
        RADRAY_PROFILE_SCOPE_N("PrepareFrame");
        if (!PrepareFlightSlot(waitForWritableSlot)) return false;

        const uint64_t frameIndex = gpuSystem->GetFrameIndex();
        const uint32_t flightIndex = static_cast<uint32_t>(frameIndex % gpuSystem->GetFlightDataCount());
        {
            RADRAY_PROFILE_SCOPE_N("Application::GpuBeginUpdateForFlight");
            _app->ServiceFrameBoundaryGT(flightIndex);
        }

        const auto now = gpuSystem->BeginFrameTiming(flightIndex);
        _deltaTime = now - _lastFrameTime;
        _lastFrameTime = now;
        _framePrepared = true;
        return true;
    }

    std::optional<uint64_t> TickFrame(bool isInModalLoop, bool waitForWritableSlot) {
        if (!PrepareFrame(waitForWritableSlot)) return std::nullopt;
        _ticking = true;
        auto scope = MakeScopeGuard([this]() noexcept { _ticking = false; });
        _framePrepared = false;
        RADRAY_PROFILE_SCOPE_N("TickFrame");
        auto* gpuSystem = _app->GetGpuSystem().Get();
        const uint64_t frameIndex = gpuSystem->GetFrameIndex();
        const uint32_t flightIndex = static_cast<uint32_t>(frameIndex % gpuSystem->GetFlightDataCount());
        const auto deltaTime = _deltaTime;
        _runnerFrameDatas[flightIndex].DeltaTime = deltaTime;
        _runnerFrameDatas[flightIndex].IsInModalLoop = isInModalLoop;
        AppUpdateResult result{};
        {
            RADRAY_PROFILE_SCOPE_N("Update");
            result = _app->Update(AppUpdateContext{
                .FlightIndex = flightIndex,
                .DeltaTime = deltaTime,
                .LastFrameLatency = gpuSystem->GetLastFrameLatency()});
        }
        _reqExit = result.ShouldExit;
        if (_reqExit) {
            return std::nullopt;
        }

        if (auto renderSystem = _app->GetRenderSystem()) renderSystem->PublishFrameGT(flightIndex);
        gpuSystem->AdvanceFrameIndex();
        _publishedFrameCount.store(frameIndex + 1, std::memory_order_release);
        _readySlotsSemaphore.release();
        return frameIndex + 1;
    }

    void NotifyRenderFrameComplete(uint64_t renderedFrameCount) {
        _renderedFrameCount.store(renderedFrameCount, std::memory_order_release);
        _renderedFrameCount.notify_all();
    }

    void WaitRenderFrameComplete(uint64_t renderedFrameCount) {
        uint64_t completed = _renderedFrameCount.load(std::memory_order_acquire);
        while (completed < renderedFrameCount) {
            _renderedFrameCount.wait(completed, std::memory_order_acquire);
            completed = _renderedFrameCount.load(std::memory_order_acquire);
        }
    }

    bool PrepareFlightSlot(bool wait) {
        RADRAY_PROFILE_SCOPE_N("WaitWritableSlot");
        auto* gpuSystem = _app->GetGpuSystem().Get();
        RetireRenderedFrames();
        if (gpuSystem->GetFrameIndex() - _retireFrameIndex < gpuSystem->GetFlightDataCount()) return true;
        if (!wait) return false;
        // Wait only for the oldest slot's submission, never for the newest recording.
        WaitRenderFrameComplete(_retireFrameIndex + 1);
        const auto flightIndex = static_cast<uint32_t>(_retireFrameIndex % gpuSystem->GetFlightDataCount());
        gpuSystem->CompleteFlightIfReady(flightIndex, true);
        ++_retireFrameIndex;
        return true;
    }

    void RetireRenderedFrames() {
        auto* gpuSystem = _app->GetGpuSystem().Get();
        const uint64_t renderedFrameCount = _renderedFrameCount.load(std::memory_order_acquire);
        while (_retireFrameIndex < renderedFrameCount) {
            const uint32_t flightIndex = static_cast<uint32_t>(_retireFrameIndex % gpuSystem->GetFlightDataCount());
            if (!gpuSystem->CompleteFlightIfReady(flightIndex, false)) {
                break;
            }
            _retireFrameIndex++;
        }
    }

    struct FrameData {
        std::chrono::duration<float> DeltaTime{};
        bool IsInModalLoop{false};
    };

    Application* _app;
    sigslot::scoped_connection _modalLoopTickConnection;
    std::counting_semaphore<> _readySlotsSemaphore;
    // 共享数据
    vector<FrameData> _runnerFrameDatas;
    std::atomic_bool _reqExit{false};
    std::atomic<uint64_t> _discardNonModalFramesBefore{0};
    std::atomic<uint64_t> _renderedFrameCount{0};
    std::atomic<uint64_t> _publishedFrameCount{0};
    // 主线程独占
    uint64_t _retireFrameIndex{0};
    std::chrono::steady_clock::time_point _lastFrameTime{std::chrono::steady_clock::now()};
    std::chrono::duration<float> _deltaTime{};
    bool _framePrepared{false};
    bool _ticking{false};
    bool _hasModalLoopActivityDuringDispatch{false};
    // 渲染线程独占
    uint64_t _renderFrameIndex{0};
    std::thread _renderThread;
};

class CpuRunner {
public:
    explicit CpuRunner(Application* app) noexcept : _app(app) {}

    int Run() {
        uint64_t frameSerial = 0;
        uint64_t pendingSerial = 0;
        uint32_t pendingFlight = 0;
        auto lastFrameTime = std::chrono::steady_clock::now();
        while (!_app->ShouldExit()) {
            auto* windows = _app->GetWindowManager().Get();
            if (windows != nullptr && windows->NeedsMaintenance()) {
                const uint64_t boundary = windows->GetOperationBoundary();
                windows->ProcessOperations(boundary);
            }
            if (pendingSerial != 0) {
                _app->GetRenderSystem()->OnFlightCompletedGT(FlightCompletion{pendingFlight, true, pendingSerial});
                pendingSerial = 0;
            }
            const uint32_t flightIndex = static_cast<uint32_t>(frameSerial % _app->_flightDataCount);
            _app->ServiceFrameBoundaryGT(flightIndex);
            if (windows != nullptr) windows->DispatchEvents();
            if (_app->ShouldExit()) break;

            const auto now = std::chrono::steady_clock::now();
            const std::chrono::duration<float> deltaTime = now - lastFrameTime;
            lastFrameTime = now;
            const auto result = _app->Update(AppUpdateContext{flightIndex, deltaTime, {}});
            if (result.ShouldExit) break;
            if (auto renderSystem = _app->GetRenderSystem()) {
                renderSystem->PublishFrameGT(flightIndex);
                renderSystem->ConsumeRenderUpdates(flightIndex, frameSerial + 1);
                pendingSerial = frameSerial + 1;
                pendingFlight = flightIndex;
            }
            ++frameSerial;
            RADRAY_PROFILE_FRAME();
        }
        if (pendingSerial != 0) {
            _app->GetRenderSystem()->OnFlightCompletedGT(FlightCompletion{pendingFlight, true, pendingSerial});
        }
        return _app->Shutdown(AppShutdownContext{});
    }

private:
    Application* _app;
};

void Application::ServiceFrameBoundaryGT(uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("Application::ServiceFrameBoundaryGT");
    if (_gpuSystem != nullptr) PumpFlightCompletions(flightIndex);
    if (_assetManager) _assetManager->Pump();
    _scheduler.Pump();
}

void Application::SetCollecting(bool collecting) {
    _scheduler._collecting = collecting;
    if (_assetManager) _assetManager->_collectingScene = collecting;
}

void Application::ApplySceneUpdatesRT(AppFrameContext& ctx) {
    if (_renderSystem != nullptr) _renderSystem->ConsumeRenderUpdates(ctx.FlightIndex(), ctx.FrameSerial());
}

void Application::WaitAndCleanupCompletedFlights() {
    _gpuSystem->WaitAndRetireFlights();
    PumpFlightCompletions(std::nullopt);
}

void Application::PumpFlightCompletions(std::optional<uint32_t> flightIndex) {
    RADRAY_ASSERT(std::this_thread::get_id() == _applicationThread);
    if (_processingFlightCompletions) return;
    _processingFlightCompletions = true;
    auto scope = MakeScopeGuard([this]() noexcept { _processingFlightCompletions = false; });

    vector<FlightCompletion> completions;
    FlightCompletion completion;
    while (_gpuSystem->_flightCompletions.TryRead(completion)) {
        completions.push_back(completion);
    }
    for (const auto& c : completions) {
        if (_renderSystem != nullptr) _renderSystem->OnFlightCompletedGT(c);
        _gpuSystem->ReleaseFrameResourcesGT(c);
    }
    if (flightIndex) {
        _gpuSystem->BeginUpdateForFlight(*flightIndex);
    } else {
        _gpuSystem->CleanupCompletedFlights();
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
    if (_renderSystem) {
        auto& frame = _renderSystem->GetFrameUpdates(flightIndex);
        if (frame.Phase != RenderSystem::SceneFlightPhase::Writable) RADRAY_ABORT("View collection requires writable flight");
        frame.Views.clear();
        {
            _renderSystem->SetCollecting(true);
            if (_worldManager) _worldManager->_collecting = true;
            auto guard = MakeScopeGuard([this]() noexcept {
                if (_worldManager) _worldManager->_collecting = false;
                _renderSystem->SetCollecting(false);
            });
            SceneViewCollector collector{frame.Views};
            OnCollectRenderViews(collector);
        }
        _renderSystem->SealFrameGT(flightIndex);
    }
}

void Application::OnCollectRenderViews(SceneViewCollector& collector) { (void)collector; }

void Application::Render(AppFrameContext& ctx) {
    this->OnRender(ctx);
}

bool Application::ShouldExit() const noexcept {
    return _exitRequested.load(std::memory_order_relaxed) || (_windowManager != nullptr && _windowManager->ShouldExit());
}

void Application::StopAndDrainRuntime() {
    _scheduler.BeginStopping();
    if (_worldManager != nullptr) _worldManager->BeginStopping();
    if (_renderSystem != nullptr) _renderSystem->BeginStoppingGT();
    if (_assetManager) _assetManager->BeginStopping();
    if (_windowManager != nullptr) _windowManager->CloseOperations();
    if (_gpuSystem != nullptr) {
        WaitAndCleanupCompletedFlights();
    }
    if (_renderSystem) _renderSystem->AbandonUnpublishedFramesGT();
    if (_gpuSystem) _gpuSystem->AbandonUnpublishedResourcesTerminalGT();
}

int Application::Shutdown(const AppShutdownContext& ctx) {
    (void)ctx;
    StopAndDrainRuntime();
    // 游戏侧清理:释放自管 per-flight 资源、置空指向 World 的非 owning 指针。
    OnShutdown();
    _scheduler.CancelAll();
    DestroyRuntime();
    return 0;
}

void Application::DestroyRuntime() noexcept {
    if (_windowManager != nullptr) _windowManager->CloseOperations();
    // 拆 World:销毁 Actor / Component，释放其持有的 StreamingAssetRef。
    if (_worldManager != nullptr) _worldManager->Shutdown();
    _worldManager.reset();
    // 其 RenderPassRegistry 随之销毁,故须先切断 WindowManager 的非 owning 引用。
    if (_windowManager != nullptr) {
        _windowManager->SetRenderSystem(nullptr);
    }
    _renderSystem.reset();
    // AssetManager 析构会 force-unload 全部资产,释放 GPU buffer(须在 device 销毁前)。
    _assetManager.reset();
    _cpuWaitFrameProcessor.reset();
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

bool Application::InitializeRuntime(const ApplicationRuntimeDescriptor& desc, RuntimeStartupResult& startup) {
    startup = {};
    const auto fail = [&startup](RuntimeStartupStatus status, std::string_view reason) {
        startup.Status = status;
        startup.Reason = reason;
        RADRAY_ERR_LOG("Application startup failed: {}", reason);
        return false;
    };
    constexpr uint8_t knownSystems = 31;
    if ((desc.Systems.value() & ~knownSystems) != 0)
        return fail(RuntimeStartupStatus::InvalidDescriptor, "Unknown ApplicationSystem flag");
    if (desc.FlightDataCount == 0)
        return fail(RuntimeStartupStatus::InvalidDescriptor, "FlightDataCount must be positive");
    if (desc.Multithreaded && !desc.Systems.HasFlag(ApplicationSystem::Gpu))
        return fail(RuntimeStartupStatus::InvalidDescriptor, "Multithreaded mode requires Gpu");
    if (!desc.AssetRoot.empty() && !desc.Systems.HasFlag(ApplicationSystem::Asset))
        return fail(RuntimeStartupStatus::InvalidDescriptor, "AssetRoot requires Asset");

    _multithreaded = desc.Multithreaded;
    _flightDataCount = desc.FlightDataCount;
    _exitRequested.store(false, std::memory_order_relaxed);
    _shaderSourceRoot = desc.ShaderSourceRoot;
    _shaderIncludePaths = desc.ShaderIncludePaths;

    if (desc.Systems.HasFlag(ApplicationSystem::Window)) {
        WindowManagerDescriptor windowManagerDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
        windowManagerDesc.Type = NativeWindowType::Win32HWND;
#endif
        _windowManager = make_unique<WindowManager>(windowManagerDesc);
    }

    if (desc.Systems.HasFlag(ApplicationSystem::Gpu)) {
        render::VulkanInstanceDescriptor instanceDesc{
        .AppName = desc.AppName,
        .EngineName = desc.EngineName,
        .IsEnableDebugLayer = desc.EnableValidation,
        .IsEnableGpuBasedValid = false,
        .IsEnableSynchronizationValidation = desc.EnableSynchronizationValidation};
        render::DXGIFactoryDescriptor factoryDesc{
        .IsEnableDebugLayer = desc.EnableValidation,
        .IsEnableGpuBasedValid = false};
        render::VulkanCommandQueueDescriptor queueDesc{render::QueueType::Direct, 1};
        render::DeviceDescriptor deviceDesc{};
        if (desc.Backend == render::RenderBackend::Vulkan) {
            render::VulkanDeviceDescriptor vulkanDeviceDesc{};
            vulkanDeviceDesc.Queues = std::span{&queueDesc, 1};
            deviceDesc = vulkanDeviceDesc;
        } else if (desc.Backend == render::RenderBackend::D3D12) {
            deviceDesc = render::D3D12DeviceDescriptor{};
        } else {
            DestroyRuntime();
            return fail(RuntimeStartupStatus::InvalidDescriptor, "Unsupported render backend");
        }

        GpuSystemDescriptor gpuSysDesc{
        .VulkanInstance = instanceDesc,
        .DXGIFactory = factoryDesc,
        .Device = deviceDesc,
        .MainQueueIndex = 0,
        .BackBufferCount = desc.BackBufferCount,
        .FlightDataCount = desc.FlightDataCount,
        .EnableFrameProfiler = desc.EnableGpuFrameProfiler};
        _gpuSystem = GpuSystem::TryCreate(gpuSysDesc, startup);
        if (_gpuSystem == nullptr) {
            DestroyRuntime();
            return false;
        }
    }
    if (desc.Systems.HasFlag(ApplicationSystem::Render)) {
        _renderSystem = make_unique<RenderSystem>(this, desc.FlightDataCount);
    }
    if (desc.Systems.HasFlag(ApplicationSystem::World)) {
        _worldManager = make_unique<WorldManager>(this, _renderSystem.get());
    }
    if (desc.Systems.HasFlag(ApplicationSystem::Asset)) {
        _assetManager = make_unique<AssetManager>();
        if (_gpuSystem == nullptr) _cpuWaitFrameProcessor = make_unique<CpuWaitFrameProcessor>();
    }
    if (_assetManager != nullptr && !desc.AssetRoot.empty()) {
        string error;
        _assetDatabase = AssetDatabase::Open(
            desc.AssetRoot,
            MakeDefaultAssetImporters(),
            error);
        if (_assetDatabase == nullptr) {
            RADRAY_ERR_LOG("open asset database failed: {}", error);
        }
    }

    if (_windowManager != nullptr) {
        _windowManager->SetGpuSystem(_gpuSystem.get());
        _windowManager->SetRenderSystem(_renderSystem.get());
    }
    if (_gpuSystem != nullptr) _gpuSystem->SetWindowManager(_windowManager.get());
    if (_renderSystem != nullptr) _renderSystem->SetGpuSystem(_gpuSystem.get());
    if (_assetManager != nullptr) {
        _assetManager->SetWaitFrameProcessor(_gpuSystem != nullptr ? static_cast<IWaitFrameProcessor*>(_gpuSystem.get()) : _cpuWaitFrameProcessor.get());
        _assetManager->SetAssetSource(_assetDatabase.get());
    }

    if (_renderSystem != nullptr && _gpuSystem != nullptr && !_renderSystem->OnInitialize()) {
        DestroyRuntime();
        return fail(RuntimeStartupStatus::InitializationFailed, "RenderSystem initialization failed");
    }

    if (_windowManager != nullptr) {
        WindowCreateDescriptor wndDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
        wndDesc.Title = desc.WindowTitle;
        wndDesc.Width = desc.WindowWidth;
        wndDesc.Height = desc.WindowHeight;
        wndDesc.Resizable = true;
        wndDesc.StartVisible = true;
#else
        DestroyRuntime();
        return fail(RuntimeStartupStatus::InitializationFailed, "Unsupported window platform");
#endif
        std::optional<WindowSwapChainDescriptor> swapchainDesc;
        if (_gpuSystem != nullptr) {
            swapchainDesc = WindowSwapChainDescriptor{};
            swapchainDesc->Width = static_cast<uint32_t>(desc.WindowWidth);
            swapchainDesc->Height = static_cast<uint32_t>(desc.WindowHeight);
            swapchainDesc->Format = desc.BackBufferFormat;
            swapchainDesc->PresentMode = desc.PresentMode;
        }
        if (!_windowManager->InitializeMainWindow(wndDesc, swapchainDesc)) {
            DestroyRuntime();
            return fail(RuntimeStartupStatus::InitializationFailed, "Main window or swapchain initialization failed");
        }
    }
    startup.Status = RuntimeStartupStatus::Started;
    startup.Reason.clear();
    return true;
}

int Application::Run(const ApplicationRuntimeDescriptor& desc) {
    RuntimeStartupResult startup;
    return Run(desc, startup);
}

int Application::Run(const ApplicationRuntimeDescriptor& desc, RuntimeStartupResult& startup) {
    if (!InitializeRuntime(desc, startup)) return 1;
    OnInit();
    if (_worldManager != nullptr) _worldManager->FinalizeWorldsGT();
    if (ShouldExit()) return Shutdown(AppShutdownContext{});
    return StartLoop();
}

int Application::StartLoop() {
    if (_gpuSystem == nullptr) return CpuRunner{this}.Run();
    if (_multithreaded) {
        return ThreadedRunner{this}.Run();
    } else {
        return SingleThreadRunner{this}.Run();
    }
}

}  // namespace radray
