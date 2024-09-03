#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class CommandBuffer {
public:
    explicit CommandBuffer(MTL::CommandQueue* queue);
    ~CommandBuffer() noexcept;

    void Begin();
    void Commit();

public:
    MTL::CommandQueue* queue;
    MTL::CommandBuffer* cmdBuffer;
};

}  // namespace radray::rhi::metal
