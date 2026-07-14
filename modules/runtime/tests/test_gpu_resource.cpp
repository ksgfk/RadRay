#include <gtest/gtest.h>

#include <cstring>

#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/gpu_system.h>

using namespace radray;

namespace {

class FakeBuffer final : public render::Buffer {
public:
    FakeBuffer(const render::BufferDescriptor& desc, uint32_t* destroyedCount)
        : _desc(desc), _storage(desc.Size), _destroyedCount(destroyedCount) {}

    ~FakeBuffer() noexcept override {
        if (_destroyedCount != nullptr) {
            ++*_destroyedCount;
        }
    }

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override {
        _valid = false;
        _storage.clear();
    }

    void* Map(uint64_t offset, uint64_t size) noexcept override {
        if (offset > _storage.size() || size > _storage.size() - offset) {
            return nullptr;
        }
        ++MapCount;
        return _storage.data() + offset;
    }

    void Unmap() noexcept override { ++UnmapCount; }

    void FlushMappedRange(render::BufferRange range) noexcept override {
        Flushes.push_back(render::MappedBufferRange{this, range});
    }

    void InvalidateMappedRange(render::BufferRange range) noexcept override {
        Invalidates.push_back(render::MappedBufferRange{this, range});
    }

    void SetDebugName(std::string_view name) noexcept override { Name = name; }

    render::BufferDescriptor GetDesc() const noexcept override { return _desc; }
    render::Device* GetDevice() const noexcept override { return nullptr; }

    uint32_t MapCount{0};
    uint32_t UnmapCount{0};
    vector<render::MappedBufferRange> Flushes;
    vector<render::MappedBufferRange> Invalidates;
    string Name;

private:
    render::BufferDescriptor _desc;
    vector<byte> _storage;
    uint32_t* _destroyedCount;
    bool _valid{true};
};

class FakeDevice final : public render::Device {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::RenderBackend GetBackend() noexcept override { return render::RenderBackend::Vulkan; }
    render::DeviceDetail GetDetail() const noexcept override { return {}; }

    Nullable<render::CommandQueue*> GetCommandQueue(render::QueueType, uint32_t) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::CommandBuffer>> CreateCommandBuffer(render::CommandQueue*) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Fence>> CreateFence() noexcept override { return nullptr; }
    Nullable<unique_ptr<render::QueryPool>> CreateQueryPool(const render::QueryPoolDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::SwapChain>> CreateSwapChain(const render::SwapChainDescriptor&) noexcept override { return nullptr; }

    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& desc) noexcept override {
        BufferDescriptors.emplace_back(desc);
        unique_ptr<render::Buffer> buffer = make_unique<FakeBuffer>(desc, &DestroyedBufferCount);
        return buffer;
    }

