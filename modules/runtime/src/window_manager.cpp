#include <radray/runtime/window_manager.h>

#include <algorithm>
#include <limits>
#include <ranges>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

bool WindowManager::NeedsRecreateSwapChain(AppWindow* window) const noexcept {
    return window != nullptr && window->NeedsSwapChainRecreate(_desiredPresentMode);
}

bool WindowManager::HasSwapChainToRecreate() const noexcept {
    for (const auto& window : _windows) {
        if (NeedsRecreateSwapChain(window.get())) {
            return true;
        }
    }
    return false;
}

AppWindow::AppWindow(
    WindowManager* manager,
    unique_ptr<NativeWindow> window,
    NativeEventPump* pump,
    bool isMain) noexcept
    : _manager(manager),
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
    auto* gpuSystem = _manager->GetGpuSystem();
    render::SwapChainDescriptor swapChainDesc = desc;
    swapChainDesc.PresentQueue = gpuSystem->GetMainQueue();
    swapChainDesc.NativeHandler = _window->GetNativeHandler();
    swapChainDesc.BackBufferCount = gpuSystem->GetBackBufferCount();
    DetachSwapChain();
    _swapchain = gpuSystem->GetDevice()->CreateSwapChain(swapChainDesc).Unwrap();
    const uint32_t backBufferCount = _swapchain->GetBackBufferCount();
    _backBufferViews.resize(backBufferCount);
    _requestRecreateSwapChain.store(false, std::memory_order_release);
    return _swapchain.Get();
}

unique_ptr<render::SwapChain> AppWindow::ReleaseSwapChain() noexcept {
    _backBufferViews.clear();
    _requestRecreateSwapChain.store(false, std::memory_order_release);
    return _swapchain.Release();
}

void AppWindow::DetachSwapChain() noexcept {
    if (_swapchain && _manager->GetGpuSystem() != nullptr) {
        auto* gpuSystem = _manager->GetGpuSystem();
        gpuSystem->WaitAndCleanupCompletedFlights();
    }
    _backBufferViews.clear();
    _swapchain = nullptr;
    _requestRecreateSwapChain.store(false, std::memory_order_release);
}

render::SwapChainAcquireResult AppWindow::AcquireNextSwapChainFrame(const AppRenderContext& ctx) noexcept {
    if (!_swapchain) {
        return render::SwapChainAcquireResult{};
    }
    uint64_t timeoutMs = ctx.IsInModalLoop ? 0 : std::numeric_limits<uint64_t>::max();
    render::SwapChainAcquireResult result = _swapchain->AcquireNext(timeoutMs);
    if (result.Status == render::SwapChainStatus::RequireRecreate) {
        _requestRecreateSwapChain.store(true, std::memory_order_release);
    }
    return result;
}

render::SwapChainPresentResult AppWindow::PresentSwapChainFrame(render::SwapChainFrame&& frame) noexcept {
    if (!_swapchain) {
        return render::SwapChainPresentResult{};
    }

    render::SwapChainPresentResult result = _swapchain->Present(std::move(frame));
    if (result.Status == render::SwapChainStatus::RequireRecreate) {
        _requestRecreateSwapChain.store(true, std::memory_order_release);
    }
    return result;
}

NativeWindow* AppWindow::GetNativeWindow() const noexcept {
    return _window.get();
}

render::SwapChain* AppWindow::GetSwapChain() const noexcept {
    if (!_swapchain) {
        return nullptr;
    }
    return _swapchain.Get();
}

bool AppWindow::IsMainWindow() const noexcept {
    return _isMain;
}

