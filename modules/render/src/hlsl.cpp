#include <radray/render/hlsl.h>

#include <algorithm>
#include <array>

#include <radray/utility.h>
#include <radray/json.h>
#include <radray/logger.h>

namespace radray {

bool JsonSerializer<render::HlslShaderTypeId>::Write(
    JsonWriteContext& context,
    const render::HlslShaderTypeId& value) noexcept {
    return SerializeJsonValue(context, value.Value);
}

bool JsonSerializer<render::HlslShaderTypeMember>::Write(
    JsonWriteContext& context,
    const render::HlslShaderTypeMember& value) noexcept {
    using value_type = render::HlslShaderTypeMember;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Type", &value_type::Type},
        JsonMember{"Offset", &value_type::Offset});
}

bool JsonSerializer<render::HlslShaderTypeDesc>::Write(
    JsonWriteContext& context,
    const render::HlslShaderTypeDesc& value) noexcept {
    using value_type = render::HlslShaderTypeDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Class", &value_type::Class},
        JsonMember{"Type", &value_type::Type},
        JsonMember{"Rows", &value_type::Rows},
        JsonMember{"Columns", &value_type::Columns},
        JsonMember{"Elements", &value_type::Elements},
        JsonMember{"Offset", &value_type::Offset},
        JsonMember{"Members", &value_type::Members});
}

bool JsonSerializer<render::HlslShaderVariableDesc>::Write(
    JsonWriteContext& context,
    const render::HlslShaderVariableDesc& value) noexcept {
    using value_type = render::HlslShaderVariableDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Type", &value_type::Type},
        JsonMember{"StartOffset", &value_type::StartOffset},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"uFlags", &value_type::uFlags},
        JsonMember{"StartTexture", &value_type::StartTexture},
        JsonMember{"TextureSize", &value_type::TextureSize},
        JsonMember{"StartSampler", &value_type::StartSampler},
        JsonMember{"SamplerSize", &value_type::SamplerSize});
}

bool JsonSerializer<render::HlslShaderBufferDesc>::Write(
    JsonWriteContext& context,
    const render::HlslShaderBufferDesc& value) noexcept {
    using value_type = render::HlslShaderBufferDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Type", &value_type::Type},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"Flags", &value_type::Flags},
        JsonMember{"IsViewInHlsl", &value_type::IsViewInHlsl},
        JsonMember{"Variables", &value_type::Variables});
}

bool JsonSerializer<render::HlslInputBindDesc>::Write(
    JsonWriteContext& context,
    const render::HlslInputBindDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("Name", value.Name) &&
           object.Member("Type", value.Type) &&
           object.Member("BindPoint", value.BindPoint) &&
           object.Member("BindCount", value.BindCount) &&
           object.Member("ReturnType", value.ReturnType) &&
           object.Member("Dimension", value.Dimension) &&
           object.Member("NumSamples", value.NumSamples) &&
           object.Member("Space", value.Space) &&
           object.Member("Flags", value.Flags) &&
           object.OptionalMember("VkBinding", value.VkBinding) &&
           object.OptionalMember("VkSet", value.VkSet);
}

bool JsonSerializer<render::HlslSignatureParameterDesc>::Write(
    JsonWriteContext& context,
    const render::HlslSignatureParameterDesc& value) noexcept {
    using value_type = render::HlslSignatureParameterDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"SemanticName", &value_type::SemanticName},
        JsonMember{"SemanticIndex", &value_type::SemanticIndex},
        JsonMember{"Register", &value_type::Register},
        JsonMember{"SystemValueType", &value_type::SystemValueType},
        JsonMember{"ComponentType", &value_type::ComponentType},
        JsonMember{"Stream", &value_type::Stream},
        JsonMember{"Mask", &value_type::Mask},
        JsonMember{"ReadWriteMask", &value_type::ReadWriteMask});
}

