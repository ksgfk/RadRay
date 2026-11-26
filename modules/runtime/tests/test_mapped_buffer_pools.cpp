#include <gtest/gtest.h>

#include <algorithm>

#include <radray/runtime/dynamic_buffer_arena.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/staging_buffer_pool.h>

#include "test_fakes.h"

using namespace radray;
using namespace radray::gpu;

namespace {

struct MappingProbe {
    uint32_t LiveBuffers{0};
};

class ObservedBuffer final : public test::FakeBuffer {
public:
    ObservedBuffer(const BufferDescriptor& desc, MappingProbe& probe)
        : FakeBuffer(desc.Size, desc.Usage), _probe(probe), _bytes(static_cast<size_t>(desc.Size)) {
        Desc = desc;
        ++_probe.LiveBuffers;
    }
    ~ObservedBuffer() noexcept override {
        --_probe.LiveBuffers;
    }
    Nullable<void*> GetMappedData() noexcept override { return _bytes.data(); }

private:
    MappingProbe& _probe;
    vector<byte> _bytes;
};

class ObservedDevice final : public test::FakeDevice {
public:
    explicit ObservedDevice(MappingProbe& probe) : _probe(probe) {}
    bool RejectDeviceUpload{false};
    vector<MemoryType> Requests;

    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        Requests.push_back(desc.Memory);
        if (RejectDeviceUpload && desc.Memory == MemoryType::DeviceUpload) return nullptr;
        return make_unique<ObservedBuffer>(desc, _probe);
    }

private:
    MappingProbe& _probe;
};

}  // namespace

TEST(MappedBufferPools, GrowthPreservesEarlierAddressesAndResetReusesMappedPages) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    uint32_t maps = 0;
    device.OnMap = [&] { ++maps; };
    DynamicBufferArena arena{device, 16};
    auto first = arena.Allocate(16, 4, 16, BufferState::Vertex);
    ASSERT_TRUE(first);
    const auto desc = first->Buffer->GetDesc();
    EXPECT_EQ(desc.Memory, MemoryType::HostUpload);
    EXPECT_TRUE(desc.Usage.HasFlag(BufferUse::MapWrite | BufferUse::CopySource | BufferUse::Vertex | BufferUse::CBuffer));
    EXPECT_EQ(desc.Hints, ResourceHint::None);
    auto second = arena.Allocate(32, 4, 32, BufferState::CopySource);
    ASSERT_TRUE(second);
    EXPECT_NE(first->Buffer, second->Buffer);
    first->Data.Get()[0] = byte{42};
    EXPECT_EQ(first->Data.Get()[0], byte{42});
    EXPECT_EQ(arena.GetCapacity(), 48u);
    EXPECT_EQ(maps, 2u);
    ASSERT_TRUE(arena.Flush());
    arena.BeginFrame();
    auto reused = arena.Allocate(16, 4, 16, BufferState::Vertex);
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->Buffer, first->Buffer);
    EXPECT_EQ(reused->Data, first->Data);
    EXPECT_EQ(maps, 2u);
}

TEST(MappedBufferPools, SharedPageUnifiesReadStatesAndPaddingDoesNotOverlap) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    vector<BufferRange> flushed;
    device.OnFlush = [&](BufferRange range) { flushed.push_back(range); };
    DynamicBufferArena arena{device, 1024};
    const auto constants = arena.Allocate(80, 256, 256, BufferState::CBuffer);
    const auto vertices = arena.Allocate(16, 4, 16, BufferState::Vertex);
    const auto copy = arena.Allocate(4, 4, 4, BufferState::CopySource);
    ASSERT_TRUE(constants && vertices && copy);
    EXPECT_EQ(constants->Buffer, vertices->Buffer);
    EXPECT_EQ(copy->Buffer, vertices->Buffer);
    EXPECT_EQ(vertices->Offset, 256u);
    EXPECT_EQ(constants->Size, 80u);
    ASSERT_TRUE(arena.Flush());
    ASSERT_EQ(flushed.size(), 1u);
    EXPECT_EQ(flushed[0].Size, 276u);
    vector<ResourceBarrierDescriptor> barriers;
    arena.AppendReadBarriers(barriers);
    ASSERT_EQ(barriers.size(), 1u);
    EXPECT_EQ(std::get<BarrierBufferDescriptor>(barriers[0]).After, BufferState::CBuffer | BufferState::Vertex | BufferState::CopySource);
}

