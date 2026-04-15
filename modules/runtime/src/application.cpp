#include <radray/runtime/application.h>

#include <exception>
#include <ranges>

#include <radray/logger.h>

namespace radray {

void AppWindow::ResetMailboxes() noexcept {
    for (auto& mailbox : _mailboxes) {
        mailbox = {};
    }
    _latestPublishedMailboxSlot.reset();
    _latestPublishedGeneration = 0;
}

std::optional<uint32_t> AppWindow::GetPublishedMailboxSlot() const noexcept {
    if (!_latestPublishedMailboxSlot.has_value()) {
        return std::nullopt;
    }

    const uint32_t mailboxSlot = *_latestPublishedMailboxSlot;
    if (mailboxSlot >= _mailboxes.size()) {
        return std::nullopt;
    }

    if (_mailboxes[mailboxSlot]._state != MailboxState::Published) {
        return std::nullopt;
    }

    return mailboxSlot;
}

std::optional<uint32_t> AppWindow::GetPrepareMailboxSlot() const noexcept {
    for (uint32_t i = 0; i < _mailboxes.size(); ++i) {
        if (_mailboxes[i]._state == MailboxState::Free) {
            return i;
        }
    }
    return this->GetPublishedMailboxSlot();
}

void AppWindow::PublishPreparedMailbox(uint32_t mailboxSlot) noexcept {
    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());

    const auto supersededMailboxSlot = this->GetPublishedMailboxSlot();
    auto& mailbox = _mailboxes[mailboxSlot];
    RADRAY_ASSERT(mailbox._state != MailboxState::InRender);

    const uint64_t nextGeneration = _latestPublishedGeneration + 1;
    mailbox._state = MailboxState::Published;
    mailbox._generation = nextGeneration;
    _latestPublishedMailboxSlot = mailboxSlot;
    _latestPublishedGeneration = nextGeneration;

    // 被新版本替代的旧槽位可以在这里释放，因为只有 Published 槽位会被覆盖
    // 一旦槽位提交渲染，它的状态会变更为 InRender，并一直保留到对应 flight 完成
    if (supersededMailboxSlot.has_value() && *supersededMailboxSlot != mailboxSlot) {
        this->ReleaseMailbox(*supersededMailboxSlot);
    }
}

void AppWindow::RestoreMailbox(uint32_t mailboxSlot) noexcept {
    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    auto& mailbox = _mailboxes[mailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::InRender);
    RADRAY_ASSERT(mailbox._generation != 0);

    mailbox._state = MailboxState::Published;
    _latestPublishedMailboxSlot = mailboxSlot;
    _latestPublishedGeneration = mailbox._generation;
}

void AppWindow::ReleaseMailbox(uint32_t mailboxSlot) noexcept {
    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    auto& mailbox = _mailboxes[mailboxSlot];
    RADRAY_ASSERT(mailbox._state != MailboxState::Free);

    mailbox._state = MailboxState::Free;
    mailbox._generation = 0;
    if (_latestPublishedMailboxSlot == mailboxSlot) {
        _latestPublishedMailboxSlot.reset();
    }
}

void AppWindow::CollectCompletedFlightTasks() noexcept {
    for (auto& i : _flights) {
        auto& task = i._task;
        auto& mailboxSlot = i._mailboxSlot;
        if (!task.has_value()) {
            RADRAY_ASSERT(!mailboxSlot.has_value());
            continue;
        }
        if (!task->IsCompleted()) {
            continue;
        }
        RADRAY_ASSERT(mailboxSlot.has_value());
        if (mailboxSlot.has_value()) {
            this->ReleaseMailbox(*mailboxSlot);
            mailboxSlot.reset();
        }
        task.reset();
    }
}