bool JsonSerializer<render::HlslShaderDesc>::Write(
    JsonWriteContext& context,
    const render::HlslShaderDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("FormatVersion", render::kReflectionFormatVersion) &&
           object.Member("Kind", "hlsl") &&
           object.Member("Creator", value.Creator) &&
           object.Member("Version", value.Version) &&
           object.Member("Flags", value.Flags) &&
           object.Member("MinFeatureLevel", value.MinFeatureLevel) &&
           object.Member("GroupSizeX", value.GroupSizeX) &&
           object.Member("GroupSizeY", value.GroupSizeY) &&
           object.Member("GroupSizeZ", value.GroupSizeZ) &&
           object.Member("ConstantBuffers", value.ConstantBuffers) &&
           object.Member("BoundResources", value.BoundResources) &&
           object.Member("InputParameters", value.InputParameters) &&
           object.Member("OutputParameters", value.OutputParameters) &&
           object.Member("Variables", value.Variables) &&
           object.Member("Types", value.Types);
}

bool JsonDeserializer<render::HlslShaderTypeId>::Read(
    const JsonValue& json,
    render::HlslShaderTypeId& value) noexcept {
    size_t decoded = 0;
    if (!DeserializeJsonValue(json, decoded)) {
        return false;
    }
    value = render::HlslShaderTypeId{decoded};
    return true;
}

bool JsonDeserializer<render::HlslShaderTypeMember>::Read(
    const JsonValue& json,
    render::HlslShaderTypeMember& value) noexcept {
    using value_type = render::HlslShaderTypeMember;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Type", &value_type::Type},
        JsonMember{"Offset", &value_type::Offset});
}

bool JsonDeserializer<render::HlslShaderTypeDesc>::Read(
    const JsonValue& json,
    render::HlslShaderTypeDesc& value) noexcept {
    using value_type = render::HlslShaderTypeDesc;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Class", &value_type::Class},
        JsonMember{"Type", &value_type::Type},
        JsonMember{"Rows", &value_type::Rows},
        JsonMember{"Columns", &value_type::Columns},
        JsonMember{"Elements", &value_type::Elements},
        JsonMember{"Offset", &value_type::Offset},
        JsonMember{"Members", &value_type::Members});
}

bool JsonDeserializer<render::HlslShaderVariableDesc>::Read(
    const JsonValue& json,
    render::HlslShaderVariableDesc& value) noexcept {
    using value_type = render::HlslShaderVariableDesc;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Type", &value_type::Type},
        JsonMember{"StartOffset", &value_type::StartOffset},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"uFlags", &value_type::uFlags},
        JsonMember{"StartTexture", &value_type::StartTexture},
        JsonMember{"TextureSize", &value_type::TextureSize},
        JsonMember{"StartSampler", &value_type::StartSampler},
        JsonMember{"SamplerSize", &value_type::SamplerSize});
}

bool JsonDeserializer<render::HlslShaderBufferDesc>::Read(
    const JsonValue& json,
    render::HlslShaderBufferDesc& value) noexcept {
    using value_type = render::HlslShaderBufferDesc;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Type", &value_type::Type},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"Flags", &value_type::Flags},
        JsonMember{"IsViewInHlsl", &value_type::IsViewInHlsl},
        JsonMember{"Variables", &value_type::Variables});
}

bool JsonDeserializer<render::HlslInputBindDesc>::Read(
    const JsonValue& json,
    render::HlslInputBindDesc& value) noexcept {
    JsonObjectReader object{json};
    render::HlslInputBindDesc decoded{};
    if (!object.IsValid() ||
        !object.Member("Name", decoded.Name) ||
        !object.Member("Type", decoded.Type) ||
        !object.Member("BindPoint", decoded.BindPoint) ||
        !object.Member("BindCount", decoded.BindCount) ||
        !object.Member("ReturnType", decoded.ReturnType) ||
        !object.Member("Dimension", decoded.Dimension) ||
        !object.Member("NumSamples", decoded.NumSamples) ||
        !object.Member("Space", decoded.Space) ||
        !object.Member("Flags", decoded.Flags) ||
        !object.OptionalMember("VkBinding", decoded.VkBinding) ||
        !object.OptionalMember("VkSet", decoded.VkSet)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonDeserializer<render::HlslSignatureParameterDesc>::Read(
    const JsonValue& json,
    render::HlslSignatureParameterDesc& value) noexcept {
    using value_type = render::HlslSignatureParameterDesc;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"SemanticName", &value_type::SemanticName},
        JsonMember{"SemanticIndex", &value_type::SemanticIndex},
        JsonMember{"Register", &value_type::Register},
        JsonMember{"SystemValueType", &value_type::SystemValueType},
        JsonMember{"ComponentType", &value_type::ComponentType},
        JsonMember{"Stream", &value_type::Stream},
        JsonMember{"Mask", &value_type::Mask},
        JsonMember{"ReadWriteMask", &value_type::ReadWriteMask});
}

