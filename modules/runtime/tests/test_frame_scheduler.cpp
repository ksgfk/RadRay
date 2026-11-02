#include <gtest/gtest.h>

#include <cstring>
#include <limits>

#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/gpu_env.h>

namespace radray {
namespace {

struct FrameEvent {
    uint32_t SlotIndex;
    int Stage;  // 0: Wait, 1: Extract, 2: Record.

    bool operator==(const FrameEvent&) const noexcept = default;
};

struct FrameArenaEvent {
    uint64_t FrameId;
    int Stage;  // 0: Wait completed, 1: Extract cleanup, 2: Record cleanup.

    bool operator==(const FrameArenaEvent&) const noexcept = default;
};

struct FrameProbe {
    uint32_t Value{1};
    uint32_t CommandCount{1};
    bool SubmitSeparately{false};
    int RetirementStage{0};  // 1: Extract, 2: Record.
    vector<FrameEvent> Events;
    vector<uint64_t> ExtractedFrameIds;
    vector<uint64_t> RecordedFrameIds;
    vector<uint64_t> WaitedFrameIds;
    vector<FrameSlot*> Slots;
    vector<uint32_t*> FrameValues;
    vector<FrameArenaEvent> ArenaEvents;
    vector<FrameArenaEvent> RetiredEvents;
    vector<vector<render::CommandBuffer*>> Commands;
    bool ReadOnDestroy{false};
    bool Destroyed{false};
    vector<std::optional<uint32_t>> DestroyedValues;
};

struct CopyFrameResources {
    unique_ptr<render::Buffer> Readback;
    Nullable<unique_ptr<render::Fence>> CompletionFence;
    uint64_t CompletionValue{0};
};

class CopyFramePipeline final : public FramePipeline {
public:
    CopyFramePipeline(FrameProbe& probe, vector<CopyFrameResources> resources, bool synchronous)
        : Probe(probe), Resources(std::move(resources)), Synchronous(synchronous) {}

    ~CopyFramePipeline() noexcept override {
        if (Probe.ReadOnDestroy) {
            for (uint32_t index = 0; index < Resources.size(); ++index) Probe.DestroyedValues.push_back(ReadValue(index));
        }
        Probe.Destroyed = true;
    }

    void Extract(const Scene& source, FrameSlot& slot) noexcept override {
        Probe.Events.push_back({slot.GetIndex(), 1});
        Probe.ExtractedFrameIds.push_back(slot.GetFrameId());
        if (slot.GetIndex() == Probe.Slots.size()) {
            Probe.Slots.push_back(&slot);
            Probe.Commands.emplace_back();
        }
        EXPECT_EQ(Probe.Slots[slot.GetIndex()], &slot);
        EXPECT_EQ(slot.GetScene().Get(), &source);
        auto& draws = slot.GetPassDrawList({0});
        EXPECT_TRUE(draws.VisibleCommands.empty());
        EXPECT_TRUE(draws.DynamicContext.Commands.empty());
        draws.VisibleCommands.push_back({.FirstInstance = Probe.Value});
        auto* value = slot.GetArena().Emplace<uint32_t>(Probe.Value);
        if (slot.GetIndex() == Probe.FrameValues.size())
            Probe.FrameValues.push_back(value);
        else
            Probe.FrameValues[slot.GetIndex()] = value;
        auto cleanup = MakeScopeGuard([this, &slot, value, expected = Probe.Value, frameId = slot.GetFrameId()]() noexcept {
            EXPECT_FALSE(Probe.Destroyed);
            EXPECT_EQ(*value, expected);
            EXPECT_EQ(slot.GetFrameId(), frameId);
            EXPECT_FALSE(slot.GetScene());
            EXPECT_TRUE(slot.GetPassDrawList({0}).VisibleCommands.empty());
            EXPECT_TRUE(slot.GetPassDrawList({0}).DynamicContext.Commands.empty());
            Probe.ArenaEvents.push_back({frameId, 1});
        });
        slot.GetArena().Emplace<decltype(cleanup)>(std::move(cleanup));
        RetireOwners(slot, 1);
    }