AppWindow::AppWindow(AppWindow&& other) noexcept
    : _selfHandle(std::exchange(other._selfHandle, {})),
      _window(std::move(other._window)),
      _surface(std::move(other._surface)),
      _flights(std::move(other._flights)),
      _mailboxes(std::move(other._mailboxes)),
      _latestPublishedMailboxSlot(std::exchange(other._latestPublishedMailboxSlot, std::nullopt)),
      _latestPublishedGeneration(std::exchange(other._latestPublishedGeneration, 0)),
      _isPrimary(std::exchange(other._isPrimary, false)),
      _pendingRecreate(std::exchange(other._pendingRecreate, false)) {}

AppWindow& AppWindow::operator=(AppWindow&& other) noexcept {
    if (this != &other) {
        AppWindow tmp{std::move(other)};
        swap(*this, tmp);
    }
    return *this;
}

AppWindow::~AppWindow() noexcept {
    _flights.clear();
    _mailboxes.clear();
    _surface.reset();
    _window.reset();
}

void swap(AppWindow& a, AppWindow& b) noexcept {
    using std::swap;
    swap(a._selfHandle, b._selfHandle);
    swap(a._window, b._window);
    swap(a._surface, b._surface);
    swap(a._flights, b._flights);
    swap(a._mailboxes, b._mailboxes);
    swap(a._latestPublishedMailboxSlot, b._latestPublishedMailboxSlot);
    swap(a._latestPublishedGeneration, b._latestPublishedGeneration);
    swap(a._isPrimary, b._isPrimary);
    swap(a._pendingRecreate, b._pendingRecreate);
}

int32_t Application::Run(int argc, char* argv[]) {
    this->OnInitialize();
    RADRAY_ASSERT(_gpu && _gpu->IsValid());
    RADRAY_ASSERT(std::ranges::any_of(_windows.Values(), [](const auto& w) { return w._isPrimary; }));

    while (!_exitRequested) {
        this->DispatchAllWindowEvents();
        this->CheckWindowStates();
        if (_exitRequested) {
            break;
        }
        this->OnUpdate();
        if (_exitRequested) {
            break;
        }
        {
            std::unique_lock<std::mutex> l{_renderMutex, std::defer_lock};
            if (_multiThreaded) {
                l.lock();
            }
            _gpu->ProcessTasks();
        }
        this->HandleSurfaceChanges();
        if (!_multiThreaded) {
            this->ScheduleFramesSingleThreaded();
        }
    }

    if (!_multiThreaded) {
        this->WaitAllFlightTasks();
    }
    this->WaitAllSurfaceQueues();
    this->OnShutdown();

    return 0;
}

void Application::OnShutdown() {
    _windows.Clear();
    _gpu.reset();
}

void Application::CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, std::optional<render::VulkanInstanceDescriptor> vkInsDesc) {
    RADRAY_ASSERT(!_gpu);
    unique_ptr<render::InstanceVulkan> vkIns;
    if (vkInsDesc.has_value()) {
        auto vkInsOpt = render::CreateVulkanInstance(*vkInsDesc);
        if (!vkInsOpt) {
            throw AppException("Failed to create Vulkan instance");
        }
        vkIns = vkInsOpt.Release();
    }
    this->CreateGpuRuntime(deviceDesc, std::move(vkIns));
}

void Application::CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, unique_ptr<render::InstanceVulkan> vkIns) {
    RADRAY_ASSERT(!_gpu);
    shared_ptr<render::Device> device;
    {
        auto deviceOpt = render::CreateDevice(deviceDesc);
        if (!deviceOpt) {
            throw AppException("Failed to create Vulkan device");
        }
        device = deviceOpt.Release();
    }
    _gpu = make_unique<GpuRuntime>(std::move(device), std::move(vkIns));
}

