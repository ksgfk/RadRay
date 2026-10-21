#pragma once

#include <radray/json.h>
#include <radray/render/rhi.h>

namespace radray::example {
class DescriptorProbeSet;
struct DescriptorProbe {
    struct Counters {
        uint64_t Created{0}, Set{0}, Flush{0}, Failed{0};
        uint64_t LiveSets{0}, PeakSets{0}, Resource{0}, Sampler{0}, PeakResource{0}, PeakSampler{0};
        uint64_t AllocatedResource{0}, AllocatedSampler{0}, FreedResource{0}, FreedSampler{0};
        map<uint32_t, uint64_t> Types, PeakTypes, AllocatedTypes, FreedTypes;
    } Total;
    vector<DescriptorProbeSet*> Live;
    unordered_map<string, vector<render::ShaderBufferBinding>> SemanticSlices;
    uint64_t PeakSemanticExtraSlices{0};
    void ObserveData(std::string_view semantic, render::ShaderBufferBinding binding);
    void BeginFrame();
    void Snapshot(JsonRef output) const;
};

class DescriptorProbeSet final : public render::ShaderParameterSet {
public:
    DescriptorProbeSet(unique_ptr<render::ShaderParameterSet> native, render::RenderBackend backend,
                       const render::ShaderParameterSetDescriptor& desc, DescriptorProbe& probe);
    ~DescriptorProbeSet() noexcept override;
    bool IsValid() const noexcept override { return _native && _native->IsValid(); }
    void Destroy() noexcept override;
    bool Set(render::BindingHandle binding, uint32_t element, render::ShaderParameterValue value) noexcept override;
    bool FlushWrites() noexcept override;
    void ResetBindingsForReuse() noexcept override;
    render::ShaderParameterSet* Native() const noexcept { return _native.get(); }
    const string& Shape() const noexcept { return _shape; }
    string ValuesKey() const;
    uint64_t ResourceCount() const noexcept { return _resource; }
    uint64_t SamplerCount() const noexcept { return _sampler; }

private:
    struct Value {
        string Slot;
        render::ShaderParameterValue Data;
    };
    unique_ptr<render::ShaderParameterSet> _native;
    render::RenderBackend _backend;
    render::PipelineLayout* _layout;
    DescriptorProbe& _probe;
    string _shape;
    vector<Value> _values;
    map<uint32_t, uint64_t> _types;
    uint64_t _resource{0}, _sampler{0};
    bool _sealed{false};
};

// All storage and counters belong to the sample; the wrapped device owns native allocations.
class DescriptorProbeDevice final : public render::Device {
public:
    DescriptorProbeDevice(render::Device* native, DescriptorProbe& probe) : _native(native), _probe(probe) {}
    bool IsValid() const noexcept override { return _native->IsValid(); }
    void Destroy() noexcept override {}
    render::RenderBackend GetBackend() noexcept override { return _native->GetBackend(); }
    render::DeviceDetail GetDetail() const noexcept override { return _native->GetDetail(); }
    const render::RenderDeviceCapabilities& GetCapabilities() const noexcept override { return _native->GetCapabilities(); }
    render::TextureSupport QueryTextureSupport(const render::TextureSupportQuery& desc) const noexcept override { return _native->QueryTextureSupport(desc); }
    Nullable<render::CommandQueue*> GetCommandQueue(render::QueueType type, uint32_t slot = 0) noexcept override { return _native->GetCommandQueue(type, slot); }
    Nullable<unique_ptr<render::CommandAllocator>> CreateCommandAllocator(render::CommandQueue* queue) noexcept override { return _native->CreateCommandAllocator(queue); }
    Nullable<unique_ptr<render::CommandBuffer>> CreateCommandBuffer(render::CommandAllocator* allocator) noexcept override { return _native->CreateCommandBuffer(allocator); }
    Nullable<unique_ptr<render::Fence>> CreateFence() noexcept override { return _native->CreateFence(); }
    Nullable<unique_ptr<render::QueryPool>> CreateQueryPool(const render::QueryPoolDescriptor& desc) noexcept override { return _native->CreateQueryPool(desc); }
    Nullable<unique_ptr<render::SwapChain>> CreateSwapChain(const render::SwapChainDescriptor& desc) noexcept override { return _native->CreateSwapChain(desc); }
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& desc) noexcept override { return _native->CreateBuffer(desc); }
    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor& desc) noexcept override { return _native->CreateTexture(desc); }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor& desc) noexcept override { return _native->CreateTextureView(desc); }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor& desc) noexcept override { return _native->CreateShader(desc); }
    Nullable<unique_ptr<render::ShaderParameterSet>> CreateShaderParameterSet(const render::ShaderParameterSetDescriptor& desc) noexcept override;
    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(const render::GraphicsPipelineStateDescriptor& desc) noexcept override { return _native->CreateGraphicsPipelineState(desc); }
    Nullable<unique_ptr<render::ComputePipelineState>> CreateComputePipelineState(const render::ComputePipelineStateDescriptor& desc) noexcept override { return _native->CreateComputePipelineState(desc); }
    Nullable<unique_ptr<render::Sampler>> CreateSampler(const render::SamplerDescriptor& desc) noexcept override { return _native->CreateSampler(desc); }
    Nullable<render::Sampler*> GetOrCreateSampler(const render::SamplerDescriptor& desc) noexcept override { return _native->GetOrCreateSampler(desc); }

private:
    render::Device* _native;
    DescriptorProbe& _probe;
};

class DescriptorProbeEncoder final : public render::GraphicsCommandEncoder {
public:
    explicit DescriptorProbeEncoder(render::GraphicsCommandEncoder* native) : _native(native) {}
    bool IsValid() const noexcept override { return _native->IsValid(); }
    void Destroy() noexcept override {}
    render::CommandBuffer* GetCommandBuffer() const noexcept override { return _native->GetCommandBuffer(); }
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets) noexcept override;
    bool SetPushConstants(render::BindingHandle binding, std::span<const byte> data) noexcept override { return _native->SetPushConstants(binding, data); }
    void SetViewport(Viewport value) noexcept override { _native->SetViewport(value); }
    void SetScissor(Rect value) noexcept override { _native->SetScissor(value); }
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> bindings) noexcept override { _native->BindVertexBuffers(bindings); }
    void BindIndexBuffer(render::IndexBufferView view) noexcept override { _native->BindIndexBuffer(view); }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* state) noexcept override { _native->BindGraphicsPipelineState(state); }
    void Draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t firstInstance) noexcept override { _native->Draw(count, instances, first, firstInstance); }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance) noexcept override { _native->DrawIndexed(count, instances, first, offset, firstInstance); }
    void DrawIndirect(render::Buffer* buffer, uint64_t offset, uint32_t count) noexcept override { _native->DrawIndirect(buffer, offset, count); }
    void DrawIndexedIndirect(render::Buffer* buffer, uint64_t offset, uint32_t count) noexcept override { _native->DrawIndexedIndirect(buffer, offset, count); }

private:
    render::GraphicsCommandEncoder* _native;
};
}  // namespace radray::example
