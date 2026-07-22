#pragma once

#include <optional>
#include <string_view>

#include <radray/json.h>
#include <radray/types.h>

namespace radray::render {

enum class SpirvBaseType {
    UNKNOWN,
    Void,
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float16,
    Float32,
    Float64,
    Struct,
    Image,
    SampledImage,
    Sampler,
    AccelerationStructure
};

enum class SpirvResourceKind {
    UNKNOWN,
    UniformBuffer,
    PushConstant,
    StorageBuffer,
    SampledImage,
    StorageImage,
    SeparateImage,
    SeparateSampler,
    AccelerationStructure
};

enum class SpirvImageDim {
    UNKNOWN,
    Dim1D,
    Dim2D,
    Dim3D,
    Cube,
    Buffer
};

struct SpirvTypeMember {
    string Name;
    uint32_t Offset{0};
    uint32_t Size{0};
    uint32_t TypeIndex{0};
    uint32_t ArraySize{0};
    uint32_t ArrayStride{0};
    uint32_t MatrixStride{0};
    bool RowMajor{false};
};

struct SpirvTypeInfo {
    string Name;
    vector<SpirvTypeMember> Members;
    SpirvBaseType BaseType{SpirvBaseType::UNKNOWN};
    uint32_t VectorSize{1};
    uint32_t Columns{1};
    uint32_t ArraySize{0};
    uint32_t ArrayStride{0};
    uint32_t MatrixStride{0};
    uint32_t Size{0};
    bool RowMajor{false};
};

struct SpirvImageInfo {
    SpirvImageDim Dim{SpirvImageDim::UNKNOWN};
    bool Arrayed{false};
    bool Multisampled{false};
    bool Depth{false};
    uint32_t SampledType{0};
};

struct SpirvStageIo {
    string Name;
    string HlslSemantic;
    uint32_t Location{0};
    uint32_t TypeIndex{0};
    std::optional<uint32_t> BuiltIn{};
};

struct SpirvResourceBinding {
    string Name;
    SpirvResourceKind Kind{SpirvResourceKind::UNKNOWN};
    uint32_t Set{0};
    uint32_t Binding{0};
    std::optional<uint32_t> HlslRegister{};
    std::optional<uint32_t> HlslSpace{};
    uint32_t ArraySize{0};
    uint32_t TypeIndex{0};

    std::optional<SpirvImageInfo> ImageInfo;
    uint32_t UniformBufferSize{0};
    bool ReadOnly{true};
    bool WriteOnly{false};
    bool IsViewInHlsl{false};
    string HlslType;

    bool IsUnboundedArray{false};
};

struct SpirvComputeInfo {
    uint32_t LocalSizeX{1};
    uint32_t LocalSizeY{1};
    uint32_t LocalSizeZ{1};
};

struct SpirvPushConstantRange {
    string Name;
    uint32_t Offset{0};
    uint32_t Size{0};
    uint32_t TypeIndex{0};
    bool IsViewInHlsl{false};
};

class SpirvShaderDesc {
public:
    vector<SpirvTypeInfo> Types;
    vector<SpirvStageIo> StageInputs;
    vector<SpirvStageIo> StageOutputs;
    vector<SpirvResourceBinding> ResourceBindings;
    vector<SpirvPushConstantRange> ConstantRanges;
    std::optional<SpirvComputeInfo> ComputeInfo;
};

// SpirvShaderDesc 的 JSON 序列化 (作为 ShaderBinary 内部的版本化反射载荷)。
//
// 枚举以底层整数值存储 (依赖枚举声明顺序稳定); 名字 / 绑定等以原始类型存储, 便于人读与 diff。
// 不依赖 DXC / SPIRV-Cross, 因此 DXC 关闭时预编译缓存仍可反序列化。

/// 序列化 SpirvShaderDesc 为 JSON 文本。失败返回 nullopt。
std::optional<string> SerializeSpirvShaderDesc(const SpirvShaderDesc& desc) noexcept;

/// 从 JSON 文本反序列化 SpirvShaderDesc。失败 (格式错误 / 版本不符) 返回 nullopt。
std::optional<SpirvShaderDesc> DeserializeSpirvShaderDesc(std::string_view json) noexcept;

}  // namespace radray::render

namespace radray {

template <>
struct JsonSerializer<render::SpirvTypeMember> {
    static bool Write(JsonWriteContext& context, const render::SpirvTypeMember& value) noexcept;
};

template <>
struct JsonSerializer<render::SpirvTypeInfo> {
    static bool Write(JsonWriteContext& context, const render::SpirvTypeInfo& value) noexcept;
};

template <>
struct JsonSerializer<render::SpirvImageInfo> {
    static bool Write(JsonWriteContext& context, const render::SpirvImageInfo& value) noexcept;
};

template <>
struct JsonSerializer<render::SpirvStageIo> {
    static bool Write(JsonWriteContext& context, const render::SpirvStageIo& value) noexcept;
};

template <>
struct JsonSerializer<render::SpirvResourceBinding> {
    static bool Write(JsonWriteContext& context, const render::SpirvResourceBinding& value) noexcept;
};

template <>
struct JsonSerializer<render::SpirvComputeInfo> {
    static bool Write(JsonWriteContext& context, const render::SpirvComputeInfo& value) noexcept;
};

template <>
struct JsonSerializer<render::SpirvPushConstantRange> {
    static bool Write(JsonWriteContext& context, const render::SpirvPushConstantRange& value) noexcept;
};

template <>
struct JsonSerializer<render::SpirvShaderDesc> {
    static bool Write(JsonWriteContext& context, const render::SpirvShaderDesc& value) noexcept;
};

}  // namespace radray