AppWindowHandle Application::CreateWindow(
    const NativeWindowCreateDescriptor& windowDesc,
    const GpuSurfaceDescriptor& surfaceDesc_,
    bool isPrimary,
    uint32_t mailboxCount) {
    RADRAY_ASSERT(_gpu && _gpu->IsValid());
    RADRAY_ASSERT(std::ranges::any_of(_windows.Values(), [](const auto& w) { return w._isPrimary; }) == false || isPrimary == false);
    if (mailboxCount == 0) {
        throw AppException("Application::CreateWindow requires mailboxCount > 0");
    }
    auto windowOpt = CreateNativeWindow(windowDesc);
    if (!windowOpt) {
        throw AppException("Failed to create native window");
    }
    auto window = windowOpt.Release();
    auto surfaceDesc = surfaceDesc_;
    surfaceDesc.NativeHandler = window->GetNativeHandler().Handle;
    auto surface = _gpu->CreateSurface(surfaceDesc);
    AppWindow appWindow{};
    appWindow._window = std::move(window);
    appWindow._surface = std::move(surface);
    if (appWindow._surface != nullptr && appWindow._surface->IsValid()) {
        appWindow._flights.resize(appWindow._surface->GetFlightFrameCount());
    }
    appWindow._mailboxes.resize(mailboxCount);
    appWindow.ResetMailboxes();
    appWindow._isPrimary = isPrimary;
    auto handle = _windows.Emplace(std::move(appWindow));
    _windows.Get(handle)._selfHandle = handle;
    return handle;
}

void Application::DispatchAllWindowEvents() {
    for (auto& i : _windows.Values()) {
        i._window->DispatchEvents();
    }
}

void Application::CheckWindowStates() {
    for (auto& window : _windows.Values()) {
        if (window._window == nullptr || !window._window->IsValid()) {
            continue;
        }
        if (window._window->ShouldClose()) {
            if (window._isPrimary) {
                _exitRequested = true;
            }
            continue;
        }
        if (window._window->IsMinimized()) {
            continue;
        }
        const auto size = window._window->GetSize();
        if (size.X <= 0 || size.Y <= 0) {
            continue;
        }
        if (window._surface != nullptr &&
            (window._surface->GetWidth() != static_cast<uint32_t>(size.X) ||
             window._surface->GetHeight() != static_cast<uint32_t>(size.Y))) {
            window._pendingRecreate = true;
        }
    }
}

void Application::HandleSurfaceChanges() {
    RADRAY_ASSERT(_gpu && _gpu->IsValid());
    std::unique_lock<std::mutex> gpuLock{_renderMutex, std::defer_lock};
    if (_multiThreaded) {
        gpuLock.lock();
    }
    for (auto& window : _windows.Values()) {
        if (!window._pendingRecreate) {
            continue;
        }
        if (window._window == nullptr || !window._window->IsValid()) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges window {} is invalid", window._selfHandle));
        }
        if (window._surface == nullptr || !window._surface->IsValid()) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges surface for window {} is invalid", window._selfHandle));
        }
        if (window._window->IsMinimized()) {
            continue;
        }
        const auto size = window._window->GetSize();
        if (size.X <= 0 || size.Y <= 0) {
            continue;
        }
        const auto nativeHandler = window._window->GetNativeHandler();
        if (nativeHandler.Handle == nullptr) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges window {} native handle is null", window._selfHandle));
        }
        auto& oldSurface = window._surface;
        const auto swapchainDesc = oldSurface->_swapchain->GetDesc();
        if (swapchainDesc.PresentQueue == nullptr) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges surface for window {} has no present queue", window._selfHandle));
        }
        _gpu->Wait(swapchainDesc.PresentQueue->GetQueueType(), oldSurface->_queueSlot);
        GpuSurfaceDescriptor recreateDesc{};
        recreateDesc.NativeHandler = nativeHandler.Handle;
        recreateDesc.Width = static_cast<uint32_t>(size.X);
        recreateDesc.Height = static_cast<uint32_t>(size.Y);
        recreateDesc.BackBufferCount = swapchainDesc.BackBufferCount;
        recreateDesc.FlightFrameCount = swapchainDesc.FlightFrameCount;
        recreateDesc.Format = swapchainDesc.Format;
        recreateDesc.PresentMode = swapchainDesc.PresentMode;
        recreateDesc.QueueSlot = oldSurface->_queueSlot;
        oldSurface.reset();
        auto recreatedSurface = _gpu->CreateSurface(recreateDesc);
        if (recreatedSurface == nullptr || !recreatedSurface->IsValid()) {
            throw AppException(fmt::format("Application::HandleSurfaceChanges failed to recreate surface for window {}", window._selfHandle));
        }
        window._surface = std::move(recreatedSurface);
        window._flights.clear();
        window._flights.resize(window._surface->GetFlightFrameCount());
        window.ResetMailboxes();
        window._pendingRecreate = false;
    }
}

