#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalDevice;

class MetalCommandQueue {
public:
    MetalCommandQueue(MetalDevice* device, size_t maxCommands);

public:
    NS::SharedPtr<MTL::CommandQueue> queue;
};

}  // namespace radray::rhi::metal
