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

// 类型成员信息（用于结构体字段）
struct SpirvTypeMember {
    string Name;            // 成员名称
    uint32_t Offset{0};     // 在结构体中的偏移量（字节）
    uint32_t Size{0};       // 成员大小（字节）
    uint32_t TypeIndex{0};  // 指向类型表的索引
};

// 完整类型信息（支持递归类型定义）
struct SpirvTypeInfo {
    string Name;  // 类型名称
    SpirvBaseType BaseType{SpirvBaseType::UNKNOWN};
    uint32_t VectorSize{1};           // 向量大小（1=标量，2-4=向量）
    uint32_t Columns{1};              // 矩阵列数（1=非矩阵）
    uint32_t ArraySize{0};            // 数组大小（0=非数组，~0u=动态数组）
    uint32_t ArrayStride{0};          // 数组元素间距（字节）
    uint32_t MatrixStride{0};         // 矩阵列间距（字节）
    bool RowMajor{false};             // 矩阵是否行主序
    uint32_t Size{0};                 // 类型总大小（字节）
    vector<SpirvTypeMember> Members;  // 结构体成员（仅当BaseType为Struct时）
};

// 图像类型详细信息
struct SpirvImageInfo {
    SpirvImageDim Dim{SpirvImageDim::UNKNOWN};
    bool Arrayed{false};       // 是否为数组纹理
    bool Multisampled{false};  // 是否为多重采样纹理
    bool Depth{false};         // 是否为深度纹理
    uint32_t SampledType{0};   // 采样类型索引（指向类型表）
};

// 顶点输入属性
struct SpirvVertexInput {
    uint32_t Location{0};                        // 输入位置
    string Name;                                 // 变量名称
    uint32_t TypeIndex{0};                       // 类型索引
    VertexFormat Format{VertexFormat::UNKNOWN};  // 推断的顶点格式（如果可能）
};

// 资源绑定信息
struct SpirvResourceBinding {
    string Name;  // 资源名称
    SpirvResourceKind Kind{SpirvResourceKind::UNKNOWN};
    uint32_t Set{0};                           // 描述符集合（Vulkan）/ Space（D3D12）
    uint32_t Binding{0};                       // 绑定点
    uint32_t ArraySize{0};                     // 数组大小（0=非数组，~0u=无界数组）
    uint32_t TypeIndex{0};                     // 类型索引（指向类型表）
    ShaderStage Stages{ShaderStage::UNKNOWN};  // 使用此资源的着色器阶段

    // 以下字段针对特定资源类型
    std::optional<SpirvImageInfo> ImageInfo;  // 图像/纹理信息
    bool ReadOnly{true};                      // 是否只读（针对缓冲区和图像）
    bool WriteOnly{false};                    // 是否只写
};

// Compute Shader 工作组信息
struct SpirvComputeInfo {
    uint32_t LocalSizeX{1};  // X维度工作组大小
    uint32_t LocalSizeY{1};  // Y维度工作组大小
    uint32_t LocalSizeZ{1};  // Z维度工作组大小
};

// Push Constant 信息
struct SpirvPushConstantRange {
    string Name;            // Push Constant 块名称
    uint32_t Offset{0};     // 偏移量（字节）
    uint32_t Size{0};       // 大小（字节）
    uint32_t TypeIndex{0};  // 类型索引
    ShaderStage Stages{ShaderStage::UNKNOWN};
};

// 完整的着色器描述
class SpirvShaderDesc {
public:
    // 类型表（所有类型信息的集中存储）
    vector<SpirvTypeInfo> Types;

    // 顶点输入（仅Vertex Shader）
    vector<SpirvVertexInput> VertexInputs;

    // 资源绑定
    vector<SpirvResourceBinding> ResourceBindings;

    // Push Constants
    vector<SpirvPushConstantRange> PushConstants;

    // Compute信息（仅Compute Shader）
    std::optional<SpirvComputeInfo> ComputeInfo;

    // 使用的着色器阶段
    ShaderStage UsedStages{ShaderStage::UNKNOWN};

    // 辅助方法：根据索引获取类型
    const SpirvTypeInfo* GetType(uint32_t index) const {
        if (index < Types.size()) {
            return &Types[index];
        }
        return nullptr;
    }
};

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

namespace radray::render {

std::optional<SpirvShaderDesc> ReflectSpirv(std::span<const SpirvBytecodeView> bytecodes);

}  // namespace radray::render

#endif
