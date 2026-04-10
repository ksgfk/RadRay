#include <radray/runtime/application.h>

#include <algorithm>
#include <exception>

#include <radray/logger.h>
#include <radray/window/native_window.h>

namespace radray {

namespace {

bool _IsSurfaceConfigValid(const AppSurfaceConfig& config, bool allowPrimary, const char* source) {
    if (config.Window == nullptr) {
        RADRAY_ERR_LOG("Application: {} requires a non-null window", source);
        return false;
    }
    if (!config.Window->IsValid()) {
        RADRAY_ERR_LOG("Application: {} requires a valid window", source);
        return false;
    }
    if (config.BackBufferCount == 0) {
        RADRAY_ERR_LOG("Application: {} requires BackBufferCount > 0", source);
        return false;
    }
    if (config.FlightFrameCount == 0) {
        RADRAY_ERR_LOG("Application: {} requires FlightFrameCount > 0", source);
        return false;
    }
    if (!allowPrimary && config.IsPrimary) {
        RADRAY_ERR_LOG("Application: {} does not allow creating another primary surface", source);
        return false;
    }
    return true;
}

void _LogCallbackException(const char* callbackName, const std::exception& ex) {
    RADRAY_ERR_LOG("Application: IAppCallbacks::{} threw: {}", callbackName, ex.what());
}

void _LogUnknownCallbackException(const char* callbackName) {
    RADRAY_ERR_LOG("Application: IAppCallbacks::{} threw an unknown exception", callbackName);
}

}  // namespace

Application::Application(AppConfig config, IAppCallbacks* callbacks)
    : _config(config),
      _callbacks(callbacks) {}

Application::~Application() noexcept {
    StopRenderThread();
}

int Application::Run() {
    auto cleanup = [this]() {
        WaitRenderIdle();
        DrainRenderCompletions();
        StopRenderThread();
        _mainThreadCommands.clear();
        _renderQueue.clear();
        _renderCompletions.clear();
        _renderWorkerBusy = false;
        _surfaces.clear();
        if (_gpu != nullptr) {
            std::lock_guard lock(_gpuMutex);
            _gpu->Destroy();
            _gpu.reset();
        }
        _primarySurfaceId = AppSurfaceId::Invalid();
        _primaryConfiguredPresentMode = render::PresentMode::FIFO;
        _scheduleCursor = 0;
        _multiThreaded = false;
        _callbackFailed = false;
    };

    vector<AppSurfaceConfig> initialSurfaces{};
    if (!BuildInitialSurfaceConfigs(initialSurfaces)) {
        cleanup();
        return 1;
    }

    if (!CreateRuntime()) {
        cleanup();
        return 1;
    }

    if (!CreateInitialSurfaces(initialSurfaces)) {
        cleanup();
        return 1;
    }

    _allowFrameDrop = _config.AllowFrameDrop;
    _multiThreaded = false;
    if (_config.MultiThreadedRender) {
        SwitchThreadMode(true);
    }

    try {
        _callbacks->OnStartup(this);
    } catch (const std::exception& ex) {
        _LogCallbackException("OnStartup", ex);
        _callbackFailed = true;
        _exitRequested = true;
    } catch (...) {
        _LogUnknownCallbackException("OnStartup");
        _callbackFailed = true;
        _exitRequested = true;
    }
    _timer.Restart();

    while (!_exitRequested) {
        DrainRenderCompletions();
        {
            std::lock_guard lock(_gpuMutex);
            _gpu->ProcessTasks();
        }

        if (auto* dispatchWindow = GetEventDispatchWindow(); dispatchWindow != nullptr) {
            dispatchWindow->DispatchEvents();
        }
        CaptureWindowState();
        if (_exitRequested) {
            break;
        }

        auto elapsed = _timer.Elapsed();
        _timer.Restart();
        float dt = std::chrono::duration<float>(elapsed).count();
        _frameInfo.DeltaTime = dt;
        _frameInfo.TotalTime += dt;
        _frameInfo.LogicFrameIndex++;

        try {
            _callbacks->OnUpdate(this, dt);
        } catch (const std::exception& ex) {
            _LogCallbackException("OnUpdate", ex);
            _callbackFailed = true;
            _exitRequested = true;
        } catch (...) {
            _LogUnknownCallbackException("OnUpdate");
            _callbackFailed = true;
            _exitRequested = true;
        }

        DrainRenderCompletions();
        {
            std::lock_guard lock(_gpuMutex);
            _gpu->ProcessTasks();
        }

        HandlePendingChanges();

        DrainRenderCompletions();
        {
            std::lock_guard lock(_gpuMutex);
            _gpu->ProcessTasks();
        }

        if (_exitRequested) {
            break;
        }
        if (!HasActiveSurfaces()) {
            _exitRequested = true;
            break;
        }

        const bool scheduledAny = ScheduleSurfaceFrames();
        if (!_surfaces.empty()) {
            _scheduleCursor = (_scheduleCursor + 1) % _surfaces.size();
        } else {
            _scheduleCursor = 0;
        }
        {
            std::lock_guard lock(_gpuMutex);
            _gpu->ProcessTasks();
        }
        if (_multiThreaded && !scheduledAny && HasOutstandingSurfaceJobs()) {
            WaitForRenderProgress();
        }
    }

    WaitRenderIdle();
    DrainRenderCompletions();
    StopRenderThread();

    if (_gpu != nullptr && HasActiveSurfaces()) {
        std::lock_guard lock(_gpuMutex);
        _gpu->Wait(render::QueueType::Direct, 0);
    }
    if (_gpu != nullptr) {
        std::lock_guard lock(_gpuMutex);
        _gpu->ProcessTasks();
    }

    try {
        _callbacks->OnShutdown(this);
    } catch (const std::exception& ex) {
        _LogCallbackException("OnShutdown", ex);
        _callbackFailed = true;
    } catch (...) {
        _LogUnknownCallbackException("OnShutdown");
        _callbackFailed = true;
    }

    const int exitCode = _callbackFailed ? 1 : 0;
    cleanup();
    return exitCode;
}

void Application::SetMultiThreadedRender(bool enable) {
    if (auto* pending = FindQueuedThreadModeSwitchCommand(); pending != nullptr) {
        pending->Enable = enable;
        return;
    }

    _mainThreadCommands.emplace_back(SwitchThreadModeCommand{.Enable = enable});
}

void Application::SetAllowFrameDrop(bool enable) {
    _allowFrameDrop = enable;
}

AppSurfaceId Application::RequestAddSurface(AppSurfaceConfig config) {
    if (!_IsSurfaceConfigValid(config, false, "RequestAddSurface")) {
        return AppSurfaceId::Invalid();
    }

    for (const auto& surface : _surfaces) {
        if (!surface.Active) {
            continue;
        }
        if (surface.Window == config.Window) {
            RADRAY_ERR_LOG("Application: RequestAddSurface cannot track the same window twice");
            return AppSurfaceId::Invalid();
        }
    }
    if (HasQueuedAddForWindow(config.Window)) {
        RADRAY_ERR_LOG("Application: RequestAddSurface already has a pending request for this window");
        return AppSurfaceId::Invalid();
    }

    config.IsPrimary = false;
    AddSurfaceCommand command{};
    command.SurfaceId = AllocateSurfaceId();
    command.Config = config;
    const auto surfaceId = command.SurfaceId;
    _mainThreadCommands.emplace_back(std::move(command));
    return surfaceId;
}

void Application::RequestRemoveSurface(AppSurfaceId surfaceId) {
    if (!surfaceId.IsValid()) {
        return;
    }

    if (auto* command = FindQueuedAddCommand(surfaceId); command != nullptr) {
        command->Cancelled = true;
        return;
    }

    if (FindSurfaceState(surfaceId) == nullptr) {
        return;
    }

    _mainThreadCommands.emplace_back(RemoveSurfaceCommand{.SurfaceId = surfaceId});
}

void Application::RequestPresentModeChange(render::PresentMode mode) {
    RequestPresentModeChange(_primarySurfaceId, mode);
}

void Application::RequestPresentModeChange(AppSurfaceId surfaceId, render::PresentMode mode) {
    if (!surfaceId.IsValid()) {
        return;
    }

    if (auto* command = FindQueuedAddCommand(surfaceId); command != nullptr) {
        command->Config.PresentMode = mode;
        return;
    }

    const auto* surface = FindSurfaceState(surfaceId);
    if (surface == nullptr) {
        return;
    }

    render::PresentMode currentMode = surface->DesiredPresentMode;
    if (surface->Surface != nullptr && surface->Surface->IsValid()) {
        currentMode = surface->Surface->GetPresentMode();
    }

    if (mode == currentMode && mode == surface->DesiredPresentMode) {
        return;
    }

    _mainThreadCommands.emplace_back(ChangePresentModeCommand{
        .SurfaceId = surfaceId,
        .PresentMode = mode,
    });
}

void Application::RequestExit() {
    _mainThreadCommands.emplace_back(RequestExitCommand{});
}

GpuRuntime* Application::GetGpuRuntime() const {
    return _gpu.get();
}

NativeWindow* Application::GetWindow() const {
    return GetWindow(_primarySurfaceId);
}

NativeWindow* Application::GetWindow(AppSurfaceId surfaceId) const {
    const auto* surface = FindSurfaceState(surfaceId);
    return surface != nullptr ? surface->Window : nullptr;
}

GpuSurface* Application::GetSurface() const {
    return GetSurface(_primarySurfaceId);
}

GpuSurface* Application::GetSurface(AppSurfaceId surfaceId) const {
    const auto* surface = FindSurfaceState(surfaceId);
    return surface != nullptr ? surface->Surface.get() : nullptr;
}

AppSurfaceId Application::GetPrimarySurfaceId() const {
    return _primarySurfaceId;
}

vector<AppSurfaceId> Application::GetSurfaceIds() const {
    vector<AppSurfaceId> ids{};
    ids.reserve(_surfaces.size());
    for (size_t slotIndex = 0; slotIndex < _surfaces.size(); ++slotIndex) {
        if (!_surfaces[slotIndex].Active) {
            continue;
        }
        ids.emplace_back(AppSurfaceId{static_cast<uint64_t>(slotIndex)});
    }
    return ids;
}

bool Application::HasSurface(AppSurfaceId surfaceId) const {
    return FindSurfaceState(surfaceId) != nullptr;
}

const AppFrameInfo& Application::GetFrameInfo() const {
    return _frameInfo;
}

bool Application::IsMultiThreadedRender() const {
    return _multiThreaded;
}

bool Application::IsFrameDropEnabled() const {
    return _allowFrameDrop;
}

render::PresentMode Application::GetCurrentPresentMode() const {
    const auto* surface = FindSurfaceState(_primarySurfaceId);
    if (surface != nullptr && surface->Surface != nullptr && surface->Surface->IsValid()) {
        return surface->Surface->GetPresentMode();
    }
    return _primaryConfiguredPresentMode;
}

bool Application::BuildInitialSurfaceConfigs(vector<AppSurfaceConfig>& configs) {
    configs.clear();
    _primarySurfaceId = AppSurfaceId::Invalid();
    _primaryConfiguredPresentMode = render::PresentMode::FIFO;

    if (_config.InitialSurfaces.empty()) {
        RADRAY_ERR_LOG("Application: InitialSurfaces requires at least one surface");
        return false;
    }
    configs = _config.InitialSurfaces;

    size_t primaryCount = 0;
    unordered_set<NativeWindow*> seenWindows{};
    for (const auto& config : configs) {
        if (!_IsSurfaceConfigValid(config, true, "AppConfig::InitialSurfaces")) {
            return false;
        }
        if (!seenWindows.emplace(config.Window).second) {
            RADRAY_ERR_LOG("Application: duplicate window in InitialSurfaces");
            return false;
        }
        if (config.IsPrimary) {
            primaryCount++;
            _primaryConfiguredPresentMode = config.PresentMode;
        }
    }

    if (primaryCount != 1) {
        RADRAY_ERR_LOG("Application: InitialSurfaces requires exactly one primary surface");
        return false;
    }
    return true;
}

bool Application::CreateRuntime() {
    bool created = false;
    switch (_config.Backend) {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
        case render::RenderBackend::D3D12: {
            render::D3D12DeviceDescriptor desc{};
            desc.AdapterIndex = std::nullopt;
#ifdef RADRAY_IS_DEBUG
            desc.IsEnableDebugLayer = true;
            desc.IsEnableGpuBasedValid = true;
#endif
            auto opt = GpuRuntime::Create(desc);
            if (opt.HasValue()) {
                _gpu = opt.Release();
                created = true;
            }
            break;
        }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        case render::RenderBackend::Vulkan: {
            render::VulkanInstanceDescriptor instanceDesc{};
            instanceDesc.AppName = "RadRay";
            instanceDesc.AppVersion = 1;
            instanceDesc.EngineName = "RadRay";
            instanceDesc.EngineVersion = 1;
#ifdef RADRAY_IS_DEBUG
            instanceDesc.IsEnableDebugLayer = true;
#endif
            render::VulkanCommandQueueDescriptor queueDesc{};
            queueDesc.Type = render::QueueType::Direct;
            queueDesc.Count = 1;
            render::VulkanDeviceDescriptor deviceDesc{};
            deviceDesc.PhysicalDeviceIndex = std::nullopt;
            deviceDesc.Queues = std::span{&queueDesc, 1};
            auto opt = GpuRuntime::Create(deviceDesc, instanceDesc);
            if (opt.HasValue()) {
                _gpu = opt.Release();
                created = true;
            }
            break;
        }
#endif
        default:
            break;
    }

    if (!created) {
        RADRAY_ERR_LOG("Application: failed to create GpuRuntime for backend {}", static_cast<int>(_config.Backend));
    }
    return created;
}

bool Application::CreateInitialSurfaces(const vector<AppSurfaceConfig>& configs) {
    _surfaces.clear();
    _surfaces.resize(configs.size());
    for (size_t slotIndex = 0; slotIndex < configs.size(); ++slotIndex) {
        const AppSurfaceId surfaceId{static_cast<uint64_t>(slotIndex)};
        const auto& config = configs[slotIndex];
        if (!CreateSurfaceFromConfig(surfaceId, config, false)) {
            return false;
        }
    }
    return true;
}

bool Application::CreateSurfaceFromConfig(AppSurfaceId surfaceId, const AppSurfaceConfig& config, bool notifyAdded) {
    if (_gpu == nullptr) {
        RADRAY_ERR_LOG("Application: cannot create surface without a valid runtime");
        return false;
    }
    if (!surfaceId.IsValid()) {
        RADRAY_ERR_LOG("Application: cannot create surface with invalid id");
        return false;
    }
    if (!_IsSurfaceConfigValid(config, true, "CreateSurfaceFromConfig")) {
        return false;
    }

    const size_t slotIndex = static_cast<size_t>(surfaceId.Handle);
    if (slotIndex >= _surfaces.size()) {
        _surfaces.resize(slotIndex + 1);
    }

    auto& surface = _surfaces[slotIndex];
    if (surface.Active) {
        RADRAY_ERR_LOG("Application: surface slot {} is already active", surfaceId);
        return false;
    }

    surface = SurfaceState{};
    surface.Active = true;
    surface.Window = config.Window;
    surface.SurfaceFormat = config.SurfaceFormat;
    surface.DesiredPresentMode = config.PresentMode;
    surface.BackBufferCount = config.BackBufferCount;
    surface.FlightFrameCount = config.FlightFrameCount;
    surface.IsPrimary = config.IsPrimary;

    auto size = config.Window->GetSize();
    const uint32_t width = size.X > 0 ? static_cast<uint32_t>(size.X) : 1;
    const uint32_t height = size.Y > 0 ? static_cast<uint32_t>(size.Y) : 1;
    {
        std::lock_guard lock(_gpuMutex);
        surface.Surface = _gpu->CreateSurface(
            config.Window->GetNativeHandler().Handle,
            width,
            height,
            config.BackBufferCount,
            config.FlightFrameCount,
            config.SurfaceFormat,
            config.PresentMode,
            0);
    }
    if (surface.Surface == nullptr || !surface.Surface->IsValid()) {
        RADRAY_ERR_LOG("Application: created invalid surface for surface {}", surfaceId);
        return false;
    }

    if (surface.IsPrimary) {
        _primarySurfaceId = surfaceId;
        _primaryConfiguredPresentMode = config.PresentMode;
    }

    if (notifyAdded) {
        try {
            _callbacks->OnSurfaceAdded(this, surfaceId, surface.Surface.get());
        } catch (const std::exception& ex) {
            _LogCallbackException("OnSurfaceAdded", ex);
            _callbackFailed = true;
            _exitRequested = true;
        } catch (...) {
            _LogUnknownCallbackException("OnSurfaceAdded");
            _callbackFailed = true;
            _exitRequested = true;
        }
    }
    return true;
}

AppSurfaceId Application::AllocateSurfaceId() const {
    for (size_t slotIndex = 0; slotIndex < _surfaces.size(); ++slotIndex) {
        const AppSurfaceId candidate{static_cast<uint64_t>(slotIndex)};
        if (IsSurfaceIdReservedByQueuedAdd(candidate)) {
            continue;
        }

        const auto& surface = _surfaces[slotIndex];
        if (!surface.Active) {
            return candidate;
        }
        if (!surface.IsPrimary && (surface.PendingRemoval || HasQueuedRemoveForSurface(candidate))) {
            return candidate;
        }
    }

    size_t slotIndex = _surfaces.size();
    for (; IsSurfaceIdReservedByQueuedAdd(AppSurfaceId{static_cast<uint64_t>(slotIndex)}); ++slotIndex) {
    }
    return AppSurfaceId{static_cast<uint64_t>(slotIndex)};
}

Application::SurfaceState* Application::FindSurfaceState(AppSurfaceId surfaceId) {
    if (!surfaceId.IsValid()) {
        return nullptr;
    }
    const size_t slotIndex = static_cast<size_t>(surfaceId.Handle);
    if (slotIndex >= _surfaces.size()) {
        return nullptr;
    }
    auto& surface = _surfaces[slotIndex];
    return surface.Active ? &surface : nullptr;
}

const Application::SurfaceState* Application::FindSurfaceState(AppSurfaceId surfaceId) const {
    if (!surfaceId.IsValid()) {
        return nullptr;
    }
    const size_t slotIndex = static_cast<size_t>(surfaceId.Handle);
    if (slotIndex >= _surfaces.size()) {
        return nullptr;
    }
    const auto& surface = _surfaces[slotIndex];
    return surface.Active ? &surface : nullptr;
}

Application::AddSurfaceCommand* Application::FindQueuedAddCommand(AppSurfaceId surfaceId) {
    for (auto& command : _mainThreadCommands) {
        auto* add = std::get_if<AddSurfaceCommand>(&command);
        if (add != nullptr && !add->Cancelled && add->SurfaceId == surfaceId) {
            return add;
        }
    }
    return nullptr;
}

bool Application::HasQueuedAddForWindow(NativeWindow* window) const {
    for (const auto& command : _mainThreadCommands) {
        const auto* add = std::get_if<AddSurfaceCommand>(&command);
        if (add != nullptr && !add->Cancelled && add->Config.Window == window) {
            return true;
        }
    }
    return false;
}

bool Application::HasQueuedRemoveForSurface(AppSurfaceId surfaceId) const {
    for (const auto& command : _mainThreadCommands) {
        const auto* remove = std::get_if<RemoveSurfaceCommand>(&command);
        if (remove != nullptr && remove->SurfaceId == surfaceId) {
            return true;
        }
    }
    return false;
}

bool Application::IsSurfaceIdReservedByQueuedAdd(AppSurfaceId surfaceId) const {
    for (const auto& command : _mainThreadCommands) {
        const auto* add = std::get_if<AddSurfaceCommand>(&command);
        if (add != nullptr && !add->Cancelled && add->SurfaceId == surfaceId) {
            return true;
        }
    }
    return false;
}

Application::SwitchThreadModeCommand* Application::FindQueuedThreadModeSwitchCommand() {
    for (auto& command : _mainThreadCommands) {
        auto* switchCommand = std::get_if<SwitchThreadModeCommand>(&command);
        if (switchCommand != nullptr) {
            return switchCommand;
        }
    }
    return nullptr;
}

NativeWindow* Application::GetEventDispatchWindow() const {
    const auto* primary = FindSurfaceState(_primarySurfaceId);
    if (primary != nullptr && primary->Window != nullptr && primary->Window->IsValid()) {
        return primary->Window;
    }

    for (const auto& surface : _surfaces) {
        if (!surface.Active) {
            continue;
        }
        if (surface.Window != nullptr && surface.Window->IsValid()) {
            return surface.Window;
        }
    }
    return nullptr;
}

bool Application::HasActiveSurfaces() const {
    return std::any_of(
        _surfaces.begin(),
        _surfaces.end(),
        [](const SurfaceState& surface) { return surface.Active; });
}

void Application::CaptureWindowState() {
    for (auto& surface : _surfaces) {
        if (!surface.Active) {
            continue;
        }
        if (surface.Window == nullptr || !surface.Window->IsValid() || surface.Window->ShouldClose()) {
            surface.Closing = true;
            if (surface.IsPrimary) {
                _exitRequested = true;
            } else {
                surface.PendingRemoval = true;
            }
            continue;
        }

        if (surface.PendingRemoval) {
            continue;
        }

        auto size = surface.Window->GetSize();
        if (size.X <= 0 || size.Y <= 0) {
            continue;
        }

        const uint32_t width = static_cast<uint32_t>(size.X);
        const uint32_t height = static_cast<uint32_t>(size.Y);
        const uint32_t currentWidth = surface.Surface != nullptr && surface.Surface->IsValid() ? surface.Surface->GetWidth() : 0;
        const uint32_t currentHeight = surface.Surface != nullptr && surface.Surface->IsValid() ? surface.Surface->GetHeight() : 0;

        const bool sizeChanged = surface.PendingResize
                                     ? (surface.LatchedWidth != width || surface.LatchedHeight != height)
                                     : (currentWidth != width || currentHeight != height);
        if (sizeChanged) {
            surface.PendingResize = true;
            surface.LatchedWidth = width;
            surface.LatchedHeight = height;
        }
    }
}

bool Application::ScheduleSurfaceFrames() {
    if (_surfaces.empty()) {
        return false;
    }

    bool scheduledAny = false;
    const size_t surfaceCount = _surfaces.size();
    const size_t start = _scheduleCursor % surfaceCount;
    for (size_t offset = 0; offset < surfaceCount; ++offset) {
        const size_t slotIndex = (start + offset) % surfaceCount;
        auto& surface = _surfaces[slotIndex];
        if (!surface.Active) {
            continue;
        }
        if (surface.PendingRemoval || surface.PendingResize || surface.PendingRecreate || surface.Closing || surface.OutstandingFrameCount >= surface.FlightFrameCount) {
            continue;
        }
        if (surface.Window == nullptr || !surface.Window->IsValid() || surface.Window->IsMinimized()) {
            continue;
        }
        if (surface.Surface == nullptr || !surface.Surface->IsValid()) {
            continue;
        }

        const AppSurfaceId surfaceId{static_cast<uint64_t>(slotIndex)};
        if (_multiThreaded) {
            surface.RenderFrameIndex++;

            AppFrameInfo appInfo = _frameInfo;
            AppSurfaceFrameInfo surfaceInfo{};
            surfaceInfo.RenderFrameIndex = surface.RenderFrameIndex;
            surfaceInfo.DroppedFrameCount = surface.DroppedFrameCount;

            unique_ptr<FramePacket> packet{};
            try {
                packet = _callbacks->OnExtractRenderData(this, surfaceId, appInfo, surfaceInfo);
            } catch (const std::exception& ex) {
                _LogCallbackException("OnExtractRenderData", ex);
                _callbackFailed = true;
                _exitRequested = true;
                return scheduledAny;
            } catch (...) {
                _LogUnknownCallbackException("OnExtractRenderData");
                _callbackFailed = true;
                _exitRequested = true;
                return scheduledAny;
            }

            RenderSurfaceFrameCommand renderCommand{};
            renderCommand.SurfaceId = surfaceId;
            renderCommand.Surface = surface.Surface.get();
            renderCommand.AppInfo = appInfo;
            renderCommand.SurfaceInfo = surfaceInfo;
            renderCommand.Packet = std::move(packet);

            surface.OutstandingFrameCount++;
            scheduledAny = true;
            KickRenderThread(RenderThreadCommand{std::move(renderCommand)});
            continue;
        }

        GpuRuntime::BeginFrameResult begin{};
        {
            std::lock_guard lock(_gpuMutex);
            if (_allowFrameDrop) {
                begin = _gpu->TryBeginFrame(surface.Surface.get());
            } else {
                begin = _gpu->BeginFrame(surface.Surface.get());
            }
        }

        switch (begin.Status) {
            case render::SwapChainStatus::Success: {
                surface.RenderFrameIndex++;

                AppFrameInfo appInfo = _frameInfo;
                AppSurfaceFrameInfo surfaceInfo{};
                surfaceInfo.RenderFrameIndex = surface.RenderFrameIndex;
                surfaceInfo.DroppedFrameCount = surface.DroppedFrameCount;

                unique_ptr<FramePacket> packet{};
                try {
                    packet = _callbacks->OnExtractRenderData(this, surfaceId, appInfo, surfaceInfo);
                } catch (const std::exception& ex) {
                    _LogCallbackException("OnExtractRenderData", ex);
                    _callbackFailed = true;
                    _exitRequested = true;

                    render::SwapChainPresentResult presentResult{};
                    {
                        std::lock_guard lock(_gpuMutex);
                        presentResult = _gpu->AbandonFrame(begin.Context.Release()).Present;
                    }
                    HandlePresentResult(surfaceId, presentResult);
                    return scheduledAny;
                } catch (...) {
                    _LogUnknownCallbackException("OnExtractRenderData");
                    _callbackFailed = true;
                    _exitRequested = true;

                    render::SwapChainPresentResult presentResult{};
                    {
                        std::lock_guard lock(_gpuMutex);
                        presentResult = _gpu->AbandonFrame(begin.Context.Release()).Present;
                    }
                    HandlePresentResult(surfaceId, presentResult);
                    return scheduledAny;
                }

                RenderSurfaceFrameCommand renderCommand{};
                renderCommand.SurfaceId = surfaceId;
                renderCommand.Ctx = begin.Context.Release();
                renderCommand.AppInfo = appInfo;
                renderCommand.SurfaceInfo = surfaceInfo;
                renderCommand.Packet = std::move(packet);

                surface.OutstandingFrameCount++;
                scheduledAny = true;

                RenderThreadCommand command{std::move(renderCommand)};
                auto result = ExecuteRenderThreadCommand(command);
                surface.OutstandingFrameCount--;
                if (result.Completion.has_value()) {
                    if (result.Completion->CallbackFailed) {
                        _callbackFailed = true;
                        _exitRequested = true;
                    }
                    HandlePresentResult(result.Completion->SurfaceId, result.Completion->PresentResult);
                }
                break;
            }
            case render::SwapChainStatus::RetryLater:
                surface.DroppedFrameCount++;
                break;
            case render::SwapChainStatus::RequireRecreate:
                surface.PendingRecreate = true;
                break;
            case render::SwapChainStatus::Error:
            default:
                RADRAY_ERR_LOG("Application: BeginFrame returned error status {}", static_cast<int>(begin.Status));
                _exitRequested = true;
                return scheduledAny;
        }
    }
    return scheduledAny;
}

bool Application::HasOutstandingSurfaceJobs() const {
    return std::any_of(
        _surfaces.begin(),
        _surfaces.end(),
        [](const SurfaceState& surface) { return surface.Active && surface.OutstandingFrameCount != 0; });
}

void Application::HandlePendingChanges() {
    DrainMainThreadCommands();
    HandlePendingSurfaceRemovals();
    DrainMainThreadCommands();
    HandlePendingSurfaceRemovals();
    HandlePendingSurfaceRecreates();
}

void Application::DrainMainThreadCommands() {
    while (!_mainThreadCommands.empty()) {
        MainThreadCommand command = std::move(_mainThreadCommands.front());
        _mainThreadCommands.pop_front();
        if (!ExecuteMainThreadCommand(command)) {
            _mainThreadCommands.emplace_front(std::move(command));
            break;
        }
    }
}

bool Application::ExecuteMainThreadCommand(MainThreadCommand& command) {
    return std::visit([this](auto& typedCommand) { return ExecuteMainThreadCommand(typedCommand); }, command);
}

bool Application::ExecuteMainThreadCommand(AddSurfaceCommand& command) {
    if (command.Cancelled) {
        return true;
    }

    if (auto* surface = FindSurfaceState(command.SurfaceId); surface != nullptr) {
        if (!surface->PendingRemoval) {
            return true;
        }
        if (surface->OutstandingFrameCount != 0) {
            return false;
        }
        FinalizeSurfaceRemoval(command.SurfaceId, *surface);
    }

    if (!CreateSurfaceFromConfig(command.SurfaceId, command.Config, true)) {
        RADRAY_ERR_LOG("Application: failed to create runtime-added surface {}", command.SurfaceId);
    }
    return true;
}

bool Application::ExecuteMainThreadCommand(RemoveSurfaceCommand& command) {
    auto* surface = FindSurfaceState(command.SurfaceId);
    if (surface == nullptr) {
        return true;
    }

    if (surface->IsPrimary) {
        _exitRequested = true;
        return true;
    }

    surface->PendingRemoval = true;
    return true;
}

bool Application::ExecuteMainThreadCommand(ChangePresentModeCommand& command) {
    auto* surface = FindSurfaceState(command.SurfaceId);
    if (surface == nullptr) {
        return true;
    }

    render::PresentMode currentMode = surface->DesiredPresentMode;
    if (surface->Surface != nullptr && surface->Surface->IsValid()) {
        currentMode = surface->Surface->GetPresentMode();
    }

    if (command.PresentMode == currentMode && command.PresentMode == surface->DesiredPresentMode) {
        return true;
    }

    surface->DesiredPresentMode = command.PresentMode;
    surface->PendingRecreate = true;
    if (surface->IsPrimary) {
        _primaryConfiguredPresentMode = command.PresentMode;
    }
    return true;
}

bool Application::ExecuteMainThreadCommand(SwitchThreadModeCommand& command) {
    if (command.Enable == _multiThreaded) {
        return true;
    }

    WaitRenderIdle();
    DrainRenderCompletions();
    SwitchThreadMode(command.Enable);
    return true;
}

bool Application::ExecuteMainThreadCommand(RequestExitCommand& command) {
    (void)command;
    _exitRequested = true;
    return true;
}

void Application::FinalizeSurfaceRemoval(AppSurfaceId surfaceId, SurfaceState& surface) {
    NativeWindow* const window = surface.Window;
    GpuSurface* const gpuSurface = surface.Surface.get();

    if (gpuSurface != nullptr && gpuSurface->IsValid()) {
        std::lock_guard lock(_gpuMutex);
        _gpu->Wait(render::QueueType::Direct, gpuSurface->_queueSlot);
        _gpu->ProcessTasks();
    }

    surface = SurfaceState{};
    try {
        _callbacks->OnSurfaceRemoved(this, surfaceId, window);
    } catch (const std::exception& ex) {
        _LogCallbackException("OnSurfaceRemoved", ex);
        _callbackFailed = true;
        _exitRequested = true;
    } catch (...) {
        _LogUnknownCallbackException("OnSurfaceRemoved");
        _callbackFailed = true;
        _exitRequested = true;
    }
}

void Application::HandlePendingSurfaceRemovals() {
    bool removedAny = false;
    for (size_t i = 0; i < _surfaces.size(); ++i) {
        auto& surface = _surfaces[i];
        if (!surface.Active || !surface.PendingRemoval || surface.OutstandingFrameCount != 0) {
            continue;
        }

        const AppSurfaceId surfaceId{static_cast<uint64_t>(i)};
        FinalizeSurfaceRemoval(surfaceId, surface);
        removedAny = true;
    }

    if (removedAny) {
        TrimInactiveSurfaceTail();
        if (!HasActiveSurfaces()) {
            _scheduleCursor = 0;
        } else if (!_surfaces.empty()) {
            _scheduleCursor %= _surfaces.size();
        }
    }
}

void Application::TrimInactiveSurfaceTail() {
    while (!_surfaces.empty() && !_surfaces.back().Active) {
        _surfaces.pop_back();
    }
}

void Application::HandlePendingSurfaceRecreates() {
    for (size_t slotIndex = 0; slotIndex < _surfaces.size(); ++slotIndex) {
        auto& surface = _surfaces[slotIndex];
        if (!surface.Active) {
            continue;
        }
        if (surface.PendingRemoval || surface.OutstandingFrameCount != 0 || (!surface.PendingResize && !surface.PendingRecreate)) {
            continue;
        }
        if (surface.Window == nullptr || !surface.Window->IsValid()) {
            if (surface.IsPrimary) {
                _exitRequested = true;
            } else {
                surface.PendingRemoval = true;
            }
            continue;
        }
        if (surface.Surface == nullptr || !surface.Surface->IsValid()) {
            continue;
        }

        const uint32_t width = surface.PendingResize ? surface.LatchedWidth : surface.Surface->GetWidth();
        const uint32_t height = surface.PendingResize ? surface.LatchedHeight : surface.Surface->GetHeight();
        const auto mode = surface.DesiredPresentMode;

        RecreateSurface(AppSurfaceId{static_cast<uint64_t>(slotIndex)}, surface, width, height, mode);
        surface.PendingResize = false;
        surface.PendingRecreate = false;
    }
}

void Application::RecreateSurface(AppSurfaceId surfaceId, SurfaceState& surface, uint32_t width, uint32_t height, render::PresentMode mode) {
    if (surface.Surface != nullptr && surface.Surface->IsValid()) {
        std::lock_guard lock(_gpuMutex);
        _gpu->Wait(render::QueueType::Direct, surface.Surface->_queueSlot);
    }
    {
        std::lock_guard lock(_gpuMutex);
        _gpu->ProcessTasks();
    }

    surface.Surface.reset();
    {
        std::lock_guard lock(_gpuMutex);
        surface.Surface = _gpu->CreateSurface(
            surface.Window->GetNativeHandler().Handle,
            width,
            height,
            surface.BackBufferCount,
            surface.FlightFrameCount,
            surface.SurfaceFormat,
            mode,
            0);
    }

    if (surface.Surface == nullptr || !surface.Surface->IsValid()) {
        RADRAY_ERR_LOG("Application: recreated invalid surface for surface {}", surfaceId);
        _exitRequested = true;
        return;
    }

    surface.DesiredPresentMode = mode;
    if (surface.IsPrimary) {
        _primaryConfiguredPresentMode = mode;
    }
    try {
        _callbacks->OnSurfaceRecreated(this, surfaceId, surface.Surface.get());
    } catch (const std::exception& ex) {
        _LogCallbackException("OnSurfaceRecreated", ex);
        _callbackFailed = true;
        _exitRequested = true;
    } catch (...) {
        _LogUnknownCallbackException("OnSurfaceRecreated");
        _callbackFailed = true;
        _exitRequested = true;
    }
}

void Application::SwitchThreadMode(bool enableMultiThread) {
    if (enableMultiThread && !_multiThreaded) {
        _renderWorkerBusy = false;
        _renderThread = std::thread(&Application::RenderThreadMain, this);
        _multiThreaded = true;
    } else if (!enableMultiThread && _multiThreaded) {
        StopRenderThread();
        _multiThreaded = false;
    }
}

void Application::RenderThreadMain() {
    while (true) {
        RenderThreadCommand command{};
        {
            std::unique_lock lock(_renderMutex);
            _renderKickCV.wait(lock, [this] { return !_renderQueue.empty(); });
            command = std::move(_renderQueue.front());
            _renderQueue.pop_front();
            _renderWorkerBusy = true;
        }

        auto result = ExecuteRenderThreadCommand(command);

        {
            std::lock_guard lock(_renderMutex);
            if (result.Completion.has_value()) {
                _renderCompletions.emplace_back(std::move(*result.Completion));
            }
            _renderWorkerBusy = false;
        }
        _renderDoneCV.notify_all();

        if (result.ShouldStop) {
            break;
        }
    }
}

void Application::KickRenderThread(RenderThreadCommand command) {
    {
        std::lock_guard lock(_renderMutex);
        _renderQueue.emplace_back(std::move(command));
    }
    _renderKickCV.notify_one();
}

Application::RenderThreadCommandResult Application::ExecuteRenderThreadCommand(RenderThreadCommand& command) {
    return std::visit([this](auto& typedCommand) { return ExecuteRenderThreadCommand(typedCommand); }, command);
}

Application::RenderThreadCommandResult Application::ExecuteRenderThreadCommand(RenderSurfaceFrameCommand& command) {
    render::SwapChainStatus beginStatus = render::SwapChainStatus::Success;
    render::SwapChainPresentResult presentResult{};
    bool callbackFailed = false;
    bool fatalError = false;

    if (command.Ctx == nullptr) {
        if (command.Surface == nullptr || !command.Surface->IsValid()) {
            beginStatus = render::SwapChainStatus::Error;
            fatalError = true;
        } else {
            try {
                GpuRuntime::BeginFrameResult begin{};
                {
                    std::lock_guard lock(_gpuMutex);
                    if (_allowFrameDrop) {
                        begin = _gpu->TryBeginFrame(command.Surface);
                    } else {
                        begin = _gpu->BeginFrame(command.Surface);
                    }
                }
                beginStatus = begin.Status;
                if (begin.Status == render::SwapChainStatus::Success) {
                    command.Ctx = begin.Context.Release();
                    if (command.Ctx == nullptr) {
                        beginStatus = render::SwapChainStatus::Error;
                        fatalError = true;
                    }
                }
            } catch (const std::exception& ex) {
                RADRAY_ERR_LOG("Application: render-thread BeginFrame for surface {} threw: {}", command.SurfaceId, ex.what());
                beginStatus = render::SwapChainStatus::Error;
                fatalError = true;
            } catch (...) {
                RADRAY_ERR_LOG("Application: render-thread BeginFrame for surface {} threw an unknown exception", command.SurfaceId);
                beginStatus = render::SwapChainStatus::Error;
                fatalError = true;
            }
        }
    }

    if (beginStatus == render::SwapChainStatus::Success && command.Ctx != nullptr) {
        try {
            _callbacks->OnRender(this, command.SurfaceId, command.Ctx.get(), command.AppInfo, command.SurfaceInfo, command.Packet.get());
        } catch (const std::exception& ex) {
            _LogCallbackException("OnRender", ex);
            callbackFailed = true;
        } catch (...) {
            _LogUnknownCallbackException("OnRender");
            callbackFailed = true;
        }

        {
            std::lock_guard lock(_gpuMutex);
            if (command.Ctx->IsEmpty() || callbackFailed) {
                presentResult = _gpu->AbandonFrame(std::move(command.Ctx)).Present;
            } else {
                presentResult = _gpu->SubmitFrame(std::move(command.Ctx)).Present;
            }
        }
    }

    RenderThreadCommandResult result{};
    result.Completion = RenderCompletion{
        .SurfaceId = command.SurfaceId,
        .BeginStatus = beginStatus,
        .PresentResult = presentResult,
        .CallbackFailed = callbackFailed,
        .FatalError = fatalError,
    };
    return result;
}

Application::RenderThreadCommandResult Application::ExecuteRenderThreadCommand(StopRenderThreadCommand& command) {
    (void)command;
    RenderThreadCommandResult result{};
    result.ShouldStop = true;
    return result;
}

void Application::DrainRenderCompletions() {
    deque<RenderCompletion> completions{};
    {
        std::lock_guard lock(_renderMutex);
        completions.swap(_renderCompletions);
    }

    for (auto& completion : completions) {
        auto* surface = FindSurfaceState(completion.SurfaceId);
        if (surface == nullptr) {
            continue;
        }
        if (surface->OutstandingFrameCount == 0) {
            RADRAY_ERR_LOG("Application: render completion for surface {} had no outstanding frame", completion.SurfaceId);
        } else {
            surface->OutstandingFrameCount--;
        }
        if (completion.CallbackFailed) {
            _callbackFailed = true;
            _exitRequested = true;
        }
        if (completion.FatalError) {
            _exitRequested = true;
        }

        switch (completion.BeginStatus) {
            case render::SwapChainStatus::Success:
                HandlePresentResult(completion.SurfaceId, completion.PresentResult);
                break;
            case render::SwapChainStatus::RetryLater:
                surface->DroppedFrameCount++;
                break;
            case render::SwapChainStatus::RequireRecreate:
                if (!surface->PendingRemoval) {
                    surface->PendingRecreate = true;
                }
                break;
            case render::SwapChainStatus::Error:
            default:
                _exitRequested = true;
                break;
        }
    }
}

void Application::WaitRenderIdle() {
    if (!_multiThreaded || !_renderThread.joinable()) {
        return;
    }

    std::unique_lock lock(_renderMutex);
    _renderDoneCV.wait(lock, [this] { return _renderQueue.empty() && !_renderWorkerBusy; });
}

void Application::WaitForRenderProgress() {
    if (!_multiThreaded || !_renderThread.joinable()) {
        return;
    }

    std::unique_lock lock(_renderMutex);
    _renderDoneCV.wait(lock, [this] {
        return !_renderCompletions.empty() || (_renderQueue.empty() && !_renderWorkerBusy);
    });
}

void Application::StopRenderThread() {
    if (!_renderThread.joinable()) {
        return;
    }

    KickRenderThread(RenderThreadCommand{StopRenderThreadCommand{}});
    _renderThread.join();
}

void Application::HandlePresentResult(AppSurfaceId surfaceId, render::SwapChainPresentResult result) {
    auto* surface = FindSurfaceState(surfaceId);
    if (surface == nullptr) {
        return;
    }

    switch (result.Status) {
        case render::SwapChainStatus::RequireRecreate:
            if (!surface->PendingRemoval) {
                surface->PendingRecreate = true;
            }
            break;
        case render::SwapChainStatus::Error:
            RADRAY_ERR_LOG("Application: present returned error for surface {}", surfaceId);
            _exitRequested = true;
            break;
        default:
            break;
    }
}

}  // namespace radray
