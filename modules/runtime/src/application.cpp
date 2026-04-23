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

bool AppWindow::CollectCompletedFlightTask(uint32_t flightSlot) noexcept {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    RADRAY_ASSERT(flightSlot < _flights.size());
    auto& flight = _flights[flightSlot];
    if (!flight._task.has_value()) {
        return false;
    }
    if (!flight._task->IsCompleted()) {
        return false;
    }

    RADRAY_ASSERT(flight._state == FlightState::InRender);
    this->ReleaseMailbox(flight._mailboxSlot);
    flight._mailboxSlot = 0;
    flight._task.reset();
    flight._state = FlightState::Free;
    return true;
}

void AppWindow::CollectCompletedFlightTasks() noexcept {
    for (uint32_t flightSlot = 0; flightSlot < _flights.size(); ++flightSlot) {
        this->CollectCompletedFlightTask(flightSlot);
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

void AppWindow::InitializeRenderThreadState() {
    RADRAY_ASSERT(_renderThreadState == nullptr);
    _renderThreadState = make_unique<RenderThreadState>();
}

void AppWindow::ResetRenderPayloadQueue() {
    if (_renderThreadState == nullptr) {
        this->InitializeRenderThreadState();
    }

    std::lock_guard lock{_renderThreadState->Mutex};
    _renderThreadState->Queue.clear();
    _renderThreadState->QueueCapacity = _flights.empty() ? 1 : _flights.size();
    _renderThreadState->Completed = false;
}

void AppWindow::CompleteRenderPayloadQueue() noexcept {
    if (_renderThreadState == nullptr) {
        return;
    }

    std::lock_guard lock{_renderThreadState->Mutex};
    _renderThreadState->Completed = true;
}

void AppWindow::DestroyRenderThreadState() noexcept {
    if (_renderThreadState == nullptr) {
        return;
    }

    RADRAY_ASSERT(_renderThreadState->Queue.empty());
    _renderThreadState.reset();
}

bool AppWindow::HasQueuedRenderPayload() const noexcept {
    if (_renderThreadState == nullptr) {
        return false;
    }

    std::lock_guard lock{_renderThreadState->Mutex};
    return !_renderThreadState->Queue.empty();
}

std::optional<AppWindow::RenderPayload> AppWindow::TryReadRenderPayload() noexcept {
    if (_renderThreadState == nullptr) {
        return std::nullopt;
    }

    std::lock_guard lock{_renderThreadState->Mutex};
    if (_renderThreadState->Queue.empty()) {
        return std::nullopt;
    }

    RenderPayload payload = _renderThreadState->Queue.front();
    _renderThreadState->Queue.pop_front();
    return payload;
}

std::optional<uint32_t> AppWindow::TryPrepareRenderMailbox() noexcept {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    if (_renderThreadState != nullptr && _renderThreadState->Completed) {
        return std::nullopt;
    }
    if (!this->CanRender()) {
        return std::nullopt;
    }
    return this->ReserveMailboxSlot();
}

void AppWindow::CancelPreparedRenderMailbox(uint32_t mailboxSlot) noexcept {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    RADRAY_ASSERT(mailboxSlot < _mailboxes.size());
    RADRAY_ASSERT(_mailboxes[mailboxSlot]._state == MailboxState::Preparing);
    this->ReleaseMailbox(mailboxSlot);
}

void AppWindow::PublishPreparedRenderMailbox(uint32_t mailboxSlot) noexcept {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    this->PublishPreparedMailbox(mailboxSlot);
}

std::optional<AppWindow::RenderPayload> AppWindow::TryQueuePublishedRenderPayload() noexcept {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    if (_renderThreadState != nullptr &&
        (_renderThreadState->Completed || _renderThreadState->Queue.size() >= _renderThreadState->QueueCapacity)) {
        return std::nullopt;
    }
    if (!this->CanRender()) {
        return std::nullopt;
    }

    const auto mailboxSlot = this->GetPublishedMailboxSlot();
    if (!mailboxSlot.has_value()) {
        return std::nullopt;
    }

    RADRAY_ASSERT(_surface != nullptr && _surface->IsValid());
    const auto nextFrameSlot = _surface->GetNextFrameSlotIndex();
    RADRAY_ASSERT(nextFrameSlot < _flights.size());

    const uint32_t flightSlot = static_cast<uint32_t>(nextFrameSlot);
    auto& flightData = _flights[flightSlot];
    if (flightData._state != FlightState::Free || flightData._task.has_value()) {
        return std::nullopt;
    }

    auto& mailbox = _mailboxes[*mailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Published);
    mailbox._state = MailboxState::Queued;
    flightData._state = FlightState::Queued;
    flightData._mailboxSlot = *mailboxSlot;

    const RenderPayload payload{flightSlot, *mailboxSlot};
    if (_renderThreadState != nullptr) {
        _renderThreadState->Queue.push_back(payload);
    }
    return payload;
}

GpuSurface* AppWindow::TryAcquireQueuedRenderPayload(RenderPayload payload) noexcept {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    RADRAY_ASSERT(payload.FlightSlot < _flights.size());
    auto& flightData = _flights[payload.FlightSlot];
    RADRAY_ASSERT(flightData._state == FlightState::Queued);
    RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
    RADRAY_ASSERT(payload.MailboxSlot < _mailboxes.size());
    auto& mailbox = _mailboxes[payload.MailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Queued);

    if (!this->CanRender()) {
        return nullptr;
    }

    RADRAY_ASSERT(_surface != nullptr && _surface->IsValid());
    const auto nextFrameSlot = _surface->GetNextFrameSlotIndex();
    RADRAY_ASSERT(nextFrameSlot < _flights.size());
    RADRAY_ASSERT(nextFrameSlot == payload.FlightSlot);
    return _surface.get();
}

void AppWindow::RestoreQueuedRenderPayload(
    RenderPayload payload,
    bool requireRecreate,
    RenderPayloadRollbackMode rollbackMode) noexcept {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    RADRAY_ASSERT(payload.FlightSlot < _flights.size());
    auto& flightData = _flights[payload.FlightSlot];
    RADRAY_ASSERT(flightData._state == FlightState::Queued);
    RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
    RADRAY_ASSERT(payload.MailboxSlot < _mailboxes.size());

    switch (rollbackMode) {
        case RenderPayloadRollbackMode::RestoreMailbox:
            this->RestoreMailbox(payload.MailboxSlot);
            break;
        case RenderPayloadRollbackMode::RestoreOrReleaseMailbox:
            this->RestoreOrReleaseMailbox(payload.MailboxSlot);
            break;
        default:
            RADRAY_ABORT("Unsupported RenderPayloadRollbackMode {}", static_cast<int>(rollbackMode));
    }
    flightData._mailboxSlot = 0;
    flightData._state = FlightState::Free;
    if (requireRecreate) {
        _pendingRecreate = true;
    }
}

void AppWindow::MarkRenderPayloadInRender(RenderPayload payload) noexcept {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    RADRAY_ASSERT(payload.FlightSlot < _flights.size());
    auto& flightData = _flights[payload.FlightSlot];
    RADRAY_ASSERT(flightData._state == FlightState::Queued);
    RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
    RADRAY_ASSERT(payload.MailboxSlot < _mailboxes.size());
    auto& mailbox = _mailboxes[payload.MailboxSlot];
    RADRAY_ASSERT(mailbox._state == MailboxState::Queued);
    mailbox._state = MailboxState::InRender;
    flightData._state = FlightState::InRender;
}

void AppWindow::ReleaseInRenderPayload(RenderPayload payload, const render::SwapChainPresentResult& presentResult) {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    RADRAY_ASSERT(payload.FlightSlot < _flights.size());
    auto& flightData = _flights[payload.FlightSlot];
    RADRAY_ASSERT(flightData._state == FlightState::InRender);
    RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
    this->ReleaseMailbox(payload.MailboxSlot);
    flightData._mailboxSlot = 0;
    flightData._state = FlightState::Free;
    this->HandlePresentResult(presentResult);
}

void AppWindow::StoreSubmittedRenderPayload(RenderPayload payload, GpuTask&& task, const render::SwapChainPresentResult& presentResult) {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    RADRAY_ASSERT(payload.FlightSlot < _flights.size());
    auto& flightData = _flights[payload.FlightSlot];
    RADRAY_ASSERT(flightData._state == FlightState::InRender);
    RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
    flightData._task.emplace(std::move(task));
    this->HandlePresentResult(presentResult);
}

void AppWindow::DrainRenderPayloadQueue() noexcept {
    if (_renderThreadState == nullptr) {
        return;
    }

    std::lock_guard lock{_renderThreadState->Mutex};
    while (!_renderThreadState->Queue.empty()) {
        const RenderPayload payload = _renderThreadState->Queue.front();
        _renderThreadState->Queue.pop_front();

        if (payload.FlightSlot >= _flights.size()) {
            continue;
        }

        auto& flightData = _flights[payload.FlightSlot];
        if (flightData._state != FlightState::Queued) {
            continue;
        }
        RADRAY_ASSERT(flightData._mailboxSlot == payload.MailboxSlot);
        if (flightData._mailboxSlot != payload.MailboxSlot) {
            continue;
        }
        if (payload.MailboxSlot >= _mailboxes.size()) {
            flightData._mailboxSlot = 0;
            flightData._state = FlightState::Free;
            continue;
        }

        auto& mailbox = _mailboxes[payload.MailboxSlot];
        RADRAY_ASSERT(mailbox._state == MailboxState::Queued);
        if (mailbox._state == MailboxState::Queued) {
            this->RestoreOrReleaseMailbox(payload.MailboxSlot);
        }
        flightData._mailboxSlot = 0;
        flightData._state = FlightState::Free;
    }
}

bool AppWindow::RefreshWindowState() {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    RADRAY_ASSERT(_window->IsValid());
    if (_window->ShouldClose()) {
        return _isPrimary;
    }
    if (_window->IsMinimized()) {
        return false;
    }
    const auto size = _window->GetSize();
    if (size.X <= 0 || size.Y <= 0) {
        return false;
    }
    if (_surface != nullptr &&
        (_surface->GetWidth() != static_cast<uint32_t>(size.X) ||
         _surface->GetHeight() != static_cast<uint32_t>(size.Y))) {
        _pendingRecreate = true;
    }
    return false;
}

bool AppWindow::CanRecreateSurface() const {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    if (!_pendingRecreate) {
        return false;
    }
    if (_window->IsMinimized()) {
        return false;
    }
    const auto size = _window->GetSize();
    return size.X > 0 && size.Y > 0;
}

void AppWindow::ReplaceSurface(unique_ptr<GpuSurface> surface, uint32_t flightFrameCount, unique_ptr<GpuSurface>& oldSurfaceToDestroy) {
    std::unique_lock<std::mutex> lock;
    if (_renderThreadState != nullptr) {
        lock = std::unique_lock{_renderThreadState->Mutex};
    }

    oldSurfaceToDestroy = std::move(_surface);
    _surface = std::move(surface);
    _flights.clear();
    _flights.resize(flightFrameCount);
    _pendingRecreate = false;
}

AppWindow::AppWindow(AppWindow&& other) noexcept
    : _selfHandle(std::exchange(other._selfHandle, {})),
      _window(std::move(other._window)),
      _surface(std::move(other._surface)),
      _flights(std::move(other._flights)),
      _mailboxes(std::move(other._mailboxes)),
      _latestPublishedMailboxSlot(std::exchange(other._latestPublishedMailboxSlot, std::nullopt)),
      _latestPublishedGeneration(std::exchange(other._latestPublishedGeneration, 0)),
      _renderThreadState(std::move(other._renderThreadState)),
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
    _renderThreadState.reset();
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
    swap(a._renderThreadState, b._renderThreadState);
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
    if (_multiThreaded) {
        appWindow.ResetRenderPayloadQueue();
    }
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
        if (window.RefreshWindowState()) {
            _exitRequested = true;
        }
    }
}

