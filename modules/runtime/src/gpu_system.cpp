#include <radray/runtime/gpu_system.h>

#include <algorithm>
#include <utility>

namespace radray {

// ---------------------------------------------------------------------------
// GpuTask
// ---------------------------------------------------------------------------

bool GpuTask::IsValid() const noexcept {
    return _runtime != nullptr && _completion != nullptr && !_completion->Points.empty();
}

bool GpuTask::IsCompleted() const noexcept {
    if (!IsValid()) {
        return false;
    }
    for (const auto& point : _completion->Points) {
        if (point.Fence == nullptr) {
            continue;
        }
        if (point.Fence->GetCompletedValue() < point.TargetValue) {
            return false;
        }
    }
    return true;
}

void GpuTask::Wait() noexcept {
    if (!IsValid()) {
        return;
    }
    for (const auto& point : _completion->Points) {
        if (point.Fence == nullptr) {
            continue;
        }
        if (point.Fence->GetCompletedValue() < point.TargetValue) {
            point.Fence->Wait();
        }
    }
}

// ---------------------------------------------------------------------------
// GpuSurfaceLease
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// GpuPresentSurface
// ---------------------------------------------------------------------------

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
    if (!IsValid()) {
        return false;
    }
    // TODO: implement swapchain reconfiguration when render layer supports it
    (void)width;
    (void)height;
    (void)format;
    (void)presentMode;
    return false;
}

uint32_t GpuPresentSurface::GetWidth() const noexcept {
    if (_swapchain == nullptr) {
        return 0;
    }
    return _swapchain->GetDesc().Width;
}

uint32_t GpuPresentSurface::GetHeight() const noexcept {
    if (_swapchain == nullptr) {
        return 0;
    }
    return _swapchain->GetDesc().Height;
}

render::TextureFormat GpuPresentSurface::GetFormat() const noexcept {
    if (_swapchain == nullptr) {
        return render::TextureFormat::UNKNOWN;
    }
    return _swapchain->GetDesc().Format;
}

