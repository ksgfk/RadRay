#include <radray/runtime/application.h>

#include <chrono>

#include <atomic>
#include <mutex>
#include <optional>
#include <semaphore>
#include <thread>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/window_system.h>

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

Application::~Application() noexcept = default;

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
        const render::RenderBackend backend = app->_renderSystem->_device->GetBackend();
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

        if (_app->_windowSystem == nullptr) {
            return nullptr;
        }
        for (const auto& window : _app->_windowSystem->_windows) {
            if (window->_isMain && window->_window != nullptr && window->_window->GetType() == NativeWindowType::Win32HWND) {
                return static_cast<HWND>(window->_window->GetNativeHandler());
            }
        }
        for (const auto& window : _app->_windowSystem->_windows) {
            if (window->_window != nullptr && window->_window->GetType() == NativeWindowType::Win32HWND) {
                return static_cast<HWND>(window->_window->GetNativeHandler());
            }
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
          _modalLoopTickConnection(_app->_windowSystem->_eventPump->EventModalLoopTick().connect(&SingleThreadRunner::OnModalLoopTick, this)) {}

    int Run() {
        while (true) {
#if defined(RADRAY_APP_IMPL_ENABLE_VBLANK_TICK)
            StopWin32ModalVBlank();
#endif
            CheckFrameComplete(false);
            _hasModalLoopActivityDuringDispatch = false;
            _isDispatchingEvents = true;
            _app->_windowSystem->_eventPump->DispatchEvents();
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
        auto* renderSystem = _app->_renderSystem.get();
        const uint32_t flightIndex = static_cast<uint32_t>(renderSystem->_nowFrameIndex % renderSystem->_flightDataCount);
        auto& flight = renderSystem->_flight[flightIndex];
        if (flight.Signal.IsValid()) {
            if (isInModalLoop) {
                if (flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
                    return false;
                }
            } else {
                flight.Signal.Fence->Wait(flight.Signal.Value);
            }
            renderSystem->CompleteFlight(flightIndex);
        }
        return true;
    }

    void TickFrame(bool isInModalLoop) {
        if (isInModalLoop) {
            MarkModalLoopActivityDuringDispatch();
        }
        auto* renderSystem = _app->_renderSystem.get();
        const uint32_t flightIndex = static_cast<uint32_t>(renderSystem->_nowFrameIndex % renderSystem->_flightDataCount);
        auto& flight = renderSystem->_flight[flightIndex];
        flight.WaitForDestroy.clear();
        flight.FrameStartTime = std::chrono::steady_clock::now();

        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<float> deltaTime = now - _lastFrameTime;
        _lastFrameTime = now;

        _app->_windowSystem->CheckRecreateSwapChains();

        auto result = _app->Update(AppUpdateContext{
            .FlightIndex = flightIndex,
            .DeltaTime = deltaTime,
            .LastFrameLatency = renderSystem->_lastFrameLatency});
        _reqExit = result.ShouldExit;
        if (_reqExit) {
            return;
        }

        _app->_windowSystem->CheckRecreateSwapChains();

        _app->Render(AppRenderContext{
            .FlightIndex = flightIndex,
            .DeltaTime = deltaTime,
            .LastFrameLatency = renderSystem->_lastFrameLatency,
            .IsInModalLoop = isInModalLoop});
        renderSystem->_nowFrameIndex++;
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
          _modalLoopTickConnection(_app->_windowSystem->_eventPump->EventModalLoopTick().connect(&ThreadedRunner::OnModalLoopTick, this)),
          _writableSlotsSemaphore(_app->_renderSystem->_flightDataCount),
          _readySlotsSemaphore(0),
          _runnerFrameDatas(_app->_renderSystem->_flightDataCount),
          _renderThread(&ThreadedRunner::RenderThread, this) {}

    int Run() {
        while (true) {
            _hasModalLoopActivityDuringDispatch = false;
            _app->_windowSystem->_eventPump->DispatchEvents();

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

            auto* renderSystem = _app->_renderSystem.get();
            _readySlotsSemaphore.acquire();

            if (_reqExit) {
                RetireRenderedFrames(true, false);
                break;
            }

            uint32_t flightIndex = static_cast<uint32_t>(_renderFrameIndex % renderSystem->_flightDataCount);
            auto& runnerFrameData = _runnerFrameDatas[flightIndex];
            if (!runnerFrameData.IsInModalLoop && _renderFrameIndex < _discardNonModalFramesBefore.load(std::memory_order_acquire)) {
                _app->NotifyRenderComplete(AppRenderCompleteContext{.FlightIndex = flightIndex});
                _renderFrameIndex++;
                NotifyRenderFrameComplete(_renderFrameIndex);
                continue;
            }

            _app->Render(AppRenderContext{
                .FlightIndex = flightIndex,
                .DeltaTime = runnerFrameData.DeltaTime,
                .LastFrameLatency = renderSystem->_lastFrameLatency,
                .IsInModalLoop = runnerFrameData.IsInModalLoop});

            _renderFrameIndex++;
            NotifyRenderFrameComplete(_renderFrameIndex);
        }
    }

    void OnModalLoopTick(NativeWindow*) {
        _hasModalLoopActivityDuringDispatch = true;
        if (_reqExit) {
            return;
        }

        const uint64_t frameIndex = _app->_renderSystem->_nowFrameIndex;
        _discardNonModalFramesBefore.store(frameIndex, std::memory_order_release);
        WaitRenderFrameComplete(frameIndex);
        RetireRenderedFrames(false, false);
        if (auto renderedFrameCount = TickFrame(true, false)) {
            WaitRenderFrameComplete(renderedFrameCount.value());
        }
    }

    void CheckRecreateSwapChains() {
        auto* windowSystem = _app->_windowSystem.get();
        if (!windowSystem->HasSwapChainToRecreate()) {
            return;
        }
        WaitRenderThreadIdle();
        windowSystem->CheckRecreateSwapChains();
    }

    std::optional<uint64_t> TickFrame(bool isInModalLoop, bool waitForWritableSlot) {
        auto* renderSystem = _app->_renderSystem.get();
        if (waitForWritableSlot) {
            WaitRenderFrameComplete(renderSystem->_nowFrameIndex);
            RetireRenderedFrames(false, false);
            CheckRecreateSwapChains();
            _writableSlotsSemaphore.acquire();
        } else if (!_writableSlotsSemaphore.try_acquire()) {
            return std::nullopt;
        } else {
            CheckRecreateSwapChains();
        }

        const uint64_t frameIndex = renderSystem->_nowFrameIndex;
        const uint32_t flightIndex = static_cast<uint32_t>(frameIndex % renderSystem->_flightDataCount);
        auto& flight = renderSystem->_flight[flightIndex];
        flight.WaitForDestroy.clear();
        flight.FrameStartTime = std::chrono::steady_clock::now();

        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<float> deltaTime = now - _lastFrameTime;
        _lastFrameTime = now;

        _runnerFrameDatas[flightIndex].DeltaTime = deltaTime;
        _runnerFrameDatas[flightIndex].IsInModalLoop = isInModalLoop;
        auto result = _app->Update(AppUpdateContext{
            .FlightIndex = flightIndex,
            .DeltaTime = deltaTime,
            .LastFrameLatency = renderSystem->_lastFrameLatency});
        _reqExit = result.ShouldExit;
        if (_reqExit) {
            return std::nullopt;
        }

        CheckRecreateSwapChains();

        renderSystem->_nowFrameIndex++;
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
        WaitRenderFrameComplete(_app->_renderSystem->_nowFrameIndex);
        RetireRenderedFrames(true, false);
    }

    void RetireRenderedFrames(bool waitForPendingFrames, bool waitWhenFrameSlotsFull) {
        std::lock_guard lock(_retireMutex);
        auto* renderSystem = _app->_renderSystem.get();
        const uint64_t renderedFrameCount = _renderedFrameCount.load(std::memory_order_acquire);
        while (_retireFrameIndex < renderedFrameCount) {
            const uint64_t inFlightFrameCount = renderedFrameCount - _retireFrameIndex;
            const uint32_t flightIndex = static_cast<uint32_t>(_retireFrameIndex % renderSystem->_flightDataCount);
            auto& flight = renderSystem->_flight[flightIndex];
            if (flight.Signal.IsValid()) {
                if (waitForPendingFrames) {
                    flight.Signal.Fence->Wait(flight.Signal.Value);
                } else if (flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
                    if (!waitWhenFrameSlotsFull || inFlightFrameCount < renderSystem->_flightDataCount) {
                        break;
                    }
                    flight.Signal.Fence->Wait(flight.Signal.Value);
                }
                renderSystem->CompleteFlight(flightIndex);
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

AppWindowSystem* Application::InitWindowSystem(const AppWindowSystemDescriptor& desc) {
    if (_windowSystem) {
        RADRAY_WARN_LOG("window system already initialized");
        return _windowSystem.get();
    }
    _windowSystem = make_unique<AppWindowSystem>(this, desc);
    if (_renderSystem) {
        _windowSystem->_renderSystem = _renderSystem.get();
        _renderSystem->_windowSystem = _windowSystem.get();
    }
    return _windowSystem.get();
}

void Application::ShutdownWindowSystem() {
    if (_renderSystem) {
        _renderSystem->_windowSystem = nullptr;
    }
    _windowSystem.reset();
}

AppRenderSystem* Application::InitRenderSystem(const AppRenderSystemDescriptor& desc) {
    if (_renderSystem) {
        RADRAY_WARN_LOG("render system already initialized");
        return _renderSystem.get();
    }
    _renderSystem = make_unique<AppRenderSystem>(this, desc);
    if (_windowSystem) {
        _renderSystem->_windowSystem = _windowSystem.get();
        _windowSystem->_renderSystem = _renderSystem.get();
    }
    return _renderSystem.get();
}

void Application::ShutdownRenderSystem() {
    if (_windowSystem) {
        for (const auto& window : _windowSystem->_windows) {
            window->DetachSwapChain();
        }
        _windowSystem->_renderSystem = nullptr;
    }
    _renderSystem.reset();
}

void Application::NotifyRenderComplete(const AppRenderCompleteContext& ctx) {
    OnRenderComplete(ctx);

    if (_windowSystem == nullptr || _renderSystem == nullptr) {
        return;
    }

    for (const unique_ptr<AppWindow>& window : _windowSystem->_windows) {
        for (AppWindow::BackBufferView& backBufferView : window->_backBufferViews) {
            if (backBufferView.View == nullptr || backBufferView.FlightIndex != ctx.FlightIndex) {
                continue;
            }
            backBufferView.View.reset();
            backBufferView.BackBuffer = nullptr;
        }
    }
}

int Application::StartLoop() {
    if (_multithreaded) {
        return ThreadedRunner{this}.Run();
    } else {
        return SingleThreadRunner{this}.Run();
    }
}

}  // namespace radray