bool Application::CanRenderWindow(AppWindowHandle window) const {
    const auto* appWindow = _windows.TryGet(window);
    if (appWindow == nullptr) {
        return false;
    }
    if (appWindow->_window == nullptr || !appWindow->_window->IsValid()) {
        return false;
    }
    if (appWindow->_surface == nullptr || !appWindow->_surface->IsValid()) {
        return false;
    }
    if (appWindow->_window->ShouldClose()) {
        return false;
    }
    if (appWindow->_window->IsMinimized()) {
        return false;
    }
    if (appWindow->_pendingRecreate) {
        return false;
    }
    const auto size = appWindow->_window->GetSize();
    if (size.X <= 0 || size.Y <= 0) {
        return false;
    }
    if (appWindow->_flights.empty()) {
        return false;
    }
    return true;
}

void Application::HandlePresentResult(AppWindow& window, const render::SwapChainPresentResult& presentResult) {
    switch (presentResult.Status) {
        case render::SwapChainStatus::Success:
            break;
        case render::SwapChainStatus::RequireRecreate:
            window._pendingRecreate = true;
            break;
        case render::SwapChainStatus::RetryLater:
        case render::SwapChainStatus::Error:
        default:
            throw AppException(fmt::format(
                "Application::HandlePresentResult window {} present failed with status {} native {}",
                window._selfHandle,
                presentResult.Status,
                presentResult.NativeStatusCode));
    }
}

void Application::WaitAllFlightTasks() {
    for (auto& window : _windows.Values()) {
        for (auto& slot : window._flights) {
            if (!slot._task.has_value()) {
                continue;
            }
            slot._task->Wait();
            _gpu->ProcessTasks();
        }
        window.CollectCompletedFlightTasks();
    }
}

void Application::WaitAllSurfaceQueues() {
    if (_gpu == nullptr || !_gpu->IsValid()) {
        return;
    }
    for (auto& window : _windows.Values()) {
        if (window._surface == nullptr || !window._surface->IsValid()) {
            continue;
        }
        if (window._surface->_swapchain == nullptr) {
            continue;
        }
        const auto swapchainDesc = window._surface->_swapchain->GetDesc();
        if (swapchainDesc.PresentQueue == nullptr) {
            continue;
        }
        _gpu->Wait(swapchainDesc.PresentQueue->GetQueueType(), window._surface->_queueSlot);
    }
}

void Application::RequestMultiThreaded(bool multiThreaded) {
    _pendingMultiThreaded = multiThreaded;
}

void Application::ApplyPendingThreadMode() {
    if (_pendingMultiThreaded == _multiThreaded) {
        return;
    }
    if (_pendingMultiThreaded) {
    } else {
        _renderCommandQueue.WaitWrite(StopRenderThreadCommand{});
        if (_renderThread.joinable()) {
            _renderThread.join();
        }
        this->WaitAllFlightTasks();
        this->WaitAllSurfaceQueues();
    }
    _multiThreaded = _pendingMultiThreaded;
}

