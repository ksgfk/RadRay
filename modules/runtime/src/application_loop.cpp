#include "application_loop.h"

#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/render_framework/scene_manager.h>
#include <radray/profiler.h>
#include <radray/scope_guard.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray {

ApplicationLoop::ApplicationLoop(Application* app)
    : _app(app), _windows(app->GetWindowManager()), _gpu(app->GetGpuSystem()),
      _frames(app->GetFrameTimeline().GetFlightDataCount()) {
    _submitted.reserve(_frames.size());
    if (_windows) _modalConnection = _windows->EventModalLoopTick().connect(&ApplicationLoop::TickModal, this);
}

ApplicationLoop::~ApplicationLoop() noexcept {
    RADRAY_ASSERT(_state == State::Stopped && !_driving && _nativeDepth == 0);
    RADRAY_ASSERT(!_renderThread.joinable() && !_drainThread.joinable());
    _modalConnection.disconnect();
}

bool ApplicationLoop::CanFinalize() const noexcept { return !_advancing && !_driving && !_modalTick && _nativeDepth == 0; }

void ApplicationLoop::Wake() noexcept {
    if (_windows) {
        _progress.fetch_add(1, std::memory_order_release);
        _windows->_eventPump->Wake();
    } else {
        {
            std::lock_guard lock(_wakeMutex);
            _progress.fetch_add(1, std::memory_order_release);
        }
        _wakeCondition.notify_all();
    }
}

void ApplicationLoop::Run() {
    if (!CanFinalize()) RADRAY_ABORT("Cannot reenter the application loop");
    _lastFrameTime = std::chrono::steady_clock::now();
    if (_gpu && _app->_multithreaded && !_app->ShouldExit()) _renderThread = std::thread(&ApplicationLoop::RenderThread, this);
    Drive();
}

void ApplicationLoop::Drain() {
    if (_state == State::Stopped) return;
    if (!CanFinalize()) RADRAY_ABORT("Runtime destruction inside frame or native dispatch");
    _app->RequestExit();
    Drive();
}

void ApplicationLoop::Drive() {
    _driving = true;
    auto driving = MakeScopeGuard([this]() noexcept { _driving = false; });
    while (_state != State::Stopped) {
        const auto progress = _progress.load(std::memory_order_acquire);
        const auto prepared = PrepareFrame(false);
        if (_state == State::Stopped) break;
        // S0 and timing precede this collection, including messages posted by completion callbacks.
        PumpNativeEvents();
        const bool advanced = prepared && ExecuteFrame(*prepared, false);
        if (!advanced && _state != State::Stopped) Idle(progress);
    }
}

void ApplicationLoop::PumpNativeEvents() {
    if (!_windows) return;
    ++_nativeDepth;
    auto dispatch = MakeScopeGuard([this]() noexcept { --_nativeDepth; });
    _windows->_eventPump->PumpNativeEvents(256);
}

void ApplicationLoop::TickModal(NativeWindow*) {
    if (!_driving || _advancing || _modalTick || _state == State::Stopped) return;
    _modalTick = true;
    auto modal = MakeScopeGuard([this]() noexcept { _modalTick = false; });
    _discardBefore.store(_published.load(std::memory_order_relaxed), std::memory_order_release);
    const auto prepared = PrepareFrame(true);
    if (!prepared) return;
    PumpNativeEvents();
    ExecuteFrame(*prepared, true);
}

