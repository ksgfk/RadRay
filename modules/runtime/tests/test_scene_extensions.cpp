#include <gtest/gtest.h>
#include <radray/runtime/application.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {
namespace {
class ExtensionProxy final : public PrimitiveSceneProxy {
public:
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
};
struct DerivedRows {
    struct Row {
        SceneObjectId Id;
        float X;
        friend bool operator==(const Row&, const Row&) = default;
    };
    vector<Row> Rows;
    uint32_t Builds{0}, Updated{0};
};
bool PrepareRows(const RenderSceneSnapshot& scene, const SceneChangeSet& changes, uint64_t scale,
                 const shared_ptr<const DerivedRows>& previous, shared_ptr<const DerivedRows>& result) {
    if (!scale || !changes.IsValid()) return false;
    auto next = previous ? make_shared<DerivedRows>(*previous) : make_shared<DerivedRows>();
    ++next->Builds;
    next->Updated = 0;
    next->Rows.resize(scene.Primitives.size());
    const auto update = [&](size_t index) {
        const auto& primitive = scene.Primitives[index];
        next->Rows[index] = {primitive.Id, primitive.LocalToWorld(0, 3) * static_cast<float>(scale)};
        ++next->Updated;
    };
    if (!previous) {
        for (size_t index = 0; index < scene.Primitives.size(); ++index) update(index);
    } else {
        for (const auto& range : changes.Get(SceneDataTable::Primitives).Ranges)
            for (size_t index = range.First; index < size_t{range.First} + range.Count; ++index) update(index);
    }
    result = std::move(next);
    return true;
}
bool OtherPrepareRows(const RenderSceneSnapshot& scene, const SceneChangeSet& changes, uint64_t scale,
                      const shared_ptr<const DerivedRows>& previous, shared_ptr<const DerivedRows>& result) {
    return PrepareRows(scene, changes, scale, previous, result);
}
SceneRenderExtension RowsContract(uint64_t scale = 2, bool everyEpoch = false) {
    return SceneRenderExtension::Make<DerivedRows, PrepareRows>(101, 1, scale, {SceneDataTable::Primitives}, everyEpoch);
}
void MoveX(PrimitiveSceneProxy& proxy, float x) {
    auto transform = Eigen::Matrix4f::Identity().eval();
    transform(0, 3) = x;
    proxy.SetLocalToWorld(transform);
}
void CheckRows(const RenderSceneSnapshot& snapshot, const SceneRenderExtension& contract) {
    const auto rows = snapshot.GetExtension<DerivedRows>(contract);
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->Rows.size(), snapshot.Primitives.size());
    for (size_t index = 0; index < rows->Rows.size(); ++index) {
        EXPECT_EQ(rows->Rows[index].Id, snapshot.Primitives[index].Id);
        EXPECT_FLOAT_EQ(rows->Rows[index].X, snapshot.Primitives[index].LocalToWorld(0, 3) * static_cast<float>(contract.Configuration));
    }
    for (const auto& value : snapshot.Extensions) {
        EXPECT_EQ(value.PublicationId, snapshot.PublicationId);
        EXPECT_EQ(value.Epoch, snapshot.SceneEpoch);
        EXPECT_EQ(value.Revision, snapshot.PublicationRevision);
    }
}

TEST(SceneExtensions, TypedWorkspaceReusesOnlyReleasedVersionsAndRetainsCapacity) {
    struct Rows {
        vector<uint64_t> Values;
        void ResetForReuse() noexcept { Values.clear(); }
    };
    SceneExtensionWorkspace workspace;
    auto& storage = workspace.GetStorage<Rows>();
    auto retained = storage.Acquire(3);
    retained->Values.assign(64, 17);
    auto next = storage.Acquire(3);
    next->Values.assign(64, 29);
    const auto address = next.get();
    const auto bytes = next->Values.data();
    auto& other = workspace.GetStorage<uint64_t>();
    auto number = other.Acquire(3);
    *number = 42;
    for (uint64_t epoch = 0; epoch < 10000; ++epoch) {
        next.reset();
        next = storage.Acquire(3);
        ASSERT_EQ(next.get(), address);
        ASSERT_TRUE(next->Values.empty());
        next->Values.assign(64, epoch);
        ASSERT_EQ(next->Values.data(), bytes);
        ASSERT_EQ(retained->Values.front(), 17u);
        ASSERT_EQ(*number, 42u);
    }
}

