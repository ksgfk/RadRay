#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory_resource>
#include <utility>
#include <vector>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/scene.h>

#include "test_fakes.h"

namespace radray {
namespace {

using test::CallbackPipeline;

class MemoryProbe final : public std::pmr::memory_resource {
public:
    explicit MemoryProbe(std::pmr::memory_resource& upstream) noexcept : _upstream(upstream) {}
    size_t LiveBytes{0}, Allocations{0};
    bool Fail{false};

private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        if (Fail) std::function<void()>{}();
        auto* result = _upstream.allocate(bytes, alignment);
        LiveBytes += bytes;
        ++Allocations;
        return result;
    }
    void do_deallocate(void* pointer, size_t bytes, size_t alignment) override {
        _upstream.deallocate(pointer, bytes, alignment);
        LiveBytes -= bytes;
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    std::pmr::memory_resource& _upstream;
};

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
    // 登记一个由槽拥有的对象，槽位回收它时记录 `text`。
    void RegisterCleanup(FrameSlot& slot, string text) {
        auto cleanup = MakeScopeGuard([this, text = std::move(text)]() noexcept { Log.push_back(text); });
        slot.Emplace<decltype(cleanup)>(std::move(cleanup));
    }

    test::FakeGpu Gpu;
    CallbackPipeline* Pipeline{nullptr};
    Scene Source;
    vector<string> Log;
};

TEST_F(FrameSchedulerTest, CreateRejectsZeroSlots) {
    EXPECT_FALSE(Create(0));
}

TEST_F(FrameSchedulerTest, ObjectsKeepAlignmentAndDestroyInReverseCompletionOrder) {
    class alignas(4096) Outer {
    public:
        Outer(FrameSlot& slot, vector<int>& destroyed) : Destroyed(destroyed) {
            auto child = MakeScopeGuard([&destroyed]() noexcept { destroyed.push_back(1); });
            slot.Emplace<decltype(child)>(std::move(child));
        }
        ~Outer() noexcept { Destroyed.push_back(2); }
        vector<int>& Destroyed;
        uint64_t Value{42};
    };
    vector<int> destroyed;
    auto scheduler = Create(1);
    auto& slot = scheduler->BeginFrame();
    auto* object = slot.Emplace<Outer>(slot, destroyed);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(object) % alignof(Outer), 0u);
    for (uint32_t i = 0; i < 32768; ++i) slot.Emplace<uint64_t>(i);
    EXPECT_EQ(object->Value, 42u);
    scheduler->RunFrame();
    EXPECT_TRUE(destroyed.empty());
    scheduler->WaitIdle();
    EXPECT_EQ(destroyed, (vector<int>{2, 1}));
    scheduler->WaitIdle();
    EXPECT_EQ(destroyed, (vector<int>{2, 1}));
}

TEST_F(FrameSchedulerTest, FailedConstructionKeepsCompletedChildrenUntilSlotCleanup) {
    class Parent {
    public:
        Parent(FrameSlot& slot, bool& childDestroyed) {
            auto child = MakeScopeGuard([&childDestroyed]() noexcept { childDestroyed = true; });
            slot.Emplace<decltype(child)>(std::move(child));
            std::function<void()>{}();
        }
    };
    bool childDestroyed = false;
    auto scheduler = Create(1);
    auto& slot = scheduler->BeginFrame();
    EXPECT_THROW(slot.Emplace<Parent>(slot, childDestroyed), std::bad_function_call);
    EXPECT_FALSE(childDestroyed);
    EXPECT_EQ(*slot.Emplace<int>(42), 42);
    scheduler->WaitIdle();
    EXPECT_TRUE(childDestroyed);
}

