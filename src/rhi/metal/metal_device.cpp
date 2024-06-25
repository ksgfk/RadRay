#include "metal_device.h"

#include <radray/logger.h>

#include "metal_command_queue.h"
#include "metal_swap_chain.h"

namespace radray::rhi::metal {

MetalDevice::MetalDevice() = default;

MetalDevice::~MetalDevice() noexcept = default;

std::unique_ptr<MetalDevice> CreateImpl(const DeviceCreateInfoMetal& info) {
    auto all = NS::TransferPtr(MTL::CopyAllDevices());
    auto deviceCount = all->count();
    if (info.DeviceIndex >= deviceCount) {
        RADRAY_ERR_LOG("device index out of range. count = {}", deviceCount);
        return std::unique_ptr<MetalDevice>{};
    }
    MTL::Device* device = all->object<MTL::Device>(info.DeviceIndex);
    RADRAY_INFO_LOG("select metal device: {}", device->name()->utf8String());
    auto result = std::make_unique<MetalDevice>();
    result->device = NS::TransferPtr(device);
    return result;
}

CommandQueueHandle MetalDevice::CreateCommandQueue(CommandListType type) {
    MetalCommandQueue* mcq = new MetalCommandQueue{this, 0};
    return CommandQueueHandle{
        reinterpret_cast<uint64_t>(mcq),
        mcq->queue.get()};
}

void MetalDevice::DestroyCommandQueue(const CommandQueueHandle& handle) {
    auto mcq = reinterpret_cast<MetalCommandQueue*>(handle.Handle);
    delete mcq;
}

SwapChainHandle MetalDevice::CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) {
    MetalSwapChain* msc = new MetalSwapChain{
        this,
        info.WindowHandle,
        info.Width, info.Height,
        info.Vsync,
        info.BackBufferCount};
    SwapChainHandle handle{};
    handle.Handle = reinterpret_cast<uint64_t>(msc);
    handle.Ptr = msc->layer.get();
    return handle;
}

void MetalDevice::DestroySwapChain(const SwapChainHandle& handle) {
    auto msc = reinterpret_cast<MetalSwapChain*>(handle.Handle);
    delete msc;
}

ResourceHandle MetalDevice::GetCurrentSwapChainBackBuffer(const SwapChainHandle& handle) {
    auto msc = reinterpret_cast<MetalSwapChain*>(handle.Handle);
    return {
        reinterpret_cast<uint64_t>(msc->nowRt.get()),
        msc->nowRt->texture.get()};
}

}  // namespace radray::rhi::metal