TEST(MappedBufferPools, StagingAndDynamicPagesRemainSeparateWithExactBufferUsage) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    DynamicBufferArena arena{device, 63};
    StagingBufferPool stagingPool{device, 63};
    const auto staging = stagingPool.Allocate(5, 4);
    const auto vertex = arena.Allocate(4, 4, 4, BufferState::Vertex);
    const auto index = arena.Allocate(4, 4, 4, BufferState::Index);
    ASSERT_TRUE(staging && vertex && index);
    EXPECT_EQ(staging->ReservedSize, 5u);
    EXPECT_NE(staging->Buffer, vertex->Buffer);
    EXPECT_EQ(vertex->Buffer, index->Buffer);
    constexpr auto stagingUsage = BufferUse::MapWrite | BufferUse::CopySource;
    EXPECT_EQ(staging->Buffer->GetDesc().Usage, stagingUsage);
    EXPECT_EQ(vertex->Buffer->GetDesc().Usage, stagingUsage | BufferUse::Vertex | BufferUse::Index | BufferUse::CBuffer | BufferUse::Resource | BufferUse::Indirect);
    EXPECT_EQ(staging->Buffer->GetDesc().Hints, ResourceHint::None);
    EXPECT_EQ(vertex->Buffer->GetDesc().Hints, ResourceHint::None);
    vector<ResourceBarrierDescriptor> barriers;
    stagingPool.AppendReadBarriers(barriers);
    arena.AppendReadBarriers(barriers);
    ASSERT_EQ(barriers.size(), 2u);
    const auto hasBarrier = [&](const UploadAllocation& slice, BufferStates reads) {
        return std::ranges::any_of(barriers, [&](const auto& entry) {
            const auto& barrier = std::get<BarrierBufferDescriptor>(entry);
            return barrier.Target == slice.Buffer.Get() && barrier.Before == BufferState::HostWrite && barrier.After == reads;
        });
    };
    EXPECT_TRUE(hasBarrier(*staging, BufferState::CopySource));
    EXPECT_TRUE(hasBarrier(*vertex, BufferState::Vertex | BufferState::Index));
    stagingPool.CommitSubmittedStates();
    arena.CommitSubmittedStates();
    stagingPool.BeginFrame();
    arena.BeginFrame();
    const auto nextVertex = arena.Allocate(4, 4, 4, BufferState::Vertex);
    const auto nextStaging = stagingPool.Allocate(4, 4);
    ASSERT_TRUE(nextVertex && nextStaging);
    EXPECT_EQ(nextVertex->Data, vertex->Data);
    EXPECT_EQ(nextStaging->Data, staging->Data);
}

TEST(MappedBufferPools, ZeroAndInvalidAllocationsDoNotMap) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    uint32_t maps = 0;
    device.OnMap = [&] { ++maps; };
    DynamicBufferArena arena{device, 64};
    StagingBufferPool stagingPool{device, 64};
    const auto empty = arena.Allocate(0, 1, 0, BufferState::Vertex);
    ASSERT_TRUE(empty);
    EXPECT_FALSE(empty->Buffer);
    const auto emptyStaging = stagingPool.Allocate(0, 0);
    ASSERT_TRUE(emptyStaging);
    EXPECT_FALSE(emptyStaging->Buffer);
    EXPECT_FALSE(stagingPool.Allocate(16, 0));
    EXPECT_FALSE(arena.Allocate(16, 0, 16, BufferState::Vertex));
    EXPECT_FALSE(arena.Allocate(16, 1, 8, BufferState::Vertex));
    EXPECT_FALSE(arena.Allocate(16, 1, 16, BufferState::UnorderedAccess));
    EXPECT_EQ(maps, 0u);
    EXPECT_EQ(arena.GetCapacity(), 0u);
}

TEST(MappedBufferPools, FailedPageCreationOrMappingPreservesExistingAllocation) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    DynamicBufferArena arena{device, 16};
    StagingBufferPool stagingPool{device, 16};
    const auto first = arena.Allocate(16, 4, 16, BufferState::Vertex);
    ASSERT_TRUE(first);
    first->Data.Get()[0] = byte{42};

    device.BuffersUntilFailure = 0;
    EXPECT_FALSE(stagingPool.Allocate(32, 4));
    device.BuffersUntilFailure = -1;
    device.FailMaps = true;
    EXPECT_FALSE(stagingPool.Allocate(32, 4));
    EXPECT_EQ(first->Data.Get()[0], byte{42});
    EXPECT_EQ(arena.GetCapacity(), 16u);
    ASSERT_TRUE(arena.Flush());

    stagingPool.BeginFrame();
    arena.BeginFrame();
    const auto reused = arena.Allocate(16, 4, 16, BufferState::Vertex);
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->Buffer, first->Buffer);
    EXPECT_EQ(reused->Data, first->Data);
}