    void Record(FrameSlot& slot) noexcept override {
        Probe.Events.push_back({slot.GetIndex(), 2});
        Probe.RecordedFrameIds.push_back(slot.GetFrameId());
        auto& commands = Probe.Commands[slot.GetIndex()];
        commands.clear();
        auto& draws = slot.GetPassDrawList({0});
        draws.DynamicContext.Commands.emplace_back();
        auto& resources = Resources[slot.GetIndex()];
        auto buffer = slot.GetDevice().CreateBuffer({.Size = 256, .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite});
        if (!buffer) RADRAY_ABORT("CopyFramePipeline: cannot create upload buffer");
        auto cleanup = MakeScopeGuard([this, frameId = slot.GetFrameId()]() noexcept {
            EXPECT_FALSE(Probe.Destroyed);
            Probe.ArenaEvents.push_back({frameId, 2});
        });
        slot.GetArena().Emplace<decltype(cleanup)>(std::move(cleanup));
        auto& upload = *slot.GetArena().Emplace<unique_ptr<render::Buffer>>(buffer.Release());
        auto* mapped = upload->Map(0, sizeof(Probe.Value));
        if (!mapped) RADRAY_ABORT("CopyFramePipeline: cannot map upload buffer");
        std::memcpy(mapped, Probe.FrameValues[slot.GetIndex()], sizeof(Probe.Value));
        upload->FlushMappedRange({0, sizeof(Probe.Value)});
        upload->Unmap();
        EXPECT_TRUE(slot.GetScene());
        EXPECT_EQ(slot.GetScene()->Primitives.Count(), 1u);
        EXPECT_EQ(draws.VisibleCommands.size(), 1u);
        EXPECT_EQ(draws.VisibleCommands[0].FirstInstance, Probe.Value);
        EXPECT_EQ(draws.DynamicContext.Commands.size(), 1u);
        const uint64_t submissionCount = Probe.SubmitSeparately ? Probe.CommandCount : (Probe.CommandCount == 0 ? 0 : 1);
        if (!Synchronous && resources.CompletionValue >= std::numeric_limits<uint64_t>::max() - submissionCount) RADRAY_ABORT("CopyFramePipeline: fence values exhausted");
        for (uint32_t index = 0; index < Probe.CommandCount; ++index) {
            auto command = slot.AcquireCommandBuffer();
            if (!command) RADRAY_ABORT("CopyFramePipeline: cannot acquire command buffer");
            command->Begin();
            command->CopyBufferToBuffer(resources.Readback.get(), index * sizeof(Probe.Value), upload.get(), 0, sizeof(Probe.Value));
            command->End();
            commands.push_back(command.Get());
        }
        if (Probe.SubmitSeparately) {
            for (auto* command : commands) {
                array<render::CommandBuffer*, 1> batch{command};
                SubmitCommands(slot, batch);
            }
        } else if (!commands.empty()) {
            SubmitCommands(slot, commands);
        }
        RetireOwners(slot, 2);
    }

    void RetireOwners(FrameSlot& slot, int stage) {
        if (Probe.RetirementStage != stage) return;
        for (int index = 1; index <= 2; ++index) {
            auto cleanup = MakeScopeGuard([this, frameId = slot.GetFrameId(), index]() noexcept {
                EXPECT_FALSE(Probe.Destroyed);
                Probe.RetiredEvents.push_back({frameId, index});
            });
            slot.RetireResource(std::move(cleanup));
        }
    }

    void SubmitCommands(FrameSlot& slot, std::span<render::CommandBuffer*> commands) noexcept {
        auto& resources = Resources[slot.GetIndex()];
        if (Synchronous) {
            EXPECT_FALSE(resources.CompletionFence);
            slot.GetQueue().Submit({.CmdBuffers = commands});
            slot.GetQueue().Wait();
        } else {
            array<render::Fence*, 1> fences{resources.CompletionFence.Get()};
            array<uint64_t, 1> values{++resources.CompletionValue};
            slot.GetQueue().Submit({.CmdBuffers = commands, .SignalFences = fences, .SignalValues = values});
        }
    }

    void Wait(FrameSlot& slot) noexcept override {
        Probe.Events.push_back({slot.GetIndex(), 0});
        Probe.WaitedFrameIds.push_back(slot.GetFrameId());
        if (!Synchronous) {
            auto& resources = Resources[slot.GetIndex()];
            if (resources.CompletionValue != 0) resources.CompletionFence.Get()->Wait(resources.CompletionValue);
        }
        Probe.ArenaEvents.push_back({slot.GetFrameId(), 0});
    }

    std::optional<uint32_t> ReadValue(uint32_t index, uint32_t word = 0) noexcept {
        auto& readback = Resources[index].Readback;
        const uint64_t offset = word * sizeof(uint32_t);
        auto* mapped = readback->Map(offset, sizeof(uint32_t));
        if (!mapped) return std::nullopt;
        readback->InvalidateMappedRange({offset, sizeof(uint32_t)});
        uint32_t value;
        std::memcpy(&value, mapped, sizeof(value));
        readback->Unmap();
        return value;
    }

    FrameProbe& Probe;
    vector<CopyFrameResources> Resources;
    bool Synchronous;
};

class SharedBufferPipeline final : public FramePipeline {
public:
    SharedBufferPipeline(unique_ptr<render::Buffer> buffer, vector<unique_ptr<render::Buffer>> readbacks, bool& released)
        : Owner(std::move(buffer)), Source(Owner.get()), Readbacks(std::move(readbacks)), Released(released) {}