render::PresentMode GpuPresentSurface::GetPresentMode() const noexcept {
    if (_swapchain == nullptr) {
        return render::PresentMode::FIFO;
    }
    return _swapchain->GetDesc().PresentMode;
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

// ---------------------------------------------------------------------------
// Internal: CreateCommandBufferForQueue (uses only public Device API)
// ---------------------------------------------------------------------------

static Nullable<unique_ptr<render::CommandBuffer>> CreateCommandBufferForQueue(
    render::Device* device, render::QueueType queueType) noexcept {
    if (device == nullptr) {
        return nullptr;
    }
    auto queueOpt = device->GetCommandQueue(queueType, 0);
    if (!queueOpt.HasValue()) {
        return nullptr;
    }
    return device->CreateCommandBuffer(queueOpt.Get());
}

// ---------------------------------------------------------------------------
// GpuFrameContext
// ---------------------------------------------------------------------------

GpuSurfaceAcquireResult GpuFrameContext::AcquireSurface(GpuPresentSurface& surface) noexcept {
    if (_consumed) {
        return {GpuSurfaceAcquireStatus::Lost, nullptr};
    }
    if (!surface.IsValid()) {
        return {GpuSurfaceAcquireStatus::Lost, nullptr};
    }

    // Wait for the flight slot to be retired before reusing
    auto& swapchain = *surface._swapchain;
    uint32_t flightFrameCount = static_cast<uint32_t>(surface._slotRetireStates.size());
    uint32_t flightSlot = static_cast<uint32_t>(surface._frameNumber % flightFrameCount);

    auto& slotState = surface._slotRetireStates[flightSlot];
    if (slotState != nullptr) {
        for (const auto& point : slotState->Points) {
            if (point.Fence != nullptr && point.Fence->GetCompletedValue() < point.TargetValue) {
                point.Fence->Wait();
            }
        }
        slotState.reset();
    }

    render::AcquireResult acquireResult = swapchain.AcquireNext();
    if (!acquireResult.BackBuffer.HasValue()) {
        return {GpuSurfaceAcquireStatus::Lost, nullptr};
    }

    uint32_t backBufferIndex = swapchain.GetCurrentBackBufferIndex();

    auto lease = std::make_unique<GpuSurfaceLease>();
    lease->_surface = &surface;
    lease->_backBuffer = acquireResult.BackBuffer.Get();
    lease->_waitToDraw = acquireResult.WaitToDraw;
    lease->_readyToPresent = acquireResult.ReadyToPresent;
    lease->_frameSlotIndex = backBufferIndex;
    lease->_presentRequested = false;

    auto* leasePtr = lease.get();
    _surfaceLeases.push_back(std::move(lease));

    ++surface._frameNumber;

    return {GpuSurfaceAcquireStatus::Success, leasePtr};
}

GpuSurfaceAcquireResult GpuFrameContext::TryAcquireSurface(GpuPresentSurface& surface) noexcept {
    // For now, delegate to AcquireSurface. When render layer supports non-blocking acquire,
    // this should attempt non-blocking and return Unavailable if not ready.
    return AcquireSurface(surface);
}

bool GpuFrameContext::Present(GpuSurfaceLease& lease) noexcept {
    if (_consumed) {
        return false;
    }
    if (!lease.IsValid()) {
        return false;
    }
    if (lease._presentRequested) {
        return false;
    }
    // Verify the lease belongs to this frame context
    bool ownedByThis = false;
    for (const auto& owned : _surfaceLeases) {
        if (owned.get() == &lease) {
            ownedByThis = true;
            break;
        }
    }
    if (!ownedByThis) {
        return false;
    }
    lease._presentRequested = true;
    return true;
}

bool GpuFrameContext::WaitFor(const GpuTask& task) noexcept {
    if (_consumed) {
        return false;
    }
    if (!task.IsValid()) {
        return false;
    }
    _dependencies.push_back(task._completion);
    return true;
}

bool GpuFrameContext::AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (_consumed || cmd == nullptr) {
        return false;
    }
    if (!_submissionPlan.empty()) {
        return false;
    }
    if (!_defaultComputeCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _defaultGraphicsCmds.push_back(std::move(cmd));
    return true;
}

bool GpuFrameContext::AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (_consumed || cmd == nullptr) {
        return false;
    }
    if (!_submissionPlan.empty()) {
        return false;
    }
    if (!_defaultGraphicsCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _defaultComputeCmds.push_back(std::move(cmd));
    return true;
}

bool GpuFrameContext::AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (_consumed || cmd == nullptr) {
        return false;
    }
    if (!_submissionPlan.empty()) {
        return false;
    }
    if (!_defaultGraphicsCmds.empty() || !_defaultComputeCmds.empty()) {
        return false;
    }
    _defaultCopyCmds.push_back(std::move(cmd));
    return true;
}

bool GpuFrameContext::SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept {
    if (_consumed) {
        return false;
    }
    if (!_defaultGraphicsCmds.empty() || !_defaultComputeCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _submissionPlan = std::move(steps);
    return true;
}

bool GpuFrameContext::IsEmpty() const noexcept {
    return _defaultGraphicsCmds.empty() &&
           _defaultComputeCmds.empty() &&
           _defaultCopyCmds.empty() &&
           _submissionPlan.empty() &&
           _surfaceLeases.empty() &&
           _dependencies.empty();
}

Nullable<unique_ptr<render::CommandBuffer>> GpuFrameContext::CreateGraphicsCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }
    return CreateCommandBufferForQueue(_runtime->_device.get(), render::QueueType::Direct);
}

Nullable<unique_ptr<render::CommandBuffer>> GpuFrameContext::CreateComputeCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }
    return CreateCommandBufferForQueue(_runtime->_device.get(), render::QueueType::Compute);
}

Nullable<unique_ptr<render::CommandBuffer>> GpuFrameContext::CreateCopyCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }
    return CreateCommandBufferForQueue(_runtime->_device.get(), render::QueueType::Copy);
}

// ---------------------------------------------------------------------------
// GpuAsyncContext
// ---------------------------------------------------------------------------

bool GpuAsyncContext::WaitFor(const GpuTask& task) noexcept {
    if (_consumed) {
        return false;
    }
    if (!task.IsValid()) {
        return false;
    }
    _dependencies.push_back(task._completion);
    return true;
}

