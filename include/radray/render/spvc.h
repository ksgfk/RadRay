#pragma once

#include <radray/render/common.h>

namespace radray::render {

class MslReflection {
public:
    class VertexInput {
    public:
        radray::string Name;
        uint32_t Location;
        VertexFormat Format;
        VertexSemantic Semantic;
        uint32_t SemanticIndex;
    };

    radray::vector<VertexInput> VertexInputs;
};

class SpirvReflection {};

};  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

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

class SpvcEntryPoint {
public:
    radray::string Name;
    ShaderStage Stage;
};

class SpvcMslOutput {
public:
    radray::string Msl;
    radray::vector<SpvcEntryPoint> EntryPoints;
};

std::pair<uint32_t, uint32_t> GetMslVersionNumber(MslVersion ver) noexcept;

std::optional<SpvcMslOutput> SpirvToMsl(
    std::span<byte> spirv,
    MslVersion ver,
    MslPlatform plat);

}  // namespace radray::render

#endif
