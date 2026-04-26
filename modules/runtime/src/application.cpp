#include <radray/runtime/application.h>

#include <chrono>
#include <variant>

#include <radray/logger.h>
#include <radray/utility.h>

namespace radray {

AppWindow::~AppWindow() noexcept {
    this->ResetMailboxes();
}

void AppWindow::RenderDataChannel::Stop() noexcept {
    std::lock_guard<std::mutex> lock{_mutex};
    _completed = true;
}

void AppWindow::ResetMailboxes() noexcept {
    std::unique_lock<std::mutex> stateLock{_stateMutex, std::defer_lock};
    std::unique_lock<std::mutex> channelLock{_channel._mutex, std::defer_lock};
    if (_app->_multiThreaded) {
        std::lock(stateLock, channelLock);
    }

    for (const RenderRequest& request : _channel._queue) {
        RADRAY_ASSERT(request.FlightSlot < _flights.size());
        RADRAY_ASSERT(request.MailboxSlot < _mailboxes.size());
        FlightData& flight = _flights[request.FlightSlot];
        RADRAY_ASSERT(flight._state == FlightState::Queued);
        RADRAY_ASSERT(flight._mailboxSlot == request.MailboxSlot);
        RADRAY_ASSERT(flight._mailboxGeneration == request.Generation);
        MailboxData& mailbox = _mailboxes[request.MailboxSlot];
        RADRAY_ASSERT(mailbox._state == MailboxState::Queued);
        RADRAY_ASSERT(mailbox._generation == request.Generation);
        mailbox._state = MailboxState::Free;
        flight._mailboxSlot = 0;
        flight._mailboxGeneration = 0;
        flight._task = {};
        flight._state = FlightState::Free;
    }
    _channel._queue.clear();
    for (FlightData& flight : _flights) {
        RADRAY_ASSERT(flight._state != FlightState::Queued);
        RADRAY_ASSERT(flight._state != FlightState::Preparing);
        if (flight._state == FlightState::InRender) {
            RADRAY_ASSERT(flight._mailboxSlot < _mailboxes.size());
            MailboxData& mailbox = _mailboxes[flight._mailboxSlot];
            RADRAY_ASSERT(mailbox._state == MailboxState::InRender);
            RADRAY_ASSERT(mailbox._generation == flight._mailboxGeneration);
            flight._task.Wait();
        }
        flight._mailboxSlot = 0;
        flight._mailboxGeneration = 0;
        flight._task = {};
        flight._state = FlightState::Free;
    }
    for (MailboxData& mailbox : _mailboxes) {
        mailbox._state = MailboxState::Free;
    }
    _latestPublished.reset();
}

std::optional<uint32_t> AppWindow::AllocMailboxSlot() noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    std::optional<uint32_t> selectedSlot;
    if (_latestPublished.has_value()) {
        const MailboxSnapshot last = *_latestPublished;
        RADRAY_ASSERT(last.Slot < _mailboxes.size());
        _latestPublished.reset();
        const MailboxData& mailbox = _mailboxes[last.Slot];
        RADRAY_ASSERT(mailbox._state == MailboxState::Published);
        selectedSlot = last.Slot;
    }
    if (!selectedSlot.has_value()) {
        for (size_t i = 0; i < _mailboxes.size(); i++) {
            if (_mailboxes[i]._state == MailboxState::Free) {
                selectedSlot = static_cast<uint32_t>(i);
                break;
            }
        }
    }
    if (!selectedSlot.has_value()) {
        return std::nullopt;
    }
    MailboxData& mailbox = _mailboxes[*selectedSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Free || mailbox._state == MailboxState::Published);
    mailbox._generation++;
    mailbox._state = MailboxState::Preparing;
    return selectedSlot;
}

