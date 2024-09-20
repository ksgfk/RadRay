#include "metal_command_encoder.h"

namespace radray::rhi::metal {

RenderCommandEncoder::RenderCommandEncoder(MTL::CommandBuffer* cmdBuffer, MTL::RenderPassDescriptor* desc)
    : encoder(cmdBuffer->renderCommandEncoder(desc)->retain()) {}

RenderCommandEncoder::~RenderCommandEncoder() noexcept {
    encoder->endEncoding();
    encoder->release();
}

}  // namespace radray::rhi::metal
