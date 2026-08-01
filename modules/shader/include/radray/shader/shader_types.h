#pragma once

#include <optional>
#include <string_view>

#include <radray/enum_flags.h>
#include <radray/json.h>
#include <radray/types.h>

// radrayshader 的类型基座: manifest 的数据词汇。
//
// 收录标准是"是不是 manifest 的内容" —— 只放出现在 *.shader.json 里或有 JSON codec 的
// 类型。只为喂给 RHI 而存在的类型留在 rhi.h。命名空间刻意保持 radray::render。
// 完整判据、放弃过的判据、以及算依赖闭包时的枚举成员陷阱:
// docs/adr/0006-shader-types-layer-boundary.md

namespace radray::render {

enum class ShaderStage : uint32_t {
    UNKNOWN = 0x0,
    Vertex = 0x1,
    Pixel = Vertex << 1,
    Compute = Pixel << 1,
    Graphics = Vertex | Pixel,
};

enum class ShaderBlobCategory : int32_t {
    DXIL,
    SPIRV,
    MSL,
    METALLIB,
};

enum class AddressMode : int32_t {
    ClampToEdge,
    Repeat,
    Mirror
};

enum class FilterMode : int32_t {
    Nearest,
    Linear
};

enum class CompareFunction : int32_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

enum class VertexStepMode : int32_t {
    Vertex,
    Instance
};

enum class VertexFormat : int32_t {
    UNKNOWN,

    UINT8X2,
    UINT8X4,
    SINT8X2,
    SINT8X4,
    UNORM8X2,
    UNORM8X4,
    SNORM8X2,
    SNORM8X4,
    UINT16X2,
    UINT16X4,
    SINT16X2,
    SINT16X4,
    UNORM16X2,
    UNORM16X4,
    SNORM16X2,
    SNORM16X4,
    FLOAT16X2,
    FLOAT16X4,
    UINT32,
    UINT32X2,
    UINT32X3,
    UINT32X4,
    SINT32,
    SINT32X2,
    SINT32X3,
    SINT32X4,
    FLOAT32,
    FLOAT32X2,
    FLOAT32X3,
    FLOAT32X4,
};

enum class ShaderParameterBindingType : int32_t {
    UNKNOWN,
    CBuffer,
    Buffer,
    RWBuffer,
    TexelBuffer,
    RWTexelBuffer,
    Texture,
    RWTexture,
    DynamicCBuffer,
    DynamicBuffer,
    DynamicRWBuffer,
    Sampler,
};

}  // namespace radray::render

namespace radray {

template <>
struct is_flags<render::ShaderStage> : public std::true_type {};
template <>
struct is_compound_enum_flags<render::ShaderStage> : public std::true_type {};

namespace render {

using ShaderStages = EnumFlags<render::ShaderStage>;

}  // namespace render

}  // namespace radray

namespace radray::render {

struct SamplerDescriptor {
    AddressMode AddressS{};
    AddressMode AddressT{};
    AddressMode AddressR{};
    FilterMode MinFilter{};
    FilterMode MagFilter{};
    FilterMode MipmapFilter{};
    float LodMin{0.0f};
    float LodMax{0.0f};
    std::optional<CompareFunction> Compare{};
    uint32_t AnisotropyClamp{0};

    friend bool operator==(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept = default;
    friend bool operator!=(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept = default;
};

struct ShaderBindingLocation {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const ShaderBindingLocation&, const ShaderBindingLocation&) noexcept = default;
};

// --------------------------- Utility Functions ---------------------------
uint32_t GetVertexFormatSizeInBytes(VertexFormat format) noexcept;
bool IsDynamicShaderParameterBindingType(ShaderParameterBindingType type) noexcept;
// -------------------------------------------------------------------------

std::string_view format_as(ShaderStage v) noexcept;
std::string_view format_as(ShaderBlobCategory v) noexcept;
std::string_view format_as(VertexFormat v) noexcept;

}  // namespace radray::render

namespace radray {

template <>
struct JsonSerializer<render::ShaderBindingLocation> {
    static bool Write(JsonWriteContext& context, const render::ShaderBindingLocation& value) noexcept;
};

template <>
struct JsonDeserializer<render::ShaderBindingLocation> {
    static bool Read(const JsonValue& json, render::ShaderBindingLocation& value) noexcept;
};

template <>
struct JsonSerializer<render::SamplerDescriptor> {
    static bool Write(JsonWriteContext& context, const render::SamplerDescriptor& value) noexcept;
};

template <>
struct JsonDeserializer<render::SamplerDescriptor> {
    static bool Read(const JsonValue& json, render::SamplerDescriptor& value) noexcept;
};

}  // namespace radray
