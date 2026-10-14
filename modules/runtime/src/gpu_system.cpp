#include <radray/runtime/gpu_system.h>

#include <radray/basic_math.h>
#include <radray/file.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/scope_guard.h>
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/vertex_data.h>
#include <radray/runtime/app_render_context.h>
#include <radray/runtime/window_manager.h>

#include <algorithm>
#include <span>
#include <type_traits>

namespace radray {

bool GpuFlightCommandAllocator::Initialize(render::Device* device, render::CommandQueue* queue) {
    auto storage = device->CreateCommandAllocator(queue);
    if (!storage) return false;
    _storage = storage.Release();
    _device = device;
    return true;
}

render::CommandBuffer* GpuFlightCommandAllocator::Allocate() {
    if (!_applicationRecording) RADRAY_ABORT("command allocation outside an active frame");
    return AllocateInternal();
}

void GpuFlightCommandAllocator::Return(std::span<render::CommandBuffer*> commands) {
    if (!_applicationRecording) RADRAY_ABORT("command return outside an active frame");
    ReturnInternal(commands);
}

render::CommandBuffer* GpuFlightCommandAllocator::AllocateInternal() {
    if (_allocatedCount == _pool.size()) {
        auto commands = _device->CreateCommandBuffer(_storage.get());
        if (!commands) RADRAY_ABORT("command buffer creation failed");
        _pool.push_back({commands.Release()});
    }
    auto& entry = _pool[_allocatedCount++];
    entry.Phase = Entry::State::Recording;
    entry.Commands->Begin();
    return entry.Commands.get();
}

void GpuFlightCommandAllocator::ReturnInternal(std::span<render::CommandBuffer*> commands) {
    for (size_t i = 0; i < commands.size(); ++i) {
        const auto end = _pool.begin() + _allocatedCount;
        const auto found = std::find_if(_pool.begin(), end, [&](const auto& entry) { return entry.Commands.get() == commands[i]; });
        if (found == end || found->Phase != Entry::State::Recording) {
            RADRAY_ABORT("command buffer is foreign, unallocated or already returned");
        }
        if (std::find(commands.begin(), commands.begin() + i, commands[i]) != commands.begin() + i) {
            RADRAY_ABORT("command batch contains a duplicate command buffer");
        }
    }
    for (auto* commandsToReturn : commands) {
        const auto entry = std::find_if(_pool.begin(), _pool.begin() + _allocatedCount,
                                        [&](const auto& item) { return item.Commands.get() == commandsToReturn; });
        commandsToReturn->End();
        entry->Phase = Entry::State::Closed;
    }
}

bool GpuFlightCommandAllocator::HasOutstanding() const noexcept {
    return std::any_of(_pool.begin(), _pool.begin() + _allocatedCount, [](const auto& entry) { return entry.Phase == Entry::State::Recording; });
}

bool GpuFlightCommandAllocator::HasUnregistered() const noexcept {
    return std::any_of(_pool.begin(), _pool.begin() + _allocatedCount, [](const auto& entry) { return entry.Phase == Entry::State::Closed; });
}

void GpuFlightCommandAllocator::Register(std::span<render::CommandBuffer*> commands) {
    const auto end = _pool.begin() + _allocatedCount;
    for (size_t i = 0; i < commands.size(); ++i) {
        const auto found = std::find_if(_pool.begin(), end, [&](const auto& entry) { return entry.Commands.get() == commands[i]; });
        if (found == end || found->Phase != Entry::State::Closed)
            RADRAY_ABORT("registration requires returned, unregistered commands from this flight");
        if (std::find(commands.begin(), commands.begin() + i, commands[i]) != commands.begin() + i)
            RADRAY_ABORT("command batch contains a duplicate command buffer");
    }
    for (auto* command : commands) {
        std::find_if(_pool.begin(), end, [&](const auto& entry) { return entry.Commands.get() == command; })->Phase = Entry::State::Accepted;
    }
}

void GpuFlightCommandAllocator::Discard(std::span<render::CommandBuffer*> commands) {
    if (!_applicationRecording) RADRAY_ABORT("command discard outside an active frame");
    DiscardInternal(commands, false);
}
void GpuFlightCommandAllocator::DiscardInternal(std::span<render::CommandBuffer*> commands, bool accepted) {
    const auto end = _pool.begin() + _allocatedCount;
    for (size_t i = 0; i < commands.size(); ++i) {
        const auto entry = std::find_if(_pool.begin(), end, [&](const auto& value) { return value.Commands.get() == commands[i]; });
        if (entry == end || (accepted ? entry->Phase != Entry::State::Accepted : entry->Phase != Entry::State::Recording && entry->Phase != Entry::State::Closed) ||
            std::find(commands.begin(), commands.begin() + i, commands[i]) != commands.begin() + i) RADRAY_ABORT("Invalid command discard");
    }
    for (auto* command : commands) {
        auto& entry = *std::find_if(_pool.begin(), end, [&](const auto& value) { return value.Commands.get() == command; });
        if (entry.Phase == Entry::State::Recording) command->End();
        entry.Phase = Entry::State::Discarded;
    }
}
void GpuFlightCommandAllocator::MarkSubmitted(std::span<render::CommandBuffer*> commands) {
    const auto end = _pool.begin() + _allocatedCount;
    for (auto* command : commands) {
        const auto entry = std::find_if(_pool.begin(), end, [&](const auto& value) { return value.Commands.get() == command; });
        if (entry == end || entry->Phase != Entry::State::Accepted) RADRAY_ABORT("Only accepted commands may submit");
        entry->Phase = Entry::State::Submitted;
    }
}

void GpuFlightCommandAllocator::ResetForReuse() {
    if (HasOutstanding() || HasUnregistered()) RADRAY_ABORT("cannot reset command allocator with unhandled commands");
    _storage->Reset();
    _allocatedCount = 0;
    _applicationRecording = false;
}

// ═══════════════════════════════════════════════════════════════
//  GpuSystem
// ═══════════════════════════════════════════════════════════════

bool GpuSystem::CompleteFlight(uint32_t flightIndex) {
    auto& flight = *_flights[flightIndex];
    if (!flight.Signal.IsValid() || flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
    }

    // End the completed queue prefix before any later retirement anchor releases its owners.
    const auto completedValue = flight.Signal.Value;
    vector<uint32_t> completed;
    for (uint32_t i = 0; i < _flights.size(); ++i)
        if (_flights[i]->Signal.IsValid() && _flights[i]->Signal.Value <= completedValue) completed.push_back(i);
    std::sort(completed.begin(), completed.end(), [this](uint32_t a, uint32_t b) { return _flights[a]->Signal.Value < _flights[b]->Signal.Value; });
    for (auto i : completed) {
        auto& done = *_flights[i];
        done.CommandAllocator.ResetForReuse();
        if (_frameProfiler) _frameProfiler->Resolve(i);
        done.Signal = FenceSignal::Invalid();
        done.FrameResources->CompleteEpoch();
        _timeline->PublishCompletion({.FlightIndex = i, .OutputsExecuted = done.Rendered, .FrameSerial = done.FrameSerial});
    }
    return true;
}

bool GpuSystem::CompleteFlightIfReady(uint32_t flightIndex, bool wait) {
    if (flightIndex >= _flights.size()) {
        return false;
    }

    FlightSlot& flight = *_flights[flightIndex];
    if (!flight.Signal.IsValid()) {
        return true;
    }
    if (wait) {
        RADRAY_PROFILE_SCOPE_N("GpuSystem::WaitFlightFence");
        flight.Signal.Fence->Wait(flight.Signal.Value);
    }
    return CompleteFlight(flightIndex);
}

GpuFenceSignal GpuSystem::GetFlightGpuSignal(uint32_t flightIndex) const noexcept {
    if (flightIndex >= _flights.size()) {
        return GpuFenceSignal::Invalid();
    }
    return _flights[flightIndex]->Signal;
}

void GpuSystem::BeginUpdateForFlight(uint32_t flightIndex) {
    if (flightIndex >= _flights.size()) {
        return;
    }

    FlightSlot& flight = *_flights[flightIndex];
    if (flight.Recording || flight.Signal.IsValid() || _timeline->IsCompleted(flightIndex, flight.FrameSerial)) RADRAY_ABORT("Release completed frame owners before reusing the flight");
    flight.FrameSerial = _timeline->OpenFrameDomain(flightIndex);
    if (flight.FrameResources) flight.FrameResources->GetHostWrites().Reset();
}

std::chrono::steady_clock::time_point GpuSystem::BeginFrameTiming(uint32_t flightIndex) noexcept {
    RADRAY_ASSERT(!_flights[flightIndex]->Signal.IsValid());
    return _timeline->BeginFrameTiming(flightIndex);
}

// ═════════════════════════════════════════════════════════════════
//  GpuSystem
// ═════════════════════════════════════════════════════════════════

GpuSystem::GpuSystem(FrameTimeline& timeline, uint32_t backBufferCount, uint32_t flightDataCount) noexcept
    : _timeline(&timeline),
      _backBufferCount(backBufferCount),
      _flightDataCount(flightDataCount) {}

unique_ptr<GpuSystem> GpuSystem::TryCreate(const GpuSystemDescriptor& desc, FrameTimeline& timeline, RuntimeStartupResult& startup) {
    if (desc.FlightDataCount != timeline.GetFlightDataCount()) {
        startup.Status = RuntimeStartupStatus::InvalidDescriptor;
        startup.Reason = "GpuSystem flight count must match FrameTimeline";
        return nullptr;
    }
    auto result = unique_ptr<GpuSystem>{new GpuSystem(timeline, desc.BackBufferCount, desc.FlightDataCount)};
    if (!result->Initialize(desc, startup)) return nullptr;
    return result;
}

unique_ptr<GpuSystem> GpuSystem::TryAttachDevice(const GpuSystemAttachmentDescriptor& desc, FrameTimeline& timeline, RuntimeStartupResult& startup) {
    startup = {};
    if (!desc.Device || desc.FlightDataCount == 0 || desc.FlightDataCount != timeline.GetFlightDataCount()) {
        startup.Status = RuntimeStartupStatus::InvalidDescriptor;
        startup.Reason = "GpuSystem attachment requires a device and matching nonzero flight count";
        return nullptr;
    }
    auto result = unique_ptr<GpuSystem>{new GpuSystem(timeline, desc.BackBufferCount, desc.FlightDataCount)};
    if (!result->InitializeDevice(desc.Device, desc.MainQueueIndex, desc.EnableFrameProfiler, startup)) return nullptr;
    return result;
}

bool GpuSystem::Initialize(const GpuSystemDescriptor& desc, RuntimeStartupResult& startup) {
    startup = {};
    const auto fail = [&startup](RuntimeStartupStatus status, std::string_view reason) {
        startup.Status = status;
        startup.Reason = reason;
        return false;
    };
    if (_flightDataCount == 0) return fail(RuntimeStartupStatus::InvalidDescriptor, "GpuSystem requires at least one flight");
    render::DeviceDescriptor deviceDesc = desc.Device;
    if (auto* vk = std::get_if<render::VulkanDeviceDescriptor>(&deviceDesc)) {
#if defined(RADRAY_ENABLE_VULKAN)
        auto instance = render::InstanceVulkan::InitEnv(desc.VulkanInstance);
        if (!instance) return fail(RuntimeStartupStatus::InitializationFailed, "Vulkan instance initialization failed");
        _vulkanInstance = instance.Get();
        if (desc.VulkanInstance.IsEnableDebugLayer && !_vulkanInstance->IsValidationEnabled())
            return fail(RuntimeStartupStatus::ValidationUnavailable, "Vulkan validation layer or debug messenger is unavailable");
        const auto adapter = _vulkanInstance->SelectHighPerformancePhysicalDevice();
        if (!adapter) return fail(RuntimeStartupStatus::NoAdapter, "No Vulkan physical device is available");
        vk->PhysicalDeviceIndex = adapter;
#else
        return fail(RuntimeStartupStatus::BackendNotBuilt, "Vulkan backend was not built");
#endif
    } else if (auto* d3d12 = std::get_if<render::D3D12DeviceDescriptor>(&deviceDesc)) {
#if defined(RADRAY_ENABLE_D3D12)
        if (d3d12->Factory != nullptr)
            return fail(RuntimeStartupStatus::InvalidDescriptor, "GpuSystem owns the DXGI factory");
        auto factory = render::DXGIFactory::Create(desc.DXGIFactory);
        if (!factory) return fail(RuntimeStartupStatus::InitializationFailed, "DXGI factory creation failed");
        _dxgiFactory = factory.Release();
        if (desc.DXGIFactory.IsEnableDebugLayer && !_dxgiFactory->IsValidationEnabled())
            return fail(RuntimeStartupStatus::ValidationUnavailable, "D3D12 debug layer is unavailable");
        const auto adapter = _dxgiFactory->SelectHighPerformanceAdapter();
        if (!adapter) return fail(RuntimeStartupStatus::NoAdapter, "No D3D12 adapter is available");
        d3d12->Factory = _dxgiFactory.Get();
        d3d12->AdapterIndex = adapter;
#else
        return fail(RuntimeStartupStatus::BackendNotBuilt, "D3D12 backend was not built");
#endif
    } else {
        return fail(RuntimeStartupStatus::InvalidDescriptor, "Unsupported render backend");
    }
    auto device = render::Device::Create(deviceDesc);
    if (!device) return fail(RuntimeStartupStatus::InitializationFailed, "GPU device creation failed for an available adapter");
    return InitializeDevice(device.Release(), desc.MainQueueIndex, desc.EnableFrameProfiler, startup);
}

bool GpuSystem::InitializeDevice(shared_ptr<render::Device> device, uint32_t queueIndex, bool enableProfiler, RuntimeStartupResult& startup) {
    const auto fail = [&startup](RuntimeStartupStatus status, std::string_view reason) {
        startup.Status = status;
        startup.Reason = reason;
        return false;
    };
    _device = std::move(device);
    auto queue = _device->GetCommandQueue(render::QueueType::Direct, queueIndex);
    if (!queue) return fail(RuntimeStartupStatus::InitializationFailed, "Direct queue acquisition failed");
    _mainQueue = queue.Get();
    _mainQueueTrack.Queue = _mainQueue;
    auto fence = _device->CreateFence();
    if (!fence) return fail(RuntimeStartupStatus::InitializationFailed, "Main queue fence creation failed");
    _mainQueueTrack.Fence = fence.Release();
    _mainQueueTrack.Fence->SetDebugName("AppMainQueue");
    _flights.reserve(_flightDataCount);
    for (uint32_t i = 0; i < _flightDataCount; ++i) {
        auto flight = make_unique<FlightSlot>();
        if (!flight->CommandAllocator.Initialize(_device.get(), _mainQueue))
            return fail(RuntimeStartupStatus::InitializationFailed, "Flight command allocator creation failed");
        _flights.push_back(std::move(flight));
    }
    if (enableProfiler) {
        _frameProfiler = GpuFrameProfiler::TryCreate(_device.get(), _mainQueue, _flightDataCount);
        if (!_frameProfiler) return fail(RuntimeStartupStatus::InitializationFailed, "GPU frame profiler creation failed");
    }
    startup.Status = RuntimeStartupStatus::Started;
    return true;
}

GpuSystem::~GpuSystem() noexcept {
    for (const auto& flight : _flights) {
        if (flight->Recording || flight->Signal.IsValid() || flight->FrameSerial || !flight->Payloads.empty() || !flight->GpuPayloads.empty()) RADRAY_ABORT("GpuSystem requires explicit resource retirement before destruction");
    }
    _flights.clear();
    _frameProfiler.reset();
    _mainQueueTrack.Fence.reset();
    _mainQueueTrack.Queue = nullptr;
    _mainQueue = nullptr;
    _device.reset();
    _dxgiFactory = nullptr;
    if (_vulkanInstance != nullptr) {
        render::InstanceVulkan::ShutdownEnv();
        _vulkanInstance = nullptr;
    }
}

float GpuSystem::GetLastGpuTimeMs() const noexcept {
    return _frameProfiler != nullptr ? _frameProfiler->GetLastGpuTimeMs() : 0.0f;
}

void GpuSystem::WaitAndRetireFlights() {
    _mainQueue->Wait();
    vector<uint32_t> order;
    for (uint32_t i = 0; i < _flights.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) { return _flights[a]->FrameSerial < _flights[b]->FrameSerial; });
    for (uint32_t flightIndex : order) {
        CompleteFlight(flightIndex);
    }
}

