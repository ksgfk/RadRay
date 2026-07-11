#include <radray/render/backend/vulkan_impl.h>

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <radray/logger.h>
#include <radray/render/shader/spirv.h>

namespace radray::render::vulkan {

namespace {

struct ParameterRecord {
    ShaderParameterInfo Parameter{};
    VulkanBindingParameterInfo Vulkan{};
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

std::optional<ResourceBindType> _MapSpirvResourceType(const SpirvResourceBinding& binding) noexcept {
    const bool isBufferImage = binding.ImageInfo.has_value() && binding.ImageInfo->Dim == SpirvImageDim::Buffer;
    switch (binding.Kind) {
        case SpirvResourceKind::UniformBuffer:
            return ResourceBindType::CBuffer;
        case SpirvResourceKind::StorageBuffer:
            return binding.ReadOnly && !binding.WriteOnly ? ResourceBindType::Buffer : ResourceBindType::RWBuffer;
        case SpirvResourceKind::SampledImage:
        case SpirvResourceKind::SeparateImage:
            return isBufferImage ? ResourceBindType::TexelBuffer : ResourceBindType::Texture;
        case SpirvResourceKind::SeparateSampler:
            return ResourceBindType::Sampler;
        case SpirvResourceKind::StorageImage:
            return isBufferImage ? ResourceBindType::RWTexelBuffer : ResourceBindType::RWTexture;
        case SpirvResourceKind::AccelerationStructure:
            return ResourceBindType::AccelerationStructure;
        case SpirvResourceKind::PushConstant:
        case SpirvResourceKind::UNKNOWN:
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<BindlessSlotType> _MapBindlessSlotType(
    const SpirvResourceBinding& binding,
    ResourceBindType type) noexcept {
    switch (type) {
        case ResourceBindType::Buffer:
        case ResourceBindType::RWBuffer:
            if (binding.Kind != SpirvResourceKind::StorageBuffer) {
                return std::nullopt;
            }
            return BindlessSlotType::BufferOnly;
        case ResourceBindType::Texture:
            if ((binding.Kind == SpirvResourceKind::SampledImage || binding.Kind == SpirvResourceKind::SeparateImage) &&
                binding.ImageInfo.has_value() &&
                binding.ImageInfo->Dim == SpirvImageDim::Dim2D) {
                return BindlessSlotType::Texture2DOnly;
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

std::optional<std::pair<uint32_t, uint32_t>> _ResolveUnifiedBinding(
    const SpirvResourceBinding& binding) noexcept {
    const bool hasHlslAbi = binding.HlslSpace.has_value() || binding.HlslRegister.has_value();
    if (hasHlslAbi && (!binding.HlslSpace.has_value() || !binding.HlslRegister.has_value())) {
        RADRAY_ERR_LOG("incomplete hlsl register metadata for '{}'", binding.Name);
        return std::nullopt;
    }
    if (binding.HlslSpace.has_value() && binding.HlslSpace.value() != binding.Set) {
        RADRAY_ERR_LOG(
            "binding abi conflict for '{}': vk set {} != d3d12 space {}",
            binding.Name,
            binding.Set,
            binding.HlslSpace.value());
        return std::nullopt;
    }
    if (binding.HlslRegister.has_value() && binding.HlslRegister.value() != binding.Binding) {
        RADRAY_ERR_LOG(
            "binding abi conflict for '{}': vk binding {} != d3d12 register {}",
            binding.Name,
            binding.Binding,
            binding.HlslRegister.value());
        return std::nullopt;
    }
    return std::pair{binding.Set, binding.Binding};
}

bool _HasSameAbi(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    if (lhs.Parameter.Kind != rhs.Parameter.Kind) {
        return false;
    }
    if (lhs.Parameter.Kind == ShaderParameterKind::Constant) {
        return lhs.Vulkan.Offset == rhs.Vulkan.Offset && lhs.Vulkan.Size == rhs.Vulkan.Size;
    }
    return lhs.Vulkan.SetIndex == rhs.Vulkan.SetIndex &&
           lhs.Vulkan.BindingIndex == rhs.Vulkan.BindingIndex &&
           lhs.Vulkan.ResourceType == rhs.Vulkan.ResourceType;
}

bool _SharesBindingLocation(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    if (lhs.Parameter.Kind == ShaderParameterKind::Constant || rhs.Parameter.Kind == ShaderParameterKind::Constant) {
        return false;
    }
    return lhs.Vulkan.SetIndex == rhs.Vulkan.SetIndex &&
           lhs.Vulkan.BindingIndex == rhs.Vulkan.BindingIndex;
}

bool _HasCompatiblePayload(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    if (lhs.Parameter.Kind != rhs.Parameter.Kind) {
        return false;
    }
    if (lhs.Parameter.Kind == ShaderParameterKind::Constant) {
        return lhs.Vulkan.Offset == rhs.Vulkan.Offset && lhs.Vulkan.Size == rhs.Vulkan.Size;
    }
    return lhs.Vulkan.SetIndex == rhs.Vulkan.SetIndex &&
           lhs.Vulkan.BindingIndex == rhs.Vulkan.BindingIndex &&
           lhs.Vulkan.ResourceType == rhs.Vulkan.ResourceType &&
           lhs.Vulkan.DescriptorCount == rhs.Vulkan.DescriptorCount &&
           lhs.Vulkan.IsReadOnly == rhs.Vulkan.IsReadOnly &&
           lhs.Vulkan.IsBindless == rhs.Vulkan.IsBindless &&
           lhs.Vulkan.DescriptorType == rhs.Vulkan.DescriptorType;
}

bool _PushConstantsOverlap(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    if (lhs.Parameter.Kind != ShaderParameterKind::Constant || rhs.Parameter.Kind != ShaderParameterKind::Constant) {
        return false;
    }
    const uint32_t lhsEnd = lhs.Vulkan.Offset + lhs.Vulkan.Size;
    const uint32_t rhsEnd = rhs.Vulkan.Offset + rhs.Vulkan.Size;
    return lhs.Vulkan.Offset < rhsEnd && rhs.Vulkan.Offset < lhsEnd;
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
            record.Parameter.Stages |= incoming.Parameter.Stages;
            record.Vulkan.Stages |= incoming.Vulkan.Stages;
            return true;
        }
        if (_SharesBindingLocation(record, incoming)) {
            RADRAY_ERR_LOG(
                "vk binding location conflict: '{}' and '{}' share set={} binding={}",
                record.Parameter.Name,
                incoming.Parameter.Name,
                incoming.Vulkan.SetIndex,
                incoming.Vulkan.BindingIndex);
            return false;
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

bool _AppendSpirvBindings(
    vector<ParameterRecord>& records,
    const SpirvShaderDesc& reflection,
    ShaderStages stages) noexcept {
    for (const auto& resource : reflection.ResourceBindings) {
        auto bindTypeOpt = _MapSpirvResourceType(resource);
        if (!bindTypeOpt.has_value()) {
            RADRAY_ERR_LOG("unsupported spirv resource type for '{}'", resource.Name);
            return false;
        }
        auto unifiedAbiOpt = _ResolveUnifiedBinding(resource);
        if (!unifiedAbiOpt.has_value()) {
            return false;
        }
        const auto [setIndex, bindingIndex] = unifiedAbiOpt.value();
        const bool isBindless = resource.IsUnboundedArray;

        ParameterRecord record{};
        record.Parameter.Name = resource.Name;
        record.Parameter.Kind = bindTypeOpt.value() == ResourceBindType::Sampler
                                    ? ShaderParameterKind::Sampler
                                    : ShaderParameterKind::Resource;
        record.Parameter.Stages = stages;
        record.Parameter.Type = bindTypeOpt.value();
        record.Parameter.Count = resource.ArraySize == 0 ? 1u : resource.ArraySize;
        record.Parameter.IsReadOnly = !_IsRwResourceType(bindTypeOpt.value());
        record.Parameter.IsBindless = isBindless;
        record.Vulkan.Name = resource.Name;
        record.Vulkan.Kind = record.Parameter.Kind;
        record.Vulkan.Stages = stages;
        record.Vulkan.SetIndex = setIndex;
        record.Vulkan.BindingIndex = bindingIndex;
        record.Vulkan.ResourceType = bindTypeOpt.value();
        record.Vulkan.DescriptorCount = record.Parameter.Count;
        record.Vulkan.IsReadOnly = record.Parameter.IsReadOnly;
        record.Vulkan.IsBindless = isBindless;
        record.Vulkan.DescriptorType = MapType(bindTypeOpt.value());

        if (isBindless) {
            auto bindlessSlotTypeOpt = _MapBindlessSlotType(resource, bindTypeOpt.value());
            if (!bindlessSlotTypeOpt.has_value()) {
                RADRAY_ERR_LOG(
                    "unsupported bindless resource '{}' with kind {} and image dimension {}",
                    resource.Name,
                    static_cast<uint32_t>(resource.Kind),
                    resource.ImageInfo.has_value() ? static_cast<uint32_t>(resource.ImageInfo->Dim) : static_cast<uint32_t>(SpirvImageDim::UNKNOWN));
                return false;
            }
            record.Parameter.Kind = ShaderParameterKind::BindlessArray;
            record.Parameter.Count = 0;
            record.Vulkan.Kind = ShaderParameterKind::BindlessArray;
            record.Vulkan.DescriptorCount = 0;
            record.Vulkan.BindlessSlotType = bindlessSlotTypeOpt.value();
        }

        if (!_MergeRecord(records, std::move(record))) {
            return false;
        }
    }

    for (const auto& pushConstant : reflection.ConstantRanges) {
        if (pushConstant.Size == 0) {
            RADRAY_ERR_LOG("push constant '{}' must have non-zero size", pushConstant.Name);
            return false;
        }
        ParameterRecord record{};
        record.Parameter.Name = pushConstant.Name;
        record.Parameter.Kind = ShaderParameterKind::Constant;
        record.Parameter.Stages = stages;
        record.Parameter.ByteSize = pushConstant.Size;
        record.Vulkan.Name = pushConstant.Name;
        record.Vulkan.Kind = ShaderParameterKind::Constant;
        record.Vulkan.Stages = stages;
        record.Vulkan.Offset = pushConstant.Offset;
        record.Vulkan.Size = pushConstant.Size;
        if (!_MergeRecord(records, std::move(record))) {
            return false;
        }
    }
    return true;
}

bool _ValidateBindlessSets(const vector<ParameterRecord>& records) noexcept {
    unordered_map<uint32_t, const ParameterRecord*> bindlessBySet{};
    unordered_map<uint32_t, uint32_t> descriptorCountBySet{};
    for (const auto& record : records) {
        if (record.Parameter.Kind == ShaderParameterKind::Constant) {
            continue;
        }
        const uint32_t setIndex = record.Vulkan.SetIndex;
        descriptorCountBySet[setIndex] += 1;
        if (!record.Parameter.IsBindless) {
            continue;
        }
        if (record.Parameter.Kind != ShaderParameterKind::BindlessArray) {
            RADRAY_ERR_LOG("bindless parameter '{}' must be a bindless array", record.Parameter.Name);
            return false;
        }
        auto [it, inserted] = bindlessBySet.emplace(setIndex, &record);
        if (!inserted) {
            RADRAY_ERR_LOG(
                "bindless set {} cannot contain more than one bindless parameter ('{}' and '{}')",
                setIndex,
                it->second->Parameter.Name,
                record.Parameter.Name);
            return false;
        }
    }
    for (const auto& [setIndex, bindlessRecord] : bindlessBySet) {
        RADRAY_UNUSED(bindlessRecord);
        auto countIt = descriptorCountBySet.find(setIndex);
        if (countIt != descriptorCountBySet.end() && countIt->second != 1) {
            RADRAY_ERR_LOG(
                "bindless set {} cannot contain ordinary descriptors together with bindless resources",
                setIndex);
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
        return std::tie(lhs.Vulkan.Offset, lhs.Vulkan.Size, lhs.Parameter.Name) <
               std::tie(rhs.Vulkan.Offset, rhs.Vulkan.Size, rhs.Parameter.Name);
    }
    return std::tie(lhs.Vulkan.SetIndex, lhs.Vulkan.BindingIndex, lhs.Parameter.Kind, lhs.Parameter.Name) <
           std::tie(rhs.Vulkan.SetIndex, rhs.Vulkan.BindingIndex, rhs.Parameter.Kind, rhs.Parameter.Name);
}

}  // namespace

std::optional<VulkanMergedPipelineLayout> BuildMergedPipelineLayoutVulkan(
    std::span<Shader*> shaders,
    std::span<const BindingGroupLayout> explicitGroups) noexcept {
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
        const auto* spirv = std::get_if<SpirvShaderDesc>(reflectionOpt.Get());
        if (spirv == nullptr) {
            RADRAY_ERR_LOG("vk merged binding layout requires spirv reflection metadata");
            return std::nullopt;
        }
        if (!_AppendSpirvBindings(records, *spirv, stages)) {
            return std::nullopt;
        }
    }

    vector<std::pair<uint32_t, uint32_t>> declaredLocations;
    for (const BindingGroupLayout& group : explicitGroups) {
        for (const BindingGroupLayoutEntry& entry : group.Entries) {
            const ShaderParameterInfo& declared = entry.Parameter;
            const std::pair location{group.GroupIndex, entry.Binding};
            if (std::ranges::find(declaredLocations, location) != declaredLocations.end()) {
                RADRAY_ERR_LOG(
                    "pipeline layout contains duplicate explicit set={} binding={}",
                    group.GroupIndex,
                    entry.Binding);
                return std::nullopt;
            }
            declaredLocations.push_back(location);
            const bool validKind =
                (declared.Kind == ShaderParameterKind::Sampler &&
                 declared.Type == ResourceBindType::Sampler) ||
                (declared.Kind == ShaderParameterKind::Resource &&
                 declared.Type != ResourceBindType::UNKNOWN &&
                 declared.Type != ResourceBindType::Sampler) ||
                declared.Kind == ShaderParameterKind::BindlessArray;
            if (!validKind || declared.Stages == ShaderStage::UNKNOWN || declared.Count == 0) {
                RADRAY_ERR_LOG(
                    "invalid explicit binding '{}' at set={} binding={}",
                    declared.Name,
                    group.GroupIndex,
                    entry.Binding);
                return std::nullopt;
            }

            auto record = std::ranges::find_if(
                records,
                [&group, &entry](const ParameterRecord& candidate) noexcept {
                    return candidate.Parameter.Kind != ShaderParameterKind::Constant &&
                           candidate.Vulkan.SetIndex == group.GroupIndex &&
                           candidate.Vulkan.BindingIndex == entry.Binding;
                });
            if (record != records.end()) {
                if (record->Parameter.Name != declared.Name ||
                    record->Parameter.Kind != declared.Kind ||
                    record->Parameter.Type != declared.Type ||
                    record->Parameter.Count != declared.Count ||
                    record->Parameter.IsBindless != declared.IsBindless) {
                    RADRAY_ERR_LOG(
                        "explicit binding '{}' does not match reflection at set={} binding={}",
                        declared.Name,
                        group.GroupIndex,
                        entry.Binding);
                    return std::nullopt;
                }
                record->Parameter.Stages = declared.Stages;
                record->Vulkan.Stages = declared.Stages;
                continue;
            }
            if (declared.IsBindless || declared.Kind == ShaderParameterKind::BindlessArray) {
                RADRAY_ERR_LOG("explicit bindless binding '{}' requires reflection metadata", declared.Name);
                return std::nullopt;
            }

            ParameterRecord synthesized{};
            synthesized.Parameter = declared;
            synthesized.Parameter.IsReadOnly = !_IsRwResourceType(declared.Type);
            synthesized.Vulkan.Name = declared.Name;
            synthesized.Vulkan.Kind = declared.Kind;
            synthesized.Vulkan.Stages = declared.Stages;
            synthesized.Vulkan.SetIndex = group.GroupIndex;
            synthesized.Vulkan.BindingIndex = entry.Binding;
            synthesized.Vulkan.ResourceType = declared.Type;
            synthesized.Vulkan.DescriptorCount = declared.Count;
            synthesized.Vulkan.IsReadOnly = synthesized.Parameter.IsReadOnly;
            synthesized.Vulkan.IsBindless = false;
            synthesized.Vulkan.DescriptorType = MapType(declared.Type);
            if (synthesized.Vulkan.DescriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM ||
                !_MergeRecord(records, std::move(synthesized))) {
                return std::nullopt;
            }
        }
    }

    if (!_ValidateBindlessSets(records)) {
        return std::nullopt;
    }

    std::sort(records.begin(), records.end(), _ParameterLess);

    VulkanMergedPipelineLayout result{};
    result.Parameters.reserve(records.size());
    result.VulkanParameters.reserve(records.size());
    uint32_t maxSetPlusOne = 0;
    for (uint32_t i = 0; i < records.size(); ++i) {
        auto& record = records[i];
        result.Parameters.push_back(record.Parameter);
        result.VulkanParameters.push_back(record.Vulkan);
        if (record.Parameter.Kind != ShaderParameterKind::Constant) {
            maxSetPlusOne = std::max(maxSetPlusOne, record.Vulkan.SetIndex + 1);
        }
    }
    result.DescriptorSetCount = maxSetPlusOne;
    return result;
}

}  // namespace radray::render::vulkan
