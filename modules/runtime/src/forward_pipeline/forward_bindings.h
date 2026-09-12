#pragma once

#include <optional>

#include <radray/runtime/material.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/render_framework/cpu_draw_record.h>

namespace radray {
struct RenderPrepareContext;
class Scene;
}  // namespace radray

namespace radray::forward_detail {

inline constexpr PassPolicyId kForwardLitPolicy{1};
inline constexpr PassPolicyId kForwardLitReadOnlyDepthPolicy{2};
inline constexpr PassPolicyId kDepthOnlyPolicy{3};
inline constexpr PassPolicyId kDepthNormalsMotionPolicy{4};
inline constexpr PassPolicyId kShadowCasterPolicy{5};
inline constexpr byte kForwardViewWireIdentity{}, kForwardMaterialWireIdentity{};
bool RegisterForwardPassPolicies(RenderPrepareContext& context, const Scene& scene);

// Forward cbuffers are one shared HLSL ABI mirrored by the generated PODs, so a program only has to
// name its groups; individual fields are addressed by struct member, not by parameter lookup.
struct ForwardProgramBindings {
    uint32_t ViewBufferIndex;
    uint32_t MaterialBufferIndex;
    uint32_t ObjectBufferIndex;
    uint32_t ViewGroup;
    uint32_t MaterialGroup;
    uint32_t ObjectGroup;
    std::optional<uint32_t> PassGroup{};
    array<uint8_t, 3> GroupOrder{0, 1, 2};
};

std::optional<ForwardProgramBindings> ResolveProgramBindings(const ShaderProgram& program);
ForwardProgramBindings ReadForwardStaticBindings(const StaticBindingRecipe& recipe) noexcept;

class ForwardBindingCache {
public:
    Nullable<const ForwardProgramBindings*> Resolve(ShaderProgram* program);
    uint64_t LayoutParses() const noexcept { return _layoutParses; }

private:
    struct Entry {
        uint64_t Generation{0};
        std::optional<ForwardProgramBindings> Bindings;
    };
    unordered_map<ShaderProgram*, Entry> _programs;
    uint64_t _layoutParses{0};
};

struct DepthOnlyProgramBindings {
    uint32_t ViewBufferIndex, ObjectBufferIndex, ViewGroup, ObjectGroup;
};

std::optional<DepthOnlyProgramBindings> ResolveDepthOnlyProgramBindings(const ShaderProgram& program);
class DepthOnlyBindingCache {
public:
    Nullable<const DepthOnlyProgramBindings*> Resolve(ShaderProgram* program);
    uint64_t LayoutParses() const noexcept { return _layoutParses; }

private:
    struct Entry {
        uint64_t Generation{0};
        std::optional<DepthOnlyProgramBindings> Bindings;
    };
    unordered_map<ShaderProgram*, Entry> _programs;
    uint64_t _layoutParses{0};
};

}  // namespace radray::forward_detail
