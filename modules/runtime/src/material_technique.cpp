#include <radray/runtime/material_technique.h>

#include <algorithm>
#include <radray/logger.h>
#include <radray/runtime/shader_program.h>

namespace radray {
namespace {

struct NumericField {
    string Path;
    ShaderParameterKind Kind;
    uint32_t Offset, Size, Stride, Count;
    friend bool operator==(const NumericField&, const NumericField&) = default;
};

vector<NumericField> NumericSchema(const MaterialPassLayout& pass) {
    vector<NumericField> result;
    for (const auto& field : pass.Program->GetParameterLayout().Parameters()) {
        if (field.Info.BufferIndex != *pass.BufferIndex) continue;
        result.push_back({field.Name.substr(pass.MaterialBufferAnchor.size() + 1), field.Info.Kind,
                          field.Info.ByteOffset, field.Info.Size, field.Info.Stride, field.Info.ElementCount});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.Path < b.Path; });
    return result;
}

bool ResolveMaterialGroup(MaterialPassLayout& pass) {
    if (pass.MaterialBufferAnchor.empty()) return true;
    const auto& layout = pass.Program->GetParameterLayout();
    for (uint32_t index = 0; index < layout.Buffers().size(); ++index) {
        if (layout.Buffers()[index].Name == pass.MaterialBufferAnchor) {
            pass.BufferIndex = index;
            pass.ParameterGroup = layout.Buffers()[index].Group;
            break;
        }
    }
    if (!pass.BufferIndex) return false;
    if (std::count_if(layout.Buffers().begin(), layout.Buffers().end(), [&](const auto& buffer) {
            return buffer.Group == *pass.ParameterGroup;
        }) != 1) return false;
    for (const auto& binding : pass.Program->GetArtifact().Generic().Bindings()) {
        if (binding.Group != *pass.ParameterGroup) continue;
        const auto kind = static_cast<shader::ShaderBindingKind>(binding.Type);
        if ((kind == shader::ShaderBindingKind::CBuffer && binding.Count != 1) ||
            (kind != shader::ShaderBindingKind::CBuffer && kind != shader::ShaderBindingKind::Texture && kind != shader::ShaderBindingKind::Sampler)) return false;
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
        if (!ResolveMaterialGroup(pass)) {
            RADRAY_ERR_LOG("material pass '{}' has invalid material buffer/group '{}'", pass.Name, pass.MaterialBufferAnchor);
            return nullptr;
        }
        layouts.push_back(std::move(pass));
    }
    if (!primaryIndex || !layouts[*primaryIndex].BufferIndex) {
        RADRAY_ERR_LOG("material technique primary pass '{}' must define one material cbuffer", primaryPass);
        return nullptr;
    }
    const auto& primary = layouts[*primaryIndex];
    const auto schema = NumericSchema(primary);
    const auto size = primary.Program->GetParameterLayout().Buffers()[*primary.BufferIndex].Size;
    for (const auto& pass : layouts) {
        if (!pass.BufferIndex) continue;
        const auto secondary = NumericSchema(pass);
        if (pass.Program->GetParameterLayout().Buffers()[*pass.BufferIndex].Size != size || secondary != schema) {
            const auto mismatch = std::mismatch(schema.begin(), schema.end(), secondary.begin(), secondary.end());
            RADRAY_ERR_LOG("material pass '{}' has incompatible numeric layout at '{}'", pass.Name,
                           mismatch.first != schema.end() ? mismatch.first->Path : string{"buffer size or extra field"});
            return nullptr;
        }
        for (const auto& resource : pass.Resources) {
            const auto found = std::find_if(primary.Resources.begin(), primary.Resources.end(), [&](const auto& value) { return value.Name == resource.Name; });
            if (found == primary.Resources.end() || found->Info.Kind != resource.Info.Kind || found->Info.ElementCount != resource.Info.ElementCount) {
                RADRAY_ERR_LOG("material pass '{}' has incompatible resource '{}'", pass.Name, resource.Name);
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
