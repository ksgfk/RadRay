#pragma once

#include <vector>
#include <memory>

#include <radray/d3d12/utility.h>
#include <radray/d3d12/descriptor_heap.h>
#include <radray/d3d12/shader_compiler.h>
#include <radray/d3d12/resource.h>

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
    std::unique_ptr<DescriptorHeap> globalResHeap;
    std::unique_ptr<DescriptorHeap> globalSamplerHeap;
    std::vector<D3D12_SAMPLER_DESC> staticSamplerDescs;
    std::unique_ptr<ShaderCompiler> shaderCompiler;
    std::unique_ptr<IGpuHeapAllocator> globalAlloc;
};

}  // namespace radray::d3d12