TEST_F(FrameSchedulerTest, PmrContainersAreDestroyedAfterWaitAndCommandReset) {
    for (uint32_t slots : {1u, 3u}) {
        auto scheduler = Create(slots);
        vector<Nullable<std::pmr::memory_resource*>> resources(slots);
        vector<weak_ptr<uint32_t>> owners;
        Log.clear();
        Pipeline->OnWait = [&](FrameSlot&) { Log.push_back("wait"); };
        Gpu.Device->OnAllocatorReset = [&] { Log.push_back("reset"); };
        for (uint32_t frame = 0; frame < slots * 3; ++frame) {
            auto& slot = scheduler->BeginFrame();
            auto& resource = slot.GetMemoryResource();
            if (resources[slot.GetIndex()]) EXPECT_EQ(resources[slot.GetIndex()].Get(), &resource);
            resources[slot.GetIndex()] = &resource;
            auto* values = slot.Emplace<std::pmr::vector<shared_ptr<uint32_t>>>(&resource);
            auto owner = shared_ptr<uint32_t>{new uint32_t{frame}, [&](uint32_t* value) {
                                                  EXPECT_GE(Log.size(), 2u);
                                                  EXPECT_LT(std::find(Log.rbegin(), Log.rend(), "reset"), std::find(Log.rbegin(), Log.rend(), "wait"));
                                                  Log.push_back("destroy");
                                                  delete value;
                                              }};
            owners.push_back(owner);
            for (uint32_t i = 0; i < 64; ++i) values->push_back(owner);
            owner.reset();
            scheduler->RunFrame();
            EXPECT_FALSE(owners.back().expired());
        }
        for (uint32_t i = 0; i < slots; ++i)
            for (uint32_t j = i + 1; j < slots; ++j) EXPECT_NE(resources[i].Get(), resources[j].Get());
        scheduler->WaitIdle();
        for (const auto& owner : owners) EXPECT_TRUE(owner.expired());
        // WaitIdle 后仍可使用同一槽资源，容器在调度器销毁时由槽统一清理。
        auto& slot = scheduler->BeginFrame();
        auto* values = slot.Emplace<std::pmr::vector<uint64_t>>(&slot.GetMemoryResource());
        values->resize(4096, 42);
        EXPECT_EQ(values->back(), 42u);
        scheduler.reset();
    }
    Gpu.Device->OnAllocatorReset = {};
}

TEST_F(FrameSchedulerTest, ScratchContainersDoNotInvalidateFrameData) {
    auto scheduler = Create(1);
    for (uint32_t frame = 0; frame < 3; ++frame) {
        auto& slot = scheduler->BeginFrame();
        auto* values = slot.Emplace<std::pmr::vector<uint32_t>>(&slot.GetMemoryResource());
        values->resize(4096, frame);
        for (uint32_t i = 0; i < 8; ++i) {
            std::pmr::vector<uint32_t> scratch{&slot.GetScratchMemoryResource()};
            scratch.resize(4096, i + 10);
        }
        EXPECT_TRUE(std::all_of(values->begin(), values->end(), [frame](uint32_t value) { return value == frame; }));
        scheduler->RunFrame();
    }
}

TEST_F(FrameSchedulerTest, EmptyFramesDoNotAllocateLinearStorage) {
    HeapMemoryResource heap;
    MemoryProbe memory{heap};
    auto scheduler = FrameScheduler::Create(Gpu.Context, make_unique<CallbackPipeline>(), 1, &memory).Unwrap();
    const auto allocations = memory.Allocations;
    scheduler->RunFrame();
    scheduler->WaitIdle();
    EXPECT_EQ(memory.Allocations, allocations);
    scheduler.reset();
    EXPECT_EQ(memory.LiveBytes, 0u);
}

