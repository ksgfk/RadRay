#include "metal_library.h"

#include "dispatch_data.h"

namespace radray::rhi::metal {

Library::Library(MTL::Device* device, std::span<const uint8_t> ir, const char* entry) {
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
    auto name = NS::String::alloc()->init(entry, NS::UTF8StringEncoding)->autorelease();
    entryPoint = lib->newFunction(name);
    if (entryPoint == nullptr) {
        RADRAY_MTL_THROW("cannot create MTLFunction");
    }
}

Library::~Library() noexcept {
    if (lib != nullptr) {
        lib->release();
    }
    if (entryPoint != nullptr) {
        entryPoint->release();
    }
}

}  // namespace radray::rhi::metal
