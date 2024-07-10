#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalBuffer {
public:
    MetalBuffer(MTL::Device* device, size_t size);
    MetalBuffer(const MetalBuffer&) = delete;
    MetalBuffer(MetalBuffer&&) = delete;
    MetalBuffer& operator=(const MetalBuffer&) = delete;
    MetalBuffer& operator=(MetalBuffer&&) = delete;
    ~MetalBuffer() noexcept;

public:
    MTL::Buffer* buffer;
};

}  // namespace radray::rhi::metal
