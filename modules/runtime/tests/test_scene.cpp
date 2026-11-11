#include <gtest/gtest.h>

#include <limits>
#include <type_traits>
#include <utility>

#include <radray/runtime/frame_slot.h>
#include <radray/runtime/scene.h>

#include "test_fakes.h"

namespace radray {
namespace {

using test::FakeBuffer;
using test::FakePipeline;

// Scene 查询仅接受对应类型的句柄，拒绝其他元素类型与底层原始句柄。
static_assert(std::is_invocable_v<decltype(&Scene::GetPrimitive), const Scene&, PrimitiveHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetPrimitive), const Scene&, MeshBatchHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetPrimitive), const Scene&, MaterialHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetPrimitive), const Scene&, SparseSetHandle>);
static_assert(std::is_invocable_v<decltype(&Scene::GetMeshBatch), const Scene&, MeshBatchHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetMeshBatch), const Scene&, PrimitiveHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetMeshBatch), const Scene&, MaterialHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetMeshBatch), const Scene&, SparseSetHandle>);
static_assert(std::is_invocable_v<decltype(&Scene::GetMaterial), const Scene&, MaterialHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetMaterial), const Scene&, PrimitiveHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetMaterial), const Scene&, MeshBatchHandle> &&
              !std::is_invocable_v<decltype(&Scene::GetMaterial), const Scene&, SparseSetHandle>);
static_assert(!std::is_constructible_v<PrimitiveHandle, SparseSetHandle> &&
              !std::is_constructible_v<MeshBatchHandle, SparseSetHandle> &&
              !std::is_constructible_v<MaterialHandle, SparseSetHandle>);

class RecordingProcessor final : public MeshPassProcessor {
public:
    explicit RecordingProcessor(render::GraphicsPipelineState& pipeline) noexcept
        : _pipeline(&pipeline) {}

    void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) override {
        Calls.push_back(context.BatchHandle);
        LastMaterial = context.Material;
        BuildMeshDrawCommands(context, {.Pipeline = _pipeline, .CustomStateSortKey = SortKey}, output);
    }

    vector<MeshBatchHandle> Calls;
    Nullable<const MaterialRenderProxy*> LastMaterial{nullptr};
    uint32_t SortKey{0};

private:
    render::GraphicsPipelineState* _pipeline;
};

class SceneTest : public testing::Test {
protected:
    SceneTest() {
        Input.Layouts.push_back({0, 12, render::VertexStepMode::Vertex});
        Input.Attributes.push_back({0, 0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0});
        Input.Streams.push_back({&Vertices, 0, 48, 0});
        Material = World.AddMaterial({});
        Primitive = World.AddPrimitive();
    }
    MeshBatch MakeBatch(uint32_t elementCount = 1) {
        MeshBatch batch;
        batch.Material = Material;
        batch.VertexInput = &Input;
        for (uint32_t index = 0; index < elementCount; ++index) {
            batch.Elements.push_back({.IndexBuffer = &Indices, .FirstIndex = index * 3, .IndexCount = 3});
        }
        return batch;
    }
    MeshBatchHandle AddBatch(uint32_t elementCount = 1) { return World.AddMeshBatch(Primitive, MakeBatch(elementCount)); }
    void Sync() {
        const MeshPassRegistration pass{Pass, &Processor};
        World.SyncCachedMeshDrawCommands(std::span{&pass, 1});
    }
    uint32_t CommandIndex(MeshBatchHandle batch) const { return World.GetPassCache(Pass)->GetBatchSlot(batch.GetIndex())->CommandDenseIndex; }
    const MeshDrawCommand& CachedCommand(MeshBatchHandle batch) const { return *World.GetPassCache(Pass)->GetCommand(batch.GetIndex()); }
    bool Gather(std::span<const VisibleMeshBatch> requests, vector<VisibleMeshDrawCommand>& output) const {
        return World.GatherVisibleMeshDrawCommands(Pass, requests, output);
    }

    static constexpr MeshPassId Pass{0};
    FakePipeline Pipeline;
    FakeBuffer Vertices{128, render::BufferUse::Vertex};
    FakeBuffer Indices{128, render::BufferUse::Index};
    VertexInput Input;
    RecordingProcessor Processor{Pipeline};
    Scene World;
    MaterialHandle Material;
    PrimitiveHandle Primitive;
};

