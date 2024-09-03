#include "metal_command_buffer.h"

namespace radray::rhi::metal {

CommandBuffer::CommandBuffer(MTL::CommandQueue* queue)
    : queue(queue),
      cmdBuffer(queue->commandBufferWithUnretainedReferences()) {
    queue->retain();
    cmdBuffer->retain();
}

CommandBuffer::~CommandBuffer() noexcept {
    queue->release();
    cmdBuffer->release();
}

void CommandBuffer::Commit() {
    cmdBuffer->commit();
    cmdBuffer->release();
    cmdBuffer = queue->commandBufferWithUnretainedReferences();
}

}  // namespace radray::rhi::metal
