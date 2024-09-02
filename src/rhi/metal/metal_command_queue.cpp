#include "metal_command_queue.h"

namespace radray::rhi::metal {

CommandQueue::CommandQueue(MTL::Device* device) : queue(device->newCommandQueue()) {}

CommandQueue::~CommandQueue() noexcept {
    queue->release();
}

}  // namespace radray::rhi::metal
