#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class CommandQueue {
public:
    explicit CommandQueue(MTL::Device* device);
    ~CommandQueue() noexcept;

public:
    MTL::CommandQueue* queue;
};

}  // namespace radray::rhi::metal
