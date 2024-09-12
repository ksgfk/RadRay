#include "metal_command_buffer.h"

namespace radray::rhi::metal {

CommandBuffer::CommandBuffer(MTL::CommandQueue* queue)
    : queue(queue->retain()),
      cmdBuffer(nullptr) {}

CommandBuffer::~CommandBuffer() noexcept {
    queue->release();
    if (cmdBuffer != nullptr) {
        cmdBuffer->release();
        cmdBuffer = nullptr;
    }
}

void CommandBuffer::Begin() {
    if (cmdBuffer != nullptr) {
        cmdBuffer->release();
        cmdBuffer = nullptr;
    }
    auto desc = MTL::CommandBufferDescriptor::alloc()->autorelease();
    desc->setRetainedReferences(false);
#if RADRAY_IS_DEBUG
    desc->setErrorOptions(MTL::CommandBufferErrorOptionEncoderExecutionStatus);
#else
    desc->setErrorOptions(MTL::CommandBufferErrorOptionNone);
#endif
    cmdBuffer = queue->commandBuffer(desc);
    cmdBuffer->retain();
}

void CommandBuffer::Commit() {
    if (cmdBuffer == nullptr) {
        return;
    }
    cmdBuffer->commit();
    cmdBuffer->release();
    cmdBuffer = nullptr;
}

}  // namespace radray::rhi::metal