render::TextureView* AppWindow::GetOrCreateBackBufferView(const render::SwapChainFrame& frame) noexcept {
    const uint32_t index = frame.GetBackBufferIndex();
    render::Texture* backBuffer = frame.GetBackBuffer();
    auto* gpuSystem = _manager->GetGpuSystem();
    BackBufferView& backBufferView = _backBufferViews[index];
    render::TextureView* view = backBufferView.View.get();
    if (view != nullptr && backBufferView.BackBuffer == backBuffer) {
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
    auto viewIns = gpuSystem->GetDevice()->CreateTextureView(viewDesc).Unwrap();
    auto viewPtr = viewIns.get();
    backBufferView.View = std::move(viewIns);
    backBufferView.BackBuffer = backBuffer;
    // 新 backbuffer（首次 / 重建）：状态必为 Undefined，下一次起始 barrier 从 Undefined 翻起。
    backBufferView.State = render::TextureState::Undefined;
    return viewPtr;
}

render::TextureStates AppWindow::GetBackBufferState(uint32_t backBufferIndex) const noexcept {
    if (backBufferIndex >= _backBufferViews.size()) {
        return render::TextureState::Undefined;
    }
    return _backBufferViews[backBufferIndex].State;
}

void AppWindow::SetBackBufferState(uint32_t backBufferIndex, render::TextureStates state) noexcept {
    if (backBufferIndex >= _backBufferViews.size()) {
        return;
    }
    _backBufferViews[backBufferIndex].State = state;
}

WindowManager::WindowManager(Application* app, const WindowManagerDescriptor& desc)
    : _app(app) {
    NativeWindow::GlobalInit();
    _eventPump = NativeEventPump::Create(desc.Type).Unwrap();
}

WindowManager::~WindowManager() noexcept {
    _windows.clear();
    _eventPump.reset();
    NativeWindow::GlobalShutdown();
}

AppWindow* WindowManager::CreateWindow(const NativeWindowCreateDescriptor& desc, bool isMain) {
    auto window = NativeWindow::Create(desc).Unwrap();
    auto& newWindow = _windows.emplace_back(make_unique<AppWindow>(this, std::move(window), _eventPump.get(), isMain));
    if (isMain) {
        _mainWindow = newWindow.get();
    }
    return newWindow.get();
}

bool AppWindow::IsMinimized() const noexcept {
    return _window == nullptr || _window->IsMinimized();
}

Eigen::Vector2i AppWindow::GetSize() const noexcept {
    return _window != nullptr ? _window->GetSize() : Eigen::Vector2i{0, 0};
}

bool AppWindow::NeedsSwapChainRecreate(std::optional<render::PresentMode> desiredPresentMode) const noexcept {
    if (!_swapchain || IsMinimized()) {
        return false;
    }

    const Eigen::Vector2i windowSize = GetSize();
    if (windowSize.x() <= 0 || windowSize.y() <= 0) {
        return false;
    }

    const render::SwapChainDescriptor desc = _swapchain->GetDesc();
    const uint32_t width = static_cast<uint32_t>(windowSize.x());
    const uint32_t height = static_cast<uint32_t>(windowSize.y());
    if (desiredPresentMode.has_value() && desc.PresentMode != desiredPresentMode.value()) {
        return true;
    }
    return desc.Width != width || desc.Height != height || _requestRecreateSwapChain.load(std::memory_order_acquire);
}

void AppWindow::ResetSwapChainRecreateRequest() noexcept {
    _requestRecreateSwapChain.store(false, std::memory_order_release);
}

bool AppWindow::RecreateSwapChain(uint32_t width, uint32_t height, render::PresentMode presentMode) noexcept {
    if (!_swapchain) {
        return false;
    }
    _backBufferViews.clear();
    const render::SwapChainDescriptor desc = _swapchain->GetDesc();
    const bool recreated = _swapchain->Recreate(width, height, desc.Format, presentMode);
    const uint32_t backBufferCount = _swapchain->GetBackBufferCount();
    _backBufferViews.resize(backBufferCount);

    if (!recreated) {
        RADRAY_ERR_LOG("failed to recreate window swapchain: {}x{} -> {}x{}", desc.Width, desc.Height, width, height);
        return false;
    }
    ResetSwapChainRecreateRequest();
    return true;
}

void WindowManager::DestroyWindow(AppWindow* window) noexcept {
    auto iter = std::ranges::find_if(_windows, [window](const unique_ptr<AppWindow>& item) {
        return item.get() == window;
    });
    if (iter != _windows.end()) {
        if (_mainWindow == iter->get()) {
            _mainWindow = nullptr;
        }
        _windows.erase(iter);
    }
}

size_t WindowManager::GetWindowCount() const noexcept {
    return _windows.size();
}

AppWindow* WindowManager::GetWindow(size_t index) noexcept {
    if (index >= _windows.size()) {
        return nullptr;
    }
    return _windows[index].get();
}

const AppWindow* WindowManager::GetWindow(size_t index) const noexcept {
    if (index >= _windows.size()) {
        return nullptr;
    }
    return _windows[index].get();
}

AppWindow* WindowManager::GetMainWindow() noexcept {
    return _mainWindow;
}

const AppWindow* WindowManager::GetMainWindow() const noexcept {
    return _mainWindow;
}

bool WindowManager::ShouldExit() const noexcept {
    return _mainWindow != nullptr && _mainWindow->GetNativeWindow()->ShouldClose();
}

render::TextureFormat WindowManager::GetMainBackBufferFormat(render::TextureFormat fallback) const noexcept {
    if (_mainWindow == nullptr) {
        return fallback;
    }
    render::SwapChain* swapChain = _mainWindow->GetSwapChain();
    if (swapChain == nullptr) {
        return fallback;
    }
    return swapChain->GetDesc().Format;
}

render::PresentMode WindowManager::GetMainPresentMode(render::PresentMode fallback) const noexcept {
    if (_mainWindow == nullptr) {
        return fallback;
    }
    render::SwapChain* swapChain = _mainWindow->GetSwapChain();
    if (swapChain == nullptr) {
        return fallback;
    }
    return swapChain->GetDesc().PresentMode;
}

void WindowManager::CheckRecreateSwapChains() noexcept {
    if (!HasSwapChainToRecreate()) {
        return;
    }

    _gpuSystem->WaitAndCleanupCompletedFlights();

    for (const auto& window : _windows) {
        if (!NeedsRecreateSwapChain(window.get())) {
            continue;
        }

        const Eigen::Vector2i windowSize = window->GetSize();
        render::SwapChain* swapChain = window->GetSwapChain();
        render::SwapChainDescriptor desc = swapChain->GetDesc();
        const uint32_t width = static_cast<uint32_t>(windowSize.x());
        const uint32_t height = static_cast<uint32_t>(windowSize.y());

        const render::PresentMode presentMode = _desiredPresentMode.value_or(desc.PresentMode);
        if (!window->RecreateSwapChain(width, height, presentMode)) {
            continue;
        }

        AppSwapChainRecreateContext ctx{
            .Window = window.get(),
            .SwapChain = window->GetSwapChain()};
        _app->OnSwapChainRecreate(ctx);
    }
}

void WindowManager::SetPresentMode(render::PresentMode presentMode) noexcept {
    _desiredPresentMode = presentMode;
}

void WindowManager::DispatchEvents() noexcept {
    if (_eventPump != nullptr) {
        _eventPump->DispatchEvents();
    }
}

sigslot::signal<NativeWindow*>& WindowManager::EventModalLoopTick() noexcept {
    return _eventPump->EventModalLoopTick();
}

void WindowManager::DetachAllSwapChains() noexcept {
    for (const unique_ptr<AppWindow>& window : _windows) {
        window->DetachSwapChain();
    }
}

NativeWindow* WindowManager::FindMainNativeWindow(NativeWindowType type) const noexcept {
    if (_mainWindow == nullptr || !_mainWindow->IsMainWindow()) {
        return nullptr;
    }
    NativeWindow* window = _mainWindow->GetNativeWindow();
    if (window == nullptr || window->GetType() != type) {
        return nullptr;
    }
    return window;
}

NativeWindow* WindowManager::FindFirstNativeWindow(NativeWindowType type) const noexcept {
    for (const unique_ptr<AppWindow>& window : _windows) {
        NativeWindow* nativeWindow = window->GetNativeWindow();
        if (nativeWindow != nullptr && nativeWindow->GetType() == type) {
            return nativeWindow;
        }
    }
    return nullptr;
}

}  // namespace radray
