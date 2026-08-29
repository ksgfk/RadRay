#include <radray/render/shader_layout.h>

#include <algorithm>
#include <cstring>
#include <tuple>

#include <fmt/format.h>

#include <radray/hash.h>
#include <radray/logger.h>

namespace radray::render {
namespace {

// Bumped whenever the canonical encoding below changes shape, so a stale cached hash can never
// collide with a differently-encoded layout that happens to hash the same.
constexpr uint32_t kResolvedLayoutEncodingVersion = 1;

constexpr uint32_t kMaxSetIndex = 64;

void SetError(ShaderLayoutResolveError* error, ShaderLayoutResolveError value) noexcept {
    if (error != nullptr) {
        *error = value;
    }
}

// A recipe that contradicts the artifact is a framework invariant violation: it means the recipe was
// written against a different shader. It is reported and fails the resolve rather than aborting,
// because the same path also validates decoded wire data, and because fail-closed behaviour has to
// stay observable in the configuration the tests actually run.
ShaderLayoutResolveError Reject(
    ShaderLayoutResolveError* error,
    ShaderLayoutResolveError value,
    std::string_view detail) noexcept {
    RADRAY_ERR_LOG("shader layout resolve failed: {}", detail);
    SetError(error, value);
    return value;
}

void AppendU32(vector<byte>& bytes, uint32_t value) noexcept {
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<byte>((value >> shift) & 0xffu));
    }
}

void AppendName(vector<byte>& bytes, std::string_view name) noexcept {
    AppendU32(bytes, static_cast<uint32_t>(name.size()));
    for (const char value : name) {
        bytes.push_back(static_cast<byte>(value));
    }
}

void AppendSampler(vector<byte>& bytes, const VulkanImmutableSamplerState& sampler) noexcept {
    // Float fields go in by their bit pattern: the state is compared for equality, so a bitwise
    // encoding is exactly the right granularity.
    static_assert(sizeof(VulkanImmutableSamplerState) == 64);
    array<uint8_t, sizeof(VulkanImmutableSamplerState)> raw{};
    std::memcpy(raw.data(), &sampler, raw.size());
    for (const uint8_t value : raw) {
        bytes.push_back(static_cast<byte>(value));
    }
}

ResolvedLayoutHash Digest(const vector<byte>& bytes) noexcept {
    return ResolvedLayoutHash{HashData128(bytes.data(), bytes.size())};
}

ShaderStages ToStages(uint32_t wireStageMask) noexcept {
    // Wire stage bits and ShaderStage share their bit positions by contract.
    return ShaderStages{static_cast<ShaderStage>(wireStageMask)};
}

// Canonicalizes a modifier list by selector so the caller's input order cannot influence the
// resolved value or its hash. One declaration takes at most one decision, so a repeated
// declaration name is rejected instead of resolved as "last wins".
template <typename Modifier>
bool CanonicalizeModifiers(
    const vector<Modifier>& source,
    vector<const Modifier*>& canonical,
    ShaderLayoutResolveError* error) noexcept {
    canonical.clear();
    canonical.reserve(source.size());
    for (const Modifier& modifier : source) {
        canonical.push_back(&modifier);
    }
    std::sort(
        canonical.begin(),
        canonical.end(),
        [](const Modifier* lhs, const Modifier* rhs) noexcept {
            return lhs->Selector < rhs->Selector;
        });
    for (size_t index = 1; index < canonical.size(); ++index) {
        if (canonical[index - 1]->Selector.DeclarationName ==
            canonical[index]->Selector.DeclarationName) {
            Reject(
                error,
                ShaderLayoutResolveError::DuplicateSelector,
                fmt::format(
                    "declaration '{}' has more than one modifier",
                    canonical[index]->Selector.DeclarationName));
            return false;
        }
    }
    return true;
}

// Resolves a selector against the artifact's active declarations. Both the name and the logical
// kind must match: a kind mismatch means the recipe describes a different shader.
template <typename Binding>
Nullable<Binding*> FindSelected(
    vector<Binding>& bindings,
    const ShaderLayoutSelector& selector,
    ShaderLayoutResolveError* error) noexcept {
    const auto found = std::find_if(
        bindings.begin(),
        bindings.end(),
        [&](const Binding& binding) noexcept {
            return binding.Name == selector.DeclarationName;
        });
    if (found == bindings.end()) {
        Reject(
            error,
            ShaderLayoutResolveError::SelectorNotFound,
            fmt::format("no active declaration named '{}'", selector.DeclarationName));
        return nullptr;
    }
    if (found->LogicalKind != selector.ExpectedLogicalResourceKind) {
        Reject(
            error,
            ShaderLayoutResolveError::SelectorNotFound,
            fmt::format(
                "declaration '{}' is a {}, not the expected {}",
                selector.DeclarationName,
                static_cast<uint32_t>(found->LogicalKind),
                static_cast<uint32_t>(selector.ExpectedLogicalResourceKind)));
        return nullptr;
    }
    return &*found;
}

