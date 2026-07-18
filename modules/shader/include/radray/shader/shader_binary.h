#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <variant>

#include <radray/nullable.h>
#include <radray/shader/shader_asset_data.h>
#include <radray/shader/spirv.h>

namespace radray::shader {

using ShaderReflectionDesc = std::variant<HlslShaderDesc, SpirvShaderDesc>;

struct ShaderHash {
    uint64_t Low{0};
    uint64_t High{0};

    friend bool operator==(const ShaderHash&, const ShaderHash&) noexcept = default;
    friend auto operator<=>(const ShaderHash&, const ShaderHash&) noexcept = default;
};

struct CompiledShaderStage {
    ShaderTarget Target{ShaderTarget::DXIL};
    ShaderBlobCategory Category{ShaderBlobCategory::DXIL};
    uint32_t PassIndex{0};
    ShaderStage Stage{ShaderStage::UNKNOWN};
    vector<string> Defines;
    string EntryPoint;
    vector<byte> Bytecode;
    string ReflectionPayload;
    std::optional<ShaderReflectionDesc> Reflection;
    ShaderHash BinaryHash{};
    ShaderHash InterfaceHash{};
};

class ShaderBinary {
public:
    ShaderAssetData Asset;
    vector<CompiledShaderStage> Stages;

    bool IsValid() const noexcept;
    Nullable<const CompiledShaderStage*> Find(
        ShaderTarget target,
        uint32_t passIndex,
        ShaderStage stage,
        const vector<string>& defines) const noexcept;
};

ShaderHash HashShaderBytes(std::span<const byte> data) noexcept;
ShaderBlobCategory GetShaderBlobCategory(ShaderTarget target) noexcept;

std::optional<ShaderBinary> ReadShaderBinary(const std::filesystem::path& path) noexcept;
bool WriteShaderBinary(const std::filesystem::path& path, const ShaderBinary& binary) noexcept;

}  // namespace radray::shader
