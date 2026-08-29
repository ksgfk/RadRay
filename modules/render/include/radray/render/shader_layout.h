#pragma once

#include <optional>
#include <span>

#include <radray/render/rhi.h>
#include <radray/shader/shader_artifact.h>

namespace radray::render {

// 128-bit digest of a resolved, target-typed layout. Program identity uses the digest of the
// current backend's layout only, so the other backend's recipe fields never perturb it.
using ResolvedLayoutHash = shader::Hash128;

// Full immutable sampler state in Vulkan terms. This is the same fixed-width record the compiler
// publishes, reused verbatim so a replacement option and a compiler-published state are the same
// shape and hash identically.
using VulkanImmutableSamplerState = shader::WireSamplerRecord;

// A layout modifier addresses one canonical declaration and states the logical kind it expects to
// find there. The kind is part of the selector, not a hint: if the artifact disagrees, the recipe
// was written against a different shader and resolution fails instead of silently retargeting.
struct ShaderLayoutSelector {
    string DeclarationName;
    shader::ShaderBindingKind ExpectedLogicalResourceKind{shader::ShaderBindingKind::CBuffer};

    friend bool operator==(const ShaderLayoutSelector&, const ShaderLayoutSelector&) noexcept = default;
    friend auto operator<=>(const ShaderLayoutSelector&, const ShaderLayoutSelector&) noexcept = default;
};

// Where a D3D12 descriptor lands. A root descriptor is bound by GPU virtual address and owns no
// descriptor table slot.
enum class D3D12BufferPlacement : uint32_t {
    Table = 0,
    RootDescriptor = 1,
};

// Vulkan buffer descriptors are either bound at their own offset or take a dynamic offset at bind
// time. This never changes the descriptor's array count.
enum class VulkanBufferDescriptorPlacement : uint32_t {
    Regular = 0,
    Dynamic = 1,
};

// Only meaningful when the artifact carries no serialized root signature: an explicit policy is
// the sole authority for its own topology, so modifying it is a contradiction rather than an
// override.
struct D3D12BufferPlacementModifier {
    ShaderLayoutSelector Selector;
    D3D12BufferPlacement Placement{D3D12BufferPlacement::RootDescriptor};
};

struct D3D12TargetLayoutOptions {
    vector<D3D12BufferPlacementModifier> BufferPlacements;

    bool Empty() const noexcept { return BufferPlacements.empty(); }
};

struct VulkanBufferDescriptorModifier {
    ShaderLayoutSelector Selector;
    VulkanBufferDescriptorPlacement Placement{VulkanBufferDescriptorPlacement::Dynamic};
};

// Replaces the selected sampler's immutable state wholesale. A partial merge would make the
// resolved state depend on what the policy happened to publish.
struct VulkanImmutableSamplerModifier {
    ShaderLayoutSelector Selector;
    VulkanImmutableSamplerState State{};
};

struct VulkanTargetLayoutOptions {
    vector<VulkanBufferDescriptorModifier> BufferDescriptors;
    vector<VulkanImmutableSamplerModifier> ImmutableSamplers;

    bool Empty() const noexcept {
        return BufferDescriptors.empty() && ImmutableSamplers.empty();
    }
};

// Both backends' options sit side by side so one pipeline recipe states the difference explicitly.
// Each backend reads and hashes only its own fields.
struct ShaderProgramLayoutRecipe {
    D3D12TargetLayoutOptions D3D12;
    VulkanTargetLayoutOptions Vulkan;
};

enum class ShaderLayoutResolveError : uint32_t {
    None = 0,
    // The artifact was decoded for the other target.
    TargetMismatch,
    // A record the decoder let through cannot be expressed as a resolved destination.
    UnsupportedBinding,
    // Two modifiers name the same selector: which one wins is not a policy this framework has.
    DuplicateSelector,
    // No active declaration matches the selector, or its logical kind differs.
    SelectorNotFound,
    // The selected declaration cannot take the requested placement.
    IllegalPlacement,
    // A D3D12 modifier was given for an artifact that carries an explicit root signature.
    ExplicitPolicyNotModifiable,
    // More sets, bindings, or push blocks than the layout shape allows.
    LimitExceeded,
};

enum class ShaderLayoutRecordKind : uint32_t {
    Descriptor = 0,
    Push = 1,
};

// Layout-local metadata: what a caller-visible name resolves to inside one resolved layout. The
// index refers to that layout's own arrays, so a record from one layout is meaningless in another.
struct ShaderLayoutMetadataRecord {
    string Name;
    ShaderLayoutRecordKind Kind{ShaderLayoutRecordKind::Descriptor};
    uint32_t ResolvedIndex{0};
};

struct ResolvedD3D12Binding {
    string Name;
    shader::ShaderBindingKind LogicalKind{shader::ShaderBindingKind::CBuffer};
    uint32_t Group{0};
    uint32_t Binding{0};
    uint32_t Count{1};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    shader::ShaderBindingPlacement Placement{shader::ShaderBindingPlacement::Table};

