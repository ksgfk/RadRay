#pragma once

#include <radray/render/rhi.h>

namespace radray::test {

class GraphCompileDevice : public render::Device {
public:
    GraphCompileDevice() {
        Capabilities.Limits = {8, 16384, 16384, 2048, 2048, UINT64_MAX, 65536, 128, 256};
        Capabilities.Features = {true, true, true, true, true};
        Capabilities.Queues[static_cast<size_t>(render::QueueType::Direct)].CreatedCount = 1;
    }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::RenderBackend GetBackend() noexcept override { return render::RenderBackend::D3D12; }
    render::DeviceDetail GetDetail() const noexcept override { return Capabilities.Detail; }
    const render::RenderDeviceCapabilities& GetCapabilities() const noexcept override { return Capabilities; }
    render::TextureSupport QueryTextureSupport(const render::TextureSupportQuery& query) const noexcept override {
        return {render::IsValidTextureSupportQuery(query) && query.Format != RejectedFormat,
                render::SampleCount::X1 | render::SampleCount::X4, true, true, 16384, 16384, 2048, 2048, 15, UINT64_MAX};
    }
    Nullable<render::CommandQueue*> GetCommandQueue(render::QueueType, uint32_t = 0) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::CommandBuffer>> CreateCommandBuffer(render::CommandQueue*) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::Fence>> CreateFence() noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::QueryPool>> CreateQueryPool(const render::QueryPoolDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::SwapChain>> CreateSwapChain(const render::SwapChainDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    void FlushMappedRanges(std::span<const render::MappedBufferRange>) noexcept override {}
    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::RenderPass>> CreateRenderPass(const render::RenderPassDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::Framebuffer>> CreateFramebuffer(const render::FramebufferDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::ShaderParameterSet>> CreateShaderParameterSet(const render::ShaderParameterSetDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(const render::GraphicsPipelineStateDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::ComputePipelineState>> CreateComputePipelineState(const render::ComputePipelineStateDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<unique_ptr<render::Sampler>> CreateSampler(const render::SamplerDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }
    Nullable<render::Sampler*> GetOrCreateSampler(const render::SamplerDescriptor&) noexcept override {
        ++NativeCreates;
        return nullptr;
    }

    render::RenderDeviceCapabilities Capabilities;
    render::TextureFormat RejectedFormat{render::TextureFormat::UNKNOWN};
    uint32_t NativeCreates{0};
};

}  // namespace radray::test