TEST_F(SceneTest, AddMeshBatchLinksBatchToPrimitive) {
    const auto batch = AddBatch();
    ASSERT_TRUE(batch.IsValid());
    EXPECT_EQ((*World.GetPrimitive(Primitive)).Meshes, (vector<MeshBatchHandle>{batch}));
    EXPECT_EQ(World.GetOwningPrimitive(batch), Primitive);
    EXPECT_EQ(World.GetDirtyBatches().size(), 1u);
}

TEST_F(SceneTest, AddMeshBatchRejectsUnknownPrimitive) {
    EXPECT_FALSE(World.AddMeshBatch({}, MakeBatch()).IsValid());
    EXPECT_EQ(World.GetMeshBatchCount(), 0u);
}

TEST_F(SceneTest, AddMeshBatchRejectsUnknownMaterial) {
    auto batch = MakeBatch();
    batch.Material = {};
    EXPECT_FALSE(World.AddMeshBatch(Primitive, std::move(batch)).IsValid());
}

TEST_F(SceneTest, AddMeshBatchRejectsIndexRangePastBuffer) {
    auto batch = MakeBatch();
    batch.Elements[0].FirstIndex = 31;
    EXPECT_FALSE(World.AddMeshBatch(Primitive, std::move(batch)).IsValid());
}

TEST_F(SceneTest, AddMeshBatchAcceptsBatchWithoutGeometry) {
    MeshBatch empty;
    empty.Material = Material;
    EXPECT_TRUE(World.AddMeshBatch(Primitive, std::move(empty)).IsValid());
}

TEST_F(SceneTest, UpdateMeshBatchRejectsInvalidBatchAndKeepsRecord) {
    const auto batch = AddBatch();
    auto invalid = MakeBatch();
    invalid.Elements[0].IndexStride = 3;
    EXPECT_FALSE(World.UpdateMeshBatch(batch, std::move(invalid)));
    EXPECT_EQ((*World.GetMeshBatch(batch)).Elements[0].IndexStride, 4u);
}

TEST_F(SceneTest, RemoveMeshBatchRequiresOwningPrimitive) {
    const auto batch = AddBatch();
    const auto other = World.AddPrimitive();
    EXPECT_FALSE(World.RemoveMeshBatch(other, batch));
    EXPECT_TRUE(World.RemoveMeshBatch(Primitive, batch));
    EXPECT_FALSE(World.GetMeshBatch(batch));
    EXPECT_TRUE((*World.GetPrimitive(Primitive)).Meshes.empty());
    EXPECT_FALSE(World.HasDirtyBatches());
}

TEST_F(SceneTest, RemovePrimitiveRemovesItsBatchesAndCommands) {
    const auto batch = AddBatch();
    Sync();
    ASSERT_TRUE(World.RemovePrimitive(Primitive));
    EXPECT_FALSE(World.GetPrimitive(Primitive));
    EXPECT_FALSE(World.GetMeshBatch(batch));
    EXPECT_EQ(World.GetCachedDrawList(Pass)->GetCommands().size(), 0u);
}

TEST_F(SceneTest, RemoveMaterialRejectsReferencedMaterial) {
    const auto batch = AddBatch();
    EXPECT_FALSE(World.RemoveMaterial(Material));
    ASSERT_TRUE(World.RemoveMeshBatch(Primitive, batch));
    EXPECT_TRUE(World.RemoveMaterial(Material));
}

TEST_F(SceneTest, AddPrimitiveRejectsPrepopulatedLinks) {
    PrimitiveSceneInfo primitive;
    primitive.Meshes.push_back({});
    EXPECT_FALSE(World.AddPrimitive(primitive).IsValid());
}

TEST_F(SceneTest, AddPrimitiveRejectsBatchOwnedByAnotherPrimitive) {
    const auto batch = AddBatch();
    PrimitiveSceneInfo primitive;
    primitive.Meshes.push_back(batch);
    EXPECT_FALSE(World.AddPrimitive(std::move(primitive)).IsValid());
    EXPECT_EQ(World.GetPrimitiveCount(), 1u);
}

TEST_F(SceneTest, FirstSyncBuildsEveryLinkedBatch) {
    const auto added = AddBatch();
    const auto direct = AddBatch();
    Sync();
    EXPECT_EQ(Processor.Calls.size(), 2u);
    EXPECT_EQ((World.GetPassCache(Pass)->GetCommand(added.GetIndex()) ? 1u : 0u), 1u);
    EXPECT_EQ((World.GetPassCache(Pass)->GetCommand(direct.GetIndex()) ? 1u : 0u), 1u);
    EXPECT_FALSE(World.HasDirtyBatches());
}

