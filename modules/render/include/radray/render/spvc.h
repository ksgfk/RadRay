#pragma once

#include <span>
#include <string_view>
#include <optional>

#include <radray/render/common.h>

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

struct SpirvBytecodeView {
    std::span<const byte> Data;
    std::string_view EntryPointName;
    ShaderStage Stage{ShaderStage::UNKNOWN};
};

struct SpirvTypeMember {
    string Name;
    uint32_t Offset{0};
    uint32_t Size{0};
    uint32_t TypeIndex{0};
};

struct SpirvTypeInfo {
    string Name;
    SpirvBaseType BaseType{SpirvBaseType::UNKNOWN};
    uint32_t VectorSize{1};
    uint32_t Columns{1};
    uint32_t ArraySize{0};
    uint32_t ArrayStride{0};
    uint32_t MatrixStride{0};
    bool RowMajor{false};
    uint32_t Size{0};
    vector<SpirvTypeMember> Members;
};

struct SpirvImageInfo {
    SpirvImageDim Dim{SpirvImageDim::UNKNOWN};
    bool Arrayed{false};
    bool Multisampled{false};
    bool Depth{false};
    uint32_t SampledType{0};
};

struct SpirvVertexInput {
    uint32_t Location{0};
    string Name;
    uint32_t TypeIndex{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};

struct SpirvResourceBinding {
    string Name;
    SpirvResourceKind Kind{SpirvResourceKind::UNKNOWN};
    uint32_t Set{0};
    uint32_t Binding{0};
    uint32_t ArraySize{0};
    uint32_t TypeIndex{0};
    ShaderStage Stages{ShaderStage::UNKNOWN};

    std::optional<SpirvImageInfo> ImageInfo;
    uint32_t UniformBufferSize{0};
    bool ReadOnly{true};
    bool WriteOnly{false};
    bool IsViewInHlsl{false};

    bool IsUnboundedArray{false};

    ResourceBindType MapResourceBindType() const noexcept;
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
    ShaderStage Stages{ShaderStage::UNKNOWN};
    bool IsViewInHlsl{false};
};

class SpirvShaderDesc {
public:
    vector<SpirvTypeInfo> Types;
    vector<SpirvVertexInput> VertexInputs;
    vector<SpirvResourceBinding> ResourceBindings;
    vector<SpirvPushConstantRange> PushConstants;
    std::optional<SpirvComputeInfo> ComputeInfo;
    ShaderStage UsedStages{ShaderStage::UNKNOWN};
};

enum class MslPlatform {
    MacOS,
    IOS
};

struct SpirvToMslOption {
    uint32_t MslMajor{2};
    uint32_t MslMinor{0};
    uint32_t MslPatch{0};
    MslPlatform Platform{MslPlatform::MacOS};
    bool UseArgumentBuffers{false};
    bool ForceNativeArrays{false};
};

struct SpirvToMslOutput {
    string MslSource;
    string EntryPointName;
};

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

namespace radray::render {

class DxcReflectionRadrayExt;

std::optional<SpirvShaderDesc> ReflectSpirv(std::span<const SpirvBytecodeView> bytecodes, std::span<const DxcReflectionRadrayExt*> extInfos);

std::optional<SpirvToMslOutput> ConvertSpirvToMsl(
    std::span<const byte> spirvData,
    std::string_view entryPoint,
    ShaderStage stage,
    const SpirvToMslOption& option = {});

}  // namespace radray::render

#endif