bool GpuAsyncContext::AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (_consumed || cmd == nullptr) {
        return false;
    }
    if (!_submissionPlan.empty()) {
        return false;
    }
    if (!_defaultComputeCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _defaultGraphicsCmds.push_back(std::move(cmd));
    return true;
}

bool GpuAsyncContext::AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (_consumed || cmd == nullptr) {
        return false;
    }
    if (!_submissionPlan.empty()) {
        return false;
    }
    if (!_defaultGraphicsCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _defaultComputeCmds.push_back(std::move(cmd));
    return true;
}

bool GpuAsyncContext::AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept {
    if (_consumed || cmd == nullptr) {
        return false;
    }
    if (!_submissionPlan.empty()) {
        return false;
    }
    if (!_defaultGraphicsCmds.empty() || !_defaultComputeCmds.empty()) {
        return false;
    }
    _defaultCopyCmds.push_back(std::move(cmd));
    return true;
}

bool GpuAsyncContext::SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept {
    if (_consumed) {
        return false;
    }
    if (!_defaultGraphicsCmds.empty() || !_defaultComputeCmds.empty() || !_defaultCopyCmds.empty()) {
        return false;
    }
    _submissionPlan = std::move(steps);
    return true;
}

bool GpuAsyncContext::IsEmpty() const noexcept {
    return _defaultGraphicsCmds.empty() &&
           _defaultComputeCmds.empty() &&
           _defaultCopyCmds.empty() &&
           _submissionPlan.empty() &&
           _dependencies.empty();
}

Nullable<unique_ptr<render::CommandBuffer>> GpuAsyncContext::CreateGraphicsCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }
    return CreateCommandBufferForQueue(_runtime->_device.get(), render::QueueType::Direct);
}

Nullable<unique_ptr<render::CommandBuffer>> GpuAsyncContext::CreateComputeCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }
    return CreateCommandBufferForQueue(_runtime->_device.get(), render::QueueType::Compute);
}

Nullable<unique_ptr<render::CommandBuffer>> GpuAsyncContext::CreateCopyCommandBuffer() noexcept {
    if (_runtime == nullptr || _runtime->_device == nullptr) {
        return nullptr;
    }
    return CreateCommandBufferForQueue(_runtime->_device.get(), render::QueueType::Copy);
}

// ---------------------------------------------------------------------------
// GpuRuntime
// ---------------------------------------------------------------------------

GpuRuntime::~GpuRuntime() noexcept {
    Destroy();
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const GpuRuntimeDescriptor& desc) noexcept {
    auto runtime = std::make_unique<GpuRuntime>();
    runtime->_backend = desc.Backend;

    // Create Vulkan instance if needed
    if (desc.Backend == render::RenderBackend::Vulkan) {
        render::VulkanInstanceDescriptor vkInsDesc{};
        vkInsDesc.AppName = "RadRay";
        vkInsDesc.AppVersion = 1;
        vkInsDesc.EngineName = "RadRay";
        vkInsDesc.EngineVersion = 1;
        vkInsDesc.IsEnableDebugLayer = desc.EnableDebugValidation;
        vkInsDesc.IsEnableGpuBasedValid = desc.EnableDebugValidation;
        vkInsDesc.LogCallback = desc.LogCallback;
        vkInsDesc.LogUserData = desc.LogUserData;
        auto vkInsOpt = render::CreateVulkanInstance(vkInsDesc);
        if (!vkInsOpt.HasValue()) {
            return nullptr;
        }
        runtime->_vkInstance = vkInsOpt.Release();
    }

    // Create device
    render::DeviceDescriptor deviceDesc;
    switch (desc.Backend) {
        case render::RenderBackend::D3D12: {
            render::D3D12DeviceDescriptor d3d12Desc{};
            d3d12Desc.IsEnableDebugLayer = desc.EnableDebugValidation;
            d3d12Desc.IsEnableGpuBasedValid = desc.EnableDebugValidation;
            d3d12Desc.LogCallback = desc.LogCallback;
            d3d12Desc.LogUserData = desc.LogUserData;
            deviceDesc = d3d12Desc;
            break;
        }
        case render::RenderBackend::Vulkan: {
            render::VulkanDeviceDescriptor vkDevDesc{};
            render::VulkanCommandQueueDescriptor queues[] = {
                {render::QueueType::Direct, 1},
                {render::QueueType::Compute, 1},
                {render::QueueType::Copy, 1},
            };
            vkDevDesc.Queues = queues;
            deviceDesc = vkDevDesc;
            break;
        }
        case render::RenderBackend::Metal: {
            render::MetalDeviceDescriptor metalDesc{};
            deviceDesc = metalDesc;
            break;
        }
        default:
            return nullptr;
    }

    auto deviceOpt = render::CreateDevice(deviceDesc);
    if (!deviceOpt.HasValue()) {
        return nullptr;
    }
    runtime->_device = deviceOpt.Release();

    // Create per-queue fences
    auto graphicsFenceOpt = runtime->_device->CreateFence();
    if (!graphicsFenceOpt.HasValue()) {
        return nullptr;
    }
    runtime->_graphicsFence = shared_ptr<render::Fence>(graphicsFenceOpt.Release());

    auto computeFenceOpt = runtime->_device->CreateFence();
    if (!computeFenceOpt.HasValue()) {
        return nullptr;
    }
    runtime->_computeFence = shared_ptr<render::Fence>(computeFenceOpt.Release());

    auto copyFenceOpt = runtime->_device->CreateFence();
    if (!copyFenceOpt.HasValue()) {
        return nullptr;
    }
    runtime->_copyFence = shared_ptr<render::Fence>(copyFenceOpt.Release());

    return runtime;
}

