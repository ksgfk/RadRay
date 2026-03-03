#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include <radray/render/render_graph_executor.h>

using namespace radray::render;
using namespace radray;

namespace {

RGTextureDescriptor MakeTextureDesc(std::string_view name, uint32_t mipLevels = 1) {
    RGTextureDescriptor desc{};
    desc.Width = 4;
    desc.Height = 4;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = mipLevels;
    desc.SampleCount = 1;
    desc.Format = TextureFormat::RGBA8_UNORM;
    desc.Name = name;
    return desc;
}

class FakeBuffer final : public Buffer {
public:
    explicit FakeBuffer(const BufferDescriptor& desc) noexcept
        : _desc(desc) {}

    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void* Map(uint64_t offset, uint64_t size) noexcept override {
        (void)offset;
        (void)size;
        return nullptr;
    }
    void Unmap(uint64_t offset, uint64_t size) noexcept override {
        (void)offset;
        (void)size;
    }
    BufferDescriptor GetDesc() const noexcept override { return _desc; }

private:
    BufferDescriptor _desc{};
};

class FakeTexture final : public Texture {
public:
    explicit FakeTexture(const TextureDescriptor& desc) noexcept
        : _desc(desc) {}

    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    TextureDescriptor GetDesc() const noexcept override { return _desc; }

private:
    TextureDescriptor _desc{};
};

class FakeCommandBuffer final : public CommandBuffer {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void Begin() noexcept override { BeginCount += 1; }
    void End() noexcept override { EndCount += 1; }
    void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept override {
        BarrierCalls += 1;
        BarrierCount += static_cast<uint32_t>(barriers.size());
    }
    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    void EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept override { (void)encoder; }
    Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept override { return nullptr; }
    void EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept override { (void)encoder; }
    Nullable<unique_ptr<RayTracingCommandEncoder>> BeginRayTracingPass() noexcept override { return nullptr; }
    void EndRayTracingPass(unique_ptr<RayTracingCommandEncoder> encoder) noexcept override { (void)encoder; }
    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override {
        (void)dst;
        (void)dstOffset;
        (void)src;
        (void)srcOffset;
        (void)size;
    }
    void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept override {
        (void)dst;
        (void)dstRange;
        (void)src;
        (void)srcOffset;
    }
    void CopyTextureToBuffer(Buffer* dst, uint64_t dstOffset, Texture* src, SubresourceRange srcRange) noexcept override {
        (void)dst;
        (void)dstOffset;
        (void)src;
        (void)srcRange;
    }

public:
    uint32_t BeginCount{0};
    uint32_t EndCount{0};
    uint32_t BarrierCalls{0};
    uint32_t BarrierCount{0};
};

class FakeCommandQueue final : public CommandQueue {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override {
        SubmitCount += 1;
        for (CommandBuffer* cmd : desc.CmdBuffers) {
            auto* fake = static_cast<FakeCommandBuffer*>(cmd);
            SubmittedBarrierCount += fake->BarrierCount;
            SubmittedBarrierCalls += fake->BarrierCalls;
            SubmittedBeginCount += fake->BeginCount;
            SubmittedEndCount += fake->EndCount;
        }
    }
    void Wait() noexcept override { WaitCount += 1; }

public:
    uint32_t SubmitCount{0};
    uint32_t WaitCount{0};
    uint32_t SubmittedBarrierCount{0};
    uint32_t SubmittedBarrierCalls{0};
    uint32_t SubmittedBeginCount{0};
    uint32_t SubmittedEndCount{0};
};

class FakeDevice final : public Device {
public:
    RenderBackend GetBackend() noexcept override { return RenderBackend::Metal; }
    DeviceDetail GetDetail() const noexcept override { return {}; }
    Nullable<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override {
        (void)type;
        (void)slot;
        return &_queue;
    }
    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept override {
        (void)queue;
        return make_unique<FakeCommandBuffer>();
    }
    Nullable<unique_ptr<Fence>> CreateFence() noexcept override { return nullptr; }
    Nullable<unique_ptr<Semaphore>> CreateSemaphoreDevice() noexcept override { return nullptr; }
    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        return make_unique<FakeBuffer>(desc);
    }
    Nullable<unique_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override {
        return make_unique<FakeTexture>(desc);
    }
    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<AccelerationStructure>> CreateAccelerationStructure(const AccelerationStructureDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<AccelerationStructureView>> CreateAccelerationStructureView(const AccelerationStructureViewDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<RayTracingPipelineState>> CreateRayTracingPipelineState(const RayTracingPipelineStateDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<ShaderBindingTable>> CreateShaderBindingTable(const ShaderBindingTableDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<DescriptorSet>> CreateDescriptorSet(RootSignature* rootSig, uint32_t index) noexcept override {
        (void)rootSig;
        (void)index;
        return nullptr;
    }
    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }
    Nullable<unique_ptr<BindlessArray>> CreateBindlessArray(const BindlessArrayDescriptor& desc) noexcept override {
        (void)desc;
        return nullptr;
    }

    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}

public:
    FakeCommandQueue _queue{};
};

}  // namespace

TEST(RenderGraphExecutor, executes_callbacks_and_dispatches_barriers) {
    RGGraphBuilder graph{};

    const auto src = graph.CreateTexture(MakeTextureDesc("src"));
    const auto out = graph.CreateTexture(MakeTextureDesc("out"));

    uint32_t executeCount = 0;
    auto passA = graph.AddPass("pass_a");
    passA.WriteTexture(src);
    passA.SetExecuteFunc([&](RGPassContext& ctx) {
        EXPECT_NE(ctx.Cmd, nullptr);
        EXPECT_NE(ctx.Registry, nullptr);
        EXPECT_NE(ctx.GetTexture(src), nullptr);
        executeCount += 1;
    });

    auto passB = graph.AddPass("pass_b");
    passB.ReadTexture(src);
    passB.WriteTexture(out);
    passB.SetExecuteFunc([&](RGPassContext& ctx) {
        EXPECT_NE(ctx.Cmd, nullptr);
        EXPECT_NE(ctx.GetTexture(out), nullptr);
        executeCount += 1;
    });
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 2u);

