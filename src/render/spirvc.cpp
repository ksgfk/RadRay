#include <radray/render/spirvc.h>

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <spirv_cross/spirv_msl.hpp>

#include <radray/logger.h>

namespace radray::render {

std::pair<uint32_t, uint32_t> GetMslVersionNumber(MslVersion ver) noexcept {
    switch (ver) {
        case MslVersion::MSL11: return {1, 1};
        case MslVersion::MSL12: return {1, 2};
        case MslVersion::MSL20: return {2, 0};
        case MslVersion::MSL21: return {2, 1};
        case MslVersion::MSL22: return {2, 2};
        case MslVersion::MSL23: return {2, 3};
        case MslVersion::MSL24: return {2, 4};
        case MslVersion::MSL30: return {3, 0};
        case MslVersion::MSL31: return {3, 1};
        case MslVersion::MSL32: return {3, 2};
    }
}

static spirv_cross::CompilerMSL::Options::Platform Cvt(MslPlatform plat) noexcept {
    switch (plat) {
        case MslPlatform::Macos: return spirv_cross::CompilerMSL::Options::Platform::macOS;
        case MslPlatform::Ios: return spirv_cross::CompilerMSL::Options::Platform::iOS;
    }
}

std::optional<radray::string> SpirvToMsl(
    std::span<byte> spirv,
    MslVersion ver,
    MslPlatform plat) {
    auto dword = ByteToDWORD({reinterpret_cast<uint8_t*>(spirv.data()), spirv.size()});
    auto [verMaj, verMin] = GetMslVersionNumber(ver);
    try {
        spirv_cross::CompilerMSL mslc{dword.data(), dword.size()};
        auto opts = mslc.get_msl_options();
        opts.set_msl_version(verMaj, verMin);
        opts.platform = Cvt(plat);
        opts.invariant_float_math = true;
        mslc.set_msl_options(opts);
        return radray::string{mslc.compile()};
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("cannot convert SPIR-V to MSL\n{}", e.what());
        return std::nullopt;
    }
}

}  // namespace radray::render

#endif
