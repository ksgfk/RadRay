#pragma once

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <radray/render/common.h>

namespace radray::render {

std::optional<radray::string> SpirvToMsl(std::span<byte> spirv);

}  // namespace radray::render

#endif
