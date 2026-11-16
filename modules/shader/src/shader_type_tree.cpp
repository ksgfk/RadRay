#include <radray/shader/shader_type_tree.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <radray/enum_flags.h>

namespace radray::shader {
namespace {
bool LayoutError(Nullable<string*> diagnostic, std::string_view message) {
    if (diagnostic) diagnostic.Get()->assign(message);
    return false;
}
bool IsRoot(std::span<const ShaderTypeRecord> types, std::optional<uint32_t> index) noexcept {
    return index && *index < types.size() && !types[*index].ParentIndex && types[*index].Kind == ShaderTypeKind::Struct;
}

}  // namespace

bool ValidateShaderTypeRecords(std::span<const ShaderTypeRecord> types, Nullable<string*> diagnostic) {
    if (diagnostic) diagnostic.Get()->clear();
    vector<bool> resolved(types.size(), false);
    size_t remaining = 0;
    for (size_t i = 0; i < types.size(); ++i) {
        const auto& t = types[i];
        const bool root = !t.ParentIndex;
        const bool array = t.Kind == ShaderTypeKind::Array;
        const bool structure = t.ElementKind == ShaderTypeKind::Struct;
        if (t.Name.empty() || !EnumContains(t.Kind) || !EnumContains(t.ElementKind) ||
            t.Kind == ShaderTypeKind::Member || t.ElementKind == ShaderTypeKind::Member ||
            t.ElementKind == ShaderTypeKind::Array || !EnumContains(t.ScalarKind) ||
            !t.Size || !t.ElementSize || !t.ElementCount)
            return LayoutError(diagnostic, "invalid type name, kind or size");
        if (array ? (t.Stride < t.ElementSize || uint64_t{t.ElementCount - 1} * t.Stride + t.ElementSize != t.Size)
                  : (t.Kind != t.ElementKind || t.Stride != 0 || t.ElementCount != 1 || t.Size != t.ElementSize))
            return LayoutError(diagnostic, "inconsistent type span or array stride");
        if (root) {
            if (t.Kind != ShaderTypeKind::Struct || t.TypeIndex || t.Offset)
                return LayoutError(diagnostic, "invalid root struct");
            ++remaining;
        } else if (!IsRoot(types, t.ParentIndex) || t.Offset > types[*t.ParentIndex].Size || t.Size > types[*t.ParentIndex].Size - t.Offset) {
            return LayoutError(diagnostic, "invalid parent or member range");
        }
        if (structure) {
            if (t.ScalarKind != ShaderScalarKind::None || t.RowCount || t.ColumnCount || t.RowMajor || t.MatrixStride ||
                (!root && (!IsRoot(types, t.TypeIndex) || t.ElementSize != types[*t.TypeIndex].Size)))
                return LayoutError(diagnostic, "invalid struct reference or shape");
        } else {
            if (t.TypeIndex || t.ScalarKind == ShaderScalarKind::None)
                return LayoutError(diagnostic, "invalid leaf type reference or scalar kind");
            if (t.ElementKind == ShaderTypeKind::Matrix) {
                const auto major = t.RowMajor ? t.RowCount : t.ColumnCount;
                const auto minor = t.RowMajor ? t.ColumnCount : t.RowCount;
                if (t.ScalarKind != ShaderScalarKind::Float || t.RowCount < 2 || t.RowCount > 4 || t.ColumnCount < 2 || t.ColumnCount > 4 ||
                    t.MatrixStride < minor * 4 || t.MatrixStride % 4 || uint64_t{major - 1} * t.MatrixStride + minor * 4 != t.ElementSize)
                    return LayoutError(diagnostic, "invalid matrix layout");
            } else if (t.RowCount != 1 || t.ColumnCount < 1 || t.ColumnCount > 4 ||
                       (t.ElementKind == ShaderTypeKind::Scalar && t.ColumnCount != 1) ||
                       t.ElementSize != t.ColumnCount * 4 || t.RowMajor || t.MatrixStride) {
                return LayoutError(diagnostic, "invalid scalar or vector layout");
            }
        }
        if (!root)
            for (size_t j = 0; j < i; ++j) {
                const auto& old = types[j];
                if (old.ParentIndex != t.ParentIndex) continue;
                if (old.Name == t.Name || (uint64_t{old.Offset} < uint64_t{t.Offset} + t.Size && uint64_t{t.Offset} < uint64_t{old.Offset} + old.Size))
                    return LayoutError(diagnostic, "duplicate member name or overlapping member ranges");
            }
    }
    while (remaining) {
        bool progress = false;
        for (size_t i = 0; i < types.size(); ++i) {
            if (types[i].ParentIndex || resolved[i]) continue;
            bool ready = true;
            uint64_t size = 0;
            for (const auto& child : types)
                if (child.ParentIndex == i) {
                    size = std::max(size, uint64_t{child.Offset} + child.Size);
                    if (child.TypeIndex && !resolved[*child.TypeIndex]) ready = false;
                }
            if (!ready) continue;
            if (size != types[i].Size) return LayoutError(diagnostic, "struct span differs from its members");
            resolved[i] = true;
            --remaining;
            progress = true;
        }
        if (!progress) return LayoutError(diagnostic, "cyclic struct references");
    }
    return true;
}

uint32_t ShaderResolvedMember::VectorPayloadSize() const noexcept {
    if (ElementKind == ShaderTypeKind::Matrix) return (RowMajor ? ColumnCount : RowCount) * 4;
    return ElementKind == ShaderTypeKind::Scalar || ElementKind == ShaderTypeKind::Vector ? ColumnCount * 4 : 0;
}
bool ShaderResolvedMember::HasInternalPadding() const noexcept { return _hasInternalPadding; }

std::optional<ShaderResolvedMember> FindShaderTypeMember(std::span<const ShaderTypeRecord> types, uint32_t rootIndex, std::string_view path) {
    if (!IsRoot(types, rootIndex) || path.empty()) return std::nullopt;
    uint64_t offset = 0;
    while (!path.empty()) {
        const size_t end = path.find_first_of(".[");
        const auto name = path.substr(0, end);
        if (name.empty()) return std::nullopt;
        const auto found = std::find_if(types.begin(), types.end(), [&](const auto& t) { return t.ParentIndex == rootIndex && t.Name == name; });
        if (found == types.end()) return std::nullopt;
        ShaderTypeRecord const& t = *found;
        offset += t.Offset;
        path.remove_prefix(name.size());
        bool indexed = false;
        if (!path.empty() && path.front() == '[') {
            const size_t close = path.find(']');
            if (t.Kind != ShaderTypeKind::Array || close == std::string_view::npos || close <= 1) return std::nullopt;
            uint32_t index = 0;
            const auto parsed = std::from_chars(path.data() + 1, path.data() + close, index);
            if (parsed.ec != std::errc{} || parsed.ptr != path.data() + close || index >= t.ElementCount) return std::nullopt;
            offset += uint64_t{index} * t.Stride;
            indexed = true;
            path.remove_prefix(close + 1);
        }
        if (offset > std::numeric_limits<uint32_t>::max()) return std::nullopt;
        if (!path.empty()) {
            if (path.front() != '.' || !t.TypeIndex || (t.Kind == ShaderTypeKind::Array && !indexed)) return std::nullopt;
            path.remove_prefix(1);
            if (path.empty()) return std::nullopt;
            rootIndex = *t.TypeIndex;
            continue;
        }
        ShaderResolvedMember result;
        result.Kind = indexed ? t.ElementKind : t.Kind;
        result.ElementKind = t.ElementKind;
        result.ScalarKind = t.ScalarKind;
        result.AbsoluteOffset = static_cast<uint32_t>(offset);
        result.Size = indexed ? t.ElementSize : t.Size;
        result.ElementSize = t.ElementSize;
        result.ElementCount = indexed ? 1 : t.ElementCount;
        result.ArrayStride = indexed ? 0 : t.Stride;
        result.MatrixStride = t.MatrixStride;
        result.RowCount = t.RowCount;
        result.ColumnCount = t.ColumnCount;
        result.RowMajor = t.RowMajor;
        result.TypeIndex = t.TypeIndex;
        vector<std::optional<uint64_t>> payload(types.size());
        for (size_t pass = 0; pass < types.size(); ++pass) {
            bool progress = false;
            for (size_t i = 0; i < types.size(); ++i) {
                if (payload[i]) continue;
                const auto& item = types[i];
                uint64_t bytes = 0;
                if (item.ElementKind != ShaderTypeKind::Struct)
                    bytes = uint64_t{item.RowCount} * item.ColumnCount * 4;
                else if (item.TypeIndex) {
                    if (!payload[*item.TypeIndex]) continue;
                    bytes = *payload[*item.TypeIndex];
                } else {
                    bool ready = true;
                    for (size_t j = 0; j < types.size(); ++j)
                        if (types[j].ParentIndex == i) {
                            if (!payload[j]) {
                                ready = false;
                                break;
                            }
                            bytes += *payload[j];
                        }
                    if (!ready) continue;
                }
                payload[i] = bytes * item.ElementCount;
                progress = true;
            }
            if (!progress) break;
        }
        const auto bytes = payload[static_cast<size_t>(found - types.begin())];
        if (!bytes) return std::nullopt;
        result._hasInternalPadding = (indexed ? *bytes / t.ElementCount : *bytes) != result.Size;
        return result;
    }
    return std::nullopt;
}

}  // namespace radray::shader
