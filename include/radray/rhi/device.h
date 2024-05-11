#pragma once

#include <radray/core/object.h>
#include <radray/rhi/common.h>

namespace radray::rhi {

class ISwapChain;
class ICommandQueue;
class IFence;
class IBuffer;
class ITexture;
class IShader;

class IDevice : public Object {
public:
    virtual ~IDevice() noexcept = default;

    virtual void CreateSwapChain(const SwapChainCreateInfo& info, ISwapChain** ppSwapChain) = 0;

    virtual void CreateCommandQueue(const CommandQueueCreateInfo& info, ICommandQueue** ppQueue) = 0;

    virtual void CreateFence(const FenceCreateInfo& info, IFence** ppFence) = 0;

    virtual void CreateBuffer(const BufferCreateInfo& info, IBuffer** ppBuffer) = 0;

    virtual void CreateTexture(const TextureCreateInfo& info, ITexture** ppTexture) = 0;

    virtual void CreateShader(const ShaderCreateInfo& info, IShader** ppShader) = 0;

    static void CreateD3D12(const DeviceCreateInfoD3D12& info, IDevice** ppDevice);
};

}  // namespace radray::rhi
