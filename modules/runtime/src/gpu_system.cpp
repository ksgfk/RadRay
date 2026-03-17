#include <radray/runtime/gpu_system.h>

#include <algorithm>
#include <array>
#include <utility>

namespace radray {
namespace {

bool IsCompletionPointCompleted(const GpuCompletionPoint& point) noexcept {
    return point.Fence != nullptr && point.Fence->GetCompletedValue() >= point.TargetValue;
}

bool IsCompletionStateValid(const shared_ptr<GpuCompletionState>& completion) noexcept {
    return completion != nullptr && !completion->Points.empty();
}

bool IsCompletionStateCompleted(const shared_ptr<GpuCompletionState>& completion) noexcept {
    return IsCompletionStateValid(completion) &&
           std::all_of(completion->Points.begin(), completion->Points.end(), IsCompletionPointCompleted);
}

void WaitCompletionState(const shared_ptr<GpuCompletionState>& completion) noexcept {
    if (!IsCompletionStateValid(completion)) {
        return;
    }

    for (const auto& point : completion->Points) {
        if (point.Fence != nullptr && !IsCompletionPointCompleted(point)) {
            point.Fence->Wait();
        }
    }
}

shared_ptr<GpuCompletionState> MakeCompletionState() {
    return std::make_shared<GpuCompletionState>();
}

}  // namespace

// ===========================================================================
// GpuTask
// ===========================================================================

bool GpuTask::IsValid() const noexcept {
    return IsCompletionStateValid(_completion);
}

bool GpuTask::IsCompleted() const noexcept {
    return IsCompletionStateCompleted(_completion);
}

void GpuTask::Wait() noexcept {
    WaitCompletionState(_completion);
}

// ===========================================================================
// GpuSurfaceLease
// ===========================================================================

bool GpuSurfaceLease::IsValid() const noexcept {
    return _surface != nullptr && _backBuffer != nullptr;
}

GpuPresentSurface* GpuSurfaceLease::GetSurface() const noexcept {
    return _surface;
}

uint32_t GpuSurfaceLease::GetFrameSlotIndex() const noexcept {
    return _frameSlotIndex;
}

render::Texture* GpuSurfaceLease::GetBackBufferTexture() const noexcept {
    return _backBuffer;
}

// ===========================================================================
// GpuPresentSurface
// ===========================================================================

GpuPresentSurface::~GpuPresentSurface() noexcept {
    Destroy();
}

bool GpuPresentSurface::IsValid() const noexcept {
    return _runtime != nullptr && _swapchain != nullptr && _swapchain->IsValid();
}

bool GpuPresentSurface::Reconfigure(
    uint32_t width,
    uint32_t height,
    std::optional<render::TextureFormat> format,
    std::optional<render::PresentMode> presentMode) noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr || _swapchain == nullptr) {
        return false;
    }

    auto desc = _swapchain->GetDesc();
    desc.Width = width;
    desc.Height = height;
    if (format.has_value()) {
        desc.Format = format.value();
    }
    if (presentMode.has_value()) {
        desc.PresentMode = presentMode.value();
    }

    auto swapchainOpt = _runtime->_device->CreateSwapChain(desc);
    if (!swapchainOpt.HasValue()) {
        return false;
    }

    _swapchain = swapchainOpt.Release();
    _slotRetireStates.clear();
    _slotRetireStates.resize(desc.FlightFrameCount);
    return true;
}

uint32_t GpuPresentSurface::GetWidth() const noexcept {
    return _swapchain != nullptr ? _swapchain->GetDesc().Width : 0;
}

uint32_t GpuPresentSurface::GetHeight() const noexcept {
    return _swapchain != nullptr ? _swapchain->GetDesc().Height : 0;
}

render::TextureFormat GpuPresentSurface::GetFormat() const noexcept {
    return _swapchain != nullptr ? _swapchain->GetDesc().Format : render::TextureFormat::UNKNOWN;
}

render::PresentMode GpuPresentSurface::GetPresentMode() const noexcept {
    return _swapchain != nullptr ? _swapchain->GetDesc().PresentMode : render::PresentMode::FIFO;
}

void GpuPresentSurface::Destroy() noexcept {
    if (_swapchain != nullptr) {
        _swapchain->Destroy();
        _swapchain.reset();
    }
    _slotRetireStates.clear();
    _frameNumber = 0;
    _runtime = nullptr;
}

