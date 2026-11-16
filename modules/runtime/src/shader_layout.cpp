#include <radray/runtime/shader_layout.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

#include <radray/binary_io.h>
#include <radray/json.h>
#include <radray/logger.h>

namespace radray {
namespace {

bool LayoutError(Nullable<string*> diagnostic, std::string_view message) {
    if (diagnostic) diagnostic.Get()->assign(message);
    return false;
}

bool ValidStages(render::ShaderStages stages) noexcept {
    return stages.value() != 0 && (stages.value() & ~uint32_t{7}) == 0;
}

template <typename T>
bool Active(const T& value, render::RenderBackend backend) noexcept {
    return backend == render::RenderBackend::D3D12 ? value.D3D12.has_value() : backend == render::RenderBackend::Vulkan && value.Vulkan.has_value();
}

bool IsRoot(std::span<const shader::ShaderTypeRecord> types, std::optional<uint32_t> index) noexcept {
    return index && *index < types.size() && !types[*index].ParentIndex && types[*index].Kind == shader::ShaderTypeKind::Struct;
}

bool ValidSampler(const render::SamplerDescriptor& s) noexcept {
    return EnumContains(s.AddressS) && EnumContains(s.AddressT) && EnumContains(s.AddressR) &&
           EnumContains(s.MinFilter) && EnumContains(s.MagFilter) && EnumContains(s.MipmapFilter) &&
           EnumContains(s.BorderColor) && EnumContains(s.Reduction) && (!s.Compare || EnumContains(*s.Compare)) &&
           std::isfinite(s.LodMin) && std::isfinite(s.LodMax) && std::isfinite(s.MipLodBias) &&
           s.LodMin <= s.LodMax && s.AnisotropyClamp <= 16 &&
           (!s.Compare || s.Reduction == render::SamplerReductionMode::WeightedAverage) &&
           (!s.UnnormalizedCoordinates ||
            (s.MinFilter == s.MagFilter && s.MipmapFilter == render::FilterMode::Nearest && s.LodMin == 0 && s.LodMax == 0 &&
             s.AnisotropyClamp <= 1 && !s.Compare &&
             (s.AddressS == render::AddressMode::ClampToEdge || s.AddressS == render::AddressMode::ClampToBorder) &&
             (s.AddressT == render::AddressMode::ClampToEdge || s.AddressT == render::AddressMode::ClampToBorder)));
}

}  // namespace

bool ValidateShaderPipelineLayoutDesc(const ShaderPipelineLayoutDesc& desc, Nullable<string*> diagnostic) {
    if (!ValidateShaderTypeRecords(desc.Types, diagnostic)) return false;
    unordered_set<string> names;
    unordered_set<string> groupNames;
    unordered_set<uint32_t> slots;
    unordered_set<uint32_t> sets;
    vector<const render::BindingDescriptor*> bindings;
    for (const auto& group : desc.Groups) {
        if (group.Name.empty() || !groupNames.insert(group.Name).second || !slots.insert(group.GroupSlot).second)
            return LayoutError(diagnostic, "invalid or duplicate group name/slot");
        unordered_set<uint32_t> ids;
        std::optional<uint32_t> set;
        for (const auto& entry : group.Bindings) {
            const auto& b = entry.Binding;
            if (entry.Name.empty() || !names.insert(entry.Name).second || !ids.insert(b.BindingId).second ||
                !EnumContains(b.Kind) || !b.Count || !ValidStages(b.Stages) || (!b.D3D12 && !b.Vulkan))
                return LayoutError(diagnostic, "invalid or duplicate binding");
            if (b.D3D12) {
                const auto& d = *b.D3D12;
                if (d.RegisterType != render::GetBindingRegisterClass(b.Kind) || !EnumContains(d.Placement) ||
                    uint64_t{d.Register} + b.Count - 1 > std::numeric_limits<uint32_t>::max() ||
                    (d.Placement == render::D3D12BufferPlacement::RootDescriptor && (b.Count != 1 || !render::CanBeRootDescriptor(b.Kind))))
                    return LayoutError(diagnostic, "invalid D3D12 binding mapping");
            }
            if (b.Vulkan) {
                const auto& v = *b.Vulkan;
                if (!EnumContains(v.Placement) || v.Set == std::numeric_limits<uint32_t>::max() || (set && *set != v.Set) ||
                    (v.Placement == render::VulkanBufferDescriptorPlacement::Dynamic && (b.Count != 1 || !render::CanBeRootDescriptor(b.Kind))))
                    return LayoutError(diagnostic, "invalid Vulkan set or binding placement");
                set = v.Set;
            }
            if (b.ImmutableSampler && (b.Kind != render::BindingKind::Sampler || b.Count != 1 || !ValidSampler(*b.ImmutableSampler)))
                return LayoutError(diagnostic, "invalid immutable sampler");
            if (b.Kind == render::BindingKind::CBuffer) {
                if (!IsRoot(desc.Types, entry.TypeIndex) || !entry.BufferByteSize || entry.BufferByteSize % 16 || entry.BufferByteSize < desc.Types[*entry.TypeIndex].Size)
                    return LayoutError(diagnostic, "invalid constant buffer type or byte size");
            } else if (entry.TypeIndex || entry.BufferByteSize) {
                return LayoutError(diagnostic, "non-constant binding cannot own a constant block");
            }
            for (const auto* old : bindings) {
                if (old->D3D12 && b.D3D12 && old->D3D12->Space == b.D3D12->Space && old->D3D12->RegisterType == b.D3D12->RegisterType &&
                    uint64_t{old->D3D12->Register} < uint64_t{b.D3D12->Register} + b.Count && uint64_t{b.D3D12->Register} < uint64_t{old->D3D12->Register} + old->Count)
                    return LayoutError(diagnostic, "overlapping D3D12 registers");
                if (old->Vulkan && b.Vulkan && old->Vulkan->Set == b.Vulkan->Set && old->Vulkan->Binding == b.Vulkan->Binding)
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
        if (entry.Name.empty() || !names.insert(entry.Name).second || !pushIds.insert(p.PushId).second || !p.Size || p.Size % 4 ||
            !ValidStages(p.Stages) || (!p.D3D12 && !p.Vulkan) || (entry.TypeIndex && (!IsRoot(desc.Types, entry.TypeIndex) || desc.Types[*entry.TypeIndex].Size > p.Size)))
            return LayoutError(diagnostic, "invalid push constant");
        if (p.Vulkan) {
            if (hasVkPush || p.Vulkan->Offset % 4 || uint64_t{p.Vulkan->Offset} + p.Size > std::numeric_limits<uint32_t>::max())
                return LayoutError(diagnostic, "multiple or invalid Vulkan push ranges");
            hasVkPush = true;
        }
        if (p.D3D12) {
            for (const auto* b : bindings)
                if (b->D3D12 && b->D3D12->RegisterType == render::BindingRegisterClass::CBV && b->D3D12->Space == p.D3D12->Space &&
                    p.D3D12->Register >= b->D3D12->Register && uint64_t{p.D3D12->Register} < uint64_t{b->D3D12->Register} + b->Count)
                    return LayoutError(diagnostic, "push constant overlaps a constant buffer register");
            for (size_t j = 0; j < i; ++j)
                if (desc.PushConstants[j].Push.D3D12 == p.D3D12)
                    return LayoutError(diagnostic, "duplicate D3D12 push register");
        }
    }
    return true;
}

Nullable<const ShaderTargetProgramInterfaceDesc*> GetShaderTargetInterface(const ShaderProgramInterfaceDesc& desc, render::RenderBackend backend) noexcept {
    if (backend == render::RenderBackend::D3D12 && desc.DXIL) return &*desc.DXIL;
    if (backend == render::RenderBackend::Vulkan && desc.SPIRV) return &*desc.SPIRV;
    return nullptr;
}

bool ValidateShaderProgramInterfaceDesc(const ShaderProgramInterfaceDesc& desc, Nullable<const ShaderPipelineLayoutDesc*> layout, Nullable<string*> diagnostic) {
    if (diagnostic) diagnostic.Get()->clear();
    if (desc.Name.empty() || (!desc.DXIL && !desc.SPIRV)) return LayoutError(diagnostic, "program needs a name and target interface");
    for (auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan}) {
        const auto target = GetShaderTargetInterface(desc, backend);
        if (!target) continue;
        const auto& t = *target.Get();
        if (!ValidStages(t.Stages) || (!t.Stages.HasFlag(render::ShaderStage::Vertex) && !t.VertexInputs.empty()))
            return LayoutError(diagnostic, "invalid program stages or vertex inputs");
        for (size_t i = 0; i < t.VertexInputs.size(); ++i) {
            const auto& input = t.VertexInputs[i];
            if (input.Semantic.empty() || !EnumContains(input.ComponentKind) || input.ComponentCount < 1 || input.ComponentCount > 4)
                return LayoutError(diagnostic, "invalid vertex signature");
            for (size_t j = 0; j < i; ++j)
                if (t.VertexInputs[j].Location == input.Location ||
                    (t.VertexInputs[j].Semantic == input.Semantic && t.VertexInputs[j].SemanticIndex == input.SemanticIndex))
                    return LayoutError(diagnostic, "duplicate vertex semantic or location");
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
        if (!ValidateShaderProgramInterfaceDesc(variant, &desc.SharedLayout, diagnostic)) return false;
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
requires std::same_as<std::remove_cvref_t<T>, render::D3D12BindingDescriptor>
bool VisitLayoutFields(T& v, F&& f) {
    return f("RegisterType", v.RegisterType) &&
           f("Register", v.Register) &&
           f("Space", v.Space) &&
           f("Placement", v.Placement);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, render::VulkanBindingDescriptor>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Set", v.Set) &&
           f("Binding", v.Binding) &&
           f("Placement", v.Placement);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, render::SamplerDescriptor>
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
requires std::same_as<std::remove_cvref_t<T>, render::BindingDescriptor>
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
requires std::same_as<std::remove_cvref_t<T>, render::PushConstantDescriptor::D3D12Location>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Register", v.Register) &&
           f("Space", v.Space);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, render::PushConstantDescriptor::VulkanLocation>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Offset", v.Offset);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, render::PushConstantDescriptor>
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
requires std::same_as<std::remove_cvref_t<T>, ShaderTargetProgramInterfaceDesc>
bool VisitLayoutFields(T& v, F&& f) {
    return f("Stages", v.Stages) &&
           f("VertexInputs", v.VertexInputs) &&
           f("ActiveBindings", v.ActiveBindings) &&
           f("ActivePushConstants", v.ActivePushConstants);
}

template <typename T, typename F>
requires std::same_as<std::remove_cvref_t<T>, ShaderProgramInterfaceDesc>
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
    else if constexpr (std::same_as<T, render::ShaderStages>)
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
                       std::same_as<T, render::ShaderStages> || requires { typename LayoutVector<T>::Element; })
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
    } else if constexpr (std::is_enum_v<T> || std::same_as<T, uint32_t> || std::same_as<T, render::ShaderStages>) {
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

template <typename T>
bool WriteLayoutJsonValue(JsonRef parent, std::optional<std::string_view> key, const T& value) {
    if (!parent.IsValid()) return false;
    bool result = true;
    if constexpr (std::same_as<T, string>) {
        if (key)
            parent.AddString(*key, value);
        else
            parent.AppendString(value);
    } else if constexpr (std::is_enum_v<T>) {
        if (key)
            parent.AddString(*key, LayoutEnumName(value));
        else
            parent.AppendString(LayoutEnumName(value));
    } else if constexpr (std::same_as<T, bool>) {
        if (key)
            parent.AddBool(*key, value);
        else
            parent.AppendBool(value);
    } else if constexpr (std::same_as<T, float>) {
        if (key)
            parent.AddDouble(*key, value);
        else
            parent.AppendDouble(value);
    } else if constexpr (std::same_as<T, uint32_t>) {
        if (key)
            parent.AddUint(*key, value);
        else
            parent.AppendUint(value);
    } else if constexpr (std::same_as<T, render::ShaderStages>) {
        result = WriteLayoutJsonValue(parent, key, value.value());
    } else if constexpr (requires { typename LayoutOptional<T>::Element; }) {
        result = !value || WriteLayoutJsonValue(parent, key, *value);
    } else if constexpr (requires { typename LayoutVector<T>::Element; }) {
        auto array = key ? parent.AddArray(*key) : parent.AppendArray();
        if (!array.IsValid()) return false;
        for (const auto& item : value)
            if (!WriteLayoutJsonValue(array, std::nullopt, item)) return false;
    } else {
        auto object = key ? parent.AddObject(*key) : parent.AppendObject();
        if (!object.IsValid()) return false;
        result = VisitLayoutFields(value, [&](std::string_view name, const auto& field) { return WriteLayoutJsonValue(object, name, field); });
    }
    return result;
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
    } else if constexpr (std::same_as<T, render::ShaderStages>) {
        uint32_t raw = 0;
        if (!DeserializeJsonValue(input, raw)) return false;
        value = render::ShaderStages{raw};
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
    auto root = writer.RootObject();
    root.AddUint("Version", 1);
    root.AddString("Kind", kind);
    if (!WriteLayoutJsonValue(root, "Data", value)) return std::nullopt;
    return writer.Write();
}

template <typename T, typename Validate>
bool DecodeLayoutJson(std::string_view text, T& output, std::string_view kind, Validate validate) {
    const auto document = JsonDocument::Parse(text);
    if (!document) return false;
    const auto root = document->Root();
    uint32_t version = 0;
    if (!root.IsObject() || !DeserializeJsonValue(root["Version"], version) || version != 1 || root["Kind"].AsString() != kind) return false;
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

std::optional<vector<byte>> WriteShaderProgramInterfaceBinary(const ShaderProgramInterfaceDesc& desc) {
    return EncodeLayout(desc, 2, [](const auto& v) { return ValidateShaderProgramInterfaceDesc(v); });
}
bool ReadShaderProgramInterfaceBinary(std::span<const byte> bytes, ShaderProgramInterfaceDesc& output) {
    return DecodeLayout(bytes, output, 2, [](const auto& v) { return ValidateShaderProgramInterfaceDesc(v); });
}
std::optional<string> WriteShaderProgramInterfaceJson(const ShaderProgramInterfaceDesc& desc) {
    return EncodeLayoutJson(desc, "ShaderProgramInterface", [](const auto& v) { return ValidateShaderProgramInterfaceDesc(v); });
}
bool ReadShaderProgramInterfaceJson(std::string_view text, ShaderProgramInterfaceDesc& output) {
    return DecodeLayoutJson(text, output, "ShaderProgramInterface", [](const auto& v) { return ValidateShaderProgramInterfaceDesc(v); });
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

}  // namespace radray
