#include <gtest/gtest.h>

#include <utility>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/scene.h>

#include "test_fakes.h"

namespace radray {
namespace {

using test::CallbackPipeline;

class CountingProcessor final : public MeshPassProcessor {
public:
    void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext&) override { Calls.push_back(context.BatchHandle); }

    vector<MeshBatchHandle> Calls;
};

class FrameSchedulerTest : public testing::Test {
protected:
    unique_ptr<FrameScheduler> Create(uint32_t slotCount = 2) { return test::CreateScheduler(Gpu.Context, Pipeline, slotCount); }

    // 为每个流水线阶段记录 "<stage><slot>:<frame>" 格式的日志。
    void LogStages() {
        Pipeline->OnExtract = [this](FrameSlot& slot) { Log.emplace_back(radray::format("E{}:{}", slot.GetIndex(), slot.GetFrameId())); };
        Pipeline->OnRecord = [this](FrameSlot& slot) { Log.emplace_back(radray::format("R{}:{}", slot.GetIndex(), slot.GetFrameId())); };
        Pipeline->OnWait = [this](FrameSlot& slot) { Log.emplace_back(radray::format("W{}:{}", slot.GetIndex(), slot.GetFrameId())); };
    }
    // 登记一个由 arena 拥有的对象，槽位回收它时记录 `text`。
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
    for (int frame = 0; frame < 3; ++frame) scheduler->RunFrame();
    EXPECT_EQ(Log, (vector<string>{"E0:0", "R0:0", "E1:1", "R1:1", "W0:0", "E0:2", "R0:2"}));
}

TEST_F(FrameSchedulerTest, BeginFrameReclaimsBeforePreparationAndIsIdempotent) {
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    Pipeline->OnExtract = [&](FrameSlot& slot) { RegisterCleanup(slot, "release"); };
    scheduler->RunFrame();
    Pipeline->OnWait = [&](FrameSlot&) { Log.push_back("wait"); };
    Gpu.Device->OnAllocatorReset = [&] { Log.push_back("reset"); };
    EXPECT_FALSE(scheduler->GetCurrentSlot());
    auto& slot = scheduler->BeginFrame();
    EXPECT_EQ(Log, (vector<string>{"wait", "reset", "release"}));
    EXPECT_EQ(slot.GetFrameId(), 1u);
    EXPECT_EQ(scheduler->GetCurrentSlot().Get(), &slot);
    auto* value = slot.GetArena().Emplace<int>(42);
    bool released = false;
    slot.RetireResource(MakeScopeGuard([&]() noexcept { released = true; }));
    Log.clear();
    EXPECT_EQ(&scheduler->BeginFrame(), &slot);
    EXPECT_TRUE(Log.empty());
    EXPECT_EQ(*value, 42);
    EXPECT_FALSE(released);
    slot.RetireResource(unique_ptr<int>{});
    scheduler->RunFrame();
    EXPECT_TRUE(Log.empty());
    EXPECT_FALSE(released);
    EXPECT_FALSE(scheduler->GetCurrentSlot());
    scheduler->WaitIdle();
    EXPECT_TRUE(released);
}

TEST_F(FrameSchedulerTest, PreparationRetirementSurvivesRepeatedBeginAndFullRotation) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame();
    bool released = false;
    auto& slot = scheduler->BeginFrame();
    slot.RetireResource(MakeScopeGuard([&]() noexcept { released = true; }));
    scheduler->BeginFrame();
    scheduler->RunFrame();
    scheduler->BeginFrame();
    EXPECT_FALSE(released);
    scheduler->RunFrame();
    scheduler->BeginFrame();
    EXPECT_TRUE(released);
}

TEST_F(FrameSchedulerTest, WaitIdleCancelsPreparedFrameWithoutAdvancing) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    auto& slot = scheduler->BeginFrame();
    RegisterCleanup(slot, "release");
    scheduler->WaitIdle();
    EXPECT_FALSE(scheduler->GetCurrentSlot());
    EXPECT_EQ(Log, (vector<string>{"release"}));
    EXPECT_EQ(&scheduler->BeginFrame(), &slot);
    EXPECT_EQ(slot.GetFrameId(), 0u);
}