void Application::HandleSurfaceChanges() {
    RADRAY_ASSERT(_gpu && _gpu->IsValid());

    const bool hasSurfaceToRecreate = std::ranges::any_of(_windows.Values(), [](const AppWindow& window) {
        return window.CanRecreateSurface();
    });
    if (!hasSurfaceToRecreate) {
        return;
    }

    bool renderThreadPaused = false;

    if (_multiThreaded) {
        this->PauseRenderThread();
        renderThreadPaused = true;
    }

    if (_multiThreaded) {
        for (auto& window : _windows.Values()) {
            window.DrainRenderPayloadQueue();
        }
    }

    this->WaitAllFlightTasks();
    this->WaitAllSurfaceQueues();

    for (auto& window : _windows.Values()) {
        if (!window.CanRecreateSurface()) {
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
        window.ReplaceSurface(std::move(recreatedSurface), flightFrameCount, oldSurfaceToDestroy);
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

void Application::PrepareRenderMailboxes() {
    for (auto& window : _windows.Values()) {
        const auto mailboxSlot = window.TryPrepareRenderMailbox();
        if (!mailboxSlot.has_value()) {
            continue;
        }

        try {
            this->OnPrepareRender(window._selfHandle, *mailboxSlot);
        } catch (...) {
            window.CancelPreparedRenderMailbox(*mailboxSlot);
            throw;
        }

        window.PublishPreparedRenderMailbox(*mailboxSlot);
    }
}

std::optional<AppWindow::RenderPayload> Application::TrySchedulePublishedRenderPayload(AppWindow& window) {
    if (!window.CanRender()) {
        return std::nullopt;
    }

    RADRAY_ASSERT(window._surface != nullptr && window._surface->IsValid());
    const auto nextFrameSlot = window._surface->GetNextFrameSlotIndex();
    RADRAY_ASSERT(nextFrameSlot < window._flights.size());

    const uint32_t flightSlot = static_cast<uint32_t>(nextFrameSlot);
    auto& flightData = window._flights[flightSlot];
    if (flightData._task.has_value()) {
        if (_allowFrameDrop) {
            if (!flightData._task->IsCompleted()) {
                return std::nullopt;
            }
        } else {
            flightData._task->Wait();
            _gpu->ProcessTasks();
        }

        RADRAY_ASSERT(flightData._task->IsCompleted());
        const bool collected = window.CollectCompletedFlightTask(flightSlot);
        RADRAY_ASSERT(collected);
    } else if (flightData._state != AppWindow::FlightState::Free) {
        return std::nullopt;
    }

    return window.TryQueuePublishedRenderPayload();
}

void Application::RenderThreadFunc() {
    while (true) {
        {
            std::unique_lock lock{_renderWakeMutex};
            _renderWakeCV.wait(lock, [this]() noexcept {
                return _renderPauseRequested || _renderStopRequested || this->HasQueuedRenderPayload();
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

        for (auto& window : _windows.Values()) {
            const auto payload = window.TryReadRenderPayload();
            if (!payload.has_value()) {
                continue;
            }
            this->ExecuteRenderPayload(
                window,
                *payload,
                AppWindow::RenderPayloadRollbackMode::RestoreOrReleaseMailbox);
        }
    }
}

void Application::ExecuteRenderPayload(
    AppWindow& window,
    AppWindow::RenderPayload payload,
    AppWindow::RenderPayloadRollbackMode rollbackMode) {
    GpuSurface* surface = window.TryAcquireQueuedRenderPayload(payload);
    if (surface == nullptr) {
        window.RestoreQueuedRenderPayload(payload, false, rollbackMode);
        return;
    }

    GpuRuntime::BeginFrameResult begin{};
    if (_allowFrameDrop) {
        begin = _gpu->TryBeginFrame(surface);
    } else {
        begin = _gpu->BeginFrame(surface);
    }

    if (begin.Status != render::SwapChainStatus::Success) {
        window.RestoreQueuedRenderPayload(payload, begin.Status == render::SwapChainStatus::RequireRecreate, rollbackMode);

        if (begin.Status == render::SwapChainStatus::RetryLater ||
            begin.Status == render::SwapChainStatus::RequireRecreate) {
            return;
        }

        throw AppException(fmt::format(
            "Application::ExecuteRenderPayload window {} begin frame failed with status {}",
            window._selfHandle,
            begin.Status));
    }

    window.MarkRenderPayloadInRender(payload);

    RADRAY_ASSERT(begin.Context.HasValue());
    auto frameContext = begin.Context.Release();
    if (frameContext->_frameSlotIndex != payload.FlightSlot) {
        const auto acquiredFlightSlot = static_cast<uint32_t>(frameContext->_frameSlotIndex);
        auto abandon = _gpu->AbandonFrame(std::move(frameContext));
        if (abandon.Task.IsValid()) {
            abandon.Task.Wait();
            _gpu->ProcessTasks();
        }
        window.ReleaseInRenderPayload(payload, abandon.Present);
        throw AppException(fmt::format(
            "Application::ExecuteRenderPayload window {} expected flight slot {} but acquired {}",
            window._selfHandle,
            payload.FlightSlot,
            acquiredFlightSlot));
    }

    std::exception_ptr renderError{};
    try {
        this->OnRender(window._selfHandle, frameContext.get(), payload.MailboxSlot);
    } catch (const std::exception& e) {
        renderError = std::current_exception();
        RADRAY_ERR_LOG("exception during OnRender for window {}\n  {}", window._selfHandle, e.what());
    } catch (...) {
        renderError = std::current_exception();
        RADRAY_ERR_LOG("exception during OnRender for window {}\n  {}", window._selfHandle, "unknown error");
    }

    if (renderError != nullptr) [[unlikely]] {
        auto abandon = _gpu->AbandonFrame(std::move(frameContext));
        if (abandon.Task.IsValid()) {
            abandon.Task.Wait();
            _gpu->ProcessTasks();
        }
        window.ReleaseInRenderPayload(payload, abandon.Present);
        std::rethrow_exception(renderError);
    }

    auto submit = frameContext->IsEmpty()
                      ? _gpu->AbandonFrame(std::move(frameContext))
                      : _gpu->SubmitFrame(std::move(frameContext));
    window.StoreSubmittedRenderPayload(payload, std::move(submit.Task), submit.Present);
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
    }

    for (auto& window : _windows.Values()) {
        window.CompleteRenderPayloadQueue();
    }

    _pauseAckCV.notify_all();
    _renderWakeCV.notify_all();

    if (_renderThread.joinable()) {
        _renderThread.join();
    }

    for (auto& window : _windows.Values()) {
        window.DrainRenderPayloadQueue();
    }
    for (auto& window : _windows.Values()) {
        window.DestroyRenderThreadState();
    }
    _renderPaused = false;
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
        for (auto& window : _windows.Values()) {
            window.ResetRenderPayloadQueue();
        }
        _renderPauseRequested = false;
        _renderPaused = false;
        _renderStopRequested = false;
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

    this->PrepareRenderMailboxes();

    for (auto& window : _windows.Values()) {
        const auto payload = this->TrySchedulePublishedRenderPayload(window);
        if (!payload.has_value()) {
            continue;
        }

        this->ExecuteRenderPayload(
            window,
            *payload,
            AppWindow::RenderPayloadRollbackMode::RestoreMailbox);
    }
}

void Application::ScheduleFramesMultiThreaded() {
    bool queuedAny = false;
    for (auto& window : _windows.Values()) {
        window.CollectCompletedFlightTasks();
    }

    this->PrepareRenderMailboxes();

    for (auto& window : _windows.Values()) {
        if (this->TrySchedulePublishedRenderPayload(window).has_value()) {
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

bool Application::HasQueuedRenderPayload() const noexcept {
    for (const auto& window : _windows.Values()) {
        if (window.HasQueuedRenderPayload()) {
            return true;
        }
    }
    return false;
}

}  // namespace radray
