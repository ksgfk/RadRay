#include <radray/runtime/render_framework/mesh_binding_contract.h>

#include <algorithm>
#include <radray/runtime/shader_program.h>

namespace radray {
bool MeshBindingPlan::Finalize() {
    Valid = false;
    Groups.clear();
    GroupBegin.clear();
    Resolved.clear();
    ProgramGeneration = 0;
    for (size_t index = 0; index < Slots.size(); ++index) {
        const auto& slot = Slots[index];
        if (slot.Scope > MeshParameterScope::Pass || slot.Kind > MeshParameterKind::GraphBuffer ||
            (slot.Kind == MeshParameterKind::CBuffer && (slot.Size == 0 || slot.WireLayout == 0))) return false;
        for (size_t previous = 0; previous < index; ++previous)
            if (Slots[previous].Id == slot.Id) return false;
    }
    for (size_t index = 0; index < Bindings.size(); ++index) {
        const auto& binding = Bindings[index];
        if (binding.Slot >= Slots.size() || binding.Group == UINT32_MAX || binding.Binding == UINT32_MAX) return false;
        if (Slots[binding.Slot].Kind == MeshParameterKind::CBuffer && binding.BufferIndex == UINT32_MAX) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            const auto& other = Bindings[previous];
            const auto destinationClass = [&](uint32_t slot) {
                const auto kind = Slots[slot].Kind;
                return kind == MeshParameterKind::GraphTexture ? MeshParameterKind::Texture : kind;
            };
            if (other.Group == binding.Group && other.Binding == binding.Binding && other.Element == binding.Element &&
                destinationClass(other.Slot) == destinationClass(binding.Slot)) return false;
        }
        Groups.push_back(binding.Group);
    }
    std::sort(Groups.begin(), Groups.end());
    Groups.erase(std::unique(Groups.begin(), Groups.end()), Groups.end());
    Valid = true;
    return true;
}

bool MeshBindingPlan::Finalize(const ShaderProgram& program) {
    if (!Finalize()) return false;
    Valid = false;
    const auto& artifact = program.GetArtifact().Generic();
    for (const auto& binding : artifact.Bindings()) {
        const auto name = artifact.GetName(binding.Name);
        const auto info = name ? program.GetArtifact().FindBindingInfo(*name) : std::nullopt;
        if (!info || (!info->Immutable && !std::binary_search(Groups.begin(), Groups.end(), info->Group))) return false;
    }
    const auto matches = [](MeshParameterKind slot, shader::ShaderBindingKind kind) {
        switch (slot) {
            case MeshParameterKind::CBuffer: return kind == shader::ShaderBindingKind::CBuffer;
            case MeshParameterKind::Texture:
            case MeshParameterKind::GraphTexture: return shader::IsImageKind(kind);
            case MeshParameterKind::Sampler: return kind == shader::ShaderBindingKind::Sampler;
            case MeshParameterKind::GraphBuffer:
                return kind == shader::ShaderBindingKind::CBuffer ||
                       kind == shader::ShaderBindingKind::TypedBuffer || kind == shader::ShaderBindingKind::RWTypedBuffer ||
                       kind == shader::ShaderBindingKind::StructuredBuffer || kind == shader::ShaderBindingKind::RWStructuredBuffer ||
                       kind == shader::ShaderBindingKind::RawBuffer || kind == shader::ShaderBindingKind::RWRawBuffer;
        }
        return false;
    };
    for (const uint32_t group : Groups) {
        GroupBegin.push_back(static_cast<uint32_t>(Resolved.size()));
        for (uint32_t index = 0; index < Bindings.size(); ++index) {
            const auto& mapping = Bindings[index];
            if (mapping.Group != group) continue;
            const auto& slot = Slots[mapping.Slot];
            const shader::WireBindingRecord* selected = nullptr;
            for (const auto& binding : artifact.Bindings()) {
                if (binding.Group != group || binding.Binding != mapping.Binding ||
                    !matches(slot.Kind, static_cast<shader::ShaderBindingKind>(binding.Type))) continue;
                if (selected) return false;
                selected = &binding;
            }
            if (!selected) return false;
            const auto name = artifact.GetName(selected->Name);
            if (!name) return false;
            const auto info = program.GetArtifact().FindBindingInfo(*name);
            if (!info || info->Immutable || info->Group != group || mapping.Element >= info->Count) return false;
            MeshResolvedParameterBinding resolved;
            resolved.Declaration = *name;
            resolved.Handle = program.GetPipelineLayout()->FindBinding(*name);
            resolved.Kind = info->LogicalKind;
            resolved.Stages = info->Stages;
            resolved.Dynamic = info->Dynamic;
            resolved.Mapping = index;
            if (info->LogicalKind == shader::ShaderBindingKind::CBuffer) {
                const auto buffers = program.GetParameterLayout().Buffers();
                const auto found = std::find_if(buffers.begin(), buffers.end(), [&](const auto& buffer) { return buffer.Name == *name; });
                if (found == buffers.end()) return false;
                resolved.BufferSize = found->Size;
                if (slot.Kind == MeshParameterKind::CBuffer &&
                    (mapping.BufferIndex != static_cast<uint32_t>(found - buffers.begin()) ||
                     mapping.Element != 0 || mapping.SourceElement != 0 || slot.Size != found->Size)) return false;
            }
            Resolved.push_back(std::move(resolved));
        }
        // Completeness is a property of this immutable program/contract pair, not of every draw.
        for (const auto& binding : artifact.Bindings()) {
            const auto name = artifact.GetName(binding.Name);
            if (!name) continue;
            const auto info = program.GetArtifact().FindBindingInfo(*name);
            if (!info || info->Group != group || info->Immutable) continue;
            for (uint32_t element = 0; element < info->Count; ++element) {
                uint32_t count = 0;
                for (uint32_t index = GroupBegin.back(); index < Resolved.size(); ++index)
                    count += Resolved[index].Declaration == *name && Bindings[Resolved[index].Mapping].Element == element;
                if (count != 1) return false;
            }
        }
    }
    GroupBegin.push_back(static_cast<uint32_t>(Resolved.size()));
    ProgramGeneration = program.GetGeneration();
    Valid = true;
    return true;
}
}  // namespace radray
