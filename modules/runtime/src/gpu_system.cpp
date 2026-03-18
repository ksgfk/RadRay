#include <radray/runtime/gpu_system.h>

#include <chrono>
#include <stdexcept>
#include <thread>

#include <radray/logger.h>
#include <radray/render/common.h>

#if defined(_WIN32) && !defined(RADRAY_PLATFORM_WINDOWS)
#define RADRAY_PLATFORM_WINDOWS 1
#endif

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray {

namespace {

[[noreturn]] void ThrowGpuRuntimeError(const string& message) {
    throw std::runtime_error(message.c_str());
}

[[noreturn]] void ThrowInvalidArgument(const string& message) {
    throw std::invalid_argument(message.c_str());
}

void ThrowIfRuntimeInvalid(const GpuRuntime* runtime, const char* action) {
    if (runtime == nullptr || !runtime->IsValid()) {
        ThrowGpuRuntimeError(fmt::format("GpuRuntime is invalid during {}.", action));
    }
}

void WaitForFenceValue(render::Device* device, render::Fence* fence, uint64_t signalValue) {
    if (device == nullptr || fence == nullptr) {
        ThrowInvalidArgument("Cannot wait for a null GPU fence.");
    }
    if (signalValue == 0) {
        return;
    }

    switch (device->GetBackend()) {
        case render::RenderBackend::D3D12: {
#ifdef RADRAY_ENABLE_D3D12
#if defined(_WIN32)
            auto* d3d12Fence = static_cast<render::d3d12::FenceD3D12*>(fence);
            if (d3d12Fence == nullptr || !d3d12Fence->IsValid()) {
                ThrowGpuRuntimeError("D3D12 fence is invalid.");
            }
            if (d3d12Fence->_fence->GetCompletedValue() >= signalValue) {
                return;
            }

            HANDLE eventHandle = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (eventHandle == nullptr) {
                ThrowGpuRuntimeError(fmt::format("CreateEventW failed: {}.", ::GetLastError()));
            }
            const HRESULT hr = d3d12Fence->_fence->SetEventOnCompletion(signalValue, eventHandle);
            if (FAILED(hr)) {
                ::CloseHandle(eventHandle);
                ThrowGpuRuntimeError(fmt::format("ID3D12Fence::SetEventOnCompletion failed: {}.", hr));
            }
            ::WaitForSingleObject(eventHandle, INFINITE);
            ::CloseHandle(eventHandle);
            return;
#else
            ThrowGpuRuntimeError("D3D12 GPU waits are unsupported on this platform.");
#endif
#else
            ThrowGpuRuntimeError("D3D12 backend is not enabled.");
#endif
        }
        case render::RenderBackend::Vulkan: {
#ifdef RADRAY_ENABLE_VULKAN
            auto* vkFence = static_cast<render::vulkan::FenceVulkan*>(fence);
            if (vkFence == nullptr || !vkFence->IsValid()) {
                ThrowGpuRuntimeError("Vulkan fence is invalid.");
            }

            auto* timeline = std::get_if<unique_ptr<render::vulkan::TimelineSemaphoreVulkan>>(&vkFence->_fence);
            if (timeline == nullptr || timeline->get() == nullptr || !(*timeline)->IsValid()) {
                ThrowGpuRuntimeError("GpuRuntime requires a Vulkan timeline semaphore fence.");
            }
            if ((*timeline)->GetCompletedValue() >= signalValue) {
                return;
            }

            const VkSemaphore semaphore = (*timeline)->_semaphore;
            VkSemaphoreWaitInfo waitInfo{};
            waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            waitInfo.pNext = nullptr;
            waitInfo.flags = 0;
            waitInfo.semaphoreCount = 1;
            waitInfo.pSemaphores = &semaphore;
            waitInfo.pValues = &signalValue;
            const VkResult vr = vkFence->_device->_ftb.vkWaitSemaphores(vkFence->_device->_device, &waitInfo, UINT64_MAX);
            if (vr != VK_SUCCESS) {
                ThrowGpuRuntimeError(fmt::format("vkWaitSemaphores failed: {}.", vr));
            }
            return;
#else
            ThrowGpuRuntimeError("Vulkan backend is not enabled.");
#endif
        }
        case render::RenderBackend::Metal:
            fence->Wait();
            return;
    }

    fence->Wait();
}

bool FenceSupportsValueWait(render::Device* device, render::Fence* fence) {
    if (device == nullptr || fence == nullptr || !fence->IsValid()) {
        return false;
    }

    switch (device->GetBackend()) {
        case render::RenderBackend::D3D12:
            return true;
        case render::RenderBackend::Vulkan: {
#ifdef RADRAY_ENABLE_VULKAN
            auto* vkFence = static_cast<render::vulkan::FenceVulkan*>(fence);
            if (vkFence == nullptr || !vkFence->IsValid()) {
                return false;
            }
            auto* timeline = std::get_if<unique_ptr<render::vulkan::TimelineSemaphoreVulkan>>(&vkFence->_fence);
            return timeline != nullptr && timeline->get() != nullptr && (*timeline)->IsValid();
#else
            return false;
#endif
        }
        case render::RenderBackend::Metal:
            return true;
    }

    return false;
}

vector<render::Texture*> EnumerateSwapChainBackBuffers(render::Device* device, render::SwapChain* swapchain) {
    vector<render::Texture*> result;
    if (device == nullptr || swapchain == nullptr || !swapchain->IsValid()) {
        return result;
    }

    result.reserve(swapchain->GetBackBufferCount());
    switch (device->GetBackend()) {
        case render::RenderBackend::D3D12: {
#ifdef RADRAY_ENABLE_D3D12
            auto* d3d12Swapchain = static_cast<render::d3d12::SwapChainD3D12*>(swapchain);
            for (auto& frame : d3d12Swapchain->_frames) {
                result.emplace_back(frame.image.get());
            }
#endif
            break;
        }
        case render::RenderBackend::Vulkan: {
#ifdef RADRAY_ENABLE_VULKAN
            auto* vkSwapchain = static_cast<render::vulkan::SwapChainVulkan*>(swapchain);
            for (auto& frame : vkSwapchain->_frames) {
                result.emplace_back(frame.image.get());
            }
#endif
            break;
        }
        case render::RenderBackend::Metal:
            break;
    }

    return result;
}
}  // namespace

GpuTask::GpuTask(GpuRuntime* runtime, GpuResourceHandle fence, uint64_t signalValue) noexcept
    : _runtime(runtime),
      _fence(fence),
      _signalValue(signalValue) {}

bool GpuTask::IsValid() const {
    return _runtime != nullptr &&
           _runtime->IsValid() &&
           _fence.IsValid() &&
           _fence.NativeHandle != nullptr &&
           _signalValue != 0;
}

bool GpuTask::IsCompleted() const {
    if (!this->IsValid()) {
        return false;
    }
    auto* fence = static_cast<render::Fence*>(_fence.NativeHandle);
    return fence->GetCompletedValue() >= _signalValue;
}

void GpuTask::Wait() {
    if (!this->IsValid()) {
        ThrowGpuRuntimeError("GpuTask is invalid.");
    }
    WaitForFenceValue(_runtime->_device.get(), static_cast<render::Fence*>(_fence.NativeHandle), _signalValue);
    _runtime->OnTaskWaited(static_cast<render::Fence*>(_fence.NativeHandle), _signalValue);
}

GpuSurface::GpuSurface(
    GpuRuntime* runtime,
    unique_ptr<render::SwapChain> swapchain,
    const render::SwapChainDescriptor& desc,
    uint32_t queueSlot) noexcept
    : _runtime(runtime),
      _swapchain(std::move(swapchain)),
      _desc(desc),
      _queueSlot(queueSlot) {}

GpuSurface::~GpuSurface() noexcept {
    this->Destroy();
}

bool GpuSurface::IsValid() const {
    return _runtime != nullptr &&
           _runtime->IsValid() &&
           _swapchain != nullptr &&
           _swapchain->IsValid();
}

void GpuSurface::Destroy() {
    GpuRuntime* runtime = _runtime;
    if (runtime != nullptr) {
        try {
            auto& queueState = runtime->EnsureQueueState(render::QueueType::Direct, _queueSlot);
            if (queueState.Queue != nullptr) {
                queueState.Queue->Wait();
            }
        } catch (...) {
        }

        std::lock_guard<std::mutex> runtimeLock(runtime->_mutex);
        runtime->_surfaces.erase(this);
        for (const auto& handle : _backBuffers) {
            if (handle.IsValid()) {
                runtime->_resourceRegistry.erase(handle.Handle);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _inFlightFrames.clear();
    }
    _backBuffers.clear();
    _backBufferStates.clear();
    _swapchain.reset();
    _runtime = nullptr;
}

uint32_t GpuSurface::GetWidth() const {
    return _desc.Width;
}

uint32_t GpuSurface::GetHeight() const {
    return _desc.Height;
}

render::TextureFormat GpuSurface::GetFormat() const {
    return _desc.Format;
}

render::PresentMode GpuSurface::GetPresentMode() const {
    return _desc.PresentMode;
}

GpuAsyncContext::GpuAsyncContext(GpuRuntime* runtime, render::QueueType type, uint32_t queueSlot) noexcept
    : _runtime(runtime),
      _queueType(type),
      _queueSlot(queueSlot) {}

GpuAsyncContext::~GpuAsyncContext() noexcept = default;

bool GpuAsyncContext::IsEmpty() const {
    return !_isFrameContext && _waitFences.empty();
}

bool GpuAsyncContext::DependsOn(const GpuTask& task) {
    if (_runtime == nullptr || task._runtime != _runtime || !task.IsValid()) {
        return false;
    }

    auto* fence = static_cast<render::Fence*>(task._fence.NativeHandle);
    if (fence == nullptr || !fence->IsValid()) {
        return false;
    }

    _waitFences.emplace_back(fence);
    _waitValues.emplace_back(task._signalValue);
    return true;
}

GpuFrameContext::GpuFrameContext(
    GpuRuntime* runtime,
    GpuSurface* surface,
    uint32_t queueSlot,
    GpuResourceHandle backBuffer,
    uint32_t backBufferIndex,
    render::SwapChainSyncObject* waitToDraw,
    render::SwapChainSyncObject* readyToPresent,
    unique_ptr<render::CommandBuffer> internalCmdBuffer) noexcept
    : GpuAsyncContext(runtime, render::QueueType::Direct, queueSlot),
      _surface(surface),
      _backBuffer(backBuffer),
      _backBufferIndex(backBufferIndex),
      _waitToDraw(waitToDraw),
      _readyToPresent(readyToPresent),
      _internalCmdBuffer(std::move(internalCmdBuffer)) {
    _isFrameContext = true;
}

GpuFrameContext::~GpuFrameContext() noexcept = default;

GpuResourceHandle GpuFrameContext::GetBackBuffer() const {
    return _backBuffer;
}

uint32_t GpuFrameContext::GetBackBufferIndex() const {
    return _backBufferIndex;
}

GpuRuntime::GpuRuntime(shared_ptr<render::Device> device, unique_ptr<render::InstanceVulkan> vkInstance) noexcept
    : _device(std::move(device)),
      _vkInstance(std::move(vkInstance)) {}

GpuRuntime::~GpuRuntime() noexcept {
    this->Destroy();
}

bool GpuRuntime::IsValid() const {
    return _device != nullptr && _device->IsValid();
}

void GpuRuntime::Destroy() {
    unique_ptr<render::InstanceVulkan> vkInstance;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_device == nullptr && _vkInstance == nullptr) {
            return;
        }

        for (auto* surface : _surfaces) {
            if (surface == nullptr) {
                continue;
            }
            if (surface->_queueSlot < _queues[static_cast<size_t>(render::QueueType::Direct)].size()) {
                const auto& queueState = _queues[static_cast<size_t>(render::QueueType::Direct)][surface->_queueSlot];
                if (queueState != nullptr && queueState->Queue != nullptr) {
                    queueState->Queue->Wait();
                }
            }
            {
                std::lock_guard<std::mutex> surfaceLock(surface->_mutex);
                surface->_inFlightFrames.clear();
            }
            surface->_backBuffers.clear();
            surface->_backBufferStates.clear();
            surface->_swapchain.reset();
            surface->_runtime = nullptr;
        }
        _surfaces.clear();

        for (auto& queues : _queues) {
            for (auto& state : queues) {
                if (state == nullptr) {
                    continue;
                }
                state->Queue = nullptr;
                state->Fence.reset();
                state->FenceHandle.Invalidate();
                state->NextSignalValue = 1;
                state->LastWaitedSignalValue = 0;
                state->PendingSubmissions.clear();
            }
            queues.clear();
        }

        _resourceRegistry.clear();
        _nextResourceHandle = 1;

        _device.reset();
        vkInstance = std::move(_vkInstance);
    }

    if (vkInstance != nullptr) {
        render::DestroyVulkanInstance(std::move(vkInstance));
    }
}

unique_ptr<GpuSurface> GpuRuntime::CreateSurface(const render::SwapChainDescriptor& desc, uint32_t queueSlot) {
    ThrowIfRuntimeInvalid(this, "CreateSurface");

    render::SwapChainDescriptor runtimeDesc = desc;
    QueueState& queueState = this->EnsureQueueState(render::QueueType::Direct, queueSlot);
    runtimeDesc.PresentQueue = queueState.Queue;

    auto swapchainOpt = _device->CreateSwapChain(runtimeDesc);
    if (!swapchainOpt.HasValue()) {
        ThrowGpuRuntimeError("GpuRuntime::CreateSurface failed to create a swapchain.");
    }

    auto swapchain = swapchainOpt.Release();
    const render::SwapChainDescriptor actualDesc = swapchain->GetDesc();
    auto surface = unique_ptr<GpuSurface>(new GpuSurface(this, std::move(swapchain), actualDesc, queueSlot));
    surface->_backBuffers.resize(surface->_swapchain->GetBackBufferCount(), GpuResourceHandle::Invalid());
    surface->_backBufferStates.resize(surface->_swapchain->GetBackBufferCount(), render::TextureState::Undefined);

    const vector<render::Texture*> backBuffers = EnumerateSwapChainBackBuffers(_device.get(), surface->_swapchain.get());
    for (size_t i = 0; i < backBuffers.size() && i < surface->_backBuffers.size(); ++i) {
        if (backBuffers[i] != nullptr) {
            surface->_backBuffers[i] = this->RegisterHandle(backBuffers[i]);
        }
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _surfaces.emplace(surface.get());
    }
    return surface;
}

unique_ptr<GpuFrameContext> GpuRuntime::BeginFrame(GpuSurface* surface) {
    ThrowIfRuntimeInvalid(this, "BeginFrame");
    if (surface == nullptr || !surface->IsValid()) {
        ThrowInvalidArgument("GpuSurface is invalid.");
    }

    while (true) {
        render::Fence* waitFence = nullptr;
        uint64_t waitValue = 0;
        {
            std::lock_guard<std::mutex> lock(surface->_mutex);
            if (surface->_desc.FlightFrameCount == 0 || surface->_inFlightFrames.size() < surface->_desc.FlightFrameCount) {
                break;
            }
            const auto& oldest = surface->_inFlightFrames.front();
            waitFence = oldest.Fence;
            waitValue = oldest.SignalValue;
        }

        WaitForFenceValue(_device.get(), waitFence, waitValue);
        this->EnsureQueueState(render::QueueType::Direct, surface->_queueSlot).Queue->Wait();
        while (true) {
            {
                std::lock_guard<std::mutex> lock(surface->_mutex);
                if (surface->_desc.FlightFrameCount == 0 || surface->_inFlightFrames.size() < surface->_desc.FlightFrameCount) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    const render::AcquireResult acquired = surface->_swapchain->AcquireNext();
    if (acquired.Status != render::SwapChainAcquireStatus::Success || !acquired.BackBuffer.HasValue()) {
        ThrowGpuRuntimeError(fmt::format(
            "Swapchain acquire failed during BeginFrame (status {}, native status {}).",
            static_cast<int32_t>(acquired.Status),
            acquired.NativeStatusCode));
    }

    return this->CreateFrameContext(surface, acquired);
}

Nullable<unique_ptr<GpuFrameContext>> GpuRuntime::TryBeginFrame(GpuSurface* surface) {
    ThrowIfRuntimeInvalid(this, "TryBeginFrame");
    if (surface == nullptr || !surface->IsValid()) {
        ThrowInvalidArgument("GpuSurface is invalid.");
    }

    this->RetireCompletedFrames(surface);
    if (surface->_desc.FlightFrameCount != 0) {
        std::lock_guard<std::mutex> lock(surface->_mutex);
        if (surface->_inFlightFrames.size() >= surface->_desc.FlightFrameCount) {
            return nullptr;
        }
    }

    const render::AcquireResult acquired = surface->_swapchain->AcquireNext();
    if (acquired.Status == render::SwapChainAcquireStatus::Unavailable) {
        return nullptr;
    }
    if (acquired.Status != render::SwapChainAcquireStatus::Success) {
        ThrowGpuRuntimeError(fmt::format(
            "Swapchain acquire failed during TryBeginFrame (status {}, native status {}).",
            static_cast<int32_t>(acquired.Status),
            acquired.NativeStatusCode));
    }
    if (!acquired.BackBuffer.HasValue()) {
        ThrowGpuRuntimeError("Swapchain acquire succeeded without a back buffer.");
    }

    return this->CreateFrameContext(surface, acquired);
}

unique_ptr<GpuAsyncContext> GpuRuntime::BeginAsync(render::QueueType type, uint32_t queueSlot) {
    ThrowIfRuntimeInvalid(this, "BeginAsync");
    static_cast<void>(this->EnsureQueueState(type, queueSlot));
    return unique_ptr<GpuAsyncContext>(new GpuAsyncContext(this, type, queueSlot));
}

GpuTask GpuRuntime::Submit(unique_ptr<GpuAsyncContext> context) {
    ThrowIfRuntimeInvalid(this, "Submit");
    if (context == nullptr || context->_runtime != this) {
        ThrowInvalidArgument("GpuAsyncContext does not belong to this runtime.");
    }

    QueueState& queueState = this->EnsureQueueState(context->_queueType, context->_queueSlot);
    const uint64_t signalValue = queueState.NextSignalValue++;

    render::Fence* signalFence = queueState.Fence.get();
    render::Fence* signalFences[] = {signalFence};
    uint64_t signalValues[] = {signalValue};

    render::CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.SignalFences = std::span{signalFences, 1};
    submitDesc.SignalValues = std::span{signalValues, 1};
    if (!context->_waitFences.empty()) {
        submitDesc.WaitFences = context->_waitFences;
        submitDesc.WaitValues = context->_waitValues;
    }

    render::SwapChainSyncObject* waitToExecute = nullptr;
    render::SwapChainSyncObject* readyToPresent = nullptr;
    GpuSurface* surface = nullptr;
    uint32_t frameBackBufferIndex = std::numeric_limits<uint32_t>::max();
    unique_ptr<render::CommandBuffer> internalCmdBuffer;
    render::CommandBuffer* cmdBuffers[] = {nullptr};
    if (context->_isFrameContext) {
        auto* frame = static_cast<GpuFrameContext*>(context.get());
        surface = frame->_surface;
        waitToExecute = frame->_waitToDraw;
        readyToPresent = frame->_readyToPresent;
        frameBackBufferIndex = frame->_backBufferIndex;
        internalCmdBuffer = std::move(frame->_internalCmdBuffer);
        if (internalCmdBuffer != nullptr) {
            cmdBuffers[0] = internalCmdBuffer.get();
            submitDesc.CmdBuffers = std::span{cmdBuffers, 1};
        }

        if (waitToExecute != nullptr) {
            submitDesc.WaitToExecute = std::span{&waitToExecute, 1};
        }
        if (readyToPresent != nullptr) {
            submitDesc.ReadyToPresent = std::span{&readyToPresent, 1};
        }
    }

    queueState.Queue->Submit(submitDesc);
    if (internalCmdBuffer != nullptr) {
        std::lock_guard<std::mutex> lock(queueState.Mutex);
        queueState.PendingSubmissions.emplace_back(GpuRuntime::QueueState::PendingSubmission{
            .SignalValue = signalValue,
            .InternalCommandBuffer = std::move(internalCmdBuffer)});
    }
    if (surface != nullptr && surface->_swapchain != nullptr) {
        surface->_swapchain->Present(readyToPresent);
        std::lock_guard<std::mutex> lock(surface->_mutex);
        if (frameBackBufferIndex < surface->_backBufferStates.size()) {
            surface->_backBufferStates[frameBackBufferIndex] = render::TextureState::Present;
        }
        if (surface->_desc.FlightFrameCount != 0) {
            surface->_inFlightFrames.emplace_back(GpuSurface::PendingFrame{signalFence, signalValue});
        }
    }

    return GpuTask(this, queueState.FenceHandle, signalValue);
}

void GpuRuntime::ProcessTasks() {
    if (!this->IsValid()) {
        return;
    }

    vector<QueueState*> queueStates;
    vector<GpuSurface*> surfaces;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& queues : _queues) {
            for (auto& state : queues) {
                if (state != nullptr && state->Fence != nullptr) {
                    queueStates.emplace_back(state.get());
                }
            }
        }
        surfaces.reserve(_surfaces.size());
        for (auto* surface : _surfaces) {
            surfaces.emplace_back(surface);
        }
    }

    for (auto* state : queueStates) {
        std::lock_guard<std::mutex> queueLock(state->Mutex);
        while (!state->PendingSubmissions.empty()) {
            const auto& pending = state->PendingSubmissions.front();
            if (state->Fence->GetCompletedValue() < pending.SignalValue) {
                break;
            }
            state->PendingSubmissions.pop_front();
        }
    }
    for (auto* surface : surfaces) {
        this->RetireCompletedFrames(surface);
    }
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(
    const render::DeviceDescriptor& desc,
    std::optional<render::VulkanInstanceDescriptor> vkInsDesc) {
    unique_ptr<render::InstanceVulkan> vkInstance;
    if (std::holds_alternative<render::VulkanDeviceDescriptor>(desc)) {
        if (!vkInsDesc.has_value()) {
            ThrowInvalidArgument("GpuRuntime::Create requires a Vulkan instance descriptor for Vulkan devices.");
        }
        auto vkInstanceOpt = render::CreateVulkanInstance(vkInsDesc.value());
        if (!vkInstanceOpt.HasValue()) {
            return nullptr;
        }
        vkInstance = vkInstanceOpt.Release();
    }

    auto deviceOpt = render::CreateDevice(desc);
    if (!deviceOpt.HasValue()) {
        if (vkInstance != nullptr) {
            render::DestroyVulkanInstance(std::move(vkInstance));
        }
        return nullptr;
    }

    auto device = deviceOpt.Release();
    if (device == nullptr || !device->IsValid()) {
        if (vkInstance != nullptr) {
            render::DestroyVulkanInstance(std::move(vkInstance));
        }
        return nullptr;
    }

    auto runtime = unique_ptr<GpuRuntime>(new GpuRuntime(std::move(device), std::move(vkInstance)));

    if (runtime->_device->GetBackend() == render::RenderBackend::Vulkan) {
        auto* queue = runtime->_device->GetCommandQueue(render::QueueType::Direct, 0).Get();
        if (queue == nullptr) {
            runtime->Destroy();
            return nullptr;
        }

        auto fenceOpt = runtime->_device->CreateFence();
        if (!fenceOpt.HasValue()) {
            runtime->Destroy();
            return nullptr;
        }
        auto fence = fenceOpt.Release();
        if (!FenceSupportsValueWait(runtime->_device.get(), fence.get())) {
            runtime->Destroy();
            return nullptr;
        }
    }

    return runtime;
}

GpuRuntime::QueueState& GpuRuntime::EnsureQueueState(render::QueueType type, uint32_t queueSlot) {
    ThrowIfRuntimeInvalid(this, "EnsureQueueState");

    const size_t typeIndex = static_cast<size_t>(type);
    if (typeIndex >= _queues.size()) {
        ThrowInvalidArgument(fmt::format("Invalid queue type index {}.", typeIndex));
    }

    std::lock_guard<std::mutex> lock(_mutex);
    auto& slots = _queues[typeIndex];
    if (slots.size() <= queueSlot) {
        slots.resize(queueSlot + 1);
    }

    unique_ptr<QueueState>& slot = slots[queueSlot];
    if (slot == nullptr) {
        auto queueOpt = _device->GetCommandQueue(type, queueSlot);
        if (!queueOpt.HasValue()) {
            ThrowGpuRuntimeError(fmt::format("Queue {}:{} is not available.", format_as(type), queueSlot));
        }

        auto fenceOpt = _device->CreateFence();
        if (!fenceOpt.HasValue()) {
            ThrowGpuRuntimeError(fmt::format("Failed to create fence for queue {}:{}.", format_as(type), queueSlot));
        }

        slot = make_unique<QueueState>();
        slot->Queue = queueOpt.Get();
        slot->Fence = fenceOpt.Release();
        if (!FenceSupportsValueWait(_device.get(), slot->Fence.get())) {
            ThrowGpuRuntimeError(fmt::format(
                "Queue fence for {}:{} does not support value waits.",
                format_as(type),
                queueSlot));
        }
        const uint64_t handleValue = _nextResourceHandle++;
        _resourceRegistry.emplace(handleValue, ResourceRegistryEntry{slot->Fence.get()});
        slot->FenceHandle = GpuResourceHandle{handleValue, slot->Fence.get()};
        slot->NextSignalValue = 1;
        slot->LastWaitedSignalValue = 0;
    }

    return *slot;
}

unique_ptr<GpuFrameContext> GpuRuntime::CreateFrameContext(GpuSurface* surface, const render::AcquireResult& acquired) {
    if (surface == nullptr || !acquired.BackBuffer.HasValue()) {
        ThrowInvalidArgument("Cannot create a frame context from an invalid acquire result.");
    }

    QueueState& queueState = this->EnsureQueueState(render::QueueType::Direct, surface->_queueSlot);
    auto cmdOpt = _device->CreateCommandBuffer(queueState.Queue);
    if (!cmdOpt.HasValue()) {
        ThrowGpuRuntimeError("GpuRuntime failed to create an internal frame command buffer.");
    }

    unique_ptr<render::CommandBuffer> internalCmdBuffer = cmdOpt.Release();
    internalCmdBuffer->Begin();

    render::Texture* backBuffer = acquired.BackBuffer.Get();
    const uint32_t backBufferIndex = acquired.BackBufferIndex;
    render::TextureState beforeState = render::TextureState::Undefined;
    {
        std::lock_guard<std::mutex> lock(surface->_mutex);
        if (backBufferIndex >= surface->_backBufferStates.size()) {
            ThrowGpuRuntimeError(fmt::format("Back buffer index {} is out of range for BeginFrame.", backBufferIndex));
        }
        beforeState = surface->_backBufferStates[backBufferIndex];
    }

    if (beforeState != render::TextureState::Present) {
        render::ResourceBarrierDescriptor presentBarrier = render::BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = beforeState,
            .After = render::TextureState::Present,
            .OtherQueue = nullptr,
            .IsFromOrToOtherQueue = false,
            .IsSubresourceBarrier = false,
            .Range = render::SubresourceRange{}};
        internalCmdBuffer->ResourceBarrier(std::span{&presentBarrier, 1});
    }
    internalCmdBuffer->End();

    const GpuResourceHandle backBufferHandle = this->EnsureBackBufferHandle(surface, backBufferIndex, backBuffer);
    return unique_ptr<GpuFrameContext>(new GpuFrameContext(
        this,
        surface,
        surface->_queueSlot,
        backBufferHandle,
        backBufferIndex,
        acquired.WaitToDraw,
        acquired.ReadyToPresent,
        std::move(internalCmdBuffer)));
}

GpuResourceHandle GpuRuntime::RegisterHandle(void* nativeHandle) {
    if (nativeHandle == nullptr) {
        return GpuResourceHandle::Invalid();
    }

    std::lock_guard<std::mutex> lock(_mutex);
    const uint64_t handleValue = _nextResourceHandle++;
    _resourceRegistry.emplace(handleValue, ResourceRegistryEntry{nativeHandle});
    return GpuResourceHandle{handleValue, nativeHandle};
}

void GpuRuntime::UnregisterHandle(GpuResourceHandle handle) noexcept {
    if (!handle.IsValid()) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _resourceRegistry.erase(handle.Handle);
}

void GpuRuntime::OnTaskWaited(render::Fence* fence, uint64_t signalValue) noexcept {
    if (fence == nullptr || signalValue == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& queues : _queues) {
        for (auto& state : queues) {
            if (state == nullptr || state->Fence.get() != fence) {
                continue;
            }
            std::lock_guard<std::mutex> queueLock(state->Mutex);
            state->LastWaitedSignalValue = std::max(state->LastWaitedSignalValue, signalValue);
            return;
        }
    }
}

void GpuRuntime::RetireCompletedFrames(GpuSurface* surface) noexcept {
    if (surface == nullptr) {
        return;
    }

    QueueState* queueState = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& queues = _queues[static_cast<size_t>(render::QueueType::Direct)];
        if (surface->_queueSlot < queues.size()) {
            queueState = queues[surface->_queueSlot].get();
        }
    }

    uint64_t lastWaitedSignalValue = 0;
    if (queueState != nullptr) {
        std::lock_guard<std::mutex> queueLock(queueState->Mutex);
        lastWaitedSignalValue = queueState->LastWaitedSignalValue;
    }

    std::lock_guard<std::mutex> lock(surface->_mutex);
    while (!surface->_inFlightFrames.empty()) {
        const auto& pending = surface->_inFlightFrames.front();
        if (pending.SignalValue > lastWaitedSignalValue) {
            break;
        }
        surface->_inFlightFrames.pop_front();
    }
}

GpuResourceHandle GpuRuntime::EnsureBackBufferHandle(GpuSurface* surface, uint32_t backBufferIndex, render::Texture* backBuffer) {
    if (surface == nullptr || backBuffer == nullptr) {
        ThrowInvalidArgument("Back buffer lookup received a null surface or texture.");
    }

    if (backBufferIndex >= surface->_backBuffers.size()) {
        ThrowGpuRuntimeError(fmt::format("Back buffer index {} is out of range.", backBufferIndex));
    }
    if (!surface->_backBuffers[backBufferIndex].IsValid()) {
        surface->_backBuffers[backBufferIndex] = this->RegisterHandle(backBuffer);
    }
    return surface->_backBuffers[backBufferIndex];
}

}  // namespace radray
