#include <radray/runtime/material_technique.h>

#include <algorithm>
#include <radray/logger.h>
#include <radray/runtime/shader_program.h>

namespace radray {
namespace {

bool ResolveMaterialGroup(MaterialPassLayout& pass, string& reason) {
    if (pass.MaterialBufferAnchor.empty()) return true;
    const auto& layout = pass.Program->GetParameterLayout();
    for (uint32_t index = 0; index < layout.Buffers().size(); ++index) {
        if (layout.Buffers()[index].Name == pass.MaterialBufferAnchor) {
            pass.BufferIndex = index;
            pass.ParameterGroup = layout.Buffers()[index].Group;
            break;
        }
    }
    if (!pass.BufferIndex) { reason = "named material cbuffer is missing"; return false; }
    if (std::count_if(layout.Buffers().begin(), layout.Buffers().end(), [&](const auto& buffer) {
            return buffer.Group == *pass.ParameterGroup;
        }) != 1) { reason = "a material group supports exactly one cbuffer"; return false; }
    for (const auto& binding : pass.Program->GetArtifact().Generic().Bindings()) {
        if (binding.Group != *pass.ParameterGroup) continue;
        const auto kind = static_cast<shader::ShaderBindingKind>(binding.Type);
        if ((kind == shader::ShaderBindingKind::CBuffer && binding.Count != 1) ||
            (kind != shader::ShaderBindingKind::CBuffer && kind != shader::ShaderBindingKind::Texture && kind != shader::ShaderBindingKind::Sampler)) {
            reason = "material groups support one non-array cbuffer and texture/sampler resources only";
            return false;
        }
    }
    for (const auto& parameter : layout.Parameters()) {
        if (parameter.Info.Group == *pass.ParameterGroup &&
            (parameter.Info.Kind == ShaderParameterKind::Texture || parameter.Info.Kind == ShaderParameterKind::Sampler)) {
            pass.Resources.push_back(parameter);
        }
    }
    return true;
}

}  // namespace

Nullable<unique_ptr<MaterialTechnique>> MaterialTechnique::Create(vector<MaterialPassDesc> passes, std::string_view primaryPass) {
    if (passes.empty() || passes.size() > std::numeric_limits<uint32_t>::max()) return nullptr;
    vector<MaterialPassLayout> layouts;
    std::optional<uint32_t> primaryIndex;
    for (auto& desc : passes) {
        if (!desc.Program || desc.Name.empty() || std::any_of(layouts.begin(), layouts.end(), [&](const auto& pass) { return pass.Name == desc.Name; })) {
            RADRAY_ERR_LOG("material technique rejected duplicate/empty pass '{}' or missing program", desc.Name);
            return nullptr;
        }
        if (desc.Name == primaryPass) primaryIndex = static_cast<uint32_t>(layouts.size());
        MaterialPassLayout pass{std::move(desc.Name), desc.Program.Get(), std::move(desc.MaterialBufferAnchor), {}, {}, {}, desc.DefaultPipelineState};
        string reason;
        if (!ResolveMaterialGroup(pass, reason)) {
            RADRAY_ERR_LOG("unsupported material ABI in pass '{}' at '{}': {}", pass.Name, pass.MaterialBufferAnchor, reason);
            return nullptr;
        }
        layouts.push_back(std::move(pass));
    }
    if (!primaryIndex || !layouts[*primaryIndex].BufferIndex) {
        RADRAY_ERR_LOG("unsupported material ABI: primary pass '{}' must define one material cbuffer", primaryPass);
        return nullptr;
    }
    const auto& primary = layouts[*primaryIndex];
    for (const auto& pass : layouts) {
        if (!pass.BufferIndex) continue;
        for (const auto& resource : pass.Resources) {
            const auto found = std::find_if(primary.Resources.begin(), primary.Resources.end(), [&](const auto& value) { return value.Name == resource.Name; });
            if (found == primary.Resources.end() || found->Info.Kind != resource.Info.Kind || found->Info.ElementCount != resource.Info.ElementCount) {
                RADRAY_ERR_LOG("unsupported material ABI: pass '{}' has incompatible resource '{}'", pass.Name, resource.Name);
                return nullptr;
            }
        }
    }
    return unique_ptr<MaterialTechnique>{new MaterialTechnique(std::move(layouts), *primaryIndex)};
}

Nullable<const MaterialPassLayout*> MaterialTechnique::FindPass(std::string_view name) const noexcept {
    for (const auto& pass : _passes)
        if (pass.Name == name) return &pass;
    return nullptr;
}

}  // namespace radray
