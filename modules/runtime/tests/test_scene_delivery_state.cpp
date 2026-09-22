#include <gtest/gtest.h>

#include <cstdio>
#include <limits>
#include <random>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

class SceneDeliveryState : public ::testing::Test {
protected:
    Application App;
    RenderSystem Renderer{&App, 3};
};

class SceneDeliveryStateDeathTest : public SceneDeliveryState {
protected:
    void SetUp() override {
        SetLogCallback([](LogLevel, std::string_view message, void*) { fmt::print(stderr, "{}\n", message); }, nullptr);
    }

    void TearDown() override { ClearLogCallback(); }
};

TEST_F(SceneDeliveryState, PublicationAndCompletionFollowFrameIdentityRatherThanSlotOrder) {
    EXPECT_EQ(Renderer.GetUpdateSequence(0), 0u);
    EXPECT_EQ(Renderer.GetFrameSerial(0), 0u);
    Renderer.SealFrameGT(2);
    Renderer.SealFrameGT(0);
    EXPECT_EQ(Renderer.GetUpdateSequence(2), 1u);
    EXPECT_EQ(Renderer.GetUpdateSequence(0), 2u);
    Renderer.PublishFrameGT(2);
    Renderer.PublishFrameGT(0);
    Renderer.ConsumeRenderUpdates(2, 10);
    Renderer.ConsumeRenderUpdates(0, 20);
    Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 20});
    Renderer.OnFlightCompletedGT({.FlightIndex = 2, .FrameSerial = 10});
    Renderer.SealFrameGT(2);
    EXPECT_EQ(Renderer.GetUpdateSequence(2), 3u);
    EXPECT_EQ(Renderer.GetFrameSerial(2), 0u);
}

TEST_F(SceneDeliveryStateDeathTest, RejectsUnconsumedCompletionAndOccupiedSeal) {
    EXPECT_DEATH(Renderer.OnFlightCompletedGT({.FlightIndex = 0}), "Stale or unconsumed");
    Renderer.SealFrameGT(0);
    EXPECT_DEATH(Renderer.SealFrameGT(0), "still occupied");
    EXPECT_DEATH(Renderer.OnFlightCompletedGT({.FlightIndex = 0}), "Stale or unconsumed");
    Renderer.PublishFrameGT(0);
    EXPECT_DEATH(Renderer.SealFrameGT(0), "still occupied");
    EXPECT_DEATH(Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 1}), "Stale or unconsumed");
    Renderer.ConsumeRenderUpdates(0, 1);
    EXPECT_DEATH(Renderer.SealFrameGT(0), "still occupied");
    Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 1});
    EXPECT_DEATH(Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 1}), "Stale or unconsumed");
}

TEST_F(SceneDeliveryStateDeathTest, RejectsOutOfOrderAndDuplicatePublication) {
    Renderer.SealFrameGT(2);
    Renderer.SealFrameGT(0);
    EXPECT_DEATH(Renderer.PublishFrameGT(0), "publication order");
    Renderer.PublishFrameGT(2);
    EXPECT_DEATH(Renderer.PublishFrameGT(2), "publication order");
    Renderer.PublishFrameGT(0);
    EXPECT_DEATH(Renderer.ConsumeRenderUpdates(0, 2), "out-of-order");
    Renderer.ConsumeRenderUpdates(2, 1);
    EXPECT_DEATH(Renderer.ConsumeRenderUpdates(2, 2), "out-of-order");
    Renderer.ConsumeRenderUpdates(0, 2);
    Renderer.OnFlightCompletedGT({.FlightIndex = 2, .FrameSerial = 1});
    Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 2});
}

TEST_F(SceneDeliveryStateDeathTest, SerialMaySkipButMustNotRepeatOrWrap) {
    Renderer.SealFrameGT(0);
    Renderer.PublishFrameGT(0);
    EXPECT_DEATH(Renderer.ConsumeRenderUpdates(0, 0), "out-of-order");
    Renderer.ConsumeRenderUpdates(0, 100);
    Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 100});
    Renderer.SealFrameGT(0);
    Renderer.PublishFrameGT(0);
    EXPECT_DEATH(Renderer.ConsumeRenderUpdates(0, 99), "out-of-order");
    EXPECT_DEATH(Renderer.ConsumeRenderUpdates(0, 100), "out-of-order");
    const auto serial = std::numeric_limits<uint64_t>::max();
    Renderer.ConsumeRenderUpdates(0, serial);
    EXPECT_DEATH(Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 100}), "Stale or unconsumed");
    EXPECT_EQ(Renderer.GetFrameSerial(0), serial);
    Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = serial});
    EXPECT_DEATH({
        Renderer.SealFrameGT(0);
        Renderer.PublishFrameGT(0);
        Renderer.ConsumeRenderUpdates(0, 1); }, "out-of-order");
}

TEST_F(SceneDeliveryStateDeathTest, StoppingDrainsPublishedFramesAndAbandonsOnlyUnpublishedFrames) {
    Renderer.SealFrameGT(0);
    Renderer.PublishFrameGT(0);
    Renderer.SealFrameGT(1);
    EXPECT_DEATH(Renderer.AbandonUnpublishedFrameGT(1), "Terminal abandon");
    Renderer.BeginStoppingGT();
    EXPECT_DEATH(Renderer.SealFrameGT(2), "after stopping");
    EXPECT_DEATH(Renderer.PublishFrameGT(1), "publication order");
    EXPECT_DEATH(Renderer.AbandonUnpublishedFrameGT(0), "Terminal abandon");
    Renderer.ConsumeRenderUpdates(0, 1);
    EXPECT_DEATH(Renderer.AbandonUnpublishedFrameGT(0), "Terminal abandon");
    Renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 1});
    Renderer.AbandonUnpublishedFramesGT();
    Renderer.BeginStoppingGT();
    EXPECT_DEATH(Renderer.SealFrameGT(1), "after stopping");
}

TEST_F(SceneDeliveryState, RandomizedFlightReusePreservesPayloadAndSubmissionIdentity) {
    std::mt19937 rng(0x5146u);
    for (uint32_t count : {1u, 2u, 3u, 8u}) {
        Application app;
        RenderSystem renderer{&app, count};
        const auto scene = renderer.CreateSceneGT();
        auto* writer = renderer.GetSceneWriterGT(scene).Get();
        const auto shape = writer->CreateShape();
        writer->SetStaticMesh(shape, {}, Eigen::Matrix4f::Identity());
        uint64_t serial = 0;
        for (uint64_t sequence = 1; sequence <= 10000; ++sequence) {
            const uint32_t flight = rng() % count;
            auto matrix = Eigen::Matrix4f::Identity().eval();
            matrix(0, 3) = static_cast<float>(sequence);
            writer->SetTransform(shape, matrix);
            renderer.SealFrameGT(flight);
            ASSERT_EQ(renderer.GetUpdateSequence(flight), sequence);
            renderer.PublishFrameGT(flight);
            serial += 1 + rng() % 7;
            renderer.ConsumeRenderUpdates(flight, serial);
            ASSERT_EQ(renderer.GetFrameSerial(flight), serial);
            const auto mesh = renderer.GetSceneRT(scene)->GetStaticMesh(shape);
            ASSERT_TRUE(mesh);
            ASSERT_FLOAT_EQ(mesh->LocalToWorld(0, 3), static_cast<float>(sequence));
            renderer.OnFlightCompletedGT({.FlightIndex = flight, .FrameSerial = serial});
            ASSERT_TRUE(renderer.GetFrameUpdatesRT(flight).empty());
        }
    }
}

}  // namespace
}  // namespace radray
