#include <gtest/gtest.h>

#include <radray/runtime/frame_snapshot_builder.h>
#include <radray/runtime/frame_snapshot_queue.h>

using namespace radray;
using namespace radray::runtime;

namespace {

FrameSnapshot BuildSnapshot(uint64_t frameId, uint32_t viewId, uint32_t cameraId) {
    FrameSnapshotBuilder builder{};
    builder.Reset(frameId, frameId);
    builder.AddView() = RenderViewRequest{
        .ViewId = viewId,
        .Type = RenderViewType::MainColor,
        .CameraId = cameraId,
        .OutputWidth = 64,
        .OutputHeight = 64,
    };
    builder.AddCamera() = CameraRenderData{
        .CameraId = cameraId,
        .ViewId = viewId,
        .OutputWidth = 64,
        .OutputHeight = 64,
    };
    return builder.Finalize();
}

}  // namespace

TEST(FrameSnapshotQueueTest, PublishAndAcquireLatestSnapshot) {
    FrameSnapshotQueue queue{};
    FrameSnapshotSlot* slot0 = queue.BeginBuild();
    ASSERT_NE(slot0, nullptr);
    EXPECT_TRUE(queue.Publish(*slot0, BuildSnapshot(1, 0, 1)));

    uint64_t frameId = 0;
    const FrameSnapshot* snapshot = queue.AcquireLatestForRender(&frameId);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(frameId, 1u);
    EXPECT_EQ(snapshot->Header.FrameId, 1u);

    queue.ReleaseRendered(frameId);
    EXPECT_EQ(queue.AcquireLatestForRender(), nullptr);
}

TEST(FrameSnapshotQueueTest, AcquireReturnsOnlyNewestPublishedSnapshot) {
    FrameSnapshotQueue queue{};

    auto* slot0 = queue.BeginBuild();
    ASSERT_NE(slot0, nullptr);
    ASSERT_TRUE(queue.Publish(*slot0, BuildSnapshot(1, 0, 1)));

    auto* slot1 = queue.BeginBuild();
    ASSERT_NE(slot1, nullptr);
    ASSERT_TRUE(queue.Publish(*slot1, BuildSnapshot(2, 1, 2)));

    uint64_t frameId = 0;
    const FrameSnapshot* snapshot = queue.AcquireLatestForRender(&frameId);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(frameId, 2u);
    EXPECT_EQ(snapshot->Header.FrameId, 2u);
}

TEST(FrameSnapshotQueueTest, BuilderSnapshotIsNotVisibleBeforePublish) {
    FrameSnapshotQueue queue{};
    auto* slot = queue.BeginBuild();
    ASSERT_NE(slot, nullptr);

    FrameSnapshotBuilder builder = queue.CreateBuilder(*slot);
    builder.ResetFromSlot(*slot, 3, 3);
    builder.AddView() = RenderViewRequest{
        .ViewId = 0,
        .Type = RenderViewType::MainColor,
        .CameraId = 7,
        .OutputWidth = 32,
        .OutputHeight = 32,
    };
    builder.AddCamera() = CameraRenderData{
        .CameraId = 7,
        .ViewId = 0,
        .OutputWidth = 32,
        .OutputHeight = 32,
    };

    EXPECT_EQ(queue.AcquireLatestForRender(), nullptr);
    FrameSnapshot snapshot = builder.Finalize();
    ASSERT_TRUE(queue.Publish(*slot, std::move(snapshot)));
    EXPECT_NE(queue.AcquireLatestForRender(), nullptr);
}
