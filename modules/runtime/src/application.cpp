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

std::optional<uint32_t> AppWindow::ReserveMailboxSlot() noexcept {
    // 预留成功时要立刻把槽位置为 Preparing，避免后续阶段把它当作可消费快照。
    for (uint32_t i = 0; i < _mailboxes.size(); ++i) {
        if (_mailboxes[i]._state == MailboxState::Free) {
            _mailboxes[i]._state = MailboxState::Preparing;
            return i;
        }
    }

    const auto mailboxSlot = this->GetPublishedMailboxSlot();
    if (!mailboxSlot.has_value()) {
        return std::nullopt;
    }

    _mailboxes[*mailboxSlot]._state = MailboxState::Preparing;
    return mailboxSlot;
}

void AppWindow::PublishPreparedMailbox(uint32_t mailboxSlot) noexcept {
    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());

    const auto supersededMailboxSlot = this->GetPublishedMailboxSlot();
    auto& mailbox = _mailboxes[mailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Preparing);

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
    RADRAY_ASSERT(mailbox._state == MailboxState::Queued);
    RADRAY_ASSERT(mailbox._generation != 0);

    mailbox._state = MailboxState::Published;
    _latestPublishedMailboxSlot = mailboxSlot;
    _latestPublishedGeneration = mailbox._generation;
}

void AppWindow::RestoreOrReleaseMailbox(uint32_t mailboxSlot) noexcept {
    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    auto& mailbox = _mailboxes[mailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Queued);
    RADRAY_ASSERT(mailbox._generation != 0);

    if (mailbox._generation >= _latestPublishedGeneration) {
        mailbox._state = MailboxState::Published;
        _latestPublishedMailboxSlot = mailboxSlot;
        _latestPublishedGeneration = mailbox._generation;
        return;
    }

    mailbox._state = MailboxState::Free;
    mailbox._generation = 0;
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
        if (!i._task.has_value()) {
            continue;
        }
        if (!i._task->IsCompleted()) {
            continue;
        }
        RADRAY_ASSERT(i._state == FlightState::InRender);
        this->ReleaseMailbox(i._mailboxSlot);
        i._mailboxSlot = 0;
        i._task.reset();
        i._state = FlightState::Free;
    }
}

bool AppWindow::CanRender() const noexcept {
    if (!_window->IsValid()) {
        return false;
    }
    if (!_surface->IsValid()) {
        return false;
    }
    if (_window->ShouldClose()) {
        return false;
    }
    if (_window->IsMinimized()) {
        return false;
    }
    if (_pendingRecreate) {
        return false;
    }
    const auto size = _window->GetSize();
    if (size.X <= 0 || size.Y <= 0) {
        return false;
    }
    if (_flights.empty()) {
        return false;
    }
    return true;
}

