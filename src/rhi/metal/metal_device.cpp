#include "metal_device.h"

#include <cstring>

#include <radray/logger.h>

#include "metal_command_queue.h"
#include "metal_command_encoder.h"
#include "metal_swap_chain.h"
#include "metal_buffer.h"
#include "metal_texture.h"
#include "metal_event.h"

namespace radray::rhi::metal {

MetalDevice::MetalDevice() = default;

MetalDevice::~MetalDevice() noexcept {
    if (device != nullptr) {
        device->release();
        device = nullptr;
    }
}

std::shared_ptr<MetalDevice> CreateImpl(const DeviceCreateInfoMetal& info) {
    ScopedAutoreleasePool arp_{};
    NS::Array* allDevice = MTL::CopyAllDevices()->autorelease();
    if (!allDevice->count()) {
        RADRAY_WARN_LOG("metal no device");
        return nullptr;
    }
    auto deviceCount = allDevice->count();
    for (size_t i = 0; i < deviceCount; i++) {
        MTL::Device* d = allDevice->object<MTL::Device>(i);
        RADRAY_INFO_LOG("metal find device: {}", d->name()->utf8String());
    }
    if (info.DeviceIndex >= deviceCount) {
        RADRAY_ERR_LOG("device index out of range. count = {}", deviceCount);
        return nullptr;
    }
    MTL::Device* device = allDevice->object<MTL::Device>(info.DeviceIndex.value_or(0));
    RADRAY_INFO_LOG("select metal device: {}", device->name()->utf8String());

    auto result = std::make_shared<MetalDevice>();
    result->device = device;
    return result;
}

CommandQueueHandle MetalDevice::CreateCommandQueue(CommandListType type) {
    ScopedAutoreleasePool arp_{};
    MetalCommandQueue* mcq = new MetalCommandQueue{this->device, 0};
    return CommandQueueHandle{
        reinterpret_cast<uint64_t>(mcq),
        mcq->queue};
}

void MetalDevice::DestroyCommandQueue(CommandQueueHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto mcq = reinterpret_cast<MetalCommandQueue*>(handle.Handle);
    delete mcq;
}

FenceHandle MetalDevice::CreateFence() {
    ScopedAutoreleasePool arp_{};
    auto e = new MetalEvent{this->device};
    return FenceHandle{
        reinterpret_cast<uint64_t>(e),
        e->event};
}

void MetalDevice::DestroyFence(FenceHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto e = reinterpret_cast<MetalEvent*>(handle.Handle);
    delete e;
}

SwapChainHandle MetalDevice::CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) {
    ScopedAutoreleasePool arp_{};
    MetalSwapChain* msc = new MetalSwapChain{
        this->device,
        info.WindowHandle,
        info.Width, info.Height,
        info.Vsync,
        info.BackBufferCount};
    return SwapChainHandle{
        reinterpret_cast<uint64_t>(msc),
        msc->layer};
}

void MetalDevice::DestroySwapChain(SwapChainHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto msc = reinterpret_cast<MetalSwapChain*>(handle.Handle);
    delete msc;
}

ResourceHandle MetalDevice::CreateBuffer(BufferType type, uint64_t size) {
    ScopedAutoreleasePool arp_{};
    MetalBuffer* buf = new MetalBuffer{this->device, size};
    return ResourceHandle{
        reinterpret_cast<uint64_t>(buf),
        buf->buffer};
}

void MetalDevice::DestroyBuffer(ResourceHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto buf = reinterpret_cast<MetalBuffer*>(handle.Handle);
    delete buf;
}

ResourceHandle MetalDevice::CreateTexture(
    PixelFormat format,
    TextureDimension dim,
    uint32_t width, uint32_t height,
    uint32_t depth,
    uint32_t mipmap) {
    ScopedAutoreleasePool arp_{};
    MetalTexture* tex = new MetalTexture{
        this->device,
        ToMtlFormat(format),
        ToMtlTextureType(dim),
        width, height,
        depth,
        mipmap};
    return ResourceHandle{
        reinterpret_cast<uint64_t>(tex),
        tex->texture};
}

void MetalDevice::DestroyTexture(ResourceHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto tex = reinterpret_cast<MetalTexture*>(handle.Handle);
    delete tex;
}

void MetalDevice::StartFrame(CommandQueueHandle queue, SwapChainHandle swapchain) {
    ScopedAutoreleasePool arp_{};
    auto msc = reinterpret_cast<MetalSwapChain*>(swapchain.Handle);
    msc->NextDrawable();
}

void MetalDevice::FinishFrame(CommandQueueHandle queue, SwapChainHandle swapchain) {
    ScopedAutoreleasePool arp_{};
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    auto msc = reinterpret_cast<MetalSwapChain*>(swapchain.Handle);
    q->Present(msc->currentDrawable);
}

void MetalDevice::DispatchCommand(CommandQueueHandle queue, CommandList&& cmdList_) {
    ScopedAutoreleasePool _arp{};
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    CommandList cmdList = std::move(cmdList_);
    MetalCommandEncoder encoder{q};
    for (auto&& cmd : cmdList.list) {
        std::visit(encoder, cmd);
    }
#ifdef RADRAY_IS_DEBUG
    if (encoder.cmdBuffer != nullptr) {
        encoder.cmdBuffer->addCompletedHandler(^(MTL::CommandBuffer* cmdBuffer) noexcept {
          if (auto err = cmdBuffer->error()) {
              RADRAY_ERR_LOG("MTL::CommnadBuffer execute error: {}", err->localizedDescription()->utf8String());
          }
          if (auto logs = cmdBuffer->logs()) {
              RadrayPrintMTLFunctionLog(logs);
          }
        });
    }
#endif
    if (encoder.cmdBuffer != nullptr) {
        encoder.cmdBuffer->commit();
    }
}

void MetalDevice::Signal(FenceHandle fence, CommandQueueHandle queue, uint64_t value) {
    ScopedAutoreleasePool arp_{};
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    q->Signal(e->event, value);
}

void MetalDevice::Wait(FenceHandle fence, CommandQueueHandle queue, uint64_t value) {
    ScopedAutoreleasePool arp_{};
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    q->Wait(e->event, value);
}

void MetalDevice::Synchronize(FenceHandle fence, uint64_t value) {
    ScopedAutoreleasePool arp_{};
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    e->Synchronize(value);
}

}  // namespace radray::rhi::metal