void AppWindow::PublishPreparedMailbox(uint32_t mailboxSlot) noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    RADRAY_ASSERT(_mailboxes[mailboxSlot]._state == MailboxState::Preparing);
    if (_latestPublished.has_value()) {
        const MailboxSnapshot oldSnapshot = *_latestPublished;
        RADRAY_ASSERT(oldSnapshot.Slot < _mailboxes.size());
        _latestPublished.reset();
        if (oldSnapshot.Slot != mailboxSlot) {
            MailboxData& oldMailbox = _mailboxes[oldSnapshot.Slot];
            RADRAY_ASSERT(oldMailbox._state == MailboxState::Published);
            RADRAY_ASSERT(oldMailbox._generation == oldSnapshot.Generation);
            oldMailbox._state = MailboxState::Free;
        }
    }
    _mailboxes[mailboxSlot]._state = MailboxState::Published;
    _latestPublished = MailboxSnapshot{mailboxSlot, MailboxState::Published, _mailboxes[mailboxSlot]._generation};
}

void AppWindow::ReleaseMailbox(RenderRequest request) noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    RADRAY_ASSERT(request.FlightSlot < _flights.size());
    RADRAY_ASSERT(request.MailboxSlot < _mailboxes.size());

    FlightData& flight = _flights[request.FlightSlot];
    RADRAY_ASSERT(flight._state == FlightState::Preparing || flight._state == FlightState::InRender);
    RADRAY_ASSERT(flight._mailboxSlot == request.MailboxSlot);
    RADRAY_ASSERT(flight._mailboxGeneration == request.Generation);
    if (flight._state == FlightState::InRender) {
        RADRAY_ASSERT(flight._task.IsCompleted());
    }
    MailboxData& mailbox = _mailboxes[request.MailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::InRender);
    RADRAY_ASSERT(mailbox._generation == request.Generation);
    RADRAY_ASSERT(!_latestPublished.has_value() || _latestPublished->Slot != request.MailboxSlot);
    mailbox._state = MailboxState::Free;
    flight._mailboxSlot = 0;
    flight._mailboxGeneration = 0;
    flight._task = {};
    flight._state = FlightState::Free;
}

std::optional<AppWindow::RenderRequest> AppWindow::TryQueueLatestPublished() noexcept {
    std::unique_lock<std::mutex> stateLock{_stateMutex, std::defer_lock};
    std::unique_lock<std::mutex> channelLock{_channel._mutex, std::defer_lock};
    if (_app->_multiThreaded) {
        std::lock(stateLock, channelLock);
    }

    if (!_latestPublished.has_value() || _channel._completed) {
        return std::nullopt;
    }
    if (_channel._queueCapacity != 0 && _channel._queue.size() >= _channel._queueCapacity) {
        return std::nullopt;
    }
    const size_t frameSlotIndex = _surface->GetNextFrameSlotIndex();
    RADRAY_ASSERT(frameSlotIndex <= std::numeric_limits<uint32_t>::max());
    const uint32_t flightSlot = static_cast<uint32_t>(frameSlotIndex);
    RADRAY_ASSERT(flightSlot < _flights.size());
    FlightData& flight = _flights[flightSlot];
    if (flight._state != FlightState::Free) {
        return std::nullopt;
    }
    const MailboxSnapshot snapshot = *_latestPublished;
    RADRAY_ASSERT(snapshot.Slot < _mailboxes.size());
    RADRAY_ASSERT(snapshot.State == MailboxState::Published);
    MailboxData& mailbox = _mailboxes[snapshot.Slot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Published);
    RADRAY_ASSERT(mailbox._generation == snapshot.Generation);
    RenderRequest request{flightSlot, snapshot.Slot, snapshot.Generation};
    _latestPublished.reset();
    mailbox._state = MailboxState::Queued;
    flight._state = FlightState::Queued;
    flight._mailboxSlot = request.MailboxSlot;
    flight._mailboxGeneration = request.Generation;
    flight._task = {};
    _channel._queue.emplace_back(request);
    return request;
}

