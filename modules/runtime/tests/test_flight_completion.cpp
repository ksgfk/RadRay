#include <atomic>
#include <thread>

#include <gtest/gtest.h>
#include <radray/runtime/flight_completion.h>

namespace radray {
namespace {

class FlightCompletionTest : public testing::Test {};

TEST_F(FlightCompletionTest, CrossThreadPushDrainsExactlyOnceOnGameThread) {
    FlightCompletionQueue queue;
    std::thread worker([&] {
        queue.Push({.FlightIndex = 1, .GpuWorkCompleted = true});
        queue.Push({.FlightIndex = 2, .GpuWorkCompleted = false});
    });
    worker.join();

    {
        FlightCompletionQueue::Drain drain{queue};
        ASSERT_EQ(drain.Items().size(), 2u);
        EXPECT_EQ(drain.Items()[0].FlightIndex, 1u);
        EXPECT_TRUE(drain.Items()[0].GpuWorkCompleted);
        EXPECT_EQ(drain.Items()[1].FlightIndex, 2u);
        EXPECT_FALSE(drain.Items()[1].GpuWorkCompleted);
    }

    FlightCompletionQueue::Drain again{queue};
    EXPECT_TRUE(again.Items().empty());
}

TEST_F(FlightCompletionTest, NestedDrainIsEmptyAndDoesNotDropItems) {
    FlightCompletionQueue queue;
    queue.Push({.FlightIndex = 3});
    FlightCompletionQueue::Drain outer{queue};
    ASSERT_EQ(outer.Items().size(), 1u);
    EXPECT_EQ(outer.Items()[0].FlightIndex, 3u);
    {
        FlightCompletionQueue::Drain nested{queue};
        EXPECT_TRUE(nested.Items().empty());
        queue.Push({.FlightIndex = 4});
    }
    ASSERT_EQ(outer.Items().size(), 1u);
    EXPECT_EQ(outer.Items()[0].FlightIndex, 3u);
}

TEST_F(FlightCompletionTest, QueueIsEmptyAfterDrainDestroys) {
    FlightCompletionQueue queue;
    queue.Push({.FlightIndex = 5});
    {
        FlightCompletionQueue::Drain drain{queue};
        ASSERT_EQ(drain.Items().size(), 1u);
    }
    FlightCompletionQueue::Drain after{queue};
    EXPECT_TRUE(after.Items().empty());
}

TEST_F(FlightCompletionTest, ConcurrentPushAndRepeatedDrainConserveCount) {
    FlightCompletionQueue queue;
    std::atomic_bool done{false};
    std::atomic<uint32_t> pushed{0};
    std::thread worker([&] {
        while (!done.load(std::memory_order_acquire)) {
            queue.Push({.FlightIndex = 0, .GpuWorkCompleted = true});
            pushed.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    uint32_t drained = 0;
    for (uint32_t i = 0; i < 2000; ++i) {
        FlightCompletionQueue::Drain drain{queue};
        drained += static_cast<uint32_t>(drain.Items().size());
    }
    done.store(true, std::memory_order_release);
    worker.join();
    {
        FlightCompletionQueue::Drain drain{queue};
        drained += static_cast<uint32_t>(drain.Items().size());
    }
    EXPECT_EQ(drained, pushed.load(std::memory_order_relaxed));
}

}  // namespace
}  // namespace radray