template <typename Binding>
void BuildMetadata(
    const vector<Binding>& bindings,
    const vector<ResolvedPushConstantBlock>& pushBlocks,
    vector<ShaderLayoutMetadataRecord>& metadata) noexcept {
    metadata.clear();
    metadata.reserve(bindings.size() + pushBlocks.size());
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        metadata.push_back(
            {.Name = bindings[index].Name,
             .Kind = ShaderLayoutRecordKind::Descriptor,
             .ResolvedIndex = index});
    }
    for (uint32_t index = 0; index < pushBlocks.size(); ++index) {
        metadata.push_back(
            {.Name = pushBlocks[index].Name,
             .Kind = ShaderLayoutRecordKind::Push,
             .ResolvedIndex = index});
    }
    std::sort(
        metadata.begin(),
        metadata.end(),
        [](const ShaderLayoutMetadataRecord& lhs, const ShaderLayoutMetadataRecord& rhs) noexcept {
            return lhs.Name < rhs.Name;
        });
}

Nullable<const ShaderLayoutMetadataRecord*> FindMetadataRecord(
    const vector<ShaderLayoutMetadataRecord>& metadata,
    std::string_view name) noexcept {
    const auto found = std::lower_bound(
        metadata.begin(),
        metadata.end(),
        name,
        [](const ShaderLayoutMetadataRecord& record, std::string_view value) noexcept {
            return std::string_view{record.Name} < value;
        });
    if (found == metadata.end() || std::string_view{found->Name} != name) {
        return nullptr;
    }
    return &*found;
}

bool CollectPushBlocks(
    const shader::ShaderArtifactView& artifact,
    vector<ResolvedPushConstantBlock>& pushBlocks,
    ShaderLayoutResolveError* error) noexcept {
    pushBlocks.reserve(artifact.RootConstants().size());
    for (const shader::WireRootConstantRecord& constant : artifact.RootConstants()) {
        const std::optional<std::string_view> name = artifact.GetName(constant.Name);
        if (!name.has_value()) {
            Reject(
                error,
                ShaderLayoutResolveError::UnsupportedBinding,
                "root constant record has no declaration name");
            return false;
        }
        pushBlocks.push_back(
            {.Name = string{name.value()},
             .RegisterSpace = constant.RegisterSpace,
             .Register = constant.Register,
             .Offset = constant.Offset,
             .Size = constant.Size,
             .Stages = ToStages(constant.StageMask)});
    }
    std::sort(
        pushBlocks.begin(),
        pushBlocks.end(),
        [](const ResolvedPushConstantBlock& lhs, const ResolvedPushConstantBlock& rhs) noexcept {
            return std::tie(lhs.RegisterSpace, lhs.Register) <
                   std::tie(rhs.RegisterSpace, rhs.Register);
        });
    return true;
}

void AppendPushBlock(vector<byte>& bytes, const ResolvedPushConstantBlock& block) noexcept {
    AppendName(bytes, block.Name);
    AppendU32(bytes, block.RegisterSpace);
    AppendU32(bytes, block.Register);
    AppendU32(bytes, block.Offset);
    AppendU32(bytes, block.Size);
    AppendU32(bytes, static_cast<uint32_t>(block.Stages.value()));
}

}  // namespace

Nullable<const ShaderLayoutMetadataRecord*> ResolvedD3D12Layout::FindRecord(
    std::string_view name) const noexcept {
    return FindMetadataRecord(Metadata, name);
}

Nullable<const ShaderLayoutMetadataRecord*> ResolvedVulkanLayout::FindRecord(
    std::string_view name) const noexcept {
    return FindMetadataRecord(Metadata, name);
}

