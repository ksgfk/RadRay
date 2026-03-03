#pragma once

#include <optional>

#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/msl.h>
#include <radray/render/spvc.h>
#include <radray/structured_buffer.h>

namespace radray::render {

struct ShaderParameter {
    string Name;
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t Register{0};
    uint32_t Space{0};
    uint32_t ArrayLength{1};
    uint32_t TypeSizeInBytes{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    bool IsPushConstant{false};
    bool IsBindless{false};
};

class ShaderReflection {
public:
    static vector<ShaderParameter> ExtractParameters(const HlslShaderDesc& desc) noexcept;
    static vector<ShaderParameter> ExtractParameters(const SpirvShaderDesc& desc) noexcept;
    static vector<ShaderParameter> ExtractParameters(const MslShaderReflection& desc) noexcept;

    static std::optional<StructuredBufferStorage::Builder> CreateCBufferLayout(const HlslShaderDesc& desc) noexcept;
    static std::optional<StructuredBufferStorage::Builder> CreateCBufferLayout(const SpirvShaderDesc& desc) noexcept;
    static std::optional<StructuredBufferStorage::Builder> CreateCBufferLayout(const MslShaderReflection& desc) noexcept;
};

}  // namespace radray::render
