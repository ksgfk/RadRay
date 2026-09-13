#pragma once

#include <radray/enum_flags.h>
#include <radray/nullable.h>
#include <radray/render/rhi.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/types.h>

namespace radray {
class ShaderProgram;

enum class MeshParameterScope : uint8_t { Scene,
                                          Material,
                                          Primitive,
                                          View,
                                          Pass };
enum class MeshParameterKind : uint8_t { CBuffer,
                                         Texture,
                                         Sampler,
                                         GraphTexture,
                                         GraphBuffer };
enum class MeshPassCacheMode : uint8_t { OnChange,
                                         PerEpoch,
                                         PerView };
enum class MeshPassDependency : uint8_t { MaterialValues = 1,
                                          MaterialBindings = 2,
                                          PrimitiveValues = 4 };
template <>
struct is_flags<MeshPassDependency> : std::true_type {};
using MeshPassDependencies = EnumFlags<MeshPassDependency>;
inline auto format_as(MeshPassDependency value) { return EnumFlagsName(value); }

/// Slot identities and wire layouts belong to the registering contract, not to a renderer type.
struct MeshParameterSlot {
    uint32_t Id{0};
    MeshParameterScope Scope{MeshParameterScope::Material};
    MeshParameterKind Kind{MeshParameterKind::CBuffer};
    uint64_t WireLayout{0};
    uint32_t Size{0};
    friend bool operator==(const MeshParameterSlot&, const MeshParameterSlot&) = default;
};

/// A resolved shader destination. Several source slots may supply the same group.
struct MeshParameterBinding {
    uint32_t Slot{0};
    uint32_t Group{0};
    uint32_t Binding{0};
    uint32_t Element{0};
    uint32_t BufferIndex{UINT32_MAX};
    uint32_t SourceElement{0};
    friend bool operator==(const MeshParameterBinding&, const MeshParameterBinding&) = default;
};

/// Program-lifetime destinations resolved at the cold boundary. Native resources are never retained here.
struct MeshResolvedParameterBinding {
    string Declaration;
    render::BindingHandle Handle{};
    shader::ShaderBindingKind Kind{shader::ShaderBindingKind::CBuffer};
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
    uint32_t Mapping{0}, BufferSize{0};
    bool Dynamic{false};
    friend bool operator==(const MeshResolvedParameterBinding&, const MeshResolvedParameterBinding&) = default;
};

/// Scene-published immutable mapping. Contains no frame resource, native set or upload address.
struct MeshBindingPlan {
    vector<MeshParameterSlot> Slots;
    vector<MeshParameterBinding> Bindings;
    vector<uint32_t> Groups;
    vector<uint32_t> GroupBegin;
    vector<MeshResolvedParameterBinding> Resolved;
    uint64_t ProgramGeneration{0};
    bool Valid{false};
    /// Cold boundary: validate indices and destinations, then build ascending unique groups.
    bool Finalize();
    /// Also validate complete groups against the immutable program and resolve their CPU binding handles.
    /// Failed native set creation and resource readiness are deliberately outside this contract.
    bool Finalize(const ShaderProgram& program);
    friend bool operator==(const MeshBindingPlan&, const MeshBindingPlan&) = default;
};

/// Pure program-layout compiler. It must not read material values, geometry or frame state.
struct MeshBindingContract {
    uint64_t Id{0}, Revision{0}, Configuration{0};
    bool (*Resolve)(const ShaderProgram&, MeshBindingPlan&){nullptr};
    bool (*ResolveConfigured)(const ShaderProgram&, uint64_t configuration, MeshBindingPlan&){nullptr};
    bool IsValid() const noexcept { return Id != 0 && Revision != 0 && (bool(Resolve) != bool(ResolveConfigured)); }
    bool Compile(const ShaderProgram& program, MeshBindingPlan& plan) const {
        return ResolveConfigured ? ResolveConfigured(program, Configuration, plan) : Resolve(program, plan);
    }
    friend bool operator==(const MeshBindingContract&, const MeshBindingContract&) = default;
};
}  // namespace radray
