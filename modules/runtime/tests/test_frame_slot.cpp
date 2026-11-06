#include <gtest/gtest.h>

#include <radray/runtime/frame_scheduler.h>

#include "test_fakes.h"

namespace radray {
namespace {

using test::FakeBuffer;
using test::FakePipeline;
using test::RecordingEncoder;

class FrameSlotTest : public testing::Test {
protected:
    unique_ptr<FrameScheduler> Create(uint32_t slotCount) { return test::CreateScheduler(Gpu.Context, Pipeline, slotCount); }

    test::FakeGpu Gpu;
    test::CallbackPipeline* Pipeline{nullptr};
    Scene Source;
};

TEST_F(FrameSlotTest, AcquireReturnsDistinctBuffersWithinFrame) {
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    Pipeline->OnRecord = [](const Scene&, FrameSlot& slot) {
        const auto first = slot.AcquireCommandBuffer();
        const auto second = slot.AcquireCommandBuffer();
        ASSERT_TRUE(first && second);
        EXPECT_NE(first.Get(), second.Get());
    };
    scheduler->RunFrame(Source);
}

TEST_F(FrameSlotTest, AcquireReusesBuffersAfterSlotReset) {
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    const array<size_t, 3> counts{2, 1, 2};
    vector<vector<render::CommandBuffer*>> frames;
    Pipeline->OnRecord = [&](const Scene&, FrameSlot& slot) {
        auto& acquired = frames.emplace_back();
        for (size_t index = 0; index < counts[frames.size() - 1]; ++index) acquired.push_back(slot.AcquireCommandBuffer().Get());
    };
    for (size_t frame = 0; frame < counts.size(); ++frame) scheduler->RunFrame(Source);
    EXPECT_EQ(frames[1][0], frames[0][0]);
    EXPECT_EQ(frames[2], frames[0]);
}

TEST_F(FrameSlotTest, AcquireReturnsNullWhenBufferCreationFails) {
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    Gpu.Device->FailCommandBuffers = true;
    Pipeline->OnRecord = [](const Scene&, FrameSlot& slot) { EXPECT_FALSE(slot.AcquireCommandBuffer()); };
    scheduler->RunFrame(Source);
}

VisibleMeshDrawCommand Keyed(uint64_t sortKey, uint32_t firstInstance) {
    return {.SortKey = sortKey, .FirstInstance = firstInstance};
}

TEST(MeshPassFrameDrawListTest, SortOrdersBySortKeyAndKeepsTies) {
    MeshPassFrameDrawList list;
    list.VisibleCommands = {Keyed(2, 0), Keyed(1, 1), Keyed(2, 2), Keyed(1, 3)};
    list.SortVisibleCommands();
    vector<uint32_t> order;
    for (const auto& visible : list.VisibleCommands) order.push_back(visible.FirstInstance);
    EXPECT_EQ(order, (vector<uint32_t>{1, 3, 0, 2}));
}

TEST(MeshPassFrameDrawListTest, SortPublishesDynamicPointersAfterCommandStorageGrows) {
    MeshPassFrameDrawList list;
    list.DynamicContext.Commands.reserve(1);
    const PrimitiveSceneInfo primitive;
    MeshBatch batch;
    batch.Elements.emplace_back();
    const MeshDrawCommand cached{};
    for (uint32_t index = 0; index < 64; ++index) {
        MeshDrawCommand initializer;
        initializer.BaseSortKey = 63 - index;
        auto& command = list.DynamicContext.AddCommand(initializer);
        list.DynamicContext.FinalizeCommand({primitive, batch}, 0, command);
        if (index == 31) list.VisibleCommands.push_back({.CachedCommand = &cached, .SortKey = 32});
    }

    list.SortVisibleCommands();
    list.SortVisibleCommands();
    ASSERT_EQ(list.VisibleCommands.size(), 65u);
    EXPECT_EQ(list.VisibleCommands[0].CachedCommand.Get(), &list.DynamicContext.Commands[63]);
    EXPECT_EQ(list.VisibleCommands[32].CachedCommand.Get(), &list.DynamicContext.Commands[31]);
    EXPECT_EQ(list.VisibleCommands[33].CachedCommand.Get(), &cached);
    EXPECT_EQ(list.VisibleCommands[64].CachedCommand.Get(), &list.DynamicContext.Commands[0]);
}

TEST(MeshPassFrameDrawListTest, MovedListKeepsDynamicContextBoundToItself) {
    vector<MeshPassFrameDrawList> lists(1);
    const MeshDrawCommand cached{};
    lists[0].VisibleCommands.push_back({.CachedCommand = &cached, .SortKey = 42});
    const PrimitiveSceneInfo primitive;
    MeshBatch batch;
    batch.Elements.emplace_back();
    auto& first = lists[0].DynamicContext.AddCommand({});
    lists[0].DynamicContext.FinalizeCommand({primitive, batch}, 0, first);
    lists.resize(16);
    auto& list = lists[0];
    auto& command = list.DynamicContext.AddCommand({});
    list.DynamicContext.FinalizeCommand({primitive, batch}, 0, command);
    ASSERT_EQ(list.VisibleCommands.size(), 3u);
    EXPECT_EQ(list.VisibleCommands[0].CachedCommand.Get(), &cached);
    EXPECT_EQ(list.VisibleCommands[0].SortKey, 42u);
    list.DynamicContext.FinalizeVisibleCommands();
    EXPECT_EQ(list.VisibleCommands[1].CachedCommand.Get(), &list.DynamicContext.Commands[0]);
    EXPECT_EQ(list.VisibleCommands[2].CachedCommand.Get(), &command);
}

TEST(MeshPassFrameDrawListTest, MoveAssignmentKeepsBorrowingAndRebindsDynamicContext) {
    MeshPassFrameDrawList source;
    const MeshDrawCommand cached{};
    source.VisibleCommands.push_back({.CachedCommand = &cached, .SortKey = 42});
    const PrimitiveSceneInfo primitive;
    MeshBatch batch;
    batch.Elements.emplace_back();
    auto& first = source.DynamicContext.AddCommand({});
    source.DynamicContext.FinalizeCommand({primitive, batch}, 0, first);
    MeshPassFrameDrawList target;
    target = std::move(source);
    auto& command = target.DynamicContext.AddCommand({});
    target.DynamicContext.FinalizeCommand({primitive, batch}, 0, command);
    ASSERT_EQ(target.VisibleCommands.size(), 3u);
    EXPECT_EQ(target.VisibleCommands[0].CachedCommand.Get(), &cached);
    EXPECT_EQ(target.VisibleCommands[0].SortKey, 42u);
    target.DynamicContext.FinalizeVisibleCommands();
    EXPECT_EQ(target.VisibleCommands[1].CachedCommand.Get(), &target.DynamicContext.Commands[0]);
    EXPECT_EQ(target.VisibleCommands[2].CachedCommand.Get(), &command);
}

TEST(MeshPassFrameDrawListTest, ClearEmptiesAllCommandsAndKeepsCapacity) {
    MeshPassFrameDrawList list;
    list.VisibleCommands.emplace_back();
    list.DynamicContext.Commands.emplace_back();
    const auto visibleCapacity = list.VisibleCommands.capacity();
    const auto dynamicCapacity = list.DynamicContext.Commands.capacity();
    list.Clear();
    EXPECT_TRUE(list.VisibleCommands.empty());
    EXPECT_TRUE(list.DynamicContext.Commands.empty());
    EXPECT_EQ(list.VisibleCommands.capacity(), visibleCapacity);
    EXPECT_EQ(list.DynamicContext.Commands.capacity(), dynamicCapacity);
}

TEST(MeshPassFrameDrawListTest, SubmitDrawsCachedAndDynamicCommands) {
    FakePipeline pipeline;
    FakeBuffer vertices{128, render::BufferUse::Vertex};
    FakeBuffer indices{128, render::BufferUse::Index};
    MeshDrawCommand command;
    command.Pipeline = &pipeline;
    command.VertexBuffers.push_back({0, {&vertices, 0, 48}});
    command.IndexBuffer = {&indices, 0, 4};
    command.IndexCount = 3;
    CachedPassMeshDrawList cached;
    const auto handle = cached.Commands.Emplace(command);
    MeshPassFrameDrawList list;
    list.VisibleCommands.push_back({.Source = MeshDrawCommandSource::Cached, .CachedCommand = &cached.Commands.Get(handle)});
    command.FirstIndex = 3;
    list.DynamicContext.Commands.push_back(command);
    list.VisibleCommands.push_back({.Source = MeshDrawCommandSource::Dynamic, .CachedCommand = &list.DynamicContext.Commands[0]});
    RecordingEncoder encoder;
    ASSERT_TRUE(list.Submit(encoder));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 0u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 3u);
}

TEST(MeshPassFrameDrawListTest, SubmitBorrowedAndDynamicCommandsWithPassBindings) {
    FakePipeline pipeline;
    FakeBuffer vertices{128, render::BufferUse::Vertex};
    FakeBuffer instances{128, render::BufferUse::Vertex};
    FakeBuffer indices{128, render::BufferUse::Index};
    test::FakeParameterSet parameters;
    MeshDrawCommand command;
    command.Pipeline = &pipeline;
    command.VertexBuffers.push_back({0, {&vertices, 0, 48}});
    command.InstanceStreamBinding = 1;
    command.IndexBuffer = {&indices, 0, 4};
    command.IndexCount = 3;
    MeshPassFrameDrawList list;
    const auto cached = command;
    list.VisibleCommands.push_back({.Source = MeshDrawCommandSource::Cached, .CachedCommand = &cached, .FirstInstance = 7, .InstanceCount = 2});
    command.FirstIndex = 3;
    list.DynamicContext.Commands.push_back(command);
    list.VisibleCommands.push_back({.Source = MeshDrawCommandSource::Dynamic, .CachedCommand = &list.DynamicContext.Commands[0], .FirstInstance = 9});
    const MeshShaderParameterBinding passBinding{0, &parameters, {}};
    const MeshPassSubmitArgs args{
        .ParameterSets = std::span{&passBinding, 1},
        .InstanceStream = render::VertexBufferBinding{1, {&instances, 0, 128}}};
    RecordingEncoder encoder;
    ASSERT_TRUE(list.Submit(encoder, args));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 0u);
    EXPECT_EQ(encoder.Draws[0].FirstInstance, 7u);
    EXPECT_EQ(encoder.Draws[0].InstanceCount, 2u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 3u);
    EXPECT_EQ(encoder.Draws[1].FirstInstance, 9u);
    ASSERT_EQ(encoder.ParameterBinds.size(), 1u);
    EXPECT_EQ(encoder.ParameterBinds[0].Set, &parameters);
    ASSERT_EQ(encoder.VertexCalls.size(), 1u);
    ASSERT_EQ(encoder.VertexCalls[0].size(), 2u);
    EXPECT_EQ(encoder.VertexCalls[0][1].View.Target, &instances);
}

