#include "forward_bindings.h"

#include <algorithm>

#include <radray/logger.h>

namespace radray::forward_detail {

std::optional<ForwardProgramBindings> ResolveProgramBindings(const ShaderProgram& program) {
    const ShaderParameterLayout& layout = program.GetParameterLayout();
    const auto find = [&](std::string_view name) -> std::optional<uint32_t> {
        for (uint32_t index = 0; index < layout.Buffers().size(); ++index) {
            if (layout.Buffers()[index].Name == name && program.IsBufferDynamic(name)) {
                return index;
            }
        }
        return std::nullopt;
    };
    const auto view = find("ForwardView");
    const auto material = find("ForwardMaterial");
    const auto object = find("ForwardObject");
    if (!view || !material || !object) {
        return std::nullopt;
    }
    const ForwardProgramBindings bindings{
        *view, *material, *object,
        layout.Buffers()[*view].Group,
        layout.Buffers()[*material].Group,
        layout.Buffers()[*object].Group};
    if (bindings.ViewGroup == bindings.MaterialGroup ||
        bindings.ViewGroup == bindings.ObjectGroup ||
        bindings.MaterialGroup == bindings.ObjectGroup) {
        return std::nullopt;
    }
    for (const std::string_view name : {"AlbedoTexture", "LinearSampler"}) {
        const ShaderParameterInfo* resource = layout.Find(name);
        if (resource != nullptr && resource->Group != bindings.MaterialGroup) {
            return std::nullopt;
        }
    }
    return bindings;
}

Nullable<const ForwardProgramBindings*> ForwardBindingCache::Resolve(ShaderProgram* program) {
    auto found = _programs.find(program);
    if (found == _programs.end()) {
        found = _programs.emplace(program, ResolveProgramBindings(*program)).first;
        if (!found->second.has_value()) {
            RADRAY_ERR_LOG("forward pipeline rejected an incompatible shader program");
        }
    }
    return found->second.has_value() ? &*found->second : nullptr;
}

Nullable<render::ShaderParameterSet*> ForwardMaterialSets::GetOrCreate(
    uint32_t materialIndex,
    const MaterialRenderData& material,
    std::span<const ForwardBufferBinding> bindings) {
    if (!material.Program.HasValue()) {
        return nullptr;
    }
    ShaderProgram* program = material.Program.Get();
    const auto found = std::find_if(_sets.begin(), _sets.end(), [&](const Entry& entry) {
        return entry.MaterialIndex == materialIndex && entry.Program == program &&
               std::equal(entry.Bindings.begin(), entry.Bindings.end(), bindings.begin(), bindings.end());
    });
    if (found != _sets.end()) {
        return found->Set.get();
    }
    const auto buffers = program->GetParameterLayout().Buffers();
    const auto expectedCount = std::count_if(buffers.begin(), buffers.end(), [&](const auto& buffer) {
        return buffer.Group == material.ParameterGroup;
    });
    if (bindings.size() != static_cast<size_t>(expectedCount)) {
        return nullptr;
    }
    auto created = program->GetDevice()->CreateShaderParameterSet(render::ShaderParameterSetDescriptor{
        .Layout = program->GetPipelineLayout(),
        .GroupIndex = material.ParameterGroup});
    if (!created.HasValue()) {
        return nullptr;
    }
    auto set = created.Release();
    for (const ForwardBufferBinding& binding : bindings) {
        if (binding.BufferIndex >= buffers.size() ||
            buffers[binding.BufferIndex].Group != material.ParameterGroup ||
            !set->Set(buffers[binding.BufferIndex].Binding, 0, binding.Value)) {
            return nullptr;
        }
    }
    for (const MaterialTextureFrameData& value : material.Textures) {
        const Nullable<render::TextureView*> view = value.Texture->GetOrCreateSrv(value.SubView);
        if (!view.HasValue() || !set->Set(value.Parameter.Binding, value.Element, view.Get())) {
            return nullptr;
        }
    }
    for (const MaterialSamplerFrameData& value : material.Samplers) {
        const auto sampler = program->GetDevice()->GetOrCreateSampler(value.Sampler);
        if (!sampler.HasValue() || !set->Set(value.Parameter.Binding, value.Element, sampler.Get())) {
            return nullptr;
        }
    }
    if (!set->FlushWrites()) {
        return nullptr;
    }
    _sets.push_back(Entry{materialIndex, program, {bindings.begin(), bindings.end()}, std::move(set)});
    return _sets.back().Set.get();
}

}  // namespace radray::forward_detail