TEST_F(SceneTest, SyncLeavesCleanBatchesUntouched) {
    const auto batch = AddBatch();
    Sync();
    const auto handle = CommandIndex(batch);
    Processor.Calls.clear();
    Sync();
    EXPECT_TRUE(Processor.Calls.empty());
    EXPECT_EQ(CommandIndex(batch), handle);
}

TEST_F(SceneTest, UpdatedBatchIsRebuiltOnNextSync) {
    const auto edited = AddBatch();
    const auto untouched = AddBatch();
    Sync();
    const auto untouchedBindings = CachedCommand(untouched).VertexBuffersIndex;
    auto batch = MakeBatch();
    batch.Elements[0].FirstIndex = 6;
    ASSERT_TRUE(World.UpdateMeshBatch(edited, std::move(batch)));
    Processor.Calls.clear();
    Sync();
    EXPECT_EQ(Processor.Calls, (vector<MeshBatchHandle>{edited}));
    EXPECT_EQ(CachedCommand(edited).FirstIndex, 6u);
    EXPECT_EQ(CachedCommand(untouched).VertexBuffersIndex, untouchedBindings);
}

TEST_F(SceneTest, UpdateRebuildsOwnedElementCopy) {
    const auto batch = AddBatch();
    Sync();
    auto edited = *World.GetMeshBatch(batch);
    edited.Elements[0].VertexOffset = 5;
    ASSERT_TRUE(World.UpdateMeshBatch(batch, edited));
    edited.Elements[0].VertexOffset = 9;
    Sync();
    EXPECT_EQ(CachedCommand(batch).VertexOffset, 5);
}

TEST_F(SceneTest, MarkBatchDirtyQueuesBatchOnce) {
    const auto batch = AddBatch();
    Sync();
    World.MarkBatchDirty(batch);
    World.MarkBatchDirty(batch);
    EXPECT_EQ(World.GetDirtyBatches().size(), 1u);
}

TEST_F(SceneTest, UpdateMaterialMarksOnlyReferencingBatchesDirty) {
    const auto referencing = AddBatch();
    auto other = MakeBatch();
    other.Material = World.AddMaterial({});
    ASSERT_TRUE(World.AddMeshBatch(Primitive, std::move(other)).IsValid());
    Sync();
    ASSERT_TRUE(World.UpdateMaterial(Material, {}));
    EXPECT_EQ(vector<MeshBatchHandle>(World.GetDirtyBatches().begin(), World.GetDirtyBatches().end()), (vector<MeshBatchHandle>{referencing}));
}

TEST_F(SceneTest, ProcessorReceivesBatchMaterial) {
    AddBatch();
    Sync();
    EXPECT_EQ(Processor.LastMaterial.Get(), &(*World.GetMaterial(Material)));
}

TEST_F(SceneTest, RegistrationChangeRebuildsEveryBatch) {
    const auto batch = AddBatch();
    Sync();
    RecordingProcessor replacement{Pipeline};
    const MeshPassRegistration pass{Pass, &replacement};
    World.SyncCachedMeshDrawCommands(std::span{&pass, 1});
    EXPECT_EQ(replacement.Calls, (vector<MeshBatchHandle>{batch}));
    EXPECT_EQ(World.GetCachedDrawList(Pass)->GetCommands().size(), 1u);
}

TEST_F(SceneTest, RemovedPassDropsCachedCommands) {
    AddBatch();
    Sync();
    World.SyncCachedMeshDrawCommands({});
    EXPECT_FALSE(World.GetPassCache(Pass));
}

TEST_F(SceneTest, BorrowedVertexInputBindingsUpdateAfterMarkBatchDirty) {
    const auto batch = AddBatch();
    Sync();
    Input.Streams[0].Offset = 16;
    Input.Streams[0].Size = 64;
    World.MarkBatchDirty(batch);
    Sync();
    EXPECT_FALSE(World.HasDirtyBatches());
    EXPECT_TRUE(World.GetDirtyBatches().empty());
    const auto& command = CachedCommand(batch);
    const auto vertices = command.BindingStorage->GetVertexBuffers(command.VertexBuffersIndex);
    ASSERT_TRUE(vertices);
    ASSERT_EQ(vertices->Bindings.size(), 1u);
    EXPECT_EQ(vertices->Bindings[0].View.Offset, 16u);
    EXPECT_EQ(vertices->Bindings[0].View.Size, 64u);
}

TEST_F(SceneTest, GatherCombinesBaseAndVisibilitySortKeys) {
    Processor.SortKey = 3;
    const auto batch = AddBatch();
    Sync();
    const VisibleMeshBatch request{.Batch = batch, .VisibilitySortKey = 7};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(std::span{&request, 1}, visible));
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].CachedCommand.Get(), &CachedCommand(batch));
    EXPECT_EQ(visible[0].SortKey, (uint64_t{3} << 32) | 7);
}

