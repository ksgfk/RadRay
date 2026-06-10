#include <radray/runtime/window_system.h>

#include <algorithm>
#include <limits>
#include <ranges>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/application.h>
#include <radray/runtime/render_system.h>

namespace radray {

bool AppWindowSystem::NeedsRecreateSwapChain(AppWindow* window) const noexcept {
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
    if (_desiredPresentMode.has_value() && desc.PresentMode != _desiredPresentMode.value()) {
        return true;
    }
    return desc.Width != width || desc.Height != height || window->_requestRecreateSwapChain.load(std::memory_order_acquire);
}

bool AppWindowSystem::HasSwapChainToRecreate() const noexcept {
    for (const auto& window : _windows) {
        if (NeedsRecreateSwapChain(window.get())) {
            return true;
        }
    }
    return false;
}

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
    _requestRecreateSwapChain.store(false, std::memory_order_release);
    return _swapchain.Get();
}

unique_ptr<render::SwapChain> AppWindow::ReleaseSwapChain() noexcept {
    _backBufferViews.clear();
    _requestRecreateSwapChain.store(false, std::memory_order_release);
    return _swapchain.Release();
}

void AppWindow::DetachSwapChain() noexcept {
    if (_swapchain && _system->_renderSystem != nullptr) {
        auto* renderSystem = _system->_renderSystem;
        renderSystem->WaitAndCleanupCompletedFlights();
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
    if (!HasSwapChainToRecreate()) {
        return;
    }

    _renderSystem->WaitAndCleanupCompletedFlights();

    for (const auto& window : _windows) {
        if (!NeedsRecreateSwapChain(window.get())) {
            continue;
        }

        const Eigen::Vector2i windowSize = window->_window->GetSize();
        render::SwapChain* swapChain = window->_swapchain.Get();
        render::SwapChainDescriptor desc = swapChain->GetDesc();
        const uint32_t width = static_cast<uint32_t>(windowSize.x());
        const uint32_t height = static_cast<uint32_t>(windowSize.y());

        window->_backBufferViews.clear();

        const render::PresentMode presentMode = _desiredPresentMode.value_or(desc.PresentMode);
        const bool recreated = swapChain->Recreate(width, height, desc.Format, presentMode);
        const uint32_t backBufferCount = swapChain->GetBackBufferCount();
        window->_backBufferViews.resize(backBufferCount);

        if (!recreated) {
            RADRAY_ERR_LOG("failed to recreate window swapchain: {}x{} -> {}x{}", desc.Width, desc.Height, width, height);
            continue;
        }
        window->_requestRecreateSwapChain.store(false, std::memory_order_release);

        AppSwapChainRecreateContext ctx{
            .Window = window.get(),
            .SwapChain = swapChain};
        _app->OnSwapChainRecreate(ctx);
    }
}

void AppWindowSystem::SetPresentMode(render::PresentMode presentMode) noexcept {
    _desiredPresentMode = presentMode;
}

}  // namespace radray
