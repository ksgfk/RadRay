#include <chrono>
#include <condition_variable>
#include <mutex>

#include <gtest/gtest.h>

#include <radray/runtime/runtime_driver.h>

using namespace radray;
using namespace radray::runtime;

namespace {

struct FakeSinkState {
    std::mutex Mutex{};
    std::condition_variable Cv{};
    vector<uint64_t> RenderedFrameIds{};
    uint32_t WaitIdleCalls{0};
};

class FakeSink final : public IRenderFrameSink {
public:
    explicit FakeSink(shared_ptr<FakeSinkState> state) noexcept
        : _state(std::move(state)) {}

    bool RenderFrame(const FrameSnapshot& snapshot, Nullable<string*> reason = nullptr) noexcept override {
        (void)reason;
        std::scoped_lock lock{_state->Mutex};
        _state->RenderedFrameIds.push_back(snapshot.Header.FrameId);
        _state->Cv.notify_all();
        return true;
    }

    void WaitIdle() noexcept override {
        std::scoped_lock lock{_state->Mutex};
        ++_state->WaitIdleCalls;
        _state->Cv.notify_all();
    }

private:
    shared_ptr<FakeSinkState> _state{};
};

FrameSnapshot BuildDriverSnapshot(FrameSnapshotBuilder& builder, FrameSnapshotSlot& slot, uint64_t frameId) {
    builder.ResetFromSlot(slot, frameId, frameId);
    builder.AddView() = RenderViewRequest{
        .ViewId = 0,
        .Type = RenderViewType::MainColor,
        .CameraId = 1,
        .OutputWidth = 64,
        .OutputHeight = 64,
    };
    builder.AddCamera() = CameraRenderData{
        .CameraId = 1,
        .ViewId = 0,
        .OutputWidth = 64,
        .OutputHeight = 64,
    };

    string reason{};
    FrameSnapshot snapshot = builder.Finalize(&reason);
    EXPECT_TRUE(reason.empty()) << reason;
    return snapshot;
}

bool PublishSnapshot(RuntimeDriver& driver, uint64_t frameId) {
    FrameSnapshotSlot* slot = driver.BeginSnapshotBuild();
    EXPECT_NE(slot, nullptr);
    if (slot == nullptr) {
        return false;
    }

    FrameSnapshotBuilder builder = driver.CreateSnapshotBuilder(*slot);
    FrameSnapshot snapshot = BuildDriverSnapshot(builder, *slot, frameId);
    return driver.PublishSnapshot(*slot, std::move(snapshot));
}

bool WaitForRenderedFrames(const shared_ptr<FakeSinkState>& state, size_t count) {
    std::unique_lock lock{state->Mutex};
    return state->Cv.wait_for(lock, std::chrono::seconds(2), [&] {
        return state->RenderedFrameIds.size() >= count;
    });
}

}  // namespace

TEST(RuntimeDriverTest, SingleThreadTickConsumesOnlyLatestSnapshot) {
    auto state = make_shared<FakeSinkState>();

    RuntimeDriverCreateDesc desc{};
    desc.Mode = RuntimeDriverMode::SingleThread;
    desc.FrameSink = make_unique<FakeSink>(state);

    string reason{};
    auto driverOpt = RuntimeDriver::Create(std::move(desc), &reason);
    ASSERT_TRUE(driverOpt.HasValue()) << reason;
    auto driver = driverOpt.Release();

    ASSERT_TRUE(PublishSnapshot(*driver, 1));
    ASSERT_TRUE(PublishSnapshot(*driver, 2));

    ASSERT_TRUE(driver->TickSingleThread(&reason)) << reason;
    {
        std::scoped_lock lock{state->Mutex};
        ASSERT_EQ(state->RenderedFrameIds.size(), 1u);
        EXPECT_EQ(state->RenderedFrameIds[0], 2u);
    }

    reason.clear();
    EXPECT_FALSE(driver->TickSingleThread(&reason));
    EXPECT_EQ(reason, "no published frame snapshot is available");

    driver->Destroy();
    {
        std::scoped_lock lock{state->Mutex};
        EXPECT_GE(state->WaitIdleCalls, 1u);
    }
}

TEST(RuntimeDriverTest, DualThreadWorkerConsumesPublishedSnapshotAndStopsCleanly) {
    auto state = make_shared<FakeSinkState>();

    RuntimeDriverCreateDesc desc{};
    desc.Mode = RuntimeDriverMode::DualThread;
    desc.FrameSink = make_unique<FakeSink>(state);

    string reason{};
    auto driverOpt = RuntimeDriver::Create(std::move(desc), &reason);
    ASSERT_TRUE(driverOpt.HasValue()) << reason;
    auto driver = driverOpt.Release();

    ASSERT_TRUE(PublishSnapshot(*driver, 7));
    ASSERT_TRUE(WaitForRenderedFrames(state, 1));
    {
        std::scoped_lock lock{state->Mutex};
        ASSERT_EQ(state->RenderedFrameIds.size(), 1u);
        EXPECT_EQ(state->RenderedFrameIds[0], 7u);
    }

    driver->Destroy();
    {
        std::scoped_lock lock{state->Mutex};
        EXPECT_GE(state->WaitIdleCalls, 1u);
    }
}