TEST_F(SceneTest, GatherUsesElementInstanceRangeUnlessOverridden) {
    auto batch = MakeBatch();
    batch.Elements[0].FirstInstance = 7;
    batch.Elements[0].InstanceCount = 3;
    const auto handle = World.AddMeshBatch(Primitive, std::move(batch));
    Sync();
    const array<VisibleMeshBatch, 3> requests{{{.Batch = handle},
                                               {.Batch = handle, .FirstInstanceOverride = 20},
                                               {.Batch = handle, .InstanceCountOverride = 0}}};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(requests, visible));
    ASSERT_EQ(visible.size(), 3u);
    EXPECT_EQ(visible[0].FirstInstance, 7u);
    EXPECT_EQ(visible[0].InstanceCount, 3u);
    EXPECT_EQ(visible[1].FirstInstance, 20u);
    EXPECT_EQ(visible[1].InstanceCount, 3u);
    EXPECT_EQ(visible[2].FirstInstance, 7u);
    EXPECT_EQ(visible[2].InstanceCount, 0u);
}

TEST_F(SceneTest, GatherSelectsSingleElement) {
    const auto batch = AddBatch(2);
    Sync();
    const VisibleMeshBatch request{.Batch = batch, .ElementMask = VisibleMeshBatch::MaskForElement(1)};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(std::span{&request, 1}, visible));
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].CachedCommand.Get(), &CachedCommand(batch));
}

TEST_F(SceneTest, GatherRejectsDirtyBatchAndKeepsEarlierOutput) {
    const auto clean = AddBatch();
    const auto dirty = AddBatch();
    Sync();
    World.MarkBatchDirty(dirty);
    vector<VisibleMeshDrawCommand> visible(1);
    const array<VisibleMeshBatch, 2> requests{{{.Batch = clean}, {.Batch = dirty}}};
    EXPECT_FALSE(Gather(requests, visible));
    EXPECT_EQ(visible.size(), 1u);
}

TEST_F(SceneTest, GatherVisiblePrimitivesCollectsEveryBatch) {
    AddBatch();
    AddBatch(2);
    Sync();
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(World.GatherVisiblePrimitives(Pass, std::span{&Primitive, 1}, visible));
    EXPECT_EQ(visible.size(), 2u);
}

TEST_F(SceneTest, GatheredCommandsSubmitWithoutCachedListLookup) {
    AddBatch(2);
    Sync();
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(World.GatherVisiblePrimitives(Pass, std::span{&Primitive, 1}, visible));

    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 0u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 3u);
}

TEST_F(SceneTest, RegatherUsesCurrentAddressesAfterOtherCacheChanges) {
    const auto first = AddBatch();
    const auto kept = AddBatch();
    Sync();
    const VisibleMeshBatch request{.Batch = kept};
    for (int index = 0; index < 64; ++index) AddBatch();
    Sync();
    ASSERT_TRUE(World.RemoveMeshBatch(Primitive, first));
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(std::span{&request, 1}, visible));
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].CachedCommand.Get(), &CachedCommand(kept));
}

TEST_F(SceneTest, FrameDrawListSubmitsSortedBorrowedCommands) {
    Processor.SortKey = 3;
    const auto batch = AddBatch(2);
    Sync();
    const auto& command = CachedCommand(batch);
    const array<VisibleMeshBatch, 2> requests{{{.Batch = batch, .ElementMask = VisibleMeshBatch::MaskForElement(1), .VisibilitySortKey = 7, .FirstInstanceOverride = 20, .InstanceCountOverride = 2},
                                               {.Batch = batch, .ElementMask = VisibleMeshBatch::MaskForElement(0), .VisibilitySortKey = 3}}};
    MeshPassFrameDrawList list;
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands(Pass, requests, list));
    ASSERT_EQ(list.VisibleCommands.size(), 2u);
    EXPECT_EQ(list.VisibleCommands[0].CachedCommand.Get(), &CachedCommand(batch));
    EXPECT_EQ(list.VisibleCommands[1].CachedCommand.Get(), &command);
    EXPECT_EQ(list.VisibleCommands[0].SortKey, (uint64_t{3} << 32) | 7);

    list.SortVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(list.Submit(encoder));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 0u);
    EXPECT_EQ(encoder.Draws[0].FirstInstance, 0u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 3u);
    EXPECT_EQ(encoder.Draws[1].FirstInstance, 20u);
    EXPECT_EQ(encoder.Draws[1].InstanceCount, 2u);
}