    void FlushMappedRanges(std::span<const render::MappedBufferRange> ranges) noexcept override {
        FlushedRanges.assign(ranges.begin(), ranges.end());
    }

    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::RenderPass>> CreateRenderPass(const render::RenderPassDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Framebuffer>> CreateFramebuffer(const render::FramebufferDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::PipelineLayout>> CreatePipelineLayout(const render::PipelineLayoutDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::DescriptorPool>> CreateDescriptorPool(const render::DescriptorPoolDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::BindingGroup>> CreateBindingGroup(
        render::DescriptorPool*,
        render::PipelineLayout*,
        uint32_t) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(
        const render::GraphicsPipelineStateDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::ComputePipelineState>> CreateComputePipelineState(
        const render::ComputePipelineStateDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::AccelerationStructure>> CreateAccelerationStructure(
        const render::AccelerationStructureDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::AccelerationStructureView>> CreateAccelerationStructureView(
        const render::AccelerationStructureViewDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::RayTracingPipelineState>> CreateRayTracingPipelineState(
        const render::RayTracingPipelineStateDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::ShaderBindingTable>> CreateShaderBindingTable(
        const render::ShaderBindingTableDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::Sampler>> CreateSampler(const render::SamplerDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::BindlessArray>> CreateBindlessArray(const render::BindlessArrayDescriptor&) noexcept override { return nullptr; }

    vector<render::BufferDescriptor> BufferDescriptors;
    vector<render::MappedBufferRange> FlushedRanges;
    uint32_t DestroyedBufferCount{0};
};

DynamicCBufferArena MakeArena(
    FakeDevice& device,
    HostWriteBatch& batch,
    uint64_t pageSize = 1024,
    uint64_t alignment = 1,
    uint64_t maxResetSize = 4096) {
    return DynamicCBufferArena{
        &device,
        &batch,
        DynamicCBufferArena::Descriptor{
            .BasicSize = pageSize,
            .Alignment = alignment,
            .MaxResetSize = maxResetSize,
            .NamePrefix = "dynamic_test"}};
}

}  // namespace

TEST(MappedUploadPageTest, CommitRecordsOnlyActualPrefix) {
    FakeDevice device;
    HostWriteBatch batch;
    auto buffer = device.CreateBuffer(render::BufferDescriptor{
                                          .Size = 256,
                                          .Memory = render::MemoryType::Upload,
                                          .Usage = render::BufferUse::MapWrite | render::BufferUse::CopySource,
                                          .Hints = render::ResourceHint::PersistentMap})
                      .Unwrap();
    MappedUploadPage page{std::move(buffer), &batch};

    auto reservation = page.Reserve(128, 16, batch);
    ASSERT_TRUE(reservation.IsValid());
    std::memset(reservation.Data(), 0x5a, 37);
    const auto allocation = reservation.Commit(37);

    EXPECT_EQ(allocation.Offset, 0u);
    EXPECT_EQ(allocation.Size, 37u);
    ASSERT_EQ(batch.GetRanges().size(), 1u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Size, 37u);
    EXPECT_EQ(batch.GetStats().PageCount, 1u);
    EXPECT_EQ(batch.GetStats().PageCapacityBytes, 256u);
    EXPECT_EQ(batch.GetStats().CommitCount, 1u);
    EXPECT_EQ(batch.GetStats().CommittedBytes, 37u);
}

TEST(MappedUploadPageTest, ZeroCommitProducesNoRange) {
    FakeDevice device;
    HostWriteBatch batch;
    auto arena = MakeArena(device, batch);

    auto reservation = arena.Reserve(64);
    const auto allocation = reservation.Commit(0);
    EXPECT_TRUE(allocation.IsValid());
    EXPECT_EQ(allocation.Size, 0u);
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(batch.GetStats().CommitCount, 1u);
    EXPECT_EQ(batch.GetStats().CommittedBytes, 0u);
}

#ifdef RADRAY_IS_DEBUG
TEST(MappedUploadPageTest, DestroyingUncommittedReservationFails) {
    EXPECT_DEATH(
        {
            FakeDevice device;
            HostWriteBatch batch;
            auto arena = MakeArena(device, batch);
            auto reservation = arena.Reserve(16);
            (void)reservation;
        },
        "");
}

TEST(MappedUploadPageTest, ZeroCommitAfterSealFails) {
    EXPECT_DEATH(
        {
            FakeDevice device;
            HostWriteBatch batch;
            auto arena = MakeArena(device, batch);
            auto reservation = arena.Reserve(16);
            batch.Seal();
            reservation.Commit(0);
        },
        "");
}
#endif

TEST(DynamicCBufferArenaTest, CoalescesAdjacentCommitsAndKeepsSparseWritesSeparate) {
    FakeDevice device;
    HostWriteBatch batch;
    auto arena = MakeArena(device, batch, 1024, 1);

    auto firstReservation = arena.Reserve(32);
    auto secondReservation = arena.Reserve(32);
    const auto first = firstReservation.Commit(32);
    const auto second = secondReservation.Commit(32);
    ASSERT_EQ(first.Target, second.Target);
    ASSERT_EQ(batch.GetRanges().size(), 1u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Offset, 0u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Size, 64u);

    batch.Reset();
    auto sparseArena = MakeArena(device, batch, 1024, 256);
    auto sparseFirstReservation = sparseArena.Reserve(100);
    auto sparseSecondReservation = sparseArena.Reserve(100);
    sparseFirstReservation.Commit(100);
    sparseSecondReservation.Commit(100);
    ASSERT_EQ(batch.GetRanges().size(), 2u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Offset, 0u);
    EXPECT_EQ(batch.GetRanges()[1].Range.Offset, 256u);

    ASSERT_GE(device.BufferDescriptors.size(), 2u);
    EXPECT_TRUE(device.BufferDescriptors[0].Hints.HasFlag(render::ResourceHint::PersistentMap));
    EXPECT_FALSE(device.BufferDescriptors[0].Hints.HasFlag(render::ResourceHint::Dedicated));
}

TEST(DynamicCBufferArenaTest, ReusesRetainedOverflowPagesAcrossResets) {
    FakeDevice device;
    HostWriteBatch batch;
    auto arena = MakeArena(device, batch, 512, 256, 512);

    auto firstReservation = arena.Reserve(100);
    auto secondReservation = arena.Reserve(100);
    auto overflowReservation = arena.Reserve(100);
    const auto first = firstReservation.Commit(100);
    const auto second = secondReservation.Commit(100);
    const auto overflow = overflowReservation.Commit(100);
    ASSERT_EQ(first.Target, second.Target);
    ASSERT_NE(first.Target, overflow.Target);

    batch.Reset();
    arena.Reset();
    auto reusedFirstReservation = arena.Reserve(100);
    auto reusedSecondReservation = arena.Reserve(100);
    auto reusedOverflowReservation = arena.Reserve(100);
    const auto reusedFirst = reusedFirstReservation.Commit(100);
    const auto reusedSecond = reusedSecondReservation.Commit(100);
    const auto reusedOverflow = reusedOverflowReservation.Commit(100);
    EXPECT_EQ(reusedFirst.Target, first.Target);
    EXPECT_EQ(reusedSecond.Target, second.Target);
    EXPECT_EQ(reusedOverflow.Target, overflow.Target);
    EXPECT_EQ(device.BufferDescriptors.size(), 2u);
    EXPECT_EQ(device.DestroyedBufferCount, 0u);
}

TEST(MaterialConstantPoolTest, ReusesReleasedSlicesAndRecordsCurrentBatch) {
    FakeDevice device;
    HostWriteBatch firstBatch;
    HostWriteBatch secondBatch;
    MaterialConstantPool pool{&device, 512, 256};

    auto firstReservation = pool.Reserve(16, firstBatch);
    auto secondReservation = pool.Reserve(16, firstBatch);
    const auto first = firstReservation.Commit(16);
    const auto second = secondReservation.Commit(16);
    ASSERT_EQ(first.Target, second.Target);
    ASSERT_EQ(firstBatch.GetRanges().size(), 2u);

    pool.Release(first);
    auto reusedReservation = pool.Reserve(8, secondBatch);
    const auto reused = reusedReservation.Commit(8);
    EXPECT_EQ(reused.Target, first.Target);
    EXPECT_EQ(reused.Offset, first.Offset);
    ASSERT_EQ(secondBatch.GetRanges().size(), 1u);
    EXPECT_EQ(secondBatch.GetRanges()[0].Range.Offset, first.Offset);
    EXPECT_EQ(secondBatch.GetRanges()[0].Range.Size, 8u);
}
