#include "metal_command_queue.h"

namespace radray::rhi::metal {

static MTL::CommandQueue* NewQueueImpl(MTL::Device* device, size_t maxCommands) {
    return maxCommands == 0 ? device->newCommandQueue() : device->newCommandQueue(maxCommands);
}

MetalCommandQueue::MetalCommandQueue(
    MTL::Device* device,
    size_t maxCommands)
    : queue(NewQueueImpl(device, maxCommands)) {}

MetalCommandQueue::~MetalCommandQueue() noexcept {
    queue->release();
}

void MetalCommandQueue::Signal(MTL::SharedEvent* event, uint64_t value) {
    MTL::CommandBuffer* cmdBuffer = queue->commandBufferWithUnretainedReferences();
    cmdBuffer->encodeSignalEvent(event, value);
    cmdBuffer->commit();
}

void MetalCommandQueue::Wait(MTL::SharedEvent* event, uint64_t value) {
    MTL::CommandBuffer* cmdBuffer = queue->commandBufferWithUnretainedReferences();
    if (value == 0) {
        RADRAY_WARN_LOG("MetalCommandQueue::Wait() called before signal");
    } else {
        cmdBuffer->encodeWait(event, value);
    }
    cmdBuffer->commit();
}

void MetalCommandQueue::Synchronize() {
    MTL::CommandBuffer* cmdBuffer = queue->commandBufferWithUnretainedReferences();
    cmdBuffer->commit();
    cmdBuffer->waitUntilCompleted();
}

}  // namespace radray::rhi::metal
