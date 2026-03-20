#include <radray/runtime/gpu_system.h>

#include <radray/logger.h>
#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

namespace radray {

GpuTask::GpuTask(GpuRuntime* runtime, render::Fence* fence, uint64_t signalValue) noexcept
    : _runtime(runtime),
      _fence(fence),
      _signalValue(signalValue) {}

bool GpuTask::IsValid() const {
    return _fence != nullptr && _signalValue != 0;
}

bool GpuTask::IsCompleted() const {
    return !this->IsValid() || _fence->GetCompletedValue() >= _signalValue;
}

void GpuTask::Wait() {
    if (this->IsValid()) {
        _fence->Wait(_signalValue);
    }
}

GpuSurface::GpuSurface(
    GpuRuntime* runtime,
    unique_ptr<render::SwapChain> swapchain,
    uint32_t queueSlot) noexcept
    : _runtime(runtime),
      _swapchain(std::move(swapchain)),
      _queueSlot(queueSlot) {}

GpuSurface::~GpuSurface() noexcept {
    this->Destroy();
}

bool GpuSurface::IsValid() const {
    return _swapchain != nullptr;
}

void GpuSurface::Destroy() {
    _swapchain.reset();
    _runtime = nullptr;
}

uint32_t GpuSurface::GetWidth() const {
    throw std::runtime_error("Not implemented.");
}

uint32_t GpuSurface::GetHeight() const {
    throw std::runtime_error("Not implemented.");
}

render::TextureFormat GpuSurface::GetFormat() const {
    throw std::runtime_error("Not implemented.");
}

render::PresentMode GpuSurface::GetPresentMode() const {
    throw std::runtime_error("Not implemented.");
}

GpuAsyncContext::~GpuAsyncContext() noexcept {
}

GpuAsyncContext::GpuAsyncContext(
    GpuRuntime* runtime,
    render::CommandQueue* queue,
    uint32_t queueSlot) noexcept
    : _runtime(runtime),
      _queue(queue),
      _queueSlot(queueSlot) {}

bool GpuAsyncContext::IsEmpty() const {
    return _cmdBuffers.empty();
}

bool GpuAsyncContext::DependsOn(const GpuTask& task) {
    if (!task.IsValid()) {
        return false;
    }
    _waitFences.emplace_back(task._fence);
    _waitValues.emplace_back(task._signalValue);
    return true;
}

render::CommandBuffer* GpuAsyncContext::CreateCommandBuffer() {
    auto cmdBufferOpt = _runtime->_device->CreateCommandBuffer(_queue);
    if (!cmdBufferOpt.HasValue()) {
        throw GpuSystemException("Device::CreateCommandBuffer failed");
    }
    auto& cmdBuffer = _cmdBuffers.emplace_back(cmdBufferOpt.Release());
    return cmdBuffer.get();
}

GpuFrameContext::GpuFrameContext(
    GpuRuntime* runtime,
    GpuSurface* surface,
    size_t frameSlotIndex,
    render::Texture* backBuffer,
    uint32_t backBufferIndex) noexcept
    : GpuAsyncContext(runtime, surface->_swapchain->GetDesc().PresentQueue, surface->_queueSlot),
      _surface(surface),
      _frameSlotIndex(frameSlotIndex),
      _backBuffer(backBuffer),
      _backBufferIndex(backBufferIndex) {}

GpuFrameContext::~GpuFrameContext() noexcept {
}

render::Texture* GpuFrameContext::GetBackBuffer() const {
    return _backBuffer;
}

uint32_t GpuFrameContext::GetBackBufferIndex() const {
    return _backBufferIndex;
}

GpuRuntime::GpuRuntime(
    shared_ptr<render::Device> device,
    unique_ptr<render::InstanceVulkan> vkInstance) noexcept
    : _device(std::move(device)),
      _vkInstance(std::move(vkInstance)) {}

GpuRuntime::~GpuRuntime() noexcept {
    this->Destroy();
}

bool GpuRuntime::IsValid() const {
    return _device != nullptr;
}

void GpuRuntime::Destroy() noexcept {
    _pendings.clear();
    for (auto& fences : _queueFences) {
        fences.clear();
    }
    _device.reset();
    if (_vkInstance != nullptr) {
        render::DestroyVulkanInstance(std::move(_vkInstance));
    }
}

unique_ptr<GpuSurface> GpuRuntime::CreateSurface(
    const void* nativeHandler,
    uint32_t width, uint32_t height,
    uint32_t backBufferCount,
    uint32_t flightFrameCount,
    render::TextureFormat format,
    render::PresentMode presentMode,
    uint32_t queueSlot) {
    if (flightFrameCount == 0) {
        throw GpuSystemException("flightFrameCount must be > 0");
    }
    auto queueOpt = _device->GetCommandQueue(render::QueueType::Direct, queueSlot);
    if (!queueOpt.HasValue()) {
        throw GpuSystemException("Device::GetCommandQueue failed");
    }
    auto queue = queueOpt.Get();
    render::SwapChainDescriptor desc{
        queue,
        nativeHandler,
        width,
        height,
        backBufferCount,
        format,
        presentMode};
    auto swapchainOpt = _device->CreateSwapChain(desc);
    if (!swapchainOpt.HasValue()) {
        throw GpuSystemException("Device::CreateSwapChain failed");
    }
    auto result = make_unique<GpuSurface>(this, swapchainOpt.Release(), queueSlot);
    result->_frameSlots.reserve(flightFrameCount);
    for (uint32_t i = 0; i < flightFrameCount; i++) {
        result->_frameSlots.emplace_back();
    }
    return result;
}

GpuRuntime::BeginFrameResult GpuRuntime::BeginFrame(GpuSurface* surface) {
    auto fence = this->GetQueueFence(render::QueueType::Direct, surface->_queueSlot);
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    auto& nowFrame = surface->_frameSlots[frameSlotIndex];
    fence->Wait(nowFrame._fenceValue);
    return this->AcquireSwapChain(surface, std::numeric_limits<uint64_t>::max());
}

GpuRuntime::BeginFrameResult GpuRuntime::TryBeginFrame(GpuSurface* surface) {
    auto fence = this->GetQueueFence(render::QueueType::Direct, surface->_queueSlot);
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    auto& nowFrame = surface->_frameSlots[frameSlotIndex];
    if (fence->GetCompletedValue() < nowFrame._fenceValue) {
        return {nullptr, render::SwapChainAcquireStatus::RetryLater};
    }
    return this->AcquireSwapChain(surface, 0);
}

unique_ptr<GpuAsyncContext> GpuRuntime::BeginAsync(render::QueueType type, uint32_t queueSlot) {
    auto queueOpt = _device->GetCommandQueue(type, queueSlot);
    if (!queueOpt.HasValue()) {
        throw GpuSystemException("Device::GetCommandQueue failed");
    }
    auto queue = queueOpt.Get();
    return make_unique<GpuAsyncContext>(this, queue, queueSlot);
}

GpuTask GpuRuntime::Submit(unique_ptr<GpuAsyncContext> context) {
    if (context->IsEmpty()) {
        throw GpuSystemException("GpuRuntime cannot submit empty context");
    }

    vector<render::CommandBuffer*> submitCmds;
    submitCmds.reserve(context->_cmdBuffers.size());
    for (const auto& cmdBuffer : context->_cmdBuffers) {
        submitCmds.emplace_back(cmdBuffer.get());
    }

    auto* fence = this->GetQueueFence(context->_queue->GetQueueType(), context->_queueSlot);
    const uint64_t signalValue = fence->GetLastSignaledValue() + 1;
    render::Fence* signalFences[] = {fence};
    uint64_t signalValues[] = {signalValue};

    render::CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = submitCmds;
    submitDesc.SignalFences = signalFences;
    submitDesc.SignalValues = signalValues;
    submitDesc.WaitFences = context->_waitFences;
    submitDesc.WaitValues = context->_waitValues;

    render::SwapChainSyncObject* waitToExecute[] = {nullptr};
    render::SwapChainSyncObject* readyToPresent[] = {nullptr};
    GpuFrameContext* frame = nullptr;
    if (context->GetType() == GpuAsyncContext::Kind::Frame) {
        frame = static_cast<GpuFrameContext*>(context.get());
        if (frame->_waitToDraw != nullptr) {
            waitToExecute[0] = frame->_waitToDraw.Get();
            submitDesc.WaitToExecute = waitToExecute;
        }
        if (frame->_readyToPresent != nullptr) {
            readyToPresent[0] = frame->_readyToPresent.Get();
            submitDesc.ReadyToPresent = readyToPresent;
        }
    }

    context->_queue->Submit(submitDesc);
    if (frame != nullptr) {
        frame->_surface->_swapchain->Present(frame->_readyToPresent.Get());
        frame->_surface->_frameSlots[frame->_frameSlotIndex]._fenceValue = signalValue;
    }

    _pendings.emplace_back(std::move(context), fence, signalValue);
    return GpuTask{this, fence, signalValue};
}

void GpuRuntime::ProcessTasks() {
    for (size_t i = 0; i < _pendings.size();) {
        auto& pending = _pendings[i];
        if (pending._fence != nullptr && pending._fence->GetCompletedValue() >= pending._signalValue) {
            _pendings.erase(_pendings.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

render::Fence* GpuRuntime::GetQueueFence(render::QueueType type, uint32_t slot) {
#ifdef RADRAY_ENABLE_D3D12
    if (_device->GetBackend() == render::RenderBackend::D3D12) {
        auto deviceD3D12 = render::d3d12::CastD3D12Object(_device.get());
        auto queueD3D12Opt = deviceD3D12->GetCommandQueue(type, slot);
        if (!queueD3D12Opt.HasValue()) {
            throw GpuSystemException("Device::GetCommandQueue failed");
        }
        auto queueD3D12 = render::d3d12::CastD3D12Object(queueD3D12Opt.Get());
        return queueD3D12->_fence.get();
    }
#endif
    auto& fences = _queueFences[(size_t)type];
    if (fences.size() <= slot) {
        fences.reserve(slot + 1);
        for (size_t i = fences.size(); i <= slot; i++) {
            fences.emplace_back(nullptr);
        }
    }
    auto& fence = fences[slot];
    if (!fence) {
        auto fenceOpt = _device->CreateFence();
        if (!fenceOpt.HasValue()) {
            throw GpuSystemException("Device::CreateFence failed");
        }
        fence = fenceOpt.Release();
    }
    return fence.get();
}

GpuRuntime::BeginFrameResult GpuRuntime::AcquireSwapChain(GpuSurface* surface, uint64_t timeoutMs) {
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    const auto acqResult = surface->_swapchain->AcquireNext(timeoutMs);
    switch (acqResult.Status) {
        case render::SwapChainAcquireStatus::Success: {
            auto context = make_unique<GpuFrameContext>(
                this,
                surface,
                frameSlotIndex,
                acqResult.BackBuffer.Get(),
                acqResult.BackBufferIndex);
            context->_type = GpuAsyncContext::Kind::Frame;
            context->_waitToDraw = acqResult.WaitToDraw;
            context->_readyToPresent = acqResult.ReadyToPresent;
            surface->_nextFrameSlotIndex = (frameSlotIndex + 1) % surface->_frameSlots.size();
            return {std::move(context), render::SwapChainAcquireStatus::Success};
        }
        case render::SwapChainAcquireStatus::RetryLater:
        case render::SwapChainAcquireStatus::RequireRecreate:
            return {nullptr, acqResult.Status};
        case render::SwapChainAcquireStatus::Error:
        default: {
            throw GpuSystemException("SwapChain::AcquireNext failed");
        }
    }
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const render::VulkanDeviceDescriptor& desc, render::VulkanInstanceDescriptor vkInsDesc) {
    auto insOpt = render::CreateVulkanInstance(vkInsDesc);
    if (!insOpt.HasValue()) {
        return nullptr;
    }
    auto deviceOpt = render::CreateDevice(desc);
    if (!deviceOpt.HasValue()) {
        return nullptr;
    }
    return make_unique<GpuRuntime>(deviceOpt.Release(), insOpt.Release());
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const render::D3D12DeviceDescriptor& desc) {
    auto deviceOpt = render::CreateDevice(desc);
    if (!deviceOpt.HasValue()) {
        return nullptr;
    }
    return make_unique<GpuRuntime>(deviceOpt.Release(), nullptr);
}

}  // namespace radray