    friend bool operator==(const ResolvedD3D12Binding&, const ResolvedD3D12Binding&) noexcept = default;
};

struct ResolvedPushConstantBlock {
    string Name;
    uint32_t RegisterSpace{0};
    uint32_t Register{0};
    uint32_t Offset{0};
    uint32_t Size{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};

    friend bool operator==(const ResolvedPushConstantBlock&, const ResolvedPushConstantBlock&) noexcept = default;
};

// Owning value. Comparison and hashing depend only on the semantics below, never on artifact
// addresses, borrowed spans, or native handles, so it outlives the artifact it came from.
struct ResolvedD3D12Layout {
    // Present exactly when the source carried an explicit policy. Then it is the sole authority
    // for the native root signature and Bindings only describes where each declaration landed.
    vector<byte> SerializedRootSignature;
    vector<ResolvedD3D12Binding> Bindings;
    vector<ResolvedPushConstantBlock> PushConstants;
    vector<ShaderLayoutMetadataRecord> Metadata;
    ResolvedLayoutHash Hash{};

    bool HasExplicitCarrier() const noexcept { return !SerializedRootSignature.empty(); }
    Nullable<const ShaderLayoutMetadataRecord*> FindRecord(std::string_view name) const noexcept;
};

struct ResolvedVulkanBinding {
    string Name;
    shader::ShaderBindingKind LogicalKind{shader::ShaderBindingKind::CBuffer};
    uint32_t Set{0};
    uint32_t Binding{0};
    uint32_t Count{1};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    VulkanBufferDescriptorPlacement Placement{VulkanBufferDescriptorPlacement::Regular};
    // Index into ImmutableSamplers, or shader::kShaderNoSampler.
    uint32_t ImmutableSamplerIndex{shader::kShaderNoSampler};

    friend bool operator==(const ResolvedVulkanBinding&, const ResolvedVulkanBinding&) noexcept = default;
};

struct ResolvedVulkanLayout {
    // Sorted by (set, binding).
    vector<ResolvedVulkanBinding> Bindings;
    // Number of set layouts the pipeline layout needs, counting unused sets below the highest
    // active one: Vulkan has no sparse set indices, so a hole becomes an empty set layout.
    uint32_t SetCount{0};
    vector<VulkanImmutableSamplerState> ImmutableSamplers;
    // At most one active push block per variant.
    std::optional<ResolvedPushConstantBlock> PushBlock;
    // Indices into Bindings in the order vkCmdBindDescriptorSets expects its dynamic offsets.
    vector<uint32_t> DynamicOffsetOrder;
    vector<ShaderLayoutMetadataRecord> Metadata;
    ResolvedLayoutHash Hash{};

    Nullable<const ShaderLayoutMetadataRecord*> FindRecord(std::string_view name) const noexcept;
};

// Resolution is fail-closed: a recipe that contradicts the artifact is a framework invariant
// violation, reported through `error` (and asserted in Debug) rather than repaired.
std::optional<ResolvedD3D12Layout> ResolveD3D12Layout(
    const shader::DxilShaderArtifactView& artifact,
    const D3D12TargetLayoutOptions& options = {},
    ShaderLayoutResolveError* error = nullptr) noexcept;

std::optional<ResolvedVulkanLayout> ResolveVulkanLayout(
    const shader::SpirvShaderArtifactView& artifact,
    const VulkanTargetLayoutOptions& options = {},
    ShaderLayoutResolveError* error = nullptr) noexcept;

}  // namespace radray::render
