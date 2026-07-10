#include <radray/runtime/application.h>

#include <chrono>

#include <atomic>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <thread>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/service_registry.h>
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

AppSubsystem::AppSubsystem() noexcept = default;

AppSubsystem::~AppSubsystem() noexcept = default;

void AppSubsystem::OnInit(Application& app) {
    (void)app;
}

void AppSubsystem::OnUpdate(Application& app, const AppUpdateContext& ctx) {
    (void)app;
    (void)ctx;
}

void AppSubsystem::OnRenderBegin(AppFrameContext& ctx) {
    (void)ctx;
}

bool AppSubsystem::OnRender(AppFrameContext& ctx, const AppFrameTarget& target, bool contentDrawn) {
    (void)ctx;
    (void)target;
    (void)contentDrawn;
    return false;
}

void AppSubsystem::OnRenderEnd(AppFrameContext& ctx) {
    (void)ctx;
}

void AppSubsystem::OnRenderComplete(Application& app, const AppRenderCompleteContext& ctx) {
    (void)app;
    (void)ctx;
}

void AppSubsystem::OnSwapChainRecreate(Application& app, const AppSwapChainRecreateContext& ctx) {
    (void)app;
    (void)ctx;
}

void AppSubsystem::OnShutdown(Application& app) {
    (void)app;
}

bool AppSubsystem::IsInitialized() const noexcept {
    return _initialized;
}

void AppSubsystem::Initialize(Application& app) {
    if (_initialized) {
        return;
    }
    OnInit(app);
    _initialized = true;
}

void AppSubsystem::Teardown(Application& app) noexcept {
    if (!_initialized) {
        return;
    }
    OnShutdown(app);
    _initialized = false;
}

Application::Application() noexcept = default;

Application::~Application() noexcept = default;

void Application::OnInit() {
}

void Application::OnUpdate(const AppUpdateContext& ctx) {
    (void)ctx;
}

bool Application::OnRenderView(AppFrameContext& ctx, const AppFrameTarget& target) {
    (void)ctx;
    (void)target;
    return false;
}

void Application::OnShutdown() {
}

