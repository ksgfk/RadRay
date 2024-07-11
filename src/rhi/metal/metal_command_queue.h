#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalCommandQueue {
public:
    MetalCommandQueue(MTL::Device* device, size_t maxCommands);
    MetalCommandQueue(const MetalCommandQueue&) = delete;
    MetalCommandQueue(MetalCommandQueue&&) = delete;
    MetalCommandQueue& operator=(const MetalCommandQueue&) = delete;
    MetalCommandQueue& operator=(MetalCommandQueue&&) = delete;
    ~MetalCommandQueue() noexcept;

    void Signal(MTL::SharedEvent* event, uint64_t value);
    void Wait(MTL::SharedEvent* event, uint64_t value);
    void Synchronize();

public:
    MTL::CommandQueue* queue;
};

}  // namespace radray::rhi::metal
