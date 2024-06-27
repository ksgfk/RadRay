#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalDevice;

class MetalBuffer {
public:
    MetalBuffer(MetalDevice* device, size_t size);

public:
    NS::SharedPtr<MTL::Buffer> buffer;
};

}  // namespace radray::rhi::metal
