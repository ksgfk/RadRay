#include <gtest/gtest.h>

#include <radray/runtime/frame_slot.h>
#include <radray/runtime/scene.h>

#include "test_fakes.h"

namespace radray {
namespace {

using test::FakeBuffer;
using test::FakePipeline;

class RecordingProcessor final : public MeshPassProcessor {
public:
    RecordingProcessor(MeshPassDrawListContext& output, render::GraphicsPipelineState& pipeline) noexcept
        : MeshPassProcessor(output), _pipeline(&pipeline) {}

    void AddMeshBatch(const MeshBatchContext& context) override {
        Calls.push_back(context.BatchHandle);
        LastMaterial = context.Material;
        EXPECT_TRUE(BuildMeshDrawCommands(context, {.Pipeline = _pipeline, .BaseSortKey = SortKey}));
    }

    vector<SparseSetHandle> Calls;
    Nullable<const MaterialRenderProxy*> LastMaterial{nullptr};
    uint64_t SortKey{0};

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
    SparseSetHandle AddBatch(uint32_t elementCount = 1) { return World.AddMeshBatch(Primitive, MakeBatch(elementCount)); }
    void Sync() {
        const MeshPassRegistration pass{Pass, &Processor};
        World.SyncCachedMeshDrawCommands(std::span{&pass, 1});
    }
    SparseSetHandle CommandHandle(SparseSetHandle batch) const { return World.Meshes.Get(batch).CachedCommands[0].CommandHandle; }
    const MeshDrawCommand& CachedCommand(SparseSetHandle batch) const {
        return World.GetCachedDrawList(Pass)->Commands.Get(CommandHandle(batch));
    }
    bool Gather(std::span<const VisibleMeshBatch> requests, vector<VisibleMeshDrawCommand>& output) const {
        return World.GatherVisibleMeshDrawCommands(Pass, requests, output);
    }

