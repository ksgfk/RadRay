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

#include <algorithm>
#include <span>
#include <type_traits>

namespace radray {

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

void GpuSystem::WaitAndRetireFlights() {
    if (_windowManager) _windowManager->EnsureRenderIdle();
    _mainQueue->Wait();

    for (uint32_t flightIndex = 0; flightIndex < _flights.size(); ++flightIndex) {
        CompleteFlight(flightIndex);
    }
}

void GpuSystem::CleanupCompletedFlights() {
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
    FlightSlot& record = *_flights[flightIndex];
    if (!record.Uploader) record.Uploader = make_unique<ResourceUploader>(_device.get(), _flightDataCount);
    record.Uploader->BeginFlight(flightIndex, record.HostWrites);
    if (record.CmdBuffer == nullptr) {
        record.CmdBuffer = _device->CreateCommandBuffer(_mainQueue).Unwrap();
    }
    record.Targets.clear();
    record.Submitted = false;
    record.FrameSerial = _nextFrameSerial++;
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

    // 应用显式录制的上传 staging 保留到该 flight 的 fence 完成。
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
            target.Window->SetBackBufferState(target.Frame.GetBackBufferIndex(), render::TextureState::Present);
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
            if (!dropPresentationWork) {
                target.Window->SetBackBufferState(target.Frame.GetBackBufferIndex(), render::TextureState::Present);
            }
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
    record.Targets.clear();
    record.Submitted = true;
}

// ══════════════════════════════════════════════
//  AppFrameContext
// ══════════════════════════════════════════════

uint64_t AppFrameContext::FrameSerial() const noexcept { return _gpuSystem->_flights[_flightIndex]->FrameSerial; }

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