TEST_F(FrameSchedulerTest, ReusesInitialStorageAndReturnsOverflowOnCleanup) {
    HeapMemoryResource heap;
    MemoryProbe memory{heap};
    auto scheduler = FrameScheduler::Create(Gpu.Context, make_unique<CallbackPipeline>(), 1, &memory).Unwrap();
    auto& first = scheduler->BeginFrame();
    first.Emplace<int>(42);
    const auto retained = memory.LiveBytes, allocations = memory.Allocations;
    scheduler->RunFrame();

    auto& next = scheduler->BeginFrame();
    EXPECT_EQ(*next.Emplace<int>(73), 73);
    EXPECT_EQ(memory.Allocations, allocations);
    auto* values = next.Emplace<std::pmr::vector<byte>>(&next.GetMemoryResource());
    values->resize(1024 * 1024, byte{0x35});
    EXPECT_GT(memory.LiveBytes, retained);

    scheduler->WaitIdle();
    EXPECT_EQ(memory.LiveBytes, retained);
    scheduler->WaitIdle();
    EXPECT_EQ(memory.LiveBytes, retained);
    scheduler.reset();
    EXPECT_EQ(memory.LiveBytes, 0u);
}

TEST_F(FrameSchedulerTest, AllocationFailureDoesNotConsumeMovedOwner) {
    HeapMemoryResource heap;
    MemoryProbe memory{heap};
    auto scheduler = FrameScheduler::Create(Gpu.Context, make_unique<CallbackPipeline>(), 1, &memory).Unwrap();
    auto& slot = scheduler->BeginFrame();
    auto owner = make_unique<int>(42);
    memory.Fail = true;
    EXPECT_THROW(slot.Emplace<decltype(owner)>(std::move(owner)), std::bad_function_call);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(*owner, 42);
    memory.Fail = false;
    auto* moved = slot.Emplace<decltype(owner)>(std::move(owner));
    EXPECT_FALSE(owner);
    EXPECT_EQ(**moved, 42);
    scheduler->WaitIdle();
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
    auto* value = slot.Emplace<int>(42);
    bool released = false;
    auto owner = MakeScopeGuard([&]() noexcept { released = true; });
    slot.Emplace<decltype(owner)>(std::move(owner));
    Log.clear();
    EXPECT_EQ(&scheduler->BeginFrame(), &slot);
    EXPECT_TRUE(Log.empty());
    EXPECT_EQ(*value, 42);
    EXPECT_FALSE(released);
    slot.Emplace<unique_ptr<int>>();
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
    auto owner = MakeScopeGuard([&]() noexcept { released = true; });
    slot.Emplace<decltype(owner)>(std::move(owner));
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
    Pipeline->OnExtract = [&](FrameSlot&) {
        ASSERT_TRUE(selected);
    };
    Pipeline->OnRecord = [&](FrameSlot&) {
        ASSERT_TRUE(selected);
        counts.push_back(selected->GetPrimitiveCount());
    };
    selected = &Source;
    scheduler->RunFrame();
    selected = &other;
    scheduler->RunFrame();
    Source.AddPrimitive();
    selected = &Source;
    scheduler->RunFrame();
    selected = nullptr;
    EXPECT_EQ(counts, (vector<uint32_t>{1, 2, 2}));
    Source.Clear();
    other.Clear();
    scheduler->WaitIdle();
}

TEST_F(FrameSchedulerTest, ObjectsLiveUntilTheirSlotIsWaited) {
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
        if (slot.GetFrameId() == 0) {
            auto owner = MakeScopeGuard([&]() noexcept { released = true; });
            slot.Emplace<decltype(owner)>(std::move(owner));
        }
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
        auto owner = MakeScopeGuard([&]() noexcept { released = true; });
        slot.Emplace<decltype(owner)>(std::move(owner));
    };
    scheduler->RunFrame();
    scheduler->WaitIdle();
    EXPECT_TRUE(released);
}

