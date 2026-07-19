#pragma once

#include <optional>
#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/shader/shader_binary.h>
#include <radray/types.h>

namespace radray {

enum class MaterialBindingKind : uint8_t {
    ConstantBuffer,
    Texture,
    Sampler,
};

struct MaterialFieldDesc {
    string Name;
    uint32_t Offset{0};
    uint32_t Size{0};

    friend bool operator==(const MaterialFieldDesc&, const MaterialFieldDesc&) = default;
};

struct MaterialBindingDesc {
    string Name;
    MaterialBindingKind Kind{MaterialBindingKind::ConstantBuffer};
    uint32_t Binding{0};
    uint32_t Count{1};
    uint32_t ByteSize{0};
    vector<MaterialFieldDesc> Fields;

    friend bool operator==(const MaterialBindingDesc&, const MaterialBindingDesc&) = default;
};

class MaterialLayout {
public:
    uint32_t Group{0};
    vector<MaterialBindingDesc> Bindings;

    bool IsValid() const noexcept;
    bool Empty() const noexcept { return Bindings.empty(); }

    Nullable<const MaterialBindingDesc*> FindBinding(std::string_view name) const noexcept;
    Nullable<const MaterialBindingDesc*> FindBinding(uint32_t binding) const noexcept;

    friend bool operator==(const MaterialLayout&, const MaterialLayout&) = default;
};

// Builds a runtime material interface from one pipeline-owned binding group.
// Missing bindings are allowed across variants; conflicting bindings are not.
std::optional<MaterialLayout> BuildMaterialLayout(
    std::span<const shader::CompiledShaderStage> stages,
    uint32_t bindingGroup) noexcept;

}  // namespace radray
