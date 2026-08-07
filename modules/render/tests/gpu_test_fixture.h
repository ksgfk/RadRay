#pragma once

#include <cstring>
#include <optional>

#include <radray/render/rhi.h>

#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render::test {

struct DeviceContext {
    bool VulkanEnvInitialized{false};
    unique_ptr<DXGIFactory> Factory;
    shared_ptr<Device> Device;
    CommandQueue* Queue{nullptr};

    ~DeviceContext() {
        Device.reset();
        Factory.reset();
#if defined(RADRAY_ENABLE_VULKAN)
        if (VulkanEnvInitialized) {
            InstanceVulkan::ShutdownEnv();
        }
#endif
    }
};

inline bool TryCreateDevice(
    RenderBackend backend,
    DeviceContext& context,
    bool enableValidation = false) {
    if (backend == RenderBackend::D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
        DXGIFactoryDescriptor factoryDesc{};
        factoryDesc.IsEnableDebugLayer = enableValidation;
        auto factory = DXGIFactory::Create(factoryDesc);
        if (!factory.HasValue()) {
            return false;
        }
        context.Factory = factory.Release();

        D3D12DeviceDescriptor deviceDesc{};
        deviceDesc.Factory = context.Factory.get();
        auto device = Device::Create(DeviceDescriptor{deviceDesc});
        if (!device.HasValue()) {
            context.Factory.reset();
            return false;
        }
        context.Device = device.Release();
#else
        return false;
#endif
    } else if (backend == RenderBackend::Vulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
        VulkanInstanceDescriptor instanceDesc{};
        instanceDesc.AppName = "radray_render_test";
        instanceDesc.EngineName = "radray";
        instanceDesc.IsEnableDebugLayer = enableValidation;
        if (!InstanceVulkan::InitEnv(instanceDesc)) {
            return false;
        }
        context.VulkanEnvInitialized = true;

        const VulkanCommandQueueDescriptor queues[]{
            VulkanCommandQueueDescriptor{QueueType::Direct, 1}};
        VulkanDeviceDescriptor deviceDesc{};
        deviceDesc.Queues = queues;
        auto device = Device::Create(DeviceDescriptor{deviceDesc});
        if (!device.HasValue()) {
            return false;
        }
        context.Device = device.Release();
#else
        return false;
#endif
    } else {
        return false;
    }

    auto queue = context.Device->GetCommandQueue(QueueType::Direct, 0);
    if (!queue.HasValue()) {
        context.Device.reset();
        context.Factory.reset();
        return false;
    }
    context.Queue = queue.Unwrap();
    return true;
}

inline bool TryCreateAnyDevice(DeviceContext& context) {
    if (TryCreateDevice(RenderBackend::D3D12, context)) {
        return true;
    }
    return TryCreateDevice(RenderBackend::Vulkan, context);
}

inline Nullable<unique_ptr<Buffer>> MakeUploadBuffer(
    Device& device,
    std::span<const byte> data,
    BufferUses usage) {
    BufferDescriptor desc{
        .Size = data.size(),
        .Memory = MemoryType::Upload,
        .Usage = usage | BufferUse::MapWrite,
        .Hints = ResourceHint::None};
    auto buffer = device.CreateBuffer(desc);
    if (!buffer.HasValue()) {
        return nullptr;
    }
    unique_ptr<Buffer> result = buffer.Release();
    void* mapped = result->Map(0, data.size());
    if (mapped == nullptr) {
        return nullptr;
    }
    std::memcpy(mapped, data.data(), data.size());
    result->FlushMappedRange(BufferRange{0, data.size()});
    result->Unmap();
    return result;
}

struct RenderTarget {
    unique_ptr<Texture> Tex;
    unique_ptr<TextureView> View;
};

inline std::optional<RenderTarget> MakeRenderTarget(
    Device* device,
    TextureFormat format,
    uint32_t width,
    uint32_t height,
    TextureUses usage = TextureUse::RenderTarget) {
    TextureDescriptor textureDesc{
        .Dim = TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = format,
        .Memory = MemoryType::Device,
        .Usage = usage,
        .Hints = ResourceHint::None};
    auto texture = device->CreateTexture(textureDesc);
    if (!texture.HasValue()) {
        return std::nullopt;
    }

    RenderTarget target{};
    target.Tex = texture.Release();
    TextureViewDescriptor viewDesc{
        .Target = target.Tex.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = format,
        .Range = SubresourceRange{0, 1, 0, 1},
        .Usage = TextureViewUsage::RenderTarget};
    auto view = device->CreateTextureView(viewDesc);
    if (!view.HasValue()) {
        return std::nullopt;
    }
    target.View = view.Release();
    return target;
}

}  // namespace radray::render::test
