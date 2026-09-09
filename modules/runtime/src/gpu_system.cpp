#include <radray/runtime/gpu_system.h>

#include <radray/basic_math.h>
#include <radray/file.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/vertex_data.h>
#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>

#include <cstring>
#include <algorithm>
#include <span>
#include <type_traits>

namespace radray {

// ═══════════════════════════════════════════════════════════════
//  FrameUploadScheduler
// ═══════════════════════════════════════════════════════════════

FrameUploadScheduler::~FrameUploadScheduler() noexcept {
    _uploads.CancelAll();
}

task<FrameUploadScope> FrameUploadScheduler::BeginUpload() {
    stop_token stop = co_await CurrentStopToken();
    std::optional<FrameUploadScope> frame = co_await BeginFrameUploadAwaitable{this, stop};
    if (!frame.has_value()) {
        co_await StopCurrentTask();
        co_return FrameUploadScope{};
    }
    co_return frame.value();
}

FrameUploadRecord* FrameUploadScheduler::RegisterUpload(stop_token stop, std::coroutine_handle<> continuation) {
    FrameUploadRecord* record = _uploads.Enqueue(stop, continuation);
    record->Cmd = nullptr;
    record->Uploader = nullptr;
    record->FlightIndex = std::numeric_limits<uint32_t>::max();
    record->CurrentStage = FrameUploadStage::AwaitingFrame;
    return record;
}

bool FrameUploadScheduler::EraseUpload(FrameUploadRecord* record) noexcept {
    return _uploads.Erase(record);
}

bool FrameUploadScheduler::IsUploadAlive(FrameUploadRecord* record) const noexcept {
    return _uploads.IsAlive(record);
}

void FrameUploadScheduler::ResumeRecord(FrameUploadRecord* record) {
    _uploads.ResumeRecord(record);
}

void FrameUploadScheduler::CancelRecord(FrameUploadRecord* record) noexcept {
    _uploads.CancelRecord(record);
}

void FrameUploadScheduler::RunUploadPhase(
    render::CommandBuffer* cmdBuffer,
    ResourceUploader& uploader,
    uint32_t flightIndex) {
    RADRAY_ASSERT(!_recordingUploads);
    _recordingUploads = true;
    struct ExitPhase {
        bool& Recording;
        ~ExitPhase() { Recording = false; }
    } exitPhase{_recordingUploads};
    vector<FrameUploadRecord*> pending;
    const size_t uploadCount = _uploads.Count();
    for (size_t i = 0; i < uploadCount; ++i) {
        FrameUploadRecord* record = _uploads.At(i);
        if (record != nullptr && record->CurrentStage == FrameUploadStage::AwaitingFrame) {
            pending.push_back(record);
        }
    }

    for (FrameUploadRecord* rec : pending) {
        if (!IsUploadAlive(rec)) {
            continue;
        }
        if (rec->Canceled || rec->Stop.stop_requested()) {
            rec->Canceled = true;
            ResumeRecord(rec);
            if (IsUploadAlive(rec)) {
                EraseUpload(rec);
            }
            continue;
        }

        rec->Cmd = cmdBuffer;
        rec->Uploader = &uploader;
        rec->FlightIndex = flightIndex;
        rec->CurrentStage = FrameUploadStage::InFrame;
        rec->ResumeOnCancel = false;

        ResumeRecord(rec);

        if (!IsUploadAlive(rec)) {
            continue;
        }
        if (rec->CurrentStage != FrameUploadStage::AwaitingFence) {
            EraseUpload(rec);
            continue;
        }
    }
}

void FrameUploadScheduler::ApplyCompletedFlights(std::span<const FlightCompletion> completions) {
    if (completions.empty()) {
        return;
    }
    const size_t uploadCount = _uploads.Count();
    for (size_t i = 0; i < uploadCount; ++i) {
        FrameUploadRecord* rec = _uploads.At(i);
        if (rec == nullptr) {
            continue;
        }
        if (rec->CurrentStage == FrameUploadStage::AwaitingFence &&
            std::find_if(completions.begin(), completions.end(), [rec](const FlightCompletion& completion) {
                return completion.FlightIndex == rec->FlightIndex;
            }) != completions.end()) {
            rec->CurrentStage = FrameUploadStage::FenceComplete;
        }
    }
}

void FrameUploadScheduler::PumpCompletedUploads() {
    RADRAY_ASSERT(!_recordingUploads);
    bool resumedAny = true;
    while (resumedAny) {
        resumedAny = false;
        for (size_t i = 0; i < _uploads.Count();) {
            FrameUploadRecord* rec = _uploads.At(i);
            if (rec->Stop.stop_requested()) {
                rec->Canceled = true;
            }
            if ((rec->Canceled && rec->CurrentStage == FrameUploadStage::AwaitingFrame) ||
                rec->CurrentStage == FrameUploadStage::FenceComplete) {
                ResumeRecord(rec);
                if (IsUploadAlive(rec)) {
                    EraseUpload(rec);
                }
                resumedAny = true;
                break;
            }
            ++i;
        }
    }
}

bool WaitFrameUploadGpuAwaitable::await_ready() noexcept {
    if (_record == nullptr) {
        return true;
    }
    if (_record->Stop.stop_requested()) {
        _record->Canceled = true;
    }
    return _record->CurrentStage == FrameUploadStage::FenceComplete;
}

bool WaitFrameUploadGpuAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    if (_record == nullptr) {
        return false;
    }
    if (_record->Stop.stop_requested()) {
        _record->Canceled = true;
    }
    _record->Continuation = h;
    _record->CurrentStage = FrameUploadStage::AwaitingFence;
    return true;
}

bool WaitFrameUploadGpuAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        return false;
    }
    bool completed = !_record->Canceled && _record->CurrentStage == FrameUploadStage::FenceComplete;
    if (_scheduler != nullptr) {
        _scheduler->EraseUpload(_record);
    }
    _record = nullptr;
    return completed;
}

render::CommandBuffer* FrameUploadScope::GetCommandBuffer() const noexcept {
    return _record->Cmd;
}

ResourceUploader& FrameUploadScope::GetUploader() const noexcept {
    return *_record->Uploader;
}

uint32_t FrameUploadScope::GetFlightIndex() const noexcept {
    return _record->FlightIndex;
}

task<void> FrameUploadScope::WaitGpu() {
    bool completed = co_await WaitFrameUploadGpuAwaitable{_scheduler, _record};
    if (!completed) {
        co_await StopCurrentTask();
    }
}

bool BeginFrameUploadAwaitable::await_ready() const noexcept {
    return _scheduler == nullptr || _stop.stop_requested();
}

bool BeginFrameUploadAwaitable::await_suspend(std::coroutine_handle<> h) {
    if (_scheduler == nullptr || _stop.stop_requested()) {
        return false;
    }
    _record = _scheduler->RegisterUpload(_stop, h);
    return true;
}

std::optional<FrameUploadScope> BeginFrameUploadAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        return std::nullopt;
    }
    if (_record->Canceled || _record->Stop.stop_requested()) {
        _record->Canceled = true;
        if (_scheduler != nullptr) {
            _scheduler->EraseUpload(_record);
        }
        _record = nullptr;
        return std::nullopt;
    }
    return FrameUploadScope{_scheduler, _record};
}

// ═══════════════════════════════════════════════════════════════
//  GpuSystem
// ═══════════════════════════════════════════════════════════════

bool WaitFrameAwaitable::await_ready() const noexcept {
    return _gpuSystem == nullptr || _stop.stop_requested();
}

bool WaitFrameAwaitable::await_suspend(std::coroutine_handle<> h) {
    if (_gpuSystem == nullptr || _stop.stop_requested()) {
        return false;
    }
    _record = _gpuSystem->RegisterWaitFrame(_stop, h);
    return _record != nullptr;
}

void WaitFrameAwaitable::await_resume() noexcept {
    // 【取消与正常完成走同一条出口】: 两种情况下调用方要做的事完全相同 —— 销毁它捕获的
    // 数据。取消发生在关停路径上, 那时 device 尚未销毁 (见 IWaitFrameProcessor 的取消说明),
    // 故不需要区分。Wait() 因此不必返回 bool。
    if (_record != nullptr && _gpuSystem != nullptr) {
        _gpuSystem->EraseWaitFrame(_record);
    }
    _record = nullptr;
}

