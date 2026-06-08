#include <radray/runtime/application.h>

#include <chrono>

#include <thread>
#include <atomic>
#include <semaphore>

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

bool CompleteFlight(AppRenderSystem* renderSystem, uint32_t flightIndex) {
    auto& flight = renderSystem->_flight[flightIndex];
    if (!flight.Signal.IsValid() || flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
    }

    renderSystem->_lastFrameLatency = std::chrono::steady_clock::now() - flight.FrameStartTime;
    flight.Signal = AppRenderSystem::FenceSignal::Invalid();
    renderSystem->_app->NotifyRenderComplete(AppRenderCompleteContext{.FlightIndex = flightIndex});
    flight.WaitForDestroy.clear();
    return true;
}

}  // namespace

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
            CompleteFlight(renderSystem, flightIndex);
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
            _app->_windowSystem->_eventPump->DispatchEvents();

            _writableSlotsSemaphore.acquire();

            auto* renderSystem = _app->_renderSystem.get();
            const uint32_t flightIndex = static_cast<uint32_t>(renderSystem->_nowFrameIndex % renderSystem->_flightDataCount);
            auto& flight = renderSystem->_flight[flightIndex];
            flight.WaitForDestroy.clear();
            flight.FrameStartTime = std::chrono::steady_clock::now();

            const auto now = std::chrono::steady_clock::now();
            const std::chrono::duration<float> deltaTime = now - _lastFrameTime;
            _lastFrameTime = now;

            _runnerFrameDatas[flightIndex].DeltaTime = deltaTime;
            auto result = _app->Update(AppUpdateContext{
                .FlightIndex = flightIndex,
                .DeltaTime = deltaTime,
                .LastFrameLatency = renderSystem->_lastFrameLatency});
            _reqExit = result.ShouldExit;
            if (_reqExit) {
                break;
            }

            renderSystem->_nowFrameIndex++;
            _readySlotsSemaphore.release();
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
            auto* renderSystem = _app->_renderSystem.get();
            while (_retireFrameIndex < _renderFrameIndex) {
                const uint64_t inFlightFrameCount = _renderFrameIndex - _retireFrameIndex;
                const uint32_t flightIndex = static_cast<uint32_t>(_retireFrameIndex % renderSystem->_flightDataCount);
                auto& flight = renderSystem->_flight[flightIndex];
                if (flight.Signal.IsValid()) {
                    if (flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
                        if (inFlightFrameCount < renderSystem->_flightDataCount) {
                            break;
                        }
                        flight.Signal.Fence->Wait(flight.Signal.Value);
                    }
                    CompleteFlight(renderSystem, flightIndex);
                }
                _retireFrameIndex++;
                _writableSlotsSemaphore.release();
            }

            _readySlotsSemaphore.acquire();

            if (_reqExit) {
                while (_retireFrameIndex < _renderFrameIndex) {
                    const uint32_t flightIndex = static_cast<uint32_t>(_retireFrameIndex % renderSystem->_flightDataCount);
                    auto& flight = renderSystem->_flight[flightIndex];
                    if (flight.Signal.IsValid()) {
                        flight.Signal.Fence->Wait(flight.Signal.Value);
                        CompleteFlight(renderSystem, flightIndex);
                    }
                    _retireFrameIndex++;
                    _writableSlotsSemaphore.release();
                }
                break;
            }

            uint32_t flightIndex = static_cast<uint32_t>(_renderFrameIndex % renderSystem->_flightDataCount);
            auto& runnerFrameData = _runnerFrameDatas[flightIndex];
            _app->Render(AppRenderContext{
                .FlightIndex = flightIndex,
                .DeltaTime = runnerFrameData.DeltaTime,
                .LastFrameLatency = renderSystem->_lastFrameLatency,
                .IsInModalLoop = false});

            _renderFrameIndex++;
        }
    }

    void OnModalLoopTick(NativeWindow* modalWindow) {
    }

    struct FrameData {
        std::chrono::duration<float> DeltaTime{};
    };

    Application* _app;
    sigslot::scoped_connection _modalLoopTickConnection;
    std::counting_semaphore<> _writableSlotsSemaphore;
    std::counting_semaphore<> _readySlotsSemaphore;
    // 共享数据
    vector<FrameData> _runnerFrameDatas;
    std::atomic_bool _reqExit{false};
    // 主线程独占
    std::chrono::steady_clock::time_point _lastFrameTime{std::chrono::steady_clock::now()};
    // 渲染线程独占
    uint64_t _renderFrameIndex{0};
    uint64_t _retireFrameIndex{0};
    std::thread _renderThread;
};

