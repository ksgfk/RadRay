#include <radray/render/spirv.h>

#include <radray/render/hlsl.h>
#include <radray/json.h>
#include <radray/logger.h>

namespace radray {

bool JsonSerializer<render::SpirvTypeMember>::Write(
    JsonWriteContext& context,
    const render::SpirvTypeMember& value) noexcept {
    using value_type = render::SpirvTypeMember;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Offset", &value_type::Offset},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"TypeIndex", &value_type::TypeIndex},
        JsonMember{"ArraySize", &value_type::ArraySize},
        JsonMember{"ArrayStride", &value_type::ArrayStride},
        JsonMember{"MatrixStride", &value_type::MatrixStride},
        JsonMember{"RowMajor", &value_type::RowMajor});
}

bool JsonSerializer<render::SpirvTypeInfo>::Write(
    JsonWriteContext& context,
    const render::SpirvTypeInfo& value) noexcept {
    using value_type = render::SpirvTypeInfo;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"BaseType", &value_type::BaseType},
        JsonMember{"VectorSize", &value_type::VectorSize},
        JsonMember{"Columns", &value_type::Columns},
        JsonMember{"ArraySize", &value_type::ArraySize},
        JsonMember{"ArrayStride", &value_type::ArrayStride},
        JsonMember{"MatrixStride", &value_type::MatrixStride},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"RowMajor", &value_type::RowMajor},
        JsonMember{"Members", &value_type::Members});
}

bool JsonSerializer<render::SpirvImageInfo>::Write(
    JsonWriteContext& context,
    const render::SpirvImageInfo& value) noexcept {
    using value_type = render::SpirvImageInfo;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Dim", &value_type::Dim},
        JsonMember{"Arrayed", &value_type::Arrayed},
        JsonMember{"Multisampled", &value_type::Multisampled},
        JsonMember{"Depth", &value_type::Depth},
        JsonMember{"SampledType", &value_type::SampledType});
}

bool JsonSerializer<render::SpirvStageIo>::Write(
    JsonWriteContext& context,
    const render::SpirvStageIo& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("Name", value.Name) &&
           object.Member("HlslSemantic", value.HlslSemantic) &&
           object.Member("Location", value.Location) &&
           object.Member("TypeIndex", value.TypeIndex) &&
           object.OptionalMember("BuiltIn", value.BuiltIn);
}

bool JsonSerializer<render::SpirvResourceBinding>::Write(
    JsonWriteContext& context,
    const render::SpirvResourceBinding& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("Name", value.Name) &&
           object.Member("Kind", value.Kind) &&
           object.Member("Set", value.Set) &&
           object.Member("Binding", value.Binding) &&
           object.OptionalMember("HlslRegister", value.HlslRegister) &&
           object.OptionalMember("HlslSpace", value.HlslSpace) &&
           object.Member("ArraySize", value.ArraySize) &&
           object.Member("TypeIndex", value.TypeIndex) &&
           object.Member("UniformBufferSize", value.UniformBufferSize) &&
           object.Member("ReadOnly", value.ReadOnly) &&
           object.Member("WriteOnly", value.WriteOnly) &&
           object.Member("IsViewInHlsl", value.IsViewInHlsl) &&
           object.Member("HlslType", value.HlslType) &&
           object.Member("IsUnboundedArray", value.IsUnboundedArray) &&
           object.OptionalMember("ImageInfo", value.ImageInfo);
}

bool JsonSerializer<render::SpirvComputeInfo>::Write(
    JsonWriteContext& context,
    const render::SpirvComputeInfo& value) noexcept {
    using value_type = render::SpirvComputeInfo;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"LocalSizeX", &value_type::LocalSizeX},
        JsonMember{"LocalSizeY", &value_type::LocalSizeY},
        JsonMember{"LocalSizeZ", &value_type::LocalSizeZ});
}

bool JsonSerializer<render::SpirvPushConstantRange>::Write(
    JsonWriteContext& context,
    const render::SpirvPushConstantRange& value) noexcept {
    using value_type = render::SpirvPushConstantRange;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Offset", &value_type::Offset},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"TypeIndex", &value_type::TypeIndex},
        JsonMember{"IsViewInHlsl", &value_type::IsViewInHlsl});
}

bool JsonSerializer<render::SpirvShaderDesc>::Write(
    JsonWriteContext& context,
    const render::SpirvShaderDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("FormatVersion", render::kReflectionFormatVersion) &&
           object.Member("Kind", "spirv") &&
           object.Member("Types", value.Types) &&
           object.Member("StageInputs", value.StageInputs) &&
           object.Member("StageOutputs", value.StageOutputs) &&
           object.Member("ResourceBindings", value.ResourceBindings) &&
           object.Member("ConstantRanges", value.ConstantRanges) &&
           object.OptionalMember("ComputeInfo", value.ComputeInfo);
}

}  // namespace radray

