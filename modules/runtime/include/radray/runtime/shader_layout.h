#pragma once

// 布局文件、类型跨度和资源所有权见 docs/architecture/runtime.md。

#include <optional>
#include <span>
#include <string_view>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/rhi.h>
#include <radray/shader/shader_type_tree.h>

namespace radray {

struct ShaderBindingDesc {
    string Name;
    render::BindingDescriptor Binding;
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
    render::PushConstantDescriptor Push;
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

struct ShaderTargetProgramInterfaceDesc {
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
    vector<ShaderVertexAttributeSignatureDesc> VertexInputs;
    vector<ShaderBindingReference> ActiveBindings;
    vector<uint32_t> ActivePushConstants;
};

struct ShaderProgramInterfaceDesc {
    string Name;
    std::optional<ShaderTargetProgramInterfaceDesc> DXIL;
    std::optional<ShaderTargetProgramInterfaceDesc> SPIRV;
};

struct ShaderFamilyLayoutDesc {
    ShaderPipelineLayoutDesc SharedLayout;
    vector<ShaderProgramInterfaceDesc> Variants;
};

// 空诊断指针表示不收集错误说明；所有索引、跨度及类型引用在这里验证。
bool ValidateShaderPipelineLayoutDesc(const ShaderPipelineLayoutDesc& desc, Nullable<string*> diagnostic = nullptr);
bool ValidateShaderProgramInterfaceDesc(const ShaderProgramInterfaceDesc& desc, Nullable<const ShaderPipelineLayoutDesc*> layout = nullptr, Nullable<string*> diagnostic = nullptr);
bool ValidateShaderFamilyLayoutDesc(const ShaderFamilyLayoutDesc& desc, Nullable<string*> diagnostic = nullptr);

Nullable<const ShaderTargetProgramInterfaceDesc*> GetShaderTargetInterface(const ShaderProgramInterfaceDesc& desc, render::RenderBackend backend) noexcept;

// 文件格式版本独立于编译器 metadata。读取失败保留 output；写入无效描述返回空。
std::optional<vector<byte>> WriteShaderPipelineLayoutBinary(const ShaderPipelineLayoutDesc& desc);
bool ReadShaderPipelineLayoutBinary(std::span<const byte> bytes, ShaderPipelineLayoutDesc& output);
std::optional<vector<byte>> WriteShaderProgramInterfaceBinary(const ShaderProgramInterfaceDesc& desc);
bool ReadShaderProgramInterfaceBinary(std::span<const byte> bytes, ShaderProgramInterfaceDesc& output);
std::optional<vector<byte>> WriteShaderFamilyLayoutBinary(const ShaderFamilyLayoutDesc& desc);
bool ReadShaderFamilyLayoutBinary(std::span<const byte> bytes, ShaderFamilyLayoutDesc& output);
std::optional<string> WriteShaderPipelineLayoutJson(const ShaderPipelineLayoutDesc& desc);
bool ReadShaderPipelineLayoutJson(std::string_view text, ShaderPipelineLayoutDesc& output);
std::optional<string> WriteShaderProgramInterfaceJson(const ShaderProgramInterfaceDesc& desc);
bool ReadShaderProgramInterfaceJson(std::string_view text, ShaderProgramInterfaceDesc& output);
std::optional<string> WriteShaderFamilyLayoutJson(const ShaderFamilyLayoutDesc& desc);
bool ReadShaderFamilyLayoutJson(std::string_view text, ShaderFamilyLayoutDesc& output);

struct ShaderBindingLocation {
    uint32_t GroupSlot{0};
    uint32_t BindingId{0};
    render::BindingKind Kind{render::BindingKind::CBuffer};
    uint32_t Count{0};
    std::optional<uint32_t> TypeIndex;
    uint32_t BufferByteSize{0};
    bool ImmutableSampler{false};
    bool ActiveOnDevice{false};
};

struct ShaderPushConstantLocation {
    uint32_t PushId{0};
    uint32_t Size{0};
    std::optional<uint32_t> TypeIndex;
    bool ActiveOnDevice{false};
};

}  // namespace radray