    void Extract(const Scene& source, FrameSlot& slot) noexcept override {
        EXPECT_EQ(slot.GetScene().Get(), &source);
        if (MoveOwnerToArena && Owner) {
            auto cleanup = MakeScopeGuard([owner = std::move(Owner), &released = Released]() mutable noexcept {
                owner.reset();
                released = true;
            });
            slot.GetArena().Emplace<decltype(cleanup)>(std::move(cleanup));
        }
        if (slot.GetFrameId() == RetireFrameId && !RetireInRecord) RetireOwner(slot);
    }
    void Record(FrameSlot& slot) noexcept override {
        if (!Source) return;
        auto command = slot.AcquireCommandBuffer();
        if (!command) RADRAY_ABORT("SharedBufferPipeline: cannot acquire command buffer");
        command->Begin();
        command->CopyBufferToBuffer(Readbacks[slot.GetIndex()].get(), 0, Source.Get(), 0, sizeof(uint32_t));
        command->End();
        array batch{command.Get()};
        slot.GetQueue().Submit({.CmdBuffers = batch});
        if (slot.GetFrameId() == RetireFrameId && RetireInRecord) RetireOwner(slot);
    }
    void Wait(FrameSlot& slot) noexcept override {
        EXPECT_FALSE(slot.GetScene());
        if (slot.GetFrameId() <= RetireFrameId) EXPECT_FALSE(Released);
        slot.GetQueue().Wait();
    }
    void RetireOwner(FrameSlot& slot) {
        ASSERT_TRUE(Owner);
        Source = nullptr;
        auto cleanup = MakeScopeGuard([owner = std::move(Owner), &released = Released]() mutable noexcept {
            owner.reset();
            released = true;
        });
        slot.RetireResource(std::move(cleanup));
    }
    std::optional<uint32_t> ReadValue(uint32_t index) {
        auto& readback = Readbacks[index];
        const Nullable<void*> mapped = readback->Map(0, sizeof(uint32_t));
        if (!mapped) return std::nullopt;
        readback->InvalidateMappedRange({0, sizeof(uint32_t)});
        uint32_t value;
        std::memcpy(&value, mapped.Get(), sizeof(value));
        readback->Unmap();
        return value;
    }
    unique_ptr<render::Buffer> Owner;
    Nullable<render::Buffer*> Source;
    vector<unique_ptr<render::Buffer>> Readbacks;
    bool& Released;
    bool MoveOwnerToArena{false};
    uint64_t RetireFrameId{std::numeric_limits<uint64_t>::max()};
    bool RetireInRecord{false};
};

class DeltaProcessor final : public MeshPassProcessor {
public:
    explicit DeltaProcessor(MeshPassDrawListContext& output) noexcept : MeshPassProcessor(output) {}
    void AddMeshBatch(const MeshBatchContext& context) override {
        Calls.push_back(context.BatchHandle);
        auto& command = DrawListContext().AddCommand({});
        DrawListContext().FinalizeCommand(context, 0, command);
    }
    vector<SparseSetHandle> Calls;
};

class DeltaFramePipeline final : public FramePipeline {
public:
    DeltaFramePipeline() noexcept : Processor(DefaultOutput), Passes{{{{0}, &Processor}}} {}
    std::span<const MeshPassRegistration> GetMeshPassRegistrations() noexcept override { return Passes; }
    void Extract(const Scene& source, FrameSlot& slot) noexcept override {
        CurrentSlot = &slot;
        EXPECT_EQ(slot.GetScene().Get(), &source);
        auto& output = slot.GetPassDrawList({0});
        EXPECT_TRUE(output.VisibleCommands.empty());
        EXPECT_TRUE(output.DynamicContext.Commands.empty());
        if (!SkipGather) {
            const auto primitive = source.Primitives.GetHandleAt(0);
            EXPECT_TRUE(source.GatherVisiblePrimitives({0}, std::span{&primitive, 1}, output.VisibleCommands));
        }
    }
    void Record(FrameSlot& slot) noexcept override {
        EXPECT_TRUE(slot.GetScene());
    }
    void Wait(FrameSlot& slot) noexcept override { EXPECT_FALSE(slot.GetScene()); }
    DynamicMeshPassDrawListContext DefaultOutput;
    DeltaProcessor Processor;
    array<MeshPassRegistration, 1> Passes;
    Nullable<FrameSlot*> CurrentSlot{nullptr};
    bool SkipGather{false};
};

class FrameLoop : public testing::TestWithParam<render::RenderBackend> {
protected:
    void SetUp() override {
        auto environment = Environment::Create(GetParam(), true, "FrameLoopTest");
        ASSERT_TRUE(environment);
        Env = environment.Release();
        auto device = DeviceContext::Create(*Env);
        ASSERT_TRUE(device);
        Device = device.Release();
        Source.Primitives.Emplace();
        Source.Meshes.Emplace();
        Source.Materials.Emplace();
    }