void AppWindow::HandlePresentResult(const render::SwapChainPresentResult& presentResult) {
    switch (presentResult.Status) {
        case render::SwapChainStatus::Success:
            break;
        case render::SwapChainStatus::RequireRecreate:
            _pendingRecreate = true;
            break;
        case render::SwapChainStatus::RetryLater:
        case render::SwapChainStatus::Error:
        default:
            throw AppException(fmt::format(
                "AppWindow::HandlePresentResult window {} present failed with status {} native {}",
                _selfHandle,
                presentResult.Status,
                presentResult.NativeStatusCode));
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
        this->ApplyPendingThreadMode();
        this->DispatchAllWindowEvents();
        this->CheckWindowStates();
        if (_exitRequested) {
            break;
        }
        this->OnUpdate();
        if (_exitRequested) {
            break;
        }
        if (_multiThreaded) {
            _gpu->ProcessTasks();
        } else {
            _gpu->ProcessTasksUnlocked();
        }
        this->HandleSurfaceChanges();
        if (_multiThreaded) {
            this->ScheduleFramesMultiThreaded();
        } else {
            this->ScheduleFramesSingleThreaded();
        }
    }

    if (_multiThreaded) {
        this->StopRenderThread();
        _multiThreaded = false;
        _pendingMultiThreaded = false;
    }
    this->WaitAllFlightTasks();
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
    std::unique_lock<std::mutex> resourceLock{_renderResourceMutex, std::defer_lock};
    if (_multiThreaded) {
        resourceLock.lock();
    }

    for (auto& window : _windows.Values()) {
        RADRAY_ASSERT(window._window->IsValid());
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

    const auto canRecreate = [](const AppWindow& window) {
        if (!window._pendingRecreate) {
            return false;
        }
        if (window._window->IsMinimized()) {
            return false;
        }
        const auto size = window._window->GetSize();
        return size.X > 0 && size.Y > 0;
    };

    bool hasSurfaceToRecreate = false;
    if (_multiThreaded) {
        {
            std::lock_guard lock{_renderResourceMutex};
            hasSurfaceToRecreate = std::ranges::any_of(_windows.Values(), canRecreate);
        }
    } else {
        hasSurfaceToRecreate = std::ranges::any_of(_windows.Values(), canRecreate);
    }
    if (!hasSurfaceToRecreate) {
        return;
    }

    bool renderThreadPaused = false;

    if (_multiThreaded) {
        this->PauseRenderThread();
        renderThreadPaused = true;
    }

    if (_multiThreaded) {
        std::lock_guard lock{_renderResourceMutex};
        this->DrainRenderPayloadQueue();
    }

    this->WaitAllFlightTasks();
    this->WaitAllSurfaceQueues();

    for (auto& window : _windows.Values()) {
        if (!canRecreate(window)) {
            continue;
        }
        const auto size = window._window->GetSize();
        const auto nativeHandler = window._window->GetNativeHandler();
        auto& oldSurface = window._surface;
        const auto swapchainDesc = oldSurface->GetDesc();
        const auto queueSlot = oldSurface->GetQueueSlot();
        _gpu->Wait(swapchainDesc.PresentQueue->GetQueueType(), queueSlot);
        GpuSurfaceDescriptor recreateDesc{};
        recreateDesc.NativeHandler = nativeHandler.Handle;
        recreateDesc.Width = static_cast<uint32_t>(size.X);
        recreateDesc.Height = static_cast<uint32_t>(size.Y);
        recreateDesc.BackBufferCount = swapchainDesc.BackBufferCount;
        recreateDesc.FlightFrameCount = swapchainDesc.FlightFrameCount;
        recreateDesc.Format = swapchainDesc.Format;
        recreateDesc.PresentMode = swapchainDesc.PresentMode;
        recreateDesc.QueueSlot = queueSlot;
        auto recreatedSurface = _gpu->CreateSurface(recreateDesc);
        RADRAY_ASSERT(recreatedSurface->IsValid());
        const auto flightFrameCount = recreatedSurface->GetFlightFrameCount();

        unique_ptr<GpuSurface> oldSurfaceToDestroy;
        {
            std::unique_lock<std::mutex> resourceLock{_renderResourceMutex, std::defer_lock};
            if (_multiThreaded) {
                resourceLock.lock();
            }
            oldSurfaceToDestroy = std::move(window._surface);
            window._surface = std::move(recreatedSurface);
            window._flights.clear();
            window._flights.resize(flightFrameCount);
            window._pendingRecreate = false;
        }
        oldSurfaceToDestroy.reset();
    }

    if (renderThreadPaused) {
        this->ResumeRenderThread();
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
        const auto swapchainDesc = window._surface->GetDesc();
        if (swapchainDesc.PresentQueue == nullptr) {
            continue;
        }
        _gpu->Wait(swapchainDesc.PresentQueue->GetQueueType(), window._surface->GetQueueSlot());
    }
}

void Application::RenderThreadFunc() {
    while (true) {
        {
            std::unique_lock lock{_renderWakeMutex};
            _renderWakeCV.wait(lock, [this]() noexcept {
                return _renderPauseRequested ||
                       _renderStopRequested ||
                       (_renderPayloadQueue != nullptr && _renderPayloadQueue->Size() != 0);
            });

            if (_renderStopRequested) {
                return;
            }

            if (_renderPauseRequested) {
                _renderPaused = true;
                _pauseAckCV.notify_all();

                _renderWakeCV.wait(lock, [this]() noexcept {
                    return !_renderPauseRequested || _renderStopRequested;
                });

                _renderPaused = false;
                _pauseAckCV.notify_all();
                if (_renderStopRequested) {
                    return;
                }
                continue;
            }
        }

        RenderFramePayload payload{};
        if (_renderPayloadQueue == nullptr || !_renderPayloadQueue->TryRead(payload)) {
            continue;
        }

        GpuSurface* surface = nullptr;
        {
            // _windows 锁
            std::lock_guard resourceLock{_renderResourceMutex};

            AppWindow& window = _windows.Get(payload.Window);
            RADRAY_ASSERT(payload.FlightSlot < window._flights.size());
            auto& flightData = window._flights[payload.FlightSlot];
            RADRAY_ASSERT(flightData._state == AppWindow::FlightState::Queued);
            RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
            RADRAY_ASSERT(payload.MailboxSlot < window._mailboxes.size());
            auto& mailbox = window._mailboxes[payload.MailboxSlot];
            RADRAY_ASSERT(mailbox._state == AppWindow::MailboxState::Queued);

            if (!window.CanRender()) {
                window.RestoreOrReleaseMailbox(payload.MailboxSlot);
                flightData._mailboxSlot = 0;
                flightData._state = AppWindow::FlightState::Free;
                continue;
            }

            surface = window._surface.get();
            RADRAY_ASSERT(surface != nullptr && surface->IsValid());
            const auto nextFrameSlot = surface->GetNextFrameSlotIndex();
            RADRAY_ASSERT(nextFrameSlot < window._flights.size());
            RADRAY_ASSERT(nextFrameSlot == payload.FlightSlot);
        }

        GpuRuntime::BeginFrameResult begin{};
        if (_allowFrameDrop) {
            begin = _gpu->TryBeginFrame(surface);
        } else {
            begin = _gpu->BeginFrame(surface);
        }

        if (begin.Status != render::SwapChainStatus::Success) {
            {
                std::lock_guard resourceLock{_renderResourceMutex};

                AppWindow* window = _windows.TryGet(payload.Window);
                RADRAY_ASSERT(window != nullptr);
                RADRAY_ASSERT(payload.FlightSlot < window->_flights.size());
                auto& flightData = window->_flights[payload.FlightSlot];
                RADRAY_ASSERT(flightData._state == AppWindow::FlightState::Queued);
                RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
                RADRAY_ASSERT(payload.MailboxSlot < window->_mailboxes.size());
                window->RestoreOrReleaseMailbox(payload.MailboxSlot);
                flightData._mailboxSlot = 0;
                flightData._state = AppWindow::FlightState::Free;
                if (begin.Status == render::SwapChainStatus::RequireRecreate) {
                    window->_pendingRecreate = true;
                }
            }

            if (begin.Status == render::SwapChainStatus::RetryLater ||
                begin.Status == render::SwapChainStatus::RequireRecreate) {
                continue;
            }

            throw AppException(fmt::format(
                "Application::RenderThreadFunc window {} begin frame failed with status {}",
                payload.Window,
                begin.Status));
        }

        {
            std::lock_guard resourceLock{_renderResourceMutex};

            AppWindow* window = _windows.TryGet(payload.Window);
            RADRAY_ASSERT(window != nullptr);
            RADRAY_ASSERT(payload.FlightSlot < window->_flights.size());
            auto& flightData = window->_flights[payload.FlightSlot];
            RADRAY_ASSERT(flightData._state == AppWindow::FlightState::Queued);
            RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
            RADRAY_ASSERT(payload.MailboxSlot < window->_mailboxes.size());
            auto& mailbox = window->_mailboxes[payload.MailboxSlot];
            RADRAY_ASSERT(mailbox._state == AppWindow::MailboxState::Queued);
            mailbox._state = AppWindow::MailboxState::InRender;
            flightData._state = AppWindow::FlightState::InRender;
        }

        RADRAY_ASSERT(begin.Context.HasValue());
        auto frameContext = begin.Context.Release();
        if (frameContext->_frameSlotIndex != payload.FlightSlot) {
            const auto acquiredFlightSlot = static_cast<uint32_t>(frameContext->_frameSlotIndex);
            auto abandon = _gpu->AbandonFrame(std::move(frameContext));
            if (abandon.Task.IsValid()) {
                abandon.Task.Wait();
                _gpu->ProcessTasks();
            }
            {
                std::lock_guard resourceLock{_renderResourceMutex};

                AppWindow* window = _windows.TryGet(payload.Window);
                RADRAY_ASSERT(window != nullptr);
                RADRAY_ASSERT(payload.FlightSlot < window->_flights.size());
                auto& flightData = window->_flights[payload.FlightSlot];
                RADRAY_ASSERT(flightData._state == AppWindow::FlightState::InRender);
                RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
                window->ReleaseMailbox(payload.MailboxSlot);
                flightData._mailboxSlot = 0;
                flightData._state = AppWindow::FlightState::Free;
                window->HandlePresentResult(abandon.Present);
            }
            throw AppException(fmt::format(
                "Application::RenderThreadFunc window {} expected flight slot {} but acquired {}",
                payload.Window,
                payload.FlightSlot,
                acquiredFlightSlot));
        }

        this->OnRender(payload.Window, frameContext.get(), payload.MailboxSlot);

        auto submit = frameContext->IsEmpty()
                          ? _gpu->AbandonFrame(std::move(frameContext))
                          : _gpu->SubmitFrame(std::move(frameContext));
        {
            std::lock_guard resourceLock{_renderResourceMutex};

            AppWindow* window = _windows.TryGet(payload.Window);
            RADRAY_ASSERT(window != nullptr);
            RADRAY_ASSERT(payload.FlightSlot < window->_flights.size());
            auto& flightData = window->_flights[payload.FlightSlot];
            RADRAY_ASSERT(flightData._state == AppWindow::FlightState::InRender);
            RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
            flightData._task.emplace(std::move(submit.Task));
            window->HandlePresentResult(submit.Present);
        }
    }
}

void Application::PauseRenderThread() {
    std::unique_lock lock{_renderWakeMutex};
    if (!_renderThread.joinable()) {
        return;
    }
    if (_renderStopRequested) {
        return;
    }

    _renderPauseRequested = true;
    _renderWakeCV.notify_one();
    _pauseAckCV.wait(lock, [this]() noexcept {
        return _renderPaused || _renderStopRequested;
    });
}

void Application::ResumeRenderThread() {
    {
        std::lock_guard lock{_renderWakeMutex};
        if (!_renderThread.joinable()) {
            return;
        }
        if (_renderStopRequested) {
            return;
        }
        _renderPauseRequested = false;
    }
    _renderWakeCV.notify_one();
}

void Application::StopRenderThread() {
    {
        std::lock_guard lock{_renderWakeMutex};
        if (!_renderThread.joinable()) {
            return;
        }
        _renderStopRequested = true;
        _renderPauseRequested = false;
        if (_renderPayloadQueue != nullptr) {
            _renderPayloadQueue->Complete();
        }
    }

    _pauseAckCV.notify_all();
    _renderWakeCV.notify_all();

    if (_renderThread.joinable()) {
        _renderThread.join();
    }

    {
        std::lock_guard resourceLock{_renderResourceMutex};
        this->DrainRenderPayloadQueue();
    }

    {
        std::lock_guard lock{_renderWakeMutex};
        _renderPaused = false;
    }
    _renderPayloadQueue.reset();
}

void Application::RequestMultiThreaded(bool multiThreaded) {
    _pendingMultiThreaded = multiThreaded;
}

void Application::ApplyPendingThreadMode() {
    if (_pendingMultiThreaded == _multiThreaded) {
        return;
    }
    if (_pendingMultiThreaded) {
        this->WaitAllFlightTasks();
        this->WaitAllSurfaceQueues();
        RADRAY_ASSERT(!_renderThread.joinable());
        size_t payloadCapacity = 0;
        for (const auto& window : _windows.Values()) {
            payloadCapacity += window._flights.empty() ? 1 : window._flights.size();
        }
        _renderPayloadQueue = make_unique<BoundedChannel<RenderFramePayload>>(payloadCapacity == 0 ? 1 : payloadCapacity);
        {
            std::lock_guard lock{_renderWakeMutex};
            _renderPauseRequested = false;
            _renderPaused = false;
            _renderStopRequested = false;
        }
        _renderThread = std::thread(&Application::RenderThreadFunc, this);
    } else {
        this->StopRenderThread();
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
        if (!window.CanRender()) {
            continue;
        }

        const auto mailboxSlot = window.ReserveMailboxSlot();
        if (!mailboxSlot.has_value()) {
            continue;
        }
        try {
            this->OnPrepareRender(window._selfHandle, *mailboxSlot);
        } catch (...) {
            window.ReleaseMailbox(*mailboxSlot);
            throw;
        }
        window.PublishPreparedMailbox(*mailboxSlot);
    }

    for (auto& window : _windows.Values()) {
        if (!window.CanRender()) {
            continue;
        }

        RADRAY_ASSERT(window._surface != nullptr && window._surface->IsValid());
        const auto nextFrameSlot = window._surface->GetNextFrameSlotIndex();
        RADRAY_ASSERT(nextFrameSlot < window._flights.size());

        const auto mailboxSlot = window.GetPublishedMailboxSlot();
        if (!mailboxSlot.has_value()) {
            continue;
        }

        const auto flightSlot = static_cast<uint32_t>(nextFrameSlot);
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
            // 当前 flight slot 已知完成，直接定点回收，不再整窗扫描。
            RADRAY_ASSERT(flightData._state == AppWindow::FlightState::InRender);
            window.ReleaseMailbox(flightData._mailboxSlot);
            flightData._mailboxSlot = 0;
            flight.reset();
            flightData._state = AppWindow::FlightState::Free;
        }

        if (flightData._state != AppWindow::FlightState::Free) {
            continue;
        }

        auto& mailbox = window._mailboxes[*mailboxSlot];
        mailbox._state = AppWindow::MailboxState::Queued;
        flightData._state = AppWindow::FlightState::Queued;
        flightData._mailboxSlot = *mailboxSlot;

        GpuRuntime::BeginFrameResult begin{};
        if (_allowFrameDrop) {
            begin = _gpu->TryBeginFrame(window._surface.get());
        } else {
            begin = _gpu->BeginFrame(window._surface.get());
        }

        switch (begin.Status) {
            case render::SwapChainStatus::Success:
                mailbox._state = AppWindow::MailboxState::InRender;
                flightData._state = AppWindow::FlightState::InRender;
                break;
            case render::SwapChainStatus::RetryLater:
                window.RestoreMailbox(*mailboxSlot);
                flightData._mailboxSlot = 0;
                flightData._state = AppWindow::FlightState::Free;
                continue;
            case render::SwapChainStatus::RequireRecreate:
                window.RestoreMailbox(*mailboxSlot);
                flightData._mailboxSlot = 0;
                flightData._state = AppWindow::FlightState::Free;
                window._pendingRecreate = true;
                continue;
            case render::SwapChainStatus::Error:
            default:
                window.RestoreMailbox(*mailboxSlot);
                flightData._mailboxSlot = 0;
                flightData._state = AppWindow::FlightState::Free;
                throw AppException(fmt::format(
                    "Application::ScheduleFramesSingleThreaded window {} begin frame failed with status {}",
                    window._selfHandle,
                    begin.Status));
        }

        RADRAY_ASSERT(begin.Context.HasValue());
        auto frameContext = begin.Context.Release();
        RADRAY_ASSERT(frameContext->_frameSlotIndex == flightSlot);

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
            flightData._mailboxSlot = 0;
            flightData._state = AppWindow::FlightState::Free;
            auto abandon = _gpu->AbandonFrame(std::move(frameContext));
            window.HandlePresentResult(abandon.Present);
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
        window.HandlePresentResult(submit.Present);
    }
}

