#pragma once

#include <optional>
#include <span>

#include <radray/runtime/shader_layout.h>
#include <radray/shader/shader_artifact.h>

namespace radray::shader_reflector {

struct ShaderVariantArtifacts {
    string Name;
    Nullable<const shader::DxilShaderArtifactView*> DXIL{nullptr};
    Nullable<const shader::SpirvShaderArtifactView*> SPIRV{nullptr};
};

struct ShaderBindingOverride {
    string Name;
    std::optional<uint32_t> GroupSlot;
    std::optional<uint32_t> BindingId;
    std::optional<string> GroupName;
};

struct ShaderPushConstantOverride {
    string Name;
    uint32_t PushId{0};
};

struct ShaderReflectionPolicy {
    bool AllowTargetOnlyBindings{false};
    vector<ShaderBindingOverride> Bindings;
    vector<ShaderPushConstantOverride> PushConstants;
};

// Artifacts are borrowed for the call. The result owns all strings and type records.
// Implicit IDs are deterministic for a family; use overrides to preserve IDs across revisions.
shader::DiagnosticResult<ShaderFamilyLayoutDesc> ReflectShaderFamily(std::span<const ShaderVariantArtifacts> variants, const ShaderReflectionPolicy& policy = {});
shader::DiagnosticResult<ShaderFamilyLayoutDesc> ReflectShaderVariant(const ShaderVariantArtifacts& variant, const ShaderReflectionPolicy& policy = {});

}  // namespace radray::shader_reflector