AppWindow::AppWindow(
    AppWindowSystem* system,
    uint64_t id,
    unique_ptr<NativeWindow> window,
    NativeEventPump* pump,
    bool isMain) noexcept
    : _system(system),
      _id(id),
      _window(std::move(window)),
      _pump(pump),
      _isMain(isMain) {
    _pump->Register(_window.get());
}

AppWindow::~AppWindow() noexcept {
    DetachSwapChain();
    _pump->Unregister(_window.get());
}

render::SwapChain* AppWindow::AttachSwapChain(const render::SwapChainDescriptor& desc) noexcept {
    auto* renderSystem = _system->_renderSystem;
    render::SwapChainDescriptor swapChainDesc = desc;
    swapChainDesc.PresentQueue = renderSystem->_mainQueue;
    swapChainDesc.NativeHandler = _window->GetNativeHandler();
    swapChainDesc.BackBufferCount = renderSystem->_backBufferCount;
    DetachSwapChain();
    _swapchain = renderSystem->_device->CreateSwapChain(swapChainDesc).Unwrap();
    const uint32_t backBufferCount = _swapchain->GetBackBufferCount();
    _backBufferViews.resize(backBufferCount);
    return _swapchain.Get();
}

unique_ptr<render::SwapChain> AppWindow::ReleaseSwapChain() noexcept {
    _backBufferViews.clear();
    return _swapchain.Release();
}

void AppWindow::DetachSwapChain() noexcept {
    if (_swapchain && _system->_renderSystem != nullptr) {
        auto* renderSystem = _system->_renderSystem;
        renderSystem->WaitAndCleanupCompletedFlights();
    }
    _backBufferViews.clear();
    _swapchain = nullptr;
}

render::TextureView* AppWindow::GetOrCreateBackBufferView(const render::SwapChainFrame& frame, uint32_t ownerFlightIndex) noexcept {
    const uint32_t index = frame.GetBackBufferIndex();
    render::Texture* backBuffer = frame.GetBackBuffer();
    auto* renderSystem = _system->_renderSystem;
    BackBufferView& backBufferView = _backBufferViews[index];
    render::TextureView* view = backBufferView.View.get();
    if (view != nullptr && backBufferView.BackBuffer == backBuffer) {
        backBufferView.FlightIndex = ownerFlightIndex;
        return view;
    }
    render::TextureDescriptor texDesc = backBuffer->GetDesc();
    render::TextureViewDescriptor viewDesc{
        .Target = backBuffer,
        .Dim = texDesc.Dim,
        .Format = texDesc.Format,
        .Range = render::SubresourceRange{
            .BaseArrayLayer = 0,
            .ArrayLayerCount = 1,
            .BaseMipLevel = 0,
            .MipLevelCount = 1},
        .Usage = render::TextureViewUsage::RenderTarget};
    auto viewIns = renderSystem->_device->CreateTextureView(viewDesc).Unwrap();
    auto viewPtr = viewIns.get();
    backBufferView.View = std::move(viewIns);
    backBufferView.BackBuffer = backBuffer;
    backBufferView.FlightIndex = ownerFlightIndex;
    return viewPtr;
}

AppWindowSystem::AppWindowSystem(Application* app, const AppWindowSystemDescriptor& desc)
    : _app(app) {
    NativeWindow::GlobalInit();
    _eventPump = NativeEventPump::Create(desc.Type).Unwrap();
}

AppWindowSystem::~AppWindowSystem() noexcept {
    _windows.clear();
    _eventPump.reset();
    NativeWindow::GlobalShutdown();
}

