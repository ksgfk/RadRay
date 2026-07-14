#include <gtest/gtest.h>

#include <cstring>

#include <radray/runtime/gpu_resource.h>

using namespace radray;

namespace {

class FakeBuffer final : public render::Buffer {
public:
    FakeBuffer(render::Device* device, render::BufferDescriptor desc)
        : _device(device), _desc(desc), _storage(desc.Size) {}

    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}

    void* Map(uint64_t offset, uint64_t size) noexcept override {
        Events.emplace_back("map");
        if (offset > _storage.size() || size > _storage.size() - offset) {
            return nullptr;
        }
        return _storage.data() + offset;
    }

    void Unmap() noexcept override { Events.emplace_back("unmap"); }

    void FlushMappedRange(render::BufferRange range) noexcept override {
        Events.emplace_back("flush");
        Flushes.emplace_back(range);
    }

    void InvalidateMappedRange(render::BufferRange range) noexcept override {
        Events.emplace_back("invalidate");
        Invalidates.emplace_back(range);
    }

    render::BufferDescriptor GetDesc() const noexcept override { return _desc; }
    render::Device* GetDevice() const noexcept override { return _device; }

    vector<string> Events;
    vector<render::BufferRange> Flushes;
    vector<render::BufferRange> Invalidates;

private:
    render::Device* _device;
    render::BufferDescriptor _desc;
    vector<byte> _storage;
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
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor&) noexcept override { return nullptr; }

    void FlushMappedRanges(std::span<const render::MappedBufferRange> ranges) noexcept override {
        ++FlushCallCount;
        LastRanges.assign(ranges.begin(), ranges.end());
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

    uint64_t FlushCallCount{0};
    vector<render::MappedBufferRange> LastRanges;
};

render::BufferDescriptor UploadDescriptor(uint64_t size = 1024) {
    return render::BufferDescriptor{
        .Size = size,
        .Memory = render::MemoryType::Upload,
        .Usage = render::BufferUse::MapWrite | render::BufferUse::CopySource,
        .Hints = render::ResourceHint::PersistentMap};
}

}  // namespace

TEST(HostWriteBatchTest, MergesOnlyAdjacentOrOverlappingLastRange) {
    FakeDevice device;
    FakeBuffer first{&device, UploadDescriptor()};
    FakeBuffer second{&device, UploadDescriptor()};
    HostWriteBatch batch;

    batch.Record(&first, render::BufferRange{16, 16});
    batch.Record(&first, render::BufferRange{32, 16});
    batch.Record(&first, render::BufferRange{24, 32});
    ASSERT_EQ(batch.GetRanges().size(), 1u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Offset, 16u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Size, 40u);

    batch.Record(&second, render::BufferRange{0, 8});
    batch.Record(&first, render::BufferRange{56, 8});
    ASSERT_EQ(batch.GetRanges().size(), 3u);
    EXPECT_EQ(batch.GetStats().CommitCount, 5u);
    EXPECT_EQ(batch.GetStats().RecordedRangeCount, 5u);
}