void Application::ScheduleFramesMultiThreaded() {
    if (_renderPayloadQueue == nullptr) {
        return;
    }

    bool queuedAny = false;
    {
        std::lock_guard resourceLock{_renderResourceMutex};
        for (auto& window : _windows.Values()) {
            window.CollectCompletedFlightTasks();
        }
    }

    for (auto& window : _windows.Values()) {
        AppWindowHandle windowHandle{};
        uint32_t flightSlot = 0;
        uint32_t mailboxSlot = 0;

        {
            std::lock_guard resourceLock{_renderResourceMutex};
            if (!window.CanRender()) {
                continue;
            }

            RADRAY_ASSERT(window._surface != nullptr && window._surface->IsValid());
            const auto nextFrameSlot = window._surface->GetNextFrameSlotIndex();
            RADRAY_ASSERT(nextFrameSlot < window._flights.size());

            const auto selectedFlightSlot = static_cast<uint32_t>(nextFrameSlot);
            auto& flightData = window._flights[selectedFlightSlot];
            if (flightData._state != AppWindow::FlightState::Free || flightData._task.has_value()) {
                continue;
            }

            const auto reservedMailboxSlot = window.ReserveMailboxSlot();
            if (!reservedMailboxSlot.has_value()) {
                continue;
            }

            windowHandle = window._selfHandle;
            flightSlot = selectedFlightSlot;
            mailboxSlot = *reservedMailboxSlot;
        }

        try {
            this->OnPrepareRender(windowHandle, mailboxSlot);
        } catch (...) {
            std::lock_guard resourceLock{_renderResourceMutex};
            AppWindow* lockedWindow = _windows.TryGet(windowHandle);
            RADRAY_ASSERT(lockedWindow != nullptr);
            RADRAY_ASSERT(mailboxSlot < lockedWindow->_mailboxes.size());
            lockedWindow->ReleaseMailbox(mailboxSlot);
            throw;
        }

        RenderFramePayload payload{};
        payload.Window = windowHandle;
        payload.FlightSlot = flightSlot;
        payload.MailboxSlot = mailboxSlot;

        {
            std::lock_guard resourceLock{_renderResourceMutex};

            AppWindow* lockedWindow = _windows.TryGet(windowHandle);
            RADRAY_ASSERT(lockedWindow != nullptr);
            RADRAY_ASSERT(flightSlot < lockedWindow->_flights.size());
            RADRAY_ASSERT(mailboxSlot < lockedWindow->_mailboxes.size());
            auto& flightData = lockedWindow->_flights[flightSlot];
            RADRAY_ASSERT(flightData._state == AppWindow::FlightState::Free);
            RADRAY_ASSERT(!flightData._task.has_value());

            lockedWindow->PublishPreparedMailbox(mailboxSlot);

            auto& mailbox = lockedWindow->_mailboxes[mailboxSlot];
            mailbox._state = AppWindow::MailboxState::Queued;
            flightData._state = AppWindow::FlightState::Queued;
            flightData._mailboxSlot = mailboxSlot;
            if (!_renderPayloadQueue->TryWrite(payload)) {
                lockedWindow->RestoreOrReleaseMailbox(mailboxSlot);
                flightData._mailboxSlot = 0;
                flightData._state = AppWindow::FlightState::Free;
                continue;
            }
            queuedAny = true;
        }
    }

    if (queuedAny) {
        {
            // Pair with the render wait mutex so notify cannot land between predicate check and sleep.
            std::lock_guard wakeLock{_renderWakeMutex};
        }
        _renderWakeCV.notify_one();
    }
}

