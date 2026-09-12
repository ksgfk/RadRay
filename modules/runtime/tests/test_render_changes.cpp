#include "gpu_test_fixture.h"
#include "upload_test_support.h"
#if defined(RADRAY_ENABLE_SHADER_JIT)
#include "stage_b_test_support.h"
#endif

#include <array>
#include <coroutine>
#include <cstring>
#include <utility>

#include <gtest/gtest.h>
#include <radray/runtime/application.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/primitive_history.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/wait_frame.h>

namespace radray {
namespace {

class TrackedProxy final : public PrimitiveSceneProxy {
public:
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    void Dirty(PrimitiveDirtyFlags flags) { MarkRenderDirty(flags); }
};

class PublishedProxy final : public PrimitiveSceneProxy {
public:
    Nullable<Material*> DrawMaterial{nullptr};
    GpuMesh::DrawData Geometry;
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
    uint32_t GetSectionCount() const noexcept override { return DrawMaterial ? 1 : 0; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {&Geometry, 0, 3, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
};

bool CompileScenePolicy(const StaticPassCompileInput& input, StaticPassCompileResult& result) {
    result.Bindings = input.Bindings ? *input.Bindings : StaticBindingRecipe{};
    result.Bindings.Valid = true;
    result.NormalState = result.MirroredState = input.Pass.PipelineState;
    result.NormalState.DepthStencil.DepthWriteEnable = result.MirroredState.DepthStencil.DepthWriteEnable = true;
    return true;
}

bool CompileScenePolicyReplacement(const StaticPassCompileInput& input, StaticPassCompileResult& result) {
    CompileScenePolicy(input, result);
    result.NormalState.DepthStencil.DepthWriteEnable = result.MirroredState.DepthStencil.DepthWriteEnable = false;
    return true;
}

TEST(SceneSnapshotPublication, PolicyRegistrationRejectsConflictingAndLateRules) {
    Scene scene;
    AppUpdateContext app;
    RenderFramePlan plan;
    RenderWorkloadBuilder workloads{plan, {}};
    vector<StreamingAssetRefAny> owners;
    const PassPolicy first{{100}, 1, "ForwardLit", CompileScenePolicy};
    const PassPolicy conflicting{{100}, 1, "ForwardLit", CompileScenePolicyReplacement};
    RenderPrepareContext invalid{app, {}, workloads, owners, kPerformanceRenderGraphRuntimeOptions, 0};
    ASSERT_TRUE(invalid.RegisterScenePolicy(scene, first));
    EXPECT_FALSE(invalid.RegisterScenePolicy(scene, conflicting));
    EXPECT_FALSE(invalid.FreezeRegisteredScenes());
    RenderPrepareContext prepare{app, {}, workloads, owners, kPerformanceRenderGraphRuntimeOptions, 1};
    ASSERT_TRUE(prepare.RegisterScenePolicy(scene, first));
    ASSERT_TRUE(prepare.FreezeRegisteredScenes());
    EXPECT_FALSE(prepare.RegisterScenePolicy(scene, conflicting));
    EXPECT_TRUE(prepare.PrepareScene(scene));
    EXPECT_FALSE(scene.GetDrawStore().SetActivePolicies(1, std::span{&conflicting, 1}));
}

TEST(SceneSnapshotPublication, DelayedFlightsReceiveTheLatestPagesWithoutIntermediateReplay) {
    Scene scene;
    auto proxy = make_unique<PublishedProxy>();
    auto* raw = proxy.get();
    scene.AddPrimitive(std::move(proxy));
    array<RenderSceneSnapshot, 3> flights;
    vector<StreamingAssetRefAny> owners;
    RenderSceneSnapshotBuilder publisher;
    for (uint64_t flight = 0; flight < flights.size(); ++flight)
        ASSERT_TRUE(publisher.Build(scene, flights[flight], owners, RenderValidationMode::Off, flight));
    const auto original = flights[1].Primitives[0].LocalToWorld;
    for (uint64_t epoch = 3; epoch < 13; ++epoch) {
        Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
        transform(0, 3) = static_cast<float>(epoch);
        raw->SetLocalToWorld(transform);
        ASSERT_TRUE(publisher.Build(scene, flights[0], owners, RenderValidationMode::Off, epoch));
        EXPECT_EQ(flights[0].Stats.PrimitiveBoundsRebuilt, 1u);
        EXPECT_TRUE(flights[1].Primitives[0].LocalToWorld.isApprox(original));
    }
    ASSERT_TRUE(publisher.Build(scene, flights[2], owners, RenderValidationMode::Off, 13));
    EXPECT_FLOAT_EQ(flights[2].Primitives[0].LocalToWorld(0, 3), 12);
    EXPECT_EQ(flights[2].Stats.PrimitiveBoundsRebuilt, 0u);
    EXPECT_EQ(flights[2].ChangedPrimitiveRanges.size(), 1u);
    ASSERT_TRUE(publisher.Build(scene, flights[1], owners, RenderValidationMode::Off, 14));
    EXPECT_FLOAT_EQ(flights[1].Primitives[0].LocalToWorld(0, 3), 12);
    EXPECT_EQ(flights[1].ChangedPrimitiveRanges.size(), 1u);
    for (uint64_t epoch = 15; epoch < 1015; ++epoch) {
        auto& current = flights[epoch % flights.size()];
        ASSERT_TRUE(publisher.Build(scene, current, owners, RenderValidationMode::Off, epoch));
        EXPECT_EQ(current.Stats.PublishedPages, 0u);
        EXPECT_EQ(current.Stats.PublishedBytes, 0u);
        EXPECT_EQ(current.Stats.PrimitiveBoundsRebuilt, 0u);
        EXPECT_EQ(current.Stats.DrawRecordPrimitivesVisited, 0u);
        EXPECT_TRUE(current.ChangedPrimitiveRanges.empty());
    }
}

TEST(SceneSnapshotPublication, SharedConsumersUseOnePublicationAndIndependentScenesKeepTheirEpochs) {
    Scene first, second;
    auto proxy = make_unique<PublishedProxy>();
    auto* raw = proxy.get();
    first.AddPrimitive(std::move(proxy));
    second.AddPrimitive(make_unique<PublishedProxy>());
    vector<StreamingAssetRefAny> owners;
    auto a = first.GetRenderState().PrepareShared(first, 100, 0, owners, RenderValidationMode::Off);
    ASSERT_TRUE(a);
    auto b = first.GetRenderState().PrepareShared(first, 100, 0, owners, RenderValidationMode::Off);
    ASSERT_TRUE(b);
    EXPECT_EQ(a.Get(), b.Get());
    const auto firstPublication = a->PublicationId;
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 3) = 20;
    raw->SetLocalToWorld(transform);
    auto sameCutoff = first.GetRenderState().PrepareShared(first, 100, 0, owners, RenderValidationMode::Off);
    EXPECT_EQ(sameCutoff.Get(), a.Get());
    EXPECT_TRUE(a->Primitives[0].LocalToWorld.isIdentity());
    auto independent = second.GetRenderState().PrepareShared(second, 100, 0, owners, RenderValidationMode::Off);
    ASSERT_TRUE(independent);
    EXPECT_NE(independent.Get(), a.Get());
    auto next = first.GetRenderState().PrepareShared(first, 101, 1, owners, RenderValidationMode::Off);
    ASSERT_TRUE(next);
    EXPECT_NE(next->PublicationId, firstPublication);
    EXPECT_FLOAT_EQ(next->Primitives[0].LocalToWorld(0, 3), 20);
    EXPECT_TRUE(a->Primitives[0].LocalToWorld.isIdentity());
    EXPECT_TRUE(independent->Primitives[0].LocalToWorld.isIdentity());
}

TEST(SceneSnapshotPublication, RemovalSlotReuseAndCopiedSnapshotsKeepIndependentStorage) {
    Scene scene;
    auto* first = scene.AddPrimitive(make_unique<PublishedProxy>()).Get();
    auto* second = scene.AddPrimitive(make_unique<PublishedProxy>()).Get();
    vector<StreamingAssetRefAny> owners;
    RenderSceneSnapshotBuilder publisher;
    RenderSceneSnapshot oldFlight, nextFlight;
    ASSERT_TRUE(publisher.Build(scene, oldFlight, owners, RenderValidationMode::Off, 0));
    const auto removed = oldFlight.Primitives[0].Id;
    nextFlight = oldFlight;
    EXPECT_EQ(nextFlight.PublicationId, 0u);
    scene.RemovePrimitive(first);
    auto* replacement = scene.AddPrimitive(make_unique<PublishedProxy>()).Get();
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(1, 3) = 8;
    replacement->SetLocalToWorld(transform);
    ASSERT_TRUE(publisher.Build(scene, nextFlight, owners, RenderValidationMode::Off, 1));
    ASSERT_EQ(nextFlight.Primitives.size(), 2u);
    EXPECT_EQ(nextFlight.Primitives[0].Id, scene.GetPrimitiveId(second));
    EXPECT_EQ(nextFlight.Primitives[1].Id, scene.GetPrimitiveId(replacement));
    EXPECT_NE(nextFlight.PublicationId, oldFlight.PublicationId);
    EXPECT_FLOAT_EQ(nextFlight.Primitives[1].LocalToWorld(1, 3), 8);
    EXPECT_EQ(oldFlight.Primitives[0].Id, removed);
    EXPECT_TRUE(oldFlight.Primitives[0].LocalToWorld.isIdentity());
    EXPECT_FALSE(scene.FindPrimitive(removed));
}

TEST(SceneSnapshotPublication, FailedPageCopyKeepsPendingPagesAndTheLastSuccessfulRevision) {
    Scene scene;
    auto* proxy = scene.AddPrimitive(make_unique<PublishedProxy>()).Get();
    RenderSceneSnapshotBuilder publisher;
    RenderSceneSnapshot oldFlight, target;
    vector<StreamingAssetRefAny> owners;
    ASSERT_TRUE(publisher.Build(scene, oldFlight, owners, RenderValidationMode::Off, 0));
    ASSERT_TRUE(publisher.Build(scene, target, owners, RenderValidationMode::Off, 1));
    const auto priorRevision = target.PublicationRevision;
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 3) = 6;
    proxy->SetLocalToWorld(transform);
    scene.GetRenderState().FailNextPublicationForTesting({.AfterCopiedTables = 1});
    EXPECT_FALSE(publisher.Build(scene, target, owners, RenderValidationMode::Off, 2));
    EXPECT_FALSE(target.Valid);
    EXPECT_EQ(target.PublicationRevision, priorRevision);
    EXPECT_TRUE(oldFlight.Primitives[0].LocalToWorld.isIdentity());
    EXPECT_TRUE(owners.empty());
    ASSERT_TRUE(publisher.Build(scene, target, owners, RenderValidationMode::Off, 2));
    EXPECT_TRUE(target.Valid);
    EXPECT_GT(target.Stats.PublishedPages, 0u);
    EXPECT_EQ(target.Stats.SceneCommits, 0u);
    EXPECT_EQ(target.Stats.DrawRecordPrimitivesVisited, 0u);
    EXPECT_EQ(target.ChangedFromPublicationRevision, priorRevision);
    EXPECT_EQ(target.PublicationRevision, priorRevision + 1);
    EXPECT_FLOAT_EQ(target.Primitives[0].LocalToWorld(0, 3), 6);
    ASSERT_TRUE(publisher.Build(scene, target, owners, RenderValidationMode::Off, 3));
    EXPECT_EQ(target.Stats.PublishedPages, 0u);
}

TEST(SceneSnapshotPublication, FailedPublicationRejectsLateChangesUntilTheNextEpoch) {
    Scene scene;
    auto* proxy = scene.AddPrimitive(make_unique<PublishedProxy>()).Get();
    RenderSceneSnapshot target;
    vector<StreamingAssetRefAny> owners;
    scene.GetRenderState().FailNextPublicationForTesting({.AfterCopiedTables = 1});
    EXPECT_FALSE(scene.GetRenderState().Publish(scene, target, owners, RenderValidationMode::Off, 0));
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 3) = 6;
    proxy->SetLocalToWorld(transform);
    EXPECT_FALSE(scene.GetRenderState().Publish(scene, target, owners, RenderValidationMode::Off, 0));
    EXPECT_FALSE(target.Valid);
    EXPECT_EQ(target.PublicationRevision, 0u);
    ASSERT_TRUE(scene.GetRenderState().Publish(scene, target, owners, RenderValidationMode::Off, 1));
    EXPECT_FLOAT_EQ(target.Primitives[0].LocalToWorld(0, 3), 6);
    EXPECT_EQ(target.PublicationRevision, 1u);
}

TEST(SceneRenderChanges, RepeatedEditsCoalesceAndLateEditsUseTheNextEpoch) {
    Scene scene;
    vector<TrackedProxy*> proxies;
    for (uint32_t index = 0; index < 8; ++index) {
        auto proxy = make_unique<TrackedProxy>();
        proxies.push_back(proxy.get());
        scene.AddPrimitive(std::move(proxy));
    }
    EXPECT_EQ(scene.BeginRenderCommit(0).size(), proxies.size());
    ASSERT_TRUE(scene.CompleteRenderCommit(0, true));
    for (uint32_t edit = 0; edit < 10000; ++edit) {
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
        matrix(0, 3) = static_cast<float>(edit);
        for (auto* proxy : proxies) {
            proxy->SetLocalToWorld(matrix);
            proxy->Dirty(PrimitiveDirtyKind::MaterialAssignment);
        }
    }
    EXPECT_EQ(scene.GetPendingRenderChangeCount(), proxies.size());
    const auto changes = scene.BeginRenderCommit(1);
    ASSERT_EQ(changes.size(), proxies.size());
    for (const auto& change : changes) {
        EXPECT_TRUE(change.Dirty.HasFlag(PrimitiveDirtyKind::TransformOrBounds));
        EXPECT_TRUE(change.Dirty.HasFlag(PrimitiveDirtyKind::MaterialAssignment));
        EXPECT_FALSE(change.Dirty.HasFlag(PrimitiveDirtyKind::Structure));
        EXPECT_FLOAT_EQ(scene.FindPrimitive(change.Id)->GetLocalToWorld()(0, 3), 9999);
    }
    ASSERT_TRUE(scene.CompleteRenderCommit(1, true));
    proxies[0]->ResetMotion();
    const auto sameEpoch = scene.BeginRenderCommit(1);
    EXPECT_EQ(sameEpoch.data(), changes.data());
    EXPECT_FALSE(sameEpoch[0].Dirty.HasFlag(PrimitiveDirtyKind::MotionReset));
    EXPECT_EQ(scene.GetPendingRenderChangeCount(), 1u);
    const auto nextEpoch = scene.BeginRenderCommit(2);
    ASSERT_EQ(nextEpoch.size(), 1u);
    EXPECT_EQ(nextEpoch[0].Dirty, PrimitiveDirtyKind::MotionReset);
    ASSERT_TRUE(scene.CompleteRenderCommit(2, true));
    EXPECT_TRUE(scene.BeginRenderCommit(3).empty());
    EXPECT_EQ(scene.GetCommitStats().LegacyProxiesObserved, 0u);
    EXPECT_EQ(scene.GetCommitStats().PendingResourcesObserved, 0u);
}

TEST(SceneRenderChanges, SlotReuseKeepsOneOriginalTombstoneAndTheFinalIdentity) {
    Scene scene;
    auto* first = scene.AddPrimitive(make_unique<TrackedProxy>()).Get();
    const auto original = scene.GetPrimitiveId(first);
    scene.BeginRenderCommit(0);
    ASSERT_TRUE(scene.CompleteRenderCommit(0, true));
    scene.RemovePrimitive(first);
    PrimitiveSceneProxy* latest = nullptr;
    for (uint32_t index = 0; index < 10000; ++index) {
        latest = scene.AddPrimitive(make_unique<TrackedProxy>()).Get();
        if (index != 9999) scene.RemovePrimitive(latest);
    }
    EXPECT_EQ(scene.GetPendingRenderChangeCount(), 1u);
    const auto current = scene.GetPrimitiveId(latest);
    EXPECT_EQ(current.Slot, original.Slot);
    EXPECT_NE(current.Generation, original.Generation);
    EXPECT_FALSE(scene.FindPrimitive(original));
    const auto changes = scene.BeginRenderCommit(1);
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].Id, original);
    EXPECT_EQ(changes[0].Dirty, PrimitiveDirtyKind::Removed);
    EXPECT_EQ(changes[1].Id, current);
    ASSERT_TRUE(scene.CompleteRenderCommit(1, false));
    latest->ResetMotion();
    const auto retry = scene.BeginRenderCommit(2);
    ASSERT_EQ(retry.size(), 2u);
    EXPECT_EQ(retry[0].Id, original);
    EXPECT_EQ(retry[1].Id, current);
    ASSERT_TRUE(scene.CompleteRenderCommit(2, true));
    EXPECT_TRUE(scene.BeginRenderCommit(3).empty());
}

