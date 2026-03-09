#include <gtest/gtest.h>

#include <radray/runtime/frame_snapshot.h>

using namespace radray;
using namespace radray::runtime;

TEST(FrameSnapshotBuilderTest, FinalizePopulatesCountsAndData) {
    FrameSnapshotBuilder builder{};
    builder.Reset(42, 7, 1.25);

    auto& view = builder.AddView();
    view.ViewId = 0;
    view.CameraId = 9;
    view.OutputWidth = 1280;
    view.OutputHeight = 720;

    auto& camera = builder.AddCamera();
    camera.CameraId = 9;
    camera.ViewId = 0;
    camera.OutputWidth = 1280;
    camera.OutputHeight = 720;
    camera.ViewProj = Eigen::Matrix4f::Identity();

    auto& batch = builder.AddMeshBatch();
    batch.Mesh = MeshHandle{1};
    batch.Material = MaterialHandle{2};
    batch.SubmeshIndex = 3;
    batch.ViewMask = 0;
    batch.SortKeyHigh = 11;
    batch.SortKeyLow = 13;

    string reason{};
    FrameSnapshot snapshot = builder.Finalize(&reason);

    ASSERT_TRUE(reason.empty());
    EXPECT_EQ(snapshot.Header.FrameId, 42u);
    EXPECT_EQ(snapshot.Header.SimulationTick, 7u);
    EXPECT_DOUBLE_EQ(snapshot.Header.CpuTimeSeconds, 1.25);
    EXPECT_EQ(snapshot.Header.ViewCount, 1u);
    EXPECT_EQ(snapshot.Header.CameraCount, 1u);
    EXPECT_EQ(snapshot.Header.MeshBatchCount, 1u);
    ASSERT_EQ(snapshot.Views.size(), 1u);
    ASSERT_EQ(snapshot.Cameras.size(), 1u);
    ASSERT_EQ(snapshot.MeshBatches.size(), 1u);
    EXPECT_EQ(snapshot.Views[0].OutputWidth, 1280u);
    EXPECT_EQ(snapshot.Cameras[0].CameraId, 9u);
    EXPECT_EQ(snapshot.MeshBatches[0].SubmeshIndex, 3u);
}

TEST(FrameSnapshotBuilderTest, ResetDropsPreviousData) {
    FrameSnapshotBuilder builder{};
    builder.Reset(1, 1);
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
    builder.AddMeshBatch() = VisibleMeshBatch{
        .Mesh = MeshHandle{1},
        .Material = MaterialHandle{1},
    };
    FrameSnapshot first = builder.Finalize();
    ASSERT_EQ(first.Header.MeshBatchCount, 1u);

    builder.Reset(2, 3);
    builder.AddView() = RenderViewRequest{
        .ViewId = 4,
        .Type = RenderViewType::MainColor,
        .CameraId = 5,
        .OutputWidth = 32,
        .OutputHeight = 16,
    };
    builder.AddCamera() = CameraRenderData{
        .CameraId = 5,
        .ViewId = 4,
        .OutputWidth = 32,
        .OutputHeight = 16,
    };

    FrameSnapshot second = builder.Finalize();
    EXPECT_EQ(second.Header.FrameId, 2u);
    EXPECT_EQ(second.Header.SimulationTick, 3u);
    EXPECT_EQ(second.Header.ViewCount, 1u);
    EXPECT_EQ(second.Header.CameraCount, 1u);
    EXPECT_EQ(second.Header.MeshBatchCount, 0u);
    EXPECT_TRUE(second.MeshBatches.empty());
}

TEST(FrameSnapshotBuilderTest, FinalizeRejectsInvalidReferences) {
    FrameSnapshotBuilder builder{};
    builder.Reset(9, 10);

    builder.AddView() = RenderViewRequest{
        .ViewId = 0,
        .Type = RenderViewType::MainColor,
        .CameraId = 1,
        .OutputWidth = 256,
        .OutputHeight = 256,
    };
    builder.AddCamera() = CameraRenderData{
        .CameraId = 1,
        .ViewId = 99,
        .OutputWidth = 256,
        .OutputHeight = 256,
    };
    builder.AddMeshBatch() = VisibleMeshBatch{
        .Mesh = MeshHandle{},
        .Material = MaterialHandle{1},
    };

    string reason{};
    FrameSnapshot snapshot = builder.Finalize(&reason);

    EXPECT_TRUE(snapshot.IsEmpty());
    EXPECT_FALSE(reason.empty());
}