TEST_F(SceneTest, FrameGatherAppendsOnlyRequestedPassAndKeepsPreviousBorrowing) {
    const auto batch = AddBatch();
    RecordingProcessor other{Pipeline};
    other.SortKey = 99;
    const array<MeshPassRegistration, 2> passes{{{Pass, &Processor}, {{1}, &other}}};
    World.SyncCachedMeshDrawCommands(passes);
    MeshPassFrameDrawList list;
    ASSERT_TRUE(World.GatherVisiblePrimitives(Pass, std::span{&Primitive, 1}, list));
    ASSERT_TRUE(World.GatherVisiblePrimitives({1}, std::span{&Primitive, 1}, list));
    ASSERT_EQ(list.VisibleCommands.size(), 2u);
    EXPECT_EQ(list.VisibleCommands[0].CachedCommand.Get(), &CachedCommand(batch));
    EXPECT_EQ(list.VisibleCommands[1].CachedCommand.Get(), World.GetPassCache({1})->GetCommand(batch.GetIndex()).Get());
    EXPECT_NE(list.VisibleCommands[0].CachedCommand.Get(), list.VisibleCommands[1].CachedCommand.Get());
    EXPECT_EQ(list.VisibleCommands[0].SortKey, 0u);
    EXPECT_EQ(list.VisibleCommands[1].SortKey, uint64_t{99} << 32);
    test::RecordingEncoder encoder;
    ASSERT_TRUE(list.Submit(encoder));
    EXPECT_EQ(encoder.Draws.size(), 2u);
}

TEST_F(SceneTest, FrameGatherRollsBackOnInvalidBatch) {
    const auto batch = AddBatch();
    Sync();
    const VisibleMeshBatch valid{.Batch = batch, .VisibilitySortKey = 7};
    MeshPassFrameDrawList list;
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands(Pass, std::span{&valid, 1}, list));
    const array<VisibleMeshBatch, 2> invalid{{valid, {.Batch = {}}}};
    EXPECT_FALSE(World.GatherVisibleMeshDrawCommands(Pass, invalid, list));
    ASSERT_EQ(list.VisibleCommands.size(), 1u);
    EXPECT_EQ(list.VisibleCommands[0].CachedCommand.Get(), &CachedCommand(batch));
    EXPECT_EQ(list.VisibleCommands[0].SortKey, 7u);
}

TEST_F(SceneTest, FrameGatherRejectsRemovedBatchWithoutChangingEarlierOutput) {
    const auto validBatch = AddBatch();
    const auto staleBatch = AddBatch();
    Sync();
    ASSERT_TRUE(World.RemoveMeshBatch(staleBatch));
    const VisibleMeshBatch valid{.Batch = validBatch};
    MeshPassFrameDrawList list;
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands(Pass, std::span{&valid, 1}, list));
    const array<VisibleMeshBatch, 2> invalid{{valid, {.Batch = staleBatch}}};
    EXPECT_FALSE(World.GatherVisibleMeshDrawCommands(Pass, invalid, list));
    EXPECT_EQ(list.VisibleCommands.size(), 1u);
}

TEST_F(SceneTest, FramePrimitiveGatherRollsBackOnInvalidPrimitive) {
    AddBatch();
    AddBatch(2);
    Sync();
    MeshPassFrameDrawList list;
    ASSERT_TRUE(World.GatherVisiblePrimitives(Pass, std::span{&Primitive, 1}, list));
    const array<PrimitiveHandle, 2> invalid{Primitive, {}};
    EXPECT_FALSE(World.GatherVisiblePrimitives(Pass, invalid, list));
    EXPECT_EQ(list.VisibleCommands.size(), 2u);
    test::RecordingEncoder encoder;
    ASSERT_TRUE(list.Submit(encoder));
    EXPECT_EQ(encoder.Draws.size(), 3u);
}