    Nullable<unique_ptr<CopyFramePipeline>> MakePipeline(FrameProbe& probe, uint32_t slotCount, bool synchronous = false) {
        vector<CopyFrameResources> resources;
        for (uint32_t index = 0; index < slotCount; ++index) {
            auto readback = Device->GetDevice().CreateBuffer({.Size = 256, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead});
            if (!readback) return nullptr;
            Nullable<unique_ptr<render::Fence>> fence;
            if (!synchronous) {
                fence = Device->GetDevice().CreateFence();
                if (!fence) return nullptr;
            }
            resources.push_back({readback.Release(), std::move(fence)});
        }
        return make_unique<CopyFramePipeline>(probe, std::move(resources), synchronous);
    }

    Nullable<unique_ptr<SharedBufferPipeline>> MakeSharedPipeline(bool& released, uint32_t slotCount, uint32_t value) {
        auto source = Device->GetDevice().CreateBuffer({.Size = 256, .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite});
        if (!source) return nullptr;
        const Nullable<void*> mapped = source->Map(0, sizeof(value));
        if (!mapped) return nullptr;
        std::memcpy(mapped.Get(), &value, sizeof(value));
        source->FlushMappedRange({0, sizeof(value)});
        source->Unmap();
        vector<unique_ptr<render::Buffer>> readbacks;
        for (uint32_t index = 0; index < slotCount; ++index) {
            auto buffer = Device->GetDevice().CreateBuffer({.Size = 256, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead});
            if (!buffer) return nullptr;
            readbacks.push_back(buffer.Release());
        }
        return make_unique<SharedBufferPipeline>(source.Release(), std::move(readbacks), released);
    }

    unique_ptr<Environment> Env;
    unique_ptr<DeviceContext> Device;
    Scene Source;
};

TEST_P(FrameLoop, ValidatesConfigurationAndDefaultsToTwoSlots) {
    FrameProbe probe;
    auto invalidPipeline = MakePipeline(probe, 0);
    ASSERT_TRUE(invalidPipeline);
    EXPECT_FALSE(FrameScheduler::Create(*Device, invalidPipeline.Release(), 0));
    EXPECT_TRUE(probe.Destroyed);
    EXPECT_TRUE(probe.Events.empty());
    EXPECT_FALSE(FrameScheduler::Create(*Device, unique_ptr<FramePipeline>{}));

    auto pipeline = MakePipeline(probe, 2);
    ASSERT_TRUE(pipeline);
    auto scheduler = FrameScheduler::Create(*Device, pipeline.Release());
    ASSERT_TRUE(scheduler);
    EXPECT_EQ(scheduler->GetSlotCount(), 2u);
}

TEST_P(FrameLoop, RotatesConfiguredSlotsAndWaitsOnlyBeforeReuse) {
    for (uint32_t slotCount = 1; slotCount <= 3; ++slotCount) {
        SCOPED_TRACE(slotCount);
        FrameProbe probe;
        vector<FrameEvent> expected;
        {
            auto pipeline = MakePipeline(probe, slotCount);
            ASSERT_TRUE(pipeline);
            auto* copy = pipeline.Get();
            auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), slotCount);
            ASSERT_TRUE(scheduler);
            EXPECT_EQ(scheduler->GetSlotCount(), slotCount);
            vector<uint32_t> values(slotCount);
            for (uint32_t frame = 0; frame < slotCount * 2 + 1; ++frame) {
                const uint32_t index = frame % slotCount;
                probe.Value = 100 + frame;
                values[index] = probe.Value;
                if (frame >= slotCount) expected.push_back({index, 0});
                for (int stage = 1; stage <= 2; ++stage) expected.push_back({index, stage});
                scheduler->RunFrame(Source);
                EXPECT_EQ(probe.Events, expected);
            }
            for (uint32_t index = 0; index < slotCount; ++index) expected.push_back({index, 0});
            scheduler->WaitIdle();
            EXPECT_EQ(probe.Events, expected);
            for (uint32_t index = 0; index < slotCount; ++index) {
                EXPECT_EQ(copy->ReadValue(index), values[index]);
                EXPECT_FALSE(probe.Slots[index]->GetScene());
                EXPECT_TRUE(probe.Slots[index]->GetPassDrawList({0}).VisibleCommands.empty());
                EXPECT_TRUE(probe.Slots[index]->GetPassDrawList({0}).DynamicContext.Commands.empty());
            }
            scheduler->WaitIdle();
            EXPECT_EQ(probe.Events, expected);
        }
        EXPECT_EQ(probe.Events, expected);
        EXPECT_TRUE(probe.Destroyed);
    }
}

