#include <gtest/gtest.h>

#include <utility>

#include <fmt/format.h>
#include <radray/scope_guard.h>
#include <radray/runtime/frame_scheduler.h>

#include "test_fakes.h"

namespace radray {
namespace {

using test::CallbackPipeline;

class CountingProcessor final : public MeshPassProcessor {
public:
    using MeshPassProcessor::MeshPassProcessor;
    void AddMeshBatch(const MeshBatchContext& context) override { Calls.push_back(context.BatchHandle); }

    vector<SparseSetHandle> Calls;
};

class FrameSchedulerTest : public testing::Test {
protected:
    unique_ptr<FrameScheduler> Create(uint32_t slotCount = 2) { return test::CreateScheduler(Gpu.Context, Pipeline, slotCount); }

    // Logs "<stage><slot>:<frame>" for every pipeline stage.
    void LogStages() {
        Pipeline->OnExtract = [this](const Scene&, FrameSlot& slot) { Log.push_back(fmt::format("E{}:{}", slot.GetIndex(), slot.GetFrameId())); };
        Pipeline->OnRecord = [this](const Scene&, FrameSlot& slot) { Log.push_back(fmt::format("R{}:{}", slot.GetIndex(), slot.GetFrameId())); };
        Pipeline->OnWait = [this](FrameSlot& slot) { Log.push_back(fmt::format("W{}:{}", slot.GetIndex(), slot.GetFrameId())); };
    }
    // Registers an arena-owned object that logs `text` when the slot reclaims it.
    void RegisterCleanup(FrameSlot& slot, string text) {
        auto cleanup = MakeScopeGuard([this, text = std::move(text)]() noexcept { Log.push_back(text); });
        slot.GetArena().Emplace<decltype(cleanup)>(std::move(cleanup));
    }

    test::FakeGpu Gpu;
    CallbackPipeline* Pipeline{nullptr};
    Scene Source;
    vector<string> Log;
};

TEST_F(FrameSchedulerTest, CreateRejectsZeroSlots) {
    EXPECT_FALSE(Create(0));
}

TEST_F(FrameSchedulerTest, CreateRejectsNullPipeline) {
    EXPECT_FALSE(FrameScheduler::Create(Gpu.Context, nullptr));
}

TEST_F(FrameSchedulerTest, DefaultsToTwoSlots) {
    auto scheduler = FrameScheduler::Create(Gpu.Context, make_unique<CallbackPipeline>());
    ASSERT_TRUE(scheduler);
    EXPECT_EQ(scheduler.Get()->GetSlotCount(), 2u);
}

TEST_F(FrameSchedulerTest, FramesRotateThroughSlotsAndWaitBeforeReuse) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    LogStages();
    for (int frame = 0; frame < 3; ++frame) scheduler->RunFrame(Source);
    EXPECT_EQ(Log, (vector<string>{"E0:0", "R0:0", "E1:1", "R1:1", "W0:0", "E0:2", "R0:2"}));
}

TEST_F(FrameSchedulerTest, WaitIdleWaitsEachInFlightSlotOnce) {
    auto scheduler = Create(3);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    LogStages();
    scheduler->WaitIdle();
    EXPECT_EQ(Log, (vector<string>{"W0:0", "W1:1"}));
    Log.clear();
    scheduler->WaitIdle();
    EXPECT_TRUE(Log.empty());
}

TEST_F(FrameSchedulerTest, WaitIdleKeepsNextSlotAndFrameNumber) {
    auto scheduler = Create(3);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    scheduler->WaitIdle();
    LogStages();
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    EXPECT_EQ(Log, (vector<string>{"E2:2", "R2:2", "E0:3", "R0:3"}));
}

TEST_F(FrameSchedulerTest, MutableSceneSyncsDirtyBatchesBeforeExtract) {
    DynamicMeshPassDrawListContext output;
    CountingProcessor processor{output};
    MeshBatch batch;
    batch.Material = Source.AddMaterial({});
    const auto handle = Source.AddMeshBatch(Source.AddPrimitive(), std::move(batch));
    ASSERT_TRUE(handle.IsValid());
    auto scheduler = Create();
    ASSERT_TRUE(scheduler);
    Pipeline->Passes = {{{0}, &processor}};
    Pipeline->OnExtract = [&](const Scene& source, FrameSlot&) {
        EXPECT_FALSE(source.HasDirtyBatches());
        EXPECT_EQ(processor.Calls, (vector<SparseSetHandle>{handle}));
    };
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    EXPECT_EQ(processor.Calls.size(), 1u);
}

TEST_F(FrameSchedulerTest, ConstSceneIsNotSynchronized) {
    DynamicMeshPassDrawListContext output;
    CountingProcessor processor{output};
    MeshBatch batch;
    batch.Material = Source.AddMaterial({});
    ASSERT_TRUE(Source.AddMeshBatch(Source.AddPrimitive(), std::move(batch)).IsValid());
    auto scheduler = Create();
    ASSERT_TRUE(scheduler);
    Pipeline->Passes = {{{0}, &processor}};
    scheduler->RunFrame(std::as_const(Source));
    EXPECT_TRUE(processor.Calls.empty());
    EXPECT_TRUE(Source.HasDirtyBatches());
}

TEST_F(FrameSchedulerTest, ExtractAndRecordBorrowTheCurrentSceneAcrossSlots) {
    Scene other;
    Source.AddPrimitive();
    other.AddPrimitive();
    other.AddPrimitive();
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    const array<const Scene*, 3> sources{&Source, &other, &Source};
    vector<uint32_t> counts;
    Pipeline->OnExtract = [&](const Scene& source, FrameSlot& slot) {
        EXPECT_EQ(&source, sources[slot.GetFrameId()]);
    };
    Pipeline->OnRecord = [&](const Scene& source, FrameSlot& slot) {
        EXPECT_EQ(&source, sources[slot.GetFrameId()]);
        counts.push_back(source.Primitives.Count());
    };
    scheduler->RunFrame(Source);
    scheduler->RunFrame(std::as_const(other));
    Source.AddPrimitive();
    scheduler->RunFrame(Source);
    EXPECT_EQ(counts, (vector<uint32_t>{1, 2, 2}));
    Source.Clear();
    other.Clear();
    scheduler->WaitIdle();
}

TEST_F(FrameSchedulerTest, ArenaObjectsLiveUntilTheirSlotIsWaited) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    Pipeline->OnExtract = [&](const Scene&, FrameSlot& slot) { RegisterCleanup(slot, fmt::format("release {}", slot.GetFrameId())); };
    Pipeline->OnWait = [&](FrameSlot& slot) { Log.push_back(fmt::format("wait {}", slot.GetFrameId())); };
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    EXPECT_TRUE(Log.empty());
    scheduler->RunFrame(Source);
    EXPECT_EQ(Log, (vector<string>{"wait 0", "release 0"}));
}

