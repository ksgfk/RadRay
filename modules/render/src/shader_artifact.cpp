#include <radray/render/backend/pipeline_layout_types.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>

namespace radray::render {
namespace {

std::optional<BackendPipelineLayoutInput> MakeBackendPipelineLayoutInputForTarget(
    const shader::ShaderArtifactView& artifact,
    shader::ShaderTarget target,
    const ShaderLayoutPolicy& policy) noexcept {
    if (artifact.Envelope().Target != static_cast<uint8_t>(target)) {
        return std::nullopt;
    }
    if (target == shader::ShaderTarget::SPIRV && !artifact.SerializedRootSignature().empty()) {
        return std::nullopt;
    }
    if (!policy.Empty() && !artifact.SerializedRootSignature().empty()) {
        return std::nullopt;
    }
    for (size_t index = 0; index < policy.DynamicBufferGroups.size(); ++index) {
        const uint32_t group = policy.DynamicBufferGroups[index];
        if (std::find(
                policy.DynamicBufferGroups.begin(),
                policy.DynamicBufferGroups.begin() + index,
                group) != policy.DynamicBufferGroups.begin() + index) {
            return std::nullopt;
        }
        const bool exists = std::any_of(
            artifact.Bindings().begin(),
            artifact.Bindings().end(),
            [group](const shader::WireBindingRecord& binding) noexcept {
                return binding.Group == group;
            });
        if (!exists) {
            return std::nullopt;
        }
    }
    BackendPipelineLayoutInput result;
    static std::atomic<uint32_t> nextBindingGeneration{1};
    result.BindingGeneration = nextBindingGeneration.fetch_add(1, std::memory_order_relaxed);
    if (result.BindingGeneration == 0) {
        result.BindingGeneration = nextBindingGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t maxGroup = 0;
    bool hasBindings = false;
    for (const shader::WireBindingRecord& binding : artifact.Bindings()) {
        maxGroup = std::max(maxGroup, binding.Group);
        hasBindings = true;
    }
    if (hasBindings) {
        if (maxGroup > 64) {
            return std::nullopt;
        }
        result.GroupEntries.resize(maxGroup + 1);
    }
    for (const shader::WireBindingRecord& binding : artifact.Bindings()) {
        ShaderParameterBindingType type = ShaderParameterBindingType::UNKNOWN;
        switch (binding.Type) {
            case 1:
                type = std::find(
                           policy.DynamicBufferGroups.begin(),
                           policy.DynamicBufferGroups.end(),
                           binding.Group) != policy.DynamicBufferGroups.end()
                           ? ShaderParameterBindingType::DynamicCBuffer
                           : ShaderParameterBindingType::CBuffer;
                break;
            case 2: type = ShaderParameterBindingType::Buffer; break;
            case 3: type = ShaderParameterBindingType::RWBuffer; break;
            case 4: type = ShaderParameterBindingType::Texture; break;
            case 5: type = ShaderParameterBindingType::RWTexture; break;
            case 6: type = ShaderParameterBindingType::Sampler; break;
            default: return std::nullopt;
        }
        const std::optional<std::string_view> bindingName = artifact.GetName(binding.Name);
        if (!bindingName.has_value()) {
            return std::nullopt;
        }
        result.BindingNames.push_back({.Name = string{bindingName.value()},
                                       .Location = {binding.Group, binding.Binding},
                                       .Namespace = shader::GetWireBindingNamespace(binding.Type)});
        ShaderParameterSetLayoutEntryDescriptor entry{
            .Binding = binding.Binding,
            .Type = type,
            .Count = binding.Count,
            .Stages = ShaderStages{static_cast<ShaderStage>(binding.StageMask)},
            .ImmutableSampler = (binding.Flags & 1u) != 0 ? std::optional<SamplerDescriptor>{SamplerDescriptor{}} : std::nullopt};
        result.GroupEntries[binding.Group].push_back(entry);
    }
    for (uint32_t group = 0; group < result.GroupEntries.size(); ++group) {
        result.ParameterSets.push_back({group, result.GroupEntries[group]});
    }
    if (target == shader::ShaderTarget::SPIRV && artifact.RootConstants().size() > 1) {
        return std::nullopt;
    }
    result.PushConstants.reserve(artifact.RootConstants().size());
    for (const shader::WireRootConstantRecord& constant : artifact.RootConstants()) {
        result.PushConstants.push_back(PushConstantDescriptor{
            .Location = {constant.RegisterSpace, constant.Register},
            .Size = constant.Size,
            .Stages = ShaderStages{static_cast<ShaderStage>(constant.StageMask)}});
    }
    result.Descriptor = PipelineLayoutDescriptor{
        .ParameterSets = result.ParameterSets,
        .PushConstants = result.PushConstants};
    const std::span<const byte> serializedRootSignature = artifact.SerializedRootSignature();
    result.SerializedRootSignature.assign(
        serializedRootSignature.begin(), serializedRootSignature.end());
    return result;
}

bool GetVertexFormatShape(
    VertexFormat format,
    uint32_t& componentType,
    uint32_t& componentCount) noexcept {
    switch (format) {
        case VertexFormat::UINT8X2:
        case VertexFormat::UINT16X2:
        case VertexFormat::UINT32X2:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::UnsignedInteger);
            componentCount = 2;
            return true;
        case VertexFormat::UINT32X3:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::UnsignedInteger);
            componentCount = 3;
            return true;
        case VertexFormat::UINT8X4:
        case VertexFormat::UINT16X4:
        case VertexFormat::UINT32X4:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::UnsignedInteger);
            componentCount = 4;
            return true;
        case VertexFormat::UINT32:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::UnsignedInteger);
            componentCount = 1;
            return true;
        case VertexFormat::SINT8X2:
        case VertexFormat::SINT16X2:
        case VertexFormat::SINT32X2:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::SignedInteger);
            componentCount = 2;
            return true;
        case VertexFormat::SINT32X3:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::SignedInteger);
            componentCount = 3;
            return true;
        case VertexFormat::SINT8X4:
        case VertexFormat::SINT16X4:
        case VertexFormat::SINT32X4:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::SignedInteger);
            componentCount = 4;
            return true;
        case VertexFormat::SINT32:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::SignedInteger);
            componentCount = 1;
            return true;
        case VertexFormat::UNORM8X2:
        case VertexFormat::SNORM8X2:
        case VertexFormat::UNORM16X2:
        case VertexFormat::SNORM16X2:
        case VertexFormat::FLOAT16X2:
        case VertexFormat::FLOAT32X2:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::Float);
            componentCount = 2;
            return true;
        case VertexFormat::FLOAT32X3:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::Float);
            componentCount = 3;
            return true;
        case VertexFormat::UNORM8X4:
        case VertexFormat::SNORM8X4:
        case VertexFormat::UNORM16X4:
        case VertexFormat::SNORM16X4:
        case VertexFormat::FLOAT16X4:
        case VertexFormat::FLOAT32X4:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::Float);
            componentCount = 4;
            return true;
        case VertexFormat::FLOAT32:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::Float);
            componentCount = 1;
            return true;
        default:
            return false;
    }
}

}  // namespace

