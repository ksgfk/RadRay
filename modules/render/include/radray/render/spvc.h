#pragma once

#include <radray/render/common.h>

namespace radray::render {

struct SpirvWorkgroupSize {
    uint32_t x = 0, y = 0, z = 0;
    uint32_t id_x = 0, id_y = 0, id_z = 0;
    uint32_t constant = 0;
};

class SpirvShaderDesc {
public:
    SpirvWorkgroupSize workgroupSize;
};

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

namespace radray::render {

std::optional<SpirvShaderDesc> ReflectSpirv(std::string_view entryPointName, ShaderStage stage, std::span<const byte> data);

}  // namespace radray::render

#endif
