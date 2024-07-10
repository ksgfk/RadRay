#include "metal_event.h"

namespace radray::rhi::metal {

MetalEvent::MetalEvent(MTL::Device* device) : event(device->newSharedEvent()) {}

MetalEvent::~MetalEvent() noexcept {
    if (event != nullptr) {
        event->release();
        event = nullptr;
    }
}

bool MetalEvent::IsCompleted(uint64_t value) const {
    return event->signaledValue() >= value;
}

void MetalEvent::Synchronize(uint64_t value) {
    if (value == 0) {
        RADRAY_WARN_LOG("MetalEvent::synchronize() is called before any signal");
        return;
    }
    while (!IsCompleted(value)) {
        std::this_thread::yield();
    }
}

}  // namespace radray::rhi::metal
