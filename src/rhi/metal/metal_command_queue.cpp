#include "metal_command_queue.h"

#include "metal_device.h"

namespace radray::rhi::metal {

MetalCommandQueue::MetalCommandQueue(MetalDevice* device, size_t maxCommands) {
    auto q = maxCommands == 0 ? device->device->newCommandQueue() : device->device->newCommandQueue(maxCommands);
    queue = NS::TransferPtr(q);
}

}  // namespace radray::rhi::metal
