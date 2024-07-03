#include "metal_event.h"

#include "metal_device.h"

namespace radray::rhi::metal {

MetalEvent::MetalEvent(MetalDevice* device) {
    auto ptr = device->device->newSharedEvent();
    event = NS::TransferPtr(ptr);
}

}  // namespace radray::rhi::metal