TEST_F(FrameSchedulerTest, WaitIdleWaitsEachInFlightSlotOnce) {
    auto scheduler = Create(3);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame();
    scheduler->RunFrame();
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
    scheduler->RunFrame();
    scheduler->RunFrame();
    scheduler->WaitIdle();
    LogStages();
    scheduler->RunFrame();
    scheduler->RunFrame();
    EXPECT_EQ(Log, (vector<string>{"E2:2", "R2:2", "E0:3", "R0:3"}));
}

TEST_F(FrameSchedulerTest, CallerExplicitlySynchronizesCurrentRegistrations) {
    CountingProcessor processor;
    MeshBatch batch;
    batch.Material = Source.AddMaterial({});
    const auto handle = Source.AddMeshBatch(Source.AddPrimitive(), std::move(batch));
    ASSERT_TRUE(handle.IsValid());
    auto scheduler = Create();
    ASSERT_TRUE(scheduler);
    Pipeline->Passes = {{{0}, &processor}};
    Pipeline->OnExtract = [&](FrameSlot&) {
        EXPECT_FALSE(Source.HasDirtyBatches());
        EXPECT_EQ(processor.Calls, (vector<MeshBatchHandle>{handle}));
    };
    Source.SyncCachedMeshDrawCommands(Pipeline->GetMeshPassRegistrations());
    scheduler->RunFrame();
    Source.SyncCachedMeshDrawCommands(Pipeline->GetMeshPassRegistrations());
    scheduler->RunFrame();
    EXPECT_EQ(processor.Calls.size(), 1u);
}

TEST_F(FrameSchedulerTest, SchedulerDoesNotSynchronizeScene) {
    CountingProcessor processor;
    MeshBatch batch;
    batch.Material = Source.AddMaterial({});
    ASSERT_TRUE(Source.AddMeshBatch(Source.AddPrimitive(), std::move(batch)).IsValid());
    auto scheduler = Create();
    ASSERT_TRUE(scheduler);
    Pipeline->Passes = {{{0}, &processor}};
    scheduler->RunFrame();
    EXPECT_TRUE(processor.Calls.empty());
    EXPECT_TRUE(Source.HasDirtyBatches());
}

TEST_F(FrameSchedulerTest, CurrentRegistrationsRebuildCleanSceneCaches) {
    CountingProcessor first, replacement;
    MeshBatch batch;
    batch.Material = Source.AddMaterial({});
    const auto handle = Source.AddMeshBatch(Source.AddPrimitive(), std::move(batch));
    auto scheduler = Create();
    ASSERT_TRUE(scheduler);
    Pipeline->Passes = {{{0}, &first}};
    Source.SyncCachedMeshDrawCommands(Pipeline->GetMeshPassRegistrations());
    scheduler->RunFrame();
    ASSERT_FALSE(Source.HasDirtyBatches());
    Pipeline->Passes = {{{1}, &replacement, MeshPassSortMode::DepthFirst}};
    Source.SyncCachedMeshDrawCommands(Pipeline->GetMeshPassRegistrations());
    scheduler->RunFrame();
    EXPECT_EQ(replacement.Calls, (vector<MeshBatchHandle>{handle}));
    EXPECT_FALSE(Source.GetPassCache({0}));
    ASSERT_TRUE(Source.GetPassCache({1}));
    EXPECT_EQ(Source.GetPassCache({1})->GetSortMode(), MeshPassSortMode::DepthFirst);
    Pipeline->Passes = {{{1}, &first}};
    Source.SyncCachedMeshDrawCommands(Pipeline->GetMeshPassRegistrations());
    EXPECT_EQ(first.Calls.size(), 2u);
}

TEST_F(FrameSchedulerTest, ExtractAndRecordBorrowTheCurrentSceneAcrossSlots) {
    Scene other;
    Source.AddPrimitive();
    other.AddPrimitive();
    other.AddPrimitive();
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    Nullable<const Scene*> selected{nullptr};
    vector<uint32_t> counts;
    Pipeline->OnExtract = [&](FrameSlot& slot) {
        ASSERT_TRUE(selected);
    };
    Pipeline->OnRecord = [&](FrameSlot& slot) {
        ASSERT_TRUE(selected);
        counts.push_back(selected->GetPrimitiveCount());
    };
    selected = &Source; scheduler->RunFrame();
    selected = &other; scheduler->RunFrame();
    Source.AddPrimitive();
    selected = &Source; scheduler->RunFrame();
    selected = nullptr;
    EXPECT_EQ(counts, (vector<uint32_t>{1, 2, 2}));
    Source.Clear();
    other.Clear();
    scheduler->WaitIdle();
}

