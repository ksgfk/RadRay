#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalDevice;

class MetalEvent {
public:
    MetalEvent(MetalDevice* device);

public:
    NS::SharedPtr<MTL::SharedEvent> event;
};

}  // namespace radray::rhi::metal
