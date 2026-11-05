#pragma once

#include <functional>
#include <string_view>

#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/gpu_device.h>

// CPU-side stand-ins for the RHI objects runtime touches. Each fake records only what tests observe.
namespace radray::test {

class FakeBuffer : public render::Buffer {
public:
    FakeBuffer(uint64_t size, render::BufferUses usage) noexcept : Desc{.Size = size, .Usage = usage} {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void* Map(uint64_t, uint64_t) noexcept override { return nullptr; }
    void Unmap() noexcept override {}
    void FlushMappedRange(render::BufferRange) noexcept override {}
    void InvalidateMappedRange(render::BufferRange) noexcept override {}
    render::BufferDescriptor GetDesc() const noexcept override { return Desc; }
    render::Device* GetDevice() const noexcept override { return nullptr; }

    render::BufferDescriptor Desc;
};

class FakePipeline final : public render::GraphicsPipelineState {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
};

class FakeParameterSet final : public render::ShaderParameterSet {
public:
    bool IsValid() const noexcept override { return Valid; }
    void Destroy() noexcept override {}
    bool Set(uint32_t, uint32_t, render::ShaderParameterValue) noexcept override { return true; }
    bool FlushWrites() noexcept override { return true; }
    void ResetBindingsForReuse() noexcept override {}

    bool Valid{true};
};

struct RecordedParameterBind {
    uint32_t Group;
    render::ShaderParameterSet* Set;
};

class RecordingEncoder final : public render::GraphicsCommandEncoder {
public:
    bool IsValid() const noexcept override { return Valid; }
    void Destroy() noexcept override {}
    render::CommandBuffer* GetCommandBuffer() const noexcept override { return nullptr; }
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    void SetStencilReference(uint8_t value) noexcept override {
        Order.emplace_back("stencil");
        Stencils.push_back(value);
    }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* value) noexcept override {
        Order.emplace_back("pipeline");
        Pipelines.push_back(value);
    }
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> values) noexcept override {
        Order.emplace_back("vertex");
        VertexCalls.emplace_back(values.begin(), values.end());
    }
    void BindIndexBuffer(render::IndexBufferView value) noexcept override {
        Order.emplace_back("index");
        IndexBuffers.push_back(value);
    }
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset>) noexcept override {
        Order.emplace_back("parameters");
        ParameterBinds.push_back({group, set});
    }
    bool SetPushConstants(uint32_t, uint32_t, std::span<const byte> bytes) noexcept override {
        Order.emplace_back("push");
        Pushes.emplace_back(bytes.begin(), bytes.end());
        return !FailPushConstants;
    }
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept override {
        Order.emplace_back("draw");
        Draws.push_back({indexCount, instanceCount, firstIndex, vertexOffset, firstInstance});
    }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override {}
    void DrawIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override {}
    void DrawIndexedIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override {}

    bool Valid{true};
    bool FailPushConstants{false};
    vector<std::string_view> Order;
    vector<render::GraphicsPipelineState*> Pipelines;
    vector<vector<render::VertexBufferBinding>> VertexCalls;
    vector<render::IndexBufferView> IndexBuffers;
    vector<uint8_t> Stencils;
    vector<RecordedParameterBind> ParameterBinds;
    vector<vector<byte>> Pushes;
    vector<render::DrawIndexedIndirectArguments> Draws;
};

class FakeCommandBuffer : public render::CommandBuffer {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void Begin() noexcept override {}
    void End() noexcept override {}
    void PushDebugGroup(std::string_view) noexcept override {}
    void PopDebugGroup() noexcept override {}
    void ResourceBarrier(std::span<const render::ResourceBarrierDescriptor>) noexcept override {}
    Nullable<unique_ptr<render::GraphicsCommandEncoder>> BeginRenderPass(const render::RenderPassBeginDescriptor&) noexcept override { return nullptr; }
    void EndRenderPass(unique_ptr<render::GraphicsCommandEncoder>) noexcept override {}
    Nullable<unique_ptr<render::ComputeCommandEncoder>> BeginComputePass() noexcept override { return nullptr; }
    void EndComputePass(unique_ptr<render::ComputeCommandEncoder>) noexcept override {}
    void CopyBufferToBuffer(render::Buffer*, uint64_t, render::Buffer*, uint64_t, uint64_t) noexcept override {}
    void CopyBufferToTexture(render::Texture*, render::SubresourceRange, render::Buffer*, uint64_t) noexcept override {}
    bool CopyBufferToTextureRegion(const render::BufferToTextureCopyDescriptor&) noexcept override { return false; }
    void CopyTextureToBuffer(render::Buffer*, uint64_t, render::Texture*, render::SubresourceRange) noexcept override {}
    void CopyTextureToTexture(const render::TextureCopyDescriptor&) noexcept override {}
    void ResolveTexture(const render::TextureResolveDescriptor&) noexcept override {}
    void ResetQueryPool(render::QueryPool*, uint32_t, uint32_t) noexcept override {}
    void WriteTimestamp(const render::QueryTimestampDescriptor&) noexcept override {}
    void ResolveQueryData(const render::QueryResolveDescriptor&) noexcept override {}
};

