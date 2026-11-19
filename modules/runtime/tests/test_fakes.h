#pragma once

#include <cstddef>
#include <span>
#include <utility>
#include <functional>
#include <limits>
#include <string_view>

#include <radray/nullable.h>
#include <radray/runtime/frame_pipeline.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/mesh_pass_processor.h>
#include <radray/viewport.h>
#include <radray/types.h>
#include <radray/gpu/types.h>
#include <radray/gpu/external_resource.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/gpu_device.h>

// runtime 所用 RHI 对象的 CPU 替身；各替身仅记录测试观察的数据。
namespace radray::test {

class FakeBuffer : public gpu::Buffer {
public:
    FakeBuffer(uint64_t size, gpu::BufferUses usage) noexcept : Desc{.Size = size, .Usage = usage} {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void* Map(uint64_t, uint64_t) noexcept override { return nullptr; }
    void Unmap() noexcept override {}
    void FlushMappedRange(gpu::BufferRange) noexcept override {}
    void InvalidateMappedRange(gpu::BufferRange) noexcept override {}
    gpu::BufferDescriptor GetDesc() const noexcept override { return Desc; }
    gpu::Device* GetDevice() const noexcept override { return nullptr; }

    gpu::BufferDescriptor Desc;
};

class FakePipelineLayout final : public gpu::PipelineLayout {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::PipelineLayoutDescriptor GetDesc() const noexcept override { return {}; }
};

class FakePipeline final : public gpu::GraphicsPipelineState {
public:
    explicit FakePipeline(Nullable<gpu::PipelineLayout*> layout = nullptr) noexcept : Layout(layout) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    Nullable<gpu::PipelineLayout*> GetLayout() const noexcept override { return Layout; }

    Nullable<gpu::PipelineLayout*> Layout{nullptr};
};

class FakeMappedBuffer final : public FakeBuffer {
public:
    FakeMappedBuffer(const gpu::BufferDescriptor& desc, bool failMap, std::function<void()>& onMap, std::function<void(gpu::BufferRange)>& onFlush)
        : FakeBuffer(desc.Size, desc.Usage), Bytes(static_cast<size_t>(desc.Size)), _failMap(failMap), _onMap(onMap), _onFlush(onFlush) { Desc = desc; }
    void* Map(uint64_t offset, uint64_t size) noexcept override {
        if (_onMap) _onMap();
        return !_failMap && offset <= Bytes.size() && size <= Bytes.size() - offset ? Bytes.data() + offset : nullptr;
    }
    void FlushMappedRange(gpu::BufferRange range) noexcept override {
        if (_onFlush) _onFlush(range);
    }
    vector<byte> Bytes;

private:
    bool _failMap;
    std::function<void()>& _onMap;
    std::function<void(gpu::BufferRange)>& _onFlush;
};

class FakeTexture final : public gpu::Texture {
public:
    FakeTexture(gpu::Device& device, gpu::TextureDescriptor desc, std::function<void()>& onDestroy) noexcept
        : Desc(desc), _device(device), _onDestroy(onDestroy) {}
    ~FakeTexture() noexcept override {
        if (_onDestroy) _onDestroy();
    }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::TextureDescriptor GetDesc() const noexcept override { return Desc; }
    gpu::Device* GetDevice() const noexcept override { return &_device; }
    gpu::TextureDescriptor Desc;

private:
    gpu::Device& _device;
    std::function<void()>& _onDestroy;
};

class FakeTextureView final : public gpu::TextureView {
public:
    FakeTextureView(gpu::TextureViewDescriptor desc, std::function<void()>& onDestroy) noexcept : Desc(desc), _onDestroy(onDestroy) {}
    ~FakeTextureView() noexcept override {
        if (_onDestroy) _onDestroy();
    }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::TextureViewDescriptor GetDesc() const noexcept override { return Desc; }
    gpu::TextureViewDescriptor Desc;

private:
    std::function<void()>& _onDestroy;
};

class FakeParameterSet final : public gpu::ShaderParameterSet {
public:
    bool IsValid() const noexcept override { return Valid; }
    void Destroy() noexcept override {}
    bool Set(uint32_t, uint32_t, gpu::ShaderParameterValue) noexcept override { return true; }
    bool FlushWrites() noexcept override { return true; }
    void ResetBindingsForReuse() noexcept override {}