std::optional<AppWindow::RenderRequest> AppWindow::TryClaimQueuedRenderRequest() noexcept {
    std::unique_lock<std::mutex> stateLock{_stateMutex, std::defer_lock};
    std::unique_lock<std::mutex> channelLock{_channel._mutex, std::defer_lock};
    if (_app->_multiThreaded) {
        std::lock(stateLock, channelLock);
    }

    if (_channel._queue.empty()) {
        return std::nullopt;
    }

    RenderRequest request = _channel._queue.front();
    RADRAY_ASSERT(request.FlightSlot < _flights.size());
    RADRAY_ASSERT(request.MailboxSlot < _mailboxes.size());
    FlightData& flight = _flights[request.FlightSlot];
    RADRAY_ASSERT(flight._state == FlightState::Queued);
    RADRAY_ASSERT(flight._mailboxSlot == request.MailboxSlot);
    RADRAY_ASSERT(flight._mailboxGeneration == request.Generation);
    MailboxData& mailbox = _mailboxes[request.MailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Queued);
    RADRAY_ASSERT(mailbox._generation == request.Generation);
    _channel._queue.pop_front();
    mailbox._state = MailboxState::InRender;
    flight._state = FlightState::Preparing;
    return request;
}

void AppWindow::EndPrepareRenderTask(RenderRequest request, GpuTask task) noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    RADRAY_ASSERT(request.FlightSlot < _flights.size());
    RADRAY_ASSERT(request.MailboxSlot < _mailboxes.size());

    FlightData& flight = _flights[request.FlightSlot];
    RADRAY_ASSERT(flight._state == FlightState::Preparing);
    RADRAY_ASSERT(flight._mailboxSlot == request.MailboxSlot);
    RADRAY_ASSERT(flight._mailboxGeneration == request.Generation);
    RADRAY_ASSERT(!flight._task.IsValid());

    MailboxData& mailbox = _mailboxes[request.MailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::InRender);
    RADRAY_ASSERT(mailbox._generation == request.Generation);

    flight._task = std::move(task);
    flight._state = FlightState::InRender;
}

void AppWindow::CollectCompletedFlightSlots() noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    for (FlightData& flight : _flights) {
        if (flight._state != FlightState::InRender || !flight._task.IsCompleted()) {
            continue;
        }
        RADRAY_ASSERT(flight._mailboxSlot < _mailboxes.size());
        MailboxData& mailbox = _mailboxes[flight._mailboxSlot];
        RADRAY_ASSERT(mailbox._state == MailboxState::InRender);
        RADRAY_ASSERT(mailbox._generation == flight._mailboxGeneration);
        RADRAY_ASSERT(!_latestPublished.has_value() || _latestPublished->Slot != flight._mailboxSlot);

        mailbox._state = MailboxState::Free;
        flight._task = {};
        flight._mailboxSlot = 0;
        flight._mailboxGeneration = 0;
        flight._state = FlightState::Free;
    }
}

bool AppWindow::CanRender() const noexcept {
    std::unique_lock<std::mutex> lock{_stateMutex, std::defer_lock};
    if (_app->_multiThreaded) lock.lock();

    if (_pendingRecreate) {
        return false;
    }
    if (!_window->IsValid() || _window->ShouldClose() || _window->IsMinimized()) {
        return false;
    }
    const WindowVec2i size = _window->GetSize();
    return size.X > 0 &&
           size.Y > 0 &&
           _surface->IsValid() &&
           !_flights.empty() &&
           !_mailboxes.empty();
}

void Application::OnShutdown() {
    _windows.clear();
    _gpu.reset();
}

