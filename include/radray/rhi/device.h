#pragma once

#include <radray/object.h>
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

    virtual std::shared_ptr<ISwapChain> CreateSwapChain(const SwapChainCreateInfo& info) = 0;

    virtual std::shared_ptr<ICommandQueue> CreateCommandQueue(const CommandQueueCreateInfo& info) = 0;

    virtual std::shared_ptr<IFence> CreateFence(const FenceCreateInfo& info) = 0;

    virtual std::shared_ptr<IBuffer> CreateBuffer(const BufferCreateInfo& info) = 0;

    virtual std::shared_ptr<ITexture> CreateTexture(const TextureCreateInfo& info) = 0;
};

std::shared_ptr<IDevice> CreateDeviceD3D12(const DeviceCreateInfoD3D12& info);

}  // namespace radray::rhi