void Application::DrainRenderPayloadQueue() noexcept {
    if (_renderPayloadQueue == nullptr) {
        return;
    }

    RenderFramePayload payload{};
    while (_renderPayloadQueue->TryRead(payload)) {
        AppWindow* window = _windows.TryGet(payload.Window);
        if (window == nullptr) {
            continue;
        }
        if (payload.FlightSlot >= window->_flights.size()) {
            continue;
        }

        auto& flightData = window->_flights[payload.FlightSlot];
        if (flightData._state != AppWindow::FlightState::Queued) {
            continue;
        }
        RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
        if (flightData._mailboxSlot != payload.MailboxSlot) {
            continue;
        }
        if (payload.MailboxSlot >= window->_mailboxes.size()) {
            flightData._mailboxSlot = 0;
            flightData._state = AppWindow::FlightState::Free;
            continue;
        }

        auto& mailbox = window->_mailboxes[payload.MailboxSlot];
        RADRAY_ASSERT(mailbox._state == AppWindow::MailboxState::Queued);
        if (mailbox._state == AppWindow::MailboxState::Queued) {
            window->RestoreOrReleaseMailbox(payload.MailboxSlot);
        }
        flightData._mailboxSlot = 0;
        flightData._state = AppWindow::FlightState::Free;
    }
}

}  // namespace radray