int32_t Application::Run(int argc, char* argv[]) {
    RADRAY_UNUSED(argc);
    RADRAY_UNUSED(argv);

    try {
        _exitRequested = false;
        _pendingMultiThreaded = _multiThreaded;
        this->OnInitialize();
        this->ApplyPendingThreadMode();

        while (!_exitRequested.load()) {
            this->DispatchWindowEvents();
            this->CheckWindowStates();
            this->HandleWindowChanges();
            if (_exitRequested.load()) {
                break;
            }

            this->OnUpdate();
            this->ApplyPendingThreadMode();
            this->HandleWindowChanges();
            if (_exitRequested.load()) {
                break;
            }

            if (_gpu != nullptr) {
                _gpu->ProcessTasks();
            }

            if (_multiThreaded) {
                this->ScheduleFramesMultiThreaded();
            } else {
                this->ScheduleFramesSingleThreaded();
            }

            if (_gpu != nullptr) {
                _gpu->ProcessTasks();
            }
        }

        this->StopRenderThread();
        this->WaitWindowTasks();
        this->OnShutdown();
        return 0;
    } catch (const std::exception& ex) {
        RADRAY_ERR_LOG("Application::Run failed: {}", ex.what());
        this->StopRenderThread();
        this->WaitWindowTasks();
        this->OnShutdown();
        return -1;
    } catch (...) {
        RADRAY_ERR_LOG("Application::Run failed with unknown exception");
        this->StopRenderThread();
        this->WaitWindowTasks();
        this->OnShutdown();
        return -1;
    }
}

void Application::CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, std::optional<render::VulkanInstanceDescriptor> vkInsDesc) {
    if (_gpu != nullptr) {
        throw AppException("GpuRuntime already created");
    }

    unique_ptr<render::InstanceVulkan> vkInstance{};
    if (std::holds_alternative<render::VulkanDeviceDescriptor>(deviceDesc)) {
        if (!vkInsDesc.has_value()) {
            throw AppException("VulkanInstanceDescriptor is required for Vulkan GpuRuntime");
        }
        auto instance = render::CreateVulkanInstance(*vkInsDesc);
        if (!instance.HasValue()) {
            throw AppException("CreateVulkanInstance failed");
        }
        vkInstance = instance.Release();
    }

    this->CreateGpuRuntime(deviceDesc, std::move(vkInstance));
}

void Application::CreateGpuRuntime(const render::DeviceDescriptor& deviceDesc, unique_ptr<render::InstanceVulkan> vkIns) {
    if (_gpu != nullptr) {
        throw AppException("GpuRuntime already created");
    }

    auto device = render::CreateDevice(deviceDesc);
    if (!device.HasValue()) {
        throw AppException("CreateDevice failed");
    }
    _gpu = make_unique<GpuRuntime>(device.Release(), std::move(vkIns));
}

AppWindowHandle Application::CreateWindow(
    const NativeWindowCreateDescriptor& windowDesc,
    const GpuSurfaceDescriptor& surfaceDesc,
    bool isPrimary,
    uint32_t mailboxCount) {
    if (_gpu == nullptr || !_gpu->IsValid()) {
        throw AppException("GpuRuntime is not created");
    }
    if (mailboxCount == 0) {
        throw AppException("mailboxCount must be greater than zero");
    }

    const bool resumeRenderThread = _multiThreaded;
    if (resumeRenderThread) {
        this->PauseRenderThread();
    }

    auto nativeWindow = CreateNativeWindow(windowDesc);
    if (!nativeWindow.HasValue()) {
        throw AppException("CreateNativeWindow failed");
    }

    unique_ptr<NativeWindow> window = nativeWindow.Release();
    WindowVec2i windowSize = window->GetSize();
    GpuSurfaceDescriptor desc = surfaceDesc;
    desc.NativeHandler = window->GetNativeHandler().Handle;
    if (desc.Width == 0 && windowSize.X > 0) {
        desc.Width = static_cast<uint32_t>(windowSize.X);
    }
    if (desc.Height == 0 && windowSize.Y > 0) {
        desc.Height = static_cast<uint32_t>(windowSize.Y);
    }

    auto surface = _gpu->CreateSurface(desc);
    if (surface == nullptr || !surface->IsValid()) {
        throw AppException("GpuRuntime::CreateSurface failed");
    }

    auto appWindow = make_unique<AppWindow>();
    appWindow->_app = this;
    appWindow->_selfHandle = AppWindowHandle{_windowIdCounter++};
    appWindow->_window = std::move(window);
    appWindow->_surface = std::move(surface);
    appWindow->_flights.resize(appWindow->_surface->GetFlightFrameCount());
    appWindow->_mailboxes.resize(mailboxCount);
    appWindow->_channel._queueCapacity = appWindow->_flights.size();
    appWindow->_channel._completed = false;
    appWindow->_isPrimary = isPrimary;
    appWindow->_pendingRecreate = false;

    const AppWindowHandle handle = appWindow->_selfHandle;
    _windows.emplace_back(std::move(appWindow));

    if (resumeRenderThread) {
        this->ResumeRenderThread();
    }
    return handle;
}