    bool Valid{true};
};

struct RecordedParameterBind {
    uint32_t Group;
    gpu::ShaderParameterSet* Set;
    vector<gpu::ShaderParameterDynamicOffset> DynamicOffsets;
};

struct RecordedIndirectDraw {
    gpu::Buffer* Buffer{nullptr};
    uint64_t Offset{0};
    uint32_t Count{0};
};

class RecordingEncoder final : public gpu::GraphicsCommandEncoder {
public:
    bool IsValid() const noexcept override { return Valid; }
    void Destroy() noexcept override {}
    gpu::CommandBuffer* GetCommandBuffer() const noexcept override { return nullptr; }
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    void SetStencilReference(uint8_t value) noexcept override {
        Order.emplace_back("stencil");
        Stencils.push_back(value);
    }
    void BindGraphicsPipelineState(gpu::GraphicsPipelineState* value) noexcept override {
        Order.emplace_back("pipeline");
        Pipelines.push_back(value);
    }
    void BindVertexBuffers(std::span<const gpu::VertexBufferBinding> values) noexcept override {
        Order.emplace_back("vertex");
        VertexCalls.emplace_back(values.begin(), values.end());
    }
    void BindIndexBuffer(gpu::IndexBufferView value) noexcept override {
        Order.emplace_back("index");
        IndexBuffers.push_back(value);
    }
    void BindShaderParameterSet(uint32_t group, gpu::ShaderParameterSet* set, std::span<const gpu::ShaderParameterDynamicOffset> offsets) noexcept override {
        Order.emplace_back("parameters");
        ParameterBinds.push_back({group, set, {offsets.begin(), offsets.end()}});
    }
    bool SetPushConstants(uint32_t, uint32_t, std::span<const byte> bytes) noexcept override {
        Order.emplace_back("push");
        Pushes.emplace_back(bytes.begin(), bytes.end());
        if (FailPushConstantsOnCall != 0 && Pushes.size() == FailPushConstantsOnCall) return false;
        return !FailPushConstants;
    }
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept override {
        Order.emplace_back("draw");
        Draws.push_back({indexCount, instanceCount, firstIndex, vertexOffset, firstInstance});
    }
    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept override {
        Order.emplace_back("draw_non_indexed");
        NonIndexedDraws.push_back({vertexCount, instanceCount, firstVertex, firstInstance});
    }
    void DrawIndirect(gpu::Buffer* buffer, uint64_t offset, uint32_t count) noexcept override {
        Order.emplace_back("draw_indirect");
        IndirectDraws.push_back({buffer, offset, count});
    }
    void DrawIndexedIndirect(gpu::Buffer* buffer, uint64_t offset, uint32_t count) noexcept override {
        Order.emplace_back("draw_indexed_indirect");
        IndexedIndirectDraws.push_back({buffer, offset, count});
    }