TEST(SceneRenderChanges, UnknownProxiesRefreshWithoutMakingControlledObjectsDirty) {
    Scene scene;
    auto* unknown = scene.AddPrimitive(make_unique<PrimitiveSceneProxy>()).Get();
    scene.AddPrimitive(make_unique<TrackedProxy>());
    scene.BeginRenderCommit(0);
    scene.CompleteRenderCommit(0, true);
    for (uint64_t epoch = 1; epoch < 10; ++epoch) {
        const auto changes = scene.BeginRenderCommit(epoch);
        ASSERT_EQ(changes.size(), 1u);
        EXPECT_EQ(changes[0].Id, scene.GetPrimitiveId(unknown));
        EXPECT_TRUE(changes[0].Dirty.HasFlag(PrimitiveDirtyKind::Structure));
        EXPECT_TRUE(changes[0].Dirty.HasFlag(PrimitiveDirtyKind::TransformOrBounds));
        EXPECT_EQ(scene.GetCommitStats().LegacyProxiesObserved, 1u);
        scene.CompleteRenderCommit(epoch, true);
    }
}

class ImmediateWait final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};

class ManualGate {
public:
    ~ManualGate() { Resume(); }
    void Resume() {
        const auto pending = _pending;
        _pending = {};
        if (pending) pending.resume();
    }
    struct Awaiter {
        ManualGate* Gate;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> pending) const noexcept { Gate->_pending = pending; }
        void await_resume() const noexcept {}
    };
    Awaiter Wait() noexcept { return {this}; }