TEST_P(FrameLoop, NumbersFramesAcrossSlotsAndPreservesIdsWhileWaiting) {
    FrameProbe probe;
    auto pipeline = MakePipeline(probe, 2);
    ASSERT_TRUE(pipeline);
    auto scheduler = FrameScheduler::Create(*Device, pipeline.Release());
    ASSERT_TRUE(scheduler);
    for (uint32_t frame = 0; frame < 4; ++frame) scheduler->RunFrame(Source);
    EXPECT_EQ(probe.ExtractedFrameIds, (vector<uint64_t>{0, 1, 2, 3}));
    EXPECT_EQ(probe.RecordedFrameIds, probe.ExtractedFrameIds);
    EXPECT_EQ(probe.WaitedFrameIds, (vector<uint64_t>{0, 1}));
    EXPECT_EQ(probe.Slots[0]->GetFrameId(), 2u);
    EXPECT_EQ(probe.Slots[1]->GetFrameId(), 3u);
    scheduler->WaitIdle();
    scheduler->WaitIdle();
    EXPECT_EQ(probe.WaitedFrameIds, (vector<uint64_t>{0, 1, 2, 3}));
    EXPECT_EQ(probe.Slots[0]->GetFrameId(), 2u);
    EXPECT_EQ(probe.Slots[1]->GetFrameId(), 3u);
    scheduler->RunFrame(Source);
    EXPECT_EQ(probe.ExtractedFrameIds, (vector<uint64_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(probe.RecordedFrameIds, probe.ExtractedFrameIds);
    scheduler->WaitIdle();
    EXPECT_EQ(probe.WaitedFrameIds, (vector<uint64_t>{0, 1, 2, 3, 4}));
}

TEST_P(FrameLoop, ArenaKeepsSlotsIndependentAndReclaimsOnlyAfterCompletion) {
    for (uint32_t slotCount = 1; slotCount <= 3; ++slotCount) {
        SCOPED_TRACE(slotCount);
        FrameProbe probe;
        vector<FrameArenaEvent> expected;
        {
            auto pipeline = MakePipeline(probe, slotCount);
            ASSERT_TRUE(pipeline);
            auto* copy = pipeline.Get();
            auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), slotCount);
            ASSERT_TRUE(scheduler);
            for (uint32_t frame = 0; frame < slotCount; ++frame) {
                probe.Value = 100 + frame;
                scheduler->RunFrame(Source);
                EXPECT_TRUE(probe.ArenaEvents.empty());
                for (uint32_t index = 0; index <= frame; ++index) {
                    EXPECT_EQ(*probe.FrameValues[index], 100 + index);
                    const auto& draws = probe.Slots[index]->GetPassDrawList({0});
                    ASSERT_EQ(draws.VisibleCommands.size(), 1u);
                    EXPECT_EQ(draws.VisibleCommands[0].FirstInstance, 100 + index);
                    EXPECT_EQ(draws.DynamicContext.Commands.size(), 1u);
                    if (index != frame) {
                        EXPECT_NE(&probe.Slots[index]->GetArena(), &probe.Slots[frame]->GetArena());
                        EXPECT_NE(probe.FrameValues[index], probe.FrameValues[frame]);
                    }
                }
            }

            probe.Value = 200;
            scheduler->RunFrame(Source);
            expected = {{0, 0}, {0, 2}, {0, 1}};
            EXPECT_EQ(probe.ArenaEvents, expected);
            EXPECT_EQ(*probe.FrameValues[0], 200u);
            for (uint32_t index = 1; index < slotCount; ++index) {
                EXPECT_EQ(*probe.FrameValues[index], 100 + index);
                EXPECT_EQ(probe.Slots[index]->GetPassDrawList({0}).VisibleCommands[0].FirstInstance, 100 + index);
            }

            scheduler->WaitIdle();
            for (uint32_t index = 0; index < slotCount; ++index) expected.push_back({index == 0 ? slotCount : index, 0});
            for (uint32_t index = 0; index < slotCount; ++index) {
                const uint64_t frameId = index == 0 ? slotCount : index;
                expected.push_back({frameId, 2});
                expected.push_back({frameId, 1});
                EXPECT_EQ(copy->ReadValue(index), index == 0 ? 200u : 100 + index);
                const size_t reserved = probe.Slots[index]->GetArena().GetReservedBytes();
                EXPECT_GT(reserved, 0u);
                EXPECT_LE(reserved, ObjectArenaOptions{}.MaxRetainedBytes);
            }
            EXPECT_EQ(probe.ArenaEvents, expected);
            scheduler->WaitIdle();
            EXPECT_EQ(probe.ArenaEvents, expected);
        }
        EXPECT_EQ(probe.ArenaEvents, expected);
        EXPECT_TRUE(probe.Destroyed);
    }
}