void Application::DispatchWindowEvents() {
    for (const auto& window : _windows) {
        if (window != nullptr && window->_window != nullptr && window->_window->IsValid()) {
            window->_window->DispatchEvents();
        }
    }
}

void Application::CheckWindowStates() {
    for (const auto& window : _windows) {
        if (window == nullptr || window->_window == nullptr) {
            continue;
        }
        if (window->_isPrimary && window->_window->ShouldClose()) {
            _exitRequested = true;
        }
        if (window->_surface == nullptr || !window->_surface->IsValid()) {
            continue;
        }
        if (window->_window->IsMinimized()) {
            continue;
        }

        const WindowVec2i size = window->_window->GetSize();
        if (size.X <= 0 || size.Y <= 0) {
            continue;
        }
        if (window->_surface->GetWidth() == static_cast<uint32_t>(size.X) &&
            window->_surface->GetHeight() == static_cast<uint32_t>(size.Y)) {
            continue;
        }

        std::unique_lock<std::mutex> lock{window->_stateMutex, std::defer_lock};
        if (_multiThreaded) {
            lock.lock();
        }
        window->_pendingRecreate = true;
    }
}

void Application::HandleWindowChanges() {
    bool hasRecreateWork = false;
    for (const auto& window : _windows) {
        if (window == nullptr || window->_window == nullptr || window->_surface == nullptr) {
            continue;
        }

        bool pending = false;
        {
            std::unique_lock<std::mutex> lock{window->_stateMutex, std::defer_lock};
            if (_multiThreaded) {
                lock.lock();
            }
            pending = window->_pendingRecreate;
        }
        if (!pending || window->_window->IsMinimized()) {
            continue;
        }

        const WindowVec2i size = window->_window->GetSize();
        if (size.X > 0 && size.Y > 0) {
            hasRecreateWork = true;
            break;
        }
    }

    if (!hasRecreateWork) {
        return;
    }

    const bool resumeRenderThread = _multiThreaded;
    if (resumeRenderThread) {
        this->PauseRenderThread();
    }

    this->WaitWindowTasks();

    for (const auto& window : _windows) {
        if (window == nullptr || window->_window == nullptr || window->_surface == nullptr) {
            continue;
        }

        bool pending = false;
        {
            std::unique_lock<std::mutex> lock{window->_stateMutex, std::defer_lock};
            if (_multiThreaded) {
                lock.lock();
            }
            pending = window->_pendingRecreate;
        }
        if (!pending || window->_window->IsMinimized()) {
            continue;
        }

        const WindowVec2i size = window->_window->GetSize();
        if (size.X <= 0 || size.Y <= 0) {
            continue;
        }

        const auto oldDesc = window->_surface->GetDesc();
        GpuSurfaceDescriptor newDesc{};
        newDesc.NativeHandler = window->_window->GetNativeHandler().Handle;
        newDesc.Width = static_cast<uint32_t>(size.X);
        newDesc.Height = static_cast<uint32_t>(size.Y);
        newDesc.BackBufferCount = oldDesc.BackBufferCount;
        newDesc.FlightFrameCount = oldDesc.FlightFrameCount;
        newDesc.Format = oldDesc.Format;
        newDesc.PresentMode = oldDesc.PresentMode;
        newDesc.QueueSlot = window->_surface->GetQueueSlot();

        window->ResetMailboxes();
        window->_surface.reset();
        window->_surface = _gpu->CreateSurface(newDesc);
        if (window->_surface == nullptr || !window->_surface->IsValid()) {
            throw AppException("GpuRuntime::CreateSurface failed during recreate");
        }
        window->_flights.clear();
        window->_flights.resize(window->_surface->GetFlightFrameCount());
        {
            std::lock_guard<std::mutex> channelLock{window->_channel._mutex};
            window->_channel._queue.clear();
            window->_channel._queueCapacity = window->_flights.size();
            window->_channel._completed = false;
        }
        {
            std::unique_lock<std::mutex> lock{window->_stateMutex, std::defer_lock};
            if (_multiThreaded) {
                lock.lock();
            }
            window->_pendingRecreate = false;
        }
    }

    if (resumeRenderThread) {
        this->ResumeRenderThread();
    }
}