TEST(MappedBufferPools, ClearAndDestructionReleasePages) {
    MappingProbe probe;
    ObservedDevice device{probe};
    {
        DynamicBufferArena arena{device, 16};
        StagingBufferPool stagingPool{device, 16};
        ASSERT_TRUE(arena.Allocate(16, 4, 16, BufferState::Vertex));
        ASSERT_TRUE(stagingPool.Allocate(32, 4));
        stagingPool.BeginFrame();
        arena.BeginFrame();
        EXPECT_EQ(probe.LiveBuffers, 2u);
        stagingPool.Clear();
        arena.Clear();
        EXPECT_EQ(arena.GetCapacity(), 0u);
        EXPECT_EQ(probe.LiveBuffers, 0u);
        stagingPool.Clear();
        arena.Clear();
        ASSERT_TRUE(arena.Allocate(16, 4, 16, BufferState::CBuffer));
    }
    EXPECT_EQ(probe.LiveBuffers, 0u);
}

TEST(MappedBufferPools, DeviceBudgetGatesNewPagesAndIdlePagesExpireAfterEightUnusedRecycles) {
    MappingProbe probe;
    ObservedDevice device{probe};
    device.Capabilities.Memory.HostVisibleDeviceLocalBytes = 4096;
    device.Budget.DeviceUpload = {1000, 750};
    DynamicBufferArena arena{device, 100};
    StagingBufferPool staging{device, 100};
    const auto allocate = [&](DynamicBufferResidency residency) {
        return arena.Allocate(50, 1, 50, BufferState::Vertex, residency);
    };
    auto local = allocate(DynamicBufferResidency::PreferDevice);
    auto stream = allocate(DynamicBufferResidency::HostOnly);
    ASSERT_TRUE(local && stream && staging.Allocate(100, 1));
    EXPECT_EQ(local->Buffer->GetDesc().Memory, MemoryType::DeviceUpload);
    EXPECT_EQ(stream->Buffer->GetDesc().Memory, MemoryType::HostUpload);
    ++device.Budget.DeviceUpload.UsageBytes;
    auto reused = allocate(DynamicBufferResidency::PreferDevice);
    auto spilled = allocate(DynamicBufferResidency::PreferDevice);
    ASSERT_TRUE(reused && spilled);
    EXPECT_EQ(reused->Buffer, local->Buffer);
    EXPECT_EQ(spilled->Buffer, stream->Buffer);
    for (int i = 0; i < 8; ++i) {
        arena.BeginFrame();
        staging.BeginFrame();
        ASSERT_EQ(arena.GetCapacity(), 200u);
        ASSERT_EQ(staging.GetCapacity(), 100u);
        ASSERT_TRUE(allocate(DynamicBufferResidency::HostOnly));
    }
    arena.BeginFrame();
    staging.BeginFrame();
    EXPECT_EQ(arena.GetCapacity(), 100u);
    EXPECT_EQ(staging.GetCapacity(), 0u);
    EXPECT_EQ(probe.LiveBuffers, 1u);
}

TEST(MappedBufferPools, FailedDeviceAllocationRetriesOnlyWithFallbackPolicy) {
    MappingProbe probe;
    ObservedDevice device{probe};
    device.Capabilities.Memory.HostVisibleDeviceLocalBytes = 1024;
    device.RejectDeviceUpload = true;
    DynamicBufferArena arena{device, 64};
    auto slice = arena.Allocate(64, 4, 64, BufferState::CBuffer);
    ASSERT_TRUE(slice);
    EXPECT_EQ(slice->Buffer->GetDesc().Memory, MemoryType::HostUpload);
    EXPECT_EQ(device.Requests, (vector<MemoryType>{MemoryType::DeviceUpload, MemoryType::HostUpload}));
    device.Requests.clear();
    EXPECT_FALSE(GpuBuffer::Create(device, {64, BufferUse::Vertex, GpuBufferMemory::DeviceUploadStrict}));
    auto persistent = GpuBuffer::Create(device, {64, BufferUse::Vertex, GpuBufferMemory::DeviceUploadWithFallback});
    ASSERT_TRUE(persistent);
    EXPECT_TRUE(persistent->IsHostVisible());
    EXPECT_FALSE(persistent->GetMemoryProperties().IsDeviceLocal);
    EXPECT_EQ(device.Requests, (vector<MemoryType>{MemoryType::DeviceUpload, MemoryType::DeviceUpload, MemoryType::HostUpload}));
    device.RejectDeviceUpload = false;
    device.Budget.DeviceUpload = {100, 22};
    device.Requests.clear();
    EXPECT_FALSE(GpuBuffer::Create(device, {64, BufferUse::Vertex, GpuBufferMemory::DeviceUploadStrict}));
    EXPECT_TRUE(device.Requests.empty());
    device.Budget.DeviceUpload.UsageBytes = 21;
    auto strict = GpuBuffer::Create(device, {64, BufferUse::Vertex, GpuBufferMemory::DeviceUploadStrict});
    ASSERT_TRUE(strict);
    EXPECT_TRUE(strict->GetMemoryProperties().IsDeviceLocal);
    EXPECT_EQ(strict->GetSubmittedState(), BufferState::HostWrite);
}