    auto device = make_shared<FakeDevice>();
    RGRegistry registry{device};
    auto executor = RGExecutor::Create(device);
    ASSERT_NE(executor, nullptr);

    FakeCommandBuffer cmd{};
    cmd.Begin();
    EXPECT_TRUE(executor->Record(&cmd, graph, compiled, &registry));
    cmd.End();
    EXPECT_EQ(executeCount, 2u);
    EXPECT_EQ(cmd.BeginCount, 1u);
    EXPECT_EQ(cmd.EndCount, 1u);
    EXPECT_GE(cmd.BarrierCalls, 1u);
    EXPECT_GE(cmd.BarrierCount, 1u);
}

TEST(RenderGraphExecutor, external_resource_requires_registry_import) {
    RGGraphBuilder graph{};

    const auto ext = graph.ImportExternalBuffer("external_data");
    const auto out = graph.CreateTexture(MakeTextureDesc("out"));

    auto pass = graph.AddPass("consume_external");
    pass.ReadBuffer(ext, {}, RGAccessMode::StorageRead);
    pass.WriteTexture(out);
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);

    auto device = make_shared<FakeDevice>();
    RGRegistry registry{device};
    auto executor = RGExecutor::Create(device);
    ASSERT_NE(executor, nullptr);

    FakeCommandBuffer cmd{};
    cmd.Begin();
    EXPECT_FALSE(executor->Record(&cmd, graph, compiled, &registry));
    cmd.End();

    BufferDescriptor desc{};
    desc.Size = 256;
    desc.Memory = MemoryType::Device;
    desc.Usage = BufferUse::Resource;
    desc.Hints = ResourceHint::None;
    desc.Name = "external_data_physical";
    auto externalBuffer = device->CreateBuffer(desc).Unwrap();
    ASSERT_TRUE(registry.ImportPhysicalBuffer(ext, externalBuffer.get()));

    cmd.Begin();
    EXPECT_TRUE(executor->Record(&cmd, graph, compiled, &registry));
    cmd.End();
}

TEST(RenderGraphExecutor, queue_class_mismatch_fails_when_validation_enabled) {
    RGGraphBuilder graph{};
    const auto out = graph.CreateTexture(MakeTextureDesc("out"));

    auto pass = graph.AddPass("direct_pass", QueueType::Direct);
    pass.WriteTexture(out);
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);

    auto device = make_shared<FakeDevice>();
    RGRegistry registry{device};
    auto executor = RGExecutor::Create(device);
    ASSERT_NE(executor, nullptr);

    FakeCommandBuffer cmd{};
    cmd.Begin();
    RGRecordOptions options{};
    options.ValidateQueueClass = true;
    options.RecordQueueClass = QueueType::Compute;
    EXPECT_FALSE(executor->Record(&cmd, graph, compiled, &registry, options));
    cmd.End();
}

TEST(RenderGraphExecutor, can_disable_barrier_emission) {
    RGGraphBuilder graph{};

    const auto src = graph.CreateTexture(MakeTextureDesc("src"));
    const auto out = graph.CreateTexture(MakeTextureDesc("out"));

    uint32_t executeCount = 0;
    auto passA = graph.AddPass("pass_a");
    passA.WriteTexture(src);
    passA.SetExecuteFunc([&](RGPassContext&) { executeCount += 1; });

    auto passB = graph.AddPass("pass_b");
    passB.ReadTexture(src);
    passB.WriteTexture(out);
    passB.SetExecuteFunc([&](RGPassContext&) { executeCount += 1; });
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);

    auto device = make_shared<FakeDevice>();
    RGRegistry registry{device};
    auto executor = RGExecutor::Create(device);
    ASSERT_NE(executor, nullptr);

    FakeCommandBuffer cmd{};
    cmd.Begin();
    RGRecordOptions options{};
    options.EmitBarriers = false;
    EXPECT_TRUE(executor->Record(&cmd, graph, compiled, &registry, options));
    cmd.End();

    EXPECT_EQ(executeCount, 2u);
    EXPECT_EQ(cmd.BarrierCalls, 0u);
    EXPECT_EQ(cmd.BarrierCount, 0u);
}