void Application::WaitWindowTasks() {
    if (_gpu == nullptr) {
        return;
    }

    for (const auto& window : _windows) {
        if (window == nullptr || window->_surface == nullptr || !window->_surface->IsValid()) {
            continue;
        }
        _gpu->Wait(render::QueueType::Direct, window->_surface->GetQueueSlot());
    }
    _gpu->ProcessTasks();
    for (const auto& window : _windows) {
        if (window != nullptr) {
            window->CollectCompletedFlightSlots();
        }
    }
}

void Application::ScheduleFramesSingleThreaded() {
    if (_gpu == nullptr) {
        return;
    }

    for (const auto& window : _windows) {
        if (window != nullptr) {
            window->CollectCompletedFlightSlots();
        }
    }

    for (const auto& window : _windows) {
        if (window == nullptr || !window->CanRender()) {
            continue;
        }

        auto mailboxSlot = window->AllocMailboxSlot();
        if (!mailboxSlot.has_value()) {
            continue;
        }
        this->OnPrepareRender(window->_selfHandle, *mailboxSlot);
        window->PublishPreparedMailbox(*mailboxSlot);
        window->TryQueueLatestPublished();
    }

    for (const auto& window : _windows) {
        if (window == nullptr) {
            continue;
        }

        bool canUseSurface = false;
        {
            canUseSurface = window->_surface != nullptr && window->_surface->IsValid() && !window->_pendingRecreate;
        }
        if (!canUseSurface) {
            continue;
        }

        bool hasQueuedRequest = false;
        {
            std::lock_guard<std::mutex> channelLock{window->_channel._mutex};
            hasQueuedRequest = !window->_channel._queue.empty();
        }
        if (!hasQueuedRequest) {
            continue;
        }

        GpuRuntime::BeginFrameResult begin{};
        if (_allowFrameDrop) {
            begin = _gpu->TryBeginFrame(window->_surface.get());
        } else {
            begin = _gpu->BeginFrame(window->_surface.get());
        }

        if (begin.Status == render::SwapChainStatus::RetryLater) {
            auto dropped = window->TryClaimQueuedRenderRequest();
            if (dropped.has_value()) {
                window->ReleaseMailbox(*dropped);
            }
            continue;
        }
        if (begin.Status == render::SwapChainStatus::RequireRecreate) {
            window->_pendingRecreate = true;
            continue;
        }
        if (begin.Status != render::SwapChainStatus::Success || begin.Context == nullptr) {
            throw AppException(fmt::format("BeginFrame failed with status {}", begin.Status));
        }

        auto request = window->TryClaimQueuedRenderRequest();
        if (!request.has_value()) {
            auto abandon = _gpu->AbandonFrame(begin.Context.Release());
            RADRAY_UNUSED(abandon);
            continue;
        }

        if (begin.Context->_frameSlotIndex != request->FlightSlot) {
            auto abandon = _gpu->AbandonFrame(begin.Context.Release());
            window->ReleaseMailbox(*request);
            RADRAY_UNUSED(abandon);
            RADRAY_ASSERT(false);
            continue;
        }

        this->OnRender(window->_selfHandle, begin.Context.Get(), request->MailboxSlot);

        GpuRuntime::SubmitFrameResult submit{};
        if (begin.Context->IsEmpty()) {
            submit = _gpu->AbandonFrame(begin.Context.Release());
        } else {
            submit = _gpu->SubmitFrame(begin.Context.Release());
        }

        if (submit.Present.Status == render::SwapChainStatus::RequireRecreate) {
            window->_pendingRecreate = true;
        } else if (submit.Present.Status != render::SwapChainStatus::Success) {
            throw AppException(fmt::format("Present failed with status {}", submit.Present.Status));
        }
        window->EndPrepareRenderTask(*request, std::move(submit.Task));
    }
}

