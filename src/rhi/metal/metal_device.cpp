#include "metal_device.h"

#include <radray/logger.h>

#include "metal_command_queue.h"
#include "metal_command_encoder.h"
#include "metal_swap_chain.h"
#include "metal_buffer.h"
#include "metal_texture.h"
#include "metal_event.h"

namespace radray::rhi::metal {

MetalDevice::MetalDevice() = default;

MetalDevice::~MetalDevice() noexcept = default;

std::shared_ptr<MetalDevice> CreateImpl(const DeviceCreateInfoMetal& info) {
    auto all = NS::TransferPtr(MTL::CopyAllDevices());
    auto deviceCount = all->count();
    if (info.DeviceIndex >= deviceCount) {
        RADRAY_ERR_LOG("device index out of range. count = {}", deviceCount);
        return std::shared_ptr<MetalDevice>{};
    }
    MTL::Device* device = all->object<MTL::Device>(info.DeviceIndex);
    RADRAY_INFO_LOG("select metal device: {}", device->name()->utf8String());
    auto result = std::make_shared<MetalDevice>();
    result->device = NS::TransferPtr(device);
    return result;
}

CommandQueueHandle MetalDevice::CreateCommandQueue(CommandListType type) {
    MetalCommandQueue* mcq = new MetalCommandQueue{this, 0};
    return CommandQueueHandle{
        reinterpret_cast<uint64_t>(mcq),
        mcq->queue.get()};
}

void MetalDevice::DestroyCommandQueue(CommandQueueHandle handle) {
    auto mcq = reinterpret_cast<MetalCommandQueue*>(handle.Handle);
    delete mcq;
}

FenceHandle MetalDevice::CreateFence() {
    auto e = new MetalEvent{this};
    return FenceHandle{
        reinterpret_cast<uint64_t>(e),
        e->event.get()};
}

void MetalDevice::DestroyFence(FenceHandle handle) {
    auto e = reinterpret_cast<MetalEvent*>(handle.Handle);
    delete e;
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

void MetalDevice::DestroySwapChain(SwapChainHandle handle) {
    auto msc = reinterpret_cast<MetalSwapChain*>(handle.Handle);
    delete msc;
}

ResourceHandle MetalDevice::CreateBuffer(BufferType type, uint64_t size) {
    MetalBuffer* buf = new MetalBuffer{this, size};
    return ResourceHandle{
        reinterpret_cast<uint64_t>(buf),
        buf->buffer.get()};
}

void MetalDevice::DestroyBuffer(ResourceHandle handle) {
    MetalBuffer* buf = reinterpret_cast<MetalBuffer*>(handle.Handle);
    delete buf;
}

ResourceHandle MetalDevice::CreateTexture(
    PixelFormat format,
    TextureDimension dim,
    uint32_t width, uint32_t height,
    uint32_t depth,
    uint32_t mipmap) {
    MetalTexture* tex = new MetalTexture{
        this,
        ToMtlFormat(format),
        ToMtlTextureType(dim),
        width, height,
        depth,
        mipmap};
    return ResourceHandle{
        reinterpret_cast<uint64_t>(tex),
        tex->texture.get()};
}

void MetalDevice::DestroyTexture(ResourceHandle handle) {
    MetalTexture* tex = reinterpret_cast<MetalTexture*>(handle.Handle);
    delete tex;
}

void MetalDevice::DispatchCommand(CommandQueueHandle queue, CommandList&& cmdList_) {
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    CommandList cmdList = std::move(cmdList_);
    MetalCommandEncoder encoder{q};
    for (auto&& cmd : cmdList.list) {
        std::visit(encoder, cmd);
    }
#ifdef RADRAY_IS_DEBUG
    if (encoder.cmdBuffer != nullptr) {
        encoder.cmdBuffer->addCompletedHandler(^(MTL::CommandBuffer* cmdBuffer) noexcept {
          ScopedAutoreleasePool _arp{};
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
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    auto cmdBuffer = q->queue->commandBufferWithUnretainedReferences();
    cmdBuffer->encodeSignalEvent(e->event.get(), value);
    cmdBuffer->commit();
}

void MetalDevice::Wait(FenceHandle fence, CommandQueueHandle queue, uint64_t value) {
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    auto cmdBuffer = q->queue->commandBufferWithUnretainedReferences();
    if (value == 0) {
        RADRAY_WARN_LOG("MetalDevice::Wait() is called before any signal");
    } else {
        cmdBuffer->encodeWait(e->event.get(), value);
    }
    cmdBuffer->commit();
}

void MetalDevice::Synchronize(FenceHandle fence, uint64_t value) {
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    while (e->event->signaledValue() < value) {
        std::this_thread::yield();
    }
}

}  // namespace radray::rhi::metal