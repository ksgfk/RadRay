#pragma once

#include <optional>

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

struct SpirvVertexInput {
    string Name;
    uint32_t Location{0};
    uint32_t TypeIndex{0};
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
    vector<SpirvVertexInput> VertexInputs;
    vector<SpirvResourceBinding> ResourceBindings;
    vector<SpirvPushConstantRange> PushConstants;
    std::optional<SpirvComputeInfo> ComputeInfo;
};

}  // namespace radray::render