AppFrameContext GpuSystem::BeginFrameRecord(
    uint32_t flightIndex,
    std::chrono::duration<float> deltaTime,
    std::chrono::duration<float> lastFrameLatency,
    bool isInModalLoop,
    bool rendered) {
    if (_timeline->IsStopping()) RADRAY_ABORT("Cannot record after terminal retirement");
    FlightSlot& record = *_flights.at(flightIndex);
    if (record.Recording || record.Signal.IsValid() ||
        _timeline->IsCompleted(flightIndex, record.FrameSerial)) {
        RADRAY_ABORT("cannot begin a flight before its previous recording has retired");
    }
    record.CommandAllocator._applicationRecording = true;
    if (!record.FrameResources) record.FrameResources = make_unique<GpuFrameResources>(_device.get());
    record.FrameResources->BeginEpoch();
    if (!record.FrameSerial) {
        if (std::this_thread::get_id() != _ownerThread) RADRAY_ABORT("GT must open the work domain before RT recording");
        record.FrameSerial = _timeline->OpenFrameDomain(flightIndex);
    }
    record.Recording = true;
    record.Rendered = rendered;
    return AppFrameContext{this, flightIndex, deltaTime, lastFrameLatency, isInModalLoop};
}

void GpuSystem::CheckCanRetainForFrame(uint32_t flightIndex) {
    if (std::this_thread::get_id() != _ownerThread || _timeline->IsStopping() || flightIndex >= _flights.size()) RADRAY_ABORT("Invalid frame retention owner or phase");
    auto& flight = *_flights[flightIndex];
    if (flight.Recording || flight.Signal.IsValid() || _timeline->IsCompleted(flightIndex, flight.FrameSerial)) RADRAY_ABORT("Frame retention requires a writable flight");
    if (!flight.FrameSerial) flight.FrameSerial = _timeline->OpenFrameDomain(flightIndex);
}

