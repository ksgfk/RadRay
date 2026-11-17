#pragma once

// 布局文件与类型跨度见 docs/architecture/shader-pipeline.md。

#include <optional>
#include <span>
#include <string_view>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/gpu/types.h>
#include <radray/shader/shader_compiler_contract.h>

namespace radray::shader {

struct ShaderTypeRecord {
    string Name;
    std::optional<uint32_t> ParentIndex;
    ShaderTypeKind Kind{ShaderTypeKind::Struct};
    ShaderTypeKind ElementKind{ShaderTypeKind::Struct};
    uint32_t ElementCount{1};
    uint32_t Offset{0};
    uint32_t Size{0};
    uint32_t ElementSize{0};
    uint32_t Stride{0};
    uint32_t MatrixStride{0};
    std::optional<uint32_t> TypeIndex;
    ShaderScalarKind ScalarKind{ShaderScalarKind::None};
    uint32_t RowCount{0};
    uint32_t ColumnCount{0};
    bool RowMajor{false};
};

class ShaderResolvedMember {
public:
    ShaderTypeKind Kind{ShaderTypeKind::Scalar};
    ShaderTypeKind ElementKind{ShaderTypeKind::Scalar};
    ShaderScalarKind ScalarKind{ShaderScalarKind::None};
    uint32_t AbsoluteOffset{0};
    uint32_t Size{0};
    uint32_t ElementSize{0};
    uint32_t ElementCount{1};
    uint32_t ArrayStride{0};
    uint32_t MatrixStride{0};
    uint32_t RowCount{0};
    uint32_t ColumnCount{0};
    bool RowMajor{false};
    std::optional<uint32_t> TypeIndex;

    uint32_t VectorPayloadSize() const noexcept;
    bool HasInternalPadding() const noexcept;

private:
    friend std::optional<ShaderResolvedMember> FindShaderTypeMember(std::span<const ShaderTypeRecord>, uint32_t, std::string_view);
    bool _hasInternalPadding{false};
};

struct ShaderBindingDesc {
    string Name;
    gpu::BindingDescriptor Binding;
    std::optional<uint32_t> TypeIndex;
    uint32_t BufferByteSize{0};
};

struct ShaderBindingGroupDesc {
    string Name;
    uint32_t GroupSlot{0};
    vector<ShaderBindingDesc> Bindings;
};

struct ShaderPushConstantDesc {
    string Name;
    gpu::PushConstantDescriptor Push;
    std::optional<uint32_t> TypeIndex;
};

struct ShaderPipelineLayoutDesc {
    vector<ShaderBindingGroupDesc> Groups;
    vector<ShaderPushConstantDesc> PushConstants;
    vector<shader::ShaderTypeRecord> Types;
};

struct ShaderVertexAttributeSignatureDesc {
    string Semantic;
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    shader::ShaderVertexComponentType ComponentKind{shader::ShaderVertexComponentType::Float};
    uint32_t ComponentCount{0};
};

struct ShaderBindingReference {
    uint32_t GroupSlot{0};
    uint32_t BindingId{0};
};

struct ShaderVariantTargetInterfaceDesc {
    gpu::ShaderStages Stages{gpu::ShaderStage::UNKNOWN};
    vector<ShaderVertexAttributeSignatureDesc> VertexInputs;
    vector<ShaderBindingReference> ActiveBindings;
    vector<uint32_t> ActivePushConstants;
};

struct ShaderVariantInterfaceDesc {
    string Name;
    std::optional<ShaderVariantTargetInterfaceDesc> DXIL;
    std::optional<ShaderVariantTargetInterfaceDesc> SPIRV;
};

struct ShaderFamilyLayoutDesc {
    ShaderPipelineLayoutDesc SharedLayout;
    vector<ShaderVariantInterfaceDesc> Variants;
};

// 空诊断指针表示不收集错误说明；所有索引、跨度及类型引用在这里验证。
bool ValidateShaderPipelineLayoutDesc(const ShaderPipelineLayoutDesc& desc, Nullable<string*> diagnostic = nullptr);
bool ValidateShaderVariantInterfaceDesc(const ShaderVariantInterfaceDesc& desc, Nullable<const ShaderPipelineLayoutDesc*> layout = nullptr, Nullable<string*> diagnostic = nullptr);
bool ValidateShaderFamilyLayoutDesc(const ShaderFamilyLayoutDesc& desc, Nullable<string*> diagnostic = nullptr);

Nullable<const ShaderVariantTargetInterfaceDesc*> GetShaderTargetInterface(const ShaderVariantInterfaceDesc& desc, gpu::RenderBackend backend) noexcept;

// 文件格式版本独立于编译器 metadata。读取失败保留 output；写入无效描述返回空。
std::optional<vector<byte>> WriteShaderPipelineLayoutBinary(const ShaderPipelineLayoutDesc& desc);
bool ReadShaderPipelineLayoutBinary(std::span<const byte> bytes, ShaderPipelineLayoutDesc& output);
std::optional<vector<byte>> WriteShaderVariantInterfaceBinary(const ShaderVariantInterfaceDesc& desc);
bool ReadShaderVariantInterfaceBinary(std::span<const byte> bytes, ShaderVariantInterfaceDesc& output);
std::optional<vector<byte>> WriteShaderFamilyLayoutBinary(const ShaderFamilyLayoutDesc& desc);
bool ReadShaderFamilyLayoutBinary(std::span<const byte> bytes, ShaderFamilyLayoutDesc& output);
std::optional<string> WriteShaderPipelineLayoutJson(const ShaderPipelineLayoutDesc& desc);
bool ReadShaderPipelineLayoutJson(std::string_view text, ShaderPipelineLayoutDesc& output);
std::optional<string> WriteShaderVariantInterfaceJson(const ShaderVariantInterfaceDesc& desc);
bool ReadShaderVariantInterfaceJson(std::string_view text, ShaderVariantInterfaceDesc& output);
std::optional<string> WriteShaderFamilyLayoutJson(const ShaderFamilyLayoutDesc& desc);
bool ReadShaderFamilyLayoutJson(std::string_view text, ShaderFamilyLayoutDesc& output);

bool ValidateShaderTypeRecords(std::span<const ShaderTypeRecord> types, Nullable<string*> diagnostic = nullptr);
// Requires validated types. The result does not borrow the path or type records.
std::optional<ShaderResolvedMember> FindShaderTypeMember(std::span<const ShaderTypeRecord> types, uint32_t rootIndex, std::string_view path);

}  // namespace radray::shader
