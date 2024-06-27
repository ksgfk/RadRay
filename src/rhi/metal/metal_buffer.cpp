#include "metal_buffer.h"

#include "metal_device.h"

namespace radray::rhi::metal {

MetalBuffer::MetalBuffer(MetalDevice* device, size_t size) {
    auto buf = device->device->newBuffer(
        size,
        MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeTracked);
    buffer = NS::TransferPtr(buf);
}

}  // namespace radray::rhi::metal