private:
    std::coroutine_handle<> _pending{};
};

TEST(SceneSnapshotPublication, EpochTargetsKeepTheirOriginalOwnersAcrossLateMeshReplacement) {
    ImmediateWait wait;
    AssetManager assets;
    assets.SetWaitFrameProcessor(&wait);
    const auto makeMesh = [](float extent) {
        GpuMesh gpu;
        gpu.Draws.emplace_back();
        return make_unique<StaticMesh>(test::MakeUploadTestMesh(), vector<StaticMeshSection>{{0, 0, 3, 0, 2}},
                                       Eigen::Vector3f::Zero(), Eigen::Vector3f::Constant(extent), std::move(gpu));
    };
    auto original = assets.AddReady<StaticMesh>(test::kUploadTestId, makeMesh(1));
    const AssetId replacementId{0xaabbccef, 0x1122, 0x3344, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};
    auto replacement = assets.AddReady<StaticMesh>(replacementId, makeMesh(4));
    Scene scene;
    auto value = make_unique<StaticMeshSceneProxy>(original, vector<Nullable<Material*>>{}, Eigen::Matrix4f::Identity());
    auto* proxy = value.get();
    scene.AddPrimitive(std::move(value));
    vector<StreamingAssetRefAny> originalOwners, nextOwners;
    const auto first = scene.GetRenderState().PrepareShared(scene, 0, 0, originalOwners, RenderValidationMode::Off);
    ASSERT_TRUE(first);
    ASSERT_EQ(originalOwners.size(), 1u);
    proxy->SetStaticMesh(replacement);
    original.Reset();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 2u);
    RenderSceneSnapshot forbidden;
    EXPECT_FALSE(scene.GetRenderState().Publish(scene, forbidden, nextOwners, RenderValidationMode::Off, 0));
    EXPECT_FALSE(forbidden.Valid);
    EXPECT_TRUE(nextOwners.empty());
    EXPECT_FALSE(scene.GetRenderState().PrepareShared(scene, 0, 1, nextOwners, RenderValidationMode::Off));
    EXPECT_FALSE(scene.GetRenderState().PrepareShared(scene, 0, 0, nextOwners, RenderValidationMode::Off));
    const auto same = scene.GetRenderState().PrepareShared(scene, 0, 0, originalOwners, RenderValidationMode::Off);
    ASSERT_TRUE(same);
    EXPECT_EQ(same.Get(), first.Get());
    EXPECT_TRUE(same->Primitives[0].WorldBounds.Max.isApprox(Eigen::Vector3f::Ones()));
    const auto next = scene.GetRenderState().PrepareShared(scene, 1, 1, nextOwners, RenderValidationMode::Off);
    ASSERT_TRUE(next);
    ASSERT_EQ(nextOwners.size(), 1u);
    EXPECT_TRUE(next->Primitives[0].WorldBounds.Max.isApprox(Eigen::Vector3f::Constant(4)));
    scene.RemovePrimitive(proxy);
    replacement.Reset();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 2u);
    originalOwners.clear();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 1u);
    nextOwners.clear();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 0u);
}