std::optional<uint64_t> ApplicationLoop::PrepareFrame(bool modal) {
    if (_advancing) return {};
    _advancing = true;
    auto advance = MakeScopeGuard([this]() noexcept { _advancing = false; });
    if (_app->ShouldExit() && !_stop.load(std::memory_order_relaxed)) BeginStop();
    if (_stop.load(std::memory_order_relaxed)) {
        if (modal && _windows) _windows->_eventPump->StopEventDelivery();
        AdvanceStop();
        return {};
    }
    if (modal && _cpuCompleted.load(std::memory_order_acquire) < _lastModalPublished) return {};
    if (_prepared) return _prepared;
    RADRAY_PROFILE_SCOPE_N("PrepareFrame");
    if (_state == State::Running && _windows && _windows->NeedsMaintenance()) {
        _operationBoundary = _windows->GetOperationBoundary();
        _maintenanceTarget = _published.load(std::memory_order_relaxed);
        _drainStarted = false;
        _drainFinished = false;
        _state = State::MaintenanceCPU;
    }
    if (_state != State::Running && !AdvanceMaintenance()) return {};
    if (_app->ShouldExit()) return {};
    RetireReadyFrames();
    auto& timeline = _app->GetFrameTimeline();
    const uint64_t identity = timeline.GetFrameIndex();
    if (identity - _retireIndex >= _frames.size()) {
        if (_gpu && _retireIndex < _cpuCompleted.load(std::memory_order_acquire)) GetWaitingSignal();
        return {};
    }
    const auto flight = timeline.GetCurrentFlightIndex();
    _app->ServiceFrameBoundaryGT(flight);
    const auto now = _gpu ? _gpu->BeginFrameTiming(flight) : timeline.BeginFrameTiming(flight);
    _frames[flight].DeltaTime = now - _lastFrameTime;
    _frames[flight].Serial = timeline.GetFrameSerial(flight);
    _lastFrameTime = now;
    _prepared = identity;
    return identity;
}

bool ApplicationLoop::ExecuteFrame(uint64_t identity, bool modal) {
    if (_advancing || !_prepared || *_prepared != identity || _app->ShouldExit()) return false;
    _advancing = true;
    auto advance = MakeScopeGuard([this]() noexcept { _advancing = false; });
    _prepared.reset();
    RADRAY_PROFILE_SCOPE_N("TickFrame");
    if (_windows) _windows->_eventPump->DispatchPendingEvents([this] { return !_app->ShouldExit(); });
    if (_app->ShouldExit()) return false;
    auto& timeline = _app->GetFrameTimeline();
    const uint32_t flight = static_cast<uint32_t>(identity % _frames.size());
    auto& data = _frames[flight];
    data.Modal = modal;
    {
        RADRAY_PROFILE_SCOPE_N("Update");
        if (_app->Update({flight, data.DeltaTime, timeline.GetLastFrameLatency()}).ShouldExit) return false;
    }
    if (auto scenes = _app->GetSceneManager()) scenes->PublishFrameGT(flight);
    if (_gpu) {
        if (!_app->_multithreaded) RecordFrame(identity, false);
    } else {
        if (auto scenes = _app->GetSceneManager()) scenes->ConsumeRenderUpdates(flight, data.Serial);
        timeline.PublishCompletion({flight, false, data.Serial});
        _retireIndex = identity + 1;
    }
    timeline.AdvanceFrameIndex();
    _published.store(identity + 1, std::memory_order_release);
    if (_gpu && _app->_multithreaded)
        _ready.release();
    else
        _cpuCompleted.store(identity + 1, std::memory_order_release);
    if (modal) _lastModalPublished = identity + 1;
    if (!_gpu || !_app->_multithreaded) RADRAY_PROFILE_FRAME();
    return true;
}

void ApplicationLoop::RecordFrame(uint64_t index, bool discard) {
    const auto flight = static_cast<uint32_t>(index % _frames.size());
    const auto& data = _frames[flight];
    auto context = _gpu->BeginFrameRecord(flight, data.DeltaTime, _app->GetFrameTimeline().GetLastFrameLatency(), data.Modal, !discard);
    _app->ApplySceneUpdatesRT(context);
    if (!discard) {
        RADRAY_PROFILE_SCOPE_N("Render");
        _app->Render(context);
    }
    {
        RADRAY_PROFILE_SCOPE_N("Submit");
        _gpu->EndFrameRecordAndSubmit(flight);
    }
}