void GpuSystem::ReleaseFrameResourcesGT(const FlightCompletion& completion) {
    if (std::this_thread::get_id() != _ownerThread || completion.FlightIndex >= _flights.size()) RADRAY_ABORT("Invalid frame retirement thread or flight");
    auto& flight = *_flights[completion.FlightIndex];
    if (completion.FrameSerial == 0 || flight.FrameSerial != completion.FrameSerial ||
        flight.Recording || flight.Signal.IsValid() || !_timeline->IsCompleted(completion.FlightIndex, completion.FrameSerial)) RADRAY_ABORT("Frame owners require matching real completion");
    flight.Payloads.clear();
    flight.GpuPayloads.clear();
    flight.FrameSerial = 0;
}

void GpuSystem::AbandonUnpublishedResourcesTerminalGT() {
    if (std::this_thread::get_id() != _ownerThread) RADRAY_ABORT("Terminal retirement requires GT");
    for (uint32_t i = 0; i < _flights.size(); ++i) {
        const auto& flight = _flights[i];
        if (flight->Recording || flight->Signal.IsValid() || _timeline->IsCompleted(i, flight->FrameSerial)) RADRAY_ABORT("Complete published resource owners before terminal abandon");
    }
    _timeline->BeginStopping();
    for (auto& flight : _flights) {
        flight->FrameResources.reset();
        flight->Payloads.clear();
        flight->GpuPayloads.clear();
        flight->FrameSerial = 0;
    }
}

