#pragma once

#include <radray/render/command_buffer.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class DeviceD3D12;
class CmdAllocatorD3D12;

class CmdListD3D12 : public CommandBuffer {
public:
    CmdListD3D12(DeviceD3D12* device, CmdAllocatorD3D12* alloc, D3D12_COMMAND_LIST_TYPE type) noexcept
        : _device(device),
          _alloc(alloc),
          _type(type) {}
    ~CmdListD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _cmdList.Get() != nullptr; }
    void Destroy() noexcept override;

public:
    ComPtr<ID3D12GraphicsCommandList> _cmdList;
    DeviceD3D12* _device;
    CmdAllocatorD3D12* _alloc;
    D3D12_COMMAND_LIST_TYPE _type;
};

}  // namespace radray::render::d3d12
