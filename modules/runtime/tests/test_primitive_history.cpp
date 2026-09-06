#include <gtest/gtest.h>
#include <radray/runtime/render_framework/primitive_history.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/view_state.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/render_framework/scene.h>
#include "graph_compile_device.h"

namespace radray {
namespace {

RenderSceneSnapshot Snapshot(float x, uint64_t generation = 1, uint64_t revision = 0) {
    RenderSceneSnapshot snapshot;
    RenderPrimitiveData primitive;
    primitive.LocalToWorld(0, 3) = x;
    primitive.Generation = generation;
    primitive.MotionRevision = revision;
    snapshot.Primitives.push_back(primitive);
    return snapshot;
}

TEST(PrimitiveHistory, T05FailedFrameDoesNotBecomePrevious) {
    PrimitiveHistory history;
    auto snapshot = Snapshot(0);
    ASSERT_TRUE(history.Prepare(snapshot, 1));
    ASSERT_TRUE(history.Commit(1));
    snapshot = Snapshot(1);
    ASSERT_TRUE(history.Prepare(snapshot, 2));
    ASSERT_TRUE(history.Commit(2));
    snapshot = Snapshot(2);
    ASSERT_TRUE(history.Prepare(snapshot, 3));
    snapshot = Snapshot(3);
    ASSERT_TRUE(history.Prepare(snapshot, 4));
    const auto motion = history.Lookup(snapshot.Primitives[0]);
    EXPECT_TRUE(motion.Valid);
    EXPECT_FLOAT_EQ(motion.PreviousLocalToWorld(0, 3), 1);
    EXPECT_EQ(history.CommittedSerial(), 2u);
    EXPECT_FALSE(history.Commit(3));
    EXPECT_TRUE(history.Commit(4));
    EXPECT_FALSE(history.Commit(4));
    EXPECT_EQ(history.CommittedSerial(), 4u);
}

TEST(PrimitiveHistory, T06ViewsKeepIndependentSuccessfulTransforms) {
    PrimitiveHistory a, b;
    ASSERT_TRUE(a.Prepare(Snapshot(1), 1));
    ASSERT_TRUE(a.Commit(1));
    ASSERT_TRUE(b.Prepare(Snapshot(4), 4));
    ASSERT_TRUE(b.Commit(4));
    const auto current = Snapshot(7);
    ASSERT_TRUE(a.Prepare(current, 7));
    ASSERT_TRUE(b.Prepare(current, 7));
    EXPECT_FLOAT_EQ(a.Lookup(current.Primitives[0]).PreviousLocalToWorld(0, 3), 1);
    EXPECT_FLOAT_EQ(b.Lookup(current.Primitives[0]).PreviousLocalToWorld(0, 3), 4);
    ASSERT_TRUE(a.Commit(7));
    EXPECT_FLOAT_EQ(b.Lookup(current.Primitives[0]).PreviousLocalToWorld(0, 3), 4);
    EXPECT_EQ(b.CommittedSerial(), 4u);
}

TEST(PrimitiveHistory, T09S03GenerationRevisionOrderingAndDeletion) {
    PrimitiveHistory history;
    auto snapshot = Snapshot(1, 11);
    snapshot.Primitives.push_back(Snapshot(2, 22).Primitives[0]);
    ASSERT_TRUE(history.Prepare(snapshot, 1));
    ASSERT_TRUE(history.Commit(1));
    std::reverse(snapshot.Primitives.begin(), snapshot.Primitives.end());
    EXPECT_FLOAT_EQ(history.Lookup(snapshot.Primitives[0]).PreviousLocalToWorld(0, 3), 2);
    snapshot.Primitives[0].MotionRevision = 1;
    for (uint32_t flight = 0; flight < 3; ++flight) EXPECT_FALSE(history.Lookup(snapshot.Primitives[0]).Valid);
    snapshot.Primitives[1].Generation = 33;
    EXPECT_FALSE(history.Lookup(snapshot.Primitives[1]).Valid);
    ASSERT_TRUE(history.Prepare(snapshot, 2));
    ASSERT_TRUE(history.Commit(2));
    EXPECT_TRUE(history.Lookup(snapshot.Primitives[0]).Valid);
    snapshot.Primitives.erase(snapshot.Primitives.begin());
    ASSERT_TRUE(history.Prepare(snapshot, 3));
    ASSERT_TRUE(history.Commit(3));
    EXPECT_EQ(history.Size(), 1u);
    EXPECT_FALSE(history.Lookup(Snapshot(2, 22).Primitives[0]).Valid);
    history.Invalidate();
    const auto motion = history.Lookup(snapshot.Primitives[0]);
    EXPECT_FALSE(motion.Valid);
    EXPECT_TRUE(motion.PreviousLocalToWorld.isApprox(snapshot.Primitives[0].LocalToWorld));
}

TEST(PrimitiveHistory, InvalidSnapshotsCannotPublishPartialMaps) {
    PrimitiveHistory history;
    ASSERT_TRUE(history.Prepare(Snapshot(1), 1));
    ASSERT_TRUE(history.Commit(1));
    auto invalid = Snapshot(2);
    invalid.Primitives.push_back(invalid.Primitives.front());
    EXPECT_FALSE(history.Prepare(invalid, 2));
    EXPECT_FALSE(history.Commit(2));
    EXPECT_EQ(history.CommittedSerial(), 1u);
    EXPECT_EQ(history.Size(), 1u);
    invalid = Snapshot(2, 0);
    EXPECT_FALSE(history.Prepare(invalid, 2));
    invalid = Snapshot(std::numeric_limits<float>::infinity());
    EXPECT_FALSE(history.Prepare(invalid, 2));
}

class SnapshotProxy : public PrimitiveSceneProxy {
public:
    Eigen::Matrix4f Matrix{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f GetLocalToWorld() const noexcept override { return Matrix; }
};
class SnapshotComponent : public PrimitiveComponent {
public:
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override { return make_unique<SnapshotProxy>(); }
};

TEST(RenderSceneIdentity, S01S02SnapshotOwnsGenerationRevisionAndTransform) {
    SnapshotComponent component;
    Scene scene;
    auto* proxy = static_cast<SnapshotProxy*>(scene.AddPrimitive(&component));
    ASSERT_NE(proxy, nullptr);
    RenderSceneSnapshot first, second;
    vector<StreamingAssetRefAny> retained;
    ASSERT_TRUE(BuildRenderSceneSnapshot(scene, first, retained));
    const auto generation = first.Primitives[0].Generation;
    EXPECT_EQ(generation, proxy->GetGeneration());
    proxy->Matrix(0, 3) = 5;
    proxy->ResetMotion();
    ASSERT_TRUE(BuildRenderSceneSnapshot(scene, second, retained));
    EXPECT_FLOAT_EQ(first.Primitives[0].LocalToWorld(0, 3), 0);
    EXPECT_EQ(first.Primitives[0].MotionRevision, 0u);
    EXPECT_FLOAT_EQ(second.Primitives[0].LocalToWorld(0, 3), 5);
    EXPECT_EQ(second.Primitives[0].MotionRevision, 1u);
    scene.RemovePrimitive(proxy);
    EXPECT_EQ(first.Primitives[0].Generation, generation);
    alignas(SnapshotProxy) byte storage[sizeof(SnapshotProxy)];
    auto* sameAddress = new (storage) SnapshotProxy;
    const auto old = sameAddress->GetGeneration();
    sameAddress->~SnapshotProxy();
    sameAddress = new (storage) SnapshotProxy;
    EXPECT_NE(sameAddress->GetGeneration(), old);
    sameAddress->~SnapshotProxy();
}

TEST(ViewTemporalState, T07T10CutsUnavailableViewsAndAtomicMotionCommit) {
    test::GraphCompileDevice device;
    render::RenderPassRegistry passes(&device);
    ViewStateRegistry registry(device, passes, 3);
    ResolvedRenderViewFamily family;
    family.OutputAvailable = true;
    family.RenderSize = {16, 16};
    family.OutputFormat = render::TextureFormat::RGBA8_UNORM;
    ResolvedRenderView view;
    view.StateId = AllocateViewStateId();
    view.ViewProjection.setIdentity();
    registry.BeginFlight(0, 1);
    registry.Resolve(view, family);
    ASSERT_TRUE(registry.PreparePrimitiveHistory(view, Snapshot(1)));
    EXPECT_FALSE(registry.CommitView(view.StateId));
    EXPECT_TRUE(registry.CommitViewWithHistory(view.StateId, {}));
    for (uint64_t serial = 2; serial < 5; ++serial) {
        registry.BeginFlight(static_cast<uint32_t>((serial - 1) % 3), serial);
        family.OutputAvailable = false;
        view.ViewProjection(0, 3) = static_cast<float>(serial);
        registry.Resolve(view, family);
        EXPECT_FALSE(registry.PreparePrimitiveHistory(view, Snapshot(static_cast<float>(serial))));
        EXPECT_FALSE(registry.CommitViewWithHistory(view.StateId, {}));
        EXPECT_EQ(registry.GetCommittedSerial(view.StateId), 1u);
        EXPECT_EQ(registry.GetPrimitiveCommittedSerial(view.StateId), 1u);
    }
    registry.BeginFlight(1, 5);
    family.OutputAvailable = true;
    view.CameraCut = true;
    registry.Resolve(view, family);
    ASSERT_TRUE(registry.PreparePrimitiveHistory(view, Snapshot(5)));
    EXPECT_FALSE(view.PreviousViewValid);
    EXPECT_FALSE(registry.GetPrimitiveMotion(view.StateId, Snapshot(5).Primitives[0]).Valid);
    ASSERT_TRUE(registry.CommitViewWithHistory(view.StateId, {}));
    EXPECT_EQ(registry.GetCommittedSerial(view.StateId), 5u);
    EXPECT_EQ(registry.GetPrimitiveCommittedSerial(view.StateId), 5u);
    EXPECT_EQ(device.NativeCreates, 0u);
}

}  // namespace
}  // namespace radray
