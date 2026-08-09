#pragma once

#include <optional>
#include <span>

#include <radray/render/rhi.h>
#include <radray/shader/shader_artifact.h>

namespace radray::render {

// 由 compiler-owned artifact records 组装的后端专用输入。这些类型刻意放在 rhi.h 之外，
// 让调用方无法自造第二种 layout 描述。
struct ShaderBindingLocation {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const ShaderBindingLocation&, const ShaderBindingLocation&) noexcept = default;
};

struct ShaderParameterSetLayoutEntryDescriptor {
    uint32_t Binding{0};
    ShaderParameterBindingType Type{ShaderParameterBindingType::UNKNOWN};
    uint32_t Count{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    std::optional<SamplerDescriptor> ImmutableSampler{};

    friend bool operator==(const ShaderParameterSetLayoutEntryDescriptor&, const ShaderParameterSetLayoutEntryDescriptor&) noexcept = default;
};

struct ShaderParameterSetLayoutDescriptor {
    uint32_t GroupIndex{0};
    std::span<const ShaderParameterSetLayoutEntryDescriptor> Entries{};
};

struct PushConstantDescriptor {
    ShaderBindingLocation Location{};
    uint32_t Size{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};

    friend bool operator==(const PushConstantDescriptor&, const PushConstantDescriptor&) noexcept = default;
};

struct BackendBindingName {
    string Name;
    ShaderBindingLocation Location{};
    uint32_t Namespace{0};
};

inline constexpr uint32_t GetShaderBindingNamespace(
    ShaderParameterBindingType type) noexcept {
    switch (type) {
        case ShaderParameterBindingType::CBuffer:
        case ShaderParameterBindingType::DynamicCBuffer:
            return 0;
        case ShaderParameterBindingType::Buffer:
        case ShaderParameterBindingType::TexelBuffer:
        case ShaderParameterBindingType::Texture:
        case ShaderParameterBindingType::DynamicBuffer:
            return 1;
        case ShaderParameterBindingType::RWBuffer:
        case ShaderParameterBindingType::RWTexelBuffer:
        case ShaderParameterBindingType::RWTexture:
        case ShaderParameterBindingType::DynamicRWBuffer:
            return 2;
        case ShaderParameterBindingType::Sampler:
            return 3;
        case ShaderParameterBindingType::UNKNOWN:
            return 0xffffffffu;
    }
    return 0xffffffffu;
}

struct PipelineLayoutDescriptor {
    std::span<const ShaderParameterSetLayoutDescriptor> ParameterSets{};
    std::span<const PushConstantDescriptor> PushConstants{};
};

struct BackendPipelineLayoutInput {
    vector<vector<ShaderParameterSetLayoutEntryDescriptor>> GroupEntries;
    vector<ShaderParameterSetLayoutDescriptor> ParameterSets;
    vector<BackendBindingName> BindingNames;
    vector<PushConstantDescriptor> PushConstants;
    vector<byte> SerializedRootSignature;
    uint32_t BindingGeneration{0};
    PipelineLayoutDescriptor Descriptor{};
};

std::optional<BackendPipelineLayoutInput> MakeBackendPipelineLayoutInput(
    const shader::DxilShaderArtifactView& artifact) noexcept;

std::optional<BackendPipelineLayoutInput> MakeBackendPipelineLayoutInput(
    const shader::SpirvShaderArtifactView& artifact) noexcept;

bool ValidateVertexInputStateAgainstArtifact(
    const VertexInputState& state,
    const shader::ShaderArtifactView& artifact) noexcept;

}  // namespace radray::render
