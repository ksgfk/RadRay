#include "metal_library.h"

#include "dispatch_data.h"

namespace radray::rhi::metal {

Library::Library(MTL::Device* device, std::span<const uint8_t> ir) {
    DispatchData data{ir.data(), ir.size()};
    NS::Error* pErr;
    MTL::Library* mtllib = device->newLibrary(data.data, &pErr);
    if (pErr) {
        pErr->autorelease();
        auto reason = pErr->localizedDescription()->utf8String();
        RADRAY_ERR_LOG("cannot create MTLLibrary, reason={}", reason);
        RADRAY_MTL_THROW("cannot create MTLLibrary");
    }
    lib = mtllib;
}

Library::~Library() noexcept {
    if (lib != nullptr) {
        lib->release();
    }
}

}  // namespace radray::rhi::metal