task<void> GpuSystem::Wait() {
    stop_token stop = co_await CurrentStopToken();
    co_await WaitFrameAwaitable{this, stop};
}

WaitFrameRecord* GpuSystem::RegisterWaitFrame(stop_token stop, std::coroutine_handle<> continuation) {
    const uint32_t flightIndex = GetCurrentFlightIndex();
    if (flightIndex >= _flights.size()) {
        return nullptr;
    }
    WaitFrameRecord* record = _flights[flightIndex]->WaitFrame.Enqueue(stop, continuation);
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

void GpuSystem::PumpWaitFrame(uint32_t flightIndex) {
    if (flightIndex >= _flights.size()) {
        return;
    }
    ManualCoroutineScheduler<WaitFrameRecord>& waiters = _flights[flightIndex]->WaitFrame;
    if (_flights[flightIndex]->WaitersCompleted.exchange(false, std::memory_order_acquire)) {
        for (size_t i = 0; i < waiters.Count(); ++i) waiters.At(i)->FlightComplete = true;
    }
    // 每轮从头重扫: 恢复一条记录会跑调用方的代码, 它可能新增或摘除记录。
    bool resumedAny = true;
    while (resumedAny) {
        resumedAny = false;
        for (size_t i = 0; i < waiters.Count();) {
            WaitFrameRecord* rec = waiters.At(i);
            if (rec->Stop.stop_requested()) {
                rec->Canceled = true;
            }
            if (rec->Canceled || rec->FlightComplete) {
                waiters.ResumeRecord(rec);
                if (waiters.IsAlive(rec)) {
                    waiters.Erase(rec);
                }
                resumedAny = true;
                break;
            }
            ++i;
        }
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
    for (const auto& submission : flight.Submissions) submission->Complete(flight.FrameSerial, flight.Rendered);
    flight.Submissions.clear();
    NotifyFlightComplete(FlightCompletion{.FlightIndex = flightIndex, .GpuWorkCompleted = flight.Rendered, .FrameSerial = flight.FrameSerial});
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

    PumpFlightCompletions();
    FlightSlot& flight = *_flights[flightIndex];
    flight.HostWrites.Reset();
    flight.FrameStartTime = std::chrono::steady_clock::now();
    // 此刻本 flight 上一轮的 fence 已完成 (runner 拿到可写槽位的前提), 等待者已被
    // CompleteFlight 标记, 且此后到下一次进入本函数之间只有本线程访问该 flight。
    PumpWaitFrame(flightIndex);
    PumpFrameUploadScheduler();
}

// ═════════════════════════════════════════════════════════════════
//  GpuFrameProfiler
// ═════════════════════════════════════════════════════════════════

GpuFrameProfiler::GpuFrameProfiler(render::Device* device, render::CommandQueue* queue, uint32_t flightCount)
    : _queue(queue) {
    // Vulkan 需要在 readback copy 前后显式 transition;D3D12 READBACK heap 始终处于 COPY_DEST。
    _readbackNeedsBarrier = device->GetBackend() == render::RenderBackend::Vulkan;
    _frames.resize(flightCount);
    for (FrameTiming& frame : _frames) {
        render::QueryPoolDescriptor poolDesc{
            .Type = render::QueryType::Timestamp,
            .Count = TimestampQueryCount,
            .DebugName = "GpuFrameProfiler Timestamp Pool"};
        frame.Pool = device->CreateQueryPool(poolDesc).Unwrap();

        render::BufferDescriptor readbackDesc{
            .Size = sizeof(uint64_t) * TimestampQueryCount,
            .Memory = render::MemoryType::ReadBack,
            .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead};
        frame.Readback = device->CreateBuffer(readbackDesc).Unwrap();
    }
}

GpuFrameProfiler::~GpuFrameProfiler() noexcept = default;

void GpuFrameProfiler::BeginFrame(render::CommandBuffer* cmdBuffer, uint32_t flightIndex) {
    if (cmdBuffer == nullptr || flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    cmdBuffer->ResetQueryPool(frame.Pool.get(), 0, TimestampQueryCount);
    cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
        .Pool = frame.Pool.get(),
        .Stage = render::QueryPipelineStage::Top,
        .Index = 0});
}

void GpuFrameProfiler::EndFrame(render::CommandBuffer* cmdBuffer, uint32_t flightIndex) {
    if (cmdBuffer == nullptr || flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
        .Pool = frame.Pool.get(),
        .Stage = render::QueryPipelineStage::Bottom,
        .Index = 1});
    if (_readbackNeedsBarrier) {
        render::ResourceBarrierDescriptor toCopyDst = render::BarrierBufferDescriptor{
            .Target = frame.Readback.get(),
            .Before = render::BufferState::Common,
            .After = render::BufferState::CopyDestination};
        cmdBuffer->ResourceBarrier(std::span{&toCopyDst, 1});
    }
    cmdBuffer->ResolveQueryData(render::QueryResolveDescriptor{
        .Pool = frame.Pool.get(),
        .FirstIndex = 0,
        .Count = TimestampQueryCount,
        .Destination = frame.Readback.get(),
        .DestinationOffset = 0});
    if (_readbackNeedsBarrier) {
        render::ResourceBarrierDescriptor toHostRead = render::BarrierBufferDescriptor{
            .Target = frame.Readback.get(),
            .Before = render::BufferState::CopyDestination,
            .After = render::BufferState::HostRead};
        cmdBuffer->ResourceBarrier(std::span{&toHostRead, 1});
    }
    frame.Pending = true;
}