void GpuSystem::EndFrameRecordAndSubmit(uint32_t flightIndex) {
    FlightSlot& record = *_flights[flightIndex];
    if (!record.Recording) {
        return;
    }
    SubmitFrame(flightIndex);
}

void GpuSystem::SubmitFrame(uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("GpuSystem::SubmitFrame");
    FlightSlot& record = *_flights.at(flightIndex);
    if (!record.Recording) {
        RADRAY_ABORT("GpuSystem::SubmitFrame called outside an active frame");
    }
    if (record.CommandAllocator.HasOutstanding()) {
        RADRAY_ABORT("cannot submit a flight with unreturned command buffers");
    }
    for (const auto& acquisition : record.Acquisitions) {
        if (!acquisition.Returned) {
            RADRAY_ABORT("cannot submit a flight with an unreturned acquired target");
        }
    }
    if (record.CommandAllocator.HasUnregistered()) RADRAY_ABORT("cannot submit a flight with unregistered command buffers");
    record.CommandAllocator._applicationRecording = false;
    record.Recording = false;
    record.FrameResources->SealEpoch();
    record.FrameResources->GetHostWrites().Flush(*_device);

    const bool isD3D12 = _device->GetBackend() == render::RenderBackend::D3D12;

    render::Fence* frameFence = _mainQueueTrack.Fence.get();
    GpuFenceSignal finalSignal;
    const auto submitQueue = [&](render::CommandQueueSubmitDescriptor desc) {
        const uint64_t value = _mainQueueTrack.NextFenceValue.fetch_add(1, std::memory_order_acq_rel);
        if (value == 0 || value == UINT64_MAX) RADRAY_ABORT("Queue fence value exhausted");
        vector<render::Fence*> signals{desc.SignalFences.begin(), desc.SignalFences.end()};
        vector<uint64_t> values{desc.SignalValues.begin(), desc.SignalValues.end()};
        // Vulkan associates acquire semaphore retirement with the last signal fence.
        signals.push_back(frameFence);
        values.push_back(value);
        desc.SignalFences = signals;
        desc.SignalValues = values;
        record.CommandAllocator.MarkSubmitted(desc.CmdBuffers);
        _mainQueue->Submit(desc);
        finalSignal = {.Fence = frameFence, .Value = value};
    };
    const auto closeCommands = [&](render::CommandBuffer* commands) {
        record.CommandAllocator.ReturnInternal(std::span{&commands, 1});
        record.CommandAllocator.Register(std::span{&commands, 1});
    };
    const auto presentTarget = [](AppFrameTarget& target, bool submitted) {
        const auto result = target._present(std::move(target._frame));
        if (target.OnOutput) target.OnOutput({submitted, result});
        if (target.OnPresent) target.OnPresent(result);
        if (result.Status != render::SwapChainStatus::Success && result.Status != render::SwapChainStatus::RequireRecreate && result.Status != render::SwapChainStatus::RetryLater) {
            RADRAY_ERR_LOG("failed to present swapchain frame: status={}, native={}", result.Status, result.NativeStatusCode);
        }
    };

    vector<render::CommandBuffer*> unconditional;
    const bool profileFrame = _frameProfiler != nullptr && record.Rendered;
    if (profileFrame) {
        auto* commands = record.CommandAllocator.AllocateInternal();
        _frameProfiler->BeginFrame(commands, flightIndex);
        closeCommands(commands);
        unconditional.push_back(commands);
    }

    const auto flushUnconditional = [&] {
        if (unconditional.empty()) return;
        submitQueue({.CmdBuffers = unconditional});
        unconditional.clear();
    };
    for (auto& batch : record.Batches) {
        // Only contiguous unconditional work can share a submission. External synchronization
        // and acquired-target boundaries keep their original ordering and presentation policy.
        if (!batch.Target && batch.SignalFences.empty() && batch.WaitFences.empty()) {
            unconditional.insert(unconditional.end(), batch.CmdBuffers.begin(), batch.CmdBuffers.end());
            continue;
        }
        flushUnconditional();
        const bool dropTarget = batch.Target && !batch.Target->IsPresentable();
        if (dropTarget) {
            record.Rendered = false;
            record.CommandAllocator.DiscardInternal(batch.CmdBuffers, true);
        }
        render::CommandQueueSubmitDescriptor desc{
            .CmdBuffers = dropTarget ? std::span<render::CommandBuffer*>{} : std::span{batch.CmdBuffers},
            .SignalFences = batch.SignalFences,
            .SignalValues = batch.SignalValues,
            .WaitFences = batch.WaitFences,
            .WaitValues = batch.WaitValues};
        Nullable<render::CommandBuffer*> cleanup{nullptr};
        render::CommandBuffer* cleanupBuffers[1];
        render::SwapChainSyncObject* waitSync[1];
        render::SwapChainSyncObject* readySync[1];
        if (batch.Target) {
            auto& target = *batch.Target;
            auto* wait = target._frame.GetWaitToDraw();
            auto* ready = target._frame.GetReadyToPresent();
            if (wait) {
                waitSync[0] = wait;
                desc.WaitToExecute = waitSync;
            }
            if (ready) {
                readySync[0] = ready;
                desc.ReadyToPresent = readySync;
            }
            const auto before = target._initialState();
            if (dropTarget && !isD3D12 && before != render::TextureState::Present) {
                cleanup = record.CommandAllocator.AllocateInternal();
                const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
                    .Target = target.BackBuffer, .Before = before, .After = render::TextureState::Present};
                cleanup->ResourceBarrier(std::span{&barrier, 1});
                closeCommands(cleanup.Get());
                cleanupBuffers[0] = cleanup.Get();
                desc.CmdBuffers = cleanupBuffers;
            }
        }
        submitQueue(desc);
        if (batch.Target) {
            auto& target = *batch.Target;
            batch.OutputCommandsSubmitted = !dropTarget;
            if (!dropTarget || cleanup) target._commitPresentState();
            if (isD3D12) {
                presentTarget(target, batch.OutputCommandsSubmitted);
            }
        }
    }

    if (!isD3D12) {
        for (auto& batch : record.Batches) {
            if (batch.Target) {
                flushUnconditional();
                presentTarget(*batch.Target, batch.OutputCommandsSubmitted);
            }
        }
    }
    // The final signal covers application commands and the presentation tail on the same queue.
    if (profileFrame) {
        auto* commands = record.CommandAllocator.AllocateInternal();
        _frameProfiler->EndFrame(commands, flightIndex);
        closeCommands(commands);
        unconditional.push_back(commands);
    }
    if (!unconditional.empty())
        flushUnconditional();
    else
        submitQueue({});
    record.Signal = finalSignal;
    record.Batches.clear();
    record.Acquisitions.clear();
}

