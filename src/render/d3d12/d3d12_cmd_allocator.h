#pragma once

#include <radray/render/command_pool.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class CmdAllocatorD3D12 : public CommandPool {
public:
    CmdAllocatorD3D12(
        ComPtr<ID3D12CommandAllocator> alloc,
        D3D12_COMMAND_LIST_TYPE type) noexcept
        : _cmdAlloc(std::move(alloc)),
          _type(type) {}
    ~CmdAllocatorD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _cmdAlloc.Get() != nullptr; }
    void Destroy() noexcept override;

    void Reset() noexcept override;

public:
    ComPtr<ID3D12CommandAllocator> _cmdAlloc;
    D3D12_COMMAND_LIST_TYPE _type;
};

}  // namespace radray::render::d3d12
