#pragma once

#include <radray/rhi/command.h>

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalCommandQueue;

class MetalCommandEncoder {
public:
    explicit MetalCommandEncoder(MetalCommandQueue* queue);

    void operator()(const ClearRenderTargetCommand& cmd);
    void operator()(const PresentCommand& cmd);

private:
    void CheckCmdBuffer();

public:
    MetalCommandQueue* queue;
    MTL::CommandBuffer* cmdBuffer{nullptr};
};

}  // namespace radray::rhi::metal
