#pragma once

#include <cstdint>

namespace radray {

enum class SceneFlightPhase : uint8_t {
    Writable,
    Sealed,
    Published,
    Consumed
};

/// Metadata travels with its payload through the existing flight handoffs.
struct SceneFlightState {
    SceneFlightPhase Phase{SceneFlightPhase::Writable};
    uint64_t UpdateSequence{0};
    uint64_t FrameSerial{0};
};

enum class SceneDeliveryError : uint8_t {
    None,
    Stopping,
    Occupied,
    SequenceExhausted,
    PublicationOrder,
    ConsumptionOrder,
    CompletionMismatch,
    InvalidAbandon
};

/// Internal protocol only: no Scene, asset owners, callbacks or synchronization.
/// GT owns seal/publish/complete/stop/abandon; RT owns consume. The runner must
/// establish exclusive access to each flight through its existing handoffs.
/// Contract: docs/architecture/render-framework.md
class SceneDeliveryState {
public:
    SceneDeliveryState() noexcept = default;
    SceneDeliveryState(const SceneDeliveryState&) = delete;
    SceneDeliveryState(SceneDeliveryState&&) = delete;
    SceneDeliveryState& operator=(const SceneDeliveryState&) = delete;
    SceneDeliveryState& operator=(SceneDeliveryState&&) = delete;

    bool IsRunningGT() const noexcept;
    void BeginStoppingGT() noexcept;

    /// Validate before changing payload/owners, then Try* after that work finishes.
    /// Rejected transitions leave both the flight and sequence cursors unchanged.
    [[nodiscard]] SceneDeliveryError ValidateSeal(const SceneFlightState& flight) const noexcept;
    [[nodiscard]] SceneDeliveryError TrySeal(SceneFlightState& flight) noexcept;
    [[nodiscard]] SceneDeliveryError TryPublish(SceneFlightState& flight) noexcept;
    [[nodiscard]] SceneDeliveryError ValidateConsume(const SceneFlightState& flight, uint64_t frameSerial) const noexcept;
    [[nodiscard]] SceneDeliveryError TryConsume(SceneFlightState& flight, uint64_t frameSerial) noexcept;
    [[nodiscard]] SceneDeliveryError ValidateComplete(const SceneFlightState& flight, uint64_t frameSerial) const noexcept;
    [[nodiscard]] SceneDeliveryError TryComplete(SceneFlightState& flight, uint64_t frameSerial) noexcept;
    [[nodiscard]] SceneDeliveryError ValidateAbandon(const SceneFlightState& flight) const noexcept;
    [[nodiscard]] SceneDeliveryError TryAbandon(SceneFlightState& flight) noexcept;

private:
    enum class Mode : uint8_t {
        Running,
        Stopping,
        Abandoned
    };

    // GT-only cursors. RT must not read these, including while GT is stopping.
    uint64_t _nextUpdateSequence{1};
    uint64_t _lastPublishedSequence{0};
    Mode _mode{Mode::Running};

    // RT-only cursors. Completion validates its own flight, not these cursors.
    uint64_t _lastConsumedSequence{0};
    uint64_t _lastFrameSerial{0};
};

}  // namespace radray