// ===========================================================================
// GpuFrameContext
// ===========================================================================

GpuSurfaceAcquireResult GpuFrameContext::AcquireSurface(GpuPresentSurface& surface) noexcept {
    GpuSurfaceAcquireResult result{};
    if (_runtime == nullptr || !surface.IsValid()) {
        return result;
    }

    auto acquire = surface._swapchain->AcquireNext();
    if (!acquire.BackBuffer.HasValue()) {
        result.Status = GpuSurfaceAcquireStatus::Unavailable;
        return result;
    }

    const uint32_t slotIndex = surface._swapchain->GetCurrentBackBufferIndex();
    if (slotIndex < surface._slotRetireStates.size() && surface._slotRetireStates[slotIndex] != nullptr) {
        WaitCompletionState(surface._slotRetireStates[slotIndex]);
    }

    auto lease = std::make_unique<GpuSurfaceLease>();
    lease->_surface = &surface;
    lease->_backBuffer = acquire.BackBuffer.Get();
    lease->_waitToDraw = acquire.WaitToDraw;
    lease->_readyToPresent = acquire.ReadyToPresent;
    lease->_frameSlotIndex = slotIndex;

    result.Status = GpuSurfaceAcquireStatus::Success;
    result.Lease = lease.get();
    _surfaceLeases.push_back(std::move(lease));
    return result;
}

GpuSurfaceAcquireResult GpuFrameContext::TryAcquireSurface(GpuPresentSurface& surface) noexcept {
    GpuSurfaceAcquireResult result{};
    if (_runtime == nullptr || !surface.IsValid()) {
        return result;
    }

    auto acquire = surface._swapchain->AcquireNext();
    if (!acquire.BackBuffer.HasValue()) {
        result.Status = GpuSurfaceAcquireStatus::Unavailable;
        return result;
    }

    const uint32_t slotIndex = surface._swapchain->GetCurrentBackBufferIndex();
    if (slotIndex < surface._slotRetireStates.size() &&
        surface._slotRetireStates[slotIndex] != nullptr &&
        !IsCompletionStateCompleted(surface._slotRetireStates[slotIndex])) {
        result.Status = GpuSurfaceAcquireStatus::Unavailable;
        return result;
    }

    auto lease = std::make_unique<GpuSurfaceLease>();
    lease->_surface = &surface;
    lease->_backBuffer = acquire.BackBuffer.Get();
    lease->_waitToDraw = acquire.WaitToDraw;
    lease->_readyToPresent = acquire.ReadyToPresent;
    lease->_frameSlotIndex = slotIndex;

    result.Status = GpuSurfaceAcquireStatus::Success;
    result.Lease = lease.get();
    _surfaceLeases.push_back(std::move(lease));
    return result;
}

bool GpuFrameContext::Present(GpuSurfaceLease& lease) noexcept {
    if (!lease.IsValid() || lease._presentRequested) {
        return false;
    }
    const bool ownedByFrame = std::any_of(
        _surfaceLeases.begin(),
        _surfaceLeases.end(),
        [&lease](const unique_ptr<GpuSurfaceLease>& owned) {
            return owned.get() == &lease;
        });
    if (!ownedByFrame) {
        return false;
    }

    lease._presentRequested = true;
    return true;
}

bool GpuFrameContext::WaitFor(const GpuTask& task) noexcept {
    if (!task.IsValid()) {
        return false;
    }
    _dependencies.push_back(task._completion);
    return true;
}

bool GpuFrameContext::AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (cmd == nullptr || !_submissionPlan.empty() || !_defaultComputeCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _defaultGraphicsCmds.push_back(std::move(cmd));
    return true;
}

bool GpuFrameContext::AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (cmd == nullptr || !_submissionPlan.empty() || !_defaultGraphicsCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _defaultComputeCmds.push_back(std::move(cmd));
    return true;
}

bool GpuFrameContext::AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (cmd == nullptr || !_submissionPlan.empty() || !_defaultGraphicsCmds.empty() || !_defaultComputeCmds.empty()) {
        return false;
    }
    _defaultCopyCmds.push_back(std::move(cmd));
    return true;
}

bool GpuFrameContext::SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept {
    if (!_defaultGraphicsCmds.empty() || !_defaultComputeCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }

    for (const auto& step : steps) {
        if (std::any_of(step.CommandBuffers.begin(), step.CommandBuffers.end(), [](const auto& cmd) { return cmd == nullptr; })) {
            return false;
        }
    }

    _submissionPlan = std::move(steps);
    return true;
}