TEST_F(FrameSchedulerTest, WaitIdleCleansOlderSlotsBeforeNewerMovedOwners) {
    for (const bool submitOwner : {false, true}) {
        auto scheduler = Create(2);
        scheduler->RunFrame();
        bool referenceReleased = false, ownerReleased = false;
        auto& older = scheduler->BeginFrame();
        auto reference = MakeScopeGuard([&]() noexcept {
            EXPECT_FALSE(ownerReleased);
            referenceReleased = true;
        });
        older.Emplace<decltype(reference)>(std::move(reference));
        scheduler->RunFrame();
        auto& newer = scheduler->BeginFrame();
        auto owner = MakeScopeGuard([&]() noexcept {
            EXPECT_TRUE(referenceReleased);
            ownerReleased = true;
        });
        newer.Emplace<decltype(owner)>(std::move(owner));
        if (submitOwner) scheduler->RunFrame();

        scheduler->WaitIdle();

        EXPECT_TRUE(referenceReleased);
        EXPECT_TRUE(ownerReleased);
    }
}

TEST_F(FrameSchedulerTest, WaitIdleResetsEveryAllocatorBeforeReleasingAnyResource) {
    auto scheduler = Create(2);
    ASSERT_TRUE(scheduler);
    Pipeline->OnExtract = [&](FrameSlot& slot) {
        RegisterCleanup(slot, "release frame object");
        auto owner = MakeScopeGuard([&]() noexcept { Log.push_back("release moved owner"); });
        slot.Emplace<decltype(owner)>(std::move(owner));
    };
    scheduler->RunFrame();
    scheduler->RunFrame();
    Gpu.Device->OnAllocatorReset = [&] { Log.push_back("reset"); };
    scheduler->WaitIdle();
    EXPECT_EQ(Log, (vector<string>{"reset", "reset", "release moved owner", "release frame object", "release moved owner", "release frame object"}));
}

TEST_F(FrameSchedulerTest, MovedTargetsOutliveLaterRegisteredViews) {
    Gpu.Device->EnableTextures = true;
    uint32_t views = 0, targets = 0;
    Gpu.Device->OnViewDestroyed = [&] { ++views; };
    Gpu.Device->OnTextureDestroyed = [&] { ++targets; EXPECT_GE(views, targets); };
    auto scheduler = Create(2);
    Pipeline->OnExtract = [&](FrameSlot& slot) {
        auto texture = Gpu.Device->CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Usage = render::TextureUse::Resource}).Unwrap();
        auto view = Gpu.Device->CreateTextureView({texture.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap();
        slot.Emplace<unique_ptr<render::Texture>>(std::move(texture));
        slot.Emplace<unique_ptr<render::TextureView>>(std::move(view));
    };
    scheduler->RunFrame();
    scheduler->RunFrame();
    scheduler->RunFrame();
    EXPECT_EQ(targets, 1u);
    scheduler->WaitIdle();
    EXPECT_EQ(targets, 3u);
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

TEST(FrameSchedulerStages, SubmitEndsTheCurrentSlotWindowAndPrecedesRotation) {
    radray::test::FakeGpu gpu;
    radray::test::CallbackPipeline* pipeline;
    auto scheduler = radray::test::CreateScheduler(gpu.Context, pipeline, 1);
    radray::vector<int> stages;
    pipeline->OnExtract = [&](radray::FrameSlot&) { stages.push_back(1); };
    pipeline->OnPrepare = [&](radray::FrameSlot&) { stages.push_back(2); };
    pipeline->OnRecord = [&](radray::FrameSlot&) { stages.push_back(3); };
    pipeline->OnSubmit = [&](radray::FrameSlot& slot) {
        stages.push_back(4);
        EXPECT_EQ(scheduler->GetCurrentSlot().Get(), &slot);
        EXPECT_EQ(slot.GetFrameId(), 0u);
    };
    pipeline->OnWait = [&](radray::FrameSlot&) { stages.push_back(5); };
    scheduler->RunFrame();
    EXPECT_FALSE(scheduler->GetCurrentSlot());
    scheduler->WaitIdle();
    EXPECT_EQ(stages, (radray::vector<int>{1, 2, 3, 4, 5}));
}