AppWindow* AppWindowSystem::CreateWindow(const NativeWindowCreateDescriptor& desc, bool isMain) {
    auto window = NativeWindow::Create(desc).Unwrap();
    auto id = _windowIdCounter++;
    auto& newWindow = _windows.emplace_back(make_unique<AppWindow>(this, id, std::move(window), _eventPump.get(), isMain));
    return newWindow.get();
}

void AppWindowSystem::DestroyWindow(AppWindow* window) noexcept {
    auto iter = std::ranges::find_if(_windows, [window](const unique_ptr<AppWindow>& item) {
        return item.get() == window;
    });
    if (iter != _windows.end()) {
        _windows.erase(iter);
    }
}

void AppWindowSystem::CheckRecreateSwapChains() noexcept {
    auto needsRecreate = [this](AppWindow* window) noexcept {
        if (!window->_swapchain || window->_window->IsMinimized()) {
            return false;
        }

        const Eigen::Vector2i windowSize = window->_window->GetSize();
        if (windowSize.x() <= 0 || windowSize.y() <= 0) {
            return false;
        }

        const render::SwapChainDescriptor desc = window->_swapchain->GetDesc();
        const uint32_t width = static_cast<uint32_t>(windowSize.x());
        const uint32_t height = static_cast<uint32_t>(windowSize.y());
        if (_presentModeOverride.has_value() && desc.PresentMode != _presentModeOverride.value()) {
            return true;
        }
        return desc.Width != width || desc.Height != height;
    };

    bool hasSwapChainToRecreate = false;
    for (const auto& window : _windows) {
        if (needsRecreate(window.get())) {
            hasSwapChainToRecreate = true;
            break;
        }
    }

    if (!hasSwapChainToRecreate) {
        return;
    }

    _renderSystem->_mainQueue->Wait();

    for (const auto& window : _windows) {
        if (!needsRecreate(window.get())) {
            continue;
        }

        const Eigen::Vector2i windowSize = window->_window->GetSize();
        render::SwapChain* swapChain = window->_swapchain.Get();
        render::SwapChainDescriptor desc = swapChain->GetDesc();
        const uint32_t width = static_cast<uint32_t>(windowSize.x());
        const uint32_t height = static_cast<uint32_t>(windowSize.y());

        window->_backBufferViews.clear();

        const render::PresentMode presentMode = _presentModeOverride.value_or(desc.PresentMode);
        const bool recreated = swapChain->Recreate(width, height, desc.Format, presentMode);
        const uint32_t backBufferCount = swapChain->GetBackBufferCount();
        window->_backBufferViews.resize(backBufferCount);

        if (!recreated) {
            RADRAY_ERR_LOG("failed to recreate window swapchain: {}x{} -> {}x{}", desc.Width, desc.Height, width, height);
            continue;
        }

        AppSwapChainRecreateContext ctx{
            .Window = window.get(),
            .SwapChain = swapChain};
        _app->OnSwapChainRecreate(ctx);
    }
}

void AppWindowSystem::SetPresentMode(render::PresentMode presentMode) noexcept {
    _presentModeOverride = presentMode;
}

AppRenderSystem::AppRenderSystem(Application* app, const AppRenderSystemDescriptor& desc)
    : _app(app),
      _device(desc.Device),
      _mainQueue(desc.Device->GetCommandQueue(render::QueueType::Direct, desc.MainQueueIndex).Unwrap()),
      _backBufferCount(desc.BackBufferCount),
      _flightDataCount(desc.FlightDataCount) {
    _mainQueueTrack.Queue = _mainQueue;
    _mainQueueTrack.Fence = _device->CreateFence().Unwrap();
    _mainQueueTrack.Fence->SetDebugName("AppMainQueue");
    _flight.resize(_flightDataCount);
}

AppRenderSystem::~AppRenderSystem() noexcept = default;

void AppRenderSystem::WaitAndCleanupCompletedFlights() {
    _mainQueue->Wait();

    for (uint32_t flightIndex = 0; flightIndex < _flight.size(); ++flightIndex) {
        CompleteFlight(this, flightIndex);
    }
}

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
