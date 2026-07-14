#include <gtest/gtest.h>

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
    void FlushMappedRange(render::BufferRange) noexcept override {}
    void InvalidateMappedRange(render::BufferRange) noexcept override {}

    void SetDebugName(std::string_view name) noexcept override { Name = name; }
    render::BufferDescriptor GetDesc() const noexcept override { return _desc; }
    render::Device* GetDevice() const noexcept override { return nullptr; }

    uint32_t MapCount{0};
    uint32_t UnmapCount{0};
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

    void FlushMappedRanges(std::span<const render::MappedBufferRange>) noexcept override {}

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
    uint32_t DestroyedBufferCount{0};
};

StagingBufferPool::Descriptor MakePoolDescriptor() {
    return StagingBufferPool::Descriptor{
        .PageSize = 1024,
        .MaxCachedBytes = 2048,
        .MaxCachedPages = 2};
}

}  // namespace

TEST(StagingBufferPoolTest, SuballocatesAlignedRangesAndCommitsToFlightBatch) {
    FakeDevice device;
    HostWriteBatch batch;
    StagingBufferPool pool{&device, 2, MakePoolDescriptor()};
    pool.BeginFlight(batch);

    auto firstReservation = pool.Reserve(100, 4);
    auto secondReservation = pool.Reserve(100, 256);
    const auto first = firstReservation.Commit(100);
    const auto second = secondReservation.Commit(100);

    ASSERT_NE(first.Target, nullptr);
    EXPECT_EQ(first.Target, second.Target);
    EXPECT_EQ(first.Offset, 0u);
    EXPECT_EQ(second.Offset, 256u);
    ASSERT_EQ(batch.GetRanges().size(), 2u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Offset, 0u);
    EXPECT_EQ(batch.GetRanges()[1].Range.Offset, 256u);

    ASSERT_EQ(device.BufferDescriptors.size(), 1u);
    const auto& desc = device.BufferDescriptors.front();
    EXPECT_EQ(desc.Size, 1024u);
    EXPECT_TRUE(desc.Hints.HasFlag(render::ResourceHint::PersistentMap));
    EXPECT_FALSE(desc.Hints.HasFlag(render::ResourceHint::Dedicated));
    EXPECT_EQ(static_cast<FakeBuffer*>(first.Target)->MapCount, 1u);
}

TEST(StagingBufferPoolTest, ReusesPageOnlyAfterFlightCollection) {
    FakeDevice device;
    HostWriteBatch firstBatch;
    HostWriteBatch secondBatch;
    StagingBufferPool pool{&device, 2, MakePoolDescriptor()};

    pool.BeginFlight(firstBatch);
    auto firstReservation = pool.Reserve(128, 16);
    const auto first = firstReservation.Commit(128);
    render::Buffer* firstBuffer = first.Target;
    pool.RetireToFlight(0);

    pool.BeginFlight(secondBatch);
    auto whilePendingReservation = pool.Reserve(128, 16);
    const auto whilePending = whilePendingReservation.Commit(128);
    EXPECT_NE(whilePending.Target, firstBuffer);
    EXPECT_EQ(device.BufferDescriptors.size(), 2u);
    pool.RetireToFlight(1);

    pool.CollectFlight(0);
    firstBatch.Reset();
    pool.BeginFlight(firstBatch);
    auto reusedReservation = pool.Reserve(64, 16);
    const auto reused = reusedReservation.Commit(64);
    EXPECT_EQ(reused.Target, firstBuffer);
    EXPECT_EQ(reused.Offset, 0u);
    EXPECT_EQ(device.BufferDescriptors.size(), 2u);
    EXPECT_EQ(static_cast<FakeBuffer*>(reused.Target)->MapCount, 1u);
}

TEST(StagingBufferPoolTest, ReleasesOversizedBuffersInsteadOfCachingThem) {
    FakeDevice device;
    HostWriteBatch batch;
    StagingBufferPool pool{&device, 1, MakePoolDescriptor()};
    pool.BeginFlight(batch);

    auto firstReservation = pool.Reserve(1025, 256);
    const auto first = firstReservation.Commit(1025);
    ASSERT_NE(first.Target, nullptr);
    EXPECT_EQ(first.Offset, 0u);
    ASSERT_EQ(device.BufferDescriptors.size(), 1u);
    EXPECT_EQ(device.BufferDescriptors[0].Size, 1280u);
    EXPECT_TRUE(device.BufferDescriptors[0].Hints.HasFlag(render::ResourceHint::PersistentMap));
    EXPECT_FALSE(device.BufferDescriptors[0].Hints.HasFlag(render::ResourceHint::Dedicated));

    pool.RetireToFlight(0);
    pool.CollectFlight(0);
    EXPECT_EQ(device.DestroyedBufferCount, 1u);

    batch.Reset();
    pool.BeginFlight(batch);
    auto secondReservation = pool.Reserve(1025, 256);
    const auto second = secondReservation.Commit(1025);
    EXPECT_NE(second.Target, nullptr);
    EXPECT_EQ(device.BufferDescriptors.size(), 2u);
}
