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

GpuSurfaceAcquireResult GpuSubmitContext::Acquire(GpuPresentSurface& surface) noexcept {
    if (!surface._swapchain) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire called on invalid surface");
        return {GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }
    if (!_queueBound) {
        auto queue = _runtime->_device->GetCommandQueue(render::QueueType::Direct, 0);
        if (!queue.HasValue()) {
            RADRAY_ERR_LOG("GpuSubmitContext::Acquire GetCommandQueue(Direct) failed");
            return {GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
        }
        _queue = queue.Get();
        _queueType = render::QueueType::Direct;
        _queueBound = true;
    } else if (_queueType != render::QueueType::Direct) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire requires a Direct queue context");
        return {GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }
    if (_acquiredSurface != nullptr) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire called but a surface is already acquired");
        return {GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
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
        return {GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }

    auto cmdOpt = _runtime->_device->CreateCommandBuffer(_queue);
    if (!cmdOpt.HasValue()) {
        RADRAY_ERR_LOG("GpuSubmitContext::Acquire CreateCommandBuffer failed");
        return {GpuResourceHandle::Invalid(), 0, GpuSurfaceStatus::Lost};
    }
    _cmdBuffer = cmdOpt.Release();

    _acquiredSurface = &surface;
    _frameSlotIndex = slotIndex;
    _waitToDraw = acq.WaitToDraw;
    _readyToPresent = acq.ReadyToPresent;

    GpuResourceHandle handle{_runtime->_nextHandleId.fetch_add(1, std::memory_order_relaxed), acq.BackBuffer.Get()};
    _acquiredBackBuffer = handle;
    return {handle, slotIndex, GpuSurfaceStatus::Success};
}

void GpuSubmitContext::Present(GpuPresentSurface& surface, GpuResourceHandle backBuffer) noexcept {
    if (!backBuffer.IsValid()) {
        RADRAY_ERR_LOG("GpuSubmitContext::Present called with invalid backBuffer handle");
        return;
    }
    if (&surface != _acquiredSurface) {
        RADRAY_ERR_LOG("GpuSubmitContext::Present called with mismatched surface");
        return;
    }
    if (backBuffer.Handle != _acquiredBackBuffer.Handle) {
        RADRAY_ERR_LOG("GpuSubmitContext::Present called with mismatched backBuffer handle");
        return;
    }
    _shouldPresent = true;
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

    if (ctx->_acquiredSurface) {
        auto* surface = ctx->_acquiredSurface;
        auto* cmd = ctx->_cmdBuffer.get();
        auto* fence = surface->_fence.get();

        cmd->Begin();

        uint32_t bbIndex = surface->_swapchain->GetCurrentBackBufferIndex();
        auto* backBuffer = surface->_swapchain->GetCurrentBackBuffer().Get();
        if (backBuffer && surface->_backBufferStates[bbIndex] != render::TextureState::Present) {
            render::BarrierTextureDescriptor barrier{};
            barrier.Target = static_cast<render::Texture*>(backBuffer);
            barrier.Before = surface->_backBufferStates[bbIndex];
            barrier.After = render::TextureState::Present;
            render::ResourceBarrierDescriptor desc = barrier;
            cmd->ResourceBarrier(std::span{&desc, 1});
        }

        cmd->End();

        render::CommandBuffer* cmdArr[] = {cmd};
        render::CommandQueueSubmitDescriptor submitDesc{};
        submitDesc.CmdBuffers = std::span{cmdArr};
        submitDesc.SignalFence = fence;
        submitDesc.WaitToExecute = ctx->_waitToDraw;
        submitDesc.ReadyToPresent = ctx->_readyToPresent;
        ctx->_queue->Submit(submitDesc);

        if (ctx->_shouldPresent) {
            surface->_swapchain->Present(ctx->_readyToPresent);
        }

        surface->_backBufferStates[bbIndex] = render::TextureState::Present;
        ++surface->_submitCount;
        surface->_slotTargetValues[ctx->_frameSlotIndex] = surface->_submitCount;
        ++surface->_frameNumber;

        task._fence = fence;
        task._runtime = this;
        task._targetValue = surface->_submitCount;
        InFlightSubmit submit{};
        submit.Fence = fence;
        submit.TargetValue = task._targetValue;
        submit.Context = std::move(ctx);
        _inFlightSubmissions.push_back(std::move(submit));
    } else {
        // No surface acquired — pure compute/copy path (not yet implemented).
        // Return an empty (invalid) task.
    }

    return task;
}

}  // namespace radray