// ══════════════════════════════════════════════
//  AppFrameContext
// ══════════════════════════════════════════════

AppFrameContext::AppFrameContext(
    GpuSystem* gpuSystem,
    uint32_t flightIndex,
    std::chrono::duration<float> deltaTime,
    std::chrono::duration<float> lastFrameLatency,
    bool isInModalLoop) noexcept
    : _gpuSystem(gpuSystem),
      _flightIndex(flightIndex),
      _frameSerial(gpuSystem->_flights[flightIndex]->FrameSerial),
      _deltaTime(deltaTime),
      _lastFrameLatency(lastFrameLatency),
      _isInModalLoop(isInModalLoop) {}

uint64_t AppFrameContext::FrameSerial() const noexcept { return _frameSerial; }

GpuFlightSlot& AppFrameContext::GetRecordingFlight() const noexcept {
    auto& flight = *_gpuSystem->_flights[_flightIndex];
    if (!flight.Recording || flight.FrameSerial != _frameSerial) {
        RADRAY_ABORT("AppFrameContext used outside its active recording");
    }
    return flight;
}

render::CommandBuffer* AppFrameContext::AllocateCommandBuffer() {
    return GetRecordingFlight().CommandAllocator.Allocate();
}

GpuFrameResources& AppFrameContext::GetGpuFrameResources() const noexcept {
    return *GetRecordingFlight().FrameResources;
}

