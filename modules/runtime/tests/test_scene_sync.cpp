#include "scene_sync_workload.h"
#include <gtest/gtest.h>
#include <fmt/format.h>

namespace radray::test {

TEST(SceneSyncCorrectness, AllWorkloadsAndFlights) {
    for (const auto& scenario : SceneSyncScenarios(true))
        for (uint32_t flights : {1u, 2u, 3u})
            for (bool threaded : {false, true}) {
                SCOPED_TRACE(fmt::format("{} F={} threaded={}", scenario.Name, flights, threaded));
                SceneSyncWorkload workload{scenario, flights, threaded, true};
                workload.RunFrames(40);
                EXPECT_TRUE(workload.FinishAndValidate());
            }
}

TEST(SceneSyncCorrectness, ProducerFillsAllFlightsBeforeConsumerStarts) {
    for (uint32_t flights : {1u, 2u, 3u}) {
        SceneSyncWorkload workload{{"pipeline_gate", Workload::Transform, 128, 32}, flights, true, true, true};
        workload.RunFrames(12);
        EXPECT_TRUE(workload.FinishAndValidate());
        EXPECT_EQ(workload.PublishedWhileGated(), flights);
    }
}

TEST(SceneSyncCorrectness, LifecycleFrameModesUseTheSameVerifiedWorkload) {
    for (uint32_t depth : {1u, 8u})
        for (bool unique : {false, true})
            for (uint32_t changes : {0u, 1u, 8u, 64u})
                for (uint32_t flights : {1u, 2u, 3u})
                    for (bool threaded : {false, true}) {
                        SCOPED_TRACE(fmt::format("depth={} unique={} changes={} F={} threaded={}", depth, unique, changes, flights, threaded));
                        Scenario scenario{.Name = "lifecycle", .Shapes = 64, .Changes = changes, .Depth = depth, .Sequential = true, .UniqueAssets = unique, .Views = 3};
                        SceneSyncWorkload workload{scenario, flights, threaded, true};
                        workload.RunFrames(8);
                        EXPECT_TRUE(workload.FinishAndValidate());
                    }
}

}  // namespace radray::test
