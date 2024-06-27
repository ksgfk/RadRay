#pragma once

#include <radray/rhi/device_interface.h>
#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalDevice : public DeviceInterface {
public:
    MetalDevice();
    ~MetalDevice() noexcept override;

    CommandQueueHandle CreateCommandQueue(CommandListType type) override;
    void DestroyCommandQueue(const CommandQueueHandle& handle) override;

    SwapChainHandle CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) override;
    void DestroySwapChain(const SwapChainHandle& handle) override;

    ResourceHandle CreateBuffer(BufferType type, uint64_t size) override;
    void DestroyBuffer(ResourceHandle handle) override;

    ResourceHandle CreateTexture(
        PixelFormat format,
        TextureDimension dim,
        uint32_t width, uint32_t height,
        uint32_t depth,
        uint32_t mipmap) override;
    void DestroyTexture(ResourceHandle handle) override;

public:
    NS::SharedPtr<MTL::Device> device;
};

std::unique_ptr<MetalDevice> CreateImpl(const DeviceCreateInfoMetal& info);

}  // namespace radray::rhi::metal
