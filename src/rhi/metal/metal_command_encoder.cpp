#include "metal_command_encoder.h"

namespace radray::rhi::metal {

CommandEncoder::CommandEncoder(MTL::CommandBuffer* cmdBuffer, MTL::RenderPassDescriptor* desc)
    : encoder(cmdBuffer->renderCommandEncoder(desc)) {}

CommandEncoder::~CommandEncoder() noexcept {
    encoder->endEncoding();
}

}  // namespace radray::rhi::metal