bool JsonDeserializer<render::HlslShaderDesc>::Read(
    const JsonValue& json,
    render::HlslShaderDesc& value) noexcept {
    JsonObjectReader object{json};
    uint32_t formatVersion = 0;
    string kind;
    if (!object.IsValid() ||
        !object.Member("FormatVersion", formatVersion) ||
        !object.Member("Kind", kind) ||
        formatVersion != render::kReflectionFormatVersion ||
        kind != "hlsl") {
        return false;
    }

    render::HlslShaderDesc decoded{};
    if (!object.Member("Creator", decoded.Creator) ||
        !object.Member("Version", decoded.Version) ||
        !object.Member("Flags", decoded.Flags) ||
        !object.Member("MinFeatureLevel", decoded.MinFeatureLevel) ||
        !object.Member("GroupSizeX", decoded.GroupSizeX) ||
        !object.Member("GroupSizeY", decoded.GroupSizeY) ||
        !object.Member("GroupSizeZ", decoded.GroupSizeZ) ||
        !object.Member("ConstantBuffers", decoded.ConstantBuffers) ||
        !object.Member("BoundResources", decoded.BoundResources) ||
        !object.Member("InputParameters", decoded.InputParameters) ||
        !object.Member("OutputParameters", decoded.OutputParameters) ||
        !object.Member("Variables", decoded.Variables) ||
        !object.Member("Types", decoded.Types)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

}  // namespace radray

namespace radray::render {

bool HlslShaderTypeDesc::IsPrimitive() const noexcept {
    return Class == HlslShaderVariableClass::SCALAR ||
           Class == HlslShaderVariableClass::VECTOR ||
           Class == HlslShaderVariableClass::MATRIX_ROWS ||
           Class == HlslShaderVariableClass::MATRIX_COLUMNS;
}

size_t HlslShaderTypeDesc::GetSizeInBytes() const noexcept {
    switch (Type) {
        case HlslShaderVariableType::INT16:
        case HlslShaderVariableType::UINT16:
        case HlslShaderVariableType::FLOAT16:
            return 2 * Columns * Rows;
        case HlslShaderVariableType::UINT8:
            return 1 * Columns * Rows;
        case HlslShaderVariableType::DOUBLE:
        case HlslShaderVariableType::INT64:
        case HlslShaderVariableType::UINT64:
            return 8 * Columns * Rows;
        case HlslShaderVariableType::BOOL:
        case HlslShaderVariableType::INT:
        case HlslShaderVariableType::FLOAT:
        case HlslShaderVariableType::UINT:
            return 4 * Columns * Rows;
        default:
            return 0;
    }
}

bool HlslInputBindDesc::IsUnboundArray() const noexcept {
    return BindCount == 0;
}

Nullable<const HlslShaderBufferDesc*> HlslShaderDesc::FindCBufferByName(std::string_view name) const noexcept {
    auto it = std::find_if(ConstantBuffers.begin(), ConstantBuffers.end(), [&](const HlslShaderBufferDesc& cb) {
        return cb.Name == name;
    });
    return it == ConstantBuffers.end() ? Nullable<const HlslShaderBufferDesc*>{} : Nullable<const HlslShaderBufferDesc*>{&(*it)};
}

bool IsBufferDimension(HlslSRVDimension dim) noexcept {
    switch (dim) {
        case HlslSRVDimension::BUFFER:
        case HlslSRVDimension::BUFFEREX: return true;
        default: return false;
    }
}

std::optional<string> SerializeHlslShaderDesc(const HlslShaderDesc& desc) noexcept {
    return SerializeJson(desc, true);
}

std::optional<HlslShaderDesc> DeserializeHlslShaderDesc(std::string_view json) noexcept {
    std::optional<HlslShaderDesc> desc = DeserializeJson<HlslShaderDesc>(json);
    if (!desc.has_value()) {
        RADRAY_ERR_LOG("DeserializeHlslShaderDesc: invalid JSON payload");
    }
    return desc;
}

}  // namespace radray::render