std::optional<ResolvedD3D12Layout> ResolveD3D12Layout(
    const shader::DxilShaderArtifactView& artifact,
    const D3D12TargetLayoutOptions& options,
    ShaderLayoutResolveError* error) noexcept {
    SetError(error, ShaderLayoutResolveError::None);
    const shader::ShaderArtifactView& view = artifact.Generic();
    if (view.Envelope().Target != static_cast<uint8_t>(shader::ShaderTarget::DXIL)) {
        SetError(error, ShaderLayoutResolveError::TargetMismatch);
        return std::nullopt;
    }

    ResolvedD3D12Layout result;
    const std::span<const byte> carrier = view.SerializedRootSignature();
    result.SerializedRootSignature.assign(carrier.begin(), carrier.end());

    result.Bindings.reserve(view.Bindings().size());
    for (const shader::WireBindingRecord& binding : view.Bindings()) {
        const std::optional<std::string_view> name = view.GetName(binding.Name);
        if (!name.has_value()) {
            SetError(error, ShaderLayoutResolveError::UnsupportedBinding);
            return std::nullopt;
        }
        result.Bindings.push_back(
            {.Name = string{name.value()},
             .LogicalKind = static_cast<shader::ShaderBindingKind>(binding.Type),
             .Group = binding.Group,
             .Binding = binding.Binding,
             .Count = binding.Count,
             .Stages = ToStages(binding.StageMask),
             .Placement = static_cast<shader::ShaderBindingPlacement>(binding.Placement)});
    }

    vector<const D3D12BufferPlacementModifier*> modifiers;
    if (!CanonicalizeModifiers(options.BufferPlacements, modifiers, error)) {
        return std::nullopt;
    }
    // An explicit policy already fixed its own topology, and the carrier is what the runtime
    // hands to CreateRootSignature. Rewriting a destination here would make the resolved value
    // disagree with the blob it ships.
    if (!modifiers.empty() && result.HasExplicitCarrier()) {
        Reject(
            error,
            ShaderLayoutResolveError::ExplicitPolicyNotModifiable,
            "artifact carries an explicit root signature, so its placements are not modifiable");
        return std::nullopt;
    }
    for (const D3D12BufferPlacementModifier* modifier : modifiers) {
        const Nullable<ResolvedD3D12Binding*> selected =
            FindSelected(result.Bindings, modifier->Selector, error);
        if (!selected.HasValue()) {
            return std::nullopt;
        }
        ResolvedD3D12Binding& binding = *selected.Get();
        if (modifier->Placement == D3D12BufferPlacement::RootDescriptor) {
            // A root descriptor is one GPU address, and the public dynamic-offset shape carries no
            // array element, so an array declaration has nowhere to put the rest.
            if (!shader::CanBeRootDescriptor(binding.LogicalKind) || binding.Count != 1) {
                Reject(
                    error,
                    ShaderLayoutResolveError::IllegalPlacement,
                    fmt::format(
                        "declaration '{}' cannot become a root descriptor",
                        binding.Name));
                return std::nullopt;
            }
            binding.Placement = shader::ShaderBindingPlacement::RootDescriptor;
        } else {
            binding.Placement = shader::ShaderBindingPlacement::Table;
        }
    }

    std::sort(
        result.Bindings.begin(),
        result.Bindings.end(),
        [](const ResolvedD3D12Binding& lhs, const ResolvedD3D12Binding& rhs) noexcept {
            const uint32_t lhsSpace =
                shader::GetWireBindingNamespace(static_cast<uint32_t>(lhs.LogicalKind));
            const uint32_t rhsSpace =
                shader::GetWireBindingNamespace(static_cast<uint32_t>(rhs.LogicalKind));
            return std::tie(lhs.Group, lhsSpace, lhs.Binding) <
                   std::tie(rhs.Group, rhsSpace, rhs.Binding);
        });
    if (!CollectPushBlocks(view, result.PushConstants, error)) {
        return std::nullopt;
    }
    BuildMetadata(result.Bindings, result.PushConstants, result.Metadata);

    vector<byte> encoded;
    encoded.reserve(256);
    AppendU32(encoded, kResolvedLayoutEncodingVersion);
    AppendU32(encoded, static_cast<uint32_t>(shader::ShaderTarget::DXIL));
    AppendU32(encoded, static_cast<uint32_t>(result.SerializedRootSignature.size()));
    encoded.insert(
        encoded.end(),
        result.SerializedRootSignature.begin(),
        result.SerializedRootSignature.end());
    AppendU32(encoded, static_cast<uint32_t>(result.Bindings.size()));
    for (const ResolvedD3D12Binding& binding : result.Bindings) {
        AppendName(encoded, binding.Name);
        AppendU32(encoded, static_cast<uint32_t>(binding.LogicalKind));
        AppendU32(encoded, binding.Group);
        AppendU32(encoded, binding.Binding);
        AppendU32(encoded, binding.Count);
        AppendU32(encoded, static_cast<uint32_t>(binding.Stages.value()));
        AppendU32(encoded, static_cast<uint32_t>(binding.Placement));
    }
    AppendU32(encoded, static_cast<uint32_t>(result.PushConstants.size()));
    for (const ResolvedPushConstantBlock& block : result.PushConstants) {
        AppendPushBlock(encoded, block);
    }
    result.Hash = Digest(encoded);
    return result;
}

