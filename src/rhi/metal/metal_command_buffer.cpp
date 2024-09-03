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
    cmdBuffer = queue->commandBufferWithUnretainedReferences();
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