    static constexpr MeshPassId Pass{0};
    FakePipeline Pipeline;
    FakeBuffer Vertices{128, render::BufferUse::Vertex};
    FakeBuffer Indices{128, render::BufferUse::Index};
    VertexInput Input;
    DynamicMeshPassDrawListContext DefaultOutput;
    RecordingProcessor Processor{DefaultOutput, Pipeline};
    Scene World;
    SparseSetHandle Material;
    SparseSetHandle Primitive;
};

TEST_F(SceneTest, AddMeshBatchLinksBatchToPrimitive) {
    const auto batch = AddBatch();
    ASSERT_TRUE(batch.IsValid());
    EXPECT_EQ(World.Primitives.Get(Primitive).Meshes, (vector<SparseSetHandle>{batch}));
    EXPECT_EQ(World.Meshes.Get(batch).OwningPrimitive, Primitive);
    EXPECT_EQ(World.GetDirtyBatches().size(), 1u);
}

TEST_F(SceneTest, AddMeshBatchRejectsUnknownPrimitive) {
    EXPECT_FALSE(World.AddMeshBatch({}, MakeBatch()).IsValid());
    EXPECT_EQ(World.Meshes.Count(), 0u);
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
    EXPECT_EQ(World.Meshes.Get(batch).Elements[0].IndexStride, 4u);
}

TEST_F(SceneTest, RemoveMeshBatchRequiresOwningPrimitive) {
    const auto batch = AddBatch();
    const auto other = World.AddPrimitive();
    EXPECT_FALSE(World.RemoveMeshBatch(other, batch));
    EXPECT_TRUE(World.RemoveMeshBatch(Primitive, batch));
    EXPECT_FALSE(World.Meshes.IsAlive(batch));
    EXPECT_TRUE(World.Primitives.Get(Primitive).Meshes.empty());
    EXPECT_FALSE(World.HasDirtyBatches());
}

TEST_F(SceneTest, RemovePrimitiveRemovesItsBatchesAndCommands) {
    const auto batch = AddBatch();
    Sync();
    ASSERT_TRUE(World.RemovePrimitive(Primitive));
    EXPECT_FALSE(World.Primitives.IsAlive(Primitive));
    EXPECT_FALSE(World.Meshes.IsAlive(batch));
    EXPECT_EQ(World.GetCachedDrawList(Pass)->Commands.Count(), 0u);
}

TEST_F(SceneTest, RemoveMaterialRejectsReferencedMaterial) {
    const auto batch = AddBatch();
    EXPECT_FALSE(World.RemoveMaterial(Material));
    ASSERT_TRUE(World.RemoveMeshBatch(Primitive, batch));
    EXPECT_TRUE(World.RemoveMaterial(Material));
}

TEST_F(SceneTest, AddPrimitiveAdoptsUnownedBatch) {
    const auto batch = World.Meshes.Emplace(MakeBatch());
    PrimitiveSceneInfo primitive;
    primitive.Meshes.push_back(batch);
    const auto owner = World.AddPrimitive(primitive);
    ASSERT_TRUE(owner.IsValid());
    EXPECT_EQ(World.Meshes.Get(batch).OwningPrimitive, owner);
    EXPECT_TRUE(World.Meshes.Get(batch).Dirty);
}

TEST_F(SceneTest, AddPrimitiveRejectsBatchOwnedByAnotherPrimitive) {
    const auto batch = AddBatch();
    PrimitiveSceneInfo primitive;
    primitive.Meshes.push_back(batch);
    EXPECT_FALSE(World.AddPrimitive(std::move(primitive)).IsValid());
    EXPECT_EQ(World.Primitives.Count(), 1u);
}

TEST_F(SceneTest, FirstSyncBuildsEveryLinkedBatch) {
    const auto added = AddBatch();
    const auto direct = World.Meshes.Emplace(MakeBatch());
    World.Primitives.Get(Primitive).Meshes.push_back(direct);
    Sync();
    EXPECT_EQ(Processor.Calls.size(), 2u);
    EXPECT_EQ(World.Meshes.Get(added).CachedCommands.size(), 1u);
    EXPECT_EQ(World.Meshes.Get(direct).CachedCommands.size(), 1u);
    EXPECT_FALSE(World.HasDirtyBatches());
}

TEST_F(SceneTest, SyncLeavesCleanBatchesUntouched) {
    const auto batch = AddBatch();
    Sync();
    const auto handle = CommandHandle(batch);
    Processor.Calls.clear();
    Sync();
    EXPECT_TRUE(Processor.Calls.empty());
    EXPECT_EQ(CommandHandle(batch), handle);
}

TEST_F(SceneTest, UpdatedBatchIsRebuiltOnNextSync) {
    const auto edited = AddBatch();
    const auto untouched = AddBatch();
    Sync();
    const auto untouchedHandle = CommandHandle(untouched);
    auto batch = MakeBatch();
    batch.Elements[0].FirstIndex = 6;
    ASSERT_TRUE(World.UpdateMeshBatch(edited, std::move(batch)));
    Processor.Calls.clear();
    Sync();
    EXPECT_EQ(Processor.Calls, (vector<SparseSetHandle>{edited}));
    EXPECT_EQ(CachedCommand(edited).FirstIndex, 6u);
    EXPECT_EQ(CommandHandle(untouched), untouchedHandle);
}

TEST_F(SceneTest, DirectEditIsRebuiltAfterMarkBatchDirty) {
    const auto batch = AddBatch();
    Sync();
    World.Meshes.Get(batch).Elements[0].VertexOffset = 5;
    World.MarkBatchDirty(batch);
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
    EXPECT_EQ(vector<SparseSetHandle>(World.GetDirtyBatches().begin(), World.GetDirtyBatches().end()), (vector<SparseSetHandle>{referencing}));
}

TEST_F(SceneTest, ProcessorReceivesBatchMaterial) {
    AddBatch();
    Sync();
    EXPECT_EQ(Processor.LastMaterial.Get(), &World.Materials.Get(Material));
}

TEST_F(SceneTest, SyncRestoresProcessorContext) {
    AddBatch();
    Sync();
    EXPECT_EQ(&Processor.DrawListContext(), &DefaultOutput);
    EXPECT_TRUE(DefaultOutput.Commands.empty());
}

TEST_F(SceneTest, RegistrationChangeRebuildsEveryBatch) {
    const auto batch = AddBatch();
    Sync();
    RecordingProcessor replacement{DefaultOutput, Pipeline};
    const MeshPassRegistration pass{Pass, &replacement};
    World.SyncCachedMeshDrawCommands(std::span{&pass, 1});
    EXPECT_EQ(replacement.Calls, (vector<SparseSetHandle>{batch}));
    EXPECT_EQ(World.GetCachedDrawList(Pass)->Commands.Count(), 1u);
}

TEST_F(SceneTest, RemovedPassDropsCachedCommands) {
    const auto batch = AddBatch();
    Sync();
    World.SyncCachedMeshDrawCommands({});
    EXPECT_TRUE(World.Meshes.Get(batch).CachedCommands.empty());
    EXPECT_EQ(World.GetCachedDrawList(Pass)->Commands.Count(), 0u);
}

TEST_F(SceneTest, InvalidDirectEditStaysDirtyUntilRepaired) {
    const auto batch = AddBatch();
    Sync();
    World.Meshes.Get(batch).Elements[0].IndexStride = 3;
    World.MarkBatchDirty(batch);
    Sync();
    EXPECT_TRUE(World.Meshes.Get(batch).CachedCommands.empty());
    EXPECT_EQ(World.GetDirtyBatches().size(), 1u);
    World.Meshes.Get(batch).Elements[0].IndexStride = 4;
    Sync();
    EXPECT_FALSE(World.HasDirtyBatches());
    EXPECT_EQ(World.Meshes.Get(batch).CachedCommands.size(), 1u);
}

TEST_F(SceneTest, GatherCombinesBaseAndVisibilitySortKeys) {
    Processor.SortKey = uint64_t{3} << 32;
    const auto batch = AddBatch();
    Sync();
    const VisibleMeshBatch request{.Batch = batch, .VisibilitySortKey = 7};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(std::span{&request, 1}, visible));
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].Source, MeshDrawCommandSource::Cached);
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
    const VisibleMeshBatch request{.Batch = batch, .ElementIndex = 1};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(Gather(std::span{&request, 1}, visible));
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].CachedCommand.Get(), World.GetCachedDrawList(Pass)->Commands.TryGet(World.Meshes.Get(batch).CachedCommands[1].CommandHandle));
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
    EXPECT_EQ(visible.size(), 3u);
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
    Processor.SortKey = uint64_t{3} << 32;
    const auto batch = AddBatch(2);
    Sync();
    auto& command = World.CachedDrawLists[Pass.Value].Commands.Get(CommandHandle(batch));
    command.PushConstants = MeshPushConstantCommandBinding{0, 0, {}};
    for (size_t index = 0; index < 80; ++index) command.PushConstants->Data.push_back(byte{0x17});
    const array<VisibleMeshBatch, 2> requests{{{.Batch = batch, .ElementIndex = 1, .VisibilitySortKey = 7, .FirstInstanceOverride = 20, .InstanceCountOverride = 2},
                                               {.Batch = batch, .ElementIndex = 0, .VisibilitySortKey = 3}}};
    MeshPassFrameDrawList list;
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands(Pass, requests, list));
    ASSERT_EQ(list.VisibleCommands.size(), 2u);
    EXPECT_EQ(list.VisibleCommands[0].CachedCommand.Get(), World.GetCachedDrawList(Pass)->Commands.TryGet(World.Meshes.Get(batch).CachedCommands[1].CommandHandle));
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
    ASSERT_EQ(encoder.Pushes.size(), 1u);
    EXPECT_EQ(encoder.Pushes[0], (vector<byte>(80, byte{0x17})));
}

