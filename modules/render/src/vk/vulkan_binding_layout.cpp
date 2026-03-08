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
    BindingParameterLayout Layout{};
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

std::optional<std::pair<DescriptorSetIndex, uint32_t>> _ResolveUnifiedBinding(
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
    return std::pair{DescriptorSetIndex{binding.Set}, binding.Binding};
}

bool _HasSameAbi(const BindingParameterLayout& lhs, const BindingParameterLayout& rhs) noexcept {
    if (lhs.Kind != rhs.Kind) {
        return false;
    }
    if (lhs.Kind == BindingParameterKind::PushConstant) {
        const auto& lhsAbi = std::get<PushConstantBindingAbi>(lhs.Abi);
        const auto& rhsAbi = std::get<PushConstantBindingAbi>(rhs.Abi);
        return lhsAbi.Offset == rhsAbi.Offset && lhsAbi.Size == rhsAbi.Size;
    }
    const auto& lhsAbi = std::get<ResourceBindingAbi>(lhs.Abi);
    const auto& rhsAbi = std::get<ResourceBindingAbi>(rhs.Abi);
    return lhsAbi.Set == rhsAbi.Set &&
           lhsAbi.Binding == rhsAbi.Binding &&
           lhsAbi.Type == rhsAbi.Type;
}

bool _SharesBindingLocation(const BindingParameterLayout& lhs, const BindingParameterLayout& rhs) noexcept {
    if (lhs.Kind == BindingParameterKind::PushConstant || rhs.Kind == BindingParameterKind::PushConstant) {
        return false;
    }
    const auto& lhsAbi = std::get<ResourceBindingAbi>(lhs.Abi);
    const auto& rhsAbi = std::get<ResourceBindingAbi>(rhs.Abi);
    return lhsAbi.Set == rhsAbi.Set &&
           lhsAbi.Binding == rhsAbi.Binding;
}

bool _HasCompatiblePayload(const ParameterRecord& lhs, const ParameterRecord& rhs) noexcept {
    if (lhs.Layout.Kind != rhs.Layout.Kind) {
        return false;
    }
    if (lhs.Layout.Kind == BindingParameterKind::PushConstant) {
        const auto& lhsAbi = std::get<PushConstantBindingAbi>(lhs.Layout.Abi);
        const auto& rhsAbi = std::get<PushConstantBindingAbi>(rhs.Layout.Abi);
        return lhsAbi.Offset == rhsAbi.Offset && lhsAbi.Size == rhsAbi.Size;
    }
    const auto& lhsAbi = std::get<ResourceBindingAbi>(lhs.Layout.Abi);
    const auto& rhsAbi = std::get<ResourceBindingAbi>(rhs.Layout.Abi);
    return lhsAbi.Set == rhsAbi.Set &&
           lhsAbi.Binding == rhsAbi.Binding &&
           lhsAbi.Type == rhsAbi.Type &&
           lhsAbi.Count == rhsAbi.Count &&
           lhsAbi.IsReadOnly == rhsAbi.IsReadOnly &&
           lhsAbi.IsBindless == rhsAbi.IsBindless &&
           lhs.Vulkan.DescriptorType == rhs.Vulkan.DescriptorType;
}

bool _PushConstantsOverlap(const BindingParameterLayout& lhs, const BindingParameterLayout& rhs) noexcept {
    if (lhs.Kind != BindingParameterKind::PushConstant || rhs.Kind != BindingParameterKind::PushConstant) {
        return false;
    }
    const auto& lhsAbi = std::get<PushConstantBindingAbi>(lhs.Abi);
    const auto& rhsAbi = std::get<PushConstantBindingAbi>(rhs.Abi);
    const uint32_t lhsEnd = lhsAbi.Offset + lhsAbi.Size;
    const uint32_t rhsEnd = rhsAbi.Offset + rhsAbi.Size;
    return lhsAbi.Offset < rhsEnd && rhsAbi.Offset < lhsEnd;
}

