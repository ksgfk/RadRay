#include <radray/render/hlsl.h>

#include <algorithm>

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

// ====================== HlslShaderDesc <-> JSON ======================

namespace {

template <typename E>
E UToEnum(uint64_t v) noexcept {
    return static_cast<E>(static_cast<std::underlying_type_t<E>>(v));
}

void ReadHlslType(const JsonValue& obj, HlslShaderTypeDesc& t) {
    t.Name = string{obj["Name"].AsString()};
    t.Class = UToEnum<HlslShaderVariableClass>(obj["Class"].AsUint());
    t.Type = UToEnum<HlslShaderVariableType>(obj["Type"].AsUint());
    t.Rows = static_cast<uint32_t>(obj["Rows"].AsUint());
    t.Columns = static_cast<uint32_t>(obj["Columns"].AsUint());
    t.Elements = static_cast<uint32_t>(obj["Elements"].AsUint());
    t.Offset = static_cast<uint32_t>(obj["Offset"].AsUint());
    JsonValue members = obj["Members"];
    const size_t n = members.Size();
    t.Members.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        JsonValue mo = members.At(i);
        HlslShaderTypeMember m{};
        m.Name = string{mo["Name"].AsString()};
        m.Type = HlslShaderTypeId{static_cast<size_t>(mo["Type"].AsUint(HlslShaderTypeId::Invalid))};
        m.Offset = static_cast<uint32_t>(mo["Offset"].AsUint());
        t.Members.push_back(std::move(m));
    }
}

void ReadHlslVariable(const JsonValue& obj, HlslShaderVariableDesc& v) {
    v.Name = string{obj["Name"].AsString()};
    v.Type = HlslShaderTypeId{static_cast<size_t>(obj["Type"].AsUint(HlslShaderTypeId::Invalid))};
    v.StartOffset = static_cast<uint32_t>(obj["StartOffset"].AsUint());
    v.Size = static_cast<uint32_t>(obj["Size"].AsUint());
    v.uFlags = static_cast<uint32_t>(obj["uFlags"].AsUint());
    v.StartTexture = static_cast<uint32_t>(obj["StartTexture"].AsUint());
    v.TextureSize = static_cast<uint32_t>(obj["TextureSize"].AsUint());
    v.StartSampler = static_cast<uint32_t>(obj["StartSampler"].AsUint());
    v.SamplerSize = static_cast<uint32_t>(obj["SamplerSize"].AsUint());
}

void ReadHlslCBuffer(const JsonValue& obj, HlslShaderBufferDesc& b) {
    b.Name = string{obj["Name"].AsString()};
    b.Type = UToEnum<HlslCBufferType>(obj["Type"].AsUint());
    b.Size = static_cast<uint32_t>(obj["Size"].AsUint());
    b.Flags = static_cast<uint32_t>(obj["Flags"].AsUint());
    b.IsViewInHlsl = obj["IsViewInHlsl"].AsBool();
    JsonValue vars = obj["Variables"];
    const size_t n = vars.Size();
    b.Variables.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        b.Variables.push_back(static_cast<size_t>(vars.At(i).AsUint()));
    }
}

void ReadHlslBind(const JsonValue& obj, HlslInputBindDesc& r) {
    r.Name = string{obj["Name"].AsString()};
    r.Type = UToEnum<HlslShaderInputType>(obj["Type"].AsUint());
    r.BindPoint = static_cast<uint32_t>(obj["BindPoint"].AsUint());
    r.BindCount = static_cast<uint32_t>(obj["BindCount"].AsUint());
    r.ReturnType = UToEnum<HlslResourceReturnType>(obj["ReturnType"].AsUint());
    r.Dimension = UToEnum<HlslSRVDimension>(obj["Dimension"].AsUint());
    r.NumSamples = static_cast<uint32_t>(obj["NumSamples"].AsUint());
    r.Space = static_cast<uint32_t>(obj["Space"].AsUint());
    r.Flags = static_cast<uint32_t>(obj["Flags"].AsUint());
    if (obj.Has("VkBinding")) {
        r.VkBinding = static_cast<uint32_t>(obj["VkBinding"].AsUint());
    }
    if (obj.Has("VkSet")) {
        r.VkSet = static_cast<uint32_t>(obj["VkSet"].AsUint());
    }
}

