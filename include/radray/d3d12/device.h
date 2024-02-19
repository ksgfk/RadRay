#pragma once

#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

class Device {
public:
    Device() noexcept;
    ~Device() noexcept = default;
    Device(Device&&) noexcept = default;
    Device& operator=(Device&&) noexcept = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

public:
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
    ComPtr<IDXGIFactory6> dxgiFactory;
};

}  // namespace radray::d3d12
