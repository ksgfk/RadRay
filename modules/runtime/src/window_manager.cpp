#include <radray/runtime/window_manager.h>

#include <algorithm>
#include <limits>
#include <utility>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/render/rhi.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

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
    bool isMain, uint64_t id) noexcept
    : _manager(manager),
      _id(id),
      _window(std::move(window)),
      _pump(pump),
      _isMain(isMain) {
    _beforeSurfaceChange = _window->EventBeforeSurfaceChange().connect([this]() {
        _manager->AssertMutationAllowed();
    });
}

AppWindow::~AppWindow() noexcept {
    DetachSwapChain();
    _pump->Unregister(_window.get());
    _window->Destroy();
}

WindowOperationStatus AppWindow::AttachSwapChain(const WindowSwapChainDescriptor& desc) noexcept {
    _manager->AssertMutationAllowed();
    DetachSwapChain();
    _swapChainDescriptor = desc;
    return UpdateSwapChain({});
}

unique_ptr<render::SwapChain> AppWindow::ReleaseSwapChain() noexcept {
    _manager->AssertMutationAllowed();
    ReleaseBackBufferViews();
    _swapChainDescriptor.reset();
    _swapChainUsable = false;
    _requestRecreateSwapChain.store(false, std::memory_order_release);
    return _swapchain.Release();
}

void AppWindow::DetachSwapChain() noexcept {
    auto released = ReleaseSwapChain();
}