bool GpuFrameContext::IsEmpty() const noexcept {
    return _surfaceLeases.empty() &&
           _defaultGraphicsCmds.empty() &&
           _defaultComputeCmds.empty() &&
           _defaultCopyCmds.empty() &&
           _submissionPlan.empty();
}

Nullable<unique_ptr<render::CommandBuffer>> GpuFrameContext::CreateGraphicsCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }

    auto queueOpt = _runtime->_device->GetCommandQueue(render::QueueType::Direct);
    if (!queueOpt.HasValue()) {
        return nullptr;
    }
    return _runtime->_device->CreateCommandBuffer(queueOpt.Get());
}

Nullable<unique_ptr<render::CommandBuffer>> GpuFrameContext::CreateComputeCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }

    auto queueOpt = _runtime->_device->GetCommandQueue(render::QueueType::Compute);
    if (!queueOpt.HasValue()) {
        return nullptr;
    }
    return _runtime->_device->CreateCommandBuffer(queueOpt.Get());
}

Nullable<unique_ptr<render::CommandBuffer>> GpuFrameContext::CreateCopyCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }

    auto queueOpt = _runtime->_device->GetCommandQueue(render::QueueType::Copy);
    if (!queueOpt.HasValue()) {
        return nullptr;
    }
    return _runtime->_device->CreateCommandBuffer(queueOpt.Get());
}

// ===========================================================================
// GpuAsyncContext
// ===========================================================================

bool GpuAsyncContext::WaitFor(const GpuTask& task) noexcept {
    if (!task.IsValid()) {
        return false;
    }
    _dependencies.push_back(task._completion);
    return true;
}

bool GpuAsyncContext::AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (cmd == nullptr || !_submissionPlan.empty() || !_defaultComputeCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _defaultGraphicsCmds.push_back(std::move(cmd));
    return true;
}

bool GpuAsyncContext::AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (cmd == nullptr || !_submissionPlan.empty() || !_defaultGraphicsCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _defaultComputeCmds.push_back(std::move(cmd));
    return true;
}

bool GpuAsyncContext::AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (cmd == nullptr || !_submissionPlan.empty() || !_defaultGraphicsCmds.empty() || !_defaultComputeCmds.empty()) {
        return false;
    }
    _defaultCopyCmds.push_back(std::move(cmd));
    return true;
}

bool GpuAsyncContext::SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept {
    if (!_defaultGraphicsCmds.empty() || !_defaultComputeCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }

    for (const auto& step : steps) {
        if (std::any_of(step.CommandBuffers.begin(), step.CommandBuffers.end(), [](const auto& cmd) { return cmd == nullptr; })) {
            return false;
        }
    }

    _submissionPlan = std::move(steps);
    return true;
}

bool GpuAsyncContext::IsEmpty() const noexcept {
    return _defaultGraphicsCmds.empty() &&
           _defaultComputeCmds.empty() &&
           _defaultCopyCmds.empty() &&
           _submissionPlan.empty();
}

Nullable<unique_ptr<render::CommandBuffer>> GpuAsyncContext::CreateGraphicsCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }

    auto queueOpt = _runtime->_device->GetCommandQueue(render::QueueType::Direct);
    if (!queueOpt.HasValue()) {
        return nullptr;
    }
    return _runtime->_device->CreateCommandBuffer(queueOpt.Get());
}

Nullable<unique_ptr<render::CommandBuffer>> GpuAsyncContext::CreateComputeCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }

    auto queueOpt = _runtime->_device->GetCommandQueue(render::QueueType::Compute);
    if (!queueOpt.HasValue()) {
        return nullptr;
    }
    return _runtime->_device->CreateCommandBuffer(queueOpt.Get());
}

Nullable<unique_ptr<render::CommandBuffer>> GpuAsyncContext::CreateCopyCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }

    auto queueOpt = _runtime->_device->GetCommandQueue(render::QueueType::Copy);
    if (!queueOpt.HasValue()) {
        return nullptr;
    }
    return _runtime->_device->CreateCommandBuffer(queueOpt.Get());
}

// ===========================================================================
// GpuRuntime
// ===========================================================================