TEST(SceneExtensions, ConsumersShareCompatibleContractsAndIsolateConfigurations) {
    Scene scene;
    auto proxy = make_unique<ExtensionProxy>();
    MoveX(*proxy, 3);
    scene.AddPrimitive(std::move(proxy));
    AppUpdateContext app;
    RenderFramePlan frame;
    RenderWorkloadBuilder workloads{frame, {}};
    vector<StreamingAssetRefAny> owners;
    const auto a = RowsContract(2), b = RowsContract(3);
    RenderPrepareContext context{app, {}, workloads, owners, kPerformanceRenderGraphRuntimeOptions, 1};
    ASSERT_TRUE(context.RegisterSceneExtension(scene, a));
    ASSERT_TRUE(context.RegisterSceneExtension(scene, a));
    ASSERT_TRUE(context.RegisterSceneExtension(scene, b));
    ASSERT_TRUE(context.FreezeRegisteredScenes());
    const auto first = context.PrepareScene(scene), second = context.PrepareScene(scene);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.Get(), second.Get());
    ASSERT_EQ(first->Extensions.size(), 2u);
    CheckRows(*first, a);
    CheckRows(*first, b);
    EXPECT_EQ(first->GetExtension<DerivedRows>(a), second->GetExtension<DerivedRows>(a));
    EXPECT_NE(first->GetExtension<DerivedRows>(a), first->GetExtension<DerivedRows>(b));
    EXPECT_EQ(first->GetExtension<DerivedRows>(a)->Builds, 1u);
    EXPECT_FALSE(first->Extensions.front().Get<uint64_t>());
    EXPECT_FALSE(context.RegisterSceneExtension(scene, a));
    auto conflict = SceneRenderExtension::Make<DerivedRows, OtherPrepareRows>(101, 1, 2, {SceneDataTable::Primitives});
    EXPECT_FALSE(scene.GetRenderState().SetActiveExtensions(2, std::span{&conflict, 1}));
}

