#pragma once

#include <optional>
#include <string_view>

#include <radray/enum_flags.h>
#include <radray/types.h>

// radrayshader 的类型基座: manifest 的数据词汇。
//
// == 判定标准: 是不是 manifest 的内容 ==
//
// 本文件只收 **出现在 *.shader.json 里、由 shader 格式层解析或序列化** 的类型。
// 每一个都能在 shader_manifest 中找到对应的字段或 JSON codec:
//
//   ShaderStage / ShaderBlobCategory       编译阶段与字节码类型, 贯穿整个工具链
//   ShaderParameterBindingType             ShaderBindingDesc::Type
//   VertexFormat / VertexStepMode          ShaderVertex{Attribute,Buffer}Desc 的字段
//   ShaderBindingLocation                  ShaderPushConstantDesc::Location (有 codec)
//   SamplerDescriptor                      ShaderBindingDesc::ImmutableSampler (有 codec)
//   AddressMode / FilterMode /             SamplerDescriptor 的成员, 随其被序列化
//     CompareFunction                      (故自身无直接引用, 但是真实依赖)
//
// == 为什么这些类型在 shader 库而不在 render 库 ==
//
// 依赖链是 core <- shader <- render <- runtime。shader 编译器 (dxc/hlsl/spirv/spvc)
// 与格式层 (shader_manifest) 都不碰 GPU 设备。若这批 manifest 词汇放在 render,
// 只想 cook shader 的工具就会被迫链入整个图形后端 (实测约 23 MB 的 d3d12/vulkan obj)。
//
// 【计算依赖闭包时必须排除枚举成员】ShaderParameterBindingType 的成员名叫 Buffer /
// Texture / Sampler, 与 device class 同名。若把枚举成员当成类型引用, 闭包会经
// ShaderParameterBindingType -> Buffer -> Device 污染到几乎整个 rhi.h (实测 110/133),
// 从而误判拆库不可行。枚举成员是值, 不是类型依赖。
//
// == 命名空间 ==
//
// 刻意保持 radray::render, 与 rhi.h 一致。两个库共同实现 render 这一概念层, 拆库是
// 物理构建边界而非概念重命名; 改名会让 ShaderStage 这类跨库使用的类型出现割裂, 且要
// 改动全仓库每一处 render:: 限定。

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