namespace radray::render {

// ResourceBindType SpirvResourceBinding::MapResourceBindType() const noexcept {
//     const bool isBufferImage = ImageInfo.has_value() && ImageInfo->Dim == SpirvImageDim::Buffer;
//     switch (Kind) {
//         case SpirvResourceKind::UniformBuffer:
//             return ResourceBindType::CBuffer;
//         case SpirvResourceKind::StorageBuffer:
//             return (ReadOnly && !WriteOnly) ? ResourceBindType::Buffer : ResourceBindType::RWBuffer;
//         case SpirvResourceKind::SampledImage:
//         case SpirvResourceKind::SeparateImage:
//             return isBufferImage ? ResourceBindType::TexelBuffer : ResourceBindType::Texture;
//         case SpirvResourceKind::SeparateSampler:
//             return ResourceBindType::Sampler;
//         case SpirvResourceKind::StorageImage:
//             return isBufferImage ? ResourceBindType::RWTexelBuffer : ResourceBindType::RWTexture;
//         case SpirvResourceKind::AccelerationStructure:
//             return ResourceBindType::AccelerationStructure;
//         default:
//             return ResourceBindType::UNKNOWN;
//     }
// }

// ====================== SpirvShaderDesc <-> JSON ======================

namespace {

template <typename E>
E UToEnum(uint64_t v) noexcept {
    return static_cast<E>(static_cast<std::underlying_type_t<E>>(v));
}

void ReadSpirvType(const JsonValue& obj, SpirvTypeInfo& t) {
    t.Name = string{obj["Name"].AsString()};
    t.BaseType = UToEnum<SpirvBaseType>(obj["BaseType"].AsUint());
    t.VectorSize = static_cast<uint32_t>(obj["VectorSize"].AsUint(1));
    t.Columns = static_cast<uint32_t>(obj["Columns"].AsUint(1));
    t.ArraySize = static_cast<uint32_t>(obj["ArraySize"].AsUint());
    t.ArrayStride = static_cast<uint32_t>(obj["ArrayStride"].AsUint());
    t.MatrixStride = static_cast<uint32_t>(obj["MatrixStride"].AsUint());
    t.Size = static_cast<uint32_t>(obj["Size"].AsUint());
    t.RowMajor = obj["RowMajor"].AsBool();
    JsonValue members = obj["Members"];
    const size_t n = members.Size();
    t.Members.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        JsonValue mo = members.At(i);
        SpirvTypeMember m{};
        m.Name = string{mo["Name"].AsString()};
        m.Offset = static_cast<uint32_t>(mo["Offset"].AsUint());
        m.Size = static_cast<uint32_t>(mo["Size"].AsUint());
        m.TypeIndex = static_cast<uint32_t>(mo["TypeIndex"].AsUint());
        m.ArraySize = static_cast<uint32_t>(mo["ArraySize"].AsUint());
        m.ArrayStride = static_cast<uint32_t>(mo["ArrayStride"].AsUint());
        m.MatrixStride = static_cast<uint32_t>(mo["MatrixStride"].AsUint());
        m.RowMajor = mo["RowMajor"].AsBool(false);
        t.Members.push_back(std::move(m));
    }
}

void ReadSpirvBinding(const JsonValue& obj, SpirvResourceBinding& r) {
    r.Name = string{obj["Name"].AsString()};
    r.Kind = UToEnum<SpirvResourceKind>(obj["Kind"].AsUint());
    r.Set = static_cast<uint32_t>(obj["Set"].AsUint());
    r.Binding = static_cast<uint32_t>(obj["Binding"].AsUint());
    if (obj.Has("HlslRegister")) {
        r.HlslRegister = static_cast<uint32_t>(obj["HlslRegister"].AsUint());
    }
    if (obj.Has("HlslSpace")) {
        r.HlslSpace = static_cast<uint32_t>(obj["HlslSpace"].AsUint());
    }
    r.ArraySize = static_cast<uint32_t>(obj["ArraySize"].AsUint());
    r.TypeIndex = static_cast<uint32_t>(obj["TypeIndex"].AsUint());
    r.UniformBufferSize = static_cast<uint32_t>(obj["UniformBufferSize"].AsUint());
    r.ReadOnly = obj["ReadOnly"].AsBool(true);
    r.WriteOnly = obj["WriteOnly"].AsBool(false);
    r.IsViewInHlsl = obj["IsViewInHlsl"].AsBool(false);
    r.HlslType = string{obj["HlslType"].AsString()};
    r.IsUnboundedArray = obj["IsUnboundedArray"].AsBool(false);
    if (obj.Has("ImageInfo")) {
        JsonValue io = obj["ImageInfo"];
        SpirvImageInfo img{};
        img.Dim = UToEnum<SpirvImageDim>(io["Dim"].AsUint());
        img.Arrayed = io["Arrayed"].AsBool();
        img.Multisampled = io["Multisampled"].AsBool();
        img.Depth = io["Depth"].AsBool();
        img.SampledType = static_cast<uint32_t>(io["SampledType"].AsUint());
        r.ImageInfo = img;
    }
}

void ReadSpirvStageIo(const JsonValue& obj, SpirvStageIo& value) {
    value.Name = string{obj["Name"].AsString()};
    value.HlslSemantic = string{obj["HlslSemantic"].AsString()};
    value.Location = static_cast<uint32_t>(obj["Location"].AsUint());
    value.TypeIndex = static_cast<uint32_t>(obj["TypeIndex"].AsUint());
    if (obj.Has("BuiltIn")) {
        value.BuiltIn = static_cast<uint32_t>(obj["BuiltIn"].AsUint());
    }
}

}  // namespace

