#include <radray/runtime/render_scene/scene_delivery_state.h>

#include <limits>

namespace radray {

bool SceneDeliveryState::IsRunningGT() const noexcept {
    return _mode == Mode::Running;
}

void SceneDeliveryState::BeginStoppingGT() noexcept {
    if (_mode == Mode::Running) _mode = Mode::Stopping;
}

SceneDeliveryError SceneDeliveryState::ValidateSeal(const SceneFlightState& flight) const noexcept {
    if (!IsRunningGT()) return SceneDeliveryError::Stopping;
    if (flight.Phase != SceneFlightPhase::Writable) return SceneDeliveryError::Occupied;
    if (_nextUpdateSequence == std::numeric_limits<uint64_t>::max()) return SceneDeliveryError::SequenceExhausted;
    return SceneDeliveryError::None;
}

SceneDeliveryError SceneDeliveryState::TrySeal(SceneFlightState& flight) noexcept {
    const auto error = ValidateSeal(flight);
    if (error != SceneDeliveryError::None) return error;
    flight.UpdateSequence = _nextUpdateSequence++;
    flight.FrameSerial = 0;
    flight.Phase = SceneFlightPhase::Sealed;
    return SceneDeliveryError::None;
}

SceneDeliveryError SceneDeliveryState::TryPublish(SceneFlightState& flight) noexcept {
    if (!IsRunningGT() || flight.Phase != SceneFlightPhase::Sealed || flight.UpdateSequence != _lastPublishedSequence + 1) {
        return SceneDeliveryError::PublicationOrder;
    }
    _lastPublishedSequence = flight.UpdateSequence;
    flight.Phase = SceneFlightPhase::Published;
    return SceneDeliveryError::None;
}

SceneDeliveryError SceneDeliveryState::ValidateConsume(const SceneFlightState& flight, uint64_t frameSerial) const noexcept {
    if (flight.Phase != SceneFlightPhase::Published || flight.UpdateSequence != _lastConsumedSequence + 1 || frameSerial == 0 || frameSerial <= _lastFrameSerial) {
        return SceneDeliveryError::ConsumptionOrder;
    }
    return SceneDeliveryError::None;
}

SceneDeliveryError SceneDeliveryState::TryConsume(SceneFlightState& flight, uint64_t frameSerial) noexcept {
    const auto error = ValidateConsume(flight, frameSerial);
    if (error != SceneDeliveryError::None) return error;
    flight.FrameSerial = frameSerial;
    flight.Phase = SceneFlightPhase::Consumed;
    _lastConsumedSequence = flight.UpdateSequence;
    _lastFrameSerial = frameSerial;
    return SceneDeliveryError::None;
}

SceneDeliveryError SceneDeliveryState::ValidateComplete(const SceneFlightState& flight, uint64_t frameSerial) const noexcept {
    if (flight.Phase != SceneFlightPhase::Consumed || flight.FrameSerial != frameSerial) {
        return SceneDeliveryError::CompletionMismatch;
    }
    return SceneDeliveryError::None;
}

SceneDeliveryError SceneDeliveryState::TryComplete(SceneFlightState& flight, uint64_t frameSerial) noexcept {
    const auto error = ValidateComplete(flight, frameSerial);
    if (error != SceneDeliveryError::None) return error;
    flight.Phase = SceneFlightPhase::Writable;
    return SceneDeliveryError::None;
}

SceneDeliveryError SceneDeliveryState::ValidateAbandon(const SceneFlightState& flight) const noexcept {
    if (IsRunningGT() || flight.Phase == SceneFlightPhase::Published || flight.Phase == SceneFlightPhase::Consumed) {
        return SceneDeliveryError::InvalidAbandon;
    }
    return SceneDeliveryError::None;
}

SceneDeliveryError SceneDeliveryState::TryAbandon(SceneFlightState& flight) noexcept {
    const auto error = ValidateAbandon(flight);
    if (error != SceneDeliveryError::None) return error;
    _mode = Mode::Abandoned;
    flight.Phase = SceneFlightPhase::Writable;
    return SceneDeliveryError::None;
}

}  // namespace radray