bool GpuRuntime::IsValid() const noexcept {
    return _device != nullptr && _device->IsValid();
}

void GpuRuntime::Destroy() noexcept {
    // Wait for all in-flight tasks to complete
    for (auto& task : _inFlightTasks) {
        if (task.Completion != nullptr) {
            for (const auto& point : task.Completion->Points) {
                if (point.Fence != nullptr && point.Fence->GetCompletedValue() < point.TargetValue) {
                    point.Fence->Wait();
                }
            }
        }
    }
    _inFlightTasks.clear();

    _graphicsFence.reset();
    _computeFence.reset();
    _copyFence.reset();

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
    if (!IsValid()) {
        return nullptr;
    }

    auto graphicsQueueOpt = _device->GetCommandQueue(render::QueueType::Direct, 0);
    if (!graphicsQueueOpt.HasValue()) {
        return nullptr;
    }

    render::SwapChainDescriptor scDesc{};
    scDesc.PresentQueue = graphicsQueueOpt.Get();
    scDesc.NativeHandler = desc.NativeWindowHandle;
    scDesc.Width = desc.Width;
    scDesc.Height = desc.Height;
    scDesc.BackBufferCount = desc.BackBufferCount;
    scDesc.FlightFrameCount = desc.FlightFrameCount;
    scDesc.Format = desc.Format;
    scDesc.PresentMode = desc.PresentMode;

    auto swapchainOpt = _device->CreateSwapChain(scDesc);
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
    if (!IsValid()) {
        return nullptr;
    }
    auto frame = std::make_unique<GpuFrameContext>();
    frame->_runtime = this;
    return frame;
}

Nullable<unique_ptr<GpuAsyncContext>> GpuRuntime::BeginAsync() noexcept {
    if (!IsValid()) {
        return nullptr;
    }
    auto asyncContext = std::make_unique<GpuAsyncContext>();
    asyncContext->_runtime = this;
    return asyncContext;
}

// ---------------------------------------------------------------------------
// GpuRuntime — submission internals
// ---------------------------------------------------------------------------

render::CommandQueue* GpuRuntime::GetQueueForType(render::QueueType type) const noexcept {
    auto opt = _device->GetCommandQueue(type, 0);
    return opt.HasValue() ? opt.Get() : nullptr;
}

shared_ptr<render::Fence>& GpuRuntime::GetFenceForQueue(render::QueueType queueType) noexcept {
    switch (queueType) {
        case render::QueueType::Compute:
            return _computeFence;
        case render::QueueType::Copy:
            return _copyFence;
        default:
            return _graphicsFence;
    }
}