TEST_F(SceneTest, RecordBorrowsCommandsAndNextFrameReflectsMeshRemoval) {
    const auto batch = AddBatch();
    test::FakeGpu gpu;
    test::RecordingEncoder encoder;
    test::CallbackPipeline* callbacks{nullptr};
    auto scheduler = test::CreateScheduler(gpu.Context, callbacks, 1);
    ASSERT_TRUE(scheduler);
    callbacks->Passes = {{Pass, &Processor}};
    callbacks->OnExtract = [&](FrameSlot& slot) {
        ASSERT_TRUE(World.GatherVisiblePrimitives(Pass, std::span{&Primitive, 1}, slot.GetPassDrawList(Pass)));
    };
    callbacks->OnRecord = [&](FrameSlot& slot) {
        auto& list = slot.GetPassDrawList(Pass);
        list.SortVisibleCommands();
        ASSERT_TRUE(list.Submit(encoder));
    };
    World.SyncCachedMeshDrawCommands(callbacks->GetMeshPassRegistrations());
    scheduler->RunFrame();
    ASSERT_EQ(encoder.Draws.size(), 1u);
    EXPECT_EQ(encoder.Draws[0].IndexCount, 3u);
    ASSERT_TRUE(World.RemoveMeshBatch(Primitive, batch));
    World.SyncCachedMeshDrawCommands(callbacks->GetMeshPassRegistrations());
    scheduler->RunFrame();
    EXPECT_EQ(encoder.Draws.size(), 1u);
    AddBatch(2);
    World.SyncCachedMeshDrawCommands(callbacks->GetMeshPassRegistrations());
    scheduler->RunFrame();
    ASSERT_EQ(encoder.Draws.size(), 3u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 0u);
    EXPECT_EQ(encoder.Draws[2].FirstIndex, 3u);
    World.Clear();
    scheduler->WaitIdle();
}

TEST_F(SceneTest, DirtyBatchRebuildReleasesBindingsAndKeepsOtherBatchAlive) {
    const auto changed = AddBatch(3);
    const auto kept = AddBatch(2);
    Sync();
    const auto oldBindings = CachedCommand(changed).VertexBuffersIndex;
    const auto keptBindings = CachedCommand(kept).VertexBuffersIndex;

    ASSERT_EQ(World.GetCachedDrawList(Pass)->GetBindings().GetVertexBufferCount(), 1u);
    World.MarkBatchDirty(changed);
    Sync();
    const auto& storage = World.GetCachedDrawList(Pass)->GetBindings();
    EXPECT_EQ(oldBindings, keptBindings);
    EXPECT_TRUE(storage.GetVertexBuffers(keptBindings));
    EXPECT_EQ(CachedCommand(kept).VertexBuffersIndex, keptBindings);
    EXPECT_EQ(storage.GetVertexBufferCount(), 1u);
    EXPECT_TRUE(World.RemoveMeshBatch(Primitive, changed));
    EXPECT_EQ(storage.GetVertexBufferCount(), 1u);
    EXPECT_TRUE(storage.GetVertexBuffers(keptBindings));
    World.SyncCachedMeshDrawCommands({});
    EXPECT_TRUE(storage.GetVertexBufferCount() == 0);
}

TEST_F(SceneTest, ClearRemovesRecordsAndCommands) {
    AddBatch();
    Sync();
    World.Clear();
    EXPECT_EQ(World.GetPrimitiveCount(), 0u);
    EXPECT_EQ(World.GetMeshBatchCount(), 0u);
    EXPECT_EQ(World.GetMaterialCount(), 0u);
    EXPECT_FALSE(World.GetPassCache(Pass));
}

TEST_F(SceneTest, GatherFiltersEmptyAndUnknownBitsWithoutExpandingBatch) {
    auto source = MakeBatch(4);
    source.Elements[1].IndexCount = 0;
    source.Elements[3].InstanceCount = 0;
    const auto batch = World.AddMeshBatch(Primitive, std::move(source));
    Sync();
    const array<VisibleMeshBatch, 3> requests{{{.Batch = batch, .ElementMask = 0},
                                               {.Batch = batch, .ElementMask = 0x8000000a},
                                               {.Batch = batch}}};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(requests, visible));
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].ElementMask, 5u);
    EXPECT_EQ((World.GetPassCache(Pass)->GetCommand(batch.GetIndex()) ? 1u : 0u), 1u);
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 6u);
    EXPECT_EQ(VisibleMeshBatch::MaskForElement(32), 0u);
}

