#pragma once

#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <variant>

#include <radray/nullable.h>
#include <radray/shader/shader_asset_data.h>
#include <radray/shader/shader_interface.h>

namespace radray::shader {

using ShaderReflectionDesc = std::variant<HlslShaderDesc, SpirvShaderDesc>;

inline constexpr uint32_t kInvalidShaderTableIndex = std::numeric_limits<uint32_t>::max();

struct ShaderReflectionRecord {
    ShaderTarget Target{ShaderTarget::DXIL};
    ShaderReflectionDesc Reflection;
    ShaderHash Hash{};

    friend bool operator==(const ShaderReflectionRecord&, const ShaderReflectionRecord&) = default;
};

// One target-specific bytecode artifact. Defines are projected to this stage,
// so multiple full program variants can reference the same artifact.
struct ShaderStageArtifact {
    ShaderTarget Target{ShaderTarget::DXIL};
    ShaderBlobCategory Category{ShaderBlobCategory::DXIL};
    uint32_t PassIndex{0};
    ShaderStage Stage{ShaderStage::UNKNOWN};
    vector<string> Defines;
    string EntryPoint;
    vector<byte> Bytecode;
    ShaderHash BinaryHash{};
    uint32_t ReflectionIndex{kInvalidShaderTableIndex};
    uint32_t InterfaceIndex{kInvalidShaderTableIndex};

    friend bool operator==(const ShaderStageArtifact&, const ShaderStageArtifact&) = default;
};

// Maps one full baked variant to its projected stage artifacts and canonical
// program interface. Entries may be sparse across targets and variants.
struct ShaderProgramVariantArtifact {
    ShaderTarget Target{ShaderTarget::DXIL};
    uint32_t PassIndex{0};
    vector<string> Defines;
    vector<uint32_t> StageArtifactIndices;
    uint32_t InterfaceIndex{kInvalidShaderTableIndex};

    friend bool operator==(const ShaderProgramVariantArtifact&, const ShaderProgramVariantArtifact&) = default;
};

class ShaderBinary {
public:
    ShaderAssetData Asset;
    vector<ShaderReflectionRecord> Reflections;
    vector<ShaderStageInterfaceDesc> StageInterfaces;
    vector<ShaderInterfaceDesc> ProgramInterfaces;
    vector<ShaderStageArtifact> StageArtifacts;
    vector<ShaderProgramVariantArtifact> ProgramVariants;

    // Structural validity intentionally does not require every BakeSet entry
    // or target to be present. Shipping completeness is checked separately.
    bool IsValid() const noexcept;
    bool IsBakeComplete(ShaderTarget target) const noexcept;

    Nullable<const ShaderStageArtifact*> FindStageArtifact(
        ShaderTarget target,
        uint32_t passIndex,
        ShaderStage stage,
        const vector<string>& fullDefines) const noexcept;
    Nullable<const ShaderProgramVariantArtifact*> FindProgramVariant(
        ShaderTarget target,
        uint32_t passIndex,
        const vector<string>& fullDefines) const noexcept;
    Nullable<const ShaderReflectionRecord*> GetReflection(const ShaderStageArtifact& artifact) const noexcept;
    Nullable<const ShaderStageInterfaceDesc*> GetStageInterface(const ShaderStageArtifact& artifact) const noexcept;
    Nullable<const ShaderInterfaceDesc*> GetProgramInterface(
        const ShaderProgramVariantArtifact& program) const noexcept;
};

ShaderBlobCategory GetShaderBlobCategory(ShaderTarget target) noexcept;

std::optional<ShaderBinary> ReadShaderBinary(const std::filesystem::path& path) noexcept;
bool WriteShaderBinary(const std::filesystem::path& path, const ShaderBinary& binary) noexcept;

}  // namespace radray::shader
