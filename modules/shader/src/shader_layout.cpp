#include <radray/shader/shader_layout.h>
#include "record_validation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

#include <radray/binary_io.h>
#include <radray/json.h>
#include <radray/logger.h>

namespace radray::shader {
namespace {

using detail::IsRoot;

bool LayoutError(Nullable<string*> diagnostic, std::string_view message) {
    if (diagnostic) diagnostic.Get()->assign(message);
    return false;
}

template <typename T>
bool Active(const T& value, gpu::RenderBackend backend) noexcept {
    return backend == gpu::RenderBackend::D3D12 ? value.D3D12.has_value() : backend == gpu::RenderBackend::Vulkan && value.Vulkan.has_value();
}

}  // namespace

bool ValidateShaderTypeRecords(std::span<const ShaderTypeRecord> types, Nullable<string*> diagnostic) {
    if (auto issue = detail::ValidateTypes(types)) {
        if (diagnostic) *diagnostic.Get() = radray::format("type record {}: {}", issue->Index, issue->Reason);
        return false;
    }
    if (diagnostic) diagnostic.Get()->clear();
    return true;
}

uint32_t ShaderResolvedMember::VectorPayloadSize() const noexcept {
    if (ElementKind == ShaderTypeKind::Matrix) return (RowMajor ? ColumnCount : RowCount) * 4;
    return ElementKind == ShaderTypeKind::Scalar || ElementKind == ShaderTypeKind::Vector ? ColumnCount * 4 : 0;
}
bool ShaderResolvedMember::HasInternalPadding() const noexcept { return _hasInternalPadding; }

std::optional<ShaderResolvedMember> FindShaderTypeMember(std::span<const ShaderTypeRecord> types, uint32_t rootIndex, std::string_view path) {
    if (!detail::IsRoot(types, rootIndex) || path.empty()) return std::nullopt;
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

bool ValidateShaderPipelineLayoutDesc(const ShaderPipelineLayoutDesc& desc, Nullable<string*> diagnostic) {
    if (!ValidateShaderTypeRecords(desc.Types, diagnostic)) return false;
    unordered_set<string> names;
    unordered_set<string> groupNames;
    unordered_set<uint32_t> slots;
    unordered_set<uint32_t> sets;
    vector<const gpu::BindingDescriptor*> bindings;
    for (const auto& group : desc.Groups) {
        if (group.Name.empty() || !groupNames.insert(group.Name).second || !slots.insert(group.GroupSlot).second)
            return LayoutError(diagnostic, "invalid or duplicate group name/slot");
        unordered_set<uint32_t> ids;
        std::optional<uint32_t> set;
        for (const auto& entry : group.Bindings) {
            const auto& b = entry.Binding;
            if (entry.Name.empty() || !names.insert(entry.Name).second || !ids.insert(b.BindingId).second || (!b.D3D12 && !b.Vulkan))
                return LayoutError(diagnostic, "invalid or duplicate binding");
            if (b.D3D12 && !gpu::IsValidBindingMapping(b, gpu::RenderBackend::D3D12))
                return LayoutError(diagnostic, "invalid D3D12 binding mapping");
            if (b.Vulkan) {
                if (!gpu::IsValidBindingMapping(b, gpu::RenderBackend::Vulkan) || (set && *set != b.Vulkan->Set))
                    return LayoutError(diagnostic, "invalid Vulkan set or binding placement");
                set = b.Vulkan->Set;
            }
            if (b.ImmutableSampler && !gpu::IsValidSamplerDescriptor(*b.ImmutableSampler))
                return LayoutError(diagnostic, "invalid immutable sampler");
            if (b.Kind == gpu::BindingKind::CBuffer) {
                if (!IsRoot(desc.Types, entry.TypeIndex) || !entry.BufferByteSize || entry.BufferByteSize % 16 || entry.BufferByteSize < desc.Types[*entry.TypeIndex].Size)
                    return LayoutError(diagnostic, "invalid constant buffer type or byte size");
            } else if (entry.TypeIndex || entry.BufferByteSize) {
                return LayoutError(diagnostic, "non-constant binding cannot own a constant block");
            }
            for (const auto* old : bindings) {
                if (gpu::BindingMappingsOverlap(*old, b, gpu::RenderBackend::D3D12))
                    return LayoutError(diagnostic, "overlapping D3D12 registers");
                if (gpu::BindingMappingsOverlap(*old, b, gpu::RenderBackend::Vulkan))
                    return LayoutError(diagnostic, "duplicate Vulkan binding");
            }
            bindings.push_back(&b);
        }
        if (set && !sets.insert(*set).second) return LayoutError(diagnostic, "Vulkan set belongs to multiple groups");
    }
    unordered_set<uint32_t> pushIds;
    bool hasVkPush = false;
    for (size_t i = 0; i < desc.PushConstants.size(); ++i) {
        const auto& entry = desc.PushConstants[i];
        const auto& p = entry.Push;
        if (entry.Name.empty() || !names.insert(entry.Name).second || !pushIds.insert(p.PushId).second || (p.D3D12 && !gpu::IsValidPushConstantRange(p, gpu::RenderBackend::D3D12)) ||
            (p.Vulkan && !gpu::IsValidPushConstantRange(p, gpu::RenderBackend::Vulkan)) || (!p.D3D12 && !p.Vulkan) || (entry.TypeIndex && (!IsRoot(desc.Types, entry.TypeIndex) || desc.Types[*entry.TypeIndex].Size > p.Size)))
            return LayoutError(diagnostic, "invalid push constant");
        if (p.Vulkan) {
            if (hasVkPush)
                return LayoutError(diagnostic, "multiple or invalid Vulkan push ranges");
            hasVkPush = true;
        }
        if (p.D3D12) {
            for (const auto* b : bindings)
                if (b->D3D12 && b->D3D12->RegisterType == gpu::BindingRegisterClass::CBV && b->D3D12->Space == p.D3D12->Space &&
                    p.D3D12->Register >= b->D3D12->Register && uint64_t{p.D3D12->Register} < uint64_t{b->D3D12->Register} + b->Count)
                    return LayoutError(diagnostic, "push constant overlaps a constant buffer register");
            for (size_t j = 0; j < i; ++j)
                if (desc.PushConstants[j].Push.D3D12 == p.D3D12)
                    return LayoutError(diagnostic, "duplicate D3D12 push register");
        }
    }
    return true;
}

Nullable<const ShaderVariantTargetInterfaceDesc*> GetShaderTargetInterface(const ShaderVariantInterfaceDesc& desc, gpu::RenderBackend backend) noexcept {
    if (backend == gpu::RenderBackend::D3D12 && desc.DXIL) return &*desc.DXIL;
    if (backend == gpu::RenderBackend::Vulkan && desc.SPIRV) return &*desc.SPIRV;
    return nullptr;
}

bool ValidateShaderVariantInterfaceDesc(const ShaderVariantInterfaceDesc& desc, Nullable<const ShaderPipelineLayoutDesc*> layout, Nullable<string*> diagnostic) {
    if (diagnostic) diagnostic.Get()->clear();
    if (desc.Name.empty() || (!desc.DXIL && !desc.SPIRV)) return LayoutError(diagnostic, "program needs a name and target interface");
    for (auto backend : {gpu::RenderBackend::D3D12, gpu::RenderBackend::Vulkan}) {
        const auto target = GetShaderTargetInterface(desc, backend);
        if (!target) continue;
        const auto& t = *target.Get();
        if (!gpu::IsValidShaderStages(t.Stages) || (!t.Stages.HasFlag(gpu::ShaderStage::Vertex) && !t.VertexInputs.empty()))
            return LayoutError(diagnostic, "invalid program stages or vertex inputs");
        if (auto issue = detail::ValidateVertexInputs(t.VertexInputs)) {
            if (diagnostic) *diagnostic.Get() = radray::format("vertex input record {}: {}", issue->Index, issue->Reason);
            return false;
        }
        for (size_t i = 0; i < t.ActiveBindings.size(); ++i) {
            const auto& ref = t.ActiveBindings[i];
            for (size_t j = 0; j < i; ++j)
                if (t.ActiveBindings[j].GroupSlot == ref.GroupSlot && t.ActiveBindings[j].BindingId == ref.BindingId)
                    return LayoutError(diagnostic, "duplicate active binding");
            if (layout) {
                bool found = false;
                for (const auto& group : layout.Get()->Groups)
                    if (group.GroupSlot == ref.GroupSlot)
                        for (const auto& entry : group.Bindings)
                            if (entry.Binding.BindingId == ref.BindingId && Active(entry.Binding, backend) && (entry.Binding.Stages & t.Stages)) found = true;
                if (!found) return LayoutError(diagnostic, "active binding is absent from target layout");
            }
        }
        for (size_t i = 0; i < t.ActivePushConstants.size(); ++i) {
            const auto id = t.ActivePushConstants[i];
            if (std::find(t.ActivePushConstants.begin(), t.ActivePushConstants.begin() + i, id) != t.ActivePushConstants.begin() + i)
                return LayoutError(diagnostic, "duplicate active push constant");
            if (layout) {
                bool found = false;
                for (const auto& entry : layout.Get()->PushConstants)
                    if (entry.Push.PushId == id && Active(entry.Push, backend) && (entry.Push.Stages & t.Stages)) found = true;
                if (!found) return LayoutError(diagnostic, "active push constant is absent from target layout");
            }
        }
    }
    return true;
}

bool ValidateShaderFamilyLayoutDesc(const ShaderFamilyLayoutDesc& desc, Nullable<string*> diagnostic) {
    if (!ValidateShaderPipelineLayoutDesc(desc.SharedLayout, diagnostic)) return false;
    unordered_set<string> names;
    for (const auto& variant : desc.Variants) {
        if (!names.insert(variant.Name).second) return LayoutError(diagnostic, "duplicate variant name");
        if (!ValidateShaderVariantInterfaceDesc(variant, &desc.SharedLayout, diagnostic)) return false;
    }
    return true;
}

namespace {

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, shader::ShaderTypeRecord>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Name", v.Name) &&
           f("ParentIndex", v.ParentIndex) &&
           f("Kind", v.Kind) &&
           f("ElementKind", v.ElementKind) &&
           f("ElementCount", v.ElementCount) &&
           f("Offset", v.Offset) &&
           f("Size", v.Size) &&
           f("ElementSize", v.ElementSize) &&
           f("Stride", v.Stride) &&
           f("MatrixStride", v.MatrixStride) &&
           f("TypeIndex", v.TypeIndex) &&
           f("ScalarKind", v.ScalarKind) &&
           f("RowCount", v.RowCount) &&
           f("ColumnCount", v.ColumnCount) &&
           f("RowMajor", v.RowMajor);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, gpu::D3D12BindingDescriptor>
bool VisitLayoutFields(T& v, F&& f) {
    return f("RegisterType", v.RegisterType) &&
           f("Register", v.Register) &&
           f("Space", v.Space) &&
           f("Placement", v.Placement);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, gpu::VulkanBindingDescriptor>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Set", v.Set) &&
           f("Binding", v.Binding) &&
           f("Placement", v.Placement);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, gpu::SamplerDescriptor>
bool VisitLayoutFields(T& v, F&& f) {
    return f("AddressS", v.AddressS) &&
           f("AddressT", v.AddressT) &&
           f("AddressR", v.AddressR) &&
           f("MinFilter", v.MinFilter) &&
           f("MagFilter", v.MagFilter) &&
           f("MipmapFilter", v.MipmapFilter) &&
           f("LodMin", v.LodMin) &&
           f("LodMax", v.LodMax) &&
           f("Compare", v.Compare) &&
           f("AnisotropyClamp", v.AnisotropyClamp) &&
           f("MipLodBias", v.MipLodBias) &&
           f("BorderColor", v.BorderColor) &&
           f("Reduction", v.Reduction) &&
           f("UnnormalizedCoordinates", v.UnnormalizedCoordinates);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, gpu::BindingDescriptor>
bool VisitLayoutFields(T& v, F&& f) {
    return f("BindingId", v.BindingId) &&
           f("Kind", v.Kind) &&
           f("Count", v.Count) &&
           f("Stages", v.Stages) &&
           f("D3D12", v.D3D12) &&
           f("Vulkan", v.Vulkan) &&
           f("ImmutableSampler", v.ImmutableSampler);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, gpu::PushConstantDescriptor::D3D12Location>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Register", v.Register) &&
           f("Space", v.Space);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, gpu::PushConstantDescriptor::VulkanLocation>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Offset", v.Offset);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, gpu::PushConstantDescriptor>
bool VisitLayoutFields(T& v, F&& f) {
    return f("PushId", v.PushId) &&
           f("Size", v.Size) &&
           f("Stages", v.Stages) &&
           f("D3D12", v.D3D12) &&
           f("Vulkan", v.Vulkan);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderBindingDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Name", v.Name) &&
           f("Binding", v.Binding) &&
           f("TypeIndex", v.TypeIndex) &&
           f("BufferByteSize", v.BufferByteSize);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderBindingGroupDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Name", v.Name) &&
           f("GroupSlot", v.GroupSlot) &&
           f("Bindings", v.Bindings);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderPushConstantDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Name", v.Name) &&
           f("Push", v.Push) &&
           f("TypeIndex", v.TypeIndex);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderPipelineLayoutDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Groups", v.Groups) &&
           f("PushConstants", v.PushConstants) &&
           f("Types", v.Types);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderVertexAttributeSignatureDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Semantic", v.Semantic) &&
           f("SemanticIndex", v.SemanticIndex) &&
           f("Location", v.Location) &&
           f("ComponentKind", v.ComponentKind) &&
           f("ComponentCount", v.ComponentCount);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderBindingReference>
bool VisitLayoutFields(T& v, F&& f) {
    return f("GroupSlot", v.GroupSlot) &&
           f("BindingId", v.BindingId);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderVariantTargetInterfaceDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Stages", v.Stages) &&
           f("VertexInputs", v.VertexInputs) &&
           f("ActiveBindings", v.ActiveBindings) &&
           f("ActivePushConstants", v.ActivePushConstants);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderVariantInterfaceDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Name", v.Name) &&
           f("DXIL", v.DXIL) &&
           f("SPIRV", v.SPIRV);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderFamilyLayoutDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("SharedLayout", v.SharedLayout) &&
           f("Variants", v.Variants);
}

template <typename T>
struct LayoutOptional;
template <typename T>
struct LayoutOptional<std::optional<T>> {
    using Element = T;
};
template <typename T>
struct LayoutVector;
template <typename T>
struct LayoutVector<vector<T>> {
    using Element = T;
};

template <typename T>
void EncodeLayoutValue(BinaryWriter& writer, const T& value) {
    if constexpr (std::same_as<T, string>)
        writer.String(value);
    else if constexpr (std::same_as<T, bool>)
        writer.Bool(value);
    else if constexpr (std::same_as<T, float>)
        writer.Float(value);
    else if constexpr (std::same_as<T, gpu::ShaderStages>)
        writer.U32(value.value());
    else if constexpr (std::same_as<T, shader::ShaderVertexComponentType>)
        writer.U32(static_cast<uint32_t>(value) - 1);
    else if constexpr (std::is_enum_v<T> || std::same_as<T, uint32_t>)
        writer.U32(static_cast<uint32_t>(value));
    else if constexpr (requires { typename LayoutOptional<T>::Element; }) {
        writer.Bool(value.has_value());
        if (value) EncodeLayoutValue(writer, *value);
    } else if constexpr (requires { typename LayoutVector<T>::Element; }) {
        writer.Size32(value.size());
        for (const auto& item : value) EncodeLayoutValue(writer, item);
    } else {
        VisitLayoutFields(value, [&](std::string_view, const auto& field) { EncodeLayoutValue(writer, field); return true; });
    }
}

template <typename T>
size_t MinimumLayoutBytes() {
    if constexpr (std::same_as<T, bool> || requires { typename LayoutOptional<T>::Element; })
        return 1;
    else if constexpr (std::same_as<T, string> || std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                       std::same_as<T, gpu::ShaderStages> || requires { typename LayoutVector<T>::Element; })
        return 4;
    else {
        T value{};
        size_t result = 0;
        VisitLayoutFields(value, [&](std::string_view, const auto& field) { result += MinimumLayoutBytes<std::remove_cvref_t<decltype(field)>>(); return true; });
        return result;
    }
}

template <typename T>
bool DecodeLayoutValue(BinaryReader& reader, T& value) {
    if constexpr (std::same_as<T, string>) {
        std::string_view text;
        if (!reader.String(text)) return false;
        value.assign(text);
        return true;
    } else if constexpr (std::same_as<T, bool>)
        return reader.Bool(value);
    else if constexpr (std::same_as<T, float>)
        return reader.Float(value) && std::isfinite(value);
    else if constexpr (std::same_as<T, shader::ShaderVertexComponentType>) {
        uint32_t raw = 0;
        if (!reader.U32(raw) || raw > 2) return false;
        // Layout version 1 uses Float=0, Sint=1, Uint=2; the shader wire enum reserves zero.
        value = static_cast<T>(raw + 1);
        return true;
    } else if constexpr (std::is_enum_v<T> || std::same_as<T, uint32_t> || std::same_as<T, gpu::ShaderStages>) {
        uint32_t raw = 0;
        if (!reader.U32(raw)) return false;
        value = static_cast<T>(raw);
        return true;
    } else if constexpr (requires { typename LayoutOptional<T>::Element; }) {
        bool present = false;
        if (!reader.Bool(present)) return false;
        if (!present) {
            value.reset();
            return true;
        }
        value.emplace();
        return DecodeLayoutValue(reader, *value);
    } else if constexpr (requires { typename LayoutVector<T>::Element; }) {
        uint32_t count = 0;
        if (!reader.U32(count) || count > reader.Remaining() / MinimumLayoutBytes<typename T::value_type>()) return false;
        value.resize(count);
        for (auto& item : value)
            if (!DecodeLayoutValue(reader, item)) return false;
        return true;
    } else
        return VisitLayoutFields(value, [&](std::string_view, auto& field) { return DecodeLayoutValue(reader, field); });
}

template <typename T>
std::string_view LayoutEnumName(T value) {
    if constexpr (std::same_as<T, shader::ShaderScalarKind> || std::same_as<T, shader::ShaderVertexComponentType>) {
        if (value == T::SignedInteger) return "Sint";
        if (value == T::UnsignedInteger) return "Uint";
    }
    return EnumName(value);
}

// Keep v1 field encodings local to layout files when using the core JSON writer.
template <typename T>
struct LayoutJson {
    const T& Value;
};

template <typename T>
bool WriteLayoutJsonValue(JsonWriteContext& context, const T& value) noexcept;

}  // namespace
}  // namespace radray::shader

namespace radray {
template <typename T>
struct JsonSerializer<shader::LayoutJson<T>> {
    static bool Write(JsonWriteContext& context, const shader::LayoutJson<T>& value) noexcept {
        return shader::WriteLayoutJsonValue(context, value.Value);
    }
};
}  // namespace radray

namespace radray::shader {
namespace {

template <typename T>
bool WriteLayoutJsonValue(JsonWriteContext& context, const T& value) noexcept {
    if constexpr (std::same_as<T, gpu::ShaderStages>) {
        return SerializeJsonValue(context, value.value());
    } else if constexpr (std::same_as<T, ShaderScalarKind> || std::same_as<T, ShaderVertexComponentType>) {
        return context.String(LayoutEnumName(value));
    } else if constexpr (json_serializable<T>) {
        return SerializeJsonValue(context, value);
    } else if constexpr (requires { typename LayoutVector<T>::Element; }) {
        auto array = context.BeginArray();
        if (!array.IsValid()) return false;
        for (const auto& item : value)
            if (!array.Element(LayoutJson{item})) return false;
        return true;
    } else {
        auto object = context.BeginObject();
        return object.IsValid() && VisitLayoutFields(value, [&](std::string_view name, const auto& field) {
            using Field = std::remove_cvref_t<decltype(field)>;
            if constexpr (requires { typename LayoutOptional<Field>::Element; })
                return !field || object.Member(name, LayoutJson{*field});
            else
                return object.Member(name, LayoutJson{field});
        });
    }
}

template <typename T>
bool ReadLayoutJsonValue(JsonValue input, T& value) {
    if constexpr (requires { typename LayoutOptional<T>::Element; }) {
        if (!input.IsValid() || input.IsNull()) {
            value.reset();
            return true;
        }
        value.emplace();
        return ReadLayoutJsonValue(input, *value);
    } else if constexpr (requires { typename LayoutVector<T>::Element; }) {
        if (!input.IsArray()) return false;
        value.resize(input.Size());
        for (size_t i = 0; i < value.size(); ++i)
            if (!ReadLayoutJsonValue(input.At(i), value[i])) return false;
        return true;
    } else if constexpr (std::same_as<T, gpu::ShaderStages>) {
        uint32_t raw = 0;
        if (!DeserializeJsonValue(input, raw)) return false;
        value = gpu::ShaderStages{raw};
        return true;
    } else if constexpr (std::same_as<T, shader::ShaderScalarKind> || std::same_as<T, shader::ShaderVertexComponentType>) {
        const auto name = input.AsString();
        if (name == "Sint")
            value = T::SignedInteger;
        else if (name == "Uint")
            value = T::UnsignedInteger;
        else if (name == "Float")
            value = T::Float;
        else if constexpr (std::same_as<T, shader::ShaderScalarKind>) {
            if (name == "None")
                value = T::None;
            else if (name == "Bool")
                value = T::Bool;
            else
                return false;
        } else
            return false;
        return true;
    } else if constexpr (std::same_as<T, string> || std::is_arithmetic_v<T> || std::is_enum_v<T>)
        return DeserializeJsonValue(input, value);
    else
        return input.IsObject() && VisitLayoutFields(value, [&](std::string_view name, auto& field) { return ReadLayoutJsonValue(input[name], field); });
}

template <typename T, typename Validate>
std::optional<vector<byte>> EncodeLayout(const T& value, uint32_t kind, Validate validate) {
    if (!validate(value)) return std::nullopt;
    BinaryWriter writer;
    writer.U32(0x4c535252u);
    writer.U32(1);
    writer.U32(kind);
    EncodeLayoutValue(writer, value);
    return std::move(writer).TakeData();
}

template <typename T, typename Validate>
bool DecodeLayout(std::span<const byte> bytes, T& output, uint32_t kind, Validate validate) {
    BinaryReader reader{bytes};
    uint32_t magic = 0, version = 0, actualKind = 0;
    if (!reader.U32(magic) || magic != 0x4c535252u || !reader.U32(version) || version != 1 || !reader.U32(actualKind) || actualKind != kind) return false;
    T candidate;
    if (!DecodeLayoutValue(reader, candidate) || !reader.AtEnd() || !validate(candidate)) return false;
    output = std::move(candidate);
    return true;
}

template <typename T, typename Validate>
std::optional<string> EncodeLayoutJson(const T& value, std::string_view kind, Validate validate) {
    if (!validate(value)) return std::nullopt;
    JsonWriter writer;
    JsonWriteContext context{writer};
    auto root = context.BeginObject();
    if (!root.Member("Version", 1u) || !root.Member("Kind", kind) ||
        !root.Member("Data", LayoutJson{value})) return std::nullopt;
    return writer.Write();
}

template <typename T, typename Validate>
bool DecodeLayoutJson(std::string_view text, T& output, std::string_view kind, Validate validate) {
    const auto document = JsonDocument::Parse(text);
    if (!document) return false;
    const auto root = document->Root();
    uint32_t version = 0;
    if (!root.IsObject() || !DeserializeJsonValue(root["Version"], version) || version != 1) return false;
    if (root["Kind"].AsString() != kind) return false;
    T candidate;
    if (!ReadLayoutJsonValue(root["Data"], candidate) || !validate(candidate)) return false;
    output = std::move(candidate);
    return true;
}

}  // namespace

std::optional<vector<byte>> WriteShaderPipelineLayoutBinary(const ShaderPipelineLayoutDesc& desc) {
    return EncodeLayout(desc, 1, [](const auto& v) { return ValidateShaderPipelineLayoutDesc(v); });
}
bool ReadShaderPipelineLayoutBinary(std::span<const byte> bytes, ShaderPipelineLayoutDesc& output) {
    return DecodeLayout(bytes, output, 1, [](const auto& v) { return ValidateShaderPipelineLayoutDesc(v); });
}
std::optional<string> WriteShaderPipelineLayoutJson(const ShaderPipelineLayoutDesc& desc) {
    return EncodeLayoutJson(desc, "ShaderPipelineLayout", [](const auto& v) { return ValidateShaderPipelineLayoutDesc(v); });
}
bool ReadShaderPipelineLayoutJson(std::string_view text, ShaderPipelineLayoutDesc& output) {
    return DecodeLayoutJson(text, output, "ShaderPipelineLayout", [](const auto& v) { return ValidateShaderPipelineLayoutDesc(v); });
}

std::optional<vector<byte>> WriteShaderVariantInterfaceBinary(const ShaderVariantInterfaceDesc& desc) {
    return EncodeLayout(desc, 2, [](const auto& v) { return ValidateShaderVariantInterfaceDesc(v); });
}
bool ReadShaderVariantInterfaceBinary(std::span<const byte> bytes, ShaderVariantInterfaceDesc& output) {
    return DecodeLayout(bytes, output, 2, [](const auto& v) { return ValidateShaderVariantInterfaceDesc(v); });
}
std::optional<string> WriteShaderVariantInterfaceJson(const ShaderVariantInterfaceDesc& desc) {
    return EncodeLayoutJson(desc, "ShaderVariantInterface", [](const auto& v) { return ValidateShaderVariantInterfaceDesc(v); });
}
bool ReadShaderVariantInterfaceJson(std::string_view text, ShaderVariantInterfaceDesc& output) {
    return DecodeLayoutJson(text, output, "ShaderVariantInterface", [](const auto& v) { return ValidateShaderVariantInterfaceDesc(v); });
}

std::optional<vector<byte>> WriteShaderFamilyLayoutBinary(const ShaderFamilyLayoutDesc& desc) {
    return EncodeLayout(desc, 3, [](const auto& v) { return ValidateShaderFamilyLayoutDesc(v); });
}
bool ReadShaderFamilyLayoutBinary(std::span<const byte> bytes, ShaderFamilyLayoutDesc& output) {
    return DecodeLayout(bytes, output, 3, [](const auto& v) { return ValidateShaderFamilyLayoutDesc(v); });
}
std::optional<string> WriteShaderFamilyLayoutJson(const ShaderFamilyLayoutDesc& desc) {
    return EncodeLayoutJson(desc, "ShaderFamilyLayout", [](const auto& v) { return ValidateShaderFamilyLayoutDesc(v); });
}
bool ReadShaderFamilyLayoutJson(std::string_view text, ShaderFamilyLayoutDesc& output) {
    return DecodeLayoutJson(text, output, "ShaderFamilyLayout", [](const auto& v) { return ValidateShaderFamilyLayoutDesc(v); });
}

}  // namespace radray::shader
