#include "metal_command_encoder.h"

namespace radray::rhi::metal {

CommandEncoder::CommandEncoder(MTL::CommandBuffer* cmdBuffer, MTL::RenderPassDescriptor* desc)
    : encoder(cmdBuffer->renderCommandEncoder(desc)->retain()) {}

CommandEncoder::~CommandEncoder() noexcept {
    encoder->endEncoding();
    encoder->release();
}

}  // namespace radray::rhi::metal