TEST_F(SceneTest, DifferentElementInstancesAndIndependentOverridesSurviveBatching) {
    auto source = MakeBatch(2);
    source.Elements[0].FirstInstance = 3;
    source.Elements[0].InstanceCount = 2;
    source.Elements[1].FirstInstance = 8;
    source.Elements[1].InstanceCount = 5;
    const auto batch = World.AddMeshBatch(Primitive, std::move(source));
    Sync();
    const array<VisibleMeshBatch, 4> requests{{{.Batch = batch},
                                               {.Batch = batch, .FirstInstanceOverride = 20},
                                               {.Batch = batch, .InstanceCountOverride = 4},
                                               {.Batch = batch, .InstanceCountOverride = 0}}};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(requests, visible));
    ASSERT_EQ(visible.size(), 4u);
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible));
    ASSERT_EQ(encoder.Draws.size(), 6u);
    const array<uint32_t, 6> first{3, 8, 20, 20, 3, 8};
    const array<uint32_t, 6> count{2, 5, 2, 5, 4, 4};
    for (size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(encoder.Draws[index].FirstInstance, first[index]);
        EXPECT_EQ(encoder.Draws[index].InstanceCount, count[index]);
    }
}

TEST_F(SceneTest, RejectsMoreThan32EntriesBeforeLinkingBatch) {
    auto source = MakeBatch();
    while (source.Elements.size() < 33) source.Elements.emplace_back();
    EXPECT_FALSE(World.AddMeshBatch(Primitive, std::move(source)).IsValid());
    EXPECT_TRUE((*World.GetPrimitive(Primitive)).Meshes.empty());
}

TEST_F(SceneTest, InstanceOverrideValidationOnlyChecksSelectedElements) {
    auto source = MakeBatch(2);
    source.Elements[0].FirstInstance = std::numeric_limits<uint32_t>::max();
    source.Elements[1].FirstInstance = 3;
    const auto batch = World.AddMeshBatch(Primitive, std::move(source));
    Sync();
    const VisibleMeshBatch request{.Batch = batch, .ElementMask = 2, .InstanceCountOverride = 2};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(std::span{&request, 1}, visible));
    ASSERT_EQ(visible.size(), 1u);
    const auto& storage = World.GetCachedDrawList(Pass)->GetBindings();
    EXPECT_TRUE(ValidateMeshDrawCommand(*visible[0].CachedCommand, storage, visible[0]));
    visible[0].ElementMask = 1;
    EXPECT_FALSE(ValidateMeshDrawCommand(*visible[0].CachedCommand, storage, visible[0]));
}

TEST_F(SceneTest, RetargetingMaterialUpdatesReverseReferences) {
    const auto handle = AddBatch();
    const auto second = World.AddMaterial({});
    auto batch = *World.GetMeshBatch(handle);
    batch.Material = second;
    ASSERT_TRUE(World.UpdateMeshBatch(handle, batch));
    EXPECT_TRUE(World.RemoveMaterial(Material));
    EXPECT_FALSE(World.RemoveMaterial(second));
    Sync();
    World.MarkMaterialDirty(second);
    EXPECT_TRUE(World.IsBatchDirty(handle));
    EXPECT_TRUE(World.RemoveMeshBatch(handle));
    EXPECT_TRUE(World.RemoveMaterial(second));
}

TEST_F(SceneTest, DeletedDirtyEntryDoesNotRebuildReusedSlotTwice) {
    const auto removed = AddBatch();
    ASSERT_TRUE(World.RemoveMeshBatch(removed));
    const auto replacement = AddBatch();
    ASSERT_EQ(removed.GetIndex(), replacement.GetIndex());
    Sync();
    EXPECT_EQ(Processor.Calls, (vector<MeshBatchHandle>{replacement}));
    EXPECT_FALSE(World.HasDirtyBatches());
    const VisibleMeshBatch stale{.Batch = removed};
    vector<VisibleMeshDrawCommand> visible;
    EXPECT_FALSE(Gather(std::span{&stale, 1}, visible));
}

TEST_F(SceneTest, RemovedAndUnregisteredPassesProduceEmptyOutput) {
    const auto batch = AddBatch();
    Sync();
    vector<VisibleMeshDrawCommand> visible;
    const VisibleMeshBatch request{.Batch = batch};
    EXPECT_TRUE(World.GatherVisibleMeshDrawCommands({3}, std::span{&request, 1}, visible));
    World.SyncCachedMeshDrawCommands({});
    EXPECT_TRUE(Gather(std::span{&request, 1}, visible));
    EXPECT_TRUE(visible.empty());
}