GpuRuntime::~GpuRuntime() noexcept {
    Destroy();
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const GpuRuntimeDescriptor& desc) noexcept {
    auto runtime = std::make_unique<GpuRuntime>();
    runtime->_backend = desc.Backend;

    render::DeviceDescriptor deviceDesc{};
    if (desc.Backend == render::RenderBackend::Vulkan) {
        render::VulkanInstanceDescriptor instanceDesc{};
        instanceDesc.EngineName = "RadRay";
        instanceDesc.AppName = "RadRay";
        instanceDesc.IsEnableDebugLayer = desc.EnableDebugValidation;
        instanceDesc.IsEnableGpuBasedValid = desc.EnableDebugValidation;
        instanceDesc.LogCallback = desc.LogCallback;
        instanceDesc.LogUserData = desc.LogUserData;

        auto instanceOpt = render::CreateVulkanInstance(instanceDesc);
        if (!instanceOpt.HasValue()) {
            return nullptr;
        }
        runtime->_vkInstance = instanceOpt.Release();

        std::array<render::VulkanCommandQueueDescriptor, 3> queues{
            render::VulkanCommandQueueDescriptor{render::QueueType::Direct, 1},
            render::VulkanCommandQueueDescriptor{render::QueueType::Compute, 1},
            render::VulkanCommandQueueDescriptor{render::QueueType::Copy, 1},
        };
        render::VulkanDeviceDescriptor vkDesc{};
        vkDesc.Queues = queues;
        deviceDesc = vkDesc;
    } else if (desc.Backend == render::RenderBackend::D3D12) {
        render::D3D12DeviceDescriptor d3d12Desc{};
        d3d12Desc.IsEnableDebugLayer = desc.EnableDebugValidation;
        d3d12Desc.IsEnableGpuBasedValid = desc.EnableDebugValidation;
        d3d12Desc.LogCallback = desc.LogCallback;
        d3d12Desc.LogUserData = desc.LogUserData;
        deviceDesc = d3d12Desc;
    } else {
        render::MetalDeviceDescriptor metalDesc{};
        deviceDesc = metalDesc;
    }

    auto deviceOpt = render::CreateDevice(deviceDesc);
    if (!deviceOpt.HasValue()) {
        if (runtime->_vkInstance != nullptr) {
            render::DestroyVulkanInstance(std::move(runtime->_vkInstance));
        }
        return nullptr;
    }
    runtime->_device = deviceOpt.Release();

    auto graphicsFenceOpt = runtime->_device->CreateFence();
    auto computeFenceOpt = runtime->_device->CreateFence();
    auto copyFenceOpt = runtime->_device->CreateFence();
    if (!graphicsFenceOpt.HasValue() || !computeFenceOpt.HasValue() || !copyFenceOpt.HasValue()) {
        runtime->Destroy();
        return nullptr;
    }

    runtime->_graphicsFence = graphicsFenceOpt.Release();
    runtime->_computeFence = computeFenceOpt.Release();
    runtime->_copyFence = copyFenceOpt.Release();
    return runtime;
}

bool GpuRuntime::IsValid() const noexcept {
    return _device != nullptr;
}

void GpuRuntime::Destroy() noexcept {
    _inFlightTasks.clear();

    _graphicsFence.reset();
    _computeFence.reset();
    _copyFence.reset();
    _graphicsNextValue = 1;
    _computeNextValue = 1;
    _copyNextValue = 1;

    if (_device != nullptr) {
        _device->Destroy();
        _device.reset();
    }

    if (_vkInstance != nullptr) {
        render::DestroyVulkanInstance(std::move(_vkInstance));
    }
}

Nullable<unique_ptr<GpuPresentSurface>> GpuRuntime::CreatePresentSurface(
    const GpuPresentSurfaceDescriptor& desc) noexcept {
    if (_device == nullptr) {
        return nullptr;
    }

    auto presentQueueOpt = _device->GetCommandQueue(render::QueueType::Direct);
    if (!presentQueueOpt.HasValue()) {
        return nullptr;
    }

    render::SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = presentQueueOpt.Get();
    swapchainDesc.NativeHandler = desc.NativeWindowHandle;
    swapchainDesc.Width = desc.Width;
    swapchainDesc.Height = desc.Height;
    swapchainDesc.BackBufferCount = desc.BackBufferCount;
    swapchainDesc.FlightFrameCount = desc.FlightFrameCount;
    swapchainDesc.Format = desc.Format;
    swapchainDesc.PresentMode = desc.PresentMode;

    auto swapchainOpt = _device->CreateSwapChain(swapchainDesc);
    if (!swapchainOpt.HasValue()) {
        return nullptr;
    }

    auto surface = std::make_unique<GpuPresentSurface>();
    surface->_runtime = this;
    surface->_swapchain = swapchainOpt.Release();
    surface->_slotRetireStates.resize(desc.FlightFrameCount);
    return surface;
}

