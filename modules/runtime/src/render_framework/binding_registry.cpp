#include <radray/runtime/render_framework/binding_registry.h>

#include <algorithm>
#include <radray/logger.h>

namespace radray {
BindingRegistry::BindingRegistry(render::Device* device, Guid owner) : _device(device), _owner(owner) {}
BindingRegistry::~BindingRegistry() noexcept {
    RADRAY_ASSERT(!HasActiveUses());
}
Nullable<BindingRegistry::Entry*> BindingRegistry::Find(DescriptorBlockId id) const noexcept {
    if (id.Owner != _owner || id.Index >= _slots.size() || _slots[id.Index].Generation != id.Generation) return nullptr;
    return _slots[id.Index].Value.get();
}
bool BindingRegistry::IsValid(DescriptorBlockId id) const noexcept { return Find(id).HasValue(); }
bool BindingRegistry::HasActiveUses() const noexcept {
    return std::any_of(_slots.begin(), _slots.end(), [](const auto& slot) { return slot.Value && slot.Value->Pins != 0; });
}
std::optional<DescriptorBlockId> BindingRegistry::Resolve(render::PipelineLayout* layout, uint32_t group,
                                                          std::span<const ParameterWrite> writes, std::span<const ResourceId> dependencies) {
    const auto compatibility = layout->GetParameterGroupCompatibility(group);
    if (!compatibility || compatibility->ValueCount() != writes.size()) return std::nullopt;
    vector<vector<uint64_t>> values(writes.size());
    for (const auto& write : writes) {
        const auto index = layout->GetParameterElementIndex(group, write.Binding, write.Element);
        if (!index || *index >= values.size() || !values[*index].empty()) return std::nullopt;
        auto value = render::GetShaderParameterValueIdentity(write.Value);
        if (!value) return std::nullopt;
        values[*index] = std::move(*value);
    }
    vector<ResourceId> resources{dependencies.begin(), dependencies.end()};
    for (const auto id : resources)
        if (!id.IsValid() || id.Owner != _owner) return std::nullopt;
    std::sort(resources.begin(), resources.end(), [](const auto& a, const auto& b) {
        return a.Index < b.Index || (a.Index == b.Index && a.Generation < b.Generation);
    });
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    HashCode hash{compatibility->Hash()};
    for (const auto& value : values) {
        hash.Add(value.size());
        for (const auto word : value) hash.Add(word);
    }
    for (const auto id : resources) {
        hash.Add(id.Index);
        hash.Add(id.Generation);
    }
    const auto key = hash.ToHashCode();
    const auto found = _lookup.find(key);
    if (found != _lookup.end()) {
        for (const auto index : found->second) {
            auto& entry = *_slots[index].Value;
            if (entry.Layout == *compatibility && entry.Values == values && entry.Dependencies == resources) {
                entry.LastUse = ++_clock;
                return DescriptorBlockId{_owner, index, _slots[index].Generation};
            }
        }
    }
    auto parameters = _device->CreateShaderParameterSet({layout, group});
    if (!parameters) {
        TrimIdle(true);
        parameters = _device->CreateShaderParameterSet({layout, group});
    }
    if (!parameters) return std::nullopt;
    for (const auto& write : writes)
        if (!parameters->Set(write.Binding, write.Element, write.Value)) return std::nullopt;
    if (!parameters->FlushWrites()) return std::nullopt;
    auto entry = make_unique<Entry>();
    entry->Layout = *compatibility;
    entry->Values = std::move(values);
    entry->Dependencies = std::move(resources);
    entry->Retiring = std::any_of(entry->Dependencies.begin(), entry->Dependencies.end(), [&](const auto id) { return _retiringResources.contains(id); });
    entry->Parameters = parameters.Release();
    entry->Hash = key;
    entry->LastUse = ++_clock;
    const uint32_t index = _free.empty() ? uint32_t(_slots.size()) : _free.back();
    if (_free.empty())
        _slots.emplace_back();
    else
        _free.pop_back();
    _idleResources += entry->Layout.ResourceDescriptors();
    _idleSamplers += entry->Layout.SamplerDescriptors();
    ++_idleGroups;
    _slots[index].Value = std::move(entry);
    _lookup[key].push_back(index);
    return DescriptorBlockId{_owner, index, _slots[index].Generation};
}
bool BindingRegistry::Retain(DescriptorBlockId id) {
    const auto entry = Find(id);
    if (!entry || entry->Pins == UINT32_MAX) return false;
    if (!entry->Pins++) {
        _idleResources -= entry->Layout.ResourceDescriptors();
        _idleSamplers -= entry->Layout.SamplerDescriptors();
        --_idleGroups;
    }
    return true;
}
bool BindingRegistry::Release(DescriptorBlockId id) {
    const auto entry = Find(id);
    if (!entry || !entry->Pins) return false;
    if (!--entry->Pins) {
        entry->LastUse = ++_clock;
        _idleResources += entry->Layout.ResourceDescriptors();
        _idleSamplers += entry->Layout.SamplerDescriptors();
        ++_idleGroups;
        if (entry->Retiring) Destroy(id.Index);
    }
    TrimIdle();
    return true;
}
bool BindingRegistry::Bind(render::GraphicsCommandEncoder* encoder, uint32_t destinationGroup, DescriptorBlockId id,
                           std::span<const render::ShaderParameterDynamicOffset> offsets) const {
    const auto entry = Find(id);
    if (!entry || !entry->Pins) return false;
    encoder->BindShaderParameterSet(destinationGroup, entry->Parameters.get(), offsets);
    return true;
}
void BindingRegistry::Destroy(uint32_t index) {
    auto& slot = _slots[index];
    auto& entry = *slot.Value;
    RADRAY_ASSERT(entry.Pins == 0);
    _idleResources -= entry.Layout.ResourceDescriptors();
    _idleSamplers -= entry.Layout.SamplerDescriptors();
    --_idleGroups;
    const auto found = _lookup.find(entry.Hash);
    std::erase(found->second, index);
    if (found->second.empty()) _lookup.erase(found);
    slot.Value.reset();
    if (slot.Generation != UINT32_MAX) {
        ++slot.Generation;
        _free.push_back(index);
    }
}
void BindingRegistry::RetireResource(ResourceId id) {
    _retiringResources.insert(id);
    for (uint32_t index = 0; index < _slots.size(); ++index) {
        const auto& entry = _slots[index].Value;
        if (!entry || std::find(entry->Dependencies.begin(), entry->Dependencies.end(), id) == entry->Dependencies.end()) continue;
        entry->Retiring = true;
        if (!entry->Pins) Destroy(index);
    }
}
void BindingRegistry::ForgetResource(ResourceId id) {
    RADRAY_ASSERT(!HasResource(id));
    _retiringResources.erase(id);
}
bool BindingRegistry::HasResource(ResourceId id) const {
    return std::any_of(_slots.begin(), _slots.end(), [&](const auto& slot) {
        return slot.Value && std::find(slot.Value->Dependencies.begin(), slot.Value->Dependencies.end(), id) != slot.Value->Dependencies.end();
    });
}
void BindingRegistry::SetBudget(BindingCacheBudget budget) {
    _budget = budget;
    TrimIdle();
}
void BindingRegistry::TrimIdle(bool all) {
    while (_idleGroups && (all || _idleResources > _budget.ResourceDescriptors || _idleSamplers > _budget.SamplerDescriptors || _idleGroups > _budget.Groups)) {
        uint32_t oldest = UINT32_MAX;
        for (uint32_t index = 0; index < _slots.size(); ++index) {
            const auto& entry = _slots[index].Value;
            if (entry && !entry->Pins && (oldest == UINT32_MAX || entry->LastUse < _slots[oldest].Value->LastUse)) oldest = index;
        }
        if (oldest == UINT32_MAX) break;
        Destroy(oldest);
    }
}
}  // namespace radray