TEST_P(FrameLoop, ReusesCommandBuffersForBatchedAndSeparateSubmissions) {
    for (bool separately : {false, true}) {
        SCOPED_TRACE(separately);
        FrameProbe probe;
        probe.SubmitSeparately = separately;
        auto pipeline = MakePipeline(probe, 2);
        ASSERT_TRUE(pipeline);
        auto* copy = pipeline.Get();
        auto scheduler = FrameScheduler::Create(*Device, pipeline.Release());
        ASSERT_TRUE(scheduler);
        vector<vector<render::CommandBuffer*>> cached(2);
        vector<vector<uint32_t>> expected(2);
        const array<uint32_t, 8> counts{0, 1, 3, 2, 1, 0, 3, 2};
        for (uint32_t frame = 0; frame < counts.size(); ++frame) {
            const uint32_t slotIndex = frame % 2;
            probe.Value = 100 + frame;
            probe.CommandCount = counts[frame];
            scheduler->RunFrame(Source);
            const auto& commands = probe.Commands[slotIndex];
            EXPECT_EQ(probe.RecordedFrameIds.back(), frame);
            ASSERT_EQ(commands.size(), probe.CommandCount);
            for (uint32_t index = 0; index < commands.size(); ++index) {
                auto* command = commands[index];
                if (index == cached[slotIndex].size()) {
                    for (const auto& slotCommands : cached) {
                        for (auto* other : slotCommands) EXPECT_NE(command, other);
                    }
                    cached[slotIndex].push_back(command);
                    expected[slotIndex].push_back(probe.Value);
                } else {
                    EXPECT_EQ(command, cached[slotIndex][index]);
                    expected[slotIndex][index] = probe.Value;
                }
            }
        }
        scheduler->WaitIdle();
        for (uint32_t slotIndex = 0; slotIndex < 2; ++slotIndex) {
            for (uint32_t word = 0; word < expected[slotIndex].size(); ++word) {
                EXPECT_EQ(copy->ReadValue(slotIndex, word), expected[slotIndex][word]);
            }
        }
    }
}

TEST_P(FrameLoop, WaitIdlePreservesNextSlotAndSkipsUnusedSlots) {
    FrameProbe probe;
    auto pipeline = MakePipeline(probe, 3);
    ASSERT_TRUE(pipeline);
    auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), 3);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame(Source);
    scheduler->RunFrame(Source);
    probe.Events.clear();
    scheduler->WaitIdle();
    EXPECT_EQ(probe.Events, (vector<FrameEvent>{{0, 0}, {1, 0}}));
    probe.Events.clear();
    scheduler->WaitIdle();
    EXPECT_TRUE(probe.Events.empty());
    scheduler->RunFrame(Source);
    EXPECT_EQ(probe.Events, (vector<FrameEvent>{{2, 1}, {2, 2}}));
    probe.Events.clear();
    scheduler->RunFrame(Source);
    EXPECT_EQ(probe.Events, (vector<FrameEvent>{{0, 1}, {0, 2}}));
}

TEST_P(FrameLoop, BorrowsExternalScenesOnlyDuringExtractionAndRecording) {
    FrameProbe probe;
    auto pipeline = MakePipeline(probe, 3);
    ASSERT_TRUE(pipeline);
    auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), 3);
    ASSERT_TRUE(scheduler);
    Scene alternate = Source;
    for (uint32_t frame = 0; frame < 7; ++frame) {
        const Scene& source = frame % 2 == 0 ? Source : alternate;
        scheduler->RunFrame(source);
        for (auto* slot : probe.Slots) EXPECT_FALSE(slot->GetScene());
        EXPECT_EQ(source.Primitives.Count(), 1u);
    }
    scheduler->WaitIdle();
    for (auto* slot : probe.Slots) {
        EXPECT_FALSE(slot->GetScene());
        EXPECT_TRUE(slot->GetPassDrawList({0}).VisibleCommands.empty());
        EXPECT_TRUE(slot->GetPassDrawList({0}).DynamicContext.Commands.empty());
    }
}

TEST_P(FrameLoop, DestructionFinishesGpuUseBeforeDestroyingPipelineResources) {
    FrameProbe probe;
    auto pipeline = MakePipeline(probe, 3);
    ASSERT_TRUE(pipeline);
    {
        auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), 3);
        ASSERT_TRUE(scheduler);
        for (uint32_t index = 0; index < 3; ++index) {
            probe.Value = 73 + index;
            scheduler->RunFrame(Source);
        }
        probe.ReadOnDestroy = true;
        probe.Events.clear();
    }
    EXPECT_EQ(probe.Events, (vector<FrameEvent>{{0, 0}, {1, 0}, {2, 0}}));
    EXPECT_EQ(probe.ArenaEvents, (vector<FrameArenaEvent>{{0, 0}, {1, 0}, {2, 0}, {0, 2}, {0, 1}, {1, 2}, {1, 1}, {2, 2}, {2, 1}}));
    EXPECT_TRUE(probe.Destroyed);
    EXPECT_EQ(probe.DestroyedValues, (vector<std::optional<uint32_t>>{73u, 74u, 75u}));
}

