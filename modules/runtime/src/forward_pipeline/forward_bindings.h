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
    std::optional<uint32_t> PassGroup{};
};

std::optional<ForwardProgramBindings> ResolveProgramBindings(const ShaderProgram& program);

class ForwardBindingCache {
public:
    Nullable<const ForwardProgramBindings*> Resolve(ShaderProgram* program);

private:
    unordered_map<ShaderProgram*, std::optional<ForwardProgramBindings>> _programs;
};

struct DepthOnlyProgramBindings {
    uint32_t ViewBufferIndex, ObjectBufferIndex, ViewGroup, ObjectGroup;
};

std::optional<DepthOnlyProgramBindings> ResolveDepthOnlyProgramBindings(const ShaderProgram& program);
class DepthOnlyBindingCache {
public:
    Nullable<const DepthOnlyProgramBindings*> Resolve(ShaderProgram* program);

private:
    unordered_map<ShaderProgram*, std::optional<DepthOnlyProgramBindings>> _programs;
};

}  // namespace radray::forward_detail
