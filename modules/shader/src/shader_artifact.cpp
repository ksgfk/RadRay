#include <radray/shader/shader_artifact.h>

#include <cstring>
#include <iterator>

namespace radray::shader {
namespace {

constexpr uint32_t kStageMask =
    (1u << static_cast<uint8_t>(ShaderStage::Vertex)) |
    (1u << static_cast<uint8_t>(ShaderStage::Pixel)) |
    (1u << static_cast<uint8_t>(ShaderStage::Compute));

void SetError(ShaderArtifactDecodeError* error, ShaderArtifactDecodeError value) noexcept {
    if (error != nullptr) {
        *error = value;
    }
}

bool IsRangeAfterHeader(const WireBlobRange& range, uint32_t headerSize) noexcept {
    return range.Size == 0 || range.Offset >= headerSize;
}

bool RangesOverlap(
    const WireBlobRange& lhs,
    const WireBlobRange& rhs) noexcept {
    if (lhs.Size == 0 || rhs.Size == 0) {
        return false;
    }
    const uint64_t lhsEnd = static_cast<uint64_t>(lhs.Offset) + lhs.Size;
    const uint64_t rhsEnd = static_cast<uint64_t>(rhs.Offset) + rhs.Size;
    return lhs.Offset < rhsEnd && rhs.Offset < lhsEnd;
}

template <typename T>
bool CopyRecords(
    std::span<const byte> blob,
    const WireBlobRange& range,
    vector<T>& records) noexcept {
    if (range.Size == 0) {
        return true;
    }
    if (range.Size % sizeof(T) != 0 || range.Offset > blob.size() || range.Size > blob.size() - range.Offset) {
        return false;
    }
    records.resize(range.Size / sizeof(T));
    std::memcpy(records.data(), blob.data() + range.Offset, range.Size);
    return true;
}

bool IsValidName(
    const ShaderArtifactView& artifact,
    WireBlobRange range,
    uint32_t bytecodeOffset) noexcept {
    return range.Size != 0 && range.IsWithin(static_cast<uint32_t>(artifact.Envelope().TotalSize)) &&
           range.Offset >= artifact.Envelope().HeaderSize &&
           range.Offset <= bytecodeOffset && range.Size <= bytecodeOffset - range.Offset;
}

bool ValidateTypeRecords(const ShaderArtifactView& artifact) noexcept {
    constexpr uint32_t kNoParent = kShaderNoType;
    constexpr uint32_t kScalar = static_cast<uint32_t>(ShaderTypeKind::Scalar);
    constexpr uint32_t kStruct = static_cast<uint32_t>(ShaderTypeKind::Struct);
    constexpr uint32_t kArray = static_cast<uint32_t>(ShaderTypeKind::Array);
    constexpr uint32_t kMember = static_cast<uint32_t>(ShaderTypeKind::Member);
    const std::span<const WireTypeRecord> types = artifact.Types();
    vector<uint32_t> parentVisit(types.size(), 0);

    for (size_t index = 0; index < types.size(); ++index) {
        const WireTypeRecord& type = types[index];
        if (!IsValidName(artifact, type.Name, artifact.Envelope().Bytecode.Offset) || type.Size == 0 ||
            type.Stride == 0 || type.Kind < kScalar || type.Kind > kMember || type.ElementCount == 0) {
            return false;
        }
        if (type.Kind == kArray) {
            const uint64_t expectedSize = static_cast<uint64_t>(type.Stride) * type.ElementCount;
            if (expectedSize != type.Size) {
                return false;
            }
        } else if (type.ElementCount != 1 || type.Size != type.Stride) {
            return false;
        }

        if (type.ParentIndex == kNoParent) {
            if (type.Offset != 0) {
                return false;
            }
        } else {
            if (type.ParentIndex >= types.size() || type.ParentIndex == index ||
                types[type.ParentIndex].Kind != kStruct) {
                return false;
            }
            const WireTypeRecord& parent = types[type.ParentIndex];
            if (type.Offset > parent.Size || type.Size > parent.Size - type.Offset) {
                return false;
            }
        }
        if (type.TypeIndex != kShaderNoType) {
            if (type.TypeIndex >= types.size() || type.TypeIndex == index ||
                types[type.TypeIndex].ParentIndex != kNoParent ||
                types[type.TypeIndex].Kind != kStruct) {
                return false;
            }
        }
        if (type.ParentIndex != kNoParent && type.Kind == kStruct &&
            type.TypeIndex == kShaderNoType) {
            return false;
        }

        const uint32_t visitToken = static_cast<uint32_t>(index + 1);
        size_t cursor = index;
        while (cursor < types.size() && types[cursor].ParentIndex != kNoParent) {
            if (parentVisit[cursor] == visitToken) {
                return false;
            }
            parentVisit[cursor] = visitToken;
            cursor = types[cursor].ParentIndex;
        }
        if (cursor >= types.size()) {
            return false;
        }

        for (size_t previous = 0; previous < index; ++previous) {
            if (types[previous].ParentIndex != type.ParentIndex) {
                continue;
            }
            const std::optional<std::string_view> previousName = artifact.GetName(types[previous].Name);
            const std::optional<std::string_view> currentName = artifact.GetName(type.Name);
            if (previousName.has_value() && currentName.has_value() && previousName == currentName) {
                return false;
            }
        }
    }
    return true;
}

bool ValidateVertexInputRecords(const ShaderArtifactView& artifact) noexcept {
    const std::span<const WireVertexInputRecord> inputs = artifact.VertexInputs();
    for (size_t index = 0; index < inputs.size(); ++index) {
        const WireVertexInputRecord& input = inputs[index];
        const std::optional<std::string_view> semantic = artifact.GetName(input.Semantic);
        const uint32_t componentType = input.ComponentType;
        if (!IsValidName(artifact, input.Semantic, artifact.Envelope().Bytecode.Offset) ||
            !semantic.has_value() || semantic->empty() || input.ComponentCount == 0 ||
            input.ComponentCount > 4 ||
            (componentType != static_cast<uint32_t>(ShaderVertexComponentType::Float) &&
             componentType != static_cast<uint32_t>(ShaderVertexComponentType::SignedInteger) &&
             componentType != static_cast<uint32_t>(ShaderVertexComponentType::UnsignedInteger))) {
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const WireVertexInputRecord& old = inputs[previous];
            const std::optional<std::string_view> oldSemantic = artifact.GetName(old.Semantic);
            if (old.Location == input.Location ||
                (oldSemantic.has_value() && oldSemantic == semantic &&
                 old.SemanticIndex == input.SemanticIndex)) {
                return false;
            }
        }
    }
    return true;
}

// Placement legality mirrors the compiler's 2122 check, so a hand-built or tampered artifact
// cannot ask a backend to do something the policy frontend would have rejected.
bool IsValidPlacement(
    const WireBindingRecord& binding,
    ShaderTarget target,
    size_t samplerCount) noexcept {
    const auto kind = static_cast<ShaderBindingKind>(binding.Type);
    switch (static_cast<ShaderBindingPlacement>(binding.Placement)) {
        case ShaderBindingPlacement::Table:
            break;
        case ShaderBindingPlacement::RootDescriptor:
            // A root descriptor carries one GPU address, and the public dynamic-offset shape has
            // no array element, so count > 1 has nowhere to land.
            if (!CanBeRootDescriptor(kind) || binding.Count != 1) {
                return false;
            }
            break;
        case ShaderBindingPlacement::StaticSampler:
            // D3D-only shape: the slot lives in the serialized carrier and owns no table entry.
            if (kind != ShaderBindingKind::Sampler || target != ShaderTarget::DXIL) {
                return false;
            }
            break;
        default:
            return false;
    }
    if (binding.SamplerIndex == kShaderNoSampler) {
        return true;
    }
    // A sampler reference is the Vulkan landing of a static sampler: still a table entry, but its
    // state comes from the record array instead of from a caller-provided sampler.
    return kind == ShaderBindingKind::Sampler &&
           static_cast<ShaderBindingPlacement>(binding.Placement) ==
               ShaderBindingPlacement::Table &&
           binding.SamplerIndex < samplerCount;
}

bool ValidateSamplerRecords(const ShaderArtifactView& artifact) noexcept {
    for (const WireSamplerRecord& sampler : artifact.Samplers()) {
        if (sampler.MagFilter > kShaderSamplerMaxFilter ||
            sampler.MinFilter > kShaderSamplerMaxFilter ||
            sampler.MipmapMode > kShaderSamplerMaxMipmapMode ||
            sampler.AddressModeU > kShaderSamplerMaxAddressMode ||
            sampler.AddressModeV > kShaderSamplerMaxAddressMode ||
            sampler.AddressModeW > kShaderSamplerMaxAddressMode ||
            sampler.AnisotropyEnable > 1 || sampler.CompareEnable > 1 ||
            sampler.CompareOp > kShaderSamplerMaxCompareOp ||
            sampler.BorderColor > kShaderSamplerMaxBorderColor ||
            sampler.ReductionMode > kShaderSamplerMaxReductionMode ||
            (sampler.Flags & ~kShaderSamplerFlagMask) != 0) {
            return false;
        }
        if (!(sampler.MaxAnisotropy >= 1.0f) || !(sampler.MipLodBias == sampler.MipLodBias) ||
            !(sampler.MinLod >= 0.0f) || !(sampler.MaxLod >= sampler.MinLod)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::span<const byte> ShaderArtifactView::SerializedRootSignature() const noexcept {
    if (_envelope.RootSignature.Size == 0) {
        return {};
    }
    return std::span<const byte>{_blob}.subspan(
        _envelope.RootSignature.Offset,
        _envelope.RootSignature.Size);
}

std::span<const byte> ShaderArtifactView::Bytecode() const noexcept {
    return std::span<const byte>{_blob}.subspan(_envelope.Bytecode.Offset, _envelope.Bytecode.Size);
}

std::optional<std::string_view> ShaderArtifactView::GetName(WireBlobRange range) const noexcept {
    if (!range.IsWithin(static_cast<uint32_t>(_blob.size()))) {
        return std::nullopt;
    }
    const auto* data = reinterpret_cast<const char*>(_blob.data() + range.Offset);
    return std::string_view{data, range.Size};
}

std::optional<std::span<const byte>> ShaderArtifactView::FindStageBytecode(
    ShaderStage stage) const noexcept {
    for (const WireEntryRecord& entry : _entries) {
        if (entry.Stage == static_cast<uint8_t>(stage)) {
            return Bytecode().subspan(entry.InterfaceOffset, entry.InterfaceSize);
        }
    }
    return std::nullopt;
}

std::optional<ShaderArtifactBindingView> ShaderArtifactView::FindBinding(
    std::string_view name) const noexcept {
    for (const WireBindingRecord& binding : _bindings) {
        const std::optional<std::string_view> bindingName = GetName(binding.Name);
        if (bindingName.has_value() && bindingName.value() == name) {
            return ShaderArtifactBindingView{bindingName.value(), binding};
        }
    }
    return std::nullopt;
}

std::optional<ShaderArtifactView> DecodeShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options,
    ShaderArtifactDecodeError* error) noexcept {
    SetError(error, ShaderArtifactDecodeError::None);
    if (blob.size() < sizeof(WireMetadataEnvelope)) {
        SetError(error, ShaderArtifactDecodeError::TruncatedEnvelope);
        return std::nullopt;
    }

    ShaderArtifactView result;
    uint16_t schema = 0;
    std::memcpy(&schema, blob.data() + sizeof(uint32_t), sizeof(schema));
    if (schema != kShaderMetadataSchemaVersion) {
        SetError(error, ShaderArtifactDecodeError::UnsupportedSchemaVersion);
        return std::nullopt;
    }
    std::memcpy(&result._envelope, blob.data(), sizeof(result._envelope));
    const WireMetadataEnvelope& envelope = result._envelope;
    if (envelope.Target > static_cast<uint8_t>(ShaderTarget::SPIRV)) {
        SetError(error, ShaderArtifactDecodeError::InvalidTarget);
        return std::nullopt;
    }
    // The carrier belongs to DXIL and the sampler records belong to SPIR-V. Neither lane may
    // publish the other's form of the same policy.
    if (envelope.Target == static_cast<uint8_t>(ShaderTarget::SPIRV) &&
        envelope.RootSignature.Size != 0) {
        SetError(error, ShaderArtifactDecodeError::InvalidRootSignature);
        return std::nullopt;
    }
    if (envelope.Target == static_cast<uint8_t>(ShaderTarget::DXIL) &&
        envelope.SamplerRecords.Size != 0) {
        SetError(error, ShaderArtifactDecodeError::InvalidSamplerRecord);
        return std::nullopt;
    }
    if (!ValidateWireMetadataEnvelope(blob, options.Target, options.ExpectedGpuArtifact)) {
        SetError(error, ShaderArtifactDecodeError::InvalidEnvelope);
        return std::nullopt;
    }
    if (options.ExpectedToolchainIdentity != 0 &&
        envelope.ToolchainIdentity != options.ExpectedToolchainIdentity) {
        SetError(error, ShaderArtifactDecodeError::ToolchainMismatch);
        return std::nullopt;
    }
    result._blob.assign(blob.begin(), blob.end());
    // Every section must sit past the header and no two may overlap: sections are copied out by
    // offset, so an overlap would let one record array reinterpret another's bytes.
    const WireBlobRange sections[]{
        envelope.EntryRecords,
        envelope.BindingRecords,
        envelope.TypeRecords,
        envelope.RootConstantRecords,
        envelope.VertexInputRecords,
        envelope.SamplerRecords,
        envelope.RootSignature,
        envelope.Bytecode};
    if (envelope.HeaderSize != sizeof(WireMetadataEnvelope)) {
        SetError(error, ShaderArtifactDecodeError::InvalidRecordRange);
        return std::nullopt;
    }
    for (size_t index = 0; index < std::size(sections); ++index) {
        if (!IsRangeAfterHeader(sections[index], envelope.HeaderSize)) {
            SetError(error, ShaderArtifactDecodeError::InvalidRecordRange);
            return std::nullopt;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (RangesOverlap(sections[previous], sections[index])) {
                SetError(error, ShaderArtifactDecodeError::InvalidRecordRange);
                return std::nullopt;
            }
        }
    }
    if (envelope.RootSignature.Size != 0 &&
        (!envelope.RootSignature.IsWithin(static_cast<uint32_t>(blob.size())) ||
         envelope.RootSignature.Offset < envelope.HeaderSize)) {
        SetError(error, ShaderArtifactDecodeError::InvalidRootSignature);
        return std::nullopt;
    }

    if (!CopyRecords(blob, envelope.EntryRecords, result._entries) ||
        !CopyRecords(blob, envelope.BindingRecords, result._bindings) ||
        !CopyRecords(blob, envelope.TypeRecords, result._types) ||
        !CopyRecords(blob, envelope.RootConstantRecords, result._rootConstants) ||
        !CopyRecords(blob, envelope.VertexInputRecords, result._vertexInputs) ||
        !CopyRecords(blob, envelope.SamplerRecords, result._samplers)) {
        SetError(error, ShaderArtifactDecodeError::InvalidRecordRange);
        return std::nullopt;
    }

    for (size_t index = 0; index < result._entries.size(); ++index) {
        const WireEntryRecord& entry = result._entries[index];
        if (!IsValidName(result, entry.Name, envelope.Bytecode.Offset) ||
            entry.Stage > static_cast<uint8_t>(ShaderStage::Compute) ||
            entry.InterfaceSize == 0 ||
            entry.InterfaceOffset > envelope.Bytecode.Size ||
            entry.InterfaceSize > envelope.Bytecode.Size - entry.InterfaceOffset) {
            SetError(error, ShaderArtifactDecodeError::InvalidEntry);
            return std::nullopt;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const std::optional<std::string_view> previousName = result.GetName(result._entries[previous].Name);
            const std::optional<std::string_view> currentName = result.GetName(entry.Name);
            if (previousName.has_value() && currentName.has_value() && previousName == currentName) {
                SetError(error, ShaderArtifactDecodeError::DuplicateEntry);
                return std::nullopt;
            }
        }
    }

    for (size_t index = 0; index < result._bindings.size(); ++index) {
        const WireBindingRecord& binding = result._bindings[index];
        const std::optional<std::string_view> bindingName = result.GetName(binding.Name);
        if (!IsValidName(result, binding.Name, envelope.Bytecode.Offset) ||
            !bindingName.has_value() || binding.Group > 0xffffu || binding.Count == 0 ||
            binding.StageMask == 0 || (binding.StageMask & ~kStageMask) != 0 ||
            GetWireBindingNamespace(binding.Type) == 0xffffffffu) {
            SetError(error, ShaderArtifactDecodeError::InvalidBinding);
            return std::nullopt;
        }
        if (!IsValidPlacement(binding, static_cast<ShaderTarget>(envelope.Target),
                              result._samplers.size())) {
            SetError(error, ShaderArtifactDecodeError::InvalidPlacement);
            return std::nullopt;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const WireBindingRecord& old = result._bindings[previous];
            const std::optional<std::string_view> oldName = result.GetName(old.Name);
            const bool sameTargetCoordinate = envelope.Target == static_cast<uint8_t>(ShaderTarget::SPIRV)
                                                  ? old.Group == binding.Group && old.Binding == binding.Binding
                                                  : old.Group == binding.Group && old.Binding == binding.Binding &&
                                                        GetWireBindingNamespace(old.Type) ==
                                                            GetWireBindingNamespace(binding.Type);
            if ((oldName.has_value() && oldName == bindingName) || sameTargetCoordinate) {
                SetError(error, ShaderArtifactDecodeError::DuplicateBinding);
                return std::nullopt;
            }
        }
    }

    if (!ValidateTypeRecords(result)) {
        SetError(error, ShaderArtifactDecodeError::InvalidTypeRecord);
        return std::nullopt;
    }

    for (size_t index = 0; index < result._rootConstants.size(); ++index) {
        const WireRootConstantRecord& constant = result._rootConstants[index];
        const std::optional<std::string_view> name = result.GetName(constant.Name);
        if (constant.Size == 0 || (constant.Size % 4) != 0 || constant.StageMask == 0 ||
            (constant.StageMask & ~kStageMask) != 0 ||
            !IsValidName(result, constant.Name, envelope.Bytecode.Offset) || !name.has_value()) {
            SetError(error, ShaderArtifactDecodeError::InvalidRootConstant);
            return std::nullopt;
        }
        // The push handle table is keyed on the declaration name, and a push block never doubles
        // as a descriptor, so the name has to be unique across both record arrays.
        for (size_t previous = 0; previous < index; ++previous) {
            if (result.GetName(result._rootConstants[previous].Name) == name) {
                SetError(error, ShaderArtifactDecodeError::InvalidRootConstant);
                return std::nullopt;
            }
        }
        if (result.FindBinding(name.value()).has_value()) {
            SetError(error, ShaderArtifactDecodeError::InvalidRootConstant);
            return std::nullopt;
        }
    }
    if (!ValidateSamplerRecords(result)) {
        SetError(error, ShaderArtifactDecodeError::InvalidSamplerRecord);
        return std::nullopt;
    }
    if (!ValidateVertexInputRecords(result)) {
        SetError(error, ShaderArtifactDecodeError::InvalidVertexInput);
        return std::nullopt;
    }
    return result;
}

std::optional<DxilShaderArtifactView> DecodeDxilShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options,
    ShaderArtifactDecodeError* error) noexcept {
    if (options.Target != ShaderTarget::DXIL) {
        SetError(error, ShaderArtifactDecodeError::InvalidTarget);
        return std::nullopt;
    }
    std::optional<ShaderArtifactView> view = DecodeShaderArtifact(blob, options, error);
    if (!view.has_value()) {
        return std::nullopt;
    }
    return DxilShaderArtifactView{std::move(view.value())};
}

std::optional<SpirvShaderArtifactView> DecodeSpirvShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options,
    ShaderArtifactDecodeError* error) noexcept {
    if (options.Target != ShaderTarget::SPIRV) {
        SetError(error, ShaderArtifactDecodeError::InvalidTarget);
        return std::nullopt;
    }
    std::optional<ShaderArtifactView> view = DecodeShaderArtifact(blob, options, error);
    if (!view.has_value()) {
        return std::nullopt;
    }
    return SpirvShaderArtifactView{std::move(view.value())};
}

}  // namespace radray::shader