void ApplicationLoop::RenderThread() {
    RADRAY_PROFILE_THREAD("RadRay Render");
    uint64_t index = 0;
    while (true) {
        {
            RADRAY_PROFILE_SCOPE_N("WaitReadySlot");
            _ready.acquire();
        }
        const bool stopping = _stop.load(std::memory_order_acquire);
        if (stopping && index == _published.load(std::memory_order_acquire)) break;
        RADRAY_PROFILE_SCOPE_N("RenderFrame");
        const auto& data = _frames[index % _frames.size()];
        const bool discard = stopping || (!data.Modal && index < _discardBefore.load(std::memory_order_acquire));
        RecordFrame(index, discard);
        // Publish only after Submit, Present, the final signal and all recording cleanup.
        _cpuCompleted.store(++index, std::memory_order_release);
        Wake();
        RADRAY_PROFILE_FRAME();
    }
    _renderReturned.store(true, std::memory_order_release);
    Wake();
}

void ApplicationLoop::RetireReadyFrames() {
    if (!_gpu) return;
    const auto completed = _cpuCompleted.load(std::memory_order_acquire);
    _submitted.clear();
    for (uint64_t i = _retireIndex; i < completed; ++i) {
        const auto flight = static_cast<uint32_t>(i % _frames.size());
        _submitted.push_back({flight, _frames[flight].Serial});
    }
    _retireIndex += _gpu->RetireSubmittedFlights(_submitted);
}

bool ApplicationLoop::AdvanceMaintenance() {
    if (_state == State::MaintenanceCPU) {
        if (_cpuCompleted.load(std::memory_order_acquire) < _maintenanceTarget) return false;
        if (_gpu) StartDrain();
        _state = State::MaintenanceGPU;
    }
    if (_gpu && !CollectDrain()) return false;
    RetireReadyFrames();
    RADRAY_ASSERT(_retireIndex == _maintenanceTarget);
    _windows->ProcessOperations(_operationBoundary);
    _state = State::Running;
    _drainStarted = false;
    _drainFinished = false;
    return true;
}

void ApplicationLoop::BeginStop() {
    _stop.store(true, std::memory_order_release);
    _prepared.reset();
    if (_windows) {
        _windows->_eventPump->StopEventDelivery();
        _windows->CloseOperations();
    }
    if (_renderThread.joinable()) _ready.release();
    _state = State::StoppingCPU;
}

bool ApplicationLoop::CollectThread(std::thread& thread, const std::atomic_bool& returned) {
    if (!thread.joinable()) return true;
#if defined(RADRAY_PLATFORM_WINDOWS)
    (void)returned;
    if (::WaitForSingleObject(thread.native_handle(), 0) != WAIT_OBJECT_0) return false;
#else
    if (!returned.load(std::memory_order_acquire)) return false;
#endif
    thread.join();
    return true;
}

void ApplicationLoop::StartDrain() {
    RADRAY_ASSERT(!_drainThread.joinable());
    RADRAY_ASSERT(_cpuCompleted.load(std::memory_order_acquire) == _published.load(std::memory_order_relaxed));
    vector<GpuFenceSignal> signals;
    for (uint64_t i = _retireIndex; i < _published.load(std::memory_order_relaxed); ++i)
        signals.push_back(_gpu->GetFlightGpuSignal(static_cast<uint32_t>(i % _frames.size())));
    _drainStarted = true;
    _drainFinished = false;
    _drainReturned.store(false, std::memory_order_relaxed);
    _drainThread = std::thread([this, signals = std::move(signals)] {
        RADRAY_PROFILE_THREAD("RadRay GPU drain");
        for (const auto& signal : signals)
            if (signal.IsValid()) signal.Fence->Wait(signal.Value);
        _gpu->WaitMainQueueIdle();
        _drainReturned.store(true, std::memory_order_release);
        Wake();
    });
}

