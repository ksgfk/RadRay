#pragma once

#include <optional>

#include <radray/runtime/material.h>
#include <radray/runtime/shader_program.h>

namespace radray::forward_detail {

struct ForwardProgramBindings {
    uint32_t ViewBufferIndex;
    uint32_t MaterialBufferIndex;
    uint32_t ObjectBufferIndex;
    uint32_t ViewGroup;
    uint32_t MaterialGroup;
    uint32_t ObjectGroup;
};

std::optional<ForwardProgramBindings> ResolveProgramBindings(const ShaderProgram& program);

class ForwardBindingCache {
public:
    Nullable<const ForwardProgramBindings*> Resolve(ShaderProgram* program);

private:
    unordered_map<ShaderProgram*, std::optional<ForwardProgramBindings>> _programs;
};

struct ForwardBufferBinding {
    uint32_t BufferIndex{0};
    render::ShaderBufferBinding Value;

    friend bool operator==(const ForwardBufferBinding&, const ForwardBufferBinding&) = default;
};

// Owned by one Forward flight. A cached set is never written after publication.
class ForwardMaterialSets {
public:
    Nullable<render::ShaderParameterSet*> GetOrCreate(
        uint32_t materialIndex,
        const MaterialRenderData& material,
        std::span<const ForwardBufferBinding> bindings);
    void Clear() noexcept { _sets.clear(); }

private:
    struct Entry {
        uint32_t MaterialIndex;
        ShaderProgram* Program;
        vector<ForwardBufferBinding> Bindings;
        unique_ptr<render::ShaderParameterSet> Set;
    };
    vector<Entry> _sets;
};

}  // namespace radray::forward_detail
