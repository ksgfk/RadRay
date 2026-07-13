#include <gtest/gtest.h>

#include <radray/runtime/gpu_resource.h>

using namespace radray;

namespace {

struct FlushedRange {
    uint64_t Offset{0};
    uint64_t Size{0};
};

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

    void Unmap(uint64_t offset, uint64_t size) noexcept override {
        FlushedRanges.push_back(FlushedRange{offset, size});
    }

    void SetDebugName(std::string_view name) noexcept override { Name = name; }

    render::BufferDescriptor GetDesc() const noexcept override { return _desc; }

    uint32_t MapCount{0};
    vector<FlushedRange> FlushedRanges;
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

}  // namespace

TEST(DynamicCBufferArenaTest, BatchesDirtyAllocationsIntoOneFlush) {
    FakeDevice device;
    DynamicCBufferArena arena{
        &device,
        DynamicCBufferArena::Descriptor{
            .BasicSize = 1024,
            .Alignment = 256,
            .MaxResetSize = 4096,
            .NamePrefix = "dynamic_test"}};

    const auto first = arena.Allocate(100);
    const auto second = arena.Allocate(100);
    ASSERT_EQ(first.Target, second.Target);
    ASSERT_TRUE(arena.HasPendingHostWrites());

    auto* buffer = static_cast<FakeBuffer*>(first.Target);
    arena.FlushHostWrites();

    ASSERT_FALSE(arena.HasPendingHostWrites());
    ASSERT_EQ(buffer->FlushedRanges.size(), 1u);
    EXPECT_EQ(buffer->FlushedRanges[0].Offset, 0u);
    EXPECT_EQ(buffer->FlushedRanges[0].Size, 356u);
    EXPECT_TRUE(device.BufferDescriptors[0].Hints.HasFlag(render::ResourceHint::PersistentMap));
    EXPECT_TRUE(device.BufferDescriptors[0].Hints.HasFlag(render::ResourceHint::Dedicated));

    arena.FlushHostWrites();
    EXPECT_EQ(buffer->FlushedRanges.size(), 1u);
}

TEST(DynamicCBufferArenaTest, ReusesRetainedOverflowPagesAcrossResets) {
    FakeDevice device;
    DynamicCBufferArena arena{
        &device,
        DynamicCBufferArena::Descriptor{
            .BasicSize = 512,
            .Alignment = 256,
            .MaxResetSize = 512,
            .NamePrefix = "overflow_test"}};

    const auto first = arena.Allocate(100);
    const auto second = arena.Allocate(100);
    const auto overflow = arena.Allocate(100);
    ASSERT_EQ(first.Target, second.Target);
    ASSERT_NE(first.Target, overflow.Target);

    auto* firstPage = static_cast<FakeBuffer*>(first.Target);
    auto* overflowPage = static_cast<FakeBuffer*>(overflow.Target);
    arena.FlushHostWrites();
    ASSERT_EQ(firstPage->FlushedRanges.size(), 1u);
    EXPECT_EQ(firstPage->FlushedRanges[0].Size, 356u);
    ASSERT_EQ(overflowPage->FlushedRanges.size(), 1u);
    EXPECT_EQ(overflowPage->FlushedRanges[0].Size, 100u);

    arena.Reset();
    const auto reusedFirst = arena.Allocate(100);
    const auto reusedSecond = arena.Allocate(100);
    const auto reusedOverflow = arena.Allocate(100);
    EXPECT_EQ(reusedFirst.Target, first.Target);
    EXPECT_EQ(reusedSecond.Target, second.Target);
    EXPECT_EQ(reusedOverflow.Target, overflow.Target);
    EXPECT_EQ(device.BufferDescriptors.size(), 2u);
    EXPECT_EQ(device.DestroyedBufferCount, 0u);

    arena.FlushHostWrites();
    EXPECT_EQ(firstPage->FlushedRanges.size(), 2u);
    EXPECT_EQ(overflowPage->FlushedRanges.size(), 2u);
}

TEST(MaterialConstantPoolTest, BatchesWritesAndTracksReusedSlices) {
    FakeDevice device;
    MaterialConstantPool pool{&device, 512, 256};

    const auto first = pool.Allocate(16);
    const auto second = pool.Allocate(16);
    ASSERT_EQ(first.Target, second.Target);
    ASSERT_TRUE(pool.HasPendingHostWrites());

    auto* buffer = static_cast<FakeBuffer*>(first.Target);
    pool.FlushHostWrites();
    ASSERT_EQ(buffer->FlushedRanges.size(), 1u);
    EXPECT_EQ(buffer->FlushedRanges[0].Offset, 0u);
    EXPECT_EQ(buffer->FlushedRanges[0].Size, 272u);

    pool.Release(first);
    const auto reused = pool.Allocate(8);
    EXPECT_EQ(reused.Target, first.Target);
    EXPECT_EQ(reused.Offset, first.Offset);
    pool.FlushHostWrites();

    ASSERT_EQ(buffer->FlushedRanges.size(), 2u);
    EXPECT_EQ(buffer->FlushedRanges[1].Offset, 0u);
    EXPECT_EQ(buffer->FlushedRanges[1].Size, 8u);
}
