#include <radray/shader/shader_types.h>

#include <radray/json.h>
#include <radray/utility.h>

namespace radray {

bool JsonSerializer<render::ShaderBindingLocation>::Write(
    JsonWriteContext& context,
    const render::ShaderBindingLocation& value) noexcept {
    using value_type = render::ShaderBindingLocation;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Group", &value_type::Group},
        JsonMember{"Binding", &value_type::Binding});
}

bool JsonDeserializer<render::ShaderBindingLocation>::Read(
    const JsonValue& json,
    render::ShaderBindingLocation& value) noexcept {
    using value_type = render::ShaderBindingLocation;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Group", &value_type::Group},
        JsonMember{"Binding", &value_type::Binding});
}

bool JsonSerializer<render::SamplerDescriptor>::Write(
    JsonWriteContext& context,
    const render::SamplerDescriptor& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("AddressS", value.AddressS) &&
           object.Member("AddressT", value.AddressT) &&
           object.Member("AddressR", value.AddressR) &&
           object.Member("MinFilter", value.MinFilter) &&
           object.Member("MagFilter", value.MagFilter) &&
           object.Member("MipmapFilter", value.MipmapFilter) &&
           object.Member("LodMin", value.LodMin) &&
           object.Member("LodMax", value.LodMax) &&
           object.OptionalMember("Compare", value.Compare) &&
           object.Member("AnisotropyClamp", value.AnisotropyClamp);
}

bool JsonDeserializer<render::SamplerDescriptor>::Read(
    const JsonValue& json,
    render::SamplerDescriptor& value) noexcept {
    JsonObjectReader object{json};
    render::SamplerDescriptor decoded{};
    if (!object.IsValid() ||
        !object.Member("AddressS", decoded.AddressS) ||
        !object.Member("AddressT", decoded.AddressT) ||
        !object.Member("AddressR", decoded.AddressR) ||
        !object.Member("MinFilter", decoded.MinFilter) ||
        !object.Member("MagFilter", decoded.MagFilter) ||
        !object.Member("MipmapFilter", decoded.MipmapFilter) ||
        !object.MemberIfPresent("LodMin", decoded.LodMin) ||
        !object.MemberIfPresent("LodMax", decoded.LodMax) ||
        !object.OptionalMember("Compare", decoded.Compare) ||
        !object.MemberIfPresent("AnisotropyClamp", decoded.AnisotropyClamp)) {
        return false;
    }
    value = decoded;
    return true;
}

}  // namespace radray

namespace radray::render {

uint32_t GetVertexFormatSizeInBytes(VertexFormat format) noexcept {
    switch (format) {
        case VertexFormat::UINT8X2:
        case VertexFormat::SINT8X2:
        case VertexFormat::UNORM8X2:
        case VertexFormat::SNORM8X2: return 2;
        case VertexFormat::UINT8X4:
        case VertexFormat::SINT8X4:
        case VertexFormat::UNORM8X4:
        case VertexFormat::SNORM8X4:
        case VertexFormat::UINT16X2:
        case VertexFormat::SINT16X2:
        case VertexFormat::UNORM16X2:
        case VertexFormat::SNORM16X2:
        case VertexFormat::FLOAT16X2:
        case VertexFormat::UINT32:
        case VertexFormat::SINT32:
        case VertexFormat::FLOAT32: return 4;
        case VertexFormat::UINT16X4:
        case VertexFormat::SINT16X4:
        case VertexFormat::UNORM16X4:
        case VertexFormat::SNORM16X4:
        case VertexFormat::FLOAT16X4:
        case VertexFormat::UINT32X2:
        case VertexFormat::SINT32X2:
        case VertexFormat::FLOAT32X2: return 8;
        case VertexFormat::UINT32X3:
        case VertexFormat::SINT32X3:
        case VertexFormat::FLOAT32X3: return 12;
        case VertexFormat::UINT32X4:
        case VertexFormat::SINT32X4:
        case VertexFormat::FLOAT32X4: return 16;
        case VertexFormat::UNKNOWN: return 0;
    }
    Unreachable();
}

bool IsDynamicShaderParameterBindingType(ShaderParameterBindingType type) noexcept {
    return type == ShaderParameterBindingType::DynamicCBuffer ||
           type == ShaderParameterBindingType::DynamicBuffer ||
           type == ShaderParameterBindingType::DynamicRWBuffer;
}

std::string_view format_as(ShaderStage v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(ShaderBlobCategory v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(VertexFormat v) noexcept {
    switch (v) {
        case VertexFormat::UNKNOWN: return "UNKNOWN";
        case VertexFormat::UINT8X2: return "byte2";
        case VertexFormat::UINT8X4: return "byte4";
        case VertexFormat::SINT8X2: return "char2";
        case VertexFormat::SINT8X4: return "char4";
        case VertexFormat::UNORM8X2: return "unorm8x2";
        case VertexFormat::UNORM8X4: return "unorm8x4";
        case VertexFormat::SNORM8X2: return "snorm8x2";
        case VertexFormat::SNORM8X4: return "snorm8x4";
        case VertexFormat::UINT16X2: return "ushort2";
        case VertexFormat::UINT16X4: return "ushort4";
        case VertexFormat::SINT16X2: return "short2";
        case VertexFormat::SINT16X4: return "short4";
        case VertexFormat::UNORM16X2: return "unorm16x2";
        case VertexFormat::UNORM16X4: return "unorm16x4";
        case VertexFormat::SNORM16X2: return "snorm16x2";
        case VertexFormat::SNORM16X4: return "snorm16x4";
        case VertexFormat::FLOAT16X2: return "half2";
        case VertexFormat::FLOAT16X4: return "half4";
        case VertexFormat::UINT32: return "uint";
        case VertexFormat::UINT32X2: return "uint2";
        case VertexFormat::UINT32X3: return "uint3";
        case VertexFormat::UINT32X4: return "uint4";
        case VertexFormat::SINT32: return "int";
        case VertexFormat::SINT32X2: return "int2";
        case VertexFormat::SINT32X3: return "int3";
        case VertexFormat::SINT32X4: return "int4";
        case VertexFormat::FLOAT32: return "float";
        case VertexFormat::FLOAT32X2: return "float2";
        case VertexFormat::FLOAT32X3: return "float3";
        case VertexFormat::FLOAT32X4: return "float4";
    }
    Unreachable();
}

}  // namespace radray::render
