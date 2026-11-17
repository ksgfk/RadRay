#include "record_validation.h"

#include <algorithm>
#include <radray/enum_flags.h>
#include <radray/shader/shader_artifact.h>
#include <radray/shader/shader_layout.h>

namespace radray::shader::detail {
namespace {

std::optional<uint32_t> Index(std::optional<uint32_t> index) noexcept { return index; }
std::optional<uint32_t> Index(uint32_t index) noexcept {
    return index == kShaderNoType ? std::nullopt : std::optional{index};
}
bool RowMajor(const ShaderTypeRecord& t) noexcept { return t.RowMajor; }
bool RowMajor(const ShaderArtifactTypeRecord& t) noexcept { return (t.Flags & kShaderTypeFlagRowMajor) != 0; }

template <typename T>
bool RootAt(std::span<const T> types, std::optional<uint32_t> index) noexcept {
    return index && *index < types.size() && !Index(types[*index].ParentIndex) &&
           static_cast<ShaderTypeKind>(types[*index].Kind) == ShaderTypeKind::Struct;
}

template <typename T, typename NameOf>
std::optional<RecordIssue> CheckTypes(std::span<const T> types, NameOf nameOf) {
    vector<bool> resolved(types.size(), false);
    size_t remaining = 0;
    for (size_t i = 0; i < types.size(); ++i) {
        const auto& t = types[i];
        const auto parent = Index(t.ParentIndex);
        const auto underlying = Index(t.TypeIndex);
        const auto kind = static_cast<ShaderTypeKind>(t.Kind);
        const auto element = static_cast<ShaderTypeKind>(t.ElementKind);
        const auto scalar = static_cast<ShaderScalarKind>(t.ScalarKind);
        const bool root = !parent;
        const bool array = kind == ShaderTypeKind::Array;
        const bool structure = element == ShaderTypeKind::Struct;
        if (nameOf(t.Name).empty() || !EnumContains(kind) || !EnumContains(element) ||
            kind == ShaderTypeKind::Member || element == ShaderTypeKind::Member ||
            element == ShaderTypeKind::Array || !EnumContains(scalar) ||
            !t.Size || !t.ElementSize || !t.ElementCount)
            return RecordIssue{i, "invalid type name, kind or size"};
        if (array ? (t.Stride < t.ElementSize || uint64_t{t.ElementCount - 1} * t.Stride + t.ElementSize != t.Size)
                  : (kind != element || t.Stride != 0 || t.ElementCount != 1 || t.Size != t.ElementSize))
            return RecordIssue{i, "inconsistent array size or type stride"};
        if (root) {
            if (kind != ShaderTypeKind::Struct || underlying || t.Offset)
                return RecordIssue{i, "invalid root struct"};
            ++remaining;
        } else if (!RootAt(types, parent) || t.Offset > types[*parent].Size || t.Size > types[*parent].Size - t.Offset) {
            return RecordIssue{i, "invalid parent or member range"};
        }
        if (structure) {
            if (scalar != ShaderScalarKind::None || t.RowCount || t.ColumnCount || RowMajor(t) || t.MatrixStride ||
                (!root && (!RootAt(types, underlying) || t.ElementSize != types[*underlying].Size)))
                return RecordIssue{i, "invalid struct reference or shape"};
        } else {
            if (underlying || scalar == ShaderScalarKind::None)
                return RecordIssue{i, "invalid leaf type reference or scalar kind"};
            if (element == ShaderTypeKind::Matrix) {
                const auto major = RowMajor(t) ? t.RowCount : t.ColumnCount;
                const auto minor = RowMajor(t) ? t.ColumnCount : t.RowCount;
                if (scalar != ShaderScalarKind::Float || t.RowCount < 2 || t.RowCount > 4 || t.ColumnCount < 2 || t.ColumnCount > 4 ||
                    t.MatrixStride < minor * 4 || t.MatrixStride % 4 || uint64_t{major - 1} * t.MatrixStride + minor * 4 != t.ElementSize)
                    return RecordIssue{i, "invalid matrix layout"};
            } else if (t.RowCount != 1 || t.ColumnCount < 1 || t.ColumnCount > 4 ||
                       (element == ShaderTypeKind::Scalar && t.ColumnCount != 1) ||
                       t.ElementSize != t.ColumnCount * 4 || RowMajor(t) || t.MatrixStride) {
                return RecordIssue{i, "invalid scalar or vector layout"};
            }
        }
        if (!root)
            for (size_t j = 0; j < i; ++j) {
                const auto& old = types[j];
                if (Index(old.ParentIndex) != parent) continue;
                if (nameOf(old.Name) == nameOf(t.Name)) return RecordIssue{i, "duplicate sibling name"};
                if (uint64_t{old.Offset} < uint64_t{t.Offset} + t.Size && uint64_t{t.Offset} < uint64_t{old.Offset} + old.Size)
                    return RecordIssue{i, "overlaps sibling range"};
            }
    }
    while (remaining) {
        std::optional<size_t> blockedRoot;
        bool progress = false;
        for (size_t i = 0; i < types.size(); ++i) {
            if (Index(types[i].ParentIndex) || resolved[i]) continue;
            bool ready = true;
            uint64_t size = 0;
            for (const auto& child : types)
                if (Index(child.ParentIndex) == i) {
                    size = std::max(size, uint64_t{child.Offset} + child.Size);
                    if (auto dependency = Index(child.TypeIndex); dependency && !resolved[*dependency]) ready = false;
                }
            if (!ready) {
                blockedRoot = i;
                continue;
            }
            if (size != types[i].Size) return RecordIssue{i, "struct span differs from its members"};
            resolved[i] = true;
            --remaining;
            progress = true;
        }
        if (!progress) return RecordIssue{*blockedRoot, "cyclic struct references"};
    }
    return std::nullopt;
}

struct VertexSignatureView {
    std::string_view Semantic;
    uint32_t SemanticIndex;
    uint32_t Location;
    ShaderVertexComponentType Kind;
    uint32_t Count;
};

template <typename T, typename Read>
std::optional<RecordIssue> CheckVertexInputs(std::span<const T> inputs, Read read) {
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto input = read(inputs[i]);
        if (input.Semantic.empty() || !EnumContains(input.Kind) || input.Count < 1 || input.Count > 4)
            return RecordIssue{i, "invalid semantic or component shape"};
        for (size_t j = 0; j < i; ++j) {
            const auto previous = read(inputs[j]);
            if (previous.Location == input.Location ||
                (previous.Semantic == input.Semantic && previous.SemanticIndex == input.SemanticIndex))
                return RecordIssue{i, "duplicate vertex semantic or location"};
        }
    }
    return std::nullopt;
}

}  // namespace

