#include <radray/runtime/render_framework/mesh_pass_executor.h>

#include <algorithm>
#include <cstring>
#include <limits>

#include <fmt/format.h>

#include <radray/runtime/sampler_cache.h>
#include <radray/runtime/shader_default_resource_library.h>
#include <radray/runtime/render_framework/material_render_snapshot.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_queue.h>

namespace radray {
namespace {

enum class ValueStatus {
    Ready,
    Missing,
    Pending,
    Invalid,
};

struct PendingWrite {
    ShaderBindingEntryKind Kind{ShaderBindingEntryKind::Texture};
    uint32_t Binding{0};
    render::BufferBindingDescriptor Buffer{};
    render::ResourceView* Resource{nullptr};
    render::Sampler* Sampler{nullptr};
    uint32_t DynamicOffset{0};
    bool HasDynamicOffset{false};
};

const MaterialRenderSnapshot::ConstantEntry* FindConstant(
    const MaterialRenderSnapshot::PropertyLayer& layer,
    std::string_view name) noexcept {
    auto it = std::ranges::find(layer.Constants, name, &MaterialRenderSnapshot::ConstantEntry::Name);
    return it != layer.Constants.end() ? &*it : nullptr;
}

const MaterialRenderSnapshot::TextureEntry* FindTexture(
    const MaterialRenderSnapshot::PropertyLayer& layer,
    std::string_view name) noexcept {
    auto it = std::ranges::find(layer.Textures, name, &MaterialRenderSnapshot::TextureEntry::Name);
    return it != layer.Textures.end() ? &*it : nullptr;
}

const MaterialRenderSnapshot::SamplerEntry* FindSampler(
    const MaterialRenderSnapshot::PropertyLayer& layer,
    std::string_view name) noexcept {
    auto it = std::ranges::find(layer.Samplers, name, &MaterialRenderSnapshot::SamplerEntry::Name);
    return it != layer.Samplers.end() ? &*it : nullptr;
}

bool LayerContainsDifferentKind(
    const MaterialRenderSnapshot::PropertyLayer& layer,
    std::string_view name,
    ShaderPropertyKind expected) noexcept {
    if (expected != ShaderPropertyKind::Texture && FindTexture(layer, name) != nullptr) {
        return true;
    }
    if (expected != ShaderPropertyKind::Sampler && FindSampler(layer, name) != nullptr) {
        return true;
    }
    if (expected != ShaderPropertyKind::Float &&
        expected != ShaderPropertyKind::Vector &&
        expected != ShaderPropertyKind::Bytes &&
        FindConstant(layer, name) != nullptr) {
        return true;
    }
    return false;
}

ValueStatus ApplyConstantValue(
    const MaterialPropertyValue& value,
    std::span<byte> destination) noexcept {
    return std::visit(
        [&](const auto& v) noexcept {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>) {
                if (destination.size() != sizeof(float)) {
                    return ValueStatus::Invalid;
                }
                std::memcpy(destination.data(), &v, sizeof(float));
                return ValueStatus::Ready;
            } else if constexpr (std::is_same_v<T, Eigen::Vector4f>) {
                if (destination.size() != sizeof(float) * 4) {
                    return ValueStatus::Invalid;
                }
                std::memcpy(destination.data(), v.data(), sizeof(float) * 4);
                return ValueStatus::Ready;
            } else if constexpr (std::is_same_v<T, vector<byte>>) {
                if (v.size() > destination.size()) {
                    return ValueStatus::Invalid;
                }
                if (!v.empty()) {
                    std::memcpy(destination.data(), v.data(), v.size());
                }
                return ValueStatus::Ready;
            } else {
                return ValueStatus::Invalid;
            }
        },
        value);
}

ValueStatus ApplyShaderDefault(
    const ShaderAsset& shader,
    std::string_view name,
    std::span<byte> destination) noexcept {
    const ShaderPropertyDesc* property = shader.FindProperty(name).Get();
    if (property == nullptr || !property->DefaultValue.has_value()) {
        return ValueStatus::Missing;
    }
    return ApplyConstantValue(*property->DefaultValue, destination);
}

ValueStatus ApplyLayerConstant(
    const ShaderAsset& shader,
    const MaterialRenderSnapshot::PropertyLayer& layer,
    std::string_view name,
    std::span<byte> destination) noexcept {
    const MaterialRenderSnapshot::ConstantEntry* entry = FindConstant(layer, name);
    if (entry == nullptr) {
        return LayerContainsDifferentKind(layer, name, ShaderPropertyKind::Bytes)
                   ? ValueStatus::Invalid
                   : ValueStatus::Missing;
    }
    const ShaderPropertyDesc* property = shader.FindProperty(name).Get();
    if (property != nullptr && property->Kind != entry->Kind) {
        return ValueStatus::Invalid;
    }
    if ((entry->Kind == ShaderPropertyKind::Float &&
         destination.size() != sizeof(float)) ||
        (entry->Kind == ShaderPropertyKind::Vector &&
         destination.size() != sizeof(float) * 4)) {
        return ValueStatus::Invalid;
    }
    if (entry->Bytes.size() > destination.size()) {
        return ValueStatus::Invalid;
    }
    if (!entry->Bytes.empty()) {
        std::memcpy(destination.data(), entry->Bytes.data(), entry->Bytes.size());
    }
    return ValueStatus::Ready;
}

ShaderParameterSet* SelectParameterSet(
    ShaderParameterScope scope,
    ShaderParameterSet& view,
    ShaderParameterSet& pass) noexcept {
    switch (scope) {
        case ShaderParameterScope::View: return &view;
        case ShaderParameterScope::Pass: return &pass;
        default: return nullptr;
    }
}

ValueStatus ResolveTexturePropertyValue(
    const MaterialPropertyValue& value,
    ShaderDefaultResourceLibrary* defaults,
    render::TextureView** view) noexcept {
    return std::visit(
        [&](const auto& v) noexcept {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, StreamingAssetRef<TextureAsset>>) {
                TextureAsset* texture = v.Get();
                if (texture != nullptr) {
                    *view = texture->GetSrv();
                    return *view != nullptr ? ValueStatus::Ready : ValueStatus::Invalid;
                }
                return v.IsValid() && !v.IsCompleted() ? ValueStatus::Pending : ValueStatus::Missing;
            } else if constexpr (std::is_same_v<T, TextureSubViewRef>) {
                TextureAsset* texture = v.Texture.Get();
                if (texture != nullptr) {
                    *view = texture->GetOrCreateSrv(v.SubView);
                    return *view != nullptr ? ValueStatus::Ready : ValueStatus::Invalid;
                }
                return v.Texture.IsValid() && !v.Texture.IsCompleted()
                           ? ValueStatus::Pending
                           : ValueStatus::Missing;
            } else if constexpr (std::is_same_v<T, ShaderDefaultTexture>) {
                if (defaults == nullptr) {
                    return ValueStatus::Invalid;
                }
                const ShaderDefaultTextureResult result = defaults->ResolveTexture(v);
                *view = result.View.Get();
                switch (result.Status) {
                    case ShaderDefaultResourceStatus::Ready: return ValueStatus::Ready;
                    case ShaderDefaultResourceStatus::Pending: return ValueStatus::Pending;
                    case ShaderDefaultResourceStatus::Invalid: return ValueStatus::Invalid;
                }
                return ValueStatus::Invalid;
            } else {
                return ValueStatus::Invalid;
            }
        },
        value);
}

