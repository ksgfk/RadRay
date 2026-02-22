#pragma once

#include <span>
#include <optional>

#include <radray/types.h>
#include <radray/render/common.h>

namespace radray::render {

enum class MslDataType : uint32_t {
    None = 0,
    Struct,
    Array,

    Float,
    Float2,
    Float3,
    Float4,
    Float2x2,
    Float2x3,
    Float2x4,
    Float3x2,
    Float3x3,
    Float3x4,
    Float4x2,
    Float4x3,
    Float4x4,

    Half,
    Half2,
    Half3,
    Half4,
    Half2x2,
    Half2x3,
    Half2x4,
    Half3x2,
    Half3x3,
    Half3x4,
    Half4x2,
    Half4x3,
    Half4x4,

    Int,
    Int2,
    Int3,
    Int4,

    UInt,
    UInt2,
    UInt3,
    UInt4,

    Short,
    Short2,
    Short3,
    Short4,

    UShort,
    UShort2,
    UShort3,
    UShort4,

    Char,
    Char2,
    Char3,
    Char4,

    UChar,
    UChar2,
    UChar3,
    UChar4,

    Bool,
    Bool2,
    Bool3,
    Bool4,

    Long,
    Long2,
    Long3,
    Long4,

    ULong,
    ULong2,
    ULong3,
    ULong4,

    Texture,
    Sampler,
    Pointer,
};

enum class MslArgumentType : uint8_t {
    Buffer,
    Texture,
    Sampler,
    ThreadgroupMemory,
};

enum class MslAccess : uint8_t {
    ReadOnly,
    ReadWrite,
    WriteOnly,
};

enum class MslTextureType : uint8_t {
    Tex1D,
    Tex1DArray,
    Tex2D,
    Tex2DArray,
    Tex2DMS,
    Tex3D,
    TexCube,
    TexCubeArray,
    TexBuffer,
};

enum class MslStage : uint8_t {
    Vertex,
    Fragment,
    Compute,
};

class MslStructMember {
public:
    string Name;
    uint64_t Offset{0};
    MslDataType DataType{MslDataType::None};
    uint32_t StructTypeIndex{UINT32_MAX};
    uint32_t ArrayTypeIndex{UINT32_MAX};
};

class MslStructType {
public:
    vector<MslStructMember> Members;
};

class MslArrayType {
public:
    MslDataType ElementType{MslDataType::None};
    uint64_t ArrayLength{0};
    uint64_t Stride{0};
    uint32_t ElementStructTypeIndex{UINT32_MAX};
    uint32_t ElementArrayTypeIndex{UINT32_MAX};
};

class MslArgument {
public:
    string Name;
    MslStage Stage{MslStage::Compute};
    MslArgumentType Type{MslArgumentType::Buffer};
    MslAccess Access{MslAccess::ReadOnly};
    uint32_t Index{0};
    bool IsActive{false};
    uint64_t ArrayLength{0};
    bool IsPushConstant{false};
    uint32_t DescriptorSet{0};

    uint64_t BufferAlignment{0};
    uint64_t BufferDataSize{0};
    MslDataType BufferDataType{MslDataType::None};
    uint32_t BufferStructTypeIndex{UINT32_MAX};

    MslTextureType TextureType{MslTextureType::Tex2D};
    MslDataType TextureDataType{MslDataType::None};
    bool IsDepthTexture{false};
};

class MslShaderReflection {
public:
    vector<MslArgument> Arguments;
    vector<MslStructType> StructTypes;
    vector<MslArrayType> ArrayTypes;
};

struct MslReflectParams {
    std::span<const byte> SpirV;
    std::string_view EntryPoint;
    ShaderStage Stage;
    bool UseArgumentBuffers{false};
};

#ifdef RADRAY_ENABLE_SPIRV_CROSS
std::optional<MslShaderReflection> ReflectMsl(std::span<const MslReflectParams> msls);
#endif

std::string_view format_as(MslDataType v) noexcept;
std::string_view format_as(MslArgumentType v) noexcept;
std::string_view format_as(MslAccess v) noexcept;
std::string_view format_as(MslTextureType v) noexcept;
std::string_view format_as(MslStage v) noexcept;

}  // namespace radray::render