render::SwapChainAcquireResult AppWindow::AcquireNextSwapChainFrame(const AppRenderContext& ctx) noexcept {
    if (!IsSwapChainPresentable()) {
        render::SwapChainAcquireResult result{};
        result.Status = render::SwapChainStatus::RetryLater;
        return result;
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
    if (!_swapchain || !_swapChainUsable) {
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
    RenderSystem* renderSystem = _manager->GetRenderSystem();
    render::RenderPassRegistry* registry = renderSystem != nullptr ? renderSystem->GetRenderPassRegistry() : nullptr;
    if (view != nullptr && registry != nullptr) {
        registry->RemoveFramebuffersUsing(view);
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

WindowManager::WindowManager(const WindowManagerDescriptor& desc) : _type(desc.Type) {
    NativeWindow::GlobalInit();
    _eventPump = NativeEventPump::Create(desc.Type).Unwrap();
}

WindowManager::~WindowManager() noexcept {
    CloseOperations();
    _applyingOperations = true;
    _windows.clear();
    _eventPump.reset();
    NativeWindow::GlobalShutdown();
}

WindowCreateResult WindowManager::CreateWindowImmediate(const WindowCreateDescriptor& desc, bool isMain) {
    AssertMutationAllowed();
    if (isMain && (_mainWindow != nullptr || _mainWindowClosed)) return {};
    Nullable<NativeWindow*> owner{nullptr};
    if (desc.OwnerWindow.Id != 0 || desc.OwnerWindow.Owner) {
        auto ownerWindow = ResolveWindow(desc.OwnerWindow);
        if (!ownerWindow) return {WindowOperationStatus::InvalidWindow, {}};
        owner = ownerWindow->GetNativeWindow();
    }
    NativeWindowCreateDescriptor nativeDesc;
    if (_type == NativeWindowType::Win32HWND)
        nativeDesc = Win32WindowCreateDescriptor{};
    else if (_type == NativeWindowType::CocoaNSWindow)
        nativeDesc = CocoaWindowCreateDescriptor{};
    else
        return {};
    std::visit([&](auto& native) {
        native.Title = desc.Title;
        native.Width = desc.Width;
        native.Height = desc.Height;
        native.X = desc.X;
        native.Y = desc.Y;
        native.Resizable = desc.Resizable;
        native.StartMaximized = desc.StartMaximized;
        native.Fullscreen = desc.Fullscreen;
        native.StartVisible = desc.StartVisible;
        native.OwnerWindow = owner;
        native.Decorated = desc.Decorated;
        native.ShowInTaskbar = desc.ShowInTaskbar;
        native.TopMost = desc.TopMost;
        native.ActivateOnShow = desc.ActivateOnShow;
        native.FocusOnClick = desc.FocusOnClick;
        native.InputPassthrough = desc.InputPassthrough;
    },
               nativeDesc);
    auto window = NativeWindow::Create(nativeDesc);
    if (!window || !_eventPump->Register(window.Get())) return {};
    if (_nextWindowId == UINT64_MAX) RADRAY_ABORT("Window identity exhausted");
    auto& result = _windows.emplace_back(make_unique<AppWindow>(this, window.Release(), _eventPump.get(), isMain, _nextWindowId++));
    result->_ownerWindow = desc.OwnerWindow;
    if (isMain) _mainWindow = result.get();
    return {WindowOperationStatus::Completed, result->GetHandle()};
}

bool WindowManager::InitializeMainWindow(const WindowCreateDescriptor& desc, std::optional<WindowSwapChainDescriptor> swapchain) {
    _applyingOperations = true;
    auto phase = MakeScopeGuard([this]() noexcept { _applyingOperations = false; });
    auto result = CreateWindowImmediate(desc, true);
    if (result.Status != WindowOperationStatus::Completed) return false;
    return !swapchain || ResolveWindow(result.Handle)->AttachSwapChain(*swapchain) == WindowOperationStatus::Completed;
}

bool AppWindow::IsMinimized() const noexcept {
    return _window == nullptr || _window->IsMinimized();
}

bool AppWindow::IsSwapChainPresentable() const noexcept {
    if (_window == nullptr || _swapchain == nullptr || !_swapChainUsable || IsMinimized() || !_window->IsVisible()) {
        return false;
    }
    const Eigen::Vector2i size = GetSize();
    return size.x() > 0 && size.y() > 0;
}

Eigen::Vector2i AppWindow::GetSize() const noexcept {
    return _window != nullptr ? _window->GetSize() : Eigen::Vector2i{0, 0};
}

bool AppWindow::NeedsSwapChainRecreate(std::optional<render::PresentMode> desiredPresentMode) const noexcept {
    if (!_swapChainDescriptor || IsMinimized() || !_window->IsVisible()) {
        return false;
    }

    const Eigen::Vector2i windowSize = GetSize();
    if (windowSize.x() <= 0 || windowSize.y() <= 0) {
        return false;
    }

    if (!_swapchain || !_swapChainUsable) return true;
    const render::SwapChainDescriptor desc = _swapchain->GetDesc();
    const uint32_t width = static_cast<uint32_t>(windowSize.x());
    const uint32_t height = static_cast<uint32_t>(windowSize.y());
    if (desiredPresentMode.has_value() && desc.PresentMode != desiredPresentMode.value()) {
        return true;
    }
    return desc.Width != width || desc.Height != height || _requestRecreateSwapChain.load(std::memory_order_acquire);
}

WindowOperationStatus AppWindow::UpdateSwapChain(std::optional<render::PresentMode> desiredMode) noexcept {
    _manager->AssertMutationAllowed();
    if (!_swapChainDescriptor) return WindowOperationStatus::Completed;
    if (!_window->IsValid()) return WindowOperationStatus::Failed;
    if (desiredMode) _swapChainDescriptor->PresentMode = *desiredMode;
    const auto size = GetSize();
    if (IsMinimized() || !_window->IsVisible() || size.x() <= 0 || size.y() <= 0) {
        _requestRecreateSwapChain.store(true, std::memory_order_release);
        return WindowOperationStatus::Deferred;
    }
    if (!NeedsSwapChainRecreate(_swapChainDescriptor->PresentMode)) return WindowOperationStatus::Completed;
    ReleaseBackBufferViews();
    auto* gpu = _manager->GetGpuSystem();
    if (gpu == nullptr) return WindowOperationStatus::Failed;
    _swapChainDescriptor->Width = static_cast<uint32_t>(size.x());
    _swapChainDescriptor->Height = static_cast<uint32_t>(size.y());
    const auto& desc = *_swapChainDescriptor;
    if (!_swapchain) {
        _swapchain = gpu->GetDevice()->CreateSwapChain(render::SwapChainDescriptor{
            .PresentQueue = gpu->GetMainQueue(), .NativeHandler = _window->GetNativeHandler(), .Width = desc.Width, .Height = desc.Height, .BackBufferCount = desc.BackBufferCount != 0 ? desc.BackBufferCount : gpu->GetBackBufferCount(), .Format = desc.Format, .PresentMode = desc.PresentMode});
        _swapChainUsable = _swapchain.HasValue();
    } else {
        _swapChainUsable = _swapchain->Recreate(desc.Width, desc.Height, desc.Format, desc.PresentMode);
    }
    _requestRecreateSwapChain.store(!_swapChainUsable, std::memory_order_release);
    if (!_swapChainUsable) {
        RADRAY_ERR_LOG("failed to create/recreate window swapchain: {}x{}", desc.Width, desc.Height);
        return WindowOperationStatus::Failed;
    }
    _backBufferViews.resize(_swapchain->GetBackBufferCount());
    return WindowOperationStatus::Completed;
}

void AppWindow::ReleaseBackBufferViews() noexcept {
    RenderSystem* renderSystem = _manager != nullptr ? _manager->GetRenderSystem() : nullptr;
    render::RenderPassRegistry* registry = renderSystem != nullptr ? renderSystem->GetRenderPassRegistry() : nullptr;
    if (registry != nullptr) {
        for (const BackBufferView& backBuffer : _backBufferViews) {
            registry->RemoveFramebuffersUsing(backBuffer.View.get());
        }
    }
    _backBufferViews.clear();
}

WindowOperationStatus WindowManager::DestroyWindowImmediate(WindowHandle handle) noexcept {
    AssertMutationAllowed();
    auto window = ResolveWindow(handle);
    if (!window) return WindowOperationStatus::InvalidWindow;
    for (const auto& child : _windows) {
        if (child->_ownerWindow == handle) {
            child->GetNativeWindow()->SetOwner(nullptr);
            child->_ownerWindow = {};
        }
    }
    auto iter = std::ranges::find_if(_windows, [&](const auto& item) { return item.get() == window.Get(); });
    if (_mainWindow == window.Get()) {
        _mainWindow = nullptr;
        _mainWindowClosed = true;
    }
    _windows.erase(iter);
    return WindowOperationStatus::Completed;
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
    return _mainWindowClosed || (_mainWindow != nullptr && _mainWindow->GetNativeWindow()->ShouldClose());
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

task<WindowCreateResult> WindowManager::CreateWindow(WindowCreateDescriptor desc, bool isMain) {
    auto permission = co_await WaitSafe();
    auto result = CreateWindowImmediate(desc, isMain);
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::DestroyWindow(WindowHandle window) {
    auto permission = co_await WaitSafe();
    const auto result = DestroyWindowImmediate(window);
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::AttachSwapChain(WindowHandle handle, WindowSwapChainDescriptor desc) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto window = ResolveWindow(handle)) {
        desc.PresentMode = _desiredPresentMode.value_or(desc.PresentMode);
        result = window->AttachSwapChain(desc);
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::DetachSwapChain(WindowHandle handle) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto window = ResolveWindow(handle)) {
        window->DetachSwapChain();
        result = WindowOperationStatus::Completed;
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowReleaseResult> WindowManager::ReleaseSwapChain(WindowHandle handle) {
    auto permission = co_await WaitSafe();
    WindowReleaseResult result{WindowOperationStatus::InvalidWindow, {}};
    if (auto window = ResolveWindow(handle)) {
        result = {WindowOperationStatus::Completed, window->ReleaseSwapChain()};
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return std::move(result);
}

task<WindowOperationStatus> WindowManager::SetPresentMode(render::PresentMode mode) {
    auto permission = co_await WaitSafe();
    _desiredPresentMode = mode;
    auto result = WindowOperationStatus::Completed;
    for (const auto& window : _windows) {
        const auto status = window->UpdateSwapChain(mode);
        if (status == WindowOperationStatus::Failed)
            result = status;
        else if (status == WindowOperationStatus::Deferred && result == WindowOperationStatus::Completed)
            result = status;
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::SetSize(WindowHandle window, int width, int height) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto target = ResolveWindow(window)) {
        result = WindowOperationStatus::Failed;
        if (width > 0 && height > 0 && target->GetNativeWindow()->IsValid()) {
            target->GetNativeWindow()->SetSize(width, height);
            result = target->UpdateSwapChain(_desiredPresentMode);
        }
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::SetPosition(WindowHandle window, int x, int y) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto target = ResolveWindow(window)) {
        target->GetNativeWindow()->SetPosition(x, y);
        result = target->UpdateSwapChain(_desiredPresentMode);
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::Show(WindowHandle window) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto target = ResolveWindow(window)) {
        target->GetNativeWindow()->Show();
        result = target->UpdateSwapChain(_desiredPresentMode);
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::Show(WindowHandle window, NativeWindowShowMode mode) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto target = ResolveWindow(window)) {
        target->GetNativeWindow()->Show(mode);
        result = target->UpdateSwapChain(_desiredPresentMode);
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::SetAlpha(WindowHandle window, float alpha) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto target = ResolveWindow(window)) {
        target->GetNativeWindow()->SetAlpha(alpha);
        result = target->UpdateSwapChain(_desiredPresentMode);
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::SetOwner(WindowHandle handle, WindowHandle owner) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto window = ResolveWindow(handle)) {
        Nullable<NativeWindow*> nativeOwner{nullptr};
        result = WindowOperationStatus::Completed;
        if (owner.Id != 0 || owner.Owner) {
            auto ownerWindow = ResolveWindow(owner);
            if (!ownerWindow)
                result = WindowOperationStatus::InvalidWindow;
            else if (owner == handle)
                result = WindowOperationStatus::Failed;
            else
                nativeOwner = ownerWindow->GetNativeWindow();
        }
        if (result == WindowOperationStatus::Completed) {
            window->GetNativeWindow()->SetOwner(nativeOwner);
            window->_ownerWindow = owner;
            result = window->UpdateSwapChain(_desiredPresentMode);
        }
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::SetDecorated(WindowHandle window, bool value) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto target = ResolveWindow(window)) {
        target->GetNativeWindow()->SetDecorated(value);
        result = target->UpdateSwapChain(_desiredPresentMode);
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::SetShowInTaskbar(WindowHandle window, bool value) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto target = ResolveWindow(window)) {
        target->GetNativeWindow()->SetShowInTaskbar(value);
        result = target->UpdateSwapChain(_desiredPresentMode);
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
}

task<WindowOperationStatus> WindowManager::SetTopMost(WindowHandle window, bool value) {
    auto permission = co_await WaitSafe();
    auto result = WindowOperationStatus::InvalidWindow;
    if (auto target = ResolveWindow(window)) {
        target->GetNativeWindow()->SetTopMost(value);
        result = target->UpdateSwapChain(_desiredPresentMode);
    }
    if (!co_await FinishOperation(permission)) co_await StopCurrentTask();
    co_return result;
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
    RADRAY_ASSERT(!_processingOperations);
    _applyingOperations = true;
    auto phase = MakeScopeGuard([this]() noexcept { _applyingOperations = false; });
    for (const auto& window : _windows) window->DetachSwapChain();
}

void WindowManager::AssertMutationAllowed() const noexcept {
    if (_gameThread != std::this_thread::get_id() || !_applyingOperations) {
        RADRAY_ABORT("runtime window mutations require the application-thread maintenance phase");
    }
}

Nullable<AppWindow*> WindowManager::ResolveWindow(WindowHandle handle) const noexcept {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    if (handle.Owner.Get() != this || handle.Id == 0) return nullptr;
    for (const auto& window : _windows) {
        if (window->_id == handle.Id) return window.get();
    }
    return nullptr;
}

bool WindowManager::NeedsMaintenance() const noexcept {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    return !_operations.Empty() || HasSwapChainToRecreate();
}

uint64_t WindowManager::GetOperationBoundary() const noexcept {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    return _operations.GetSequenceBoundary() - 1;
}

Nullable<WindowManager::WindowOperationRecord*> WindowManager::EnqueueOperation(stop_token stop, std::coroutine_handle<> continuation) {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    if (!_acceptOperations) return nullptr;
    auto* record = _operations.Enqueue(stop, continuation);
    record->ResumeOnCancel = !_applyingOperations;
    return record;
}

void WindowManager::ProcessOperations(uint64_t boundary) {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    RADRAY_ASSERT(!_processingOperations);
    _processingOperations = true;
    auto processing = MakeScopeGuard([this]() noexcept { _processingOperations = false; });
    {
        _applyingOperations = true;
        auto phase = MakeScopeGuard([this]() noexcept { _applyingOperations = false; });
        for (size_t i = 0; i < _operations.Count(); ++i) _operations.At(i)->ResumeOnCancel = false;
        for (const auto& window : _windows) {
            if (NeedsRecreateSwapChain(window.get())) window->UpdateSwapChain(_desiredPresentMode);
        }
        for (;;) {
            Nullable<WindowOperationRecord*> next{nullptr};
            for (size_t i = 0; i < _operations.Count(); ++i) {
                auto* record = _operations.At(i);
                if (record->Sequence <= boundary && record->Phase == WindowOperationPhase::WaitingForSafety && !record->Canceled) {
                    next = record;
                    break;
                }
            }
            if (!next) break;
            next->Phase = WindowOperationPhase::Executing;
            _operations.ResumeRecord(next.Get());
            // The operation must run synchronously to its delivery barrier, even when canceled.
            if (next->Phase != WindowOperationPhase::WaitingForDelivery || !next->Continuation) {
                RADRAY_ABORT("window operation suspended before FinishOperation");
            }
        }
    }
    for (size_t i = 0; i < _operations.Count(); ++i) _operations.At(i)->ResumeOnCancel = true;
    for (;;) {
        Nullable<WindowOperationRecord*> next{nullptr};
        for (size_t i = 0; i < _operations.Count(); ++i) {
            auto* record = _operations.At(i);
            if ((record->Sequence <= boundary && record->Phase == WindowOperationPhase::WaitingForDelivery) || record->Canceled) {
                next = record;
                break;
            }
        }
        if (!next) break;
        _operations.ResumeRecord(next.Get());
    }
}

void WindowManager::CloseOperations() noexcept {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    RADRAY_ASSERT(!_processingOperations);
    _acceptOperations = false;
    _operations.CancelAll();
}

WindowManager::WindowOperationPermission::WindowOperationPermission(WindowOperationRecord* record) noexcept
    : Record(record) {}

WindowManager::WindowOperationPermission::WindowOperationPermission(WindowOperationPermission&& other) noexcept
    : Record(std::exchange(other.Record, nullptr)) {}

WindowManager::WindowOperationPermission::~WindowOperationPermission() noexcept {
    if (Record) RADRAY_ABORT("window operation returned or unwound before FinishOperation");
}

task<WindowManager::WindowOperationPermission> WindowManager::WaitSafe() {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    auto stop = co_await CurrentStopToken();
    auto record = co_await WaitSafeAwaitable{this, stop};
    if (!record) co_await StopCurrentTask();
    co_return WindowOperationPermission{record.Get()};
}

WindowManager::FinishOperationAwaitable WindowManager::FinishOperation(WindowOperationPermission& permission) noexcept {
    return {this, &permission};
}

WindowManager::WaitSafeAwaitable::WaitSafeAwaitable(WindowManager* manager, stop_token stop) noexcept
    : _manager(manager), _stop(stop) {}

bool WindowManager::WaitSafeAwaitable::await_ready() const noexcept {
    return _stop.stop_requested() || !_manager->_acceptOperations;
}

bool WindowManager::WaitSafeAwaitable::await_suspend(std::coroutine_handle<> continuation) {
    _record = _manager->EnqueueOperation(_stop, continuation);
    return _record.HasValue();
}

Nullable<WindowManager::WindowOperationRecord*> WindowManager::WaitSafeAwaitable::await_resume() noexcept {
    RADRAY_ASSERT(_manager->_gameThread == std::this_thread::get_id());
    if (!_record) return nullptr;
    if (_record->Phase == WindowOperationPhase::Executing) {
        _manager->AssertMutationAllowed();
        return _record;
    }
    RADRAY_ASSERT(!_manager->_applyingOperations && _record->Canceled);
    _manager->_operations.Erase(_record.Get());
    _record = nullptr;
    return nullptr;
}

WindowManager::FinishOperationAwaitable::FinishOperationAwaitable(WindowManager* manager, WindowOperationPermission* permission) noexcept
    : _manager(manager), _permission(permission) {}

bool WindowManager::FinishOperationAwaitable::await_ready() const noexcept {
    return false;
}

void WindowManager::FinishOperationAwaitable::await_suspend(std::coroutine_handle<> continuation) noexcept {
    _manager->AssertMutationAllowed();
    auto record = _permission->Record;
    RADRAY_ASSERT(record && record->Phase == WindowOperationPhase::Executing && !record->ResumeOnCancel);
    record->Phase = WindowOperationPhase::WaitingForDelivery;
    record->Continuation = continuation;
}

bool WindowManager::FinishOperationAwaitable::await_resume() noexcept {
    RADRAY_ASSERT(_manager->_gameThread == std::this_thread::get_id() && !_manager->_applyingOperations);
    auto record = std::exchange(_permission->Record, nullptr);
    RADRAY_ASSERT(record && record->Phase == WindowOperationPhase::WaitingForDelivery);
    const bool completed = !record->Canceled && !record->Stop.stop_requested();
    _manager->_operations.Erase(record.Get());
    return completed;
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