TEST(MeshPassFrameDrawListTest, SubmitFinalizesDynamicPointersWithoutSortingAndClearAllowsNextFrame) {
    FakePipeline pipeline;
    FakeBuffer indices{1024, render::BufferUse::Index};
    const PrimitiveSceneInfo primitive;
    MeshBatch batch;
    batch.Elements.push_back({.IndexBuffer = &indices, .IndexCount = 3});
    MeshDrawCommand initializer;
    initializer.Pipeline = &pipeline;
    MeshPassFrameDrawList list;
    list.DynamicContext.Commands.reserve(1);
    for (uint32_t frame = 0; frame < 2; ++frame) {
        for (uint32_t index = 0; index < 64; ++index) {
            batch.Elements[0].FirstIndex = index;
            auto& command = list.DynamicContext.AddCommand(initializer);
            list.DynamicContext.FinalizeCommand({primitive, batch}, 0, command);
            list.VisibleCommands.back().FirstInstance = frame + index;
            list.VisibleCommands.back().InstanceCount = 2;
        }
        RecordingEncoder encoder;
        ASSERT_TRUE(list.Submit(encoder));
        ASSERT_EQ(encoder.Draws.size(), 64u);
        for (uint32_t index = 0; index < encoder.Draws.size(); ++index) {
            EXPECT_EQ(encoder.Draws[index].FirstIndex, index);
            EXPECT_EQ(encoder.Draws[index].FirstInstance, frame + index);
            EXPECT_EQ(encoder.Draws[index].InstanceCount, 2u);
        }
        list.Clear();
    }
}

}  // namespace
}  // namespace radray