TEST(SceneRenderChanges, LoadingResourcesBecomeVisibleWithoutSetterAndLeaveObservationSet) {
    ImmediateWait wait;
    AssetManager assets;
    assets.SetWaitFrameProcessor(&wait);
    ManualGate gate;
    auto mesh = assets.Load<StaticMesh>({test::kUploadTestId,
                                         [](ManualGate* pending) -> task<AssetLoadResult> {
                                             co_await pending->Wait();
                                             GpuMesh gpu;
                                             gpu.Draws.emplace_back();
                                             co_return AssetLoadResult::Success(make_unique<StaticMesh>(test::MakeUploadTestMesh(),
                                                                                                        vector<StaticMeshSection>{{0, 0, 3, 0, 2}}, Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), std::move(gpu)));
                                         }(&gate),
                                         "scene change pending mesh"});
    Scene scene;
    auto* proxy = scene.AddPrimitive(make_unique<StaticMeshSceneProxy>(mesh, vector<Nullable<Material*>>{}, Eigen::Matrix4f::Identity())).Get();
    const auto id = scene.GetPrimitiveId(proxy);
    scene.BeginRenderCommit(0);
    scene.CompleteRenderCommit(0, true);
    EXPECT_TRUE(scene.BeginRenderCommit(1).empty());
    EXPECT_EQ(scene.GetCommitStats().PendingResourcesObserved, 1u);
    scene.CompleteRenderCommit(1, true);
    gate.Resume();
    assets.Pump();
    const auto ready = scene.BeginRenderCommit(2);
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].Id, id);
    EXPECT_TRUE(ready[0].Dirty.HasFlag(PrimitiveDirtyKind::Structure));
    EXPECT_EQ(proxy->GetSectionCount(), 1u);
    scene.CompleteRenderCommit(2, true);
    EXPECT_TRUE(scene.BeginRenderCommit(3).empty());
    EXPECT_EQ(scene.GetCommitStats().PendingResourcesObserved, 0u);
    RenderSceneSnapshot target;
    vector<StreamingAssetRefAny> owners;
    scene.GetRenderState().FailNextPublicationForTesting({.AfterRetainedOwners = 1});
    EXPECT_FALSE(scene.GetRenderState().Publish(scene, target, owners, RenderValidationMode::Off, 4));
    EXPECT_FALSE(target.Valid);
    EXPECT_TRUE(owners.empty());
    ASSERT_TRUE(scene.GetRenderState().Publish(scene, target, owners, RenderValidationMode::Off, 4));
    ASSERT_EQ(owners.size(), 1u);
    ASSERT_EQ(target.Primitives.size(), 1u);
    owners.clear();
    scene.RemovePrimitive(proxy);
    mesh.Reset();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 0u);
}

