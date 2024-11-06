#pragma once

#include <radray/render/command_buffer.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class DeviceD3D12;

class CmdListD3D12 : public CommandBuffer {
public:
    CmdListD3D12(
        ComPtr<ID3D12GraphicsCommandList> cmdList,
        std::shared_ptr<DeviceD3D12> device,
        D3D12_COMMAND_LIST_TYPE type) noexcept
        : _cmdList(std::move(cmdList)),
          _device(std::move(device)),
          _type(type) {}
    ~CmdListD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _cmdList.Get() != nullptr; }
    void Destroy() noexcept override;

public:
    ComPtr<ID3D12GraphicsCommandList> _cmdList;
    std::shared_ptr<DeviceD3D12> _device;
    D3D12_COMMAND_LIST_TYPE _type;
};

}  // namespace radray::render::d3d12
