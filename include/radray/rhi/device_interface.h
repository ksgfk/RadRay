#pragma once

#include <memory>
#include <array>

#include <radray/rhi/common.h>
#include <radray/rhi/resource.h>

namespace radray::rhi {

class DeviceInterface {
public:
    virtual ~DeviceInterface() noexcept = default;

    virtual CommandQueueHandle CreateCommandQueue(CommandListType type) = 0;
    virtual void DestroyCommandQueue(const CommandQueueHandle& handle) = 0;

    virtual SwapChainHandle CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) = 0;
    virtual void DestroySwapChain(const SwapChainHandle& handle) = 0;
    virtual ResourceHandle GetCurrentSwapChainBackBuffer(const SwapChainHandle& handle) = 0;
};

using SupportApiArray = std::array<bool, (size_t)ApiType::MAX_COUNT>;

void GetSupportApi(SupportApiArray& api);

std::unique_ptr<DeviceInterface> CreateDeviceD3D12(const DeviceCreateInfoD3D12& info);

std::unique_ptr<DeviceInterface> CreateDeviceMetal(const DeviceCreateInfoMetal& info);

}  // namespace radray::rhi