TEST(SceneExtensions, ThreeFlightDeltasRetryAtomicallyAndRetainRemovedVersions) {
    Scene scene;
    vector<PrimitiveSceneProxy*> proxies;
    for (uint32_t index = 0; index < 80; ++index) {
        auto proxy = make_unique<ExtensionProxy>();
        MoveX(*proxy, static_cast<float>(index));
        proxies.push_back(proxy.get());
        scene.AddPrimitive(std::move(proxy));
    }
    auto contract = RowsContract();
    ASSERT_TRUE(scene.GetRenderState().SetActiveExtensions(0, std::span{&contract, 1}));
    array<RenderSceneSnapshot, 3> snapshots;
    vector<StreamingAssetRefAny> owners;
    RenderSceneSnapshotBuilder publisher;
    for (uint64_t index = 0; index < snapshots.size(); ++index) {
        ASSERT_TRUE(publisher.Build(scene, snapshots[index], owners, RenderValidationMode::Off, index));
        CheckRows(snapshots[index], contract);
    }
    auto retained = snapshots[0].GetExtension<DerivedRows>(contract);
    const auto original = retained->Rows;
    for (uint64_t epoch = 3; epoch < 15; ++epoch) {
        MoveX(*proxies[17], static_cast<float>(epoch));
        ASSERT_TRUE(publisher.Build(scene, snapshots[0], owners, RenderValidationMode::Off, epoch));
        CheckRows(snapshots[0], contract);
    }
    const auto oldRevision = snapshots[1].PublicationRevision;
    const auto previous = snapshots[1].GetExtension<DerivedRows>(contract);
    scene.RemovePrimitive(proxies[2]);
    scene.AddPrimitive(make_unique<ExtensionProxy>());
    scene.RemovePrimitive(proxies[3]);
    scene.GetRenderState().FailNextPublicationForTesting({.AfterPreparedExtensions = 1});
    EXPECT_FALSE(publisher.Build(scene, snapshots[1], owners, RenderValidationMode::Off, 15));
    EXPECT_FALSE(snapshots[1].Valid);
    EXPECT_FALSE(snapshots[1].Changes.IsValid());
    EXPECT_EQ(snapshots[1].PublicationRevision, oldRevision);
    EXPECT_EQ(snapshots[1].Extensions[0].Get<DerivedRows>(), previous);
    ASSERT_TRUE(publisher.Build(scene, snapshots[1], owners, RenderValidationMode::Off, 15));
    CheckRows(snapshots[1], contract);
    EXPECT_EQ(snapshots[1].GetExtension<DerivedRows>(contract)->Builds, previous->Builds + 1);
    EXPECT_EQ(snapshots[1].Changes.FromRevision, oldRevision);
    ASSERT_TRUE(publisher.Build(scene, snapshots[2], owners, RenderValidationMode::Off, 16));
    CheckRows(snapshots[2], contract);
    EXPECT_EQ(snapshots[2].GetExtension<DerivedRows>(contract)->Rows, snapshots[1].GetExtension<DerivedRows>(contract)->Rows);
    EXPECT_EQ(retained->Rows, original);
    for (uint64_t epoch = 17; epoch < 20; ++epoch) {
        ASSERT_TRUE(scene.GetRenderState().SetActiveExtensions(epoch, {}));
        ASSERT_TRUE(publisher.Build(scene, snapshots[epoch % 3], owners, RenderValidationMode::Off, epoch));
        EXPECT_TRUE(snapshots[epoch % 3].Extensions.empty());
    }
    EXPECT_EQ(retained->Rows, original);
}

TEST(SceneExtensions, DependencyModesSkipStableWorkAndFailedProvidersCanRecover) {
    Scene scene;
    scene.AddPrimitive(make_unique<ExtensionProxy>());
    auto changed = RowsContract(2), epoch = RowsContract(3, true);
    const array contracts{changed, epoch};
    ASSERT_TRUE(scene.GetRenderState().SetActiveExtensions(1, contracts));
    RenderSceneSnapshot snapshot;
    vector<StreamingAssetRefAny> owners;
    RenderSceneSnapshotBuilder publisher;
    ASSERT_TRUE(publisher.Build(scene, snapshot, owners, RenderValidationMode::Off, 1));
    auto stable = snapshot.GetExtension<DerivedRows>(changed);
    for (uint64_t serial = 2; serial < 10; ++serial) {
        ASSERT_TRUE(publisher.Build(scene, snapshot, owners, RenderValidationMode::Off, serial));
        EXPECT_EQ(snapshot.GetExtension<DerivedRows>(changed), stable);
        EXPECT_EQ(snapshot.GetExtension<DerivedRows>(epoch)->Builds, serial);
    }
    auto bad = RowsContract(0);
    ASSERT_TRUE(scene.GetRenderState().SetActiveExtensions(10, std::span{&bad, 1}));
    EXPECT_FALSE(publisher.Build(scene, snapshot, owners, RenderValidationMode::Off, 10));
    EXPECT_FALSE(snapshot.Changes.IsValid());
    EXPECT_EQ(snapshot.Extensions[0].Get<DerivedRows>(), stable);
    ASSERT_TRUE(scene.GetRenderState().SetActiveExtensions(11, std::span{&changed, 1}));
    ASSERT_TRUE(publisher.Build(scene, snapshot, owners, RenderValidationMode::Off, 11));
    CheckRows(snapshot, changed);
    EXPECT_EQ(snapshot.GetExtension<DerivedRows>(changed), stable);
}
}  // namespace
}  // namespace radray