TEST_P(FrameLoop, SupportsSynchronousCompletionWithoutFence) {
    FrameProbe probe;
    auto pipeline = MakePipeline(probe, 2, true);
    ASSERT_TRUE(pipeline);
    auto* copy = pipeline.Get();
    auto scheduler = FrameScheduler::Create(*Device, pipeline.Release());
    ASSERT_TRUE(scheduler);
    vector<FrameEvent> expected;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        const uint32_t index = frame % 2;
        probe.Value = frame + 1;
        if (frame == 2) expected.push_back({index, 0});
        for (int stage = 1; stage <= 2; ++stage) expected.push_back({index, stage});
        scheduler->RunFrame(Source);
        EXPECT_EQ(probe.Events, expected);
        EXPECT_EQ(copy->ReadValue(index), probe.Value);
    }
    scheduler->WaitIdle();
    expected.push_back({0, 0});
    expected.push_back({1, 0});
    EXPECT_EQ(probe.Events, expected);
    EXPECT_EQ(copy->ReadValue(0), 3u);
    EXPECT_EQ(copy->ReadValue(1), 2u);
}

TEST_P(FrameLoop, WaitIdleResetsAllCommandAllocatorsBeforeDestroyingArenaResources) {
    for (const bool arenaOwner : {true, false}) {
        SCOPED_TRACE(arenaOwner);
        bool released = false;
        auto pipeline = MakeSharedPipeline(released, 2, 123);
        ASSERT_TRUE(pipeline);
        auto* shared = pipeline.Get();
        shared->MoveOwnerToArena = arenaOwner;
        auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), 2);
        ASSERT_TRUE(scheduler);
        scheduler->RunFrame(Source);
        scheduler->RunFrame(Source);
        if (!arenaOwner) {
            shared->RetireFrameId = 2;
            scheduler->RunFrame(Source);
        }
        EXPECT_FALSE(released);
        scheduler->WaitIdle();
        EXPECT_TRUE(released);
        for (uint32_t index = 0; index < 2; ++index) EXPECT_EQ(shared->ReadValue(index), 123u);
        scheduler->WaitIdle();
    }
}

TEST_P(FrameLoop, RetiredSharedBufferWaitsForEverySlotDuringNormalRotation) {
    for (uint32_t slotCount = 1; slotCount <= 3; ++slotCount) {
        SCOPED_TRACE(slotCount);
        for (const bool retireInRecord : {false, true}) {
            SCOPED_TRACE(retireInRecord);
            bool released = false;
            auto pipeline = MakeSharedPipeline(released, slotCount, 123);
            ASSERT_TRUE(pipeline);
            auto* shared = pipeline.Get();
            shared->RetireFrameId = slotCount;
            shared->RetireInRecord = retireInRecord;
            auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), slotCount);
            ASSERT_TRUE(scheduler);
            for (uint32_t frame = 0; frame <= slotCount; ++frame) scheduler->RunFrame(Source);
            EXPECT_FALSE(shared->Owner);
            EXPECT_FALSE(released);
            for (uint32_t index = 1; index < slotCount; ++index) {
                scheduler->RunFrame(Source);
                EXPECT_FALSE(released);
            }
            scheduler->RunFrame(Source);
            EXPECT_TRUE(released);
            for (uint32_t index = 0; index < slotCount; ++index) EXPECT_EQ(shared->ReadValue(index), 123u);
            scheduler->WaitIdle();
        }
    }
}

TEST_P(FrameLoop, RetiredOwnersReclaimInReverseOrderOnReuseAndDestruction) {
    for (uint32_t slotCount = 1; slotCount <= 3; ++slotCount) {
        SCOPED_TRACE(slotCount);
        for (const int stage : {1, 2}) {
            SCOPED_TRACE(stage);
            FrameProbe probe;
            probe.RetirementStage = stage;
            vector<FrameArenaEvent> expected;
            {
                auto pipeline = MakePipeline(probe, slotCount);
                ASSERT_TRUE(pipeline);
                auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), slotCount);
                ASSERT_TRUE(scheduler);
                for (uint32_t frame = 0; frame < slotCount * 2; ++frame) {
                    if (frame >= slotCount) {
                        expected.push_back({frame - slotCount, 2});
                        expected.push_back({frame - slotCount, 1});
                    }
                    scheduler->RunFrame(Source);
                    EXPECT_EQ(probe.RetiredEvents, expected);
                }
            }
            for (uint32_t frame = slotCount; frame < slotCount * 2; ++frame) {
                expected.push_back({frame, 2});
                expected.push_back({frame, 1});
            }
            EXPECT_EQ(probe.RetiredEvents, expected);
            EXPECT_TRUE(probe.Destroyed);
        }
    }
}