uint64_t& GpuRuntime::GetNextValueForQueue(render::QueueType queueType) noexcept {
    switch (queueType) {
        case render::QueueType::Compute:
            return _computeNextValue;
        case render::QueueType::Copy:
            return _copyNextValue;
        default:
            return _graphicsNextValue;
    }
}

void GpuRuntime::CollectDependencyWaits(
    const vector<shared_ptr<GpuCompletionState>>& dependencies,
    vector<render::Fence*>& outWaitFences,
    vector<uint64_t>& outWaitValues) noexcept {
    for (const auto& dep : dependencies) {
        if (dep == nullptr) continue;
        for (const auto& point : dep->Points) {
            if (point.Fence != nullptr) {
                outWaitFences.push_back(point.Fence.get());
                outWaitValues.push_back(point.TargetValue);
            }
        }
    }
}

void GpuRuntime::CollectSwapChainSync(
    const vector<unique_ptr<GpuSurfaceLease>>& surfaceLeases,
    vector<render::SwapChainSyncObject*>& outWaitToExecute,
    vector<render::SwapChainSyncObject*>& outReadyToPresent) noexcept {
    for (const auto& lease : surfaceLeases) {
        if (lease == nullptr) continue;
        if (lease->_waitToDraw != nullptr) {
            outWaitToExecute.push_back(lease->_waitToDraw);
        }
        if (lease->_presentRequested && lease->_readyToPresent != nullptr) {
            outReadyToPresent.push_back(lease->_readyToPresent);
        }
    }
}

shared_ptr<GpuCompletionState> GpuRuntime::ExecuteSubmissionPlan(
    vector<GpuQueueSubmitStep>& steps,
    const vector<shared_ptr<GpuCompletionState>>& dependencies,
    const vector<unique_ptr<GpuSurfaceLease>>* surfaceLeases) noexcept {
    auto completion = std::make_shared<GpuCompletionState>();

    // Per-step signal fences and values (for inter-step dependencies)
    vector<render::Fence*> stepSignalFences(steps.size(), nullptr);
    vector<uint64_t> stepSignalValues(steps.size(), 0);

    for (size_t i = 0; i < steps.size(); ++i) {
        auto& step = steps[i];
        render::CommandQueue* queue = GetQueueForType(step.Queue);
        if (queue == nullptr) {
            continue;
        }

        // Gather command buffer raw pointers
        vector<render::CommandBuffer*> cmdPtrs;
        cmdPtrs.reserve(step.CommandBuffers.size());
        for (auto& cmd : step.CommandBuffers) {
            if (cmd != nullptr) {
                cmdPtrs.push_back(cmd.get());
            }
        }

        // Build wait lists
        vector<render::Fence*> waitFences;
        vector<uint64_t> waitValues;

        // Wait for prior steps this step depends on
        for (uint32_t waitIdx : step.WaitStepIndices) {
            if (waitIdx < i && stepSignalFences[waitIdx] != nullptr) {
                waitFences.push_back(stepSignalFences[waitIdx]);
                waitValues.push_back(stepSignalValues[waitIdx]);
            }
        }

        // External dependencies: attach to steps that have no inter-step waits
        if (step.WaitStepIndices.empty()) {
            CollectDependencyWaits(dependencies, waitFences, waitValues);
        }

        // Swap chain sync objects
        vector<render::SwapChainSyncObject*> waitToExecute;
        vector<render::SwapChainSyncObject*> readyToPresent;
        if (surfaceLeases != nullptr) {
            CollectSwapChainSync(*surfaceLeases, waitToExecute, readyToPresent);
        }

        // Signal fence for this step
        auto& fenceRef = GetFenceForQueue(step.Queue);
        render::Fence* signalFence = fenceRef.get();
        uint64_t signalValue = GetNextValueForQueue(step.Queue)++;

        stepSignalFences[i] = signalFence;
        stepSignalValues[i] = signalValue;

        // Submit
        render::CommandQueueSubmitDescriptor desc{};
        desc.CmdBuffers = cmdPtrs;
        desc.SignalFences = std::span<render::Fence*>{&signalFence, 1};
        desc.SignalValues = std::span<uint64_t>{&signalValue, 1};
        if (!waitFences.empty()) {
            desc.WaitFences = waitFences;
            desc.WaitValues = waitValues;
        }
        if (!waitToExecute.empty() && i == 0) {
            desc.WaitToExecute = waitToExecute;
        }
        if (!readyToPresent.empty() && i == steps.size() - 1) {
            desc.ReadyToPresent = readyToPresent;
        }

        queue->Submit(desc);

        completion->Points.push_back(GpuCompletionPoint{fenceRef, signalValue});
    }

    return completion;
}

