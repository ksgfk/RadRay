#include <radray/runtime/gpu_system.h>

#include <radray/logger.h>

namespace radray {

bool GpuTask::IsValid() const {
    return false;
}

bool GpuTask::IsCompleted() const {
    return false;
}

void GpuTask::Wait() {
}

GpuSurface::GpuSurface(
    GpuRuntime* runtime,
    unique_ptr<render::SwapChain> swapchain) noexcept
    : _runtime(runtime),
      _swapchain(std::move(swapchain)) {}

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

bool GpuAsyncContext::IsEmpty() const {
    return false;
}

bool GpuAsyncContext::DependsOn(const GpuTask& task) {
    return false;
}

GpuFrameContext::~GpuFrameContext() noexcept {
}

GpuResourceHandle GpuFrameContext::GetBackBuffer() const {
    throw std::runtime_error("Not implemented.");
}

uint32_t GpuFrameContext::GetBackBufferIndex() const {
    throw std::runtime_error("Not implemented.");
}

GpuRuntime::GpuRuntime(
    shared_ptr<render::Device> device,
    unique_ptr<render::InstanceVulkan> vkInstance) noexcept
    : _device(std::move(device)),
      _vkInstance(std::move(vkInstance)) {}

GpuRuntime::~GpuRuntime() noexcept {
}

bool GpuRuntime::IsValid() const {
    return _device != nullptr;
}

void GpuRuntime::Destroy() noexcept {
    _device.reset();
    _vkInstance.reset();
}

unique_ptr<GpuSurface> GpuRuntime::CreateSurface(
    const void* nativeHandler,
    uint32_t width, uint32_t height,
    uint32_t backBufferCount,
    uint32_t flightFrameCount,
    render::TextureFormat format,
    render::PresentMode presentMode,
    uint32_t queueSlot) {
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
    auto result = make_unique<GpuSurface>(this, swapchainOpt.Release());
    result->_frameSlots.reserve(flightFrameCount);
    for (uint32_t i = 0; i < flightFrameCount; i++) {
        result->_frameSlots.emplace_back();
    }
    return result;
}

unique_ptr<GpuFrameContext> GpuRuntime::BeginFrame(GpuSurface* surface) {
    return nullptr;
}

Nullable<unique_ptr<GpuFrameContext>> GpuRuntime::TryBeginFrame(GpuSurface* surface) {
    return nullptr;
}

unique_ptr<GpuAsyncContext> GpuRuntime::BeginAsync(render::QueueType type, uint32_t queueSlot) {
    return nullptr;
}

GpuTask GpuRuntime::Submit(unique_ptr<GpuAsyncContext> context) {
    throw std::runtime_error("Not implemented.");
}

void GpuRuntime::ProcessTasks() {
}

render::Fence* GpuRuntime::GetQueueFence(render::QueueType type, uint32_t slot) {
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
