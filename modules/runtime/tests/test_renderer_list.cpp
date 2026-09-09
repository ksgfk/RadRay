#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <span>
#include <tuple>
#include <radray/runtime/render_framework/cpu_draw_record.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>

namespace radray {
namespace {

TEST(MeshPassDrawListContext, ConsumesInlineAndSpilledGroupsIntoFinalStorage) {
    for (uint32_t groupCount : {3u, 5u}) {
        MeshPassDrawListContext context;
        MeshDrawCommand command;
        command.IndexCount = 42;
        command.FirstIndex = 7;
        for (uint32_t index = 0; index < groupCount; ++index) {
            PreparedShaderGroup group;
            group.Group = index;
            for (uint32_t offset = 0; offset < groupCount; ++offset)
                group.DynamicOffsets.push_back({{}, offset * 256});
            command.Groups.push_back(std::move(group));
        }
        vector<MeshDrawCommand> commands(1);
        commands.front().IndexCount = 9;
        context.AddCommand(std::move(command));
        ASSERT_TRUE(context.HasCommand());
        ASSERT_TRUE(context.AppendCommandTo(commands));
        EXPECT_FALSE(context.HasCommand());
        EXPECT_FALSE(context.AppendCommandTo(commands));
        ASSERT_EQ(commands.size(), 2u);
        EXPECT_EQ(commands.front().IndexCount, 9u);
        EXPECT_EQ(commands.back().IndexCount, 42u);
        EXPECT_EQ(commands.back().FirstIndex, 7u);
        ASSERT_EQ(commands.back().Groups.size(), groupCount);
        for (uint32_t index = 0; index < groupCount; ++index) {
            EXPECT_EQ(commands.back().Groups[index].Group, index);
            ASSERT_EQ(commands.back().Groups[index].DynamicOffsets.size(), groupCount);
            for (uint32_t offset = 0; offset < groupCount; ++offset)
                EXPECT_EQ(commands.back().Groups[index].DynamicOffsets[offset].Offset, offset * 256);
        }
        context.AddCommand(MeshDrawCommand{});
        EXPECT_FALSE(context.HasCommand());
        EXPECT_FALSE(context.AppendCommandTo(commands));
        EXPECT_EQ(context.Reason(), MeshPassRejectReason::ProcessorRejected);
    }
}

TEST(MeshPassDrawListContext, RejectAndDuplicatePublicationDiscardCandidate) {
    vector<MeshDrawCommand> commands;
    MeshPassDrawListContext duplicate;
    duplicate.AddCommand(MeshDrawCommand{});
    duplicate.AddCommand(MeshDrawCommand{});
    EXPECT_FALSE(duplicate.HasCommand());
    EXPECT_FALSE(duplicate.AppendCommandTo(commands));
    MeshPassDrawListContext rejected;
    rejected.AddCommand(MeshDrawCommand{});
    rejected.Reject(MeshPassRejectReason::InvalidBindings);
    EXPECT_FALSE(rejected.AppendCommandTo(commands));
    EXPECT_EQ(rejected.Reason(), MeshPassRejectReason::InvalidBindings);
    rejected.AddCommand(MeshDrawCommand{});
    EXPECT_FALSE(rejected.HasCommand());
    EXPECT_TRUE(commands.empty());
}

class RecordingProcessor final : public MeshPassProcessor {
public:
    uint32_t Calls{0};
    unordered_map<RenderPrimitiveIndex, MeshPassRejectReason> Rejects;
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene, const MeshBatch& batch, MeshPassDrawListContext& out) override {
        ++Calls;
        if (const auto found = Rejects.find(batch.Primitive); found != Rejects.end()) {
            out.Reject(found->second);
            return;
        }
        MeshDrawCommand command;
        command.Program = scene.Materials[batch.Material].FindPass(desc.MaterialPassName)->Program;
        command.IndexCount = batch.IndexCount;
        command.FirstIndex = batch.FirstIndex;
        out.AddCommand(std::move(command));
    }
};

struct ListFixture {
    // Tokens are never dereferenced; the list builder must not query programs or resource state.
    array<byte, 2> ProgramTokens{};
    RenderSceneSnapshot Scene;
    ResolvedRenderView View;
    CullingResults Culling;

