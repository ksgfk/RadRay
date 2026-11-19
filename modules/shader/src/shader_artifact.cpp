#include <radray/shader/shader_artifact.h>
#include <radray/gpu/types.h>
#include "record_validation.h"
#include <radray/shader/shader_compiler_contract.h>

#include <cstring>
#include <iterator>
#include <utility>

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

// Placement legality mirrors the compiler's 2122 check, so a hand-built or tampered artifact
// cannot ask a backend to do something the policy frontend would have rejected.
bool IsValidPlacement(
    const ShaderArtifactBindingRecord& binding,
    gpu::BindingKind kind,
    ShaderTarget target,
    size_t samplerCount) noexcept {
    switch (static_cast<ShaderBindingPlacement>(binding.Placement)) {
        case ShaderBindingPlacement::Table:
            break;
        case ShaderBindingPlacement::RootDescriptor:
            // A root descriptor carries one GPU address, and the public dynamic-offset shape has
            // no array element, so count > 1 has nowhere to land.
            if (!gpu::CanBeRootDescriptor(kind) || binding.Count != 1) {
                return false;
            }
            break;
        case ShaderBindingPlacement::StaticSampler:
            // D3D-only shape: the slot lives in the serialized carrier and owns no table entry.
            if (kind != gpu::BindingKind::Sampler || target != ShaderTarget::DXIL) {
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
    return kind == gpu::BindingKind::Sampler &&
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
        if (!detail::IsArtifactName(result, entry.Name) ||
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
        const auto kind = DecodeBindingKind(binding.Type);
        const std::optional<std::string_view> bindingName = result.GetName(binding.Name);
        if (!detail::IsArtifactName(result, binding.Name) ||
            !bindingName.has_value() || binding.Group > 0xffffu || binding.Count == 0 ||
            binding.StageMask == 0 || (binding.StageMask & ~kStageMask) != 0 ||
            !kind) {
            return {std::nullopt, radray::format("binding record {} has an invalid name, kind, count, stage mask, or group", index)};
        }
        if (!IsValidPlacement(binding, *kind, static_cast<ShaderTarget>(envelope.Target),
                              result._samplers.size())) {
            return {std::nullopt, radray::format("binding record {} ({}) has an invalid target placement", index, *bindingName)};
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const ShaderArtifactBindingRecord& old = result._bindings[previous];
            const std::optional<std::string_view> oldName = result.GetName(old.Name);
            const bool sameTargetCoordinate = envelope.Target == static_cast<uint8_t>(ShaderTarget::SPIRV)
                                                  ? old.Group == binding.Group && old.Binding == binding.Binding
                                                  : old.Group == binding.Group && old.Binding == binding.Binding &&
                                                        gpu::GetBindingRegisterClass(*DecodeBindingKind(old.Type)) ==
                                                            gpu::GetBindingRegisterClass(*kind);
            if ((oldName.has_value() && oldName == bindingName) || sameTargetCoordinate) {
                return {std::nullopt, radray::format("duplicate binding name or target coordinate at record {} ({})", index, *bindingName)};
            }
        }
    }

    if (auto issue = detail::ValidateTypes(result)) {
        return {std::nullopt, radray::format("type record {}: {}", issue->Index, issue->Reason)};
    }
    for (const ShaderArtifactBindingRecord& binding : result._bindings) {
        const bool cbuffer = DecodeBindingKind(binding.Type) == gpu::BindingKind::CBuffer;
        if ((cbuffer && !detail::IsRoot(result.Types(), binding.TypeIndex)) ||
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
            !detail::IsArtifactName(result, constant.Name) || !name.has_value()) {
            return {std::nullopt, radray::format("root constant record {} has an invalid name, range, type owner, or duplicate declaration", index)};
        }
        if (constant.TypeIndex != kShaderNoType &&
            (!detail::IsRoot(result.Types(), constant.TypeIndex) ||
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
    if (auto issue = detail::ValidateVertexInputs(result)) {
        return {std::nullopt, radray::format("vertex input record {}: {}", issue->Index, issue->Reason)};
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

const WireMetadataEnvelope& ShaderArtifactView::Envelope() const noexcept {
    return _envelope;
}

std::span<const ShaderArtifactEntryRecord> ShaderArtifactView::Entries() const noexcept {
    return _entries;
}

std::span<const ShaderArtifactBindingRecord> ShaderArtifactView::Bindings() const noexcept {
    return _bindings;
}

std::span<const ShaderArtifactTypeRecord> ShaderArtifactView::Types() const noexcept {
    return _types;
}

std::span<const ShaderArtifactRootConstantRecord> ShaderArtifactView::RootConstants() const noexcept {
    return _rootConstants;
}

std::span<const ShaderArtifactVertexInputRecord> ShaderArtifactView::VertexInputs() const noexcept {
    return _vertexInputs;
}

std::span<const ShaderArtifactSamplerRecord> ShaderArtifactView::Samplers() const noexcept {
    return _samplers;
}

const ShaderArtifactView& DxilShaderArtifactView::Generic() const noexcept {
    return _view;
}

DxilShaderArtifactView::DxilShaderArtifactView(ShaderArtifactView view) noexcept
    : _view(std::move(view)) {}

const ShaderArtifactView& SpirvShaderArtifactView::Generic() const noexcept {
    return _view;
}

SpirvShaderArtifactView::SpirvShaderArtifactView(ShaderArtifactView view) noexcept
    : _view(std::move(view)) {}

}  // namespace radray::shader