TEST_F(SceneTest, FrameGatherAppendsOnlyRequestedPassAndKeepsPreviousBorrowing) {
    const auto batch = AddBatch();
    RecordingProcessor other{DefaultOutput, Pipeline};
    other.SortKey = 99;
    const array<MeshPassRegistration, 2> passes{{{Pass, &Processor}, {{1}, &other}}};
    World.SyncCachedMeshDrawCommands(passes);
    MeshPassFrameDrawList list;
    ASSERT_TRUE(World.GatherVisiblePrimitives(Pass, std::span{&Primitive, 1}, list));
    ASSERT_TRUE(World.GatherVisiblePrimitives({1}, std::span{&Primitive, 1}, list));
    ASSERT_EQ(list.VisibleCommands.size(), 2u);
    EXPECT_EQ(list.VisibleCommands[0].CachedCommand.Get(), &CachedCommand(batch));
    EXPECT_EQ(list.VisibleCommands[1].CachedCommand.Get(), World.GetCachedDrawList({1})->Commands.TryGet(World.Meshes.Get(batch).CachedCommands[1].CommandHandle));
    EXPECT_NE(list.VisibleCommands[0].CachedCommand.Get(), list.VisibleCommands[1].CachedCommand.Get());
    EXPECT_EQ(list.VisibleCommands[0].SortKey, 0u);
    EXPECT_EQ(list.VisibleCommands[1].SortKey, 99u);
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

TEST_F(SceneTest, FrameGatherRejectsStaleCachedCommandWithoutChangingEarlierOutput) {
    const auto validBatch = AddBatch();
    const auto staleBatch = AddBatch();
    Sync();
    World.CachedDrawLists[Pass.Value].Commands.Destroy(CommandHandle(staleBatch));
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
    const array<SparseSetHandle, 2> invalid{Primitive, {}};
    EXPECT_FALSE(World.GatherVisiblePrimitives(Pass, invalid, list));
    EXPECT_EQ(list.VisibleCommands.size(), 3u);
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
    callbacks->OnExtract = [&](const Scene& source, FrameSlot& slot) {
        ASSERT_TRUE(source.GatherVisiblePrimitives(Pass, std::span{&Primitive, 1}, slot.GetPassDrawList(Pass)));
    };
    callbacks->OnRecord = [&](const Scene& source, FrameSlot& slot) {
        EXPECT_EQ(&source, &World);
        auto& list = slot.GetPassDrawList(Pass);
        list.SortVisibleCommands();
        ASSERT_TRUE(list.Submit(encoder));
    };
    scheduler->RunFrame(World);
    ASSERT_EQ(encoder.Draws.size(), 1u);
    EXPECT_EQ(encoder.Draws[0].IndexCount, 3u);
    ASSERT_TRUE(World.RemoveMeshBatch(Primitive, batch));
    scheduler->RunFrame(World);
    EXPECT_EQ(encoder.Draws.size(), 1u);
    AddBatch(2);
    scheduler->RunFrame(World);
    ASSERT_EQ(encoder.Draws.size(), 3u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 0u);
    EXPECT_EQ(encoder.Draws[2].FirstIndex, 3u);
    World.Clear();
    scheduler->WaitIdle();
}

TEST_F(SceneTest, ClearRemovesRecordsAndCommands) {
    const auto batch = AddBatch();
    Sync();
    const auto handle = CommandHandle(batch);
    World.Clear();
    EXPECT_EQ(World.Primitives.Count(), 0u);
    EXPECT_EQ(World.Meshes.Count(), 0u);
    EXPECT_EQ(World.Materials.Count(), 0u);
    EXPECT_FALSE(World.GetCachedDrawList(Pass)->Commands.IsAlive(handle));
}

}  // namespace
}  // namespace radray
