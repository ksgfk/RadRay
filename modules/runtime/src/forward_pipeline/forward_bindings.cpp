#include "forward_bindings.h"

#include <algorithm>

#include <radray/logger.h>

namespace radray::forward_detail {

std::optional<ForwardProgramBindings> ResolveProgramBindings(const ShaderProgram& program) {
    const ShaderParameterLayout& layout = program.GetParameterLayout();
    const auto& artifact = program.GetArtifact().Generic();
    if (layout.Buffers().size() != 3 || !artifact.RootConstants().empty()) return std::nullopt;
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
    for (const auto& binding : artifact.Bindings()) {
        const auto kind = static_cast<shader::ShaderBindingKind>(binding.Type);
        if (kind == shader::ShaderBindingKind::CBuffer) {
            if (binding.Count != 1) return std::nullopt;
        } else if ((kind != shader::ShaderBindingKind::Texture && kind != shader::ShaderBindingKind::Sampler) ||
                   binding.Group != bindings.MaterialGroup) {
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

std::optional<DepthOnlyProgramBindings> ResolveDepthOnlyProgramBindings(const ShaderProgram& program) {
    const auto& artifact = program.GetArtifact().Generic();
    if (!artifact.RootConstants().empty()) return std::nullopt;
    for (const auto& binding : artifact.Bindings()) {
        if (static_cast<shader::ShaderBindingKind>(binding.Type) != shader::ShaderBindingKind::CBuffer || binding.Count != 1) return std::nullopt;
    }
    const auto buffers = program.GetParameterLayout().Buffers();
    std::optional<uint32_t> view, object;
    for (uint32_t index = 0; index < buffers.size(); ++index) {
        if (!program.IsBufferDynamic(buffers[index].Name)) return std::nullopt;
        if (buffers[index].Name == "ForwardView")
            view = index;
        else if (buffers[index].Name == "ForwardObject")
            object = index;
        else
            return std::nullopt;
    }
    if (!view || !object || buffers[*view].Group == buffers[*object].Group) return std::nullopt;
    for (const auto& parameter : program.GetParameterLayout().Parameters()) {
        if (parameter.Info.Kind == ShaderParameterKind::Texture || parameter.Info.Kind == ShaderParameterKind::Sampler) return std::nullopt;
    }
    return DepthOnlyProgramBindings{*view, *object, buffers[*view].Group, buffers[*object].Group};
}

Nullable<const DepthOnlyProgramBindings*> DepthOnlyBindingCache::Resolve(ShaderProgram* program) {
    auto found = _programs.find(program);
    if (found == _programs.end()) {
        found = _programs.emplace(program, ResolveDepthOnlyProgramBindings(*program)).first;
        if (!found->second) RADRAY_ERR_LOG("DepthOnly rejected an incompatible shader program; its depth draws are disabled");
    }
    return found->second ? &*found->second : nullptr;
}

}  // namespace radray::forward_detail