void Application::ScheduleFramesMultiThreaded() {
    if (_gpu == nullptr) {
        return;
    }

    bool queuedAny = false;
    for (const auto& window : _windows) {
        if (window != nullptr) {
            window->CollectCompletedFlightSlots();
        }
    }

    for (const auto& window : _windows) {
        if (window == nullptr || !window->CanRender()) {
            continue;
        }

        auto mailboxSlot = window->AllocMailboxSlot();
        if (!mailboxSlot.has_value()) {
            continue;
        }
        this->OnPrepareRender(window->_selfHandle, *mailboxSlot);
        window->PublishPreparedMailbox(*mailboxSlot);
        if (window->TryQueueLatestPublished().has_value()) {
            queuedAny = true;
        }
    }

    if (queuedAny) {
        _renderWakeCV.notify_one();
    }
}

void Application::RenderThreadImpl() {
    for (;;) {
        {
            std::unique_lock<std::mutex> wakeLock{_renderWakeMutex};
            if (_renderPauseRequested && !_renderStopRequested) {
                _renderPaused = true;
                _pauseAckCV.notify_all();
                _renderWakeCV.wait(wakeLock, [this] {
                    return _renderStopRequested || !_renderPauseRequested;
                });
                _renderPaused = false;
                _pauseAckCV.notify_all();
            }
            if (_renderStopRequested) {
                break;
            }
            _renderWakeCV.wait(wakeLock, [this] {
                if (_renderStopRequested || _renderPauseRequested) {
                    return true;
                }
                for (const auto& window : _windows) {
                    if (window == nullptr) {
                        continue;
                    }
                    std::lock_guard<std::mutex> channelLock{window->_channel._mutex};
                    if (!window->_channel._queue.empty()) {
                        return true;
                    }
                }
                return false;
            });
            if (_renderStopRequested) {
                break;
            }
            if (_renderPauseRequested) {
                continue;
            }
        }

        bool didWork = false;
        for (const auto& window : _windows) {
            {
                std::lock_guard<std::mutex> wakeLock{_renderWakeMutex};
                if (_renderStopRequested || _renderPauseRequested) {
                    break;
                }
            }

            if (window == nullptr) {
                continue;
            }

            GpuSurface* surface = nullptr;
            {
                std::lock_guard<std::mutex> stateLock{window->_stateMutex};
                if (window->_pendingRecreate || window->_surface == nullptr || !window->_surface->IsValid()) {
                    continue;
                }
                surface = window->_surface.get();
            }

            bool hasQueuedRequest = false;
            {
                std::lock_guard<std::mutex> channelLock{window->_channel._mutex};
                hasQueuedRequest = !window->_channel._queue.empty();
            }
            if (!hasQueuedRequest) {
                continue;
            }

            GpuRuntime::BeginFrameResult begin{};
            if (_allowFrameDrop) {
                begin = _gpu->TryBeginFrame(surface);
            } else {
                begin = _gpu->BeginFrame(surface);
            }

            if (begin.Status == render::SwapChainStatus::RetryLater) {
                auto dropped = window->TryClaimQueuedRenderRequest();
                if (dropped.has_value()) {
                    window->ReleaseMailbox(*dropped);
                    didWork = true;
                }
                continue;
            }
            if (begin.Status == render::SwapChainStatus::RequireRecreate) {
                std::lock_guard<std::mutex> stateLock{window->_stateMutex};
                window->_pendingRecreate = true;
                continue;
            }
            if (begin.Status != render::SwapChainStatus::Success || begin.Context == nullptr) {
                throw AppException(fmt::format("BeginFrame failed with status {}", begin.Status));
            }

            auto request = window->TryClaimQueuedRenderRequest();
            if (!request.has_value()) {
                auto abandon = _gpu->AbandonFrame(begin.Context.Release());
                RADRAY_UNUSED(abandon);
                didWork = true;
                continue;
            }

            if (begin.Context->_frameSlotIndex != request->FlightSlot) {
                auto abandon = _gpu->AbandonFrame(begin.Context.Release());
                window->ReleaseMailbox(*request);
                RADRAY_UNUSED(abandon);
                RADRAY_ASSERT(false);
                didWork = true;
                continue;
            }

            this->OnRender(window->_selfHandle, begin.Context.Get(), request->MailboxSlot);

            GpuRuntime::SubmitFrameResult submit{};
            if (begin.Context->IsEmpty()) {
                submit = _gpu->AbandonFrame(begin.Context.Release());
            } else {
                submit = _gpu->SubmitFrame(begin.Context.Release());
            }

            if (submit.Present.Status == render::SwapChainStatus::RequireRecreate) {
                std::lock_guard<std::mutex> stateLock{window->_stateMutex};
                window->_pendingRecreate = true;
            } else if (submit.Present.Status != render::SwapChainStatus::Success) {
                throw AppException(fmt::format("Present failed with status {}", submit.Present.Status));
            }
            window->EndPrepareRenderTask(*request, std::move(submit.Task));
            didWork = true;
        }

        if (!didWork) {
            std::unique_lock<std::mutex> wakeLock{_renderWakeMutex};
            _renderWakeCV.wait_for(wakeLock, std::chrono::milliseconds{1});
        }
    }

    {
        std::lock_guard<std::mutex> wakeLock{_renderWakeMutex};
        _renderPaused = false;
    }
    _pauseAckCV.notify_all();
}