bool ApplicationLoop::CollectDrain() {
    if (_drainFinished) return true;
    if (!_drainStarted || !CollectThread(_drainThread, _drainReturned)) return false;
    _drainFinished = true;
    return true;
}

void ApplicationLoop::AdvanceStop() {
    if (_state == State::StoppingCPU) {
        if (!CollectThread(_renderThread, _renderReturned)) return;
        RADRAY_ASSERT(_cpuCompleted.load(std::memory_order_acquire) == _published.load(std::memory_order_relaxed));
        if (!_systemsStopping) {
            _app->BeginStoppingRuntime();
            _systemsStopping = true;
        }
        if (_gpu && !_drainStarted) StartDrain();
        _state = State::StoppingGPU;
    }
    if (_gpu && !CollectDrain()) return;
    RetireReadyFrames();
    RADRAY_ASSERT(_retireIndex == _published.load(std::memory_order_relaxed));
    _app->PumpFlightCompletions(std::nullopt);
    if (auto scenes = _app->GetSceneManager()) scenes->AbandonUnpublishedFramesGT();
    _state = State::Stopped;
}

GpuFenceSignal ApplicationLoop::GetWaitingSignal() {
    const auto flight = static_cast<uint32_t>(_retireIndex % _frames.size());
    const auto signal = _gpu->GetFlightGpuSignal(flight);
    if (_waitingSerial != _frames[flight].Serial) {
        _waitingSerial = _frames[flight].Serial;
        _gpuDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    }
    if (std::chrono::steady_clock::now() >= _gpuDeadline && signal.Fence->GetCompletedValue() < signal.Value)
        RADRAY_ABORT("Window flight fence value {} did not complete within 30 seconds", signal.Value);
    return signal;
}

void ApplicationLoop::Idle(uint64_t observedProgress) {
    if (_progress.load(std::memory_order_acquire) != observedProgress ||
        (_app->ShouldExit() && !_stop.load(std::memory_order_relaxed))) return;
    if (_state == State::Running) {
        // A modal tick may have consumed the prepared identity during native dispatch.
        if (_prepared || _app->GetFrameTimeline().GetFrameIndex() - _retireIndex < _frames.size()) return;
        const auto completed = _cpuCompleted.load(std::memory_order_acquire);
        if (_gpu && _retireIndex < completed) {
            const auto signal = GetWaitingSignal();
            RADRAY_PROFILE_SCOPE_N("GpuSystem::WaitFlightFence");
            _advancing = true;
            auto wait = MakeScopeGuard([this]() noexcept { _advancing = false; });
            if (!signal.Fence->WaitFor(signal.Value, 1) && std::chrono::steady_clock::now() >= _gpuDeadline)
                RADRAY_ABORT("Window flight fence value {} did not complete within 30 seconds", signal.Value);
            return;
        }
    }
    RADRAY_PROFILE_SCOPE_N("ApplicationLoop::WaitProgress");
    if (_windows) {
        array<void*, 2> handles{};
        size_t count = 0;
#if defined(RADRAY_PLATFORM_WINDOWS)
        if (_renderThread.joinable() && _stop.load(std::memory_order_relaxed)) handles[count++] = _renderThread.native_handle();
        if (_drainThread.joinable()) handles[count++] = _drainThread.native_handle();
#endif
        ++_nativeDepth;
        auto wait = MakeScopeGuard([this]() noexcept { --_nativeDepth; });
        _windows->_eventPump->WaitEvents(UINT32_MAX, std::span<void* const>{handles.data(), count});
    } else {
        std::unique_lock lock(_wakeMutex);
        // Headless portable hosts need no message service; the bounded fallback observes thread epilogues.
        _wakeCondition.wait_for(lock, std::chrono::milliseconds{1}, [this, observedProgress] {
            return _progress.load(std::memory_order_acquire) != observedProgress;
        });
    }
}

}  // namespace radray