bool _MergeRecord(vector<ParameterRecord>& records, ParameterRecord incoming) noexcept {
    for (auto& record : records) {
        if (_HasSameAbi(record.Layout, incoming.Layout)) {
            if (record.Layout.Name != incoming.Layout.Name) {
                RADRAY_ERR_LOG(
                    "binding abi conflict: parameter '{}' shares abi with '{}'",
                    incoming.Layout.Name,
                    record.Layout.Name);
                return false;
            }
            if (!_HasCompatiblePayload(record, incoming)) {
                RADRAY_ERR_LOG("binding payload conflict for parameter '{}'", incoming.Layout.Name);
                return false;
            }
            record.Layout.Stages |= incoming.Layout.Stages;
            record.Vulkan.Stages |= incoming.Vulkan.Stages;
            return true;
        }
        if (_SharesBindingLocation(record.Layout, incoming.Layout)) {
            RADRAY_ERR_LOG(
                "vk binding location conflict: '{}' and '{}' share set={} binding={}",
                record.Layout.Name,
                incoming.Layout.Name,
                std::get<ResourceBindingAbi>(incoming.Layout.Abi).Set.Value,
                std::get<ResourceBindingAbi>(incoming.Layout.Abi).Binding);
            return false;
        }
        if (record.Layout.Name == incoming.Layout.Name) {
            RADRAY_ERR_LOG("binding name conflict: '{}' maps to different abi", incoming.Layout.Name);
            return false;
        }
        if (_PushConstantsOverlap(record.Layout, incoming.Layout)) {
            RADRAY_ERR_LOG(
                "push constant ranges overlap but are not identical: '{}' and '{}'",
                record.Layout.Name,
                incoming.Layout.Name);
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
        record.Layout.Name = resource.Name;
        record.Layout.Kind = bindTypeOpt.value() == ResourceBindType::Sampler
                                 ? BindingParameterKind::Sampler
                                 : BindingParameterKind::Resource;
        record.Layout.Stages = stages;
        record.Vulkan.IsBindless = isBindless;
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
            record.Layout.Kind = BindingParameterKind::Resource;
            record.Layout.Abi = ResourceBindingAbi{
                .Set = setIndex,
                .Binding = bindingIndex,
                .Type = bindTypeOpt.value(),
                .Count = 0,
                .IsReadOnly = !_IsRwResourceType(bindTypeOpt.value()),
                .IsBindless = true,
            };
            record.Vulkan.BindlessSlotType = bindlessSlotTypeOpt.value();
        } else {
            record.Layout.Abi = ResourceBindingAbi{
                .Set = setIndex,
                .Binding = bindingIndex,
                .Type = bindTypeOpt.value(),
                .Count = resource.ArraySize == 0 ? 1u : resource.ArraySize,
                .IsReadOnly = !_IsRwResourceType(bindTypeOpt.value()),
                .IsBindless = false,
            };
        }
        record.Vulkan.Kind = record.Layout.Kind;
        record.Vulkan.Stages = stages;
        record.Vulkan.SetIndex = setIndex.Value;
        record.Vulkan.BindingIndex = bindingIndex;
        record.Vulkan.ResourceType = bindTypeOpt.value();
        record.Vulkan.DescriptorCount = std::get<ResourceBindingAbi>(record.Layout.Abi).Count;
        record.Vulkan.IsReadOnly = std::get<ResourceBindingAbi>(record.Layout.Abi).IsReadOnly;
        record.Vulkan.DescriptorType = MapType(bindTypeOpt.value());
        if (!_MergeRecord(records, std::move(record))) {
            return false;
        }
    }

    for (const auto& pushConstant : reflection.PushConstants) {
        if (pushConstant.Size == 0) {
            RADRAY_ERR_LOG("push constant '{}' must have non-zero size", pushConstant.Name);
            return false;
        }
        ParameterRecord record{};
        record.Layout.Name = pushConstant.Name;
        record.Layout.Kind = BindingParameterKind::PushConstant;
        record.Layout.Stages = stages;
        record.Layout.Abi = PushConstantBindingAbi{
            .Offset = pushConstant.Offset,
            .Size = pushConstant.Size,
        };
        record.Vulkan.Kind = BindingParameterKind::PushConstant;
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
        if (record.Layout.Kind == BindingParameterKind::PushConstant) {
            continue;
        }
        const auto& abi = std::get<ResourceBindingAbi>(record.Layout.Abi);
        const uint32_t setIndex = abi.Set.Value;
        descriptorCountBySet[setIndex] += 1;
        if (!abi.IsBindless) {
            continue;
        }
        if (record.Layout.Kind != BindingParameterKind::Resource) {
            RADRAY_ERR_LOG("bindless parameter '{}' must be a resource", record.Layout.Name);
            return false;
        }
        auto [it, inserted] = bindlessBySet.emplace(setIndex, &record);
        if (!inserted) {
            RADRAY_ERR_LOG(
                "bindless set {} cannot contain more than one bindless parameter ('{}' and '{}')",
                setIndex,
                it->second->Layout.Name,
                record.Layout.Name);
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
    const bool lhsPush = lhs.Layout.Kind == BindingParameterKind::PushConstant;
    const bool rhsPush = rhs.Layout.Kind == BindingParameterKind::PushConstant;
    if (lhsPush != rhsPush) {
        return !lhsPush;
    }
    if (lhsPush) {
        const auto& lhsAbi = std::get<PushConstantBindingAbi>(lhs.Layout.Abi);
        const auto& rhsAbi = std::get<PushConstantBindingAbi>(rhs.Layout.Abi);
        return std::tie(lhsAbi.Offset, lhsAbi.Size, lhs.Layout.Name) <
               std::tie(rhsAbi.Offset, rhsAbi.Size, rhs.Layout.Name);
    }
    const auto& lhsAbi = std::get<ResourceBindingAbi>(lhs.Layout.Abi);
    const auto& rhsAbi = std::get<ResourceBindingAbi>(rhs.Layout.Abi);
    return std::tie(lhsAbi.Set, lhsAbi.Binding, lhs.Layout.Kind, lhs.Layout.Name) <
           std::tie(rhsAbi.Set, rhsAbi.Binding, rhs.Layout.Kind, rhs.Layout.Name);
}

}  // namespace

std::optional<VulkanMergedBindingLayout> BuildMergedBindingLayoutVulkan(std::span<Shader*> shaders) noexcept {
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

    if (!_ValidateBindlessSets(records)) {
        return std::nullopt;
    }

    std::sort(records.begin(), records.end(), _ParameterLess);

    VulkanMergedBindingLayout result{};
    vector<BindingParameterLayout> layouts{};
    layouts.reserve(records.size());
    result.Parameters.reserve(records.size());
    uint32_t maxSetPlusOne = 0;
    for (uint32_t i = 0; i < records.size(); ++i) {
        auto& record = records[i];
        record.Layout.Id = BindingParameterId{i};
        record.Vulkan.Id = BindingParameterId{i};
        layouts.push_back(record.Layout);
        result.Parameters.push_back(record.Vulkan);
        if (record.Layout.Kind != BindingParameterKind::PushConstant) {
            const auto& abi = std::get<ResourceBindingAbi>(record.Layout.Abi);
            maxSetPlusOne = std::max(maxSetPlusOne, abi.Set.Value + 1);
        }
    }
    result.Layout = BindingLayout{std::move(layouts)};
    result.SetLayoutCount = maxSetPlusOne;
    return result;
}

}  // namespace radray::render::vulkan
