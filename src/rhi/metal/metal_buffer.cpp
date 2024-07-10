#include "metal_buffer.h"

namespace radray::rhi::metal {

MetalBuffer::MetalBuffer(MTL::Device* device, size_t size)
    : buffer(device->newBuffer(
          size,
          MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeTracked)) {}

MetalBuffer::~MetalBuffer() noexcept {
    buffer->release();
}

}  // namespace radray::rhi::metal