std::optional<BackendPipelineLayoutInput> MakeBackendPipelineLayoutInput(
    const shader::DxilShaderArtifactView& artifact,
    const ShaderLayoutPolicy& policy) noexcept {
    return MakeBackendPipelineLayoutInputForTarget(artifact.Generic(), shader::ShaderTarget::DXIL, policy);
}

std::optional<BackendPipelineLayoutInput> MakeBackendPipelineLayoutInput(
    const shader::SpirvShaderArtifactView& artifact,
    const ShaderLayoutPolicy& policy) noexcept {
    return MakeBackendPipelineLayoutInputForTarget(artifact.Generic(), shader::ShaderTarget::SPIRV, policy);
}

bool ValidateVertexInputStateAgainstArtifact(
    const VertexInputState& state,
    const shader::ShaderArtifactView& artifact) noexcept {
    if (!ValidateVertexInputState(state) ||
        state.Attributes.size() != artifact.VertexInputs().size()) {
        return false;
    }
    for (const shader::WireVertexInputRecord& expected : artifact.VertexInputs()) {
        const std::optional<std::string_view> semantic = artifact.GetName(expected.Semantic);
        if (!semantic.has_value()) {
            return false;
        }
        const auto attribute = std::find_if(
            state.Attributes.begin(),
            state.Attributes.end(),
            [&](const VertexAttribute& value) noexcept {
                return value.Semantic == semantic.value() &&
                       value.SemanticIndex == expected.SemanticIndex;
            });
        if (attribute == state.Attributes.end() || attribute->Location != expected.Location) {
            return false;
        }
        uint32_t componentType = 0;
        uint32_t componentCount = 0;
        if (!GetVertexFormatShape(attribute->Format, componentType, componentCount) ||
            componentType != expected.ComponentType || componentCount != expected.ComponentCount) {
            return false;
        }
    }
    return true;
}

}  // namespace radray::render