IGpuCmdAllocator& AppFrameContext::GetCmdAllocator() const noexcept {
    return GetRecordingFlight().CommandAllocator;
}

void AppFrameContext::ReturnCommandBuffers(const render::CommandQueueSubmitDescriptor& desc, std::optional<AppFrameTarget> target) {
    RegisterBatch(desc, std::move(target), true);
}

void AppFrameContext::RegisterClosedCommandBuffers(const render::CommandQueueSubmitDescriptor& desc, std::optional<AppFrameTarget> target) {
    RegisterBatch(desc, std::move(target), false);
}

void AppFrameContext::RegisterRequired(std::span<render::CommandBuffer*> commands) {
    RegisterClosedCommandBuffers({.CmdBuffers = commands});
}

void AppFrameContext::RegisterBatch(
    const render::CommandQueueSubmitDescriptor& desc,
    std::optional<AppFrameTarget> target, bool closeCommands) {
    auto& flight = GetRecordingFlight();
    if (!desc.WaitToExecute.empty() || !desc.ReadyToPresent.empty()) {
        RADRAY_ABORT("swapchain synchronization must be supplied through an acquired target");
    }
    if (desc.SignalFences.size() != desc.SignalValues.size() || desc.WaitFences.size() != desc.WaitValues.size()) {
        RADRAY_ABORT("command batch fence/value counts do not match");
    }
    const auto validateFences = [&](std::span<render::Fence*> fences) {
        for (auto* fence : fences) {
            if (fence == nullptr || fence == _gpuSystem->_mainQueueTrack.Fence.get()) {
                RADRAY_ABORT("command batch contains a null or runtime-owned fence");
            }
        }
    };
    validateFences(desc.SignalFences);
    validateFences(desc.WaitFences);
    if (target) {
        if (desc.CmdBuffers.empty()) RADRAY_ABORT("a presentation batch must contain commands");
        if (target->_owner.Get() != _gpuSystem || target->_flightIndex != _flightIndex ||
            target->_frameSerial != _frameSerial || target->_registrationIndex >= flight.Acquisitions.size() ||
            !target->_frame.IsValid()) {
            RADRAY_ABORT("invalid or foreign acquired target");
        }
        const auto& registration = flight.Acquisitions[target->_registrationIndex];
        if (registration.Returned || target->Window != registration.Window ||
            target->ExternalSwapChain.Get() != registration.ExternalSwapChain.Get() ||
            target->BackBuffer != registration.BackBuffer || target->BackBufferView != registration.BackBufferView ||
            target->BackBufferIndex != registration.BackBufferIndex ||
            target->_frame.GetBackBuffer() != registration.BackBuffer || target->_frame.GetBackBufferIndex() != registration.BackBufferIndex) {
            RADRAY_ABORT("acquired target was modified or already returned");
        }
    }
    GpuFlightSubmitBatch batch{
        .CmdBuffers = {desc.CmdBuffers.begin(), desc.CmdBuffers.end()},
        .SignalFences = {desc.SignalFences.begin(), desc.SignalFences.end()},
        .SignalValues = {desc.SignalValues.begin(), desc.SignalValues.end()},
        .WaitFences = {desc.WaitFences.begin(), desc.WaitFences.end()},
        .WaitValues = {desc.WaitValues.begin(), desc.WaitValues.end()},
        .Target = std::move(target)};
    if (closeCommands) flight.CommandAllocator.Return(batch.CmdBuffers);
    flight.CommandAllocator.Register(batch.CmdBuffers);
    if (batch.Target) flight.Acquisitions[batch.Target->_registrationIndex].Returned = true;
    flight.Batches.emplace_back(std::move(batch));
}