void Application::OnRenderFrameComplete(const AppRenderCompleteContext& ctx) {
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
            CheckFrameComplete(false);
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
        MarkModalLoopActivityDuringDispatch();
#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
        StartWin32ModalVBlank(modalWindow);
#endif
        if (CheckFrameComplete(true)) {
            TickFrame(true);
        }
    }

    bool CheckFrameComplete(bool isInModalLoop) {
        auto* gpuSystem = _app->GetGpuSystem();
        const uint32_t flightIndex = gpuSystem->GetCurrentFlightIndex();
        return gpuSystem->CompleteFlightIfReady(flightIndex, !isInModalLoop);
    }

    void TickFrame(bool isInModalLoop) {
        if (isInModalLoop) {
            MarkModalLoopActivityDuringDispatch();
        }
        auto* gpuSystem = _app->GetGpuSystem();
        const uint32_t flightIndex = gpuSystem->GetCurrentFlightIndex();
        gpuSystem->BeginUpdateForFlight(flightIndex);

        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<float> deltaTime = now - _lastFrameTime;
        _lastFrameTime = now;

        _app->GetWindowManager()->CheckRecreateSwapChains();
        gpuSystem->PumpFrameUploadScheduler();

        auto result = _app->Update(AppUpdateContext{
            .FlightIndex = flightIndex,
            .DeltaTime = deltaTime,
            .LastFrameLatency = gpuSystem->GetLastFrameLatency()});
        _reqExit = result.ShouldExit;
        if (_reqExit) {
            return;
        }

        _app->GetWindowManager()->CheckRecreateSwapChains();

        AppFrameContext frameCtx = gpuSystem->BeginFrameRecord(
            flightIndex,
            deltaTime,
            gpuSystem->GetLastFrameLatency(),
            isInModalLoop);
        _app->Render(frameCtx);
        gpuSystem->EndFrameRecordAndSubmit(flightIndex);
        gpuSystem->AdvanceFrameIndex();
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
    bool _reqExit{false};
    bool _isDispatchingEvents{false};
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

    if (_runner->CheckFrameComplete(true)) {
        _runner->TickFrame(true);
    }
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
          _renderThread(&ThreadedRunner::RenderThread, this) {}

    int Run() {
        while (true) {
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
        while (true) {
            RetireRenderedFrames(false, true);

            auto* gpuSystem = _app->GetGpuSystem();
            _readySlotsSemaphore.acquire();

            if (_reqExit) {
                RetireRenderedFrames(true, false);
                break;
            }

            uint32_t flightIndex = static_cast<uint32_t>(_renderFrameIndex % gpuSystem->GetFlightDataCount());
            auto& runnerFrameData = _runnerFrameDatas[flightIndex];
            if (!runnerFrameData.IsInModalLoop && _renderFrameIndex < _discardNonModalFramesBefore.load(std::memory_order_acquire)) {
                _app->NotifyRenderComplete(AppRenderCompleteContext{
                    .FlightIndex = flightIndex,
                    .GpuWorkCompleted = false});
                _renderFrameIndex++;
                NotifyRenderFrameComplete(_renderFrameIndex);
                continue;
            }
            AppFrameContext frameCtx = gpuSystem->BeginFrameRecord(
                flightIndex,
                runnerFrameData.DeltaTime,
                gpuSystem->GetLastFrameLatency(),
                runnerFrameData.IsInModalLoop);
            _app->Render(frameCtx);
            gpuSystem->EndFrameRecordAndSubmit(flightIndex);

            _renderFrameIndex++;
            NotifyRenderFrameComplete(_renderFrameIndex);
        }
    }

    void OnModalLoopTick(NativeWindow*) {
        _hasModalLoopActivityDuringDispatch = true;
        if (_reqExit) {
            return;
        }

        const uint64_t frameIndex = _app->GetGpuSystem()->GetFrameIndex();
        _discardNonModalFramesBefore.store(frameIndex, std::memory_order_release);
        WaitRenderFrameComplete(frameIndex);
        RetireRenderedFrames(false, false);
        if (auto renderedFrameCount = TickFrame(true, false)) {
            WaitRenderFrameComplete(renderedFrameCount.value());
        }
    }

    void CheckRecreateSwapChains() {
        auto* windowManager = _app->GetWindowManager();
        if (!windowManager->HasSwapChainToRecreate()) {
            return;
        }
        WaitRenderThreadIdle();
        windowManager->CheckRecreateSwapChains();
    }

    std::optional<uint64_t> TickFrame(bool isInModalLoop, bool waitForWritableSlot) {
        auto* gpuSystem = _app->GetGpuSystem();
        if (waitForWritableSlot) {
            WaitRenderFrameComplete(gpuSystem->GetFrameIndex());
            RetireRenderedFrames(false, false);
            CheckRecreateSwapChains();
            _writableSlotsSemaphore.acquire();
        } else if (!_writableSlotsSemaphore.try_acquire()) {
            return std::nullopt;
        } else {
            CheckRecreateSwapChains();
        }

        const uint64_t frameIndex = gpuSystem->GetFrameIndex();
        const uint32_t flightIndex = static_cast<uint32_t>(frameIndex % gpuSystem->GetFlightDataCount());
        gpuSystem->BeginUpdateForFlight(flightIndex);

        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<float> deltaTime = now - _lastFrameTime;
        _lastFrameTime = now;

        _runnerFrameDatas[flightIndex].DeltaTime = deltaTime;
        _runnerFrameDatas[flightIndex].IsInModalLoop = isInModalLoop;
        gpuSystem->PumpFrameUploadScheduler();
        auto result = _app->Update(AppUpdateContext{
            .FlightIndex = flightIndex,
            .DeltaTime = deltaTime,
            .LastFrameLatency = gpuSystem->GetLastFrameLatency()});
        _reqExit = result.ShouldExit;
        if (_reqExit) {
            return std::nullopt;
        }

        CheckRecreateSwapChains();

        gpuSystem->AdvanceFrameIndex();
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

    void WaitRenderThreadIdle() {
        WaitRenderFrameComplete(_app->GetGpuSystem()->GetFrameIndex());
        RetireRenderedFrames(true, false);
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
    std::mutex _retireMutex;
    // 主线程独占
    std::chrono::steady_clock::time_point _lastFrameTime{std::chrono::steady_clock::now()};
    bool _hasModalLoopActivityDuringDispatch{false};
    // 渲染线程独占
    uint64_t _renderFrameIndex{0};
    uint64_t _retireFrameIndex{0};
    std::thread _renderThread;
};

AppSubsystem* Application::RegisterSubsystem(unique_ptr<AppSubsystem> subsystem) {
    if (_windowManager != nullptr || _gpuSystem != nullptr || _renderSystem != nullptr || _assetManager != nullptr || _world != nullptr || _device != nullptr) {
        RADRAY_ERR_LOG("Application: subsystems must be registered before Run()");
        return nullptr;
    }
    if (subsystem == nullptr) {
        return nullptr;
    }

    const RuntimeTypeId type = subsystem->GetTypeId();
    if (type.IsEmpty()) {
        RADRAY_ERR_LOG("Application: subsystem type id must not be empty");
        return nullptr;
    }

    for (const unique_ptr<AppSubsystem>& existing : _subsystems) {
        if (existing != nullptr && existing->GetTypeId() == type) {
            return existing.get();
        }
    }

    AppSubsystem* raw = subsystem.get();
    _subsystems.emplace_back(std::move(subsystem));
    return raw;
}

AppSubsystem* Application::GetSubsystem(RuntimeTypeId type) noexcept {
    for (const unique_ptr<AppSubsystem>& subsystem : _subsystems) {
        if (subsystem != nullptr && subsystem->GetTypeId() == type) {
            return subsystem.get();
        }
    }
    return nullptr;
}

const AppSubsystem* Application::GetSubsystem(RuntimeTypeId type) const noexcept {
    for (const unique_ptr<AppSubsystem>& subsystem : _subsystems) {
        if (subsystem != nullptr && subsystem->GetTypeId() == type) {
            return subsystem.get();
        }
    }
    return nullptr;
}

vector<AppSubsystem*> Application::GetSubsystems() noexcept {
    vector<AppSubsystem*> subsystems;
    subsystems.reserve(_subsystems.size());
    for (const unique_ptr<AppSubsystem>& subsystem : _subsystems) {
        if (subsystem != nullptr) {
            subsystems.push_back(subsystem.get());
        }
    }
    return subsystems;
}

void Application::NotifyRenderComplete(const AppRenderCompleteContext& ctx) {
    OnRenderComplete(ctx);
}

// ════════════════════════════════════════════════════════════════
//  固化的帧序 / 生命周期(框架驱动,非游戏 override 点)
// ════════════════════════════════════════════════════════════════

void Application::InitializeSubsystems() {
    for (const unique_ptr<AppSubsystem>& subsystem : _subsystems) {
        if (subsystem != nullptr) {
            subsystem->Initialize(*this);
        }
    }
}

void Application::TeardownSubsystems() noexcept {
    for (size_t i = _subsystems.size(); i > 0; --i) {
        const size_t index = i - 1;
        if (_subsystems[index] != nullptr) {
            _subsystems[index]->Teardown(*this);
        }
    }
    _subsystems.clear();
}

AppUpdateResult Application::Update(const AppUpdateContext& ctx) {
    // 1) 推进资产加载状态机(恢复本帧 GPU 上传已完成的协程 → 启动未启动协程 → reap 终态)。
    if (_assetManager != nullptr) {
        _assetManager->Pump();
    }
    // Resume coroutines that need to continue on the application update thread.
    _scheduler.Pump();
    // 2) 游戏逻辑。
    OnUpdate(ctx);
    // 3) World Tick(组件解析当帧就绪的资产、建代理)。
    if (_world != nullptr) {
        _world->Tick(ctx.DeltaTime.count());
    }
    // 4) 注册子系统帧更新。UI、调试层等可在这里推进自己的游戏线程状态。
    for (const unique_ptr<AppSubsystem>& subsystem : _subsystems) {
        if (subsystem != nullptr) {
            subsystem->OnUpdate(*this, ctx);
        }
    }
    return AppUpdateResult{ShouldExit()};
}

bool Application::ShouldExit() const noexcept {
    return _windowManager != nullptr && _windowManager->ShouldExit();
}

void Application::Render(AppFrameContext& ctx) {
    if (_renderSystem != nullptr) {
        _renderSystem->Render(ctx);
    }
}

bool Application::RenderViewContent(AppFrameContext& ctx, const AppFrameTarget& target) {
    return OnRenderView(ctx, target);
}

void Application::OnRenderComplete(const AppRenderCompleteContext& ctx) {
    OnRenderFrameComplete(ctx);

    for (const unique_ptr<AppSubsystem>& subsystem : _subsystems) {
        if (subsystem != nullptr) {
            subsystem->OnRenderComplete(*this, ctx);
        }
    }
}

void Application::OnSwapChainRecreate(const AppSwapChainRecreateContext& ctx) {
    for (const unique_ptr<AppSubsystem>& subsystem : _subsystems) {
        if (subsystem != nullptr) {
            subsystem->OnSwapChainRecreate(*this, ctx);
        }
    }
}

int Application::Shutdown(const AppShutdownContext& ctx) {
    (void)ctx;
    if (_gpuSystem != nullptr) {
        _gpuSystem->WaitAndCleanupCompletedFlights();
    }
    // 游戏侧清理:释放自管 per-flight 资源、置空指向 World 的非 owning 指针。
    OnShutdown();
    // 子系统可能持有窗口、renderer、GPU 临时资源；在 World / AssetManager 拆除前释放。
    TeardownSubsystems();
    _scheduler.CancelAll();
    // 拆 World:销毁 Actor → 移除 SceneProxy → drop 其持有的 StreamingAssetRef。
    _world.reset();
    // RenderSystem owns Scene objects and must outlive World teardown.
    _renderSystem.reset();
    // AssetManager 析构会 force-unload 全部资产,释放 GPU buffer(须在 device 销毁前)。
    _assetManager.reset();
    if (_windowManager != nullptr) {
        _windowManager->DetachAllSwapChains();
        _windowManager->SetGpuSystem(nullptr);
    }
    if (_gpuSystem != nullptr) {
        _gpuSystem->SetWindowManager(nullptr);
    }
    _gpuSystem.reset();
    _windowManager.reset();
    _device.reset();
    _dxgiFactory.reset();
    return 0;
}

void Application::InitializeRuntime(const ApplicationRuntimeDescriptor& desc) {
    _multithreaded = desc.Multithreaded;

    // —— device / instance ——
    if (desc.Backend == render::RenderBackend::Vulkan) {
        render::VulkanInstanceDescriptor insDesc{
            .AppName = desc.AppName,
            .EngineName = desc.EngineName,
            .IsEnableDebugLayer = desc.EnableValidation,
            .IsEnableGpuBasedValid = false};
        render::InstanceVulkan::InitEnv(insDesc).Unwrap();
        render::VulkanCommandQueueDescriptor queueDesc{render::QueueType::Direct, 1};
        render::VulkanDeviceDescriptor deviceDesc{};
        deviceDesc.Queues = std::span{&queueDesc, 1};
        _device = render::Device::Create(deviceDesc).Unwrap();
    } else if (desc.Backend == render::RenderBackend::D3D12) {
        render::DXGIFactoryDescriptor dxgiDesc{};
        dxgiDesc.IsEnableDebugLayer = desc.EnableValidation;
        dxgiDesc.IsEnableGpuBasedValid = false;
        _dxgiFactory = render::DXGIFactory::Create(dxgiDesc).Unwrap();
        render::D3D12DeviceDescriptor deviceDesc{};
        deviceDesc.Factory = _dxgiFactory.get();
        _device = render::Device::Create(deviceDesc).Unwrap();
    } else {
        RADRAY_ABORT("unsupported render backend");
    }

    // ════════════════════════════════════════════════════════════════
    //  phase 1:实例化全部核心服务(构造函数只做平凡/自身初始化,不碰兄弟系统)。
    //  顺序任意 —— 互相引用的装配推迟到 phase 2。
    // ════════════════════════════════════════════════════════════════
    WindowManagerDescriptor windowManagerDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
    windowManagerDesc.Type = NativeWindowType::Win32HWND;
#endif
    _windowManager = make_unique<WindowManager>(this, windowManagerDesc);

    GpuSystemDescriptor gpuSysDesc{
        .Device = _device.get(),
        .MainQueueIndex = 0,
        .BackBufferCount = desc.BackBufferCount,
        .FlightDataCount = desc.FlightDataCount};
    _gpuSystem = make_unique<GpuSystem>(this, gpuSysDesc);
    _renderSystem = make_unique<RenderSystem>(this);
    _assetManager = make_unique<AssetManager>();
    _world = make_unique<World>(this);

    // ════════════════════════════════════════════════════════════════
    //  phase 2:按 ServiceTraits 装配交叉引用。此刻全部实例已在,
    //  WindowManager <-> GpuSystem 的环天然可解;AssetManager 拿到 GpuSystem 作回收器。
    // ════════════════════════════════════════════════════════════════
    ServiceRegistry registry;
    registry.Add(_windowManager.get());
    registry.Add(_gpuSystem.get());
    registry.Add(_renderSystem.get());
    registry.Add(_assetManager.get());
    registry.Add(_world.get());
    registry.Wire();

    // ════════════════════════════════════════════════════════════════
    //  phase 3:初始化数据。先跑各服务可选的 OnInitialize 钩子,
    //  再创建主窗口 + swapchain(依赖 WindowManager 已装配 GpuSystem)。
    // ════════════════════════════════════════════════════════════════
    registry.Initialize();

#ifdef RADRAY_PLATFORM_WINDOWS
    Win32WindowCreateDescriptor wndDesc{};
    wndDesc.Title = desc.WindowTitle;
    wndDesc.Width = desc.WindowWidth;
    wndDesc.Height = desc.WindowHeight;
    wndDesc.Resizable = true;
    wndDesc.StartVisible = true;
    AppWindow* mainWindow = _windowManager->CreateWindow(wndDesc, true);
#else
    RADRAY_ABORT("unsupported platform");
#endif
    render::SwapChainDescriptor swapchainDesc{};
    swapchainDesc.Width = static_cast<uint32_t>(desc.WindowWidth);
    swapchainDesc.Height = static_cast<uint32_t>(desc.WindowHeight);
    swapchainDesc.Format = desc.BackBufferFormat;
    swapchainDesc.PresentMode = desc.PresentMode;
    mainWindow->AttachSwapChain(swapchainDesc);

    InitializeSubsystems();
}

int Application::Run(const ApplicationRuntimeDescriptor& desc) {
    InitializeRuntime(desc);
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