TEST(HostWriteBatchTest, TenThousandAdjacentRecordsRemainOneRange) {
    FakeDevice device;
    FakeBuffer buffer{&device, UploadDescriptor(10'000)};
    HostWriteBatch batch;

    for (uint64_t offset = 0; offset < 10'000; ++offset) {
        batch.Record(&buffer, render::BufferRange{offset, 1});
    }

    ASSERT_EQ(batch.GetRanges().size(), 1u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Size, 10'000u);
    EXPECT_EQ(batch.GetStats().CommitCount, 10'000u);
}

TEST(HostWriteBatchTest, NormalizesAllAndCountsZeroCommitWithoutRange) {
    FakeDevice device;
    FakeBuffer buffer{&device, UploadDescriptor(64)};
    HostWriteBatch batch;

    batch.Record(&buffer, render::BufferRange{16, render::BufferRange::All()});
    batch.Record(&buffer, render::BufferRange{64, 0});

    ASSERT_EQ(batch.GetRanges().size(), 1u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Offset, 16u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Size, 48u);
    EXPECT_EQ(batch.GetStats().CommitCount, 2u);
    EXPECT_EQ(batch.GetStats().CommittedBytes, 48u);
    EXPECT_EQ(batch.GetStats().RecordedRangeCount, 1u);
}

TEST(HostWriteBatchTest, FlushesOnceClearsRangesAndRetainsCumulativeStats) {
    FakeDevice device;
    FakeBuffer buffer{&device, UploadDescriptor()};
    HostWriteBatch batch;
    batch.RecordPageAllocation(1024);
    batch.Record(&buffer, render::BufferRange{0, 32});

    batch.Flush(device);
    ASSERT_EQ(device.FlushCallCount, 1u);
    ASSERT_EQ(device.LastRanges.size(), 1u);
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(batch.GetStats().FlushedRangeCount, 1u);
    EXPECT_EQ(batch.GetStats().PageCount, 1u);
    EXPECT_EQ(batch.GetStats().PageCapacityBytes, 1024u);

    batch.Flush(device);
    EXPECT_EQ(device.FlushCallCount, 2u);
    EXPECT_TRUE(device.LastRanges.empty());
    EXPECT_EQ(batch.GetStats().FlushedRangeCount, 1u);
}

TEST(HostWriteBatchTest, SealRejectsAllCommitsAndResetReopensBatch) {
    FakeDevice device;
    FakeBuffer buffer{&device, UploadDescriptor()};
    HostWriteBatch batch;
    batch.Seal();

    EXPECT_DEATH(batch.Record(&buffer, render::BufferRange{4, 0}), "");
    batch.Reset();
    EXPECT_NO_FATAL_FAILURE(batch.Record(&buffer, render::BufferRange{4, 4}));
}

TEST(HostWriteBatchTest, RejectsNonPersistentAndOutOfBoundsRanges) {
    FakeDevice device;
    auto invalidDesc = UploadDescriptor(64);
    invalidDesc.Hints = render::ResourceHint::None;
    FakeBuffer invalid{&device, invalidDesc};
    FakeBuffer valid{&device, UploadDescriptor(64)};
    HostWriteBatch batch;

    EXPECT_DEATH(batch.Record(&invalid, render::BufferRange{0, 4}), "");
    EXPECT_DEATH(batch.Record(&valid, render::BufferRange{60, 8}), "");
}

TEST(ScopedBufferMapTest, WriteFlushesBeforeUnmap) {
    FakeDevice device;
    auto desc = UploadDescriptor(64);
    desc.Hints = render::ResourceHint::None;
    FakeBuffer buffer{&device, desc};
    {
        ScopedBufferMap mapping{&buffer, render::BufferRange{8, 16}};
        ASSERT_TRUE(mapping);
        std::memset(mapping.Data(), 0x4a, 16);
    }

    EXPECT_EQ(buffer.Events, (vector<string>{"map", "flush", "unmap"}));
    ASSERT_EQ(buffer.Flushes.size(), 1u);
    EXPECT_EQ(buffer.Flushes[0].Offset, 8u);
    EXPECT_EQ(buffer.Flushes[0].Size, 16u);
}

TEST(ScopedBufferMapTest, ReadInvalidatesBeforeDataIsReturned) {
    FakeDevice device;
    FakeBuffer buffer{&device, render::BufferDescriptor{
                                   .Size = 64,
                                   .Memory = render::MemoryType::ReadBack,
                                   .Usage = render::BufferUse::MapRead}};
    {
        ScopedBufferMap mapping{&buffer, render::BufferRange{4, 12}};
        ASSERT_TRUE(mapping);
        EXPECT_EQ(buffer.Events, (vector<string>{"map", "invalidate"}));
    }

    EXPECT_EQ(buffer.Events, (vector<string>{"map", "invalidate", "unmap"}));
    ASSERT_EQ(buffer.Invalidates.size(), 1u);
    EXPECT_EQ(buffer.Invalidates[0].Offset, 4u);
    EXPECT_EQ(buffer.Invalidates[0].Size, 12u);
}