class MaterialRenderChanges : public testing::TestWithParam<render::RenderBackend> {
protected:
    void SetUp() override {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
        GTEST_SKIP() << "Shader JIT disabled";
#else
        if (!render::test::TryCreateDevice(GetParam(), Device, true)) GTEST_SKIP() << "Backend unavailable";
        auto program = test::CompileStageBProgram(*Device.Device, test::StageBMaterialSource());
        ASSERT_TRUE(program);
        Program = program.Release();
        auto technique = MaterialTechnique::Create({{"ForwardLit", Program.get(), "MaterialValues", {}}}, "ForwardLit");
        ASSERT_TRUE(technique);
        Technique = technique.Release();
#endif
    }
    void TearDown() override { EXPECT_EQ(Device.ValidationErrors.load(), 0u); }
    render::test::DeviceContext Device;
    unique_ptr<ShaderProgram> Program;
    unique_ptr<MaterialTechnique> Technique;
};

TEST_P(MaterialRenderChanges, TrackedEditsAvoidPollingAndSeparateValueBindingAndStructureRevisions) {
    auto material = Material::Create(Technique.get());
    const auto initial = material->GetRevisions(0);
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    for (uint32_t edit = 0; edit < 1000; ++edit)
        ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    const auto changed = material->GetRevisions(1);
    EXPECT_GT(changed.ValuesRevision, initial.ValuesRevision);
    EXPECT_EQ(changed.StructureRevision, initial.StructureRevision);
    EXPECT_EQ(changed.BindingsRevision, initial.BindingsRevision);
    EXPECT_EQ(material->GetObservationStats().TrackedChanges, 1u);
    for (uint64_t epoch = 2; epoch < 1002; ++epoch)
        EXPECT_EQ(material->GetRevisions(epoch).Revision, changed.Revision);
    EXPECT_FALSE(material->HasEscapedWrites());
    EXPECT_EQ(material->GetObservationStats().LegacyMaterialsObserved, 0u);
    EXPECT_EQ(material->GetObservationStats().LegacyBytesCompared, 0u);
    EXPECT_EQ(material->GetObservationStats().PendingResourcesObserved, 0u);

    const array<float, 4> value{2, 3, 4, 5};
    ASSERT_TRUE(material->SetNumeric(value));
    const auto numeric = material->GetRevisions(1002);
    EXPECT_GT(numeric.ValuesRevision, changed.ValuesRevision);
    EXPECT_EQ(numeric.StructureRevision, changed.StructureRevision);
    ASSERT_TRUE(material->SetNumeric(value));
    EXPECT_EQ(material->GetRevisions(1003).Revision, numeric.Revision);
    auto state = std::as_const(*material).GetPipelineState();
    state.Primitive.Cull = render::CullMode::None;
    ASSERT_TRUE(material->SetPassPipelineState("ForwardLit", state));
    const auto structural = material->GetRevisions(1004);
    EXPECT_GT(structural.StructureRevision, numeric.StructureRevision);
    EXPECT_EQ(structural.ValuesRevision, numeric.ValuesRevision);
    MaterialRenderData published;
    vector<StreamingAssetRefAny> owners;
    ASSERT_TRUE(material->BuildRenderData(published, owners, nullptr, 1004));
    EXPECT_EQ(published.StructureRevision, structural.StructureRevision);
    EXPECT_EQ(published.Passes[0].ProgramGeneration, Program->GetGeneration());
    EXPECT_FALSE(material->HasEscapedWrites());
}

