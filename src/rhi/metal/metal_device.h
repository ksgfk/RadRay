#pragma once

#include <radray/rhi/device_interface.h>
#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalDevice : public DeviceInterface {
public:
    MetalDevice();
    ~MetalDevice() noexcept override;

    CommandQueueHandle CreateCommandQueue(CommandListType type) override;
    void DestroyCommandQueue(CommandQueueHandle handle) override;

    FenceHandle CreateFence() override;
    void DestroyFence(FenceHandle handle) override;

    SwapChainHandle CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) override;
    void DestroySwapChain(SwapChainHandle handle) override;

    ResourceHandle CreateBuffer(BufferType type, uint64_t size) override;
    void DestroyBuffer(ResourceHandle handle) override;

    ResourceHandle CreateTexture(
        PixelFormat format,
        TextureDimension dim,
        uint32_t width, uint32_t height,
        uint32_t depth,
        uint32_t mipmap) override;
    void DestroyTexture(ResourceHandle handle) override;

    void DispatchCommand(CommandQueueHandle queue, CommandList&& cmdList) override;
    void Signal(FenceHandle fence, CommandQueueHandle queue, uint64_t value) override;
    void Wait(FenceHandle fence, CommandQueueHandle queue, uint64_t value) override;
    void Synchronize(FenceHandle fence, uint64_t value) override;

public:
    NS::SharedPtr<MTL::Device> device;
};

std::shared_ptr<MetalDevice> CreateImpl(const DeviceCreateInfoMetal& info);

}  // namespace radray::rhi::metal
