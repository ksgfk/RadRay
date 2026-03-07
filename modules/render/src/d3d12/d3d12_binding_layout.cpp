#include <radray/render/backend/d3d12_impl.h>

#include <algorithm>
#include <tuple>

#include <radray/logger.h>
#include <radray/render/shader/hlsl.h>

namespace radray::render::d3d12 {

namespace {

struct ParameterRecord {
    BindingParameterLayout Layout{};
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

bool _IsHlslPushConstantCandidate(const HlslInputBindDesc& binding, const HlslShaderBufferDesc& cbuffer) noexcept {
    return binding.Type == HlslShaderInputType::CBUFFER &&
           binding.BindPoint == 0 &&
           binding.Space == 0 &&
           cbuffer.IsViewInHlsl;
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
    return lhsAbi.SpaceOrSet == rhsAbi.SpaceOrSet &&
           lhsAbi.BindingOrRegister == rhsAbi.BindingOrRegister &&
           lhsAbi.Type == rhsAbi.Type;
}

bool _HasCompatiblePayload(const BindingParameterLayout& lhs, const BindingParameterLayout& rhs) noexcept {
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
    return lhsAbi.SpaceOrSet == rhsAbi.SpaceOrSet &&
           lhsAbi.BindingOrRegister == rhsAbi.BindingOrRegister &&
           lhsAbi.Type == rhsAbi.Type &&
           lhsAbi.Count == rhsAbi.Count &&
           lhsAbi.IsReadOnly == rhsAbi.IsReadOnly;
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
            if (!_HasCompatiblePayload(record.Layout, incoming.Layout)) {
                RADRAY_ERR_LOG("binding payload conflict for parameter '{}'", incoming.Layout.Name);
                return false;
            }
            if (record.Layout.Kind == BindingParameterKind::PushConstant &&
                record.D3D12.IsAvailable &&
                incoming.D3D12.IsAvailable &&
                (record.D3D12.ShaderRegister != incoming.D3D12.ShaderRegister ||
                 record.D3D12.RegisterSpace != incoming.D3D12.RegisterSpace)) {
                RADRAY_ERR_LOG("d3d12 push constant register conflict for '{}'", incoming.Layout.Name);
                return false;
            }
            record.Layout.Stages |= incoming.Layout.Stages;
            if (!record.D3D12.IsAvailable && incoming.D3D12.IsAvailable) {
                record.D3D12 = incoming.D3D12;
            }
            return true;
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

bool _AppendHlslBindings(
    vector<ParameterRecord>& records,
    const HlslShaderDesc& reflection,
    ShaderStages stages) noexcept {
    for (const auto& resource : reflection.BoundResources) {
        if (resource.IsUnboundArray()) {
            RADRAY_ERR_LOG("unbounded resource array is not supported: '{}'", resource.Name);
            return false;
        }
        auto bindTypeOpt = _MapHlslResourceType(resource);
        if (!bindTypeOpt.has_value()) {
            RADRAY_ERR_LOG("unsupported hlsl resource type for '{}'", resource.Name);
            return false;
        }

        ParameterRecord record{};
        record.Layout.Name = resource.Name;
        record.Layout.Stages = stages;
        record.D3D12.IsAvailable = true;
        record.D3D12.ShaderRegister = resource.BindPoint;
        record.D3D12.RegisterSpace = resource.Space;
        record.D3D12.Type = bindTypeOpt.value();

        if (resource.Type == HlslShaderInputType::CBUFFER) {
            auto cbufferOpt = reflection.FindCBufferByName(resource.Name);
            if (!cbufferOpt.HasValue()) {
                RADRAY_ERR_LOG("cannot find cbuffer reflection for '{}'", resource.Name);
                return false;
            }
            const auto* cbuffer = cbufferOpt.Get();
            if (_IsHlslPushConstantCandidate(resource, *cbuffer)) {
                record.Layout.Kind = BindingParameterKind::PushConstant;
                record.Layout.Abi = PushConstantBindingAbi{
                    .Offset = 0,
                    .Size = cbuffer->Size,
                };
                record.D3D12.Kind = BindingParameterKind::PushConstant;
                record.D3D12.PushConstantOffset = 0;
                record.D3D12.PushConstantSize = cbuffer->Size;
                if (!_MergeRecord(records, std::move(record))) {
                    return false;
                }
                continue;
            }
        }

        record.Layout.Kind = bindTypeOpt.value() == ResourceBindType::Sampler
                                 ? BindingParameterKind::Sampler
                                 : BindingParameterKind::Resource;
        record.Layout.Abi = ResourceBindingAbi{
            .SpaceOrSet = resource.Space,
            .BindingOrRegister = resource.BindPoint,
            .Type = bindTypeOpt.value(),
            .Count = resource.BindCount == 0 ? 1u : resource.BindCount,
            .IsReadOnly = !_IsRwResourceType(bindTypeOpt.value()),
        };
        record.D3D12.Kind = record.Layout.Kind;
        record.D3D12.Count = std::get<ResourceBindingAbi>(record.Layout.Abi).Count;
        record.D3D12.IsReadOnly = std::get<ResourceBindingAbi>(record.Layout.Abi).IsReadOnly;
        if (!_MergeRecord(records, std::move(record))) {
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
    return std::tie(lhsAbi.SpaceOrSet, lhsAbi.BindingOrRegister, lhs.Layout.Kind, lhs.Layout.Name) <
           std::tie(rhsAbi.SpaceOrSet, rhsAbi.BindingOrRegister, rhs.Layout.Kind, rhs.Layout.Name);
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

    std::sort(records.begin(), records.end(), _ParameterLess);

    D3D12MergedBindingLayout result{};
    vector<BindingParameterLayout> layouts{};
    layouts.reserve(records.size());
    result.D3D12Parameters.reserve(records.size());
    for (uint32_t i = 0; i < records.size(); ++i) {
        auto& record = records[i];
        record.Layout.Id = BindingParameterId{i};
        record.D3D12.Id = BindingParameterId{i};
        layouts.push_back(record.Layout);
        result.D3D12Parameters.push_back(record.D3D12);
    }
    result.Layout = BindingLayout{std::move(layouts)};
    return result;
}

}  // namespace radray::render::d3d12