bool IsRoot(std::span<const ShaderTypeRecord> types, std::optional<uint32_t> index) noexcept { return RootAt(types, index); }
bool IsRoot(std::span<const ShaderArtifactTypeRecord> types, uint32_t index) noexcept { return RootAt(types, Index(index)); }

bool IsArtifactName(const ShaderArtifactView& artifact, WireBlobRange range) noexcept {
    const uint32_t bytecodeOffset = artifact.Envelope().Bytecode.Offset;
    return range.Size != 0 && range.IsWithin(static_cast<uint32_t>(artifact.Envelope().TotalSize)) &&
           range.Offset >= artifact.Envelope().HeaderSize &&
           range.Offset <= bytecodeOffset && range.Size <= bytecodeOffset - range.Offset;
}

std::optional<RecordIssue> ValidateTypes(const ShaderArtifactView& artifact) {
    const auto types = artifact.Types();
    for (size_t i = 0; i < types.size(); ++i) {
        const auto& t = types[i];
        if (!IsArtifactName(artifact, t.Name) || t.Kind < 1 || t.Kind > 5 || t.ElementKind < 1 ||
            t.ElementKind > 4 || t.ScalarKind > 4 || (t.Flags & ~kShaderTypeFlagMask))
            return RecordIssue{i, "invalid shape or name"};
    }
    return CheckTypes(types, [&](WireBlobRange range) { return *artifact.GetName(range); });
}

std::optional<RecordIssue> ValidateTypes(std::span<const ShaderTypeRecord> types) {
    return CheckTypes(types, [](const string& name) -> std::string_view { return name; });
}

std::optional<RecordIssue> ValidateVertexInputs(const ShaderArtifactView& artifact) {
    const auto inputs = artifact.VertexInputs();
    for (size_t i = 0; i < inputs.size(); ++i)
        if (!IsArtifactName(artifact, inputs[i].Semantic)) return RecordIssue{i, "invalid name range"};
    return CheckVertexInputs(inputs, [&](const ShaderArtifactVertexInputRecord& input) {
        return VertexSignatureView{*artifact.GetName(input.Semantic), input.SemanticIndex, input.Location,
                                   static_cast<ShaderVertexComponentType>(input.ComponentType), input.ComponentCount};
    });
}

std::optional<RecordIssue> ValidateVertexInputs(std::span<const ShaderVertexAttributeSignatureDesc> inputs) {
    return CheckVertexInputs(inputs, [](const ShaderVertexAttributeSignatureDesc& input) {
        return VertexSignatureView{input.Semantic, input.SemanticIndex, input.Location, input.ComponentKind, input.ComponentCount};
    });
}

}  // namespace radray::shader::detail