void GpuFrameProfiler::Resolve(uint32_t flightIndex) {
    if (flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    if (!frame.Pending) {
        return;
    }
    frame.Pending = false;

    const uint64_t mappedSize = sizeof(uint64_t) * TimestampQueryCount;
    ScopedBufferMap mapping{
        frame.Readback.get(),
        render::BufferRange{.Offset = 0, .Size = mappedSize}};
    if (!mapping) {
        return;
    }
    uint64_t ticks[TimestampQueryCount]{};
    std::memcpy(ticks, mapping.Data(), mappedSize);

    if (ticks[1] <= ticks[0]) {
        return;
    }
    const render::TimestampQueryCalibration calibration = frame.Pool->GetTimestampCalibration(_queue);
    if (calibration.TickPeriodNs <= 0.0) {
        return;
    }
    const double elapsedNs = static_cast<double>(ticks[1] - ticks[0]) * calibration.TickPeriodNs;
    _lastGpuTimeMs.store(static_cast<float>(elapsedNs / 1'000'000.0), std::memory_order_relaxed);
}

// ═════════════════════════════════════════════════════════════════
//  GpuSystem
// ═════════════════════════════════════════════════════════════════

void ServiceTraits<GpuSystem>::Inject(GpuSystem& self, WindowManager& windows) noexcept {
    self.SetWindowManager(&windows);
}

void ServiceTraits<GpuSystem>::Unwire(GpuSystem& self) noexcept {
    self.SetWindowManager(nullptr);
}

GpuSystem::GpuSystem(const GpuSystemDescriptor& desc)
    : _backBufferCount(desc.BackBufferCount),
      _flightDataCount(desc.FlightDataCount) {
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
                backendDesc.Factory = _dxgiFactory.get();
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
    _frameUploadScheduler = make_unique<FrameUploadScheduler>();
    if (desc.EnableFrameProfiler) {
        _frameProfiler = make_unique<GpuFrameProfiler>(_device.get(), _mainQueue, _flightDataCount);
    }
}

GpuSystem::~GpuSystem() noexcept {
    // 挂在未提交 flight 上的等待者永远等不到 fence, 必须显式取消 —— 取消会就地恢复它们,
    // 让它们在自己的作用域里析构所持有的 GPU 对象。
    //
    // 【为何不靠 ManualCoroutineScheduler 自己的析构】: 那要等到 _flights.clear() 逐个销毁
    // 槽位时才发生, 于是前一个槽位的 CmdBuffer 已经死了而后一个槽位的等待者才刚恢复。
    // 显式先取消全部, 把"恢复"与"拆 flight"分成两个不重叠的阶段。
    CancelAllWaitFrames();
    _frameUploadScheduler.reset();
    _flights.clear();
    _frameProfiler.reset();
    _mainQueueTrack.Fence.reset();
    _mainQueueTrack.Queue = nullptr;
    _mainQueue = nullptr;
    _device.reset();
    _dxgiFactory.reset();
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

void GpuSystem::NotifyFlightComplete(FlightCompletion completion) {
    _flightCompletions.Push(completion);
}

void GpuSystem::PumpFlightCompletions() {
    FlightCompletionQueue::Drain drain{_flightCompletions};
    if (drain.Items().empty()) {
        return;
    }
    if (_frameUploadScheduler) {
        _frameUploadScheduler->ApplyCompletedFlights(drain.Items());
    }
    for (auto* observer : _completionObservers) {
        observer->OnFlightsComplete(drain.Items());
    }
}

void GpuSystem::AddFlightCompletionObserver(IFlightCompletionObserver* observer) {
    _completionObservers.push_back(observer);
}

void GpuSystem::RemoveFlightCompletionObserver(IFlightCompletionObserver* observer) noexcept {
    std::erase(_completionObservers, observer);
}

void GpuSystem::PumpFrameUploadScheduler() {
    if (_frameUploadScheduler != nullptr) {
        _frameUploadScheduler->PumpCompletedUploads();
    }
}

void GpuSystem::WaitAndCleanupCompletedFlights() {
    if (_windowManager) _windowManager->EnsureRenderIdle();
    _mainQueue->Wait();

    for (uint32_t flightIndex = 0; flightIndex < _flights.size(); ++flightIndex) {
        CompleteFlight(flightIndex);
    }
    PumpFlightCompletions();
    // 队列已 idle,故所有【已提交】flight 的等待者都已就绪。此处恢复它们,让延迟销毁的
    // GPU 对象在正常路径上归还。挂在未提交 flight 上的记录等不到 fence,留给析构里的
    // CancelAllWaitFrames。
    //
    // 【调用点在主线程】: Application::Shutdown 早于渲染线程 join 之后的一切,见其顺序。
    for (uint32_t flightIndex = 0; flightIndex < _flights.size(); ++flightIndex) {
        PumpWaitFrame(flightIndex);
    }
    PumpFrameUploadScheduler();
}

void GpuSystem::PrepareFrameUploads(uint32_t flightIndex) {
    FlightSlot& record = *_flights.at(flightIndex);
    RADRAY_ASSERT(!record.UploadsPrepared && !record.Signal.IsValid());
    if (!record.UploadCommands) record.UploadCommands = _device->CreateCommandBuffer(_mainQueue).Unwrap();
    if (!record.Uploader) record.Uploader = make_unique<ResourceUploader>(_device.get(), _flightDataCount);
    record.UploadCommands->Begin();
    record.Uploader->BeginFlight(flightIndex, record.HostWrites);
    if (_frameUploadScheduler) _frameUploadScheduler->RunUploadPhase(record.UploadCommands.get(), *record.Uploader, flightIndex);
    record.UploadCommands->End();
    record.UploadsPrepared = true;
}

AppFrameContext GpuSystem::BeginFrameRecord(
    uint32_t flightIndex,
    std::chrono::duration<float> deltaTime,
    std::chrono::duration<float> lastFrameLatency,
    bool isInModalLoop,
    bool rendered) {
    FlightSlot& record = *_flights[flightIndex];
    // Direct/manual callers run both phases on one thread. ThreadedRunner prepares before handoff.
    if (!record.UploadsPrepared) PrepareFrameUploads(flightIndex);
    if (record.CmdBuffer == nullptr) {
        record.CmdBuffer = _device->CreateCommandBuffer(_mainQueue).Unwrap();
    }
    record.Targets.clear();
    record.Submitted = false;
    for (const auto& submission : record.Submissions) submission->Cancel();
    record.Submissions.clear();
    static std::atomic<uint64_t> nextFrameSerial{1};
    record.FrameSerial = nextFrameSerial.fetch_add(1, std::memory_order_relaxed);
    if (record.FrameSerial == 0 || record.FrameSerial == UINT64_MAX) RADRAY_ABORT("Frame serial exhausted");
    record.Recording = true;
    record.Rendered = rendered;
    record.CmdBuffer->Begin();
    if (_frameProfiler != nullptr) {
        _frameProfiler->BeginFrame(record.CmdBuffer.get(), flightIndex);
    }
    return AppFrameContext{this, flightIndex, deltaTime, lastFrameLatency, isInModalLoop};
}

void GpuSystem::EndFrameRecordAndSubmit(uint32_t flightIndex) {
    FlightSlot& record = *_flights[flightIndex];
    if (record.Submitted || !record.Recording) {
        return;
    }
    SubmitFrame(flightIndex, {});
}

void GpuSystem::SubmitFrame(
    uint32_t flightIndex,
    const AppFrameSubmitDescriptor& desc) {
    RADRAY_PROFILE_SCOPE_N("GpuSystem::SubmitFrame");
    FlightSlot& record = *_flights.at(flightIndex);
    if (!record.Recording || record.Submitted) {
        RADRAY_ABORT("GpuSystem::SubmitFrame called outside an active frame");
    }
    if (desc.SignalFences.size() != desc.SignalValues.size() ||
        desc.WaitFences.size() != desc.WaitValues.size()) {
        RADRAY_ABORT("AppFrameSubmitDescriptor fence/value counts do not match");
    }
    record.Recording = false;

    // 闭合上传链路：本帧录制的 staging + AssetRef 绑定到该 flight。
    record.Uploader->EndFlight(flightIndex);

    // 帧尾 GPU 耗时收尾:写 Bottom timestamp + resolve 到 readback(在 End 之前、后续 barrier 之后录制)。
    if (_frameProfiler != nullptr) {
        _frameProfiler->EndFrame(record.CmdBuffer.get(), flightIndex);
    }

    record.CmdBuffer->End();
    for (FlightSlot::AcquiredTarget& target : record.Targets) {
        if (target.Commands != nullptr) {
            target.Commands->End();
        }
    }

    vector<render::SwapChainSyncObject*> waitToExecute;
    vector<render::SwapChainSyncObject*> readyToPresent;
    waitToExecute.reserve(record.Targets.size());
    readyToPresent.reserve(record.Targets.size());
    for (FlightSlot::AcquiredTarget& target : record.Targets) {
        if (render::SwapChainSyncObject* syncObject = target.Frame.GetWaitToDraw()) {
            waitToExecute.emplace_back(syncObject);
        }
        if (render::SwapChainSyncObject* syncObject = target.Frame.GetReadyToPresent()) {
            readyToPresent.emplace_back(syncObject);
        }
    }

    bool dropPresentationWork = false;
    for (const FlightSlot::AcquiredTarget& target : record.Targets) {
        if (target.Window == nullptr || !target.Window->IsSwapChainPresentable()) {
            dropPresentationWork = true;
            break;
        }
    }
    if (dropPresentationWork) {
        RADRAY_WARN_LOG("skip swapchain GPU work: an acquired window became unpresentable before submit");
    }

    render::Fence* frameFence = _mainQueueTrack.Fence.get();
    const uint64_t frameFenceValue = _mainQueueTrack.NextFenceValue.fetch_add(1, std::memory_order_acq_rel);
    vector<render::Fence*> frameSignalFences;
    vector<uint64_t> frameSignalValues;
    frameSignalFences.reserve(desc.SignalFences.size() + 1);
    frameSignalValues.reserve(desc.SignalValues.size() + 1);
    frameSignalFences.push_back(frameFence);
    frameSignalValues.push_back(frameFenceValue);
    frameSignalFences.insert(frameSignalFences.end(), desc.SignalFences.begin(), desc.SignalFences.end());
    frameSignalValues.insert(frameSignalValues.end(), desc.SignalValues.begin(), desc.SignalValues.end());

    const auto submitQueue = [&](std::span<render::CommandBuffer*> cmdBuffers,
                                 std::span<render::Fence*> signalFences,
                                 std::span<uint64_t> signalValues,
                                 std::span<render::Fence*> waitFences,
                                 std::span<uint64_t> waitValues,
                                 std::span<render::SwapChainSyncObject*> waitSync,
                                 std::span<render::SwapChainSyncObject*> readySync) {
        _mainQueue->Submit(render::CommandQueueSubmitDescriptor{
            .CmdBuffers = cmdBuffers,
            .SignalFences = signalFences,
            .SignalValues = signalValues,
            .WaitFences = waitFences,
            .WaitValues = waitValues,
            .WaitToExecute = waitSync,
            .ReadyToPresent = readySync});
    };

    const bool splitHwndPresent =
        !dropPresentationWork &&
        record.Targets.size() > 1 &&
        _device->GetBackend() == render::RenderBackend::D3D12;

    vector<render::CommandBuffer*> sharedCmdBuffers;
    sharedCmdBuffers.reserve(desc.CmdBuffers.size() + 2 + record.Targets.size());
    sharedCmdBuffers.push_back(record.UploadCommands.get());
    if (!dropPresentationWork) {
        sharedCmdBuffers.push_back(record.CmdBuffer.get());
        sharedCmdBuffers.insert(sharedCmdBuffers.end(), desc.CmdBuffers.begin(), desc.CmdBuffers.end());
    }

    record.HostWrites.Flush(*_device);
    const std::span<render::Fence*> noFences{};
    const std::span<uint64_t> noFenceValues{};
    const std::span<render::SwapChainSyncObject*> noSync{};
    if (splitHwndPresent) {
        submitQueue(sharedCmdBuffers, noFences, noFenceValues, desc.WaitFences, desc.WaitValues, noSync, noSync);
        for (size_t i = 0; i < record.Targets.size(); ++i) {
            FlightSlot::AcquiredTarget& target = record.Targets[i];
            if (target.Commands == nullptr) {
                RADRAY_ABORT("D3D12 multi-HWND present command buffer is missing");
            }
            const bool last = i + 1 == record.Targets.size();
            render::CommandBuffer* presentCmds[]{target.Commands};
            const size_t waitCount = target.Frame.GetWaitToDraw() != nullptr ? 1 : 0;
            const size_t readyCount = target.Frame.GetReadyToPresent() != nullptr ? 1 : 0;
            render::SwapChainSyncObject* waitSync[1]{};
            render::SwapChainSyncObject* readySync[1]{};
            if (waitCount != 0) {
                waitSync[0] = target.Frame.GetWaitToDraw();
            }
            if (readyCount != 0) {
                readySync[0] = target.Frame.GetReadyToPresent();
            }
            submitQueue(
                std::span{presentCmds, 1},
                last ? std::span{frameSignalFences} : std::span<render::Fence*>{},
                last ? std::span{frameSignalValues} : std::span<uint64_t>{},
                {},
                {},
                std::span{waitSync, waitCount},
                std::span{readySync, readyCount});
            render::SwapChainPresentResult present =
                target.Window->PresentSwapChainFrame(std::move(target.Frame));
            if (present.Status == render::SwapChainStatus::RequireRecreate) {
                continue;
            }
            if (present.Status != render::SwapChainStatus::Success) {
                RADRAY_ERR_LOG("failed to present swapchain frame: status={}, native={}", present.Status, present.NativeStatusCode);
            }
        }
    } else {
        if (!dropPresentationWork) {
            for (FlightSlot::AcquiredTarget& target : record.Targets) {
                if (target.Commands != nullptr) {
                    sharedCmdBuffers.push_back(target.Commands);
                }
            }
        }
        submitQueue(
            sharedCmdBuffers,
            frameSignalFences,
            frameSignalValues,
            desc.WaitFences,
            desc.WaitValues,
            waitToExecute,
            readyToPresent);
        for (FlightSlot::AcquiredTarget& target : record.Targets) {
            render::SwapChainPresentResult present =
                target.Window->PresentSwapChainFrame(std::move(target.Frame));
            if (present.Status == render::SwapChainStatus::RequireRecreate) {
                continue;
            }
            if (present.Status != render::SwapChainStatus::Success) {
                RADRAY_ERR_LOG("failed to present swapchain frame: status={}, native={}", present.Status, present.NativeStatusCode);
            }
        }
    }
    record.HostWrites.Seal();
    _flights[flightIndex]->Signal = GpuSystem::FenceSignal{
        .Fence = frameFence,
        .Value = frameFenceValue};
    for (const auto& submission : record.Submissions) submission->Submit(record.FrameSerial);
    record.Targets.clear();
    record.Submitted = true;
    record.UploadsPrepared = false;
}

// ══════════════════════════════════════════════
//  AppFrameContext
// ══════════════════════════════════════════════

uint64_t AppFrameContext::FrameSerial() const noexcept { return _gpuSystem->_flights[_flightIndex]->FrameSerial; }
void AppFrameContext::TrackSubmission(shared_ptr<FrameSubmission> submission) {
    if (!submission || submission->Serial() != FrameSerial()) RADRAY_ABORT("Submission receipt belongs to another frame serial");
    _gpuSystem->_flights[_flightIndex]->Submissions.push_back(std::move(submission));
}

render::CommandBuffer* AppFrameContext::GetCommandBuffer() const noexcept {
    return _gpuSystem->_flights[_flightIndex]->CmdBuffer.get();
}

render::CommandBuffer* AppFrameContext::GetCommandBufferForTexture(render::Texture* texture) const noexcept {
    render::CommandBuffer* shared = GetCommandBuffer();
    if (texture == nullptr) {
        return shared;
    }
    GpuSystem::FlightSlot& record = *_gpuSystem->_flights[_flightIndex];
    for (GpuSystem::FlightSlot::AcquiredTarget& target : record.Targets) {
        if (target.Commands != nullptr && target.Frame.GetBackBuffer() == texture) {
            return target.Commands;
        }
    }
    return shared;
}

std::optional<AppFrameTarget> AppFrameContext::AcquireWindow(AppWindow* window) {
    RADRAY_PROFILE_SCOPE_N("AcquireWindow");
    if (window == nullptr) {
        return std::nullopt;
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

    render::SwapChainFrame frame = std::move(acquire.Frame.value());
    render::Texture* backBuffer = frame.GetBackBuffer();
    const uint32_t backBufferIndex = frame.GetBackBufferIndex();
    render::TextureView* backBufferView = window->GetOrCreateBackBufferView(frame);
    if (backBufferView == nullptr) {
        // 未能建立 view：丢弃该 frame（不提交）。
        return std::nullopt;
    }

    GpuSystem::FlightSlot& record = *_gpuSystem->_flights[_flightIndex];
    const uint32_t presentIndex = static_cast<uint32_t>(record.Targets.size());
    if (record.PresentCommandPool.size() <= presentIndex) {
        record.PresentCommandPool.push_back(_gpuSystem->_device->CreateCommandBuffer(_gpuSystem->_mainQueue).Unwrap());
    }
    render::CommandBuffer* presentCommands = record.PresentCommandPool[presentIndex].get();
    presentCommands->Begin();
    record.Targets.emplace_back(GpuFlightAcquiredTarget{
        .Window = window,
        .Frame = std::move(frame),
        .Commands = presentCommands});
    return AppFrameTarget{
        .Window = window,
        .BackBuffer = backBuffer,
        .BackBufferView = backBufferView,
        .BackBufferIndex = backBufferIndex};
}

ResourceUploader& AppFrameContext::GetUploader() const noexcept {
    return *_gpuSystem->_flights[_flightIndex]->Uploader;
}

HostWriteBatch& AppFrameContext::GetHostWrites() const noexcept {
    return _gpuSystem->_flights[_flightIndex]->HostWrites;
}

render::Device* AppFrameContext::GetDevice() const noexcept {
    return _gpuSystem->_device.get();
}

void AppFrameContext::SubmitFrame(const AppFrameSubmitDescriptor& desc) {
    _gpuSystem->SubmitFrame(_flightIndex, desc);
}

}  // namespace radray