ValueStatus ResolveTextureEntry(
    const MaterialRenderSnapshot::TextureEntry& entry,
    ShaderDefaultResourceLibrary* defaults,
    render::TextureView** view) noexcept {
    if (entry.DefaultTexture.has_value()) {
        return ResolveTexturePropertyValue(*entry.DefaultTexture, defaults, view);
    }
    TextureAsset* texture = entry.Texture.Get();
    if (texture != nullptr) {
        *view = texture->GetOrCreateSrv(entry.SubView);
        return *view != nullptr ? ValueStatus::Ready : ValueStatus::Invalid;
    }
    return entry.Texture.IsValid() && !entry.Texture.IsCompleted()
               ? ValueStatus::Pending
               : ValueStatus::Missing;
}

void AppendDescriptorWord(vector<uint64_t>& key, uint64_t value) {
    key.push_back(value);
}

}  // namespace

MeshPassExecutor::BindingResolveResult MeshPassExecutor::ResolveBindingGroups(
    const DrawItem& item,
    const CompiledShaderVariant& variant,
    FrameResources& resources) noexcept {
    BindingResolveResult result{};
    if (item.Material == nullptr || item.Proxy == nullptr || variant.Layout == nullptr || _device == nullptr) {
        result.Diagnostic.Reason = "draw has no material, proxy, layout, or device";
        return result;
    }
    ShaderAsset* shader = item.Material->Shader.Get();
    if (shader == nullptr) {
        result.Diagnostic.Reason = "material shader is unavailable";
        return result;
    }
    result.Diagnostic.ProgramId = shader->GetProgramId();
    result.Diagnostic.PassIndex = item.PassIndex;
    result.Diagnostic.VariantKey = variant.Key;

    const uint64_t oldPlanHits = _bindingPlans.GetHitCount();
    const uint64_t oldPlanMisses = _bindingPlans.GetMissCount();
    const ShaderBindingPlan* plan = _bindingPlans.GetOrCreate(*shader, item.PassIndex, variant).Get();
    resources.Counters.BindingPlanCacheHits += _bindingPlans.GetHitCount() - oldPlanHits;
    resources.Counters.BindingPlanCacheMisses += _bindingPlans.GetMissCount() - oldPlanMisses;
    if (plan == nullptr || !plan->Valid) {
        result.Diagnostic.Reason = plan != nullptr ? plan->Error : "failed to build shader binding plan";
        if (plan != nullptr) {
            result.Diagnostic.Group = plan->ErrorGroup;
            result.Diagnostic.Binding = plan->ErrorBinding;
        }
        ++resources.Counters.BindingResolutionFailures;
        return result;
    }

    if (_materialDescriptorPool == nullptr) {
        auto poolOpt = _device->CreateDescriptorPool(render::DescriptorPoolDescriptor{
            .MaxBindingGroups = 512,
            .MaxSampledTextures = 4096,
            .MaxStorageTextures = 128,
            .MaxUniformBuffers = 512,
            .MaxDynamicUniformBuffers = 128,
            .MaxStorageBuffers = 128,
            .MaxReadOnlyTexelBuffers = 64,
            .MaxReadWriteTexelBuffers = 64,
            .MaxSamplers = 4096,
            .MaxAccelerationStructures = 0,
            .Lifetime = render::DescriptorPoolLifetime::Persistent});
        if (!poolOpt.HasValue()) {
            result.Diagnostic.Reason = "failed to create the persistent material descriptor pool";
            ++resources.Counters.BindingResolutionFailures;
            return result;
        }
        _materialDescriptorPool = shared_ptr<render::DescriptorPool>{poolOpt.Release()};
        _materialDescriptorPool->SetDebugName("material_descriptors");
    }
    if (_materialConstantPool == nullptr) {
        _materialConstantPool = make_shared<MaterialConstantPool>(
            _device,
            256 * 1024,
            std::max<uint64_t>(256, _device->GetDetail().CBufferAlignment));
    }

    const auto resolveProviderConstant = [&](const ShaderBindingSource& source, vector<byte>& out) {
        const ShaderParameterSet::ConstantEntry* value = nullptr;
        if (source.Scope == ShaderParameterScope::View) {
            value = _viewParameters.FindConstant(source.ProviderName).Get();
        } else if (source.Scope == ShaderParameterScope::Pass) {
            value = _passParameters.FindConstant(source.ProviderName).Get();
        }
        if (value != nullptr) {
            out = value->Bytes;
            return true;
        }
        if (source.Scope != ShaderParameterScope::Object) {
            return false;
        }
        if (source.ProviderName != _perObjectName &&
            source.ProviderName != "ObjectToWorld" &&
            source.ProviderName != "radray.ObjectToWorld") {
            return false;
        }
        PerObjectConstants constants{};
        const Eigen::Matrix4f matrix = item.Proxy->GetLocalToWorld();
        std::memcpy(constants.ObjectToWorld, matrix.data(), sizeof(constants.ObjectToWorld));
        out.resize(sizeof(constants));
        std::memcpy(out.data(), &constants, sizeof(constants));
        return true;
    };

    const auto applyMaterialLayer = [&](
                                        const ShaderBindingEntryPlan& entry,
                                        const MaterialRenderSnapshot::PropertyLayer& layer,
                                        vector<byte>& bytes,
                                        bool fields) {
        const std::string_view blockName = entry.Source.ProviderName.empty()
                                               ? std::string_view{entry.Name}
                                               : std::string_view{entry.Source.ProviderName};
        if (!fields) {
            return ApplyLayerConstant(*shader, layer, blockName, bytes);
        }
        ValueStatus status = ValueStatus::Missing;
        for (const ShaderConstantFieldPlan& field : entry.Fields) {
            if (field.Source.Scope != ShaderParameterScope::Material || field.Offset >= bytes.size()) {
                continue;
            }
            const std::string_view name = field.Source.ProviderName.empty()
                                              ? std::string_view{field.Name}
                                              : std::string_view{field.Source.ProviderName};
            const ValueStatus fieldStatus = ApplyLayerConstant(
                *shader,
                layer,
                name,
                std::span<byte>{bytes}.subspan(
                    field.Offset,
                    std::min<size_t>(field.Size, bytes.size() - field.Offset)));
            if (fieldStatus == ValueStatus::Invalid) {
                return fieldStatus;
            }
            if (fieldStatus == ValueStatus::Ready) {
                status = fieldStatus;
            }
        }
        return status;
    };

    for (const ShaderBindingGroupPlan& groupPlan : plan->Groups) {
        if (groupPlan.Frequency != ShaderBindingFrequency::Material) {
            const void* objectKey = groupPlan.Frequency == ShaderBindingFrequency::Object
                                        ? static_cast<const void*>(item.Proxy)
                                        : nullptr;
            const auto resolved = std::ranges::find_if(
                resources.ResolvedBindingStates,
                [&](const FrameResolvedBindingStateCacheEntry& cached) {
                    return cached.Plan == plan &&
                           cached.GroupIndex == groupPlan.Group &&
                           cached.MaterialKeyLo == item.Material->BindingKey.Lo &&
                           cached.MaterialKeyHi == item.Material->BindingKey.Hi &&
                           cached.Object == objectKey &&
                           cached.ViewRevision == _viewParameters.GetRevision() &&
                           cached.PassRevision == _passParameters.GetRevision();
                });
            if (resolved != resources.ResolvedBindingStates.end()) {
                ++resources.Counters.SystemGroupCacheHits;
                if (result.ObjectBufferPage == 0) {
                    result.ObjectBufferPage = resolved->ObjectBufferPage;
                }
                result.Groups.push_back(MeshDrawCommand::BindingGroupState{
                    .GroupIndex = groupPlan.Group,
                    .Group = resolved->Group,
                    .DynamicOffsets = resolved->DynamicOffsets});
                continue;
            }
        }

        if (groupPlan.Frequency == ShaderBindingFrequency::Material) {
            auto cached = std::ranges::find_if(
                _genericMaterialBindings,
                [&](GenericMaterialBinding& binding) {
                    return binding.Key == item.Material->BindingKey &&
                           binding.Layout == variant.Layout &&
                           binding.GroupIndex == groupPlan.Group;
                });
            if (cached != _genericMaterialBindings.end()) {
                cached->LastUsedFrame = _frameSerial;
                ++resources.Counters.MaterialGroupCacheHits;
                result.Groups.push_back(MeshDrawCommand::BindingGroupState{
                    .GroupIndex = groupPlan.Group,
                    .Group = cached->Group.get(),
                    .DynamicOffsets = cached->DynamicOffsets});
                continue;
            }
            ++resources.Counters.MaterialGroupCacheMisses;
        }

        vector<PendingWrite> writes;
        vector<uint64_t> descriptorKey;
        vector<MaterialConstantPool::Allocation> persistentAllocations;
        const auto releasePersistent = [&]() {
            for (const MaterialConstantPool::Allocation& allocation : persistentAllocations) {
                _materialConstantPool->Release(allocation);
            }
            persistentAllocations.clear();
        };
        ValueStatus groupStatus = ValueStatus::Ready;
        string groupError;
        uint32_t groupErrorBinding = std::numeric_limits<uint32_t>::max();

        for (uint32_t entryIndex : groupPlan.EntryIndices) {
            const ShaderBindingEntryPlan& entry = plan->Entries[entryIndex];
            groupErrorBinding = entry.Binding;
            PendingWrite write{
                .Kind = entry.Kind,
                .Binding = entry.Binding,
                .HasDynamicOffset = entry.HasDynamicOffset};
            AppendDescriptorWord(descriptorKey, entry.Binding);
            AppendDescriptorWord(descriptorKey, static_cast<uint64_t>(entry.Kind));

            if (entry.Kind == ShaderBindingEntryKind::StaticSampler) {
                writes.push_back(write);
                continue;
            }
            if (entry.Kind == ShaderBindingEntryKind::ConstantBuffer) {
                vector<byte> bytes(entry.ByteSize, byte{0});
                if (entry.Source.Explicit && entry.Source.Scope != ShaderParameterScope::Material) {
                    vector<byte> provider;
                    if (!resolveProviderConstant(entry.Source, provider)) {
                        groupStatus = ValueStatus::Invalid;
                        groupError = fmt::format("missing {} cbuffer provider '{}'", entry.Name, entry.Source.ProviderName);
                        break;
                    }
                    if (provider.size() != bytes.size()) {
                        groupStatus = ValueStatus::Invalid;
                        groupError = fmt::format(
                            "cbuffer provider '{}' has {} bytes; '{}' requires {}",
                            entry.Source.ProviderName,
                            provider.size(),
                            entry.Name,
                            bytes.size());
                        break;
                    }
                    std::memcpy(bytes.data(), provider.data(), bytes.size());
                } else {
                    const std::string_view blockName = entry.Source.ProviderName.empty()
                                                           ? std::string_view{entry.Name}
                                                           : std::string_view{entry.Source.ProviderName};
                    const ValueStatus defaultBlock = ApplyShaderDefault(*shader, blockName, bytes);
                    if (defaultBlock == ValueStatus::Invalid) {
                        groupStatus = defaultBlock;
                        groupError = fmt::format("shader property '{}' is not a constant", blockName);
                        break;
                    }
                    for (const ShaderConstantFieldPlan& field : entry.Fields) {
                        if (field.Source.Scope != ShaderParameterScope::Material || field.Offset >= bytes.size()) {
                            continue;
                        }
                        const std::string_view name = field.Source.ProviderName.empty()
                                                          ? std::string_view{field.Name}
                                                          : std::string_view{field.Source.ProviderName};
                        const ValueStatus status = ApplyShaderDefault(
                            *shader,
                            name,
                            std::span<byte>{bytes}.subspan(
                                field.Offset,
                                std::min<size_t>(field.Size, bytes.size() - field.Offset)));
                        if (status == ValueStatus::Invalid) {
                            groupStatus = status;
                            groupError = fmt::format("shader property '{}' is not a constant", name);
                            break;
                        }
                    }
                    if (groupStatus != ValueStatus::Ready) {
                        break;
                    }
                    for (const auto* layer : {
                             &item.Material->MaterialProperties,
                             &item.Material->PropertyBlockProperties}) {
                        if (applyMaterialLayer(entry, *layer, bytes, false) == ValueStatus::Invalid ||
                            applyMaterialLayer(entry, *layer, bytes, true) == ValueStatus::Invalid) {
                            groupStatus = ValueStatus::Invalid;
                            groupError = fmt::format("material property for cbuffer '{}' has the wrong type", entry.Name);
                            break;
                        }
                    }
                    if (groupStatus != ValueStatus::Ready) {
                        break;
                    }
                    for (const ShaderConstantFieldPlan& field : entry.Fields) {
                        if (!field.Source.Explicit || field.Source.Scope == ShaderParameterScope::Material ||
                            field.Offset >= bytes.size()) {
                            continue;
                        }
                        vector<byte> provider;
                        if (!resolveProviderConstant(field.Source, provider)) {
                            groupStatus = ValueStatus::Invalid;
                            groupError = fmt::format(
                                "missing provider '{}' for {}.{}",
                                field.Source.ProviderName,
                                entry.Name,
                                field.Name);
                            break;
                        }
                        if (provider.size() != field.Size) {
                            groupStatus = ValueStatus::Invalid;
                            groupError = fmt::format(
                                "provider '{}' has {} bytes; {}.{} requires {}",
                                field.Source.ProviderName,
                                provider.size(),
                                entry.Name,
                                field.Name,
                                field.Size);
                            break;
                        }
                        std::memcpy(bytes.data() + field.Offset, provider.data(), field.Size);
                    }
                    if (groupStatus != ValueStatus::Ready) {
                        break;
                    }
                }

                render::Buffer* target = nullptr;
                uint64_t offset = 0;
                const DynamicCBufferArena::Allocation* sharedAllocation = nullptr;
                if (entry.Source.Explicit &&
                    entry.Source.Scope == ShaderParameterScope::Object &&
                    (entry.Source.ProviderName == _perObjectName ||
                     entry.Source.ProviderName == "ObjectToWorld" ||
                     entry.Source.ProviderName == "radray.ObjectToWorld")) {
                    sharedAllocation = EnsureObjectBinding(resources, item.Proxy).Get();
                } else if (entry.Source.Explicit &&
                           entry.Source.Scope == ShaderParameterScope::View &&
                           entry.Source.ProviderName == _viewName) {
                    sharedAllocation = EnsureViewBinding(resources).Get();
                }

                if (sharedAllocation != nullptr) {
                    target = sharedAllocation->Target;
                    offset = sharedAllocation->Offset;
                } else if (groupPlan.Frequency == ShaderBindingFrequency::Material) {
                    const auto allocation = _materialConstantPool->Allocate(bytes.size());
                    if (!allocation.IsValid() || allocation.Mapped == nullptr) {
                        groupStatus = ValueStatus::Invalid;
                        groupError = fmt::format("failed to allocate material cbuffer '{}'", entry.Name);
                        break;
                    }
                    std::memcpy(allocation.Mapped, bytes.data(), bytes.size());
                    target = allocation.Target;
                    offset = allocation.Offset;
                    persistentAllocations.push_back(allocation);
                } else {
                    DynamicCBufferArena& arena =
                        groupPlan.Frequency == ShaderBindingFrequency::Object
                            ? resources.PerObjectArena
                            : resources.ViewArena;
                    const auto allocation = arena.Allocate(bytes.size());
                    if (allocation.Target == nullptr || allocation.Mapped == nullptr) {
                        groupStatus = ValueStatus::Invalid;
                        groupError = fmt::format("failed to allocate transient cbuffer '{}'", entry.Name);
                        break;
                    }
                    std::memcpy(allocation.Mapped, bytes.data(), bytes.size());
                    target = allocation.Target;
                    offset = allocation.Offset;
                }
                if (target == nullptr) {
                    groupStatus = ValueStatus::Invalid;
                    groupError = fmt::format("failed to resolve cbuffer '{}' storage", entry.Name);
                    break;
                }
                if (entry.Frequency == ShaderBindingFrequency::Object && result.ObjectBufferPage == 0) {
                    result.ObjectBufferPage = reinterpret_cast<uint64_t>(target);
                }
                if (entry.HasDynamicOffset && offset > std::numeric_limits<uint32_t>::max()) {
                    groupStatus = ValueStatus::Invalid;
                    groupError = fmt::format("dynamic cbuffer '{}' offset exceeds uint32", entry.Name);
                    break;
                }
                write.Buffer.Target = target;
                write.Buffer.Range = render::BufferRange{
                    .Offset = entry.HasDynamicOffset ? 0u : offset,
                    .Size = bytes.size()};
                write.Buffer.Usage = render::BufferViewUsage::CBuffer;
                write.DynamicOffset = entry.HasDynamicOffset ? static_cast<uint32_t>(offset) : 0u;
                AppendDescriptorWord(descriptorKey, reinterpret_cast<uint64_t>(target));
                AppendDescriptorWord(descriptorKey, write.Buffer.Range.Offset);
                AppendDescriptorWord(descriptorKey, write.Buffer.Range.Size);
                writes.push_back(write);
                continue;
            }

            if (entry.Kind == ShaderBindingEntryKind::Texture) {
                render::TextureView* textureView = nullptr;
                ValueStatus status = ValueStatus::Missing;
                if (ShaderParameterSet* parameters = SelectParameterSet(
                        entry.Source.Scope, _viewParameters, _passParameters)) {
                    render::ResourceView* resource = parameters->FindResource(entry.Source.ProviderName).Get();
                    if (resource != nullptr && resource->GetTag() != render::RenderObjectTag::TextureView) {
                        status = ValueStatus::Invalid;
                    } else {
                        textureView = static_cast<render::TextureView*>(resource);
                        status = textureView != nullptr ? ValueStatus::Ready : ValueStatus::Missing;
                    }
                } else if (entry.Source.Scope == ShaderParameterScope::Material) {
                    const std::string_view name = entry.Source.ProviderName;
                    const auto resolveLayer = [&](const MaterialRenderSnapshot::PropertyLayer& layer) {
                        if (const auto* texture = FindTexture(layer, name); texture != nullptr) {
                            return ResolveTextureEntry(*texture, _defaultResources, &textureView);
                        }
                        return LayerContainsDifferentKind(layer, name, ShaderPropertyKind::Texture)
                                   ? ValueStatus::Invalid
                                   : ValueStatus::Missing;
                    };
                    status = resolveLayer(item.Material->PropertyBlockProperties);
                    if (status == ValueStatus::Missing) {
                        status = resolveLayer(item.Material->MaterialProperties);
                    }
                    if (status == ValueStatus::Missing) {
                        const ShaderPropertyDesc* property = shader->FindProperty(name).Get();
                        status = property != nullptr && property->DefaultValue.has_value()
                                     ? ResolveTexturePropertyValue(*property->DefaultValue, _defaultResources, &textureView)
                                     : ValueStatus::Missing;
                    }
                } else {
                    status = ValueStatus::Invalid;
                }
                if (status != ValueStatus::Ready) {
                    groupStatus = status == ValueStatus::Pending ? ValueStatus::Pending : ValueStatus::Invalid;
                    groupError = fmt::format("texture '{}' is not available", entry.Source.ProviderName);
                    break;
                }
                write.Resource = textureView;
                AppendDescriptorWord(descriptorKey, reinterpret_cast<uint64_t>(textureView));
                writes.push_back(write);
                continue;
            }

            if (entry.Kind == ShaderBindingEntryKind::Sampler) {
                render::Sampler* sampler = nullptr;
                ValueStatus status = ValueStatus::Missing;
                if (ShaderParameterSet* parameters = SelectParameterSet(
                        entry.Source.Scope, _viewParameters, _passParameters)) {
                    sampler = parameters->FindSampler(entry.Source.ProviderName).Get();
                    status = sampler != nullptr ? ValueStatus::Ready : ValueStatus::Missing;
                } else if (entry.Source.Scope == ShaderParameterScope::Material) {
                    const std::string_view name = entry.Source.ProviderName;
                    const auto resolveLayer = [&](const MaterialRenderSnapshot::PropertyLayer& layer) {
                        if (const auto* value = FindSampler(layer, name); value != nullptr) {
                            sampler = _samplerCache != nullptr
                                          ? _samplerCache->GetOrCreate(value->Desc).Get()
                                          : nullptr;
                            return sampler != nullptr ? ValueStatus::Ready : ValueStatus::Invalid;
                        }
                        return LayerContainsDifferentKind(layer, name, ShaderPropertyKind::Sampler)
                                   ? ValueStatus::Invalid
                                   : ValueStatus::Missing;
                    };
                    status = resolveLayer(item.Material->PropertyBlockProperties);
                    if (status == ValueStatus::Missing) {
                        status = resolveLayer(item.Material->MaterialProperties);
                    }
                    if (status == ValueStatus::Missing) {
                        const ShaderPropertyDesc* property = shader->FindProperty(name).Get();
                        if (property != nullptr && property->DefaultValue.has_value()) {
                            if (const auto* desc = std::get_if<render::SamplerDescriptor>(&*property->DefaultValue)) {
                                sampler = _samplerCache != nullptr
                                              ? _samplerCache->GetOrCreate(*desc).Get()
                                              : nullptr;
                                status = sampler != nullptr ? ValueStatus::Ready : ValueStatus::Invalid;
                            } else {
                                status = ValueStatus::Invalid;
                            }
                        }
                    }
                } else {
                    status = ValueStatus::Invalid;
                }
                if (status != ValueStatus::Ready) {
                    groupStatus = ValueStatus::Invalid;
                    groupError = fmt::format("sampler '{}' is not available", entry.Source.ProviderName);
                    break;
                }
                write.Sampler = sampler;
                AppendDescriptorWord(descriptorKey, reinterpret_cast<uint64_t>(sampler));
                writes.push_back(write);
            }
        }

        if (groupStatus != ValueStatus::Ready) {
            releasePersistent();
            result.Status = groupStatus == ValueStatus::Pending
                                ? BindingResolveStatus::Pending
                                : BindingResolveStatus::Invalid;
            result.Diagnostic.Group = groupPlan.Group;
            if (groupErrorBinding != std::numeric_limits<uint32_t>::max()) {
                result.Diagnostic.Binding = groupErrorBinding;
            }
            result.Diagnostic.Reason = std::move(groupError);
            if (result.Status == BindingResolveStatus::Invalid) {
                ++resources.Counters.BindingResolutionFailures;
            }
            return result;
        }

        render::BindingGroup* group = nullptr;
        const bool persistentSystemDescriptors =
            groupPlan.Frequency != ShaderBindingFrequency::Material &&
            std::ranges::all_of(groupPlan.EntryIndices, [&](uint32_t entryIndex) {
                const ShaderBindingEntryPlan& entry = plan->Entries[entryIndex];
                return entry.Kind == ShaderBindingEntryKind::StaticSampler ||
                       (entry.Kind == ShaderBindingEntryKind::ConstantBuffer &&
                        entry.HasDynamicOffset);
            });
        if (groupPlan.Frequency != ShaderBindingFrequency::Material) {
            auto cached = std::ranges::find_if(
                resources.ResolvedGroups,
                [&](const FrameResolvedBindingGroupCacheEntry& entry) {
                    return entry.Layout == variant.Layout &&
                           entry.GroupIndex == groupPlan.Group &&
                           entry.DescriptorKey == descriptorKey;
                });
            if (cached != resources.ResolvedGroups.end()) {
                group = cached->Group.get();
                ++resources.Counters.SystemGroupCacheHits;
            } else {
                ++resources.Counters.SystemGroupCacheMisses;
            }
        }

        unique_ptr<render::BindingGroup> ownedGroup;
        if (group == nullptr) {
            render::DescriptorPool* pool =
                groupPlan.Frequency == ShaderBindingFrequency::Material
                    ? _materialDescriptorPool.get()
                    : persistentSystemDescriptors
                          ? resources.SystemDescriptorPool.get()
                          : resources.TransientDescriptorPool.get();
            auto groupOpt = _device->CreateBindingGroup(pool, variant.Layout, groupPlan.Group);
            if (!groupOpt.HasValue()) {
                releasePersistent();
                result.Diagnostic.Group = groupPlan.Group;
                result.Diagnostic.Reason = fmt::format("failed to create binding group {}", groupPlan.Group);
                ++resources.Counters.BindingResolutionFailures;
                return result;
            }
            ownedGroup = groupOpt.Release();
            group = ownedGroup.get();
            uint64_t updates = 0;
            for (const PendingWrite& write : writes) {
                bool applied = true;
                switch (write.Kind) {
                    case ShaderBindingEntryKind::ConstantBuffer:
                        applied = group->SetResource(write.Binding, write.Buffer);
                        break;
                    case ShaderBindingEntryKind::Texture:
                        applied = group->SetResource(write.Binding, write.Resource);
                        break;
                    case ShaderBindingEntryKind::Sampler:
                        applied = group->SetSampler(write.Binding, write.Sampler);
                        break;
                    case ShaderBindingEntryKind::StaticSampler:
                        continue;
                }
                if (!applied) {
                    releasePersistent();
                    result.Diagnostic.Group = groupPlan.Group;
                    result.Diagnostic.Binding = write.Binding;
                    result.Diagnostic.Reason = fmt::format(
                        "failed to write group {} binding {}",
                        groupPlan.Group,
                        write.Binding);
                    ++resources.Counters.BindingResolutionFailures;
                    return result;
                }
                ++updates;
            }
            if (!group->IsFullyWritten()) {
                releasePersistent();
                result.Diagnostic.Group = groupPlan.Group;
                result.Diagnostic.Reason = fmt::format(
                    "binding group {} is not fully written",
                    groupPlan.Group);
                ++resources.Counters.BindingResolutionFailures;
                return result;
            }
            ++resources.Counters.DescriptorGroupCreates;
            resources.Counters.DescriptorGroupUpdates += updates;
        }

        vector<uint32_t> dynamicOffsets;
        dynamicOffsets.reserve(groupPlan.DynamicEntryIndices.size());
        for (uint32_t dynamicEntry : groupPlan.DynamicEntryIndices) {
            const uint32_t binding = plan->Entries[dynamicEntry].Binding;
            const auto write = std::ranges::find(writes, binding, &PendingWrite::Binding);
            if (write != writes.end()) {
                dynamicOffsets.push_back(write->DynamicOffset);
            }
        }

        if (ownedGroup != nullptr) {
            if (groupPlan.Frequency == ShaderBindingFrequency::Material) {
                GenericMaterialBinding& binding = _genericMaterialBindings.emplace_back(GenericMaterialBinding{
                    .Key = item.Material->BindingKey,
                    .Snapshot = item.Material,
                    .Layout = variant.Layout,
                    .GroupIndex = groupPlan.Group,
                    .Group = std::move(ownedGroup),
                    .ConstantAllocations = std::move(persistentAllocations),
                    .DynamicOffsets = dynamicOffsets,
                    .LastUsedFrame = _frameSerial});
                group = binding.Group.get();
            } else {
                vector<render::Buffer*> dynamicBuffers;
                for (const PendingWrite& write : writes) {
                    if (write.Kind == ShaderBindingEntryKind::ConstantBuffer &&
                        write.HasDynamicOffset && write.Buffer.Target != nullptr &&
                        std::ranges::find(dynamicBuffers, write.Buffer.Target) == dynamicBuffers.end()) {
                        dynamicBuffers.push_back(write.Buffer.Target);
                    }
                }
                FrameResolvedBindingGroupCacheEntry& binding = resources.ResolvedGroups.emplace_back(
                    FrameResolvedBindingGroupCacheEntry{
                        .Layout = variant.Layout,
                        .GroupIndex = groupPlan.Group,
                        .DescriptorKey = std::move(descriptorKey),
                        .DynamicBuffers = std::move(dynamicBuffers),
                        .Persistent = persistentSystemDescriptors,
                        .Group = std::move(ownedGroup)});
                group = binding.Group.get();
            }
        }
        MeshDrawCommand::BindingGroupState resolvedState{
            .GroupIndex = groupPlan.Group,
            .Group = group,
            .DynamicOffsets = std::move(dynamicOffsets)};
        if (groupPlan.Frequency != ShaderBindingFrequency::Material) {
            resources.ResolvedBindingStates.push_back(FrameResolvedBindingStateCacheEntry{
                .Plan = plan,
                .GroupIndex = groupPlan.Group,
                .MaterialKeyLo = item.Material->BindingKey.Lo,
                .MaterialKeyHi = item.Material->BindingKey.Hi,
                .Object = groupPlan.Frequency == ShaderBindingFrequency::Object
                              ? static_cast<const void*>(item.Proxy)
                              : nullptr,
                .ViewRevision = _viewParameters.GetRevision(),
                .PassRevision = _passParameters.GetRevision(),
                .Group = group,
                .DynamicOffsets = resolvedState.DynamicOffsets,
                .ObjectBufferPage = result.ObjectBufferPage});
        }
        result.Groups.push_back(std::move(resolvedState));
    }

    result.Status = BindingResolveStatus::Ready;
    return result;
}

}  // namespace radray