TEST_F(SceneTest, CachedAndDynamicCommandsUseTheSamePassSortMode) {
    const auto cachedBatch = AddBatch();
    const auto dynamicBatch = MakeBatch();
    Processor.SortKey = 7;
    for (const auto mode : {MeshPassSortMode::StateFirst, MeshPassSortMode::DepthFirst}) {
        const MeshPassRegistration pass{Pass, &Processor, mode};
        World.SyncCachedMeshDrawCommands(std::span{&pass, 1});
        MeshPassFrameDrawList list{mode};
        const VisibleMeshBatch request{.Batch = cachedBatch, .VisibilitySortKey = 3};
        ASSERT_TRUE(World.GatherVisibleMeshDrawCommands(Pass, std::span{&request, 1}, list));
        RecordingProcessor dynamic{Pipeline};
        dynamic.SortKey = 2;
        dynamic.AddMeshBatch({*World.GetPrimitive(Primitive), dynamicBatch, {}, {}, nullptr, 9}, list.DynamicContext);
        list.SortVisibleCommands();
        ASSERT_EQ(list.VisibleCommands.size(), 2u);
        const auto cachedKey = ComposeMeshDrawSortKey(mode, 7, 3);
        const auto dynamicKey = ComposeMeshDrawSortKey(mode, 2, 9);
        EXPECT_EQ(list.VisibleCommands[0].SortKey, std::min(cachedKey, dynamicKey));
        EXPECT_EQ(list.VisibleCommands[1].SortKey, std::max(cachedKey, dynamicKey));
        const size_t cachedPosition = mode == MeshPassSortMode::DepthFirst ? 0 : 1;
        EXPECT_EQ(list.VisibleCommands[cachedPosition].CachedCommand.Get(), &CachedCommand(cachedBatch));
    }
}

TEST_F(SceneTest, SwappingLastCommandIntoHolePreservesItsBatchAndInstanceDefaults) {
    World.Reserve(1, 128, 1);
    const auto removed = AddBatch();
    auto batch = MakeBatch(2);
    batch.Elements[0].FirstInstance = batch.Elements[1].FirstInstance = 17;
    const auto moved = World.AddMeshBatch(Primitive, batch);
    Sync();
    const auto removedIndex = CommandIndex(removed);
    ASSERT_TRUE(World.RemoveMeshBatch(removed));
    EXPECT_EQ(CommandIndex(moved), removedIndex);
    vector<VisibleMeshDrawCommand> visible;
    const VisibleMeshBatch request{.Batch = moved};
    ASSERT_TRUE(Gather(std::span{&request, 1}, visible));
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].CachedCommand.Get(), &CachedCommand(moved));
    EXPECT_EQ(visible[0].FirstInstance, 17u);
}

TEST_F(SceneTest, SyncRevalidatesBorrowedVerticesOnlyInDebug) {
    class DescribedBuffer final : public FakeBuffer {
    public:
        using FakeBuffer::FakeBuffer;
        render::BufferDescriptor GetDesc() const noexcept override {
            ++Descriptions;
            return Desc;
        }
        mutable uint32_t Descriptions{0};
    };
    DescribedBuffer indices{128, render::BufferUse::Index};
    DescribedBuffer vertices{128, render::BufferUse::Vertex};
    Input.Streams[0].Buffer = &vertices;
    auto batch = MakeBatch(3);
    for (auto& element : batch.Elements) element.IndexBuffer = &indices;
    const auto handle = World.AddMeshBatch(Primitive, batch);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_GT(indices.Descriptions, 0u);
    EXPECT_GT(vertices.Descriptions, 0u);
    indices.Descriptions = vertices.Descriptions = 0;
    Sync();
    EXPECT_EQ(indices.Descriptions, 0u);
#if RADRAY_IS_DEBUG
    EXPECT_GT(vertices.Descriptions, 0u);
#else
    EXPECT_EQ(vertices.Descriptions, 0u);
#endif
    World.MarkMaterialDirty(Material);
    Sync();
    EXPECT_EQ(indices.Descriptions, 0u);
    ASSERT_TRUE(World.UpdateMeshBatch(handle, batch));
    EXPECT_GT(indices.Descriptions, 0u);
}

TEST_F(SceneTest, RemovedDepthPassDoesNotChangeExistingFrameSortMode) {
    const auto batch = AddBatch();
    const MeshPassRegistration pass{Pass, &Processor, MeshPassSortMode::DepthFirst};
    World.SyncCachedMeshDrawCommands(std::span{&pass, 1});
    World.SyncCachedMeshDrawCommands({});
    MeshPassFrameDrawList list;
    list.VisibleCommands.push_back({.SortKey = 42});
    const VisibleMeshBatch request{.Batch = batch};
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands(Pass, std::span{&request, 1}, list));
    EXPECT_EQ(list.VisibleCommands.size(), 1u);
    EXPECT_EQ(list.VisibleCommands[0].SortKey, 42u);
    EXPECT_EQ(list.GetSortMode(), MeshPassSortMode::StateFirst);
}

}  // namespace
}  // namespace radray