Nullable<unique_ptr<GpuFrameContext>> GpuRuntime::BeginFrame() noexcept {
    if (_device == nullptr) {
        return nullptr;
    }

    auto frame = std::make_unique<GpuFrameContext>();
    frame->_runtime = this;
    return frame;
}

Nullable<unique_ptr<GpuAsyncContext>> GpuRuntime::BeginAsync() noexcept {
    if (_device == nullptr) {
        return nullptr;
    }

    auto asyncContext = std::make_unique<GpuAsyncContext>();
    asyncContext->_runtime = this;
    return asyncContext;
}

GpuTask GpuRuntime::Submit(unique_ptr<GpuFrameContext> frame) noexcept {
    if (frame == nullptr || frame->_runtime != this) {
        return {};
    }

    auto getQueue = [this](render::QueueType type) -> Nullable<render::CommandQueue*> {
        return _device != nullptr ? _device->GetCommandQueue(type) : nullptr;
    };
    auto getFence = [this](render::QueueType type) -> shared_ptr<render::Fence> {
        switch (type) {
            case render::QueueType::Direct: return _graphicsFence;
            case render::QueueType::Compute: return _computeFence;
            case render::QueueType::Copy: return _copyFence;
            case render::QueueType::MAX_COUNT: return nullptr;
        }
        return nullptr;
    };
    auto getNextValue = [this](render::QueueType type) -> uint64_t* {
        switch (type) {
            case render::QueueType::Direct: return &_graphicsNextValue;
            case render::QueueType::Compute: return &_computeNextValue;
            case render::QueueType::Copy: return &_copyNextValue;
            case render::QueueType::MAX_COUNT: return nullptr;
        }
        return nullptr;
    };

    auto hasPresentWork = [&]() {
        return std::any_of(
            frame->_surfaceLeases.begin(),
            frame->_surfaceLeases.end(),
            [](const unique_ptr<GpuSurfaceLease>& lease) {
                return lease != nullptr && lease->_presentRequested;
            });
    };

    vector<GpuQueueSubmitStep> plan{};
    if (!frame->_submissionPlan.empty()) {
        plan = std::move(frame->_submissionPlan);
    } else {
        if (!frame->_defaultGraphicsCmds.empty()) {
            GpuQueueSubmitStep step{};
            step.Queue = render::QueueType::Direct;
            step.CommandBuffers = std::move(frame->_defaultGraphicsCmds);
            plan.push_back(std::move(step));
        }
        if (!frame->_defaultComputeCmds.empty()) {
            GpuQueueSubmitStep step{};
            step.Queue = render::QueueType::Compute;
            step.CommandBuffers = std::move(frame->_defaultComputeCmds);
            plan.push_back(std::move(step));
        }
        if (!frame->_defaultCopyCmds.empty()) {
            GpuQueueSubmitStep step{};
            step.Queue = render::QueueType::Copy;
            step.CommandBuffers = std::move(frame->_defaultCopyCmds);
            plan.push_back(std::move(step));
        }
    }

    size_t presentStepIndex = plan.size();
    if (hasPresentWork()) {
        for (size_t i = 0; i < plan.size(); ++i) {
            if (plan[i].Queue == render::QueueType::Direct) {
                presentStepIndex = i;
                break;
            }
        }

        if (presentStepIndex == plan.size()) {
            GpuQueueSubmitStep step{};
            step.Queue = render::QueueType::Direct;
            plan.push_back(std::move(step));
            presentStepIndex = plan.size() - 1;
        }
    }

    if (plan.empty()) {
        return {};
    }

    auto completion = MakeCompletionState();
    vector<GpuCompletionPoint> stepCompletions{};
    stepCompletions.reserve(plan.size());

    for (size_t i = 0; i < plan.size(); ++i) {
        auto queueOpt = getQueue(plan[i].Queue);
        auto fence = getFence(plan[i].Queue);
        auto* nextValue = getNextValue(plan[i].Queue);
        if (!queueOpt.HasValue() || fence == nullptr || nextValue == nullptr) {
            return {};
        }

        vector<render::CommandBuffer*> rawCmdBuffers{};
        rawCmdBuffers.reserve(plan[i].CommandBuffers.size());
        for (const auto& cmd : plan[i].CommandBuffers) {
            if (cmd == nullptr) {
                return {};
            }
            rawCmdBuffers.push_back(cmd.get());
        }

        vector<render::Fence*> waitFences{};
        vector<uint64_t> waitValues{};
        for (const auto& dependency : frame->_dependencies) {
            if (dependency == nullptr) {
                continue;
            }
            for (const auto& point : dependency->Points) {
                if (point.Fence != nullptr) {
                    waitFences.push_back(point.Fence.get());
                    waitValues.push_back(point.TargetValue);
                }
            }
        }
        for (uint32_t waitStepIndex : plan[i].WaitStepIndices) {
            if (waitStepIndex >= stepCompletions.size()) {
                return {};
            }
            waitFences.push_back(stepCompletions[waitStepIndex].Fence.get());
            waitValues.push_back(stepCompletions[waitStepIndex].TargetValue);
        }

        vector<render::SwapChainSyncObject*> waitToExecute{};
        vector<render::SwapChainSyncObject*> readyToPresent{};
        if (i == presentStepIndex) {
            for (const auto& lease : frame->_surfaceLeases) {
                if (lease == nullptr || !lease->_presentRequested) {
                    continue;
                }
                if (lease->_waitToDraw != nullptr) {
                    waitToExecute.push_back(lease->_waitToDraw);
                }
                if (lease->_readyToPresent != nullptr) {
                    readyToPresent.push_back(lease->_readyToPresent);
                }
            }
        }

        uint64_t signalValue = (*nextValue)++;
        render::Fence* signalFence = fence.get();
        render::CommandQueueSubmitDescriptor desc{};
        desc.CmdBuffers = rawCmdBuffers;
        desc.SignalFences = std::span<render::Fence*>(&signalFence, 1);
        desc.SignalValues = std::span<uint64_t>(&signalValue, 1);
        desc.WaitFences = waitFences;
        desc.WaitValues = waitValues;
        desc.WaitToExecute = waitToExecute;
        desc.ReadyToPresent = readyToPresent;
        queueOpt.Get()->Submit(desc);

        GpuCompletionPoint point{fence, signalValue};
        completion->Points.push_back(point);
        stepCompletions.push_back(point);
    }

    for (const auto& lease : frame->_surfaceLeases) {
        if (lease == nullptr || !lease->_presentRequested || lease->_surface == nullptr || lease->_surface->_swapchain == nullptr) {
            continue;
        }

        lease->_surface->_swapchain->Present(lease->_readyToPresent);
        if (lease->_frameSlotIndex >= lease->_surface->_slotRetireStates.size()) {
            lease->_surface->_slotRetireStates.resize(lease->_frameSlotIndex + 1);
        }
        lease->_surface->_slotRetireStates[lease->_frameSlotIndex] = completion;
        ++lease->_surface->_frameNumber;
    }

    GpuTask task{};
    if (IsCompletionStateValid(completion)) {
        task._runtime = this;
        task._completion = completion;
    }
    if (!task.IsValid()) {
        return {};
    }

    frame->_consumed = true;
    _inFlightTasks.push_back(InFlightTask{
        .Completion = completion,
        .Frame = std::move(frame),
        .Async = nullptr,
    });
    return task;
}

