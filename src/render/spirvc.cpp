#include <radray/render/spirvc.h>

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <spirv_cross/spirv_msl.hpp>

#include <radray/logger.h>

namespace radray::render {

std::optional<radray::string> SpirvToMsl(std::span<byte> spirv) {
    auto dword = ByteToDWORD({reinterpret_cast<uint8_t*>(spirv.data()), spirv.size()});
    try {
        spirv_cross::CompilerMSL mslc{dword.data(), dword.size()};
        return radray::string{mslc.compile()};
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("cannot convert SPIR-V to MSL\n{}", e.what());
        return std::nullopt;
    }
}

}  // namespace radray::render

#endif
