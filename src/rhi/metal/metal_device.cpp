#include "metal_device.h"

#include "dispatch_semaphore.h"
#include "metal_command_queue.h"
#include "metal_command_buffer.h"
#include "metal_swapchain.h"

namespace radray::rhi::metal {

Device::Device(const RadrayDeviceDescriptorMetal& desc) {
    AutoRelease([this, &desc]() {
        auto allDevices = MTL::CopyAllDevices()->autorelease();
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

void Device::SubmitQueue(const RadraySubmitQueueDescriptor& desc) {
    auto q = reinterpret_cast<CommandQueue*>(desc.Queue.Ptr);
    // for (size_t i = 0; i < desc.WaitSemaphoreCount; i++) {
    // TODO:
    // }
    for (size_t i = 0; i < desc.ListCount; i++) {
        auto cb = reinterpret_cast<CommandBuffer*>(desc.Lists[i].Ptr);
        cb->Commit();
    }
    if (!RADRAY_RHI_IS_EMPTY_RES(desc.SignalFence)) {
        auto sem = reinterpret_cast<Semaphore*>(desc.SignalFence.Ptr);
        auto cb = q->queue->commandBufferWithUnretainedReferences();
        cb->addCompletedHandler([=](MTL::CommandBuffer* cmdBuf) {
            sem->Signal();
        });
        cb->commit();
    }
    // for (size_t i = 0; i < desc.SignalSemaphoreCount; i++) {
    // TODO:
    // }
}

void Device::WaitQueue(RadrayCommandQueue queue) {
    auto q = reinterpret_cast<CommandQueue*>(queue.Ptr);
    MTL::CommandBuffer* cb = q->queue->commandBufferWithUnretainedReferences();
    cb->commit();
    cb->waitUntilCompleted();
}

RadrayFence Device::CreateFence() {
    return AutoRelease([]() {
        auto e = RhiNew<Semaphore>(1);
        return RadrayFence{e, e->semaphore};
    });
}

void Device::DestroyFence(RadrayFence fence) {
    AutoRelease([=]() {
        auto e = reinterpret_cast<Semaphore*>(fence.Ptr);
        RhiDelete(e);
    });
}

RadrayFenceState Device::GetFenceState(RadrayFence fence) {
    auto e = reinterpret_cast<Semaphore*>(fence.Ptr);
    return e->count < 0 ? RADRAY_FENCE_STATE_INCOMPLETE : RADRAY_FENCE_STATE_COMPLETE;
}

void Device::WaitFences(std::span<const RadrayFence> fences) {
    for (auto&& i : fences) {
        auto e = reinterpret_cast<Semaphore*>(i.Ptr);
        e->Wait();
    }
}

RadrayCommandAllocator Device::CreateCommandAllocator(RadrayCommandQueue queue) {
    return RadrayCommandAllocator{queue.Ptr, queue.Native};
}

void Device::DestroyCommandAllocator(RadrayCommandAllocator alloc) {}

RadrayCommandList Device::CreateCommandList(RadrayCommandAllocator alloc) {
    return AutoRelease([=]() {
        auto q = reinterpret_cast<CommandQueue*>(alloc.Ptr);
        auto cb = RhiNew<CommandBuffer>(q->queue);
        return RadrayCommandList{cb, cb->cmdBuffer};
    });
}

void Device::DestroyCommandList(RadrayCommandList list) {
    AutoRelease([=]() {
        auto cb = reinterpret_cast<CommandBuffer*>(list.Ptr);
        RhiDelete(cb);
    });
}

void Device::ResetCommandAllocator(RadrayCommandAllocator alloc) {}

void Device::BeginCommandList(RadrayCommandList list) {
    
}

void Device::EndCommandList(RadrayCommandList list) {}

RadraySwapChain Device::CreateSwapChain(const RadraySwapChainDescriptor& desc) {
    return AutoRelease([&desc, this]() {
        auto q = reinterpret_cast<CommandQueue*>(desc.PresentQueue.Ptr);
        auto sc = RhiNew<SwapChain>(
            device,
            q->queue,
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

uint32_t Device::AcquireNextRenderTarget(RadraySwapChain swapchain) {
    return AutoRelease([=]() {
        auto sc = reinterpret_cast<SwapChain*>(swapchain.Ptr);
        sc->AcquireNextDrawable();
        return 0;
    });
}

void Device::Present(RadraySwapChain swapchain) {
    AutoRelease([=]() {
        auto sc = reinterpret_cast<SwapChain*>(swapchain.Ptr);
        MTL::CommandBuffer* cmdBuffer = sc->queue->commandBufferWithUnretainedReferences();
        cmdBuffer->presentDrawable(sc->currentDrawable);
        cmdBuffer->commit();
    });
}

}  // namespace radray::rhi::metal
