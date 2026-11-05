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
    Pipeline->OnRecord = [](FrameSlot& slot) {
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
    Pipeline->OnRecord = [&](FrameSlot& slot) {
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
    Pipeline->OnRecord = [](FrameSlot& slot) { EXPECT_FALSE(slot.AcquireCommandBuffer()); };
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

TEST(MeshPassFrameDrawListTest, MovedListKeepsDynamicContextBoundToItself) {
    vector<MeshPassFrameDrawList> lists(1);
    lists.resize(16);
    const PrimitiveSceneInfo primitive;
    MeshBatch batch;
    batch.Elements.emplace_back();
    auto& list = lists[0];
    auto& command = list.DynamicContext.AddCommand({});
    list.DynamicContext.FinalizeCommand({primitive, batch}, 0, command);
    EXPECT_EQ(list.VisibleCommands.size(), 1u);
}

TEST(MeshPassFrameDrawListTest, ClearEmptiesVisibleAndDynamicCommands) {
    MeshPassFrameDrawList list;
    list.VisibleCommands.emplace_back();
    list.DynamicContext.Commands.emplace_back();
    list.Clear();
    EXPECT_TRUE(list.VisibleCommands.empty());
    EXPECT_TRUE(list.DynamicContext.Commands.empty());
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
    list.VisibleCommands.push_back({.Source = MeshDrawCommandSource::Cached, .CommandHandle = handle});
    command.FirstIndex = 3;
    list.DynamicContext.Commands.push_back(command);
    list.VisibleCommands.push_back({.Source = MeshDrawCommandSource::Dynamic, .CommandHandle = {0, 0}});
    RecordingEncoder encoder;
    ASSERT_TRUE(list.Submit(encoder, &cached));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 0u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 3u);
}

}  // namespace
}  // namespace radray
