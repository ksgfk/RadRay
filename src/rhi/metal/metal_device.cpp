#include "metal_device.h"

namespace radray::rhi::metal {

Device::Device(const RadrayDeviceDescriptorMetal& desc) {
    RADRAY_MTL_THROW("no impl");
}
Device::~Device() noexcept = default;

RadrayCommandQueue Device::CreateCommandQueue(RadrayQueueType type) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyCommandQueue(RadrayCommandQueue queue) {
    RADRAY_MTL_THROW("no impl");
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
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroySwapChian(RadraySwapChain swapchain) {
    RADRAY_MTL_THROW("no impl");
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
