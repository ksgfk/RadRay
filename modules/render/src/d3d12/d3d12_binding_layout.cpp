#include <radray/render/backend/d3d12_impl.h>

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <radray/logger.h>
#include <radray/render/shader/hlsl.h>

namespace radray::render::d3d12 {

namespace {

struct ParameterRecord {
    ShaderParameterInfo Parameter{};
    D3D12BindingParameterInfo D3D12{};
};

constexpr bool _IsRwResourceType(ResourceBindType type) noexcept {
    switch (type) {
        case ResourceBindType::RWBuffer:
        case ResourceBindType::RWTexelBuffer:
        case ResourceBindType::RWTexture:
            return true;
        default:
            return false;
    }
}

std::optional<ResourceBindType> _MapHlslResourceType(const HlslInputBindDesc& binding) noexcept {
    switch (binding.Type) {
        case HlslShaderInputType::CBUFFER:
            return ResourceBindType::CBuffer;
        case HlslShaderInputType::TBUFFER:
        case HlslShaderInputType::STRUCTURED:
        case HlslShaderInputType::BYTEADDRESS:
            return ResourceBindType::Buffer;
        case HlslShaderInputType::TEXTURE:
            return IsBufferDimension(binding.Dimension) ? ResourceBindType::TexelBuffer : ResourceBindType::Texture;
        case HlslShaderInputType::SAMPLER:
            return ResourceBindType::Sampler;
        case HlslShaderInputType::UAV_RWTYPED:
            return IsBufferDimension(binding.Dimension) ? ResourceBindType::RWTexelBuffer : ResourceBindType::RWTexture;
        case HlslShaderInputType::UAV_RWSTRUCTURED:
        case HlslShaderInputType::UAV_APPEND_STRUCTURED:
        case HlslShaderInputType::UAV_CONSUME_STRUCTURED:
        case HlslShaderInputType::UAV_RWSTRUCTURED_WITH_COUNTER:
        case HlslShaderInputType::UAV_RWBYTEADDRESS:
            return ResourceBindType::RWBuffer;
        case HlslShaderInputType::RTACCELERATIONSTRUCTURE:
            return ResourceBindType::AccelerationStructure;
        case HlslShaderInputType::UAV_FEEDBACKTEXTURE:
            return ResourceBindType::RWTexture;
        case HlslShaderInputType::UNKNOWN:
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<BindlessSlotType> _MapBindlessSlotType(
    const HlslInputBindDesc& binding,
    ResourceBindType type) noexcept {
    switch (type) {
        case ResourceBindType::Buffer:
        case ResourceBindType::RWBuffer:
            if (binding.Type == HlslShaderInputType::TEXTURE && IsBufferDimension(binding.Dimension)) {
                return std::nullopt;
            }
            return BindlessSlotType::BufferOnly;
        case ResourceBindType::Texture:
            if (binding.Dimension == HlslSRVDimension::TEXTURE2D) {
                return BindlessSlotType::Texture2DOnly;
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

std::optional<std::pair<uint32_t, uint32_t>> _ResolveUnifiedBinding(
    const HlslInputBindDesc& binding) noexcept {
    const bool hasVkAbi = binding.VkSet.has_value() || binding.VkBinding.has_value();
    if (hasVkAbi && (!binding.VkSet.has_value() || !binding.VkBinding.has_value())) {
        RADRAY_ERR_LOG("incomplete vk binding metadata for '{}'", binding.Name);
        return std::nullopt;
    }
    if (binding.VkSet.has_value() && binding.VkSet.value() != binding.Space) {
        RADRAY_ERR_LOG(
            "binding abi conflict for '{}': vk set {} != d3d12 space {}",
            binding.Name,
            binding.VkSet.value(),
            binding.Space);
        return std::nullopt;
    }
    if (binding.VkBinding.has_value() && binding.VkBinding.value() != binding.BindPoint) {
        RADRAY_ERR_LOG(
            "binding abi conflict for '{}': vk binding {} != d3d12 register {}",
            binding.Name,
            binding.VkBinding.value(),
            binding.BindPoint);
        return std::nullopt;
    }
    return std::pair{binding.VkSet.value_or(binding.Space), binding.VkBinding.value_or(binding.BindPoint)};
}

bool _IsHlslPushConstantCandidate(const HlslInputBindDesc& binding, const HlslShaderBufferDesc& cbuffer) noexcept {
    return binding.Type == HlslShaderInputType::CBUFFER &&
           binding.BindPoint == 0 &&
           binding.Space == 0 &&
           cbuffer.IsViewInHlsl;
}

bool _HasSameAbi(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    if (lhs.Parameter.Kind != rhs.Parameter.Kind) {
        return false;
    }
    if (lhs.Parameter.Kind == ShaderParameterKind::Constant) {
        return lhs.D3D12.PushConstantOffset == rhs.D3D12.PushConstantOffset &&
               lhs.D3D12.PushConstantSize == rhs.D3D12.PushConstantSize;
    }
    return lhs.D3D12.RegisterSpace == rhs.D3D12.RegisterSpace &&
           lhs.D3D12.BindingIndex == rhs.D3D12.BindingIndex &&
           lhs.D3D12.Type == rhs.D3D12.Type;
}

bool _HasCompatiblePayload(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    if (lhs.Parameter.Kind != rhs.Parameter.Kind) {
        return false;
    }
    if (lhs.Parameter.Kind == ShaderParameterKind::Constant) {
        return lhs.D3D12.PushConstantOffset == rhs.D3D12.PushConstantOffset &&
               lhs.D3D12.PushConstantSize == rhs.D3D12.PushConstantSize;
    }
    return lhs.D3D12.RegisterSpace == rhs.D3D12.RegisterSpace &&
           lhs.D3D12.BindingIndex == rhs.D3D12.BindingIndex &&
           lhs.D3D12.Type == rhs.D3D12.Type &&
           lhs.D3D12.Count == rhs.D3D12.Count &&
           lhs.D3D12.IsReadOnly == rhs.D3D12.IsReadOnly &&
           lhs.D3D12.IsBindless == rhs.D3D12.IsBindless;
}

bool _PushConstantsOverlap(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    if (lhs.Parameter.Kind != ShaderParameterKind::Constant || rhs.Parameter.Kind != ShaderParameterKind::Constant) {
        return false;
    }
    const uint32_t lhsEnd = lhs.D3D12.PushConstantOffset + lhs.D3D12.PushConstantSize;
    const uint32_t rhsEnd = rhs.D3D12.PushConstantOffset + rhs.D3D12.PushConstantSize;
    return lhs.D3D12.PushConstantOffset < rhsEnd && rhs.D3D12.PushConstantOffset < lhsEnd;
}

bool _MergeRecord(vector<ParameterRecord>& records, ParameterRecord incoming) noexcept {
    for (auto& record : records) {
        if (_HasSameAbi(record, incoming)) {
            if (record.Parameter.Name != incoming.Parameter.Name) {
                RADRAY_ERR_LOG(
                    "binding abi conflict: parameter '{}' shares abi with '{}'",
                    incoming.Parameter.Name,
                    record.Parameter.Name);
                return false;
            }
            if (!_HasCompatiblePayload(record, incoming)) {
                RADRAY_ERR_LOG("binding payload conflict for parameter '{}'", incoming.Parameter.Name);
                return false;
            }
            if (record.Parameter.Kind == ShaderParameterKind::Constant &&
                record.D3D12.IsAvailable &&
                incoming.D3D12.IsAvailable &&
                (record.D3D12.ShaderRegister != incoming.D3D12.ShaderRegister ||
                 record.D3D12.RegisterSpace != incoming.D3D12.RegisterSpace)) {
                RADRAY_ERR_LOG("d3d12 push constant register conflict for '{}'", incoming.Parameter.Name);
                return false;
            }
            record.Parameter.Stages |= incoming.Parameter.Stages;
            record.D3D12.Stages |= incoming.D3D12.Stages;
            if (!record.D3D12.IsAvailable && incoming.D3D12.IsAvailable) {
                record.D3D12 = incoming.D3D12;
            }
            return true;
        }
        if (record.Parameter.Name == incoming.Parameter.Name) {
            RADRAY_ERR_LOG("binding name conflict: '{}' maps to different abi", incoming.Parameter.Name);
            return false;
        }
        if (_PushConstantsOverlap(record, incoming)) {
            RADRAY_ERR_LOG(
                "push constant ranges overlap but are not identical: '{}' and '{}'",
                record.Parameter.Name,
                incoming.Parameter.Name);
            return false;
        }
    }
    records.push_back(std::move(incoming));
    return true;
}

bool _AppendHlslBindings(
    vector<ParameterRecord>& records,
    const HlslShaderDesc& reflection,
    ShaderStages stages) noexcept {
    for (const auto& resource : reflection.BoundResources) {
        auto bindTypeOpt = _MapHlslResourceType(resource);
        if (!bindTypeOpt.has_value()) {
            RADRAY_ERR_LOG("unsupported hlsl resource type for '{}'", resource.Name);
            return false;
        }

        auto unifiedAbiOpt = _ResolveUnifiedBinding(resource);
        if (!unifiedAbiOpt.has_value()) {
            return false;
        }
        const auto [registerSpace, bindingIndex] = unifiedAbiOpt.value();
        const bool isBindless = resource.IsUnboundArray();

        ParameterRecord record{};
        record.Parameter.Name = resource.Name;
        record.Parameter.Stages = stages;
        record.Parameter.Type = bindTypeOpt.value();
        record.Parameter.Count = resource.BindCount == 0 ? 1u : resource.BindCount;
        record.Parameter.IsReadOnly = !_IsRwResourceType(bindTypeOpt.value());
        record.Parameter.IsBindless = isBindless;
        record.D3D12.Name = resource.Name;
        record.D3D12.IsAvailable = true;
        record.D3D12.BindingIndex = bindingIndex;
        record.D3D12.ShaderRegister = resource.BindPoint;
        record.D3D12.RegisterSpace = registerSpace;
        record.D3D12.Type = bindTypeOpt.value();
        record.D3D12.Count = record.Parameter.Count;
        record.D3D12.IsReadOnly = record.Parameter.IsReadOnly;
        record.D3D12.IsBindless = isBindless;
        record.D3D12.Stages = stages;

        if (resource.Type == HlslShaderInputType::CBUFFER) {
            auto cbufferOpt = reflection.FindCBufferByName(resource.Name);
            if (!cbufferOpt.HasValue()) {
                RADRAY_ERR_LOG("cannot find cbuffer reflection for '{}'", resource.Name);
                return false;
            }
            const auto* cbuffer = cbufferOpt.Get();
            if (_IsHlslPushConstantCandidate(resource, *cbuffer)) {
                record.Parameter.Kind = ShaderParameterKind::Constant;
                record.Parameter.Type = ResourceBindType::UNKNOWN;
                record.Parameter.Count = 1;
                record.Parameter.ByteSize = cbuffer->Size;
                record.Parameter.IsReadOnly = true;
                record.Parameter.IsBindless = false;
                record.D3D12.Kind = ShaderParameterKind::Constant;
                record.D3D12.Count = 0;
                record.D3D12.IsBindless = false;
                record.D3D12.PushConstantOffset = 0;
                record.D3D12.PushConstantSize = cbuffer->Size;
                if (!_MergeRecord(records, std::move(record))) {
                    return false;
                }
                continue;
            }
        }

        if (isBindless) {
            auto bindlessSlotTypeOpt = _MapBindlessSlotType(resource, bindTypeOpt.value());
            if (!bindlessSlotTypeOpt.has_value()) {
                RADRAY_ERR_LOG(
                    "unsupported bindless resource '{}' with type {} and dimension {}",
                    resource.Name,
                    bindTypeOpt.value(),
                    static_cast<uint32_t>(resource.Dimension));
                return false;
            }
            record.Parameter.Kind = ShaderParameterKind::BindlessArray;
            record.Parameter.Count = 0;
            record.D3D12.Kind = ShaderParameterKind::BindlessArray;
            record.D3D12.Count = 0;
            record.D3D12.BindlessSlotType = bindlessSlotTypeOpt.value();
            if (!_MergeRecord(records, std::move(record))) {
                return false;
            }
            continue;
        }

        record.Parameter.Kind = bindTypeOpt.value() == ResourceBindType::Sampler
                                    ? ShaderParameterKind::Sampler
                                    : ShaderParameterKind::Resource;
        record.D3D12.Kind = record.Parameter.Kind;
        if (!_MergeRecord(records, std::move(record))) {
            return false;
        }
    }
    return true;
}

bool _ValidateBindlessSpaces(const vector<ParameterRecord>& records) noexcept {
    unordered_map<uint32_t, const ParameterRecord*> bindlessBySpace{};
    unordered_map<uint32_t, uint32_t> descriptorCountBySpace{};
    for (const auto& record : records) {
        if (record.Parameter.Kind == ShaderParameterKind::Constant) {
            continue;
        }
        const uint32_t registerSpace = record.D3D12.RegisterSpace;
        descriptorCountBySpace[registerSpace] += 1;
        if (!record.Parameter.IsBindless) {
            continue;
        }
        if (record.Parameter.Kind != ShaderParameterKind::BindlessArray) {
            RADRAY_ERR_LOG("bindless parameter '{}' must be a bindless array", record.Parameter.Name);
            return false;
        }
        auto [it, inserted] = bindlessBySpace.emplace(registerSpace, &record);
        if (!inserted) {
            RADRAY_ERR_LOG(
                "bindless register space {} cannot contain more than one bindless parameter ('{}' and '{}')",
                registerSpace,
                it->second->Parameter.Name,
                record.Parameter.Name);
            return false;
        }
    }
    for (const auto& [registerSpace, bindlessRecord] : bindlessBySpace) {
        RADRAY_UNUSED(bindlessRecord);
        auto countIt = descriptorCountBySpace.find(registerSpace);
        if (countIt != descriptorCountBySpace.end() && countIt->second != 1) {
            RADRAY_ERR_LOG(
                "bindless register space {} cannot contain ordinary descriptors together with bindless resources",
                registerSpace);
            return false;
        }
    }
    return true;
}

bool _ParameterLess(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    const bool lhsConstant = lhs.Parameter.Kind == ShaderParameterKind::Constant;
    const bool rhsConstant = rhs.Parameter.Kind == ShaderParameterKind::Constant;
    if (lhsConstant != rhsConstant) {
        return !lhsConstant;
    }
    if (lhsConstant) {
        return std::tie(lhs.D3D12.PushConstantOffset, lhs.D3D12.PushConstantSize, lhs.Parameter.Name) <
               std::tie(rhs.D3D12.PushConstantOffset, rhs.D3D12.PushConstantSize, rhs.Parameter.Name);
    }
    return std::tie(lhs.D3D12.RegisterSpace, lhs.D3D12.BindingIndex, lhs.Parameter.Kind, lhs.Parameter.Name) <
           std::tie(rhs.D3D12.RegisterSpace, rhs.D3D12.BindingIndex, rhs.Parameter.Kind, rhs.Parameter.Name);
}

}  // namespace

std::optional<D3D12MergedBindingLayout> BuildMergedBindingLayoutD3D12(std::span<Shader*> shaders) noexcept {
    vector<ParameterRecord> records{};
    records.reserve(shaders.size() * 4);

    for (Shader* shader : shaders) {
        if (shader == nullptr) {
            RADRAY_ERR_LOG("root signature shader is null");
            return std::nullopt;
        }
        const ShaderStages stages = shader->GetStages();
        if (stages == ShaderStage::UNKNOWN) {
            RADRAY_ERR_LOG("shader '{}' does not have stage metadata", static_cast<void*>(shader));
            return std::nullopt;
        }
        auto reflectionOpt = shader->GetReflection();
        if (!reflectionOpt.HasValue() || reflectionOpt.Get() == nullptr) {
            RADRAY_ERR_LOG("shader '{}' does not have reflection metadata", static_cast<void*>(shader));
            return std::nullopt;
        }
        const ShaderReflectionDesc& reflection = *reflectionOpt.Get();
        const auto* hlsl = std::get_if<HlslShaderDesc>(&reflection);
        if (hlsl == nullptr) {
            RADRAY_ERR_LOG("d3d12 merged binding layout requires hlsl reflection metadata");
            return std::nullopt;
        }
        if (!_AppendHlslBindings(records, *hlsl, stages)) {
            return std::nullopt;
        }
    }

    if (!_ValidateBindlessSpaces(records)) {
        return std::nullopt;
    }

    std::sort(records.begin(), records.end(), _ParameterLess);

    D3D12MergedBindingLayout result{};
    result.Parameters.reserve(records.size());
    result.D3D12Parameters.reserve(records.size());
    uint32_t maxRegisterSpacePlusOne = 0;
    for (uint32_t i = 0; i < records.size(); ++i) {
        auto& record = records[i];
        record.Parameter.Id = ShaderParameterId{i};
        record.D3D12.Id = ShaderParameterId{i};
        result.Parameters.push_back(record.Parameter);
        result.D3D12Parameters.push_back(record.D3D12);
        if (record.Parameter.Kind != ShaderParameterKind::Constant) {
            maxRegisterSpacePlusOne = std::max(maxRegisterSpacePlusOne, record.D3D12.RegisterSpace + 1);
        }
    }
    result.RegisterSpaceCount = maxRegisterSpacePlusOne;
    return result;
}

}  // namespace radray::render::d3d12
