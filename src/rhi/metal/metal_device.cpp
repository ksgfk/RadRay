#include "metal_device.h"

#include "metal_command_queue.h"
#include "metal_swapchain.h"

namespace radray::rhi::metal {

Device::Device(const RadrayDeviceDescriptorMetal& desc) {
    AutoRelease([this, &desc]() {
        auto allDevices = MTL::CopyAllDevices();
        auto deviceCount = allDevices->count();
        uint32_t deviceIndex = desc.DeviceIndex == RADRAY_RHI_AUTO_SELECT_DEVICE ? 0 : desc.DeviceIndex;
        if (deviceIndex >= deviceCount) {
            RADRAY_MTL_THROW("Metal device index out of range (need={}, count={})", desc.DeviceIndex, deviceCount);
        }
        device = allDevices->object<MTL::Device>(deviceIndex)->retain();
        RADRAY_INFO_LOG("Metal device: {}", device->name()->utf8String());
    });
}

Device::~Device() noexcept {
    AutoRelease([this]() {
        device->release();
    });
}

RadrayCommandQueue Device::CreateCommandQueue(RadrayQueueType type) {
    return AutoRelease([this]() {
        auto q = RhiNew<CommandQueue>(device);
        return RadrayCommandQueue{q, q->queue};
    });
}

void Device::DestroyCommandQueue(RadrayCommandQueue queue) {
    AutoRelease([=]() {
        auto q = reinterpret_cast<CommandQueue*>(queue.Ptr);
        RhiDelete(q);
    });
}

RadrayFence Device::CreateFence() {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyFence(RadrayFence fence) {
    RADRAY_MTL_THROW("no impl");
}

RadrayCommandAllocator Device::CreateCommandAllocator(RadrayQueueType type) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyCommandAllocator(RadrayCommandAllocator alloc) {
    RADRAY_MTL_THROW("no impl");
}
RadrayCommandList Device::CreateCommandList(RadrayCommandAllocator alloc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyCommandList(RadrayCommandList list) {
    RADRAY_MTL_THROW("no impl");
}
void Device::ResetCommandAllocator(RadrayCommandAllocator alloc) {
    RADRAY_MTL_THROW("no impl");
}

RadraySwapChain Device::CreateSwapChain(const RadraySwapChainDescriptor& desc) {
    return AutoRelease([&desc, this]() {
        auto sc = RhiNew<SwapChain>(
            device,
            desc.NativeWindow,
            desc.Width,
            desc.Height,
            desc.BackBufferCount,
            EnumConvert(desc.Format),
            desc.EnableSync);
        return RadraySwapChain{sc, sc->layer};
    });
}

void Device::DestroySwapChian(RadraySwapChain swapchain) {
    AutoRelease([=]() {
        auto sc = reinterpret_cast<SwapChain*>(swapchain.Ptr);
        RhiDelete(sc);
    });
}

RadrayBuffer Device::CreateBuffer(const RadrayBufferDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyBuffer(RadrayBuffer buffer) {
    RADRAY_MTL_THROW("no impl");
}
RadrayBufferView Device::CreateBufferView(const RadrayBufferViewDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyBufferView(RadrayBuffer buffer, RadrayBufferView view) {
    RADRAY_MTL_THROW("no impl");
}

RadrayTexture Device::CreateTexture(const RadrayTextureDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyTexture(RadrayTexture texture) {
    RADRAY_MTL_THROW("no impl");
}
RadrayTextureView Device::CreateTextureView(const RadrayTextureViewDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyTextureView(RadrayTextureView view) {
    RADRAY_MTL_THROW("no impl");
}

RadrayShader Device::CompileShader(const RadrayCompileRasterizationShaderDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyShader(RadrayShader shader) {
    RADRAY_MTL_THROW("no impl");
}

RadrayRootSignature Device::CreateRootSignature(const RadrayRootSignatureDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyRootSignature(RadrayRootSignature rootSig) {
    RADRAY_MTL_THROW("no impl");
}

RadrayGraphicsPipeline Device::CreateGraphicsPipeline(const RadrayGraphicsPipelineDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyGraphicsPipeline(RadrayGraphicsPipeline pipe) {
    RADRAY_MTL_THROW("no impl");
}

}  // namespace radray::rhi::metal
