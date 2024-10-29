#pragma once

#include <radray/render/device.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class Device : public radray::render::Device {
public:
    ~Device() noexcept override = default;

    Backend GetBackend() noexcept override { return Backend::D3D12; }

private:
    ComPtr<ID3D12Device> _device;
    D3D_FEATURE_LEVEL _maxFeature;
    D3D_SHADER_MODEL _maxShaderModel;

    friend std::optional<std::shared_ptr<Device>> CreateDevice(const D3D12DeviceDescriptor& desc);
};

std::optional<std::shared_ptr<Device>> CreateDevice(const D3D12DeviceDescriptor& desc);

}  // namespace radray::render::d3d12
