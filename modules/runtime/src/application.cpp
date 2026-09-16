#include <radray/runtime/application.h>

#include <chrono>

#include <atomic>
#include <mutex>
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
#include <radray/runtime/game_framework/world.h>
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

}  // namespace

bool SwitchToApplicationSchedulerAwaitable::await_ready() const noexcept {
    return _scheduler == nullptr || _stop.stop_requested();
}

bool SwitchToApplicationSchedulerAwaitable::await_suspend(std::coroutine_handle<> continuation) {
    if (_scheduler == nullptr || _stop.stop_requested()) {
        return false;
    }
    _record = _scheduler->Enqueue(_stop, continuation);
    return true;
}

bool SwitchToApplicationSchedulerAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        return !_stop.stop_requested();
    }

    const bool completed = !_record->Canceled && !_record->Stop.stop_requested();
    if (_scheduler != nullptr) {
        _scheduler->Erase(_record);
    }
    _record = nullptr;
    return completed;
}

ApplicationScheduler::~ApplicationScheduler() noexcept {
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

bool ApplicationScheduler::IsAlive(ApplicationSchedulerRecord* record) const noexcept {
    return _records.IsAlive(record);
}

void ApplicationScheduler::ResumeRecord(ApplicationSchedulerRecord* record) noexcept {
    _records.ResumeRecord(record);
}

void ApplicationScheduler::CancelRecord(ApplicationSchedulerRecord* record) noexcept {
    _records.CancelRecord(record);
}

void ApplicationScheduler::Pump() {
    RADRAY_PROFILE_SCOPE_N("ApplicationScheduler::Pump");
    const size_t recordCount = _records.Count();
    for (size_t i = 0; i < recordCount && !_records.Empty(); ++i) {
        ApplicationSchedulerRecord* record = _records.Front();
        if (record->Stop.stop_requested()) {
            record->Canceled = true;
        }
        ResumeRecord(record);
        if (IsAlive(record)) {
            Erase(record);
        }
    }
}

void ApplicationScheduler::CancelAll() noexcept {
    _records.CancelAll();
}

Application::Application() noexcept = default;

Application::~Application() noexcept {
    if (_windowManager != nullptr) _windowManager->CloseOperations();
    if (_gpuSystem != nullptr) {
        WaitAndCleanupCompletedFlights();
    }
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
        const GpuSystem* gpuSystem = app->GetGpuSystem();
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

        const WindowManager* windowManager = _app->GetWindowManager();
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
        : _app(app),
          _modalLoopTickConnection(_app->GetWindowManager()->EventModalLoopTick().connect(&SingleThreadRunner::OnModalLoopTick, this)) {}

    int Run() {
        while (true) {
#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
            StopWin32ModalVBlank();
#endif
            PrepareFrame(false);
            _hasModalLoopActivityDuringDispatch = false;
            _isDispatchingEvents = true;
            _app->GetWindowManager()->DispatchEvents();
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
        auto* windows = _app->GetWindowManager();
        if (!windows->NeedsMaintenance()) return;
        const uint64_t boundary = windows->GetOperationBoundary();
        _app->GetGpuSystem()->WaitAndRetireFlights();
        windows->ProcessOperations(boundary);
        if (windows->ShouldExit()) _reqExit = true;
    }

    bool PrepareFrame(bool isInModalLoop) {
        if (_ticking || _reqExit) return false;
        if (_framePrepared) return true;
        _ticking = true;
        auto scope = MakeScopeGuard([this]() noexcept { _ticking = false; });
        auto* gpuSystem = _app->GetGpuSystem();
        MaintainWindows();
        if (_reqExit) return false;
        const uint32_t flightIndex = gpuSystem->GetCurrentFlightIndex();
        if (!gpuSystem->CompleteFlightIfReady(flightIndex, !isInModalLoop)) return false;
        _app->BeginUpdateForFlight(flightIndex);
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
        auto* gpuSystem = _app->GetGpuSystem();
        const uint32_t flightIndex = gpuSystem->GetCurrentFlightIndex();
        const auto deltaTime = _deltaTime;

        MaintainWindows();
        if (_reqExit) return;

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

        MaintainWindows();
        if (_app->GetWindowManager()->ShouldExit()) {
            _reqExit = true;
            return;
        }

        AppFrameContext frameCtx = gpuSystem->BeginFrameRecord(
            flightIndex,
            deltaTime,
            gpuSystem->GetLastFrameLatency(),
            isInModalLoop);
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
          _modalLoopTickConnection(_app->GetWindowManager()->EventModalLoopTick().connect(&ThreadedRunner::OnModalLoopTick, this)),
          _writableSlotsSemaphore(_app->GetGpuSystem()->GetFlightDataCount()),
          _readySlotsSemaphore(0),
          _runnerFrameDatas(_app->GetGpuSystem()->GetFlightDataCount()),
          _renderThread(&ThreadedRunner::RenderThread, this) {
    }

    int Run() {
        while (true) {
            PrepareFrame(true);
            _hasModalLoopActivityDuringDispatch = false;
            _app->GetWindowManager()->DispatchEvents();

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

        _app->GetWindowManager()->CloseOperations();
        _writableSlotsSemaphore.release();
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
            RetireRenderedFrames(false, true);

            auto* gpuSystem = _app->GetGpuSystem();
            {
                RADRAY_PROFILE_SCOPE_N("WaitReadySlot");
                _readySlotsSemaphore.acquire();
            }

            if (_reqExit && _renderFrameIndex == _publishedFrameCount.load(std::memory_order_acquire)) {
                RetireRenderedFrames(true, false);
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
        RetireRenderedFrames(false, false);
        if (auto renderedFrameCount = TickFrame(true, false)) {
            WaitRenderFrameComplete(renderedFrameCount.value());
        }
    }

    void MaintainWindows() {
        auto* windows = _app->GetWindowManager();
        if (!windows->NeedsMaintenance()) return;
        const uint64_t boundary = windows->GetOperationBoundary();
        WaitRenderFrameComplete(_publishedFrameCount.load(std::memory_order_acquire));
        RetireRenderedFrames(true, false);
        _app->GetGpuSystem()->WaitAndRetireFlights();
        windows->ProcessOperations(boundary);
        if (windows->ShouldExit()) _reqExit = true;
    }

    bool PrepareFrame(bool waitForWritableSlot) {
        if (_ticking || _reqExit) return false;
        if (_framePrepared) return true;
        _ticking = true;
        auto scope = MakeScopeGuard([this]() noexcept { _ticking = false; });
        auto* gpuSystem = _app->GetGpuSystem();
        MaintainWindows();
        if (_reqExit) return false;
        if (!waitForWritableSlot && _renderedFrameCount.load(std::memory_order_acquire) < gpuSystem->GetFrameIndex()) return false;
        RADRAY_PROFILE_SCOPE_N("PrepareFrame");
        if (waitForWritableSlot) {
            RetireRenderedFrames(false, false);
            RADRAY_PROFILE_SCOPE_N("WaitWritableSlot");
            WaitForWritableFlightSlot();
        } else if (!_writableSlotsSemaphore.try_acquire()) {
            return false;
        }

        const uint64_t frameIndex = gpuSystem->GetFrameIndex();
        const uint32_t flightIndex = static_cast<uint32_t>(frameIndex % gpuSystem->GetFlightDataCount());
        {
            RADRAY_PROFILE_SCOPE_N("Application::GpuBeginUpdateForFlight");
            _app->BeginUpdateForFlight(flightIndex);
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
        auto* gpuSystem = _app->GetGpuSystem();
        const uint64_t frameIndex = gpuSystem->GetFrameIndex();
        const uint32_t flightIndex = static_cast<uint32_t>(frameIndex % gpuSystem->GetFlightDataCount());
        const auto deltaTime = _deltaTime;
        MaintainWindows();
        if (_reqExit) return std::nullopt;
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

        MaintainWindows();
        if (_app->GetWindowManager()->ShouldExit()) {
            _reqExit = true;
            return std::nullopt;
        }

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

    void WaitForWritableFlightSlot() {
        for (;;) {
            if (_writableSlotsSemaphore.try_acquire()) {
                return;
            }
            RetireRenderedFrames(false, false);
            if (_writableSlotsSemaphore.try_acquire()) {
                return;
            }

            GpuFenceSignal submitted{};
            uint64_t waitRendered = 0;
            {
                std::lock_guard lock(_retireMutex);
                auto* gpuSystem = _app->GetGpuSystem();
                const uint64_t renderedFrameCount = _renderedFrameCount.load(std::memory_order_acquire);
                if (_retireFrameIndex < renderedFrameCount) {
                    const uint32_t flightIndex = static_cast<uint32_t>(_retireFrameIndex % gpuSystem->GetFlightDataCount());
                    submitted = gpuSystem->GetFlightGpuSignal(flightIndex);
                } else {
                    waitRendered = _retireFrameIndex + 1;
                }
            }
            if (submitted.IsValid()) {
                RADRAY_PROFILE_SCOPE_N("WaitSubmittedFlight");
                submitted.Fence->Wait(submitted.Value);
            } else if (waitRendered != 0) {
                WaitRenderFrameComplete(waitRendered);
            }
        }
    }

    void RetireRenderedFrames(bool waitForPendingFrames, bool waitWhenFrameSlotsFull) {
        std::lock_guard lock(_retireMutex);
        auto* gpuSystem = _app->GetGpuSystem();
        const uint64_t renderedFrameCount = _renderedFrameCount.load(std::memory_order_acquire);
        while (_retireFrameIndex < renderedFrameCount) {
            const uint64_t inFlightFrameCount = renderedFrameCount - _retireFrameIndex;
            const uint32_t flightIndex = static_cast<uint32_t>(_retireFrameIndex % gpuSystem->GetFlightDataCount());
            bool wait = waitForPendingFrames;
            if (!wait && waitWhenFrameSlotsFull && inFlightFrameCount >= gpuSystem->GetFlightDataCount()) {
                wait = true;
            }
            if (!gpuSystem->CompleteFlightIfReady(flightIndex, wait)) {
                break;
            }
            _retireFrameIndex++;
            _writableSlotsSemaphore.release();
        }
    }

    struct FrameData {
        std::chrono::duration<float> DeltaTime{};
        bool IsInModalLoop{false};
    };

    Application* _app;
    sigslot::scoped_connection _modalLoopTickConnection;
    std::counting_semaphore<> _writableSlotsSemaphore;
    std::counting_semaphore<> _readySlotsSemaphore;
    // 共享数据
    vector<FrameData> _runnerFrameDatas;
    std::atomic_bool _reqExit{false};
    std::atomic<uint64_t> _discardNonModalFramesBefore{0};
    std::atomic<uint64_t> _renderedFrameCount{0};
    std::atomic<uint64_t> _publishedFrameCount{0};
    std::mutex _retireMutex;
    // 主线程独占
    std::chrono::steady_clock::time_point _lastFrameTime{std::chrono::steady_clock::now()};
    std::chrono::duration<float> _deltaTime{};
    bool _framePrepared{false};
    bool _ticking{false};
    bool _hasModalLoopActivityDuringDispatch{false};
    // 渲染线程独占
    uint64_t _renderFrameIndex{0};
    uint64_t _retireFrameIndex{0};
    std::thread _renderThread;
};

void Application::BeginUpdateForFlight(uint32_t flightIndex) {
    PumpFlightCompletions(flightIndex);
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
    // 1) 提交资产加载结果并回收零引用资产。
    if (_assetManager != nullptr) {
        _assetManager->Pump();
    }
    // 恢复需要在应用 update 线程上继续执行的协程。
    _scheduler.Pump();
    // 2) 游戏逻辑。
    OnUpdate(ctx);
    // 3) World Tick。
    if (_world != nullptr) {
        _world->Tick(ctx.DeltaTime.count());
    }
    return AppUpdateResult{ShouldExit()};
}

void Application::Render(AppFrameContext& ctx) {
    this->OnRender(ctx);
}

bool Application::ShouldExit() const noexcept {
    return _windowManager != nullptr && _windowManager->ShouldExit();
}

int Application::Shutdown(const AppShutdownContext& ctx) {
    (void)ctx;
    if (_windowManager != nullptr) _windowManager->CloseOperations();
    if (_gpuSystem != nullptr) {
        WaitAndCleanupCompletedFlights();
    }
    // 游戏侧清理:释放自管 per-flight 资源、置空指向 World 的非 owning 指针。
    OnShutdown();
    _scheduler.CancelAll();
    DestroyRuntime();
    return 0;
}

void Application::DestroyRuntime() noexcept {
    if (_windowManager != nullptr) _windowManager->CloseOperations();
    // 拆 World:销毁 Actor / Component，释放其持有的 StreamingAssetRef。
    _world.reset();
    // 其 RenderPassRegistry 随之销毁,故须先切断 WindowManager 的非 owning 引用。
    if (_windowManager != nullptr) {
        _windowManager->SetRenderSystem(nullptr);
    }
    _renderSystem.reset();
    // AssetManager 析构会 force-unload 全部资产,释放 GPU buffer(须在 device 销毁前)。
    _assetManager.reset();
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
    _multithreaded = desc.Multithreaded;
    _shaderSourceRoot = desc.ShaderSourceRoot;
    _shaderIncludePaths = desc.ShaderIncludePaths;

    WindowManagerDescriptor windowManagerDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
    windowManagerDesc.Type = NativeWindowType::Win32HWND;
#endif
    _windowManager = make_unique<WindowManager>(windowManagerDesc);

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
        RADRAY_ABORT("unsupported render backend");
    }

    GpuSystemDescriptor gpuSysDesc{
        .VulkanInstance = instanceDesc,
        .DXGIFactory = factoryDesc,
        .Device = deviceDesc,
        .MainQueueIndex = 0,
        .BackBufferCount = desc.BackBufferCount,
        .FlightDataCount = desc.FlightDataCount};
    _gpuSystem = make_unique<GpuSystem>(gpuSysDesc);
    _renderSystem = make_unique<RenderSystem>(this);
    _assetManager = make_unique<AssetManager>();
    if (!desc.AssetRoot.empty()) {
        string error;
        _assetDatabase = AssetDatabase::Open(
            desc.AssetRoot,
            MakeDefaultAssetImporters(),
            error);
        if (_assetDatabase == nullptr) {
            RADRAY_ERR_LOG("open asset database failed: {}", error);
        }
    }
    _world = make_unique<World>(this);

    _windowManager->SetGpuSystem(_gpuSystem.get());
    _windowManager->SetRenderSystem(_renderSystem.get());
    _gpuSystem->SetWindowManager(_windowManager.get());
    _renderSystem->SetGpuSystem(_gpuSystem.get());
    _assetManager->SetWaitFrameProcessor(_gpuSystem.get());
    _assetManager->SetAssetSource(_assetDatabase.get());

    string renderError;
    if (!_renderSystem->OnInitialize(renderError)) {
        RADRAY_ERR_LOG("initialize RenderSystem failed: {}", renderError);
        DestroyRuntime();
        return false;
    }

    WindowCreateDescriptor wndDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
    wndDesc.Title = desc.WindowTitle;
    wndDesc.Width = desc.WindowWidth;
    wndDesc.Height = desc.WindowHeight;
    wndDesc.Resizable = true;
    wndDesc.StartVisible = true;
#else
    RADRAY_ABORT("unsupported platform");
#endif
    WindowSwapChainDescriptor swapchainDesc{};
    swapchainDesc.Width = static_cast<uint32_t>(desc.WindowWidth);
    swapchainDesc.Height = static_cast<uint32_t>(desc.WindowHeight);
    swapchainDesc.Format = desc.BackBufferFormat;
    swapchainDesc.PresentMode = desc.PresentMode;
    if (!_windowManager->InitializeMainWindow(wndDesc, swapchainDesc)) {
        DestroyRuntime();
        return false;
    }
    return true;
}

int Application::Run(const ApplicationRuntimeDescriptor& desc) {
    if (!InitializeRuntime(desc)) return 1;
    OnInit();
    return StartLoop();
}

int Application::StartLoop() {
    if (_multithreaded) {
        return ThreadedRunner{this}.Run();
    } else {
        return SingleThreadRunner{this}.Run();
    }
}

}  // namespace radray