shared_ptr<GpuCompletionState> GpuRuntime::ExecuteHelperPath(
    render::QueueType queueType,
    vector<unique_ptr<render::CommandBuffer>>& cmds,
    const vector<shared_ptr<GpuCompletionState>>& dependencies,
    const vector<unique_ptr<GpuSurfaceLease>>* surfaceLeases) noexcept {
    render::CommandQueue* queue = GetQueueForType(queueType);
    if (queue == nullptr) {
        return nullptr;
    }

    auto completion = std::make_shared<GpuCompletionState>();

    // Command buffer raw pointers
    vector<render::CommandBuffer*> cmdPtrs;
    cmdPtrs.reserve(cmds.size());
    for (auto& cmd : cmds) {
        if (cmd != nullptr) {
            cmdPtrs.push_back(cmd.get());
        }
    }

    // Dependency waits
    vector<render::Fence*> waitFences;
    vector<uint64_t> waitValues;
    CollectDependencyWaits(dependencies, waitFences, waitValues);

    // Swap chain sync
    vector<render::SwapChainSyncObject*> waitToExecute;
    vector<render::SwapChainSyncObject*> readyToPresent;
    if (surfaceLeases != nullptr) {
        CollectSwapChainSync(*surfaceLeases, waitToExecute, readyToPresent);
    }

    // Signal
    auto& fenceRef = GetFenceForQueue(queueType);
    render::Fence* signalFence = fenceRef.get();
    uint64_t signalValue = GetNextValueForQueue(queueType)++;

    render::CommandQueueSubmitDescriptor desc{};
    desc.CmdBuffers = cmdPtrs;
    desc.SignalFences = std::span<render::Fence*>{&signalFence, 1};
    desc.SignalValues = std::span<uint64_t>{&signalValue, 1};
    if (!waitFences.empty()) {
        desc.WaitFences = waitFences;
        desc.WaitValues = waitValues;
    }
    if (!waitToExecute.empty()) {
        desc.WaitToExecute = waitToExecute;
    }
    if (!readyToPresent.empty()) {
        desc.ReadyToPresent = readyToPresent;
    }

    queue->Submit(desc);

    completion->Points.push_back(GpuCompletionPoint{fenceRef, signalValue});

    return completion;
}

// ---------------------------------------------------------------------------
// GpuRuntime — Submit
// ---------------------------------------------------------------------------

static render::QueueType DetermineHelperQueueType(
    const vector<unique_ptr<render::CommandBuffer>>& graphics,
    const vector<unique_ptr<render::CommandBuffer>>& compute,
    const vector<unique_ptr<render::CommandBuffer>>& copy) noexcept {
    if (!graphics.empty()) return render::QueueType::Direct;
    if (!compute.empty()) return render::QueueType::Compute;
    if (!copy.empty()) return render::QueueType::Copy;
    return render::QueueType::Direct;
}

static vector<unique_ptr<render::CommandBuffer>>& GetHelperCmds(
    vector<unique_ptr<render::CommandBuffer>>& graphics,
    vector<unique_ptr<render::CommandBuffer>>& compute,
    vector<unique_ptr<render::CommandBuffer>>& copy,
    render::QueueType type) noexcept {
    switch (type) {
        case render::QueueType::Compute:
            return compute;
        case render::QueueType::Copy:
            return copy;
        default:
            return graphics;
    }
}

