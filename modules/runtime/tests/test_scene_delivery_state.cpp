#include <gtest/gtest.h>

#include <limits>
#include <random>

#include <radray/runtime/render_scene/scene_delivery_state.h>
#include <radray/types.h>

namespace radray {
namespace {

void ExpectSameFlight(const SceneFlightState& actual, const SceneFlightState& expected) {
    EXPECT_EQ(actual.Phase, expected.Phase);
    EXPECT_EQ(actual.UpdateSequence, expected.UpdateSequence);
    EXPECT_EQ(actual.FrameSerial, expected.FrameSerial);
}

TEST(SceneDeliveryState, InitialStateAndFullCycle) {
    SceneDeliveryState delivery;
    SceneFlightState frame;
    EXPECT_TRUE(delivery.IsRunningGT());
    EXPECT_EQ(frame.Phase, SceneFlightPhase::Writable);
    EXPECT_EQ(frame.UpdateSequence, 0u);
    EXPECT_EQ(frame.FrameSerial, 0u);

    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    EXPECT_EQ(frame.UpdateSequence, 1u);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryConsume(frame, 7), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryComplete(frame, 7), SceneDeliveryError::None);
    EXPECT_EQ(frame.Phase, SceneFlightPhase::Writable);
    EXPECT_EQ(frame.FrameSerial, 7u);
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    EXPECT_EQ(frame.UpdateSequence, 2u);
    EXPECT_EQ(frame.FrameSerial, 0u);
}

TEST(SceneDeliveryState, ValidationDoesNotCommitBeforePayloadWork) {
    SceneDeliveryState delivery;
    SceneFlightState frame;
    auto before = frame;
    ASSERT_EQ(delivery.ValidateSeal(frame), SceneDeliveryError::None);
    ExpectSameFlight(frame, before);
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    before = frame;
    ASSERT_EQ(delivery.ValidateConsume(frame, 1), SceneDeliveryError::None);
    ExpectSameFlight(frame, before);
    ASSERT_EQ(delivery.TryConsume(frame, 1), SceneDeliveryError::None);
    before = frame;
    ASSERT_EQ(delivery.ValidateComplete(frame, 1), SceneDeliveryError::None);
    ExpectSameFlight(frame, before);
}

TEST(SceneDeliveryState, RejectedSealDoesNotAdvanceSequence) {
    SceneDeliveryState delivery;
    SceneFlightState first, second;
    ASSERT_EQ(delivery.TrySeal(first), SceneDeliveryError::None);
    const auto before = first;
    EXPECT_EQ(delivery.TrySeal(first), SceneDeliveryError::Occupied);
    ExpectSameFlight(first, before);
    ASSERT_EQ(delivery.TrySeal(second), SceneDeliveryError::None);
    EXPECT_EQ(second.UpdateSequence, 2u);
}

TEST(SceneDeliveryState, PublicationFollowsSequenceNotSlotIndex) {
    SceneDeliveryState delivery;
    SceneFlightState first, second;
    ASSERT_EQ(delivery.TrySeal(second), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TrySeal(first), SceneDeliveryError::None);
    const auto before = first;
    EXPECT_EQ(delivery.TryPublish(first), SceneDeliveryError::PublicationOrder);
    ExpectSameFlight(first, before);
    ASSERT_EQ(delivery.TryPublish(second), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(first), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryPublish(first), SceneDeliveryError::PublicationOrder);
}

TEST(SceneDeliveryState, ConsumptionRejectsOutOfOrderAndDuplicateFrames) {
    SceneDeliveryState delivery;
    SceneFlightState first, second;
    ASSERT_EQ(delivery.TrySeal(first), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(first), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TrySeal(second), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(second), SceneDeliveryError::None);
    const auto before = second;
    EXPECT_EQ(delivery.TryConsume(second, 1), SceneDeliveryError::ConsumptionOrder);
    ExpectSameFlight(second, before);
    ASSERT_EQ(delivery.TryConsume(first, 1), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryConsume(first, 2), SceneDeliveryError::ConsumptionOrder);
    ASSERT_EQ(delivery.TryConsume(second, 2), SceneDeliveryError::None);
}

TEST(SceneDeliveryState, SerialMaySkipButMustNotRepeatOrWrap) {
    SceneDeliveryState delivery;
    SceneFlightState frame;
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryConsume(frame, 0), SceneDeliveryError::ConsumptionOrder);
    ASSERT_EQ(delivery.TryConsume(frame, 100), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryComplete(frame, 100), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryConsume(frame, 99), SceneDeliveryError::ConsumptionOrder);
    EXPECT_EQ(delivery.TryConsume(frame, 100), SceneDeliveryError::ConsumptionOrder);
    const auto maxSerial = std::numeric_limits<uint64_t>::max();
    ASSERT_EQ(delivery.TryConsume(frame, maxSerial), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryComplete(frame, maxSerial), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryConsume(frame, 1), SceneDeliveryError::ConsumptionOrder);
}