std::optional<ResolvedVulkanLayout> ResolveVulkanLayout(
    const shader::SpirvShaderArtifactView& artifact,
    const VulkanTargetLayoutOptions& options,
    ShaderLayoutResolveError* error) noexcept {
    SetError(error, ShaderLayoutResolveError::None);
    const shader::ShaderArtifactView& view = artifact.Generic();
    if (view.Envelope().Target != static_cast<uint8_t>(shader::ShaderTarget::SPIRV)) {
        SetError(error, ShaderLayoutResolveError::TargetMismatch);
        return std::nullopt;
    }

    ResolvedVulkanLayout result;
    const std::span<const shader::WireSamplerRecord> samplers = view.Samplers();
    result.ImmutableSamplers.assign(samplers.begin(), samplers.end());

    uint32_t maxSet = 0;
    bool hasBindings = false;
    result.Bindings.reserve(view.Bindings().size());
    for (const shader::WireBindingRecord& binding : view.Bindings()) {
        const std::optional<std::string_view> name = view.GetName(binding.Name);
        if (!name.has_value()) {
            SetError(error, ShaderLayoutResolveError::UnsupportedBinding);
            return std::nullopt;
        }
        const auto placement = static_cast<shader::ShaderBindingPlacement>(binding.Placement);
        // The policy's root descriptor is the same intent as a Vulkan dynamic buffer: bind the
        // resource once and move the window at bind time.
        const bool dynamic = placement == shader::ShaderBindingPlacement::RootDescriptor;
        if (dynamic && binding.Count != 1) {
            Reject(
                error,
                ShaderLayoutResolveError::IllegalPlacement,
                fmt::format("dynamic declaration '{}' must be a single descriptor", name.value()));
            return std::nullopt;
        }
        result.Bindings.push_back(
            {.Name = string{name.value()},
             .LogicalKind = static_cast<shader::ShaderBindingKind>(binding.Type),
             .Set = binding.Group,
             .Binding = binding.Binding,
             .Count = binding.Count,
             .Stages = ToStages(binding.StageMask),
             .Placement = dynamic ? VulkanBufferDescriptorPlacement::Dynamic
                                  : VulkanBufferDescriptorPlacement::Regular,
             .ImmutableSamplerIndex = binding.SamplerIndex});
        maxSet = std::max(maxSet, binding.Group);
        hasBindings = true;
    }
    if (hasBindings && maxSet > kMaxSetIndex) {
        Reject(
            error,
            ShaderLayoutResolveError::LimitExceeded,
            fmt::format("set index {} exceeds the supported maximum {}", maxSet, kMaxSetIndex));
        return std::nullopt;
    }
    result.SetCount = hasBindings ? maxSet + 1 : 0;

    vector<const VulkanBufferDescriptorModifier*> bufferModifiers;
    vector<const VulkanImmutableSamplerModifier*> samplerModifiers;
    if (!CanonicalizeModifiers(options.BufferDescriptors, bufferModifiers, error) ||
        !CanonicalizeModifiers(options.ImmutableSamplers, samplerModifiers, error)) {
        return std::nullopt;
    }
    for (const VulkanBufferDescriptorModifier* modifier : bufferModifiers) {
        const Nullable<ResolvedVulkanBinding*> selected =
            FindSelected(result.Bindings, modifier->Selector, error);
        if (!selected.HasValue()) {
            return std::nullopt;
        }
        ResolvedVulkanBinding& binding = *selected.Get();
        // Only uniform and storage buffers have a dynamic form; the modifier never changes the
        // descriptor count.
        if (!shader::CanBeRootDescriptor(binding.LogicalKind) ||
            (modifier->Placement == VulkanBufferDescriptorPlacement::Dynamic &&
             binding.Count != 1)) {
            Reject(
                error,
                ShaderLayoutResolveError::IllegalPlacement,
                fmt::format(
                    "declaration '{}' cannot take a dynamic buffer descriptor",
                    binding.Name));
            return std::nullopt;
        }
        binding.Placement = modifier->Placement;
    }
    for (const VulkanImmutableSamplerModifier* modifier : samplerModifiers) {
        const Nullable<ResolvedVulkanBinding*> selected =
            FindSelected(result.Bindings, modifier->Selector, error);
        if (!selected.HasValue()) {
            return std::nullopt;
        }
        ResolvedVulkanBinding& binding = *selected.Get();
        if (binding.LogicalKind != shader::ShaderBindingKind::Sampler || binding.Count != 1) {
            Reject(
                error,
                ShaderLayoutResolveError::IllegalPlacement,
                fmt::format(
                    "declaration '{}' is not a single sampler",
                    binding.Name));
            return std::nullopt;
        }
        // Replace in place when the policy already published a state, otherwise append; either way
        // the descriptor ends up owning exactly one recipe.
        if (binding.ImmutableSamplerIndex == shader::kShaderNoSampler) {
            binding.ImmutableSamplerIndex = static_cast<uint32_t>(result.ImmutableSamplers.size());
            result.ImmutableSamplers.push_back(modifier->State);
        } else {
            result.ImmutableSamplers[binding.ImmutableSamplerIndex] = modifier->State;
        }
    }

    std::sort(
        result.Bindings.begin(),
        result.Bindings.end(),
        [](const ResolvedVulkanBinding& lhs, const ResolvedVulkanBinding& rhs) noexcept {
            return std::tie(lhs.Set, lhs.Binding) < std::tie(rhs.Set, rhs.Binding);
        });

    vector<ResolvedPushConstantBlock> pushBlocks;
    if (!CollectPushBlocks(view, pushBlocks, error)) {
        return std::nullopt;
    }
    // Vulkan exposes one push constant block per pipeline layout, so a variant with more than one
    // active block cannot be expressed at all.
    if (pushBlocks.size() > 1) {
        Reject(
            error,
            ShaderLayoutResolveError::LimitExceeded,
            "a Vulkan variant supports at most one active push constant block");
        return std::nullopt;
    }
    if (!pushBlocks.empty()) {
        result.PushBlock = pushBlocks.front();
    }

    // Dynamic offsets are consumed in (set, binding) order, which the sort above already
    // established, so the caller's argument order never enters the picture.
    for (uint32_t index = 0; index < result.Bindings.size(); ++index) {
        if (result.Bindings[index].Placement == VulkanBufferDescriptorPlacement::Dynamic) {
            result.DynamicOffsetOrder.push_back(index);
        }
    }
    BuildMetadata(result.Bindings, pushBlocks, result.Metadata);

    vector<byte> encoded;
    encoded.reserve(256);
    AppendU32(encoded, kResolvedLayoutEncodingVersion);
    AppendU32(encoded, static_cast<uint32_t>(shader::ShaderTarget::SPIRV));
    AppendU32(encoded, result.SetCount);
    AppendU32(encoded, static_cast<uint32_t>(result.Bindings.size()));
    for (const ResolvedVulkanBinding& binding : result.Bindings) {
        AppendName(encoded, binding.Name);
        AppendU32(encoded, static_cast<uint32_t>(binding.LogicalKind));
        AppendU32(encoded, binding.Set);
        AppendU32(encoded, binding.Binding);
        AppendU32(encoded, binding.Count);
        AppendU32(encoded, static_cast<uint32_t>(binding.Stages.value()));
        AppendU32(encoded, static_cast<uint32_t>(binding.Placement));
        // The referenced state is encoded inline rather than by index: an index would let two
        // layouts with different sampler states hash the same when the arrays happen to differ.
        if (binding.ImmutableSamplerIndex == shader::kShaderNoSampler) {
            AppendU32(encoded, 0);
        } else {
            AppendU32(encoded, 1);
            AppendSampler(encoded, result.ImmutableSamplers[binding.ImmutableSamplerIndex]);
        }
    }
    AppendU32(encoded, result.PushBlock.has_value() ? 1u : 0u);
    if (result.PushBlock.has_value()) {
        AppendPushBlock(encoded, result.PushBlock.value());
    }
    result.Hash = Digest(encoded);
    return result;
}

}  // namespace radray::render
