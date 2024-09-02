#include "metal_event.h"

namespace radray::rhi::metal {

Event::Event(MTL::Device* device) : event(device->newSharedEvent()) {}

Event::~Event() noexcept {
    event->release();
}

}  // namespace radray::rhi::metal
