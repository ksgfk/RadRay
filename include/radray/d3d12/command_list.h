#pragma once

#include <span>

#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

class Device;
class CommandAllocator;

class CommandList {
public:
    CommandList(Device* device, CommandAllocator* allocator) noexcept;

    void Reset();
    void Close();

    void Upload(ID3D12Resource* dst, uint64 dstOffset, std::span<const uint8> src);

    Device* device;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    CommandAllocator* alloc;
    bool isOpen;
};

}  // namespace radray::d3d12
