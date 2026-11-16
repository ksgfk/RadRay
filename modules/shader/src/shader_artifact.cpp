#include <radray/shader/shader_artifact.h>
#include <radray/shader/shader_metadata_wire.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include <radray/logger.h>

namespace radray::shader {
namespace {

constexpr uint32_t kStageMask =
    (1u << static_cast<uint8_t>(ShaderStage::Vertex)) |
    (1u << static_cast<uint8_t>(ShaderStage::Pixel)) |
    (1u << static_cast<uint8_t>(ShaderStage::Compute));

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
    vector<T>& records) {
    if (range.Size == 0) {
        return true;
    }
    const auto stride = GetShaderMetadataRecordSize(T{});
    if (range.Size % stride != 0 || range.Offset > blob.size() || range.Size > blob.size() - range.Offset) {
        return false;
    }
    records.resize(range.Size / stride);
    for (size_t i = 0; i < records.size(); ++i)
        if (!DecodeShaderMetadataRecord(blob.subspan(range.Offset + i * stride, stride), records[i])) return false;
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

string ValidateTypeRecords(const ShaderArtifactView& artifact) {
    const auto types = artifact.Types();
    vector<bool> resolved(types.size(), false);
    size_t unresolvedRoots = 0;
    for (size_t index = 0; index < types.size(); ++index) {
        const auto& type = types[index];
        const bool root = type.ParentIndex == kShaderNoType;
        const bool array = type.Kind == static_cast<uint32_t>(ShaderTypeKind::Array);
        const bool structure = type.ElementKind == static_cast<uint32_t>(ShaderTypeKind::Struct);
        if (!IsValidName(artifact, type.Name, artifact.Envelope().Bytecode.Offset) || type.Size == 0 ||
            type.ElementSize == 0 || type.ElementCount == 0 || type.Kind < 1 || type.Kind > 5 ||
            type.ElementKind < 1 || type.ElementKind > 4 || (type.Flags & ~kShaderTypeFlagMask) != 0) {
            return radray::format("type record {} has invalid shape or name", index);
        }
        if (array) {
            if (type.Stride < type.ElementSize ||
                uint64_t{type.ElementCount - 1} * type.Stride + type.ElementSize != type.Size) {
                return radray::format("type record {} array size or stride mismatch", index);
            }
        } else if (type.Kind != type.ElementKind || type.ElementCount != 1 || type.Stride != 0 || type.Size != type.ElementSize) {
            return radray::format("type record {} has incompatible size or stride", index);
        }
        if (root) {
            if (type.Kind != 4 || type.TypeIndex != kShaderNoType || type.Offset != 0) {
                return radray::format("type record {} is not a root struct", index);
            }
            ++unresolvedRoots;
        } else if (type.ParentIndex >= types.size() || types[type.ParentIndex].ParentIndex != kShaderNoType ||
                   types[type.ParentIndex].Kind != 4 || type.Offset > types[type.ParentIndex].Size ||
                   type.Size > types[type.ParentIndex].Size - type.Offset) {
            return radray::format("type record {} has invalid parent or exceeds parent range", index);
        }
        if (structure) {
            if (type.ScalarKind != 0 || type.RowCount != 0 || type.ColumnCount != 0 || type.Flags != 0 || type.MatrixStride != 0 ||
                (!root && (type.TypeIndex >= types.size() || types[type.TypeIndex].ParentIndex != kShaderNoType ||
                           types[type.TypeIndex].Kind != 4 || type.ElementSize != types[type.TypeIndex].Size))) {
                return radray::format("type record {} has invalid underlying struct or shape", index);
            }
        } else {
            if (type.TypeIndex != kShaderNoType || type.ScalarKind < 1 || type.ScalarKind > 4) {
                return radray::format("type record {} has invalid scalar kind or type owner", index);
            }
            if (type.ElementKind == 3) {
                const bool rowMajor = (type.Flags & kShaderTypeFlagRowMajor) != 0;
                const uint32_t major = rowMajor ? type.RowCount : type.ColumnCount;
                const uint32_t minor = rowMajor ? type.ColumnCount : type.RowCount;
                if (type.ScalarKind != 1 || type.RowCount < 2 || type.RowCount > 4 || type.ColumnCount < 2 || type.ColumnCount > 4 ||
                    type.MatrixStride < minor * 4 || type.MatrixStride % 4 != 0 ||
                    uint64_t{major - 1} * type.MatrixStride + minor * 4 != type.ElementSize) {
                    return radray::format("type record {} has invalid matrix shape", index);
                }
            } else if (type.RowCount != 1 || type.ColumnCount < 1 || type.ColumnCount > 4 ||
                       (type.ElementKind == 1 && type.ColumnCount != 1) ||
                       type.ElementSize != type.ColumnCount * 4 || type.MatrixStride != 0 || type.Flags != 0) {
                return radray::format("type record {} has invalid scalar/vector shape", index);
            }
        }
        if (!root) {
            for (size_t previous = 0; previous < index; ++previous) {
                const auto& old = types[previous];
                if (old.ParentIndex != type.ParentIndex) continue;
                if (artifact.GetName(old.Name) == artifact.GetName(type.Name))
                    return radray::format("type record {} duplicates sibling name", index);
                if (uint64_t{type.Offset} < uint64_t{old.Offset} + old.Size && uint64_t{old.Offset} < uint64_t{type.Offset} + type.Size)
                    return radray::format("type record {} overlaps sibling {}", index, previous);
            }
        }
    }
    // Resolve the struct dependency graph without recursive traversal of untrusted input.
    while (unresolvedRoots != 0) {
        bool progress = false;
        for (size_t index = 0; index < types.size(); ++index) {
            if (types[index].ParentIndex != kShaderNoType || resolved[index]) continue;
            bool ready = true;
            uint64_t size = 0;
            for (const auto& child : types) {
                if (child.ParentIndex != index) continue;
                size = std::max(size, uint64_t{child.Offset} + child.Size);
                if (child.TypeIndex != kShaderNoType && !resolved[child.TypeIndex]) ready = false;
            }
            if (!ready) continue;
            if (size != types[index].Size) return radray::format("root type {} size differs from members", index);
            resolved[index] = true;
            --unresolvedRoots;
            progress = true;
        }
        if (!progress) return "cyclic underlying struct types";
    }
    return {};
}
bool IsRootStruct(
    const ShaderArtifactView& artifact,
    uint32_t typeIndex) noexcept {
    const std::span<const ShaderArtifactTypeRecord> types = artifact.Types();
    return typeIndex < types.size() &&
           types[typeIndex].ParentIndex == kShaderNoType &&
           types[typeIndex].Kind == static_cast<uint32_t>(ShaderTypeKind::Struct);
}

string ValidateVertexInputRecords(const ShaderArtifactView& artifact) {
    const std::span<const ShaderArtifactVertexInputRecord> inputs = artifact.VertexInputs();
    for (size_t index = 0; index < inputs.size(); ++index) {
        const ShaderArtifactVertexInputRecord& input = inputs[index];
        const std::optional<std::string_view> semantic = artifact.GetName(input.Semantic);
        const uint32_t componentType = input.ComponentType;
        if (!IsValidName(artifact, input.Semantic, artifact.Envelope().Bytecode.Offset) ||
            !semantic.has_value() || semantic->empty() || input.ComponentCount == 0 ||
            input.ComponentCount > 4 ||
            (componentType != static_cast<uint32_t>(ShaderVertexComponentType::Float) &&
             componentType != static_cast<uint32_t>(ShaderVertexComponentType::SignedInteger) &&
             componentType != static_cast<uint32_t>(ShaderVertexComponentType::UnsignedInteger))) {
            return radray::format("vertex input record {} has an invalid semantic, component type, or component count", index);
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const ShaderArtifactVertexInputRecord& old = inputs[previous];
            const std::optional<std::string_view> oldSemantic = artifact.GetName(old.Semantic);
            if (old.Location == input.Location ||
                (oldSemantic.has_value() && oldSemantic == semantic &&
                 old.SemanticIndex == input.SemanticIndex)) {
                return radray::format("vertex input record {} ({}) duplicates a semantic or location from record {}", index, *semantic, previous);
            }
        }
    }
    return {};
}

// Placement legality mirrors the compiler's 2122 check, so a hand-built or tampered artifact
// cannot ask a backend to do something the policy frontend would have rejected.
bool IsValidPlacement(
    const ShaderArtifactBindingRecord& binding,
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

string ValidateSamplerRecords(const ShaderArtifactView& artifact) {
    for (size_t index = 0; index < artifact.Samplers().size(); ++index) {
        const ShaderArtifactSamplerRecord& sampler = artifact.Samplers()[index];
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
            return radray::format("sampler record {} has unsupported filter, address, compare, border, reduction, or flags", index);
        }
        if (!(sampler.MaxAnisotropy >= 1.0f) || !(sampler.MipLodBias == sampler.MipLodBias) ||
            !(sampler.MinLod >= 0.0f) || !(sampler.MaxLod >= sampler.MinLod)) {
            return radray::format("sampler record {} has invalid anisotropy or LOD bounds", index);
        }
    }
    return {};
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
    for (const ShaderArtifactEntryRecord& entry : _entries) {
        if (entry.Stage == static_cast<uint8_t>(stage)) {
            return Bytecode().subspan(entry.InterfaceOffset, entry.InterfaceSize);
        }
    }
    return std::nullopt;
}

std::optional<ShaderArtifactBindingView> ShaderArtifactView::FindBinding(
    std::string_view name) const noexcept {
    for (const ShaderArtifactBindingRecord& binding : _bindings) {
        const std::optional<std::string_view> bindingName = GetName(binding.Name);
        if (bindingName.has_value() && bindingName.value() == name) {
            return ShaderArtifactBindingView{bindingName.value(), binding};
        }
    }
    return std::nullopt;
}

DiagnosticResult<ShaderArtifactView> DecodeShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options) {
    if (blob.size() < sizeof(WireMetadataEnvelope)) {
        return {std::nullopt, radray::format("metadata envelope is truncated: expected at least {} bytes, got {}", sizeof(WireMetadataEnvelope), blob.size())};
    }

    ShaderArtifactView result;
    DecodeShaderMetadataEnvelope(blob.first(152), result._envelope);
    const auto schema = result._envelope.SchemaVersion;
    if (schema != kShaderMetadataSchemaVersion) {
        return {std::nullopt, radray::format("unsupported metadata schema: expected {}, got {}", kShaderMetadataSchemaVersion, schema)};
    }
    const WireMetadataEnvelope& envelope = result._envelope;
    if (envelope.Target > static_cast<uint8_t>(ShaderTarget::SPIRV)) {
        return {std::nullopt, radray::format("artifact target does not match the requested decoder: requested {}", static_cast<uint32_t>(options.Target))};
    }
    // The carrier belongs to DXIL and the sampler records belong to SPIR-V. Neither lane may
    // publish the other's form of the same policy.
    if (envelope.Target == static_cast<uint8_t>(ShaderTarget::SPIRV) &&
        envelope.RootSignature.Size != 0) {
        return {std::nullopt, "root signature carrier is invalid for this target or lies outside the payload"};
    }
    if (envelope.Target == static_cast<uint8_t>(ShaderTarget::DXIL) &&
        envelope.SamplerRecords.Size != 0) {
        return {std::nullopt, "sampler records are invalid for this target or contain unsupported sampler state"};
    }
    if (!ValidateWireMetadataEnvelope(blob, options.Target, options.ExpectedGpuArtifact)) {
        return {std::nullopt, "metadata envelope magic, size, target, artifact identity, or payload range is invalid"};
    }
    if (options.ExpectedToolchainIdentity != 0 &&
        envelope.ToolchainIdentity != options.ExpectedToolchainIdentity) {
        return {std::nullopt, radray::format("toolchain identity mismatch: expected {:#x}, got {:#x}", options.ExpectedToolchainIdentity, envelope.ToolchainIdentity)};
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
        return {std::nullopt, radray::format("metadata header size mismatch: expected {}, got {}", sizeof(WireMetadataEnvelope), envelope.HeaderSize)};
    }
    for (size_t index = 0; index < std::size(sections); ++index) {
        if (!IsRangeAfterHeader(sections[index], envelope.HeaderSize)) {
            return {std::nullopt, radray::format("metadata section {} overlaps header: offset {}, size {}", index, sections[index].Offset, sections[index].Size)};
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (RangesOverlap(sections[previous], sections[index])) {
                return {std::nullopt, radray::format("metadata section {} overlaps section {}", index, previous)};
            }
        }
    }
    if (envelope.RootSignature.Size != 0 &&
        (!envelope.RootSignature.IsWithin(static_cast<uint32_t>(blob.size())) ||
         envelope.RootSignature.Offset < envelope.HeaderSize)) {
        return {std::nullopt, "root signature carrier is invalid for this target or lies outside the payload"};
    }

    if (!CopyRecords(blob, envelope.EntryRecords, result._entries) ||
        !CopyRecords(blob, envelope.BindingRecords, result._bindings) ||
        !CopyRecords(blob, envelope.TypeRecords, result._types) ||
        !CopyRecords(blob, envelope.RootConstantRecords, result._rootConstants) ||
        !CopyRecords(blob, envelope.VertexInputRecords, result._vertexInputs) ||
        !CopyRecords(blob, envelope.SamplerRecords, result._samplers)) {
        return {std::nullopt, "metadata record range is misaligned or outside the payload"};
    }

    for (size_t index = 0; index < result._entries.size(); ++index) {
        const ShaderArtifactEntryRecord& entry = result._entries[index];
        if (!IsValidName(result, entry.Name, envelope.Bytecode.Offset) ||
            entry.Stage > static_cast<uint8_t>(ShaderStage::Compute) ||
            entry.InterfaceSize == 0 ||
            entry.InterfaceOffset > envelope.Bytecode.Size ||
            entry.InterfaceSize > envelope.Bytecode.Size - entry.InterfaceOffset) {
            return {std::nullopt, radray::format("entry record {} has an invalid name, stage, or bytecode range", index)};
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const std::optional<std::string_view> previousName = result.GetName(result._entries[previous].Name);
            const std::optional<std::string_view> currentName = result.GetName(entry.Name);
            if (previousName.has_value() && currentName.has_value() && previousName == currentName) {
                return {std::nullopt, radray::format("duplicate entry name at record {}: {}", index, *currentName)};
            }
        }
    }

    for (size_t index = 0; index < result._bindings.size(); ++index) {
        const ShaderArtifactBindingRecord& binding = result._bindings[index];
        const std::optional<std::string_view> bindingName = result.GetName(binding.Name);
        if (!IsValidName(result, binding.Name, envelope.Bytecode.Offset) ||
            !bindingName.has_value() || binding.Group > 0xffffu || binding.Count == 0 ||
            binding.StageMask == 0 || (binding.StageMask & ~kStageMask) != 0 ||
            GetWireBindingNamespace(binding.Type) == 0xffffffffu) {
            return {std::nullopt, radray::format("binding record {} has an invalid name, kind, count, stage mask, or group", index)};
        }
        if (!IsValidPlacement(binding, static_cast<ShaderTarget>(envelope.Target),
                              result._samplers.size())) {
            return {std::nullopt, radray::format("binding record {} ({}) has an invalid target placement", index, *bindingName)};
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const ShaderArtifactBindingRecord& old = result._bindings[previous];
            const std::optional<std::string_view> oldName = result.GetName(old.Name);
            const bool sameTargetCoordinate = envelope.Target == static_cast<uint8_t>(ShaderTarget::SPIRV)
                                                  ? old.Group == binding.Group && old.Binding == binding.Binding
                                                  : old.Group == binding.Group && old.Binding == binding.Binding &&
                                                        GetWireBindingNamespace(old.Type) ==
                                                            GetWireBindingNamespace(binding.Type);
            if ((oldName.has_value() && oldName == bindingName) || sameTargetCoordinate) {
                return {std::nullopt, radray::format("duplicate binding name or target coordinate at record {} ({})", index, *bindingName)};
            }
        }
    }

    if (string diagnostic = ValidateTypeRecords(result); !diagnostic.empty()) {
        return {std::nullopt, std::move(diagnostic)};
    }
    for (const ShaderArtifactBindingRecord& binding : result._bindings) {
        const bool cbuffer =
            binding.Type == static_cast<uint32_t>(ShaderBindingKind::CBuffer);
        if ((cbuffer && !IsRootStruct(result, binding.TypeIndex)) ||
            (!cbuffer && (binding.TypeIndex != kShaderNoType || binding.BufferByteSize != 0)) ||
            (cbuffer && (binding.BufferByteSize == 0 || binding.BufferByteSize % 16 != 0 ||
                         binding.BufferByteSize < result._types[binding.TypeIndex].Size))) {
            return {std::nullopt, radray::format("binding {} has invalid type owner {}", *result.GetName(binding.Name), binding.TypeIndex)};
        }
    }

    for (size_t index = 0; index < result._rootConstants.size(); ++index) {
        const ShaderArtifactRootConstantRecord& constant = result._rootConstants[index];
        const std::optional<std::string_view> name = result.GetName(constant.Name);
        if (constant.Size == 0 || (constant.Size % 4) != 0 || constant.StageMask == 0 ||
            (constant.StageMask & ~kStageMask) != 0 ||
            !IsValidName(result, constant.Name, envelope.Bytecode.Offset) || !name.has_value()) {
            return {std::nullopt, radray::format("root constant record {} has an invalid name, range, type owner, or duplicate declaration", index)};
        }
        if (constant.TypeIndex != kShaderNoType &&
            (!IsRootStruct(result, constant.TypeIndex) ||
             result._types[constant.TypeIndex].Size > constant.Size)) {
            return {std::nullopt, radray::format("root constant record {} has an invalid name, range, type owner, or duplicate declaration", index)};
        }
        // The push handle table is keyed on the declaration name, and a push block never doubles
        // as a descriptor, so the name has to be unique across both record arrays.
        for (size_t previous = 0; previous < index; ++previous) {
            if (result.GetName(result._rootConstants[previous].Name) == name) {
                return {std::nullopt, radray::format("root constant record {} has an invalid name, range, type owner, or duplicate declaration", index)};
            }
        }
        if (result.FindBinding(name.value()).has_value()) {
            return {std::nullopt, radray::format("root constant record {} has an invalid name, range, type owner, or duplicate declaration", index)};
        }
    }
    if (string diagnostic = ValidateSamplerRecords(result); !diagnostic.empty()) {
        return {std::nullopt, std::move(diagnostic)};
    }
    if (string diagnostic = ValidateVertexInputRecords(result); !diagnostic.empty()) {
        return {std::nullopt, std::move(diagnostic)};
    }
    return {std::move(result), {}};
}

DiagnosticResult<DxilShaderArtifactView> DecodeDxilShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options) {
    if (options.Target != ShaderTarget::DXIL) {
        return {std::nullopt, radray::format("artifact target does not match the requested decoder: requested {}", static_cast<uint32_t>(options.Target))};
    }
    DiagnosticResult<ShaderArtifactView> view = DecodeShaderArtifact(blob, options);
    if (!view.Succeeded()) {
        return {std::nullopt, std::move(view.Diagnostics)};
    }
    return {DxilShaderArtifactView{std::move(*view.Value)}, std::move(view.Diagnostics)};
}

DiagnosticResult<SpirvShaderArtifactView> DecodeSpirvShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options) {
    if (options.Target != ShaderTarget::SPIRV) {
        return {std::nullopt, radray::format("artifact target does not match the requested decoder: requested {}", static_cast<uint32_t>(options.Target))};
    }
    DiagnosticResult<ShaderArtifactView> view = DecodeShaderArtifact(blob, options);
    if (!view.Succeeded()) {
        return {std::nullopt, std::move(view.Diagnostics)};
    }
    return {SpirvShaderArtifactView{std::move(*view.Value)}, std::move(view.Diagnostics)};
}

}  // namespace radray::shader
