#include <radray/shader/spirv.h>

#include <radray/shader/hlsl.h>
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

bool JsonDeserializer<render::SpirvTypeMember>::Read(
    const JsonValue& json,
    render::SpirvTypeMember& value) noexcept {
    using value_type = render::SpirvTypeMember;
    return DeserializeJsonObject(
        json,
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

bool JsonDeserializer<render::SpirvTypeInfo>::Read(
    const JsonValue& json,
    render::SpirvTypeInfo& value) noexcept {
    using value_type = render::SpirvTypeInfo;
    return DeserializeJsonObject(
        json,
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

bool JsonDeserializer<render::SpirvImageInfo>::Read(
    const JsonValue& json,
    render::SpirvImageInfo& value) noexcept {
    using value_type = render::SpirvImageInfo;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Dim", &value_type::Dim},
        JsonMember{"Arrayed", &value_type::Arrayed},
        JsonMember{"Multisampled", &value_type::Multisampled},
        JsonMember{"Depth", &value_type::Depth},
        JsonMember{"SampledType", &value_type::SampledType});
}

bool JsonDeserializer<render::SpirvStageIo>::Read(
    const JsonValue& json,
    render::SpirvStageIo& value) noexcept {
    JsonObjectReader object{json};
    render::SpirvStageIo decoded{};
    if (!object.IsValid() ||
        !object.Member("Name", decoded.Name) ||
        !object.Member("HlslSemantic", decoded.HlslSemantic) ||
        !object.Member("Location", decoded.Location) ||
        !object.Member("TypeIndex", decoded.TypeIndex) ||
        !object.OptionalMember("BuiltIn", decoded.BuiltIn)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonDeserializer<render::SpirvResourceBinding>::Read(
    const JsonValue& json,
    render::SpirvResourceBinding& value) noexcept {
    JsonObjectReader object{json};
    render::SpirvResourceBinding decoded{};
    if (!object.IsValid() ||
        !object.Member("Name", decoded.Name) ||
        !object.Member("Kind", decoded.Kind) ||
        !object.Member("Set", decoded.Set) ||
        !object.Member("Binding", decoded.Binding) ||
        !object.OptionalMember("HlslRegister", decoded.HlslRegister) ||
        !object.OptionalMember("HlslSpace", decoded.HlslSpace) ||
        !object.Member("ArraySize", decoded.ArraySize) ||
        !object.Member("TypeIndex", decoded.TypeIndex) ||
        !object.Member("UniformBufferSize", decoded.UniformBufferSize) ||
        !object.Member("ReadOnly", decoded.ReadOnly) ||
        !object.Member("WriteOnly", decoded.WriteOnly) ||
        !object.Member("IsViewInHlsl", decoded.IsViewInHlsl) ||
        !object.Member("HlslType", decoded.HlslType) ||
        !object.Member("IsUnboundedArray", decoded.IsUnboundedArray) ||
        !object.OptionalMember("ImageInfo", decoded.ImageInfo)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonDeserializer<render::SpirvComputeInfo>::Read(
    const JsonValue& json,
    render::SpirvComputeInfo& value) noexcept {
    using value_type = render::SpirvComputeInfo;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"LocalSizeX", &value_type::LocalSizeX},
        JsonMember{"LocalSizeY", &value_type::LocalSizeY},
        JsonMember{"LocalSizeZ", &value_type::LocalSizeZ});
}

bool JsonDeserializer<render::SpirvPushConstantRange>::Read(
    const JsonValue& json,
    render::SpirvPushConstantRange& value) noexcept {
    using value_type = render::SpirvPushConstantRange;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Offset", &value_type::Offset},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"TypeIndex", &value_type::TypeIndex},
        JsonMember{"IsViewInHlsl", &value_type::IsViewInHlsl});
}

bool JsonDeserializer<render::SpirvShaderDesc>::Read(
    const JsonValue& json,
    render::SpirvShaderDesc& value) noexcept {
    JsonObjectReader object{json};
    uint32_t formatVersion = 0;
    string kind;
    if (!object.IsValid() ||
        !object.Member("FormatVersion", formatVersion) ||
        !object.Member("Kind", kind) ||
        formatVersion != render::kReflectionFormatVersion ||
        kind != "spirv") {
        return false;
    }

    render::SpirvShaderDesc decoded{};
    if (!object.Member("Types", decoded.Types) ||
        !object.Member("StageInputs", decoded.StageInputs) ||
        !object.Member("StageOutputs", decoded.StageOutputs) ||
        !object.Member("ResourceBindings", decoded.ResourceBindings) ||
        !object.Member("ConstantRanges", decoded.ConstantRanges) ||
        !object.OptionalMember("ComputeInfo", decoded.ComputeInfo)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

}  // namespace radray

namespace radray::render {

std::optional<string> SerializeSpirvShaderDesc(const SpirvShaderDesc& desc) noexcept {
    return SerializeJson(desc, true);
}

std::optional<SpirvShaderDesc> DeserializeSpirvShaderDesc(std::string_view json) noexcept {
    std::optional<SpirvShaderDesc> desc = DeserializeJson<SpirvShaderDesc>(json);
    if (!desc.has_value()) {
        RADRAY_ERR_LOG("DeserializeSpirvShaderDesc: invalid JSON payload");
    }
    return desc;
}

}  // namespace radray::render
