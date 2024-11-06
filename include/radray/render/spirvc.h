#pragma once

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <radray/render/common.h>

namespace radray::render {

enum class MslVersion {
    MSL11,
    MSL12,
    MSL20,
    MSL21,
    MSL22,
    MSL23,
    MSL24,
    MSL30,
    MSL31,
    MSL32,
};

enum class MslPlatform {
    Macos,
    Ios
};

std::pair<uint32_t, uint32_t> GetMslVersionNumber(MslVersion ver) noexcept;

std::optional<radray::string> SpirvToMsl(
    std::span<byte> spirv,
    MslVersion ver,
    MslPlatform plat);

}  // namespace radray::render

#endif