    bool Valid{true};
    bool FailPushConstants{false};
    uint32_t FailPushConstantsOnCall{0};
    vector<std::string_view> Order;
    vector<gpu::GraphicsPipelineState*> Pipelines;
    vector<vector<gpu::VertexBufferBinding>> VertexCalls;
    vector<gpu::IndexBufferView> IndexBuffers;
    vector<uint8_t> Stencils;
    vector<RecordedParameterBind> ParameterBinds;
    vector<vector<byte>> Pushes;
    vector<gpu::DrawIndexedIndirectArguments> Draws;
    vector<gpu::DrawIndirectArguments> NonIndexedDraws;
    vector<RecordedIndirectDraw> IndirectDraws;
    vector<RecordedIndirectDraw> IndexedIndirectDraws;
};

class FakeCommandBuffer : public gpu::CommandBuffer {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void Begin() noexcept override {}
    void End() noexcept override {}
    void PushDebugGroup(std::string_view) noexcept override {}
    void PopDebugGroup() noexcept override {}
    void ResourceBarrier(std::span<const gpu::ResourceBarrierDescriptor>) noexcept override {}
    Nullable<unique_ptr<gpu::GraphicsCommandEncoder>> BeginRenderPass(const gpu::RenderPassBeginDescriptor&) noexcept override { return nullptr; }
    void EndRenderPass(unique_ptr<gpu::GraphicsCommandEncoder>) noexcept override {}
    Nullable<unique_ptr<gpu::ComputeCommandEncoder>> BeginComputePass() noexcept override { return nullptr; }
    void EndComputePass(unique_ptr<gpu::ComputeCommandEncoder>) noexcept override {}
    void CopyBufferToBuffer(gpu::Buffer*, uint64_t, gpu::Buffer*, uint64_t, uint64_t) noexcept override {}
    void CopyBufferToTexture(gpu::Texture*, gpu::SubresourceRange, gpu::Buffer*, uint64_t) noexcept override {}
    bool CopyTextureToBufferRegion(const gpu::TextureToBufferCopyDescriptor&) noexcept override { return false; }
    bool CopyBufferToTextureRegion(const gpu::BufferToTextureCopyDescriptor&) noexcept override { return false; }
    void CopyTextureToBuffer(gpu::Buffer*, uint64_t, gpu::Texture*, gpu::SubresourceRange) noexcept override {}
    void CopyTextureToTexture(const gpu::TextureCopyDescriptor&) noexcept override {}
    void ResolveTexture(const gpu::TextureResolveDescriptor&) noexcept override {}
    void ResetQueryPool(gpu::QueryPool*, uint32_t, uint32_t) noexcept override {}
    void WriteTimestamp(const gpu::QueryTimestampDescriptor&) noexcept override {}
    void ResolveQueryData(const gpu::QueryResolveDescriptor&) noexcept override {}
};

class FakeCommandAllocator final : public gpu::CommandAllocator {
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

class FakeQueue final : public gpu::CommandQueue {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Submit(const gpu::CommandQueueSubmitDescriptor&) noexcept override { return OnSubmit ? OnSubmit() : true; }
    uint32_t GetTimestampValidBits() const noexcept override { return TimestampBits; }
    uint32_t TimestampBits{};
    std::function<bool()> OnSubmit;
    void Wait() noexcept override {}
    gpu::QueueType GetQueueType() const noexcept override { return gpu::QueueType::Direct; }
    gpu::TimestampClockCalibrationResult GetTimestampClockCalibration() const noexcept override { return {}; }
};

// 默认仅创建命令对象；上传测试可显式开启 host buffer 和纹理支持。
class FakeDevice : public gpu::Device {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool IsDeviceLost() const noexcept override { return Lost; }
    bool Lost{false};
    gpu::GpuBackend GetBackend() noexcept override { return gpu::GpuBackend::D3D12; }
    Nullable<unique_ptr<gpu::ExternalResourceExtension>> CreateExternalResourceExtension() noexcept override { return nullptr; }
    gpu::DeviceDetail GetDetail() const noexcept override { return Capabilities.Detail; }
    const gpu::GpuDeviceCapabilities& GetCapabilities() const noexcept override { return Capabilities; }
    gpu::TextureSupport QueryTextureSupport(const gpu::TextureSupportQuery&) const noexcept override {
        if (!EnableTextures) return {};
        return {.MaxResourceSize = std::numeric_limits<uint64_t>::max(), .MaxWidth = 16384, .MaxHeight = 16384, .MaxDepth = 1, .MaxArrayLayers = 2048, .MaxMipLevels = 15, .SampleCounts = gpu::SampleCount::X1, .Supported = true};
    }
    gpu::AttachmentResolveSupport QueryAttachmentResolveSupport(gpu::TextureFormat, uint32_t) const noexcept override { return {}; }
    Nullable<gpu::CommandQueue*> GetCommandQueue(gpu::QueueType, uint32_t) noexcept override { return &Queue; }
    Nullable<unique_ptr<gpu::CommandAllocator>> CreateCommandAllocator(gpu::CommandQueue*) noexcept override {
        return make_unique<FakeCommandAllocator>(OnAllocatorReset);
    }
    Nullable<unique_ptr<gpu::CommandBuffer>> CreateCommandBuffer(gpu::CommandAllocator*) noexcept override {
        if (FailCommandBuffers) return nullptr;
        return make_unique<FakeCommandBuffer>();
    }
    Nullable<unique_ptr<gpu::Fence>> CreateFence() noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::QueryPool>> CreateQueryPool(const gpu::QueryPoolDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::SwapChain>> CreateSwapChain(const gpu::SwapChainDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::Buffer>> CreateBuffer(const gpu::BufferDescriptor& desc) noexcept override {
        if (!EnableHostBuffers || BuffersUntilFailure == 0) return nullptr;
        if (BuffersUntilFailure > 0) --BuffersUntilFailure;
        if (OnCreateBuffer) OnCreateBuffer(desc);
        return make_unique<FakeMappedBuffer>(desc, FailMaps, OnMap, OnFlush);
    }
    Nullable<unique_ptr<gpu::Texture>> CreateTexture(const gpu::TextureDescriptor& desc) noexcept override {
        if (!EnableTextures || FailTextures) return nullptr;
        return make_unique<FakeTexture>(*this, desc, OnTextureDestroyed);
    }
    Nullable<unique_ptr<gpu::TextureView>> CreateTextureView(const gpu::TextureViewDescriptor& desc) noexcept override {
        if (!EnableTextures || FailViews) return nullptr;
        return make_unique<FakeTextureView>(desc, OnViewDestroyed);
    }
    Nullable<unique_ptr<gpu::Shader>> CreateShader(const gpu::ShaderDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::BindingGroupLayout>> CreateBindingGroupLayout(const gpu::BindingGroupLayoutDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::PipelineLayout>> CreatePipelineLayout(const gpu::PipelineLayoutDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::ShaderParameterSet>> CreateShaderParameterSet(const gpu::ShaderParameterSetDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::GraphicsPipelineState>> CreateGraphicsPipelineState(const gpu::GraphicsPipelineStateDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::ComputePipelineState>> CreateComputePipelineState(const gpu::ComputePipelineStateDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::Sampler>> CreateSampler(const gpu::SamplerDescriptor&) noexcept override { return nullptr; }
    Nullable<gpu::Sampler*> GetOrCreateSampler(const gpu::SamplerDescriptor&) noexcept override { return nullptr; }

    FakeQueue Queue;
    gpu::GpuDeviceCapabilities Capabilities{};
    std::function<void()> OnAllocatorReset;
    bool FailCommandBuffers{false};
    int32_t BuffersUntilFailure{-1};
    bool EnableHostBuffers{false}, EnableTextures{false};
    bool FailMaps{false}, FailTextures{false}, FailViews{false};
    std::function<void()> OnMap, OnTextureDestroyed, OnViewDestroyed;
    std::function<void(gpu::BufferRange)> OnFlush;
    std::function<void(const gpu::BufferDescriptor&)> OnCreateBuffer;
};

// 将各阶段转发到可选的测试回调。
class CallbackPipeline final : public FramePipeline {
public:
    void Extract(FrameSlot& slot) noexcept override {
        if (OnExtract) OnExtract(slot);
    }
    void Prepare(FrameSlot& slot) noexcept override {
        if (OnPrepare) OnPrepare(slot);
    }
    bool Submit(FrameSlot& slot) noexcept override {
        if (OnSubmit) OnSubmit(slot);
        return SubmitResult;
    }
    void Record(FrameSlot& slot) noexcept override {
        if (OnRecord) OnRecord(slot);
    }
    void Wait(FrameSlot& slot) noexcept override {
        if (OnWait) OnWait(slot);
    }
    void OnSlotCompleted(FrameSlot& slot) noexcept override {
        if (OnCompleted) OnCompleted(slot);
    }
    std::function<void(FrameSlot&)> OnCompleted;
    std::span<const MeshPassRegistration> GetMeshPassRegistrations() noexcept override { return Passes; }

    bool SubmitResult{true};
    std::function<void(FrameSlot&)> OnExtract;
    std::function<void(FrameSlot&)> OnPrepare, OnSubmit;
    std::function<void(FrameSlot&)> OnRecord;
    std::function<void(FrameSlot&)> OnWait;
    vector<MeshPassRegistration> Passes;
};

struct FakeGpu {
    shared_ptr<FakeDevice> Device{make_shared<FakeDevice>()};
    DeviceContext Context{Device, &Device->Queue};
};

// 创建失败时返回空值；pipeline 继续借用调度器拥有的流水线。
inline unique_ptr<FrameScheduler> CreateScheduler(DeviceContext& device, CallbackPipeline*& pipeline, uint32_t slotCount = 2) {
    auto owner = make_unique<CallbackPipeline>();
    pipeline = owner.get();
    auto scheduler = FrameScheduler::Create(device, std::move(owner), slotCount);
    return scheduler ? scheduler.Release() : nullptr;
}

}  // namespace radray::test
