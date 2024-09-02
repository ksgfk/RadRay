#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class Event {
public:
    explicit Event(MTL::Device* device);
    ~Event() noexcept;

public:
    MTL::SharedEvent* event;
};

}  // namespace radray::rhi::metal
