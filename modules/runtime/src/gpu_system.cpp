#include <radray/runtime/gpu_system.h>

#include <algorithm>

#include <radray/logger.h>

namespace radray {

// ===========================================================================
// GpuTask
// ===========================================================================

bool GpuTask::IsValid() const noexcept {
    return _fence != nullptr;
}

bool GpuTask::IsCompleted() const noexcept {
    return _fence == nullptr || _fence->GetCompletedValue() >= _targetValue;
}

void GpuTask::Wait() noexcept {
    if (_fence && _fence->GetCompletedValue() < _targetValue) {
        // Slow path: Fence::Wait() waits for the latest signaled value,
        // which is >= _targetValue, so our target will be done when it returns.
        _fence->Wait();
    }
    if (_runtime) {
        _runtime->ProcessSubmits();
    }
}

// ===========================================================================
// GpuPresentSurface
// ===========================================================================

GpuPresentSurface::~GpuPresentSurface() noexcept {
    Destroy();
}

bool GpuPresentSurface::IsValid() const noexcept {
    return _swapchain != nullptr;
}

void GpuPresentSurface::Destroy() noexcept {
    if (_fence) {
        _fence->Wait();
    }
    if (_presentQueue) {
        _presentQueue->Wait();
    }
    if (_runtime) {
        _runtime->ProcessSubmits();
    }
    _swapchain.reset();
    _fence.reset();
    _slotTargetValues.clear();
    _backBufferStates.clear();
    _submitCount = 0;
    _presentQueue = nullptr;
    _runtime = nullptr;
}

uint32_t GpuPresentSurface::GetWidth() const noexcept {
    return _width;
}

uint32_t GpuPresentSurface::GetHeight() const noexcept {
    return _height;
}

render::TextureFormat GpuPresentSurface::GetFormat() const noexcept {
    return _format;
}

render::PresentMode GpuPresentSurface::GetPresentMode() const noexcept {
    return _presentMode;
}

// ===========================================================================
// GpuSubmitContext
// ===========================================================================

GpuResourceHandle GpuSubmitContext::CreateCommandInternal() noexcept {
    if (_queue == nullptr) {
        RADRAY_ERR_LOG("GpuSubmitContext::CreateCommandInternal requires a bound queue");
        return GpuResourceHandle::Invalid();
    }

    auto cmdOpt = _runtime->_device->CreateCommandBuffer(_queue);
    if (!cmdOpt.HasValue()) {
        RADRAY_ERR_LOG("GpuSubmitContext::CreateCommandInternal CreateCommandBuffer failed");
        return GpuResourceHandle::Invalid();
    }

    auto cmd = cmdOpt.Release();
    auto* nativeCmd = cmd.get();
    nativeCmd->Begin();
    _commands.push_back(std::move(cmd));

    GpuResourceHandle handle{
        _runtime->_nextHandleId.fetch_add(1, std::memory_order_relaxed),
        nativeCmd,
    };
    _openCommand = OpenCommand{handle, nativeCmd};
    return handle;
}

bool GpuSubmitContext::IsSurfaceQueueCompatible(const GpuPresentSurface& surface) const noexcept {
    if (_queue == nullptr) {
        return true;
    }
    return _queueType == render::QueueType::Direct && _queue == surface._presentQueue;
}

render::Fence* GpuSubmitContext::ResolveSubmitFence() noexcept {
    if (_surfaceLease.has_value()) {
        if (_surfaceLease->Surface == nullptr || _surfaceLease->Surface->_fence == nullptr) {
            RADRAY_ERR_LOG("GpuSubmitContext::ResolveSubmitFence surface fence is invalid");
            return nullptr;
        }
        return _surfaceLease->Surface->_fence.get();
    }
    if (_submitFence) {
        return _submitFence.get();
    }

    auto fenceOpt = _runtime->_device->CreateFence();
    if (!fenceOpt.HasValue()) {
        RADRAY_ERR_LOG("GpuSubmitContext::ResolveSubmitFence CreateFence failed");
        return nullptr;
    }
    _submitFence = fenceOpt.Release();
    return _submitFence.get();
}

render::CommandBuffer* GpuSubmitContext::AppendPresentBarrierCommand() noexcept {
    if (!_surfaceLease.has_value()) {
        return nullptr;
    }

    auto& lease = _surfaceLease.value();
    auto* surface = lease.Surface;
    if (surface == nullptr) {
        return nullptr;
    }

    const render::TextureState before = surface->_backBufferStates[lease.BackBufferIndex];
    if (before == render::TextureState::Present) {
        return nullptr;
    }
    if (lease.BackBuffer.NativeHandle == nullptr) {
        RADRAY_ERR_LOG("GpuSubmitContext::AppendPresentBarrierCommand backBuffer native handle is invalid");
        return nullptr;
    }

    auto cmdOpt = _runtime->_device->CreateCommandBuffer(_queue);
    if (!cmdOpt.HasValue()) {
        RADRAY_ERR_LOG("GpuSubmitContext::AppendPresentBarrierCommand CreateCommandBuffer failed");
        return nullptr;
    }

    auto cmd = cmdOpt.Release();
    auto* nativeCmd = cmd.get();
    nativeCmd->Begin();
    render::BarrierTextureDescriptor barrier{};
    barrier.Target = static_cast<render::Texture*>(lease.BackBuffer.NativeHandle);
    barrier.Before = before;
    barrier.After = render::TextureState::Present;
    render::ResourceBarrierDescriptor desc = barrier;
    nativeCmd->ResourceBarrier(std::span{&desc, 1});
    nativeCmd->End();
    _commands.push_back(std::move(cmd));
    return nativeCmd;
}

GpuSurfaceAcquireResult GpuSubmitContext::Acquire(GpuPresentSurface& surface) noexcept {
    if (!surface._swapchain) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire called on invalid surface");
        return {GpuResourceHandle::Invalid(), GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }
    if (!IsSurfaceQueueCompatible(surface)) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire requires a Direct queue context");
        return {GpuResourceHandle::Invalid(), GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }
    if (_surfaceLease.has_value()) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire called but a surface is already acquired");
        return {GpuResourceHandle::Invalid(), GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }
    if (_hasPresent) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire called after Present");
        return {GpuResourceHandle::Invalid(), GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }
    if (_openCommand.has_value()) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire called while another command is open");
        return {GpuResourceHandle::Invalid(), GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }

    if (_queue == nullptr) {
        _queue = surface._presentQueue;
        _queueType = render::QueueType::Direct;
        if (_queue == nullptr) {
            RADRAY_ERR_LOG("GpuSubmitContext::Acquire surface present queue is invalid");
            return {GpuResourceHandle::Invalid(), GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
        }
    }

    uint32_t slotIndex = static_cast<uint32_t>(surface._frameNumber % surface._flightFrameCount);
    const uint64_t slotTargetValue = surface._slotTargetValues[slotIndex];
    if (slotTargetValue != 0 && surface._fence && surface._fence->GetCompletedValue() < slotTargetValue) {
        // Reuse of a flight slot must wait until the previous submission bound to
        // that slot has retired, otherwise backends like Vulkan may reuse
        // acquire/present sync primitives while they are still pending.
        surface._fence->Wait();
        _runtime->ProcessSubmits();
    }

    render::AcquireResult acq = surface._swapchain->AcquireNext();
    if (!acq.BackBuffer.HasValue()) {
        return {GpuResourceHandle::Invalid(), GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }

    auto command = CreateCommandInternal();
    if (!command.IsValid()) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire failed to create command");
        return {GpuResourceHandle::Invalid(), GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }

    const uint32_t backBufferIndex = surface._swapchain->GetCurrentBackBufferIndex();
    GpuResourceHandle backBuffer{
        _runtime->_nextHandleId.fetch_add(1, std::memory_order_relaxed),
        acq.BackBuffer.Get(),
    };

    SurfaceLease lease{};
    lease.Surface = &surface;
    lease.BackBuffer = backBuffer;
    lease.FrameSlotIndex = slotIndex;
    lease.BackBufferIndex = backBufferIndex;
    lease.WaitToDraw = acq.WaitToDraw;
    lease.ReadyToPresent = acq.ReadyToPresent;
    _surfaceLease = lease;
    return {backBuffer, command, slotIndex, GpuSurfaceStatus::Success};
}

GpuResourceHandle GpuSubmitContext::BeginCommand(render::QueueType queueType) noexcept {
    if (_hasPresent) {
        RADRAY_ERR_LOG("GpuSubmitContext::BeginCommand called after Present");
        return GpuResourceHandle::Invalid();
    }
    if (_openCommand.has_value()) {
        RADRAY_ERR_LOG("GpuSubmitContext::BeginCommand called while another command is open");
        return GpuResourceHandle::Invalid();
    }

    if (_queue == nullptr) {
        auto queue = _runtime->_device->GetCommandQueue(queueType, 0);
        if (!queue.HasValue()) {
            RADRAY_ERR_LOG("GpuSubmitContext::BeginCommand GetCommandQueue failed");
            return GpuResourceHandle::Invalid();
        }
        _queue = queue.Get();
        _queueType = queueType;
    } else if (_queueType != queueType) {
        RADRAY_ERR_LOG("GpuSubmitContext::BeginCommand called with mismatched queue type");
        return GpuResourceHandle::Invalid();
    }

    auto command = CreateCommandInternal();
    if (!command.IsValid()) {
        return GpuResourceHandle::Invalid();
    }

    return command;
}

void GpuSubmitContext::EndCommand(GpuResourceHandle cmd) noexcept {
    if (!_openCommand.has_value() || !_openCommand->Handle.IsValid() || !_openCommand->Buffer) {
        RADRAY_ERR_LOG("GpuSubmitContext::EndCommand called without an open command");
        return;
    }
    if (_openCommand->Handle.Handle != cmd.Handle) {
        RADRAY_ERR_LOG("GpuSubmitContext::EndCommand called with a non-current command handle");
        return;
    }

    _openCommand->Buffer->End();
    _ops.push_back(CommandOp{_openCommand->Buffer});
    _openCommand.reset();
}

bool GpuSubmitContext::Present(GpuPresentSurface& surface, GpuResourceHandle backBuffer) noexcept {
    if (!backBuffer.IsValid()) {
        RADRAY_ERR_LOG("GpuSubmitContext::Present called with invalid backBuffer handle");
        return false;
    }
    if (_openCommand.has_value()) {
        RADRAY_ERR_LOG("GpuSubmitContext::Present called while a command is still open");
        return false;
    }
    if (_hasPresent) {
        RADRAY_ERR_LOG("GpuSubmitContext::Present called more than once");
        return false;
    }
    if (!_surfaceLease.has_value() || &surface != _surfaceLease->Surface) {
        RADRAY_ERR_LOG("GpuSubmitContext::Present called with mismatched surface");
        return false;
    }
    if (backBuffer.Handle != _surfaceLease->BackBuffer.Handle) {
        RADRAY_ERR_LOG("GpuSubmitContext::Present called with mismatched backBuffer handle");
        return false;
    }
    _hasPresent = true;
    _ops.push_back(PresentOp{});
    return true;
}

bool GpuPresentSurface::Reconfigure(
    uint32_t width,
    uint32_t height,
    std::optional<render::TextureFormat> format,
    std::optional<render::PresentMode> presentMode) noexcept {
    if (_fence) {
        _fence->Wait();
    }
    if (_presentQueue) {
        _presentQueue->Wait();
    }
    if (_runtime) {
        _runtime->ProcessSubmits();
    }

    // Must destroy old swapchain before creating new one (same HWND constraint).
    _swapchain.reset();
    _fence.reset();
    _slotTargetValues.clear();
    _submitCount = 0;

    _width = width;
    _height = height;
    if (format.has_value()) {
        _format = format.value();
    }
    if (presentMode.has_value()) {
        _presentMode = presentMode.value();
    }

    render::SwapChainDescriptor scDesc{};
    scDesc.PresentQueue = _presentQueue;
    scDesc.NativeHandler = _nativeWindowHandle;
    scDesc.Width = _width;
    scDesc.Height = _height;
    scDesc.BackBufferCount = _backBufferCount;
    scDesc.FlightFrameCount = _flightFrameCount;
    scDesc.Format = _format;
    scDesc.PresentMode = _presentMode;

    auto scOpt = _runtime->_device->CreateSwapChain(scDesc);
    if (!scOpt.HasValue()) {
        RADRAY_ERR_LOG("GpuPresentSurface::Reconfigure CreateSwapChain failed");
        return false;
    }
    _swapchain = scOpt.Release();

    auto fenceOpt = _runtime->_device->CreateFence();
    if (!fenceOpt.HasValue()) {
        RADRAY_ERR_LOG("GpuPresentSurface::Reconfigure CreateFence failed");
        _swapchain.reset();
        return false;
    }
    _fence = fenceOpt.Release();

    _slotTargetValues.assign(_flightFrameCount, 0);
    _frameNumber = 0;
    _backBufferStates.assign(_backBufferCount, render::TextureState::Undefined);
    return true;
}

// ===========================================================================
// GpuRuntime
// ===========================================================================

GpuRuntime::~GpuRuntime() noexcept {
    Destroy();
}

bool GpuRuntime::IsValid() const noexcept {
    return _device != nullptr;
}

void GpuRuntime::ProcessSubmits() noexcept {
    std::erase_if(
        _inFlightSubmissions,
        [](const InFlightSubmit& submit) noexcept {
            return submit.Fence == nullptr || submit.Fence->GetCompletedValue() >= submit.TargetValue;
        });
}

void GpuRuntime::Destroy() noexcept {
    if (!_device) {
        return;
    }
    for (int i = 0; i < static_cast<int>(render::QueueType::MAX_COUNT); ++i) {
        auto q = _device->GetCommandQueue(static_cast<render::QueueType>(i), 0);
        if (q.HasValue()) {
            q.Get()->Wait();
        }
    }
    _inFlightSubmissions.clear();
    _device->Destroy();
    _device.reset();
    if (_vkInstance) {
        render::DestroyVulkanInstance(std::move(_vkInstance));
    }
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const GpuRuntimeDescriptor& desc) noexcept {
    auto runtime = make_unique<GpuRuntime>();
    runtime->_backend = desc.Backend;

    switch (desc.Backend) {
        case render::RenderBackend::D3D12: {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
            render::D3D12DeviceDescriptor d3d12Desc{};
            d3d12Desc.IsEnableDebugLayer = desc.EnableDebugValidation;
            d3d12Desc.IsEnableGpuBasedValid = desc.EnableDebugValidation;
            d3d12Desc.LogCallback = desc.LogCallback;
            d3d12Desc.LogUserData = desc.LogUserData;
            auto deviceOpt = render::CreateDevice(d3d12Desc);
            if (!deviceOpt.HasValue()) {
                RADRAY_ERR_LOG("GpuRuntime::Create CreateDevice(D3D12) failed");
                return nullptr;
            }
            runtime->_device = deviceOpt.Release();
#else
            RADRAY_ERR_LOG("GpuRuntime::Create D3D12 backend not available");
            return nullptr;
#endif
            break;
        }
        case render::RenderBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
            render::VulkanInstanceDescriptor insDesc{};
            insDesc.AppName = "RadRay";
            insDesc.AppVersion = 1;
            insDesc.EngineName = "RadRay";
            insDesc.EngineVersion = 1;
            insDesc.IsEnableDebugLayer = desc.EnableDebugValidation;
            insDesc.IsEnableGpuBasedValid = desc.EnableDebugValidation;
            insDesc.LogCallback = desc.LogCallback;
            insDesc.LogUserData = desc.LogUserData;
            auto insOpt = render::CreateVulkanInstance(insDesc);
            if (!insOpt.HasValue()) {
                RADRAY_ERR_LOG("GpuRuntime::Create CreateVulkanInstance failed");
                return nullptr;
            }
            runtime->_vkInstance = insOpt.Release();

            render::VulkanCommandQueueDescriptor queueDesc{};
            queueDesc.Type = render::QueueType::Direct;
            queueDesc.Count = 1;
            render::VulkanDeviceDescriptor vkDevDesc{};
            vkDevDesc.Queues = std::span{&queueDesc, 1};
            auto deviceOpt = render::CreateDevice(vkDevDesc);
            if (!deviceOpt.HasValue()) {
                RADRAY_ERR_LOG("GpuRuntime::Create CreateDevice(Vulkan) failed");
                render::DestroyVulkanInstance(std::move(runtime->_vkInstance));
                return nullptr;
            }
            runtime->_device = deviceOpt.Release();
#else
            RADRAY_ERR_LOG("GpuRuntime::Create Vulkan backend not available");
            return nullptr;
#endif
            break;
        }
        default: {
            RADRAY_ERR_LOG("GpuRuntime::Create unsupported backend");
            return nullptr;
        }
    }

    return runtime;
}

Nullable<unique_ptr<GpuPresentSurface>> GpuRuntime::CreatePresentSurface(const GpuPresentSurfaceDescriptor& desc) noexcept {
    auto queue = _device->GetCommandQueue(render::QueueType::Direct, 0);
    if (!queue.HasValue()) {
        RADRAY_ERR_LOG("GpuRuntime::CreatePresentSurface GetCommandQueue(Direct) failed");
        return nullptr;
    }

    render::SwapChainDescriptor scDesc{};
    scDesc.PresentQueue = queue.Get();
    scDesc.NativeHandler = desc.NativeWindowHandle;
    scDesc.Width = desc.Width;
    scDesc.Height = desc.Height;
    scDesc.BackBufferCount = desc.BackBufferCount;
    scDesc.FlightFrameCount = desc.FlightFrameCount;
    scDesc.Format = desc.Format;
    scDesc.PresentMode = desc.PresentMode;

    auto scOpt = _device->CreateSwapChain(scDesc);
    if (!scOpt.HasValue()) {
        RADRAY_ERR_LOG("GpuRuntime::CreatePresentSurface CreateSwapChain failed");
        return nullptr;
    }

    auto surface = make_unique<GpuPresentSurface>();
    surface->_runtime = this;
    surface->_nativeWindowHandle = desc.NativeWindowHandle;
    surface->_width = desc.Width;
    surface->_height = desc.Height;
    surface->_backBufferCount = desc.BackBufferCount;
    surface->_flightFrameCount = desc.FlightFrameCount;
    surface->_format = desc.Format;
    surface->_presentMode = desc.PresentMode;
    surface->_swapchain = scOpt.Release();
    surface->_presentQueue = queue.Get();

    // Single shared fence
    auto fenceOpt = _device->CreateFence();
    if (!fenceOpt.HasValue()) {
        RADRAY_ERR_LOG("GpuRuntime::CreatePresentSurface CreateFence failed");
        return nullptr;
    }
    surface->_fence = fenceOpt.Release();

    surface->_slotTargetValues.assign(desc.FlightFrameCount, 0);
    surface->_backBufferStates.assign(desc.BackBufferCount, render::TextureState::Undefined);
    return surface;
}

Nullable<unique_ptr<GpuSubmitContext>> GpuRuntime::BeginSubmit() noexcept {
    this->ProcessSubmits();
    auto ctx = make_unique<GpuSubmitContext>();
    ctx->_runtime = this;
    return ctx;
}

GpuTask GpuRuntime::Submit(unique_ptr<GpuSubmitContext> ctx) noexcept {
    this->ProcessSubmits();

    GpuTask task{};

    if (!ctx || ctx->_consumed) {
        return task;
    }
    ctx->_consumed = true;

    if (ctx->_queue == nullptr) {
        RADRAY_ERR_LOG("GpuRuntime::Submit called on a context without a bound queue");
        return task;
    }
    if (ctx->_openCommand.has_value()) {
        RADRAY_ERR_LOG("GpuRuntime::Submit called with unclosed commands");
        return task;
    }
    if (ctx->_ops.empty()) {
        RADRAY_ERR_LOG("GpuRuntime::Submit called on a context without commands");
        return task;
    }

    auto* fence = ctx->ResolveSubmitFence();
    if (fence == nullptr) {
        return task;
    }

    vector<render::CommandBuffer*> submitCommands;
    submitCommands.reserve(ctx->_commands.size() + 1);

    bool sawPresent = false;
    for (const auto& op : ctx->_ops) {
        if (const auto* cmdOp = std::get_if<GpuSubmitContext::CommandOp>(&op)) {
            if (cmdOp->Buffer == nullptr) {
                RADRAY_ERR_LOG("GpuRuntime::Submit encountered an invalid recorded command");
                return task;
            }
            submitCommands.push_back(cmdOp->Buffer);
            continue;
        }

        if (!ctx->_surfaceLease.has_value()) {
            RADRAY_ERR_LOG("GpuRuntime::Submit encountered Present without an acquired surface");
            return task;
        }
        if (sawPresent) {
            RADRAY_ERR_LOG("GpuRuntime::Submit encountered multiple Present ops");
            return task;
        }

        sawPresent = true;
        if (auto* barrierCmd = ctx->AppendPresentBarrierCommand()) {
            submitCommands.push_back(barrierCmd);
        }
    }

    if (submitCommands.empty()) {
        RADRAY_ERR_LOG("GpuRuntime::Submit compiled an empty command list");
        return task;
    }

    render::CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = std::span<render::CommandBuffer*>{submitCommands};
    submitDesc.SignalFence = fence;
    if (ctx->_surfaceLease.has_value()) {
        submitDesc.WaitToExecute = ctx->_surfaceLease->WaitToDraw;
        submitDesc.ReadyToPresent = ctx->_surfaceLease->ReadyToPresent;
    }
    ctx->_queue->Submit(submitDesc);

    uint64_t targetValue = 1;
    if (ctx->_surfaceLease.has_value()) {
        auto& lease = ctx->_surfaceLease.value();
        auto* surface = lease.Surface;
        if (ctx->_hasPresent) {
            surface->_swapchain->Present(lease.ReadyToPresent);
        }

        surface->_backBufferStates[lease.BackBufferIndex] = render::TextureState::Present;
        ++surface->_submitCount;
        surface->_slotTargetValues[lease.FrameSlotIndex] = surface->_submitCount;
        ++surface->_frameNumber;
        targetValue = surface->_submitCount;
    }

    task._fence = fence;
    task._runtime = this;
    task._targetValue = targetValue;

    InFlightSubmit submit{};
    submit.Fence = fence;
    submit.TargetValue = targetValue;
    submit.Context = std::move(ctx);
    _inFlightSubmissions.push_back(std::move(submit));

    return task;
}

}  // namespace radray