void Application::PauseRenderThread() {
    if (!_renderThread.joinable()) {
        return;
    }

    std::unique_lock<std::mutex> wakeLock{_renderWakeMutex};
    _renderPauseRequested = true;
    _renderWakeCV.notify_all();
    _pauseAckCV.wait(wakeLock, [this] {
        return _renderPaused || _renderStopRequested || !_renderThread.joinable();
    });
}

void Application::ResumeRenderThread() {
    {
        std::lock_guard<std::mutex> wakeLock{_renderWakeMutex};
        _renderPauseRequested = false;
    }
    _renderWakeCV.notify_all();
}

void Application::StopRenderThread() {
    if (!_renderThread.joinable()) {
        return;
    }

    for (const auto& window : _windows) {
        if (window != nullptr) {
            window->_channel.Stop();
        }
    }

    {
        std::lock_guard<std::mutex> wakeLock{_renderWakeMutex};
        _renderStopRequested = true;
        _renderPauseRequested = false;
    }
    _renderWakeCV.notify_all();
    _renderThread.join();

    for (const auto& window : _windows) {
        if (window == nullptr) {
            continue;
        }
        while (auto request = window->TryClaimQueuedRenderRequest()) {
            window->ReleaseMailbox(*request);
        }
    }

    {
        std::lock_guard<std::mutex> wakeLock{_renderWakeMutex};
        _renderStopRequested = false;
        _renderPauseRequested = false;
        _renderPaused = false;
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
        for (const auto& window : _windows) {
            if (window == nullptr) {
                continue;
            }
            std::lock_guard<std::mutex> channelLock{window->_channel._mutex};
            window->_channel._completed = false;
        }
        {
            std::lock_guard<std::mutex> wakeLock{_renderWakeMutex};
            _renderStopRequested = false;
            _renderPauseRequested = false;
            _renderPaused = false;
        }
        _multiThreaded = true;
        _renderThread = std::thread{&Application::RenderThreadImpl, this};
    } else {
        this->StopRenderThread();
        _multiThreaded = false;
        for (const auto& window : _windows) {
            if (window == nullptr) {
                continue;
            }
            std::lock_guard<std::mutex> channelLock{window->_channel._mutex};
            window->_channel._completed = false;
        }
    }
}

}  // namespace radray
