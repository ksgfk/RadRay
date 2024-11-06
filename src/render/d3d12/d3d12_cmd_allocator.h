#pragma once

#include <radray/render/command_pool.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class DeviceD3D12;

class CmdAllocatorD3D12 : public CommandPool {
public:
    CmdAllocatorD3D12(
        ComPtr<ID3D12CommandAllocator> alloc,
        std::shared_ptr<DeviceD3D12> device,
        D3D12_COMMAND_LIST_TYPE type) noexcept
        : _cmdAlloc(std::move(alloc)),
          _device(std::move(device)),
          _type(type) {}
    ~CmdAllocatorD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _cmdAlloc.Get() != nullptr; }
    void Destroy() noexcept override;

    std::optional<radray::shared_ptr<CommandBuffer>> CreateCommandBuffer() noexcept override;

public:
    ComPtr<ID3D12CommandAllocator> _cmdAlloc;
    std::shared_ptr<DeviceD3D12> _device;
    D3D12_COMMAND_LIST_TYPE _type;
};

}  // namespace radray::render::d3d12
