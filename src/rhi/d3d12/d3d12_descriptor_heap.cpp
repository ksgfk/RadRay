#include "d3d12_descriptor_heap.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

DescriptorHeap::DescriptorHeap(
    Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    uint32_t length,
    bool isShaderVisible) {
}

}  // namespace radray::rhi::d3d12