std::optional<string> SerializeSpirvShaderDesc(const SpirvShaderDesc& desc) noexcept {
    return SerializeJson(desc, true);
}

std::optional<SpirvShaderDesc> DeserializeSpirvShaderDesc(std::string_view json) noexcept {
    std::optional<JsonDocument> docOpt = JsonDocument::Parse(json);
    if (!docOpt.has_value()) {
        RADRAY_ERR_LOG("DeserializeSpirvShaderDesc: JSON parse failed");
        return std::nullopt;
    }
    JsonValue root = docOpt->Root();
    if (!root.IsObject()) {
        RADRAY_ERR_LOG("DeserializeSpirvShaderDesc: root is not object");
        return std::nullopt;
    }
    const uint32_t ver = static_cast<uint32_t>(root["FormatVersion"].AsUint());
    if (ver != kReflectionFormatVersion) {
        RADRAY_ERR_LOG("DeserializeSpirvShaderDesc: format version {} != expected {}", ver, kReflectionFormatVersion);
        return std::nullopt;
    }

    SpirvShaderDesc desc{};
    JsonValue types = root["Types"];
    desc.Types.reserve(types.Size());
    for (size_t i = 0; i < types.Size(); ++i) {
        SpirvTypeInfo t{};
        ReadSpirvType(types.At(i), t);
        desc.Types.push_back(std::move(t));
    }
    JsonValue inputs = root["StageInputs"];
    desc.StageInputs.reserve(inputs.Size());
    for (size_t i = 0; i < inputs.Size(); ++i) {
        SpirvStageIo value{};
        ReadSpirvStageIo(inputs.At(i), value);
        desc.StageInputs.push_back(std::move(value));
    }
    JsonValue outputs = root["StageOutputs"];
    desc.StageOutputs.reserve(outputs.Size());
    for (size_t i = 0; i < outputs.Size(); ++i) {
        SpirvStageIo value{};
        ReadSpirvStageIo(outputs.At(i), value);
        desc.StageOutputs.push_back(std::move(value));
    }
    JsonValue binds = root["ResourceBindings"];
    desc.ResourceBindings.reserve(binds.Size());
    for (size_t i = 0; i < binds.Size(); ++i) {
        SpirvResourceBinding r{};
        ReadSpirvBinding(binds.At(i), r);
        desc.ResourceBindings.push_back(std::move(r));
    }
    JsonValue ranges = root["ConstantRanges"];
    desc.ConstantRanges.reserve(ranges.Size());
    for (size_t i = 0; i < ranges.Size(); ++i) {
        JsonValue co = ranges.At(i);
        SpirvPushConstantRange c{};
        c.Name = string{co["Name"].AsString()};
        c.Offset = static_cast<uint32_t>(co["Offset"].AsUint());
        c.Size = static_cast<uint32_t>(co["Size"].AsUint());
        c.TypeIndex = static_cast<uint32_t>(co["TypeIndex"].AsUint());
        c.IsViewInHlsl = co["IsViewInHlsl"].AsBool();
        desc.ConstantRanges.push_back(std::move(c));
    }
    if (root.Has("ComputeInfo")) {
        JsonValue co = root["ComputeInfo"];
        SpirvComputeInfo ci{};
        ci.LocalSizeX = static_cast<uint32_t>(co["LocalSizeX"].AsUint(1));
        ci.LocalSizeY = static_cast<uint32_t>(co["LocalSizeY"].AsUint(1));
        ci.LocalSizeZ = static_cast<uint32_t>(co["LocalSizeZ"].AsUint(1));
        desc.ComputeInfo = ci;
    }
    return desc;
}

}  // namespace radray::render