TEST(SceneDeliveryState, CompletionRejectsEveryUnconsumedPhase) {
    SceneDeliveryState delivery;
    SceneFlightState frame;
    EXPECT_EQ(delivery.TryComplete(frame, 0), SceneDeliveryError::CompletionMismatch);
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryComplete(frame, 0), SceneDeliveryError::CompletionMismatch);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryComplete(frame, 1), SceneDeliveryError::CompletionMismatch);
}

TEST(SceneDeliveryState, StaleCompletionCannotReleaseReusedSlot) {
    SceneDeliveryState delivery;
    SceneFlightState frame;
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryConsume(frame, 1), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryComplete(frame, 1), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryComplete(frame, 1), SceneDeliveryError::CompletionMismatch);
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryConsume(frame, 2), SceneDeliveryError::None);
    const auto before = frame;
    EXPECT_EQ(delivery.TryComplete(frame, 1), SceneDeliveryError::CompletionMismatch);
    ExpectSameFlight(frame, before);
    ASSERT_EQ(delivery.TryComplete(frame, 2), SceneDeliveryError::None);
}

TEST(SceneDeliveryState, CompletionUsesItsOwnFlightNotLatestConsumedSerial) {
    SceneDeliveryState delivery;
    SceneFlightState first, second;
    ASSERT_EQ(delivery.TrySeal(first), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(first), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TrySeal(second), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(second), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryConsume(first, 10), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryConsume(second, 20), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryComplete(second, 20), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryComplete(first, 10), SceneDeliveryError::None);
}

TEST(SceneDeliveryState, StoppingStillDrainsPublishedWork) {
    SceneDeliveryState delivery;
    SceneFlightState frame, empty;
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
    delivery.BeginStoppingGT();
    EXPECT_FALSE(delivery.IsRunningGT());
    EXPECT_EQ(delivery.TrySeal(empty), SceneDeliveryError::Stopping);
    EXPECT_EQ(delivery.TryAbandon(frame), SceneDeliveryError::InvalidAbandon);
    ASSERT_EQ(delivery.TryConsume(frame, 1), SceneDeliveryError::None);
    EXPECT_EQ(delivery.TryAbandon(frame), SceneDeliveryError::InvalidAbandon);
    ASSERT_EQ(delivery.TryComplete(frame, 1), SceneDeliveryError::None);
    ASSERT_EQ(delivery.TryAbandon(frame), SceneDeliveryError::None);
}

TEST(SceneDeliveryState, TerminalAbandonCannotRestartPublication) {
    SceneDeliveryState delivery;
    SceneFlightState frame, empty;
    ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
    const auto before = frame;
    EXPECT_EQ(delivery.TryAbandon(frame), SceneDeliveryError::InvalidAbandon);
    ExpectSameFlight(frame, before);
    delivery.BeginStoppingGT();
    EXPECT_EQ(delivery.TryPublish(frame), SceneDeliveryError::PublicationOrder);
    ASSERT_EQ(delivery.ValidateAbandon(frame), SceneDeliveryError::None);
    ExpectSameFlight(frame, before);
    ASSERT_EQ(delivery.TryAbandon(frame), SceneDeliveryError::None);
    delivery.BeginStoppingGT();
    EXPECT_FALSE(delivery.IsRunningGT());
    EXPECT_EQ(delivery.TrySeal(frame), SceneDeliveryError::Stopping);
    ASSERT_EQ(delivery.TryAbandon(empty), SceneDeliveryError::None);
}

TEST(SceneDeliveryState, RandomizedFlightReuseKeepsIdentitiesDistinct) {
    std::mt19937 rng(0x5146u);
    for (uint32_t flightCount : {1u, 2u, 3u, 8u}) {
        SceneDeliveryState delivery;
        array<SceneFlightState, 8> frames{};
        uint64_t lastSerial = 0;
        for (uint64_t sequence = 1; sequence <= 10000; ++sequence) {
            auto& frame = frames[rng() % flightCount];
            const auto oldSerial = frame.FrameSerial;
            ASSERT_EQ(delivery.TrySeal(frame), SceneDeliveryError::None);
            ASSERT_EQ(frame.UpdateSequence, sequence);
            ASSERT_EQ(delivery.TryPublish(frame), SceneDeliveryError::None);
            const auto serial = lastSerial + 1 + rng() % 7;
            ASSERT_EQ(delivery.TryConsume(frame, serial), SceneDeliveryError::None);
            const auto before = frame;
            EXPECT_EQ(delivery.TryComplete(frame, oldSerial), SceneDeliveryError::CompletionMismatch);
            ExpectSameFlight(frame, before);
            ASSERT_EQ(delivery.TryComplete(frame, serial), SceneDeliveryError::None);
            lastSerial = serial;
        }
    }
}

}  // namespace
}  // namespace radray