GpuTask GpuRuntime::Submit(unique_ptr<GpuFrameContext> frame) noexcept {
    GpuTask result{};
    if (frame == nullptr || frame->_consumed) {
        return result;
    }
    frame->_consumed = true;

    bool hasCommands = !frame->_defaultGraphicsCmds.empty() ||
                       !frame->_defaultComputeCmds.empty() ||
                       !frame->_defaultCopyCmds.empty() ||
                       !frame->_submissionPlan.empty();

    if (!hasCommands) {
        return result;
    }

    shared_ptr<GpuCompletionState> completion;

    if (!frame->_submissionPlan.empty()) {
        completion = ExecuteSubmissionPlan(
            frame->_submissionPlan, frame->_dependencies, &frame->_surfaceLeases);
    } else {
        render::QueueType queueType = DetermineHelperQueueType(
            frame->_defaultGraphicsCmds, frame->_defaultComputeCmds, frame->_defaultCopyCmds);
        auto& cmds = GetHelperCmds(
            frame->_defaultGraphicsCmds, frame->_defaultComputeCmds, frame->_defaultCopyCmds, queueType);
        completion = ExecuteHelperPath(
            queueType, cmds, frame->_dependencies, &frame->_surfaceLeases);
    }

    if (completion == nullptr || completion->Points.empty()) {
        return result;
    }

    // Present all requested leases
    for (const auto& lease : frame->_surfaceLeases) {
        if (lease != nullptr && lease->_presentRequested && lease->_surface != nullptr) {
            auto& swapchain = *lease->_surface->_swapchain;
            swapchain.Present(lease->_readyToPresent);

            // Update slot retire state
            if (lease->_frameSlotIndex < lease->_surface->_slotRetireStates.size()) {
                lease->_surface->_slotRetireStates[lease->_frameSlotIndex] = completion;
            }
        }
    }

    // Record in-flight task
    InFlightTask inFlight{};
    inFlight.Completion = completion;
    inFlight.Frame = std::move(frame);
    _inFlightTasks.push_back(std::move(inFlight));

    result._runtime = this;
    result._completion = completion;
    return result;
}

GpuTask GpuRuntime::Submit(unique_ptr<GpuAsyncContext> asyncContext) noexcept {
    GpuTask result{};
    if (asyncContext == nullptr || asyncContext->_consumed) {
        return result;
    }
    asyncContext->_consumed = true;

    bool hasCommands = !asyncContext->_defaultGraphicsCmds.empty() ||
                       !asyncContext->_defaultComputeCmds.empty() ||
                       !asyncContext->_defaultCopyCmds.empty() ||
                       !asyncContext->_submissionPlan.empty();

    if (!hasCommands) {
        return result;
    }

    shared_ptr<GpuCompletionState> completion;

    if (!asyncContext->_submissionPlan.empty()) {
        completion = ExecuteSubmissionPlan(
            asyncContext->_submissionPlan, asyncContext->_dependencies, nullptr);
    } else {
        render::QueueType queueType = DetermineHelperQueueType(
            asyncContext->_defaultGraphicsCmds, asyncContext->_defaultComputeCmds, asyncContext->_defaultCopyCmds);
        auto& cmds = GetHelperCmds(
            asyncContext->_defaultGraphicsCmds, asyncContext->_defaultComputeCmds, asyncContext->_defaultCopyCmds, queueType);
        completion = ExecuteHelperPath(
            queueType, cmds, asyncContext->_dependencies, nullptr);
    }

    if (completion == nullptr || completion->Points.empty()) {
        return result;
    }

    InFlightTask inFlight{};
    inFlight.Completion = completion;
    inFlight.Async = std::move(asyncContext);
    _inFlightTasks.push_back(std::move(inFlight));

    result._runtime = this;
    result._completion = completion;
    return result;
}

void GpuRuntime::ProcessTasks() noexcept {
    _inFlightTasks.erase(
        std::remove_if(
            _inFlightTasks.begin(),
            _inFlightTasks.end(),
            [](const InFlightTask& task) {
                if (task.Completion == nullptr) {
                    return true;
                }
                for (const auto& point : task.Completion->Points) {
                    if (point.Fence != nullptr && point.Fence->GetCompletedValue() < point.TargetValue) {
                        return false;
                    }
                }
                return true;
            }),
        _inFlightTasks.end());
}

}  // namespace radray
