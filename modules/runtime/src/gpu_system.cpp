#include <radray/runtime/gpu_system.h>

#include <radray/basic_math.h>
#include <radray/file.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/scope_guard.h>
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/vertex_data.h>
#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>

#include <algorithm>
#include <span>
#include <type_traits>

namespace radray {

render::CommandBuffer* GpuFlightCommandAllocator::Allocate(render::Device* device, render::CommandQueue* queue) {
    if (_allocatedCount == _pool.size()) {
        _pool.push_back({device->CreateCommandBuffer(queue).Unwrap()});
    }
    auto& entry = _pool[_allocatedCount++];
    entry.Returned = false;
    entry.Commands->Begin();
    return entry.Commands.get();
}

void GpuFlightCommandAllocator::Return(std::span<render::CommandBuffer*> commands) {
    for (size_t i = 0; i < commands.size(); ++i) {
        const auto end = _pool.begin() + _allocatedCount;
        const auto found = std::find_if(_pool.begin(), end, [&](const auto& entry) { return entry.Commands.get() == commands[i]; });
        if (found == end || found->Returned) {
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
        entry->Returned = true;
    }
}

bool GpuFlightCommandAllocator::HasOutstanding() const noexcept {
    return std::any_of(_pool.begin(), _pool.begin() + _allocatedCount, [](const auto& entry) { return !entry.Returned; });
}

void GpuFlightCommandAllocator::Reset() {
    if (HasOutstanding()) RADRAY_ABORT("cannot reset command allocator with unreturned commands");
    _allocatedCount = 0;
}

// ═══════════════════════════════════════════════════════════════
//  GpuSystem
// ═══════════════════════════════════════════════════════════════

bool WaitFrameAwaitable::await_ready() const noexcept {
    return _gpuSystem == nullptr || _gpuSystem->_retirementStopping || _stop.stop_requested();
}

bool WaitFrameAwaitable::await_suspend(std::coroutine_handle<> h) {
    if (_gpuSystem == nullptr || _gpuSystem->_retirementStopping || _stop.stop_requested()) {
        return false;
    }
    _record = _gpuSystem->RegisterWaitFrame(_stop, h);
    return _record != nullptr;
}

bool WaitFrameAwaitable::await_resume() noexcept {
    const bool completed = !_stop.stop_requested() && (_record == nullptr || !_record->Canceled) &&
                           (_gpuSystem == nullptr || !_gpuSystem->_retirementStopping);
    if (_record != nullptr && _gpuSystem != nullptr) {
        _gpuSystem->EraseWaitFrame(_record);
    }
    _record = nullptr;
    return completed;
}

task<void> GpuSystem::Wait() {
    stop_token stop = co_await CurrentStopToken();
    if (!co_await WaitFrameAwaitable{this, stop}) co_await StopCurrentTask();
}

WaitFrameRecord* GpuSystem::RegisterWaitFrame(stop_token stop, std::coroutine_handle<> continuation) {
    const uint32_t flightIndex = GetCurrentFlightIndex();
    if (flightIndex >= _flights.size()) {
        return nullptr;
    }
    WaitFrameRecord* record = _flights[flightIndex]->WaitFrame.Enqueue(stop, continuation);
    if (_nextWaitSequence == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Frame waiter sequence exhausted");
    record->Sequence = _nextWaitSequence++;
    record->FlightIndex = flightIndex;
    record->FlightComplete = false;
    return record;
}

void GpuSystem::EraseWaitFrame(WaitFrameRecord* record) noexcept {
    if (record == nullptr || record->FlightIndex >= _flights.size()) {
        return;
    }
    _flights[record->FlightIndex]->WaitFrame.Erase(record);
}

void GpuSystem::MarkCompletedWaitFrames(uint32_t flightIndex) noexcept {
    auto& flight = *_flights[flightIndex];
    if (flight.WaitersCompleted.exchange(false, std::memory_order_acquire)) {
        for (size_t i = 0; i < flight.WaitFrame.Count(); ++i) flight.WaitFrame.At(i)->FlightComplete = true;
    }
}

void GpuSystem::PumpWaitFrame(uint32_t flightIndex) {
    if (flightIndex >= _flights.size()) {
        return;
    }
    if (_pumpingWaiters) RADRAY_ABORT("Cannot reenter frame waiter dispatch");
    _pumpingWaiters = true;
    auto guard = MakeScopeGuard([this]() noexcept { _pumpingWaiters = false; });
    MarkCompletedWaitFrames(flightIndex);
    ManualCoroutineScheduler<WaitFrameRecord>& waiters = _flights[flightIndex]->WaitFrame;
    vector<std::pair<WaitFrameRecord*, uint64_t>> ready;
    const auto boundary = _waitDispatchBoundary.value_or(_nextWaitSequence);
    for (size_t i = 0; i < waiters.Count(); ++i) {
        auto* record = waiters.At(i);
        if (record->Stop.stop_requested()) record->Canceled = true;
        if (record->Sequence < boundary && (record->Canceled || record->FlightComplete)) ready.emplace_back(record, record->Sequence);
    }
    for (auto [record, sequence] : ready) {
        if (!waiters.IsAlive(record) || record->Sequence != sequence) continue;
        waiters.ResumeRecord(record);
        if (waiters.IsAlive(record) && record->Sequence == sequence) waiters.Erase(record);
    }
}

void GpuSystem::CancelAllWaitFrames() noexcept {
    for (unique_ptr<FlightSlot>& flight : _flights) {
        flight->WaitFrame.CancelAll();
    }
}

bool GpuSystem::CompleteFlight(uint32_t flightIndex) {
    auto& flight = *_flights[flightIndex];
    if (!flight.Signal.IsValid() || flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
    }

    const std::chrono::duration<float> latency = std::chrono::steady_clock::now() - flight.FrameStartTime;
    _lastFrameLatencySeconds.store(latency.count(), std::memory_order_relaxed);
    flight.Signal = GpuSystem::FenceSignal::Invalid();
    if (_frameProfiler != nullptr) {
        _frameProfiler->Resolve(flightIndex);
    }
    flight.CompletedFrameSerial.store(flight.FrameSerial, std::memory_order_release);
    [[maybe_unused]] const bool published = _flightCompletions.TryWrite(FlightCompletion{.FlightIndex = flightIndex, .GpuWorkCompleted = flight.Rendered, .FrameSerial = flight.FrameSerial});
    RADRAY_ASSERT(published);
    flight.Uploader->CollectFlight(flightIndex);
    // The coroutine records, including cancellation, remain entirely game-thread owned.
    flight.WaitersCompleted.store(true, std::memory_order_release);
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
        flight.Signal.Fence->Wait(flight.Signal.Value);
    } else if (flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
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
    if (flight.FrameSerial != 0 || !flight.Payloads.empty()) RADRAY_ABORT("Release completed frame owners before reusing the flight");
    flight.HostWrites.Reset();
    // 此刻本 flight 上一轮的 fence 已完成 (runner 拿到可写槽位的前提), 等待者已被
    // CompleteFlight 标记, 且此后到下一次进入本函数之间只有本线程访问该 flight。
    PumpWaitFrame(flightIndex);
}

std::chrono::steady_clock::time_point GpuSystem::BeginFrameTiming(uint32_t flightIndex) noexcept {
    FlightSlot& flight = *_flights[flightIndex];
    RADRAY_ASSERT(!flight.Signal.IsValid());
    flight.FrameStartTime = std::chrono::steady_clock::now();
    return flight.FrameStartTime;
}

// ═════════════════════════════════════════════════════════════════
//  GpuSystem
// ═════════════════════════════════════════════════════════════════

GpuSystem::GpuSystem(const GpuSystemDescriptor& desc)
    : _backBufferCount(desc.BackBufferCount),
      _flightDataCount(desc.FlightDataCount) {
    if (_flightDataCount == 0) RADRAY_ABORT("GpuSystem requires at least one flight");
    render::DeviceDescriptor deviceDesc = desc.Device;
    std::visit(
        [this, &desc](auto& backendDesc) {
            using BackendDescriptor = std::remove_cvref_t<decltype(backendDesc)>;
            if constexpr (std::is_same_v<BackendDescriptor, render::VulkanDeviceDescriptor>) {
                _vulkanInstance = render::InstanceVulkan::InitEnv(desc.VulkanInstance).Unwrap();
            } else if constexpr (std::is_same_v<BackendDescriptor, render::D3D12DeviceDescriptor>) {
                if (backendDesc.Factory != nullptr) {
                    RADRAY_ABORT("GpuSystem owns the DXGI factory; D3D12DeviceDescriptor::Factory must be null");
                }
                _dxgiFactory = render::DXGIFactory::Create(desc.DXGIFactory).Unwrap();
                backendDesc.Factory = _dxgiFactory.Get();
            } else {
                RADRAY_ABORT("unsupported render backend");
            }
        },
        deviceDesc);
    _device = render::Device::Create(deviceDesc).Unwrap();

    _mainQueue = _device->GetCommandQueue(render::QueueType::Direct, desc.MainQueueIndex).Unwrap();
    _mainQueueTrack.Queue = _mainQueue;
    _mainQueueTrack.Fence = _device->CreateFence().Unwrap();
    _mainQueueTrack.Fence->SetDebugName("AppMainQueue");
    _flights.reserve(_flightDataCount);
    for (uint32_t i = 0; i < _flightDataCount; ++i) {
        _flights.push_back(make_unique<FlightSlot>());
    }
    if (desc.EnableFrameProfiler) {
        _frameProfiler = make_unique<GpuFrameProfiler>(_device.get(), _mainQueue, _flightDataCount);
    }
}

GpuSystem::~GpuSystem() noexcept {
    for (const auto& flight : _flights) {
        if (!flight->Payloads.empty()) RADRAY_ABORT("GpuSystem requires explicit resource retirement before destruction");
    }
    // 挂在未提交 flight 上的等待者永远等不到 fence, 必须显式取消 —— 取消会就地恢复它们,
    // 让它们在自己的作用域里析构所持有的 GPU 对象。
    //
    // 【为何不靠 ManualCoroutineScheduler 自己的析构】: 那要等到 _flights.clear() 逐个销毁
    // 槽位时才发生, 于是前一个槽位的命令池已经死了而后一个槽位的等待者才刚恢复。
    // 显式先取消全部, 把"恢复"与"拆 flight"分成两个不重叠的阶段。
    CancelAllWaitFrames();
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

uint32_t GpuSystem::GetCurrentFlightIndex() const noexcept {
    return static_cast<uint32_t>(_nowFrameIndex % _flightDataCount);
}

void GpuSystem::WaitAndRetireFlights() {
    _mainQueue->Wait();
    vector<uint32_t> order;
    for (uint32_t i = 0; i < _flights.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) { return _flights[a]->FrameSerial < _flights[b]->FrameSerial; });
    for (uint32_t flightIndex : order) {
        CompleteFlight(flightIndex);
        MarkCompletedWaitFrames(flightIndex);
    }
}

void GpuSystem::CleanupCompletedFlights() {
    if (_waitDispatchBoundary || _pumpingWaiters) RADRAY_ABORT("Cannot reenter completion notification batch");
    _waitDispatchBoundary = _nextWaitSequence;
    auto boundaryGuard = MakeScopeGuard([this]() noexcept { _waitDispatchBoundary.reset(); });
    // 队列已 idle,故所有【已提交】flight 的等待者都已就绪。此处恢复它们,让延迟销毁的
    // GPU 对象在正常路径上归还。挂在未提交 flight 上的记录等不到 fence,留给析构里的
    // CancelAllWaitFrames。
    //
    // 调用方已等待 render/GPU idle；所有等待表只在 GT 恢复。
    for (uint32_t flightIndex = 0; flightIndex < _flights.size(); ++flightIndex) {
        PumpWaitFrame(flightIndex);
    }
}

AppFrameContext GpuSystem::BeginFrameRecord(
    uint32_t flightIndex,
    std::chrono::duration<float> deltaTime,
    std::chrono::duration<float> lastFrameLatency,
    bool isInModalLoop,
    bool rendered) {
    if (_retirementStopping) RADRAY_ABORT("Cannot record after terminal retirement");
    FlightSlot& record = *_flights.at(flightIndex);
    if (record.FrameSerial != 0 || record.Recording || record.Signal.IsValid()) {
        RADRAY_ABORT("cannot begin a flight before its previous recording has retired");
    }
    record.CommandAllocator.Reset();
    record.Batches.clear();
    record.Acquisitions.clear();
    if (!record.Uploader) record.Uploader = make_unique<ResourceUploader>(_device.get(), _flightDataCount);
    record.Uploader->BeginFlight(flightIndex, record.HostWrites);
    record.Submitted = false;
    record.FrameSerial = _nextFrameSerial++;
    if (record.FrameSerial == 0 || record.FrameSerial == UINT64_MAX) RADRAY_ABORT("Frame serial exhausted");
    record.Recording = true;
    record.Rendered = rendered;
    return AppFrameContext{this, flightIndex, deltaTime, lastFrameLatency, isInModalLoop};
}

void GpuSystem::CheckCanRetainForFrame(uint32_t flightIndex) const {
    if (std::this_thread::get_id() != _ownerThread || _retirementStopping || flightIndex >= _flights.size()) RADRAY_ABORT("Invalid frame retention owner or phase");
    const auto& flight = *_flights[flightIndex];
    if (flight.FrameSerial != 0 || flight.Recording || flight.Signal.IsValid()) RADRAY_ABORT("Frame retention requires a writable flight");
}

void GpuSystem::ReleaseFrameResourcesGT(const FlightCompletion& completion) {
    if (std::this_thread::get_id() != _ownerThread || completion.FlightIndex >= _flights.size()) RADRAY_ABORT("Invalid frame retirement thread or flight");
    auto& flight = *_flights[completion.FlightIndex];
    if (completion.FrameSerial == 0 || flight.FrameSerial != completion.FrameSerial ||
        flight.CompletedFrameSerial.load(std::memory_order_acquire) != completion.FrameSerial) RADRAY_ABORT("Frame owners require matching real completion");
    flight.Payloads.clear();
    flight.FrameSerial = 0;
}

void GpuSystem::AbandonUnpublishedResourcesTerminalGT() {
    if (std::this_thread::get_id() != _ownerThread) RADRAY_ABORT("Terminal retirement requires GT");
    for (const auto& flight : _flights) {
        if (flight->FrameSerial != 0 || flight->Recording || flight->Signal.IsValid()) RADRAY_ABORT("Complete published resource owners before terminal abandon");
    }
    _retirementStopping = true;
    for (auto& flight : _flights) flight->Payloads.clear();
}

void GpuSystem::EndFrameRecordAndSubmit(uint32_t flightIndex) {
    FlightSlot& record = *_flights[flightIndex];
    if (record.Submitted || !record.Recording) {
        return;
    }
    SubmitFrame(flightIndex);
}

void GpuSystem::SubmitFrame(uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("GpuSystem::SubmitFrame");
    FlightSlot& record = *_flights.at(flightIndex);
    if (!record.Recording || record.Submitted) {
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
    record.Recording = false;
    record.Uploader->EndFlight(flightIndex);
    record.HostWrites.Flush(*_device);

    const bool isD3D12 = _device->GetBackend() == render::RenderBackend::D3D12;
    const bool dropApplicationWork = std::any_of(record.Batches.begin(), record.Batches.end(), [](const auto& batch) {
        return batch.Target && !batch.Target->Window->IsSwapChainPresentable();
    });
    if (dropApplicationWork) {
        RADRAY_WARN_LOG("skip frame GPU work: an acquired window became unpresentable before submit");
        record.Rendered = false;
    }

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
        _mainQueue->Submit(desc);
        finalSignal = {.Fence = frameFence, .Value = value};
    };
    const auto closeCommands = [&](render::CommandBuffer* commands) {
        record.CommandAllocator.Return(std::span{&commands, 1});
    };
    const auto presentTarget = [](AppFrameTarget& target) {
        const auto result = target.Window->PresentSwapChainFrame(std::move(target._frame));
        if (result.Status != render::SwapChainStatus::Success && result.Status != render::SwapChainStatus::RequireRecreate) {
            RADRAY_ERR_LOG("failed to present swapchain frame: status={}, native={}", result.Status, result.NativeStatusCode);
        }
    };

    const bool profileFrame = _frameProfiler != nullptr && record.Rendered;
    if (profileFrame) {
        auto* commands = record.CommandAllocator.Allocate(_device.get(), _mainQueue);
        _frameProfiler->BeginFrame(commands, flightIndex);
        closeCommands(commands);
        submitQueue({.CmdBuffers = std::span{&commands, 1}});
    }

    for (auto& batch : record.Batches) {
        render::CommandQueueSubmitDescriptor desc{
            .CmdBuffers = dropApplicationWork ? std::span<render::CommandBuffer*>{} : std::span{batch.CmdBuffers},
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
            const auto before = target.Window->GetBackBufferState(target.BackBufferIndex);
            if (dropApplicationWork && !isD3D12 && before != render::TextureState::Present) {
                cleanup = record.CommandAllocator.Allocate(_device.get(), _mainQueue);
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
            if (!dropApplicationWork || cleanup) {
                target.Window->SetBackBufferState(target.BackBufferIndex, render::TextureState::Present);
            }
            if (isD3D12) {
                presentTarget(target);
            }
        }
    }

    // A final submission covers all application batches, even for an empty or skipped flight.
    if (profileFrame) {
        auto* commands = record.CommandAllocator.Allocate(_device.get(), _mainQueue);
        _frameProfiler->EndFrame(commands, flightIndex);
        closeCommands(commands);
        submitQueue({.CmdBuffers = std::span{&commands, 1}});
    } else {
        submitQueue({});
    }
    if (!isD3D12) {
        for (auto& batch : record.Batches) {
            if (batch.Target) presentTarget(*batch.Target);
        }
    }
    record.HostWrites.Seal();
    record.Signal = finalSignal;
    record.Batches.clear();
    record.Acquisitions.clear();
    record.Submitted = true;
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
    if (!flight.Recording || flight.Submitted || flight.FrameSerial != _frameSerial) {
        RADRAY_ABORT("AppFrameContext used outside its active recording");
    }
    return flight;
}

render::CommandBuffer* AppFrameContext::AllocateCommandBuffer() {
    return GetRecordingFlight().CommandAllocator.Allocate(_gpuSystem->_device.get(), _gpuSystem->_mainQueue);
}

void AppFrameContext::ReturnCommandBuffers(
    const render::CommandQueueSubmitDescriptor& desc,
    std::optional<AppFrameTarget> target) {
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
    flight.CommandAllocator.Return(batch.CmdBuffers);
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
    target._frame = std::move(*acquire.Frame);
    target.BackBuffer = target._frame.GetBackBuffer();
    target.BackBufferIndex = target._frame.GetBackBufferIndex();
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

ResourceUploader& AppFrameContext::GetUploader() const noexcept {
    return *GetRecordingFlight().Uploader;
}

HostWriteBatch& AppFrameContext::GetHostWrites() const noexcept {
    return GetRecordingFlight().HostWrites;
}

render::Device* AppFrameContext::GetDevice() const noexcept {
    return _gpuSystem->_device.get();
}

void AppFrameContext::SubmitFrame() {
    GetRecordingFlight();
    _gpuSystem->SubmitFrame(_flightIndex);
}

}  // namespace radray
