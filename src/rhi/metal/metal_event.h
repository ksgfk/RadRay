#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalEvent {
public:
    explicit MetalEvent(MTL::Device* device);
    MetalEvent(const MetalEvent&) = delete;
    MetalEvent(MetalEvent&&) = delete;
    MetalEvent& operator=(const MetalEvent&) = delete;
    MetalEvent& operator=(MetalEvent&&) = delete;
    ~MetalEvent() noexcept;

    bool IsCompleted(uint64_t value) const;
    void Synchronize(uint64_t value);

public:
    MTL::SharedEvent* event;
};

}  // namespace radray::rhi::metal