std::optional<AppFrameTarget> AppFrameContext::AcquireWindow(AppWindow* window) {
    RADRAY_PROFILE_SCOPE_N("AcquireWindow");
    auto& flight = GetRecordingFlight();
    if (window == nullptr) RADRAY_ABORT("AcquireWindow requires a window");
    for (const auto& registration : flight.Acquisitions) {
        if (registration.Window == window) RADRAY_ABORT("window already acquired in this flight");
    }
    const AppRenderContext renderCtx{
        .FlightIndex = _flightIndex,
        .DeltaTime = _deltaTime,
        .LastFrameLatency = _lastFrameLatency,
        .IsInModalLoop = _isInModalLoop};
    render::SwapChainAcquireResult acquire = window->AcquireNextSwapChainFrame(renderCtx);
    if (acquire.Status != render::SwapChainStatus::Success || !acquire.Frame.has_value()) {
        if (acquire.Status != render::SwapChainStatus::RequireRecreate &&
            acquire.Status != render::SwapChainStatus::RetryLater) {
            RADRAY_ERR_LOG("failed to acquire swapchain frame: status={}, native={}", acquire.Status, acquire.NativeStatusCode);
        }
        return std::nullopt;
    }

    AppFrameTarget target;
    target.Window = window;
    target.IsPresentable = [window] { return window->IsSwapChainPresentable(); };
    target._present = [window](render::SwapChainFrame acquired) { return window->PresentSwapChainFrame(std::move(acquired)); };
    target._frame = std::move(*acquire.Frame);
    target.BackBuffer = target._frame.GetBackBuffer();
    target.BackBufferIndex = target._frame.GetBackBufferIndex();
    const auto backbuffer = target.BackBufferIndex;
    target._initialState = [window, backbuffer] { return window->GetBackBufferState(backbuffer); };
    target._commitPresentState = [window, backbuffer] { window->SetBackBufferState(backbuffer, render::TextureState::Present); };
    target.BackBufferView = window->GetOrCreateBackBufferView(target._frame);
    if (target.BackBufferView == nullptr) {
        RADRAY_ABORT("cannot create a backbuffer view after successful acquire");
    }
    target._owner = _gpuSystem;
    target._flightIndex = _flightIndex;
    target._frameSerial = _frameSerial;
    target._registrationIndex = flight.Acquisitions.size();
    flight.Acquisitions.push_back({target.Window, target.BackBuffer, target.BackBufferView, target.BackBufferIndex});
    return target;
}