TEST_P(MaterialRenderChanges, EscapedPointersRemainObservedAcrossCleanEpochsOncePerEpoch) {
    auto material = Material::Create(Technique.get());
    auto* typed = material->As<array<float, 4>>();
    const auto bytes = material->NumericBytes();
    auto& pipeline = material->GetPipelineState();
    auto previous = material->GetRevisions(0);
    for (uint64_t epoch = 1; epoch <= 8; ++epoch) {
        EXPECT_EQ(material->GetRevisions(epoch).Revision, previous.Revision);
        material->GetRevisions(epoch);
    }
    EXPECT_EQ(material->GetObservationStats().LegacyMaterialsObserved, 9u);
    (*typed)[0] = 5;
    auto changed = material->GetRevisions(9);
    EXPECT_GT(changed.ValuesRevision, previous.ValuesRevision);
    EXPECT_EQ(changed.StructureRevision, previous.StructureRevision);
    previous = changed;
    for (uint64_t epoch = 10; epoch <= 18; ++epoch) material->GetRevisions(epoch);
    const float next = 7;
    std::memcpy(bytes.data() + sizeof(float), &next, sizeof(next));
    changed = material->GetRevisions(19);
    EXPECT_GT(changed.ValuesRevision, previous.ValuesRevision);
    previous = changed;
    for (uint64_t epoch = 20; epoch <= 28; ++epoch) material->GetRevisions(epoch);
    pipeline.Primitive.Cull = render::CullMode::None;
    changed = material->GetRevisions(29);
    EXPECT_GT(changed.StructureRevision, previous.StructureRevision);
    EXPECT_EQ(changed.ValuesRevision, previous.ValuesRevision);
    MaterialRenderData published;
    vector<StreamingAssetRefAny> owners;
    ASSERT_TRUE(material->BuildRenderData(published, owners, nullptr, 29));
    EXPECT_EQ(material->GetObservationStats().LegacyMaterialsObserved, 30u);
    EXPECT_EQ(published.Passes[0].PipelineState.Primitive.Cull, render::CullMode::None);
    EXPECT_TRUE(material->HasEscapedWrites());
}

TEST_P(MaterialRenderChanges, MaterialAssignmentsPreserveSceneAndMotionIdentity) {
    auto a = Material::Create(Technique.get());
    auto b = Material::Create(Technique.get());
    Scene scene;
    auto value = make_unique<StaticMeshSceneProxy>(StreamingAssetRef<StaticMesh>{}, vector<Nullable<Material*>>{a.Get()}, Eigen::Matrix4f::Identity());
    auto* proxy = value.get();
    scene.AddPrimitive(std::move(value));
    const auto id = scene.GetPrimitiveId(proxy);
    const auto generation = proxy->GetGeneration();
    const auto motion = proxy->GetMotionRevision();
    scene.BeginRenderCommit(0);
    scene.CompleteRenderCommit(0, true);
    RenderSceneSnapshot before;
    before.Primitives.emplace_back();
    before.Primitives[0].Generation = generation;
    before.Primitives[0].MotionRevision = motion;
    PrimitiveHistory history;
    ASSERT_TRUE(history.Prepare(before, 1));
    ASSERT_TRUE(history.Commit(1));
    for (uint32_t edit = 0; edit < 1000; ++edit) proxy->SetMaterial(0, edit % 2 ? a.Get() : b.Get());
    proxy->SetMaterial(0, b.Get());
    EXPECT_EQ(scene.GetPrimitiveId(proxy), id);
    EXPECT_EQ(proxy->GetGeneration(), generation);
    EXPECT_EQ(proxy->GetMotionRevision(), motion);
    EXPECT_TRUE(history.Lookup(before.Primitives[0]).Valid);
    EXPECT_EQ(proxy->GetMaterial(0).Get(), b.Get());
    const auto changes = scene.BeginRenderCommit(1);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].Dirty, PrimitiveDirtyKind::MaterialAssignment);
    scene.CompleteRenderCommit(1, true);
    scene.RemovePrimitive(proxy);
    auto* replacement = scene.AddPrimitive(make_unique<StaticMeshSceneProxy>(StreamingAssetRef<StaticMesh>{}, vector<Nullable<Material*>>{b.Get()}, Eigen::Matrix4f::Identity())).Get();
    EXPECT_FALSE(scene.FindPrimitive(id));
    EXPECT_NE(scene.GetPrimitiveId(replacement).Generation, id.Generation);
    before.Primitives[0].Generation = replacement->GetGeneration();
    EXPECT_FALSE(history.Lookup(before.Primitives[0]).Valid);
}

