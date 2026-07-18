#include <radray/shader/hlsl.h>

#include <algorithm>

#include <radray/utility.h>
#include <radray/json.h>
#include <radray/logger.h>

namespace radray::shader {

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

// 枚举以底层整数值存储。反序列化时越界回落到默认 (通常 UNKNOWN=0)。
template <typename E>
uint64_t EnumToU(E v) noexcept {
    return static_cast<uint64_t>(static_cast<std::underlying_type_t<E>>(v));
}

template <typename E>
E UToEnum(uint64_t v) noexcept {
    return static_cast<E>(static_cast<std::underlying_type_t<E>>(v));
}

void WriteHlslType(JsonRef obj, const HlslShaderTypeDesc& t) {
    obj.AddString("Name", t.Name);
    obj.AddUint("Class", EnumToU(t.Class));
    obj.AddUint("Type", EnumToU(t.Type));
    obj.AddUint("Rows", t.Rows);
    obj.AddUint("Columns", t.Columns);
    obj.AddUint("Elements", t.Elements);
    obj.AddUint("Offset", t.Offset);
    JsonRef members = obj.AddArray("Members");
    for (const HlslShaderTypeMember& m : t.Members) {
        JsonRef mo = members.AppendObject();
        mo.AddString("Name", m.Name);
        mo.AddUint("Type", m.Type.Value);
    }
}

void WriteHlslVariable(JsonRef obj, const HlslShaderVariableDesc& v) {
    obj.AddString("Name", v.Name);
    obj.AddUint("Type", v.Type.Value);
    obj.AddUint("StartOffset", v.StartOffset);
    obj.AddUint("Size", v.Size);
    obj.AddUint("uFlags", v.uFlags);
    obj.AddUint("StartTexture", v.StartTexture);
    obj.AddUint("TextureSize", v.TextureSize);
    obj.AddUint("StartSampler", v.StartSampler);
    obj.AddUint("SamplerSize", v.SamplerSize);
}

void WriteHlslCBuffer(JsonRef obj, const HlslShaderBufferDesc& b) {
    obj.AddString("Name", b.Name);
    obj.AddUint("Type", EnumToU(b.Type));
    obj.AddUint("Size", b.Size);
    obj.AddUint("Flags", b.Flags);
    obj.AddBool("IsViewInHlsl", b.IsViewInHlsl);
    JsonRef vars = obj.AddArray("Variables");
    for (size_t idx : b.Variables) {
        vars.AppendUint(idx);
    }
}

void WriteHlslBind(JsonRef obj, const HlslInputBindDesc& r) {
    obj.AddString("Name", r.Name);
    obj.AddUint("Type", EnumToU(r.Type));
    obj.AddUint("BindPoint", r.BindPoint);
    obj.AddUint("BindCount", r.BindCount);
    obj.AddUint("ReturnType", EnumToU(r.ReturnType));
    obj.AddUint("Dimension", EnumToU(r.Dimension));
    obj.AddUint("NumSamples", r.NumSamples);
    obj.AddUint("Space", r.Space);
    obj.AddUint("Flags", r.Flags);
    if (r.VkBinding.has_value()) {
        obj.AddUint("VkBinding", r.VkBinding.value());
    }
    if (r.VkSet.has_value()) {
        obj.AddUint("VkSet", r.VkSet.value());
    }
}

void WriteHlslSignature(JsonRef obj, const HlslSignatureParameterDesc& p) {
    obj.AddString("SemanticName", p.SemanticName);
    obj.AddUint("SemanticIndex", p.SemanticIndex);
    obj.AddUint("Register", p.Register);
    obj.AddUint("SystemValueType", EnumToU(p.SystemValueType));
    obj.AddUint("ComponentType", EnumToU(p.ComponentType));
    obj.AddUint("Stream", p.Stream);
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
}

}  // namespace

std::optional<string> SerializeHlslShaderDesc(const HlslShaderDesc& desc) noexcept {
    JsonWriter w{};
    if (!w.IsValid()) {
        return std::nullopt;
    }
    JsonRef root = w.RootObject();
    root.AddUint("FormatVersion", kReflectionFormatVersion);
    root.AddString("Kind", "hlsl");
    root.AddString("Creator", desc.Creator);
    root.AddUint("Version", desc.Version);
    root.AddUint("Flags", desc.Flags);
    root.AddUint("MinFeatureLevel", EnumToU(desc.MinFeatureLevel));
    root.AddUint("GroupSizeX", desc.GroupSizeX);
    root.AddUint("GroupSizeY", desc.GroupSizeY);
    root.AddUint("GroupSizeZ", desc.GroupSizeZ);

    JsonRef cbuffers = root.AddArray("ConstantBuffers");
    for (const HlslShaderBufferDesc& b : desc.ConstantBuffers) {
        WriteHlslCBuffer(cbuffers.AppendObject(), b);
    }
    JsonRef binds = root.AddArray("BoundResources");
    for (const HlslInputBindDesc& r : desc.BoundResources) {
        WriteHlslBind(binds.AppendObject(), r);
    }
    JsonRef inputs = root.AddArray("InputParameters");
    for (const HlslSignatureParameterDesc& p : desc.InputParameters) {
        WriteHlslSignature(inputs.AppendObject(), p);
    }
    JsonRef outputs = root.AddArray("OutputParameters");
    for (const HlslSignatureParameterDesc& p : desc.OutputParameters) {
        WriteHlslSignature(outputs.AppendObject(), p);
    }
    JsonRef vars = root.AddArray("Variables");
    for (const HlslShaderVariableDesc& v : desc.Variables) {
        WriteHlslVariable(vars.AppendObject(), v);
    }
    JsonRef types = root.AddArray("Types");
    for (const HlslShaderTypeDesc& t : desc.Types) {
        WriteHlslType(types.AppendObject(), t);
    }
    return w.Write(true);
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

}  // namespace radray::shader