TEST_P(FrameLoop, WaitIdleDrainsRetiredOwnersWithUnusedSlotsAndAllowsNewRetirement) {
    FrameProbe probe;
    probe.RetirementStage = 1;
    auto pipeline = MakePipeline(probe, 3);
    ASSERT_TRUE(pipeline);
    auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), 3);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame(Source);
    EXPECT_TRUE(probe.RetiredEvents.empty());
    scheduler->WaitIdle();
    const vector<FrameArenaEvent> first{{0, 2}, {0, 1}};
    EXPECT_EQ(probe.RetiredEvents, first);
    scheduler->WaitIdle();
    EXPECT_EQ(probe.RetiredEvents, first);
    scheduler->RunFrame(Source);
    EXPECT_EQ(probe.RetiredEvents, first);
    scheduler->WaitIdle();
    EXPECT_EQ(probe.RetiredEvents, (vector<FrameArenaEvent>{{0, 2}, {0, 1}, {1, 2}, {1, 1}}));
}

TEST_P(FrameLoop, RetiringOutsideExtractionAndRecordingTerminates) {
    FrameProbe probe;
    auto pipeline = MakePipeline(probe, 1);
    ASSERT_TRUE(pipeline);
    auto scheduler = FrameScheduler::Create(*Device, pipeline.Release(), 1);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame(Source);
    ASSERT_EQ(probe.Slots.size(), 1u);
    EXPECT_DEATH(probe.Slots[0]->RetireResource(make_unique<int>(42)), "");
    scheduler->WaitIdle();
}

TEST_P(FrameLoop, DeltaSyncPrecedesExtractionAndUnchangedFramesReuseCommands) {
    Scene scene;
    const auto material = scene.AddMaterial({});
    const auto primitive = scene.AddPrimitive();
    MeshBatch batch;
    batch.Material = material;
    batch.Elements.emplace_back();
    const auto batchHandle = scene.AddMeshBatch(primitive, std::move(batch));
    ASSERT_TRUE(batchHandle.IsValid());
    auto pipeline = make_unique<DeltaFramePipeline>();
    auto* delta = pipeline.get();
    auto scheduler = FrameScheduler::Create(*Device, std::move(pipeline));
    ASSERT_TRUE(scheduler);
    for (int frame = 0; frame < 2; ++frame) {
        auto edited = scene.Meshes.Get(batchHandle);
        edited.Elements[0].VertexOffset = frame + 1;
        ASSERT_TRUE(scene.UpdateMeshBatch(batchHandle, std::move(edited)));
        scheduler->RunFrame(scene);
        EXPECT_EQ(scene.Meshes.Get(batchHandle).Elements[0].VertexOffset, frame + 1);
        EXPECT_FALSE(scene.HasDirtyBatches());
        ASSERT_TRUE(delta->CurrentSlot);
        EXPECT_EQ(delta->CurrentSlot->GetIndex(), frame);
        EXPECT_EQ(delta->CurrentSlot->GetFrameId(), frame);
        EXPECT_FALSE(delta->CurrentSlot->GetScene());
    }
    EXPECT_EQ(delta->Processor.Calls.size(), 2u);
    const auto retained = scene.Meshes.Get(batchHandle).CachedCommands[0].CommandHandle;
    scheduler->RunFrame(scene);
    EXPECT_EQ(delta->Processor.Calls.size(), 2u);
    EXPECT_EQ(scene.Meshes.Get(batchHandle).CachedCommands[0].CommandHandle, retained);
    EXPECT_FALSE(delta->CurrentSlot->GetScene());
    scheduler->WaitIdle();
}

TEST_P(FrameLoop, ConstSceneOverloadLeavesCacheSynchronizationToCaller) {
    auto pipeline = make_unique<DeltaFramePipeline>();
    auto* delta = pipeline.get();
    delta->SkipGather = true;
    auto scheduler = FrameScheduler::Create(*Device, std::move(pipeline));
    ASSERT_TRUE(scheduler);
    const Scene& source = Source;
    scheduler->RunFrame(source);
    EXPECT_TRUE(delta->Processor.Calls.empty());
    EXPECT_TRUE(source.CachedDrawLists.empty());
    scheduler->WaitIdle();
}

vector<render::RenderBackend> FrameLoopBackends() {
    vector<render::RenderBackend> backends;
#if defined(RADRAY_ENABLE_D3D12)
    backends.push_back(render::RenderBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.push_back(render::RenderBackend::Vulkan);
#endif
    return backends;
}

INSTANTIATE_TEST_SUITE_P(Backends, FrameLoop, testing::ValuesIn(FrameLoopBackends()));

}  // namespace
}  // namespace radray