TEST_F(FrameSchedulerTest, ArenaObjectsLiveUntilTheirSlotIsWaited) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    Pipeline->OnExtract = [&](FrameSlot& slot) { RegisterCleanup(slot, radray::format("release {}", slot.GetFrameId())); };
    Pipeline->OnWait = [&](FrameSlot& slot) { Log.emplace_back(radray::format("wait {}", slot.GetFrameId())); };
    scheduler->RunFrame();
    scheduler->RunFrame();
    EXPECT_TRUE(Log.empty());
    scheduler->RunFrame();
    EXPECT_EQ(Log, (vector<string>{"wait 0", "release 0"}));
}

TEST_F(FrameSchedulerTest, DrawListsAreEmptyWhenSlotIsReused) {
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    Pipeline->OnExtract = [](FrameSlot& slot) {
        auto& list = slot.GetPassDrawList({0});
        EXPECT_TRUE(list.VisibleCommands.empty());
        EXPECT_TRUE(list.DynamicContext.Commands.empty());
        list.VisibleCommands.emplace_back();
        list.DynamicContext.Commands.emplace_back();
    };
    scheduler->RunFrame();
    scheduler->RunFrame();
}

TEST_F(FrameSchedulerTest, RetiredOwnerIsReleasedWhenItsSlotIsReused) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    bool released = false;
    Pipeline->OnRecord = [&](FrameSlot& slot) {
        if (slot.GetFrameId() == 0) slot.RetireResource(MakeScopeGuard([&]() noexcept { released = true; }));
    };
    scheduler->RunFrame();
    scheduler->RunFrame();
    EXPECT_FALSE(released);
    scheduler->RunFrame();
    EXPECT_TRUE(released);
}

TEST_F(FrameSchedulerTest, WaitIdleReleasesRetiredOwners) {
    auto scheduler = Create(3);
    ASSERT_TRUE(scheduler);
    bool released = false;
    Pipeline->OnExtract = [&](FrameSlot& slot) {
        slot.RetireResource(MakeScopeGuard([&]() noexcept { released = true; }));
    };
    scheduler->RunFrame();
    scheduler->WaitIdle();
    EXPECT_TRUE(released);
}

TEST_F(FrameSchedulerTest, RetireResourceRejectsWaitCallback) {
#if RADRAY_IS_DEBUG
    auto scheduler = Create(1);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame();
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
    Pipeline->OnExtract = [&](FrameSlot& slot) { used = &slot; };
    scheduler->RunFrame();
    ASSERT_TRUE(used);
    EXPECT_DEATH(used->RetireResource(unique_ptr<int>{}), "");
#else
    GTEST_SKIP() << "Release retirement trusts the callback precondition";
#endif
}

TEST_F(FrameSchedulerTest, WaitIdleResetsEveryAllocatorBeforeReleasingAnyResource) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    Pipeline->OnExtract = [&](FrameSlot& slot) {
        RegisterCleanup(slot, "release frame object");
        slot.RetireResource(MakeScopeGuard([&]() noexcept { Log.push_back("release retired"); }));
    };
    scheduler->RunFrame();
    scheduler->RunFrame();
    Gpu.Device->OnAllocatorReset = [&] { Log.push_back("reset"); };
    scheduler->WaitIdle();
    EXPECT_EQ(Log, (vector<string>{"reset", "reset", "release retired", "release retired", "release frame object", "release frame object"}));
}

TEST_F(FrameSchedulerTest, DestructionWaitsSlotsBeforeDestroyingPipeline) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame();
    scheduler->RunFrame();
    // 仅流水线回调持有此标记，因此流水线销毁时才会执行它的删除器。
    Pipeline->OnWait = [this, marker = shared_ptr<int>{nullptr, [this](int*) { Log.push_back("pipeline destroyed"); }}](FrameSlot& slot) {
        Log.emplace_back(radray::format("wait {}", slot.GetIndex()));
    };
    scheduler.reset();
    EXPECT_EQ(Log, (vector<string>{"wait 0", "wait 1", "pipeline destroyed"}));
}

}  // namespace
}  // namespace radray