void ReadHlslSignature(const JsonValue& obj, HlslSignatureParameterDesc& p) {
    p.SemanticName = string{obj["SemanticName"].AsString()};
    p.SemanticIndex = static_cast<uint32_t>(obj["SemanticIndex"].AsUint());
    p.Register = static_cast<uint32_t>(obj["Register"].AsUint());
    p.SystemValueType = UToEnum<HlslSystemValueType>(obj["SystemValueType"].AsUint());
    p.ComponentType = UToEnum<HlslRegisterComponentType>(obj["ComponentType"].AsUint());
    p.Stream = static_cast<uint32_t>(obj["Stream"].AsUint());
    p.Mask = static_cast<uint8_t>(obj["Mask"].AsUint());
    p.ReadWriteMask = static_cast<uint8_t>(obj["ReadWriteMask"].AsUint());
}

}  // namespace

std::optional<string> SerializeHlslShaderDesc(const HlslShaderDesc& desc) noexcept {
    return SerializeJson(desc, true);
}

std::optional<HlslShaderDesc> DeserializeHlslShaderDesc(std::string_view json) noexcept {
    std::optional<JsonDocument> docOpt = JsonDocument::Parse(json);
    if (!docOpt.has_value()) {
        RADRAY_ERR_LOG("DeserializeHlslShaderDesc: JSON parse failed");
        return std::nullopt;
    }
    JsonValue root = docOpt->Root();
    if (!root.IsObject()) {
        RADRAY_ERR_LOG("DeserializeHlslShaderDesc: root is not object");
        return std::nullopt;
    }
    const uint32_t ver = static_cast<uint32_t>(root["FormatVersion"].AsUint());
    if (ver != kReflectionFormatVersion) {
        RADRAY_ERR_LOG("DeserializeHlslShaderDesc: format version {} != expected {}", ver, kReflectionFormatVersion);
        return std::nullopt;
    }

    HlslShaderDesc desc{};
    desc.Creator = string{root["Creator"].AsString()};
    desc.Version = static_cast<uint32_t>(root["Version"].AsUint());
    desc.Flags = static_cast<uint32_t>(root["Flags"].AsUint());
    desc.MinFeatureLevel = UToEnum<HlslFeatureLevel>(root["MinFeatureLevel"].AsUint());
    desc.GroupSizeX = static_cast<uint32_t>(root["GroupSizeX"].AsUint());
    desc.GroupSizeY = static_cast<uint32_t>(root["GroupSizeY"].AsUint());
    desc.GroupSizeZ = static_cast<uint32_t>(root["GroupSizeZ"].AsUint());

    JsonValue cbuffers = root["ConstantBuffers"];
    desc.ConstantBuffers.reserve(cbuffers.Size());
    for (size_t i = 0; i < cbuffers.Size(); ++i) {
        HlslShaderBufferDesc b{};
        ReadHlslCBuffer(cbuffers.At(i), b);
        desc.ConstantBuffers.push_back(std::move(b));
    }
    JsonValue binds = root["BoundResources"];
    desc.BoundResources.reserve(binds.Size());
    for (size_t i = 0; i < binds.Size(); ++i) {
        HlslInputBindDesc r{};
        ReadHlslBind(binds.At(i), r);
        desc.BoundResources.push_back(std::move(r));
    }
    JsonValue inputs = root["InputParameters"];
    desc.InputParameters.reserve(inputs.Size());
    for (size_t i = 0; i < inputs.Size(); ++i) {
        HlslSignatureParameterDesc p{};
        ReadHlslSignature(inputs.At(i), p);
        desc.InputParameters.push_back(std::move(p));
    }
    JsonValue outputs = root["OutputParameters"];
    desc.OutputParameters.reserve(outputs.Size());
    for (size_t i = 0; i < outputs.Size(); ++i) {
        HlslSignatureParameterDesc p{};
        ReadHlslSignature(outputs.At(i), p);
        desc.OutputParameters.push_back(std::move(p));
    }
    JsonValue vars = root["Variables"];
    desc.Variables.reserve(vars.Size());
    for (size_t i = 0; i < vars.Size(); ++i) {
        HlslShaderVariableDesc v{};
        ReadHlslVariable(vars.At(i), v);
        desc.Variables.push_back(std::move(v));
    }
    JsonValue types = root["Types"];
    desc.Types.reserve(types.Size());
    for (size_t i = 0; i < types.Size(); ++i) {
        HlslShaderTypeDesc t{};
        ReadHlslType(types.At(i), t);
        desc.Types.push_back(std::move(t));
    }
    return desc;
}

}  // namespace radray::render