TEST_F(FrameSchedulerTest, DrawListsAreEmptyWhenSlotIsReused) {
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    Pipeline->OnExtract = [](const Scene&, FrameSlot& slot) {
        auto& list = slot.GetPassDrawList({0});
        EXPECT_TRUE(list.VisibleCommands.empty());
        EXPECT_TRUE(list.DynamicContext.Commands.empty());
        list.VisibleCommands.emplace_back();
        list.DynamicContext.Commands.emplace_back();
    };
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
}

TEST_F(FrameSchedulerTest, RetiredOwnerIsReleasedWhenItsSlotIsReused) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    bool released = false;
    Pipeline->OnRecord = [&](const Scene&, FrameSlot& slot) {
        if (slot.GetFrameId() == 0) slot.RetireResource(MakeScopeGuard([&]() noexcept { released = true; }));
    };
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    EXPECT_FALSE(released);
    scheduler->RunFrame(Source);
    EXPECT_TRUE(released);
}

TEST_F(FrameSchedulerTest, WaitIdleReleasesRetiredOwners) {
    auto scheduler = Create(3);
    ASSERT_TRUE(scheduler);
    bool released = false;
    Pipeline->OnExtract = [&](const Scene&, FrameSlot& slot) {
        slot.RetireResource(MakeScopeGuard([&]() noexcept { released = true; }));
    };
    scheduler->RunFrame(Source);
    scheduler->WaitIdle();
    EXPECT_TRUE(released);
}

TEST_F(FrameSchedulerTest, RetireResourceRejectsWaitCallback) {
#if RADRAY_IS_DEBUG
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame(Source);
    EXPECT_DEATH({
        Pipeline->OnWait = [](FrameSlot& slot) { slot.RetireResource(unique_ptr<int>{}); };
        scheduler->WaitIdle(); }, "");
#else
    GTEST_SKIP() << "Release retirement trusts the callback precondition";
#endif
}

TEST_F(FrameSchedulerTest, RetireResourceRejectsCallsOutsideFrame) {
#if RADRAY_IS_DEBUG
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    Nullable<FrameSlot*> used{nullptr};
    Pipeline->OnExtract = [&](const Scene&, FrameSlot& slot) { used = &slot; };
    scheduler->RunFrame(Source);
    ASSERT_TRUE(used);
    EXPECT_DEATH(used->RetireResource(unique_ptr<int>{}), "");
#else
    GTEST_SKIP() << "Release retirement trusts the callback precondition";
#endif
}

TEST_F(FrameSchedulerTest, WaitIdleResetsEveryAllocatorBeforeReleasingAnyResource) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    Pipeline->OnExtract = [&](const Scene&, FrameSlot& slot) {
        RegisterCleanup(slot, "release frame object");
        slot.RetireResource(MakeScopeGuard([&]() noexcept { Log.push_back("release retired"); }));
    };
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    Gpu.Device->OnAllocatorReset = [&] { Log.push_back("reset"); };
    scheduler->WaitIdle();
    EXPECT_EQ(Log, (vector<string>{"reset", "reset", "release retired", "release retired", "release frame object", "release frame object"}));
}

TEST_F(FrameSchedulerTest, DestructionWaitsSlotsBeforeDestroyingPipeline) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    // Only the pipeline's callback owns the marker, so its deleter runs when the pipeline is destroyed.
    Pipeline->OnWait = [this, marker = shared_ptr<int>{nullptr, [this](int*) { Log.push_back("pipeline destroyed"); }}](FrameSlot& slot) {
        Log.push_back(fmt::format("wait {}", slot.GetIndex()));
    };
    scheduler.reset();
    EXPECT_EQ(Log, (vector<string>{"wait 0", "wait 1", "pipeline destroyed"}));
}

}  // namespace
}  // namespace radray