TEST_P(MaterialRenderChanges, SharedNumericChangePublishesOneMaterialWithoutVisitingTenThousandDraws) {
    auto material = Material::Create(Technique.get());
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    Scene scene;
    for (uint32_t index = 0; index < 10000; ++index) {
        auto proxy = make_unique<PublishedProxy>();
        proxy->DrawMaterial = material.Get();
        scene.AddPrimitive(std::move(proxy));
    }
    RenderSceneSnapshotBuilder publisher;
    array<RenderSceneSnapshot, 3> flights;
    vector<StreamingAssetRefAny> owners;
    for (uint64_t flight = 0; flight < flights.size(); ++flight)
        ASSERT_TRUE(publisher.Build(scene, flights[flight], owners, RenderValidationMode::Off, flight));
    ASSERT_EQ(flights[0].DrawRecords.size(), 10000u);
    const auto oldBytes = flights[1].Materials[0].Passes[0].NumericBytes;
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Constant(3)));
    ASSERT_TRUE(publisher.Build(scene, flights[0], owners, RenderValidationMode::Off, 3));
    EXPECT_EQ(flights[0].Stats.MaterialsRebuilt, 1u);
    EXPECT_EQ(flights[0].Stats.DrawRecordBuilds, 0u);
    EXPECT_EQ(flights[0].Stats.DrawRecordPrimitivesVisited, 0u);
    EXPECT_EQ(flights[0].Stats.DrawRecordCopies, 0u);
    EXPECT_EQ(flights[0].Stats.PublishedPages, 1u);
    EXPECT_TRUE(flights[0].ChangedPrimitiveRanges.empty());
    EXPECT_EQ(flights[1].Materials[0].Passes[0].NumericBytes, oldBytes);
    EXPECT_NE(flights[0].Materials[0].Passes[0].NumericBytes, oldBytes);
    ASSERT_TRUE(publisher.Build(scene, flights[2], owners, RenderValidationMode::Off, 4));
    EXPECT_EQ(flights[2].Stats.PublishedPages, 1u);
    EXPECT_EQ(flights[2].Stats.MaterialsRebuilt, 0u);
    EXPECT_EQ(flights[2].Materials[0].Passes[0].NumericBytes, flights[0].Materials[0].Passes[0].NumericBytes);
}

TEST_P(MaterialRenderChanges, RegisteredScenesFreezeSharedMaterialBeforeAnyPrepareConsumer) {
    auto material = Material::Create(Technique.get());
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    Scene first, second, lateScene;
    for (Scene* scene : {&first, &second}) {
        auto proxy = make_unique<PublishedProxy>();
        proxy->DrawMaterial = material.Get();
        scene->AddPrimitive(std::move(proxy));
    }
    RenderFramePlan plan;
    RenderWorkloadBuilder workloads{plan, {}};
    AppUpdateContext app{.FlightIndex = 0};
    array<vector<StreamingAssetRefAny>, 2> owners;
    RenderPrepareContext prepare{app, {}, workloads, owners[0], kPerformanceRenderGraphRuntimeOptions, 0};
    EXPECT_FALSE(prepare.PrepareScene(first));
    ASSERT_TRUE(prepare.RegisterScene(first));
    ASSERT_TRUE(prepare.RegisterScene(second));
    ASSERT_TRUE(prepare.RegisterScene(first));
    ASSERT_EQ(prepare.RegisteredScenes.size(), 2u);
    ASSERT_TRUE(prepare.FreezeRegisteredScenes());
    const auto a = prepare.PrepareScene(first);
    const auto b = prepare.PrepareScene(second);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_EQ(a->Materials.size(), 1u);
    ASSERT_EQ(b->Materials.size(), 1u);
    const auto oldBytes = a->Materials[0].Passes[0].NumericBytes;
    EXPECT_EQ(b->Materials[0].Passes[0].NumericBytes, oldBytes);
    const auto revision = b->PublicationRevision;
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Constant(8)));
    material->SetRenderQueue(RenderQueue::Transparent);
    ASSERT_TRUE(prepare.FreezeRegisteredScenes());
    EXPECT_FALSE(prepare.RegisterScene(lateScene));
    EXPECT_FALSE(prepare.PrepareScene(lateScene));
    const auto laterConsumer = prepare.PrepareScene(second);
    ASSERT_TRUE(laterConsumer);
    EXPECT_EQ(laterConsumer.Get(), b.Get());
    EXPECT_EQ(laterConsumer->PublicationRevision, revision);
    EXPECT_EQ(laterConsumer->Materials[0].Passes[0].NumericBytes, oldBytes);
    EXPECT_NE(laterConsumer->Materials[0].Queue, RenderQueue::Transparent);
    AppUpdateContext nextApp{.FlightIndex = 1};
    RenderPrepareContext nextPrepare{nextApp, {}, workloads, owners[1], kPerformanceRenderGraphRuntimeOptions, 1};
    ASSERT_TRUE(nextPrepare.RegisterScene(second));
    ASSERT_TRUE(nextPrepare.RegisterScene(first));
    ASSERT_TRUE(nextPrepare.FreezeRegisteredScenes());
    const auto nextA = nextPrepare.PrepareScene(first);
    const auto nextB = nextPrepare.PrepareScene(second);
    ASSERT_TRUE(nextA);
    ASSERT_TRUE(nextB);
    EXPECT_NE(nextA->Materials[0].Passes[0].NumericBytes, oldBytes);
    EXPECT_EQ(nextA->Materials[0].Passes[0].NumericBytes, nextB->Materials[0].Passes[0].NumericBytes);
    EXPECT_EQ(nextA->Materials[0].Queue, RenderQueue::Transparent);
    EXPECT_EQ(nextB->Materials[0].Queue, RenderQueue::Transparent);
    EXPECT_EQ(a->Materials[0].Passes[0].NumericBytes, oldBytes);
    EXPECT_EQ(b->Materials[0].Passes[0].NumericBytes, oldBytes);
}

