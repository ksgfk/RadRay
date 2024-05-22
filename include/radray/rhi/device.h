#pragma once

#include <radray/core/object.h>
#include <radray/rhi/common.h>

namespace radray::rhi {

class ISwapChain;
class ICommandQueue;
class IFence;
class IBuffer;
class ITexture;

class IDevice : public Object {
public:
    virtual ~IDevice() noexcept = default;

    virtual RC<ISwapChain> CreateSwapChain(const SwapChainCreateInfo& info) = 0;

    virtual RC<ICommandQueue> CreateCommandQueue(const CommandQueueCreateInfo& info) = 0;

    virtual RC<IFence> CreateFence(const FenceCreateInfo& info) = 0;

    virtual RC<IBuffer> CreateBuffer(const BufferCreateInfo& info) = 0;

    virtual RC<ITexture> CreateTexture(const TextureCreateInfo& info) = 0;
};

RC<IDevice> CreateDeviceD3D12(const DeviceCreateInfoD3D12& info);

}  // namespace radray::rhi