    ListFixture() {
        Culling.Scene = &Scene;
        Culling.View = &View;
        Culling.Stats.Valid = true;
        for (uint32_t index = 0; index < 3; ++index) {
            MaterialRenderData material;
            material.Queue = index == 2 ? RenderQueue::Transparent : RenderQueue::Geometry;
            for (const auto name : {"ForwardLit", "DepthOnly"}) {
                MaterialPassRenderData pass;
                pass.PassName = name;
                pass.Program = reinterpret_cast<ShaderProgram*>(&ProgramTokens[index % 2]);
                pass.ProgramFrameId = index % 2;
                pass.Valid = true;
                material.Passes.push_back(std::move(pass));
            }
            Scene.Materials.push_back(std::move(material));
        }
    }
    void Add(uint32_t material, float depth, uint32_t layer = 0xffffffffu) {
        const auto index = static_cast<uint32_t>(Scene.Primitives.size());
        Scene.Primitives.push_back({.LayerMask = layer, .FirstMeshBatch = index, .MeshBatchCount = 1});
        Scene.MeshBatches.push_back({index, material, nullptr, index * 3, 3, 0, 0});
        Culling.Primitives.push_back({index, depth});
    }
    void FillDrawRecords() {
        Scene.DrawRecords.clear();
        Scene.PrimitiveDrawBegin.resize(Scene.Primitives.size() + 1);
        for (uint32_t primitiveIndex = 0; primitiveIndex < Scene.Primitives.size(); ++primitiveIndex) {
            Scene.PrimitiveDrawBegin[primitiveIndex] = static_cast<uint32_t>(Scene.DrawRecords.size());
            const auto& primitive = Scene.Primitives[primitiveIndex];
            for (uint32_t offset = 0; offset < primitive.MeshBatchCount; ++offset) {
                const auto batchIndex = primitive.FirstMeshBatch + offset;
                const auto& batch = Scene.MeshBatches[batchIndex];
                const auto& material = Scene.Materials[batch.Material];
                for (uint32_t passIndex = 0; passIndex < material.Passes.size(); ++passIndex) {
                    const auto& pass = material.Passes[passIndex];
                    DrawRecord record;
                    record.Primitive = primitiveIndex;
                    record.Batch = batchIndex;
                    record.Material = batch.Material;
                    record.PassIndex = passIndex;
                    record.PassNameHash = HashPassName(pass.PassName);
                    record.ProgramFrameId = pass.ProgramFrameId;
                    record.Queue = material.Queue;
                    record.Status = DrawRecordStatus::Ready;
                    Scene.DrawRecords.push_back(record);
                }
            }
        }
        Scene.PrimitiveDrawBegin.back() = static_cast<uint32_t>(Scene.DrawRecords.size());
    }
    RendererListDesc Desc(RenderQueueRange queue = {}, RendererListSorting sorting = RendererListSorting::StateThenFrontToBack) {
        return {"test", "ForwardLit", &Culling, &View, queue, 0xffffffffu, sorting};
    }
};

TEST(RendererList, OneCullingResultBuildsDepthOpaqueTransparent) {
    ListFixture data;
    data.Add(0, 2);
    data.Add(1, 4);
    data.Add(2, 6);
    RecordingProcessor processor;
    RendererList depth, opaque, transparent;
    auto desc = data.Desc(RenderQueueRange::Opaque());
    ASSERT_TRUE(BuildRendererList(desc, processor, opaque));
    desc.MaterialPassName = "DepthOnly";
    ASSERT_TRUE(BuildRendererList(desc, processor, depth));
    desc = data.Desc(RenderQueueRange::Transparent(), RendererListSorting::BackToFront);
    ASSERT_TRUE(BuildRendererList(desc, processor, transparent));
    EXPECT_EQ(opaque.Commands.size(), 2u);
    EXPECT_EQ(depth.Commands.size(), 2u);
    EXPECT_EQ(transparent.Commands.size(), 1u);
    EXPECT_EQ(processor.Calls, 5u);
    EXPECT_EQ(data.Culling.Primitives.size(), 3u);
    EXPECT_EQ(opaque.GetItems()[0].SortData.Primitive, depth.GetItems()[0].SortData.Primitive);
}

TEST(RendererList, AdditionalLayerAndMissingPassHaveNoFallback) {
    ListFixture data;
    data.Add(0, 1, 1);
    data.Add(1, 2, 2);
    data.Scene.Materials[0].Passes.pop_back();
    RecordingProcessor processor;
    RendererList list;
    auto desc = data.Desc();
    desc.MaterialPassName = "DepthOnly";
    desc.LayerMask = 1;
    ASSERT_TRUE(BuildRendererList(desc, processor, list));
    EXPECT_TRUE(list.Commands.empty());
    EXPECT_EQ(list.Stats.LayerRejected, 1u);
    EXPECT_EQ(list.Stats.MissingPass, 1u);
    EXPECT_EQ(processor.Calls, 0u);
    EXPECT_TRUE(list.Stats.ContentSucceeded());
    desc.RequireMaterialPass = true;
    ASSERT_TRUE(BuildRendererList(desc, processor, list));
    EXPECT_EQ(list.Stats.MissingRequiredPass, 1u);
    EXPECT_FALSE(list.Stats.ContentSucceeded());
}

TEST(RendererList, OpaqueOrderUsesFrameIdsAndNotAddresses) {
    ListFixture data;
    data.Add(1, 1);
    data.Add(0, 8);
    data.Add(0, 2);
    data.Add(0, 2);
    RecordingProcessor processor;
    RendererList list;
    const auto desc = data.Desc();
    ASSERT_TRUE(BuildRendererList(desc, processor, list));
    const vector<uint32_t> expected{2, 3, 1, 0};
    vector<uint32_t> actual;
    for (const auto& item : list.GetItems()) actual.push_back(item.SortData.Primitive);
    EXPECT_EQ(actual, expected);
    std::swap(data.Scene.Materials[0].Passes[0].Program, data.Scene.Materials[1].Passes[0].Program);
    ASSERT_TRUE(BuildRendererList(desc, processor, list));
    actual.clear();
    for (const auto& item : list.GetItems()) actual.push_back(item.SortData.Primitive);
    EXPECT_EQ(actual, expected);
}

TEST(RendererList, S07TwelveBatchesMatchIndependentSortForOneHundredAllocationOrders) {
    for (uint32_t repeat = 0; repeat < 100; ++repeat) {
        ListFixture data;
        vector<byte> tokenStorage(128 + repeat);
        for (uint32_t material = 0; material < 3; ++material)
            for (auto& pass : data.Scene.Materials[material].Passes)
                pass.Program = reinterpret_cast<ShaderProgram*>(tokenStorage.data() + (material * 17 + repeat) % tokenStorage.size());
        for (uint32_t i = 0; i < 12; ++i) data.Add(i % 3, float((i / 2) % 3));
        for (const auto sorting : {RendererListSorting::StateThenFrontToBack, RendererListSorting::BackToFront}) {
            vector<uint32_t> expected(12);
            std::iota(expected.begin(), expected.end(), 0);
            const auto key = [&](uint32_t primitive) {
                const auto& batch = data.Scene.MeshBatches[primitive];
                const auto& material = data.Scene.Materials[batch.Material];
                const bool back = sorting == RendererListSorting::BackToFront;
                return std::tuple{static_cast<uint32_t>(material.Queue), back ? 0u : material.Passes[0].ProgramFrameId,
                                  back ? 0u : batch.Material, data.Culling.Primitives[primitive].ViewDepth * (back ? -1 : 1), primitive};
            };
            std::sort(expected.begin(), expected.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
            RendererList list;
            RecordingProcessor processor;
            ASSERT_TRUE(BuildRendererList(data.Desc({}, sorting), processor, list));
            ASSERT_EQ(list.Commands.size(), expected.size());
            for (uint32_t i = 0; i < expected.size(); ++i) {
                EXPECT_EQ(list.GetItems()[i].SortData.Primitive, expected[i]);
                EXPECT_EQ(list.GetCommand(i).FirstIndex, expected[i] * 3);
                EXPECT_EQ(list.GetCommand(i).IndexCount, 3u);
            }
        }
    }
}

TEST(RendererList, TransparentDepthHasDeterministicTiesAndNonFiniteEndpoint) {
    ListFixture data;
    for (float depth : {2.0f, 5.0f, 5.0f, std::numeric_limits<float>::quiet_NaN()}) data.Add(2, depth);
    RecordingProcessor processor;
    RendererList list;
    ASSERT_TRUE(BuildRendererList(data.Desc({}, RendererListSorting::BackToFront), processor, list));
    vector<uint32_t> actual;
    for (const auto& item : list.GetItems()) actual.push_back(item.SortData.Primitive);
    EXPECT_EQ(actual, (vector<uint32_t>{3, 1, 2, 0}));
    EXPECT_EQ(list.Stats.NonFiniteDepth, 1u);
}

TEST(RendererList, StatsClassifyEveryCandidate) {
    ListFixture data;
    data.Add(0, 0, 2);
    data.Add(2, 0);
    data.Add(1, 0);
    data.Add(0, 0);
    data.Add(0, 0);
    data.Add(0, 0);
    data.Add(0, 0);
    data.Scene.Materials[1].Passes.clear();
    RecordingProcessor processor;
    processor.Rejects = {{3, MeshPassRejectReason::InvalidGeometry}, {4, MeshPassRejectReason::InvalidBindings}, {5, MeshPassRejectReason::PrepareResourceFailed}};
    RendererList list;
    auto desc = data.Desc(RenderQueueRange::Opaque());
    desc.LayerMask = 1;
    ASSERT_TRUE(BuildRendererList(desc, processor, list));
    const auto& stats = list.Stats;
    EXPECT_EQ(stats.ConsideredBatches, 7u);
    EXPECT_EQ(stats.ConsideredBatches, stats.LayerRejected + stats.QueueRejected + stats.MissingPass + stats.InvalidGeometry + stats.InvalidBindings + stats.PrepareResourceFailed + stats.ProcessorRejected + stats.Commands);
    EXPECT_EQ(stats.Commands, 1u);
    EXPECT_EQ(stats.InvalidGeometry, 1u);
    EXPECT_EQ(stats.InvalidBindings, 1u);
    EXPECT_EQ(stats.PrepareResourceFailed, 1u);
}

TEST(RendererList, InvalidDescriptorAndBatchRangesPublishEmptyList) {
    ListFixture data;
    data.Add(0, 0);
    RecordingProcessor processor;
    RendererList list;
    auto desc = data.Desc();
    ASSERT_TRUE(BuildRendererList(desc, processor, list));
    const auto capacity = list.Commands.capacity();
    data.Scene.Primitives[0].MeshBatchCount = 2;
    EXPECT_FALSE(BuildRendererList(desc, processor, list));
    EXPECT_FALSE(list.Stats.Valid);
    EXPECT_TRUE(list.Commands.empty());
    EXPECT_EQ(list.Commands.capacity(), capacity);
    EXPECT_TRUE(list.GetItems().empty());
    data.Scene.Primitives[0].MeshBatchCount = 1;
    ResolvedRenderView other;
    desc.View = &other;
    EXPECT_FALSE(BuildRendererList(desc, processor, list));
    desc = data.Desc({2, 1});
    EXPECT_FALSE(BuildRendererList(desc, processor, list));
    desc = data.Desc();
    desc.Culling = nullptr;
    EXPECT_FALSE(BuildRendererList(desc, processor, list));
}

TEST(RendererList, EmptyInputSucceedsWithoutProcessorCalls) {
    ListFixture data;
    RecordingProcessor processor;
    RendererList list;
    ASSERT_TRUE(BuildRendererList(data.Desc(), processor, list));
    EXPECT_TRUE(list.Stats.Valid);
    EXPECT_TRUE(list.Commands.empty());
    EXPECT_EQ(processor.Calls, 0u);
}

TEST(RendererList, SortingKeepsDescriptionsAndFrameBindingsInPublicationOrder) {
    ListFixture data;
    for (float depth : {2.0f, 5.0f, 3.0f}) data.Add(2, depth);
    class GroupProcessor final : public MeshPassProcessor {
    public:
        array<byte, 3> Sets{};
        void AddMeshBatch(const RendererListDesc&, const RenderSceneSnapshot&, const MeshBatch& batch, MeshPassDrawListContext& out) override {
            MeshDrawCommand command;
            command.FirstIndex = batch.FirstIndex;
            // These tokens are only compared; no set or binding is dereferenced by list construction.
            command.Groups.push_back({0, reinterpret_cast<render::ShaderParameterSet*>(&Sets[batch.Primitive]), {}});
            out.AddCommand(std::move(command));
        }
    } processor;
    RendererList list;
    ASSERT_TRUE(BuildRendererList(data.Desc({}, RendererListSorting::BackToFront), processor, list));
    const array<uint32_t, 3> sorted{1, 2, 0};
    for (uint32_t index = 0; index < sorted.size(); ++index) {
        EXPECT_EQ(list.Commands[index].FirstIndex, index * 3);
        EXPECT_EQ(list.Commands[index].Groups[0].Set.Get(), reinterpret_cast<render::ShaderParameterSet*>(&processor.Sets[index]));
        EXPECT_EQ(&list.GetCommand(index), &list.Commands[sorted[index]]);
        EXPECT_EQ(list.GetCommand(index).FirstIndex, sorted[index] * 3);
        EXPECT_EQ(list.GetCommand(index).Groups[0].Set.Get(), reinterpret_cast<render::ShaderParameterSet*>(&processor.Sets[sorted[index]]));
    }
    list.ResetForReuse();
    EXPECT_TRUE(list.GetItems().empty());
    MeshDrawCommand manual;
    manual.FirstIndex = 99;
    list.Commands.push_back(std::move(manual));
    EXPECT_EQ(list.GetCommand(0).FirstIndex, 99u);
}

void ExpectSameList(const RendererList& left, const RendererList& right) {
    EXPECT_EQ(left.Commands.size(), right.Commands.size());
    EXPECT_EQ(left.Items.size(), right.Items.size());
    EXPECT_EQ(left.Stats.Commands, right.Stats.Commands);
    EXPECT_EQ(left.Stats.ConsideredBatches, right.Stats.ConsideredBatches);
    EXPECT_EQ(left.Stats.QueueRejected, right.Stats.QueueRejected);
    EXPECT_EQ(left.Stats.MissingPass, right.Stats.MissingPass);
    EXPECT_EQ(left.Stats.LayerRejected, right.Stats.LayerRejected);
    for (size_t index = 0; index < left.GetItems().size(); ++index) {
        EXPECT_EQ(left.GetItems()[index].SortData.Primitive, right.GetItems()[index].SortData.Primitive);
        EXPECT_EQ(left.GetCommand(index).FirstIndex, right.GetCommand(index).FirstIndex);
    }
}

TEST(RendererList, SharedCullingBuildsDepthOpaqueTransparentTogether) {
    ListFixture data;
    data.Add(0, 2);
    data.Add(1, 4);
    data.Add(2, 6);
    const RendererListDesc descs[] = {
        {"depth", "DepthOnly", &data.Culling, &data.View, RenderQueueRange::Opaque(), 0xffffffffu, RendererListSorting::FrontToBack},
        {"opaque", "ForwardLit", &data.Culling, &data.View, RenderQueueRange::Opaque()},
        {"transparent", "ForwardLit", &data.Culling, &data.View, RenderQueueRange::Transparent(), 0xffffffffu, RendererListSorting::BackToFront},
    };
    RecordingProcessor sequential;
    RendererList depth, opaque, transparent;
    ASSERT_TRUE(BuildRendererList(descs[0], sequential, depth));
    ASSERT_TRUE(BuildRendererList(descs[1], sequential, opaque));
    ASSERT_TRUE(BuildRendererList(descs[2], sequential, transparent));
    RecordingProcessor together;
    RendererList sharedDepth, sharedOpaque, sharedTransparent;
    RendererList* outs[] = {&sharedDepth, &sharedOpaque, &sharedTransparent};
    ASSERT_TRUE(BuildRendererLists(descs, together, outs));
    ExpectSameList(depth, sharedDepth);
    ExpectSameList(opaque, sharedOpaque);
    ExpectSameList(transparent, sharedTransparent);
    EXPECT_EQ(sequential.Calls, together.Calls);
}

TEST(RendererList, DrawRecordsSharedCullingMatchesSequential) {
    ListFixture data;
    data.Add(0, 2);
    data.Add(1, 4);
    data.Add(2, 6);
    data.FillDrawRecords();
    const RendererListDesc descs[] = {
        {"depth", "DepthOnly", &data.Culling, &data.View, RenderQueueRange::Opaque(), 0xffffffffu, RendererListSorting::FrontToBack},
        {"opaque", "ForwardLit", &data.Culling, &data.View, RenderQueueRange::Opaque()},
        {"transparent", "ForwardLit", &data.Culling, &data.View, RenderQueueRange::Transparent(), 0xffffffffu, RendererListSorting::BackToFront},
    };
    RecordingProcessor sequential;
    RendererList depth, opaque, transparent;
    ASSERT_TRUE(BuildRendererList(descs[0], sequential, depth));
    ASSERT_TRUE(BuildRendererList(descs[1], sequential, opaque));
    ASSERT_TRUE(BuildRendererList(descs[2], sequential, transparent));
    RecordingProcessor together;
    RendererList sharedDepth, sharedOpaque, sharedTransparent;
    RendererList* outs[] = {&sharedDepth, &sharedOpaque, &sharedTransparent};
    ASSERT_TRUE(BuildRendererLists(descs, together, outs));
    ExpectSameList(depth, sharedDepth);
    ExpectSameList(opaque, sharedOpaque);
    ExpectSameList(transparent, sharedTransparent);
    EXPECT_EQ(sequential.Calls, together.Calls);
}

TEST(RendererList, DrawRecordsSkipMeshBatchRangeValidation) {
    ListFixture data;
    data.Add(0, 0);
    data.FillDrawRecords();
    data.Scene.Primitives[0].MeshBatchCount = 2;
    RecordingProcessor processor;
    RendererList list;
    ASSERT_TRUE(BuildRendererList(data.Desc(), processor, list));
    EXPECT_EQ(list.Commands.size(), 1u);
}

TEST(RendererList, SharedListCountMustMatchOutputs) {
    ListFixture data;
    data.Add(0, 0);
    RecordingProcessor processor;
    RendererList list;
    const RendererListDesc descs[] = {data.Desc(), data.Desc()};
    RendererList* outs[] = {&list};
    EXPECT_FALSE(BuildRendererLists(descs, processor, outs));
}

}  // namespace
}  // namespace radray