AppFrameTarget AppFrameContext::AdoptSwapChainFrame(render::SwapChain* chain, render::SwapChainFrame frame,
                                                    std::function<bool()> isPresentable, std::function<void(render::SwapChainPresentResult)> onPresent) {
    auto& flight = GetRecordingFlight();
    if (!chain || !frame.IsFrom(chain) || !isPresentable || chain->GetDesc().PresentQueue != _gpuSystem->GetMainQueue())
        RADRAY_ABORT("external output requires its main-queue surface's real acquire token and presentability query");
    for (const auto& registration : flight.Acquisitions)
        if (registration.ExternalSwapChain.Get() == chain) RADRAY_ABORT("external surface already acquired in this flight");
    AppFrameTarget target;
    target.ExternalSwapChain = chain;
    target._present = [chain](render::SwapChainFrame acquired) { return chain->Present(std::move(acquired)); };
    target._initialState = [] { return render::TextureStates{render::TextureState::Present}; };
    target._commitPresentState = [] {};
    target.IsPresentable = std::move(isPresentable);
    target.OnPresent = std::move(onPresent);
    target._frame = std::move(frame);
    target.BackBuffer = target._frame.GetBackBuffer();
    target.BackBufferIndex = target._frame.GetBackBufferIndex();
    target._owner = _gpuSystem;
    target._flightIndex = _flightIndex;
    target._frameSerial = _frameSerial;
    target._registrationIndex = flight.Acquisitions.size();
    flight.Acquisitions.push_back({nullptr, target.BackBuffer, nullptr, target.BackBufferIndex, false, chain});
    return target;
}

ResourceUploader& AppFrameContext::GetUploader() const noexcept {
    return GetRecordingFlight().FrameResources->GetUploader();
}

HostWriteBatch& AppFrameContext::GetHostWrites() const noexcept {
    return GetRecordingFlight().FrameResources->GetHostWrites();
}

render::Device* AppFrameContext::GetDevice() const noexcept {
    return _gpuSystem->_device.get();
}

void AppFrameContext::SubmitFrame() {
    GetRecordingFlight();
    _gpuSystem->SubmitFrame(_flightIndex);
}

}  // namespace radray
