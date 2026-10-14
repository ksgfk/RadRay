#include <radray/runtime/frame_timeline.h>

#include <radray/logger.h>
#include <radray/scope_guard.h>

namespace radray {

bool WaitFrameAwaitable::await_ready() const noexcept {
    return _timeline == nullptr || _timeline->IsStopping() || _stop.stop_requested();
}

bool WaitFrameAwaitable::await_suspend(std::coroutine_handle<> continuation) {
    if (_timeline == nullptr || _timeline->IsStopping() || _stop.stop_requested()) {
        return false;
    }
    _record = _timeline->RegisterWaitFrame(_stop, continuation);
    return _record != nullptr;
}

bool WaitFrameAwaitable::await_resume() noexcept {
    const bool completed = !_stop.stop_requested() && (_record == nullptr || !_record->Canceled) &&
                           (_timeline == nullptr || !_timeline->IsStopping());
    if (_record != nullptr && _timeline != nullptr) {
        _timeline->EraseWaitFrame(_record);
    }
    _record = nullptr;
    return completed;
}

FrameTimeline::FrameTimeline(uint32_t flightCount)
    : _flightDataCount(flightCount) {
    if (_flightDataCount == 0) RADRAY_ABORT("FrameTimeline requires at least one flight");
    _completions.resize(_flightDataCount);
    _flights.reserve(_flightDataCount);
    for (uint32_t i = 0; i < _flightDataCount; ++i) {
        _flights.push_back(make_unique<Flight>());
    }
}

FrameTimeline::~FrameTimeline() noexcept {
    CancelAllWaitFrames();
}

task<void> FrameTimeline::Wait() {
    stop_token stop = co_await CurrentStopToken();
    if (!co_await WaitFrameAwaitable{this, stop}) co_await StopCurrentTask();
}

WaitFrameRecord* FrameTimeline::RegisterWaitFrame(stop_token stop, std::coroutine_handle<> continuation) {
    const uint32_t flightIndex = GetCurrentFlightIndex();
    if (flightIndex >= _flights.size()) {
        return nullptr;
    }
    WaitFrameRecord* record = _flights[flightIndex]->Waiters.Enqueue(stop, continuation);
    record->FlightIndex = flightIndex;
    record->TargetSerial = OpenFrameDomain(flightIndex);
    return record;
}

void FrameTimeline::EraseWaitFrame(WaitFrameRecord* record) noexcept {
    if (record == nullptr || record->FlightIndex >= _flights.size()) {
        return;
    }
    _flights[record->FlightIndex]->Waiters.Erase(record);
}

void FrameTimeline::PumpWaitFrame(uint32_t flightIndex) {
    if (flightIndex >= _flights.size()) {
        return;
    }
    if (_pumpingWaiters) RADRAY_ABORT("Cannot reenter frame waiter dispatch");
    _pumpingWaiters = true;
    auto guard = MakeScopeGuard([this]() noexcept { _pumpingWaiters = false; });

    ManualCoroutineScheduler<WaitFrameRecord>& waiters = _flights[flightIndex]->Waiters;
    const auto boundary = _waitDispatchBoundaries.empty() ? waiters.GetSequenceBoundary() : _waitDispatchBoundaries[flightIndex];
    const auto completed = _flights[flightIndex]->CompletedSerial.load(std::memory_order_acquire);
    waiters.DispatchReady([completed](const auto& record) { return record.Canceled || record.TargetSerial <= completed; }, boundary);
}

void FrameTimeline::CancelAllWaitFrames() noexcept {
    for (unique_ptr<Flight>& flight : _flights) {
        flight->Waiters.CancelAll();
    }
}

void FrameTimeline::CleanupCompletedFlights() {
    if (!_waitDispatchBoundaries.empty() || _pumpingWaiters) RADRAY_ABORT("Cannot reenter completion notification batch");
    for (const auto& flight : _flights) _waitDispatchBoundaries.push_back(flight->Waiters.GetSequenceBoundary());
    auto boundaryGuard = MakeScopeGuard([this]() noexcept { _waitDispatchBoundaries.clear(); });
    for (uint32_t flightIndex = 0; flightIndex < _flights.size(); ++flightIndex) {
        PumpWaitFrame(flightIndex);
    }
}

void FrameTimeline::PublishCompletion(const FlightCompletion& completion) {
    std::lock_guard lock{_completionMutex};
    if (completion.FlightIndex >= _flights.size()) RADRAY_ABORT("Invalid completion flight");
    auto& flight = *_flights[completion.FlightIndex];
    if (!completion.FrameSerial || completion.FrameSerial != flight.OpenSerial || flight.CompletionPending ||
        completion.FrameSerial <= flight.CompletedSerial.load(std::memory_order_relaxed)) RADRAY_ABORT("Duplicate or foreign frame completion");
    if (_completionCount == _completions.size()) RADRAY_ABORT("Frame completion capacity exceeded");
    _completions[(_completionHead + _completionCount) % _completions.size()] = completion;
    ++_completionCount;
    flight.CompletionPending = true;
    const std::chrono::duration<float> latency = std::chrono::steady_clock::now() - flight.FrameStartTime;
    _lastFrameLatencySeconds.store(latency.count(), std::memory_order_relaxed);
    flight.CompletedSerial.store(completion.FrameSerial, std::memory_order_release);
}

bool FrameTimeline::TryReadCompletion(FlightCompletion& out) {
    std::lock_guard lock{_completionMutex};
    if (!_completionCount) return false;
    out = _completions[_completionHead];
    _completionHead = (_completionHead + 1) % _completions.size();
    --_completionCount;
    _flights[out.FlightIndex]->CompletionPending = false;
    return true;
}

uint64_t FrameTimeline::OpenFrameDomain(uint32_t flightIndex) {
    std::lock_guard lock{_completionMutex};
    if (flightIndex >= _flights.size() || _stopping) RADRAY_ABORT("Invalid frame domain");
    auto& flight = *_flights[flightIndex];
    if (!flight.OpenSerial || flight.OpenSerial <= flight.CompletedSerial.load(std::memory_order_relaxed)) {
        const auto serial = _nextFrameSerial++;
        if (serial == 0 || serial == UINT64_MAX) RADRAY_ABORT("Frame serial exhausted");
        flight.OpenSerial = serial;
    }
    return flight.OpenSerial;
}
bool FrameTimeline::IsCompleted(uint32_t flightIndex, uint64_t frameSerial) const noexcept {
    return flightIndex < _flights.size() && frameSerial != 0 &&
           _flights[flightIndex]->CompletedSerial.load(std::memory_order_acquire) >= frameSerial;
}

uint64_t FrameTimeline::GetFrameSerial(uint32_t flightIndex) const noexcept {
    std::lock_guard lock{_completionMutex};
    return flightIndex < _flights.size() ? _flights[flightIndex]->OpenSerial : 0;
}

std::chrono::steady_clock::time_point FrameTimeline::BeginFrameTiming(uint32_t flightIndex) noexcept {
    OpenFrameDomain(flightIndex);
    std::lock_guard lock{_completionMutex};
    Flight& flight = *_flights[flightIndex];
    flight.FrameStartTime = std::chrono::steady_clock::now();
    return flight.FrameStartTime;
}

uint32_t FrameTimeline::GetCurrentFlightIndex() const noexcept {
    return static_cast<uint32_t>(_nowFrameIndex % _flightDataCount);
}

}  // namespace radray