class FakeCommandAllocator final : public render::CommandAllocator {
public:
    explicit FakeCommandAllocator(std::function<void()>& onReset) noexcept : _onReset(onReset) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void Reset() noexcept override {
        if (_onReset) _onReset();
    }

private:
    std::function<void()>& _onReset;
};

class FakeQueue final : public render::CommandQueue {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void Submit(const render::CommandQueueSubmitDescriptor&) noexcept override {}
    void Wait() noexcept override {}
    render::QueueType GetQueueType() const noexcept override { return render::QueueType::Direct; }
    render::TimestampClockCalibrationResult GetTimestampClockCalibration() const noexcept override { return {}; }
};

// Creates command allocators and buffers; every other factory fails.
class FakeDevice : public render::Device {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::RenderBackend GetBackend() noexcept override { return render::RenderBackend::D3D12; }
    Nullable<render::ExternalResourceExtension*> GetExternalResourceExtension() noexcept override { return nullptr; }
    render::DeviceDetail GetDetail() const noexcept override { return Capabilities.Detail; }
    const render::RenderDeviceCapabilities& GetCapabilities() const noexcept override { return Capabilities; }
    render::TextureSupport QueryTextureSupport(const render::TextureSupportQuery&) const noexcept override { return {}; }
    render::AttachmentResolveSupport QueryAttachmentResolveSupport(render::TextureFormat, uint32_t) const noexcept override { return {}; }
    Nullable<render::CommandQueue*> GetCommandQueue(render::QueueType, uint32_t) noexcept override { return &Queue; }
    Nullable<unique_ptr<render::CommandAllocator>> CreateCommandAllocator(render::CommandQueue*) noexcept override {
        return make_unique<FakeCommandAllocator>(OnAllocatorReset);
    }
    Nullable<unique_ptr<render::CommandBuffer>> CreateCommandBuffer(render::CommandAllocator*) noexcept override {
        if (FailCommandBuffers) return nullptr;
        return make_unique<FakeCommandBuffer>();
    }
    Nullable<unique_ptr<render::Fence>> CreateFence() noexcept override { return nullptr; }
    Nullable<unique_ptr<render::QueryPool>> CreateQueryPool(const render::QueryPoolDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::SwapChain>> CreateSwapChain(const render::SwapChainDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::BindingGroupLayout>> CreateBindingGroupLayout(const render::BindingGroupLayoutDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::PipelineLayout>> CreatePipelineLayout(const render::PipelineLayoutDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::ShaderParameterSet>> CreateShaderParameterSet(const render::ShaderParameterSetDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(const render::GraphicsPipelineStateDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::ComputePipelineState>> CreateComputePipelineState(const render::ComputePipelineStateDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Sampler>> CreateSampler(const render::SamplerDescriptor&) noexcept override { return nullptr; }
    Nullable<render::Sampler*> GetOrCreateSampler(const render::SamplerDescriptor&) noexcept override { return nullptr; }

    FakeQueue Queue;
    render::RenderDeviceCapabilities Capabilities{};
    std::function<void()> OnAllocatorReset;
    bool FailCommandBuffers{false};
};

// Forwards each stage to an optional test callback.
class CallbackPipeline final : public FramePipeline {
public:
    void Extract(const Scene& source, FrameSlot& slot) noexcept override {
        if (OnExtract) OnExtract(source, slot);
    }
    void Record(FrameSlot& slot) noexcept override {
        if (OnRecord) OnRecord(slot);
    }
    void Wait(FrameSlot& slot) noexcept override {
        if (OnWait) OnWait(slot);
    }
    std::span<const MeshPassRegistration> GetMeshPassRegistrations() noexcept override { return Passes; }

    std::function<void(const Scene&, FrameSlot&)> OnExtract;
    std::function<void(FrameSlot&)> OnRecord;
    std::function<void(FrameSlot&)> OnWait;
    vector<MeshPassRegistration> Passes;
};

struct FakeGpu {
    shared_ptr<FakeDevice> Device{make_shared<FakeDevice>()};
    DeviceContext Context{Device, &Device->Queue};
};

// Returns null when creation fails; pipeline keeps borrowing the scheduler-owned pipeline.
inline unique_ptr<FrameScheduler> CreateScheduler(DeviceContext& device, CallbackPipeline*& pipeline, uint32_t slotCount = 2) {
    auto owner = make_unique<CallbackPipeline>();
    pipeline = owner.get();
    auto scheduler = FrameScheduler::Create(device, std::move(owner), slotCount);
    return scheduler ? scheduler.Release() : nullptr;
}

}  // namespace radray::test