TEST_P(MaterialRenderChanges, PolicyOnlyChangesPublishDrawAndBindingPagesWithoutPrimitiveOrValueBroadcast) {
    auto material = Material::Create(Technique.get());
    Scene scene;
    auto proxy = make_unique<PublishedProxy>();
    proxy->DrawMaterial = material.Get();
    scene.AddPrimitive(std::move(proxy));
    RenderFramePlan plan;
    RenderWorkloadBuilder workloads{plan, {}};
    array<vector<StreamingAssetRefAny>, 2> owners;
    array<shared_ptr<const RenderSceneSnapshot>, 2> flights;
    PassPolicy policy{{100}, 1, "ForwardLit", CompileScenePolicy};
    for (uint64_t serial = 0; serial < 2; ++serial) {
        AppUpdateContext app{.FlightIndex = static_cast<uint32_t>(serial)};
        RenderPrepareContext prepare{app, {}, workloads, owners[serial], kPerformanceRenderGraphRuntimeOptions, serial};
        ASSERT_TRUE(prepare.RegisterScenePolicy(scene, policy));
        ASSERT_TRUE(prepare.RegisterScenePolicy(scene, policy));
        ASSERT_EQ(prepare.ScenePolicies.size(), 1u);
        ASSERT_TRUE(prepare.FreezeRegisteredScenes());
        auto snapshot = prepare.PrepareScene(scene);
        ASSERT_TRUE(snapshot);
        flights[serial] = snapshot.Release();
        ASSERT_EQ(flights[serial]->DrawRecords.size(), 1u);
    }
    const auto oldId = flights[1]->DrawRecords[0].Id;
    policy.Revision = 2;
    policy.CompileStatic = CompileScenePolicyReplacement;
    for (uint64_t serial = 2; serial < 4; ++serial) {
        const auto flight = static_cast<uint32_t>(serial % 2);
        AppUpdateContext app{.FlightIndex = flight};
        owners[flight].clear();
        RenderPrepareContext prepare{app, {}, workloads, owners[flight], kPerformanceRenderGraphRuntimeOptions, serial};
        ASSERT_TRUE(prepare.RegisterScenePolicy(scene, policy));
        ASSERT_TRUE(prepare.FreezeRegisteredScenes());
        auto snapshot = prepare.PrepareScene(scene);
        ASSERT_TRUE(snapshot);
        flights[flight] = snapshot.Release();
        EXPECT_EQ(flights[flight]->DrawRecords[0].Id, oldId);
        EXPECT_FALSE(flights[flight]->DrawRecords[0].Description.PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_TRUE(flights[flight]->ChangedPrimitiveRanges.empty());
        EXPECT_EQ(flights[flight]->Stats.MaterialBytesCopied, 0u);
        EXPECT_EQ(flights[flight]->Stats.StaticRecipeCompiles, serial == 2 ? 1u : 0u);
        EXPECT_EQ(flights[flight]->Stats.BindingRecipeCompiles, serial == 2 ? 1u : 0u);
        EXPECT_GT(flights[flight]->Stats.PublishedPages, 0u);
        ASSERT_LT(flights[flight]->DrawRecords[0].BindingRecipe, flights[flight]->BindingRecipes.size());
        EXPECT_TRUE(flights[flight]->BindingRecipes[flights[flight]->DrawRecords[0].BindingRecipe].Valid);
        if (serial == 2) EXPECT_TRUE(flights[1]->DrawRecords[0].Description.PipelineState.DepthStencil.DepthWriteEnable);
    }
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Constant(9)));
    AppUpdateContext app{};
    RenderPrepareContext numeric{app, {}, workloads, owners[0], kPerformanceRenderGraphRuntimeOptions, 4};
    ASSERT_TRUE(numeric.RegisterScenePolicy(scene, policy));
    ASSERT_TRUE(numeric.FreezeRegisteredScenes());
    const auto values = numeric.PrepareScene(scene);
    ASSERT_TRUE(values);
    EXPECT_EQ(values->Stats.StaticRecipeCompiles, 0u);
    EXPECT_EQ(values->Stats.DrawRecordPrimitivesVisited, 0u);
    EXPECT_EQ(values->Stats.PublishedPages, 1u);
    EXPECT_TRUE(values->ChangedPrimitiveRanges.empty());
}

INSTANTIATE_TEST_SUITE_P(Backends, MaterialRenderChanges, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