GpuTask GpuRuntime::Submit(unique_ptr<GpuAsyncContext> asyncContext) noexcept {
    if (asyncContext == nullptr || asyncContext->_runtime != this) {
        return {};
    }

    auto getQueue = [this](render::QueueType type) -> Nullable<render::CommandQueue*> {
        return _device != nullptr ? _device->GetCommandQueue(type) : nullptr;
    };
    auto getFence = [this](render::QueueType type) -> shared_ptr<render::Fence> {
        switch (type) {
            case render::QueueType::Direct: return _graphicsFence;
            case render::QueueType::Compute: return _computeFence;
            case render::QueueType::Copy: return _copyFence;
            case render::QueueType::MAX_COUNT: return nullptr;
        }
        return nullptr;
    };
    auto getNextValue = [this](render::QueueType type) -> uint64_t* {
        switch (type) {
            case render::QueueType::Direct: return &_graphicsNextValue;
            case render::QueueType::Compute: return &_computeNextValue;
            case render::QueueType::Copy: return &_copyNextValue;
            case render::QueueType::MAX_COUNT: return nullptr;
        }
        return nullptr;
    };

    vector<GpuQueueSubmitStep> plan{};
    if (!asyncContext->_submissionPlan.empty()) {
        plan = std::move(asyncContext->_submissionPlan);
    } else {
        if (!asyncContext->_defaultGraphicsCmds.empty()) {
            GpuQueueSubmitStep step{};
            step.Queue = render::QueueType::Direct;
            step.CommandBuffers = std::move(asyncContext->_defaultGraphicsCmds);
            plan.push_back(std::move(step));
        }
        if (!asyncContext->_defaultComputeCmds.empty()) {
            GpuQueueSubmitStep step{};
            step.Queue = render::QueueType::Compute;
            step.CommandBuffers = std::move(asyncContext->_defaultComputeCmds);
            plan.push_back(std::move(step));
        }
        if (!asyncContext->_defaultCopyCmds.empty()) {
            GpuQueueSubmitStep step{};
            step.Queue = render::QueueType::Copy;
            step.CommandBuffers = std::move(asyncContext->_defaultCopyCmds);
            plan.push_back(std::move(step));
        }
    }

    if (plan.empty()) {
        return {};
    }

    auto completion = MakeCompletionState();
    vector<GpuCompletionPoint> stepCompletions{};
    stepCompletions.reserve(plan.size());

    for (size_t i = 0; i < plan.size(); ++i) {
        auto queueOpt = getQueue(plan[i].Queue);
        auto fence = getFence(plan[i].Queue);
        auto* nextValue = getNextValue(plan[i].Queue);
        if (!queueOpt.HasValue() || fence == nullptr || nextValue == nullptr) {
            return {};
        }

        vector<render::CommandBuffer*> rawCmdBuffers{};
        rawCmdBuffers.reserve(plan[i].CommandBuffers.size());
        for (const auto& cmd : plan[i].CommandBuffers) {
            if (cmd == nullptr) {
                return {};
            }
            rawCmdBuffers.push_back(cmd.get());
        }

        vector<render::Fence*> waitFences{};
        vector<uint64_t> waitValues{};
        for (const auto& dependency : asyncContext->_dependencies) {
            if (dependency == nullptr) {
                continue;
            }
            for (const auto& point : dependency->Points) {
                if (point.Fence != nullptr) {
                    waitFences.push_back(point.Fence.get());
                    waitValues.push_back(point.TargetValue);
                }
            }
        }
        for (uint32_t waitStepIndex : plan[i].WaitStepIndices) {
            if (waitStepIndex >= stepCompletions.size()) {
                return {};
            }
            waitFences.push_back(stepCompletions[waitStepIndex].Fence.get());
            waitValues.push_back(stepCompletions[waitStepIndex].TargetValue);
        }

        uint64_t signalValue = (*nextValue)++;
        render::Fence* signalFence = fence.get();
        render::CommandQueueSubmitDescriptor desc{};
        desc.CmdBuffers = rawCmdBuffers;
        desc.SignalFences = std::span<render::Fence*>(&signalFence, 1);
        desc.SignalValues = std::span<uint64_t>(&signalValue, 1);
        desc.WaitFences = waitFences;
        desc.WaitValues = waitValues;
        queueOpt.Get()->Submit(desc);

        GpuCompletionPoint point{fence, signalValue};
        completion->Points.push_back(point);
        stepCompletions.push_back(point);
    }

    GpuTask task{};
    if (IsCompletionStateValid(completion)) {
        task._runtime = this;
        task._completion = completion;
    }
    if (!task.IsValid()) {
        return {};
    }

    asyncContext->_consumed = true;
    _inFlightTasks.push_back(InFlightTask{
        .Completion = completion,
        .Frame = nullptr,
        .Async = std::move(asyncContext),
    });
    return task;
}

void GpuRuntime::ProcessTasks() noexcept {
    std::erase_if(
        _inFlightTasks,
        [](const InFlightTask& task) {
            return IsCompletionStateCompleted(task.Completion);
        });
}

}  // namespace radray