void Application::ScheduleFramesSingleThreaded() {
    for (auto& window : _windows.Values()) {
        window.CollectCompletedFlightTasks();
    }
    for (auto& window : _windows.Values()) {
        if (!this->CanRenderWindow(window._selfHandle)) {
            continue;
        }

        const auto mailboxSlot = window.GetPrepareMailboxSlot();
        if (!mailboxSlot.has_value()) {
            continue;
        }
        this->OnPrepareRender(window._selfHandle, *mailboxSlot);
        window.PublishPreparedMailbox(*mailboxSlot);
    }

    for (auto& window : _windows.Values()) {
        if (!this->CanRenderWindow(window._selfHandle)) {
            continue;
        }

        RADRAY_ASSERT(window._surface != nullptr && window._surface->IsValid());
        RADRAY_ASSERT(window._surface->_nextFrameSlotIndex < window._flights.size());

        const auto mailboxSlot = window.GetPublishedMailboxSlot();
        if (!mailboxSlot.has_value()) {
            continue;
        }

        const auto flightSlot = static_cast<uint32_t>(window._surface->_nextFrameSlotIndex);
        auto& flightData = window._flights[flightSlot];
        auto& flight = flightData._task;
        if (flight.has_value()) {
            if (_allowFrameDrop) {
                if (!flight->IsCompleted()) {
                    continue;
                }
            } else {
                flight->Wait();
                _gpu->ProcessTasks();
            }
            RADRAY_ASSERT(flight->IsCompleted());
            RADRAY_ASSERT(flightData._mailboxSlot.has_value());
            // 当前 flight slot 已知完成，直接定点回收，不再整窗扫描。
            window.ReleaseMailbox(*flightData._mailboxSlot);
            flightData._mailboxSlot.reset();
            flight.reset();
        }

        auto& mailbox = window._mailboxes[*mailboxSlot];
        mailbox._state = AppWindow::MailboxState::InRender;

        GpuRuntime::BeginFrameResult begin{};
        if (_allowFrameDrop) {
            begin = _gpu->TryBeginFrame(window._surface.get());
        } else {
            begin = _gpu->BeginFrame(window._surface.get());
        }

        switch (begin.Status) {
            case render::SwapChainStatus::Success:
                break;
            case render::SwapChainStatus::RetryLater:
                window.RestoreMailbox(*mailboxSlot);
                continue;
            case render::SwapChainStatus::RequireRecreate:
                window.RestoreMailbox(*mailboxSlot);
                window._pendingRecreate = true;
                continue;
            case render::SwapChainStatus::Error:
            default:
                window.RestoreMailbox(*mailboxSlot);
                throw AppException(fmt::format(
                    "Application::ScheduleFramesSingleThreaded window {} begin frame failed with status {}",
                    window._selfHandle,
                    begin.Status));
        }

        RADRAY_ASSERT(begin.Context.HasValue());
        auto frameContext = begin.Context.Release();

        std::exception_ptr renderError{};
        try {
            this->OnRender(window._selfHandle, frameContext.get(), *mailboxSlot);
        } catch (const std::exception& e) {
            renderError = std::current_exception();
            RADRAY_ERR_LOG("exception during OnRender for window {}\n  {}", window._selfHandle, e.what());
        } catch (...) {
            renderError = std::current_exception();
            RADRAY_ERR_LOG("exception during OnRender for window {}\n  {}", window._selfHandle, "unknown error");
        }

        if (renderError != nullptr) [[unlikely]] {
            window.ReleaseMailbox(*mailboxSlot);
            auto abandon = _gpu->AbandonFrame(std::move(frameContext));
            this->HandlePresentResult(window, abandon.Present);
            if (abandon.Task.IsValid()) {
                abandon.Task.Wait();
                _gpu->ProcessTasks();
            }
            std::rethrow_exception(renderError);
        }

        auto submit = frameContext->IsEmpty()
                          ? _gpu->AbandonFrame(std::move(frameContext))
                          : _gpu->SubmitFrame(std::move(frameContext));
        flight.emplace(std::move(submit.Task));
        flightData._mailboxSlot = *mailboxSlot;
        this->HandlePresentResult(window, submit.Present);
    }
}

void Application::ScheduleFramesMultiThreaded() {
}

}  // namespace radray
