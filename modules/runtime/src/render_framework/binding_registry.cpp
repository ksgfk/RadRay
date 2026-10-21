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
uint32_t BindingRegistry::Intern(const render::ParameterGroupCompatibility& compatibility) {
    const size_t hash = compatibility.Hash();
    auto& candidates = _compatLookup[hash];
    for (const auto index : candidates)
        if (_compatibilities[index].Value == compatibility) return index;
    const auto index = uint32_t(_compatibilities.size());
    _compatibilities.push_back({compatibility, hash});
    candidates.push_back(index);
    return index;
}
uint32_t BindingRegistry::InternCompatibility(render::PipelineLayout* layout, uint32_t group) {
    const auto compatibility = layout->GetParameterGroupCompatibility(group);
    return compatibility ? Intern(*compatibility) : UINT32_MAX;
}
std::optional<DescriptorBlockId> BindingRegistry::Resolve(render::PipelineLayout* layout, uint32_t group,
                                                          std::span<const ParameterWrite> writes, std::span<const ResourceId> dependencies,
                                                          std::span<const uint32_t> elementIndices, uint32_t compatibility) {
    if (compatibility == UINT32_MAX) {
        const auto value = layout->GetParameterGroupCompatibility(group);
        if (!value) return std::nullopt;
        compatibility = Intern(*value);
    }
    if (compatibility >= _compatibilities.size()) return std::nullopt;
    const auto& compatible = _compatibilities[compatibility].Value;
    if (compatible.ValueCount() != writes.size()) return std::nullopt;
    if (!elementIndices.empty() && elementIndices.size() != writes.size()) return std::nullopt;
    vector<vector<uint64_t>> values(writes.size());
    for (size_t slot = 0; slot < writes.size(); ++slot) {
        const auto& write = writes[slot];
        uint32_t index;
        if (!elementIndices.empty()) {
            index = elementIndices[slot];
        } else {
            const auto resolved = layout->GetParameterElementIndex(group, write.Binding, write.Element);
            if (!resolved) return std::nullopt;
            index = *resolved;
        }
        if (index >= values.size() || !values[index].empty()) return std::nullopt;
        auto value = render::GetShaderParameterValueIdentity(write.Value);
        if (!value) return std::nullopt;
        values[index] = std::move(*value);
    }
    vector<uint64_t> flatValues;
    vector<uint32_t> valueOffsets;
    valueOffsets.reserve(values.size() + 1);
    valueOffsets.push_back(0);
    for (const auto& value : values) {
        flatValues.insert(flatValues.end(), value.begin(), value.end());
        valueOffsets.push_back(uint32_t(flatValues.size()));
    }
    vector<ResourceId> resources{dependencies.begin(), dependencies.end()};
    for (const auto id : resources)
        if (!id.IsValid() || id.Owner != _owner) return std::nullopt;
    std::sort(resources.begin(), resources.end(), [](const auto& a, const auto& b) {
        return a.Index < b.Index || (a.Index == b.Index && a.Generation < b.Generation);
    });
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    HashCode hash{_compatibilities[compatibility].Hash};
    for (size_t slot = 0; slot < values.size(); ++slot) {
        hash.Add(valueOffsets[slot + 1] - valueOffsets[slot]);
        for (uint32_t word = valueOffsets[slot]; word < valueOffsets[slot + 1]; ++word) hash.Add(flatValues[word]);
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
            if (entry.Compatibility == compatibility && entry.Values == flatValues && entry.ValueOffsets == valueOffsets && entry.Dependencies == resources) {
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
    entry->Compatibility = compatibility;
    entry->Values = std::move(flatValues);
    entry->ValueOffsets = std::move(valueOffsets);
    entry->Dependencies = std::move(resources);
    entry->Retiring = std::any_of(entry->Dependencies.begin(), entry->Dependencies.end(), [&](const auto id) { return _retiringResources.contains(id); });
    entry->Parameters = parameters.Release();
    entry->Hash = key;
    entry->LastUse = ++_clock;
    entry->Resources = compatible.ResourceDescriptors();
    entry->Samplers = compatible.SamplerDescriptors();
    const uint32_t index = _free.empty() ? uint32_t(_slots.size()) : _free.back();
    if (_free.empty())
        _slots.emplace_back();
    else
        _free.pop_back();
    _idleResources += entry->Resources;
    _idleSamplers += entry->Samplers;
    ++_idleGroups;
    for (const auto id : entry->Dependencies) ++_dependents[id];
    _slots[index].Value = std::move(entry);
    _lookup[key].push_back(index);
    return DescriptorBlockId{_owner, index, _slots[index].Generation};
}
bool BindingRegistry::Retain(DescriptorBlockId id) {
    const auto entry = Find(id);
    if (!entry || entry->Pins == UINT32_MAX) return false;
    if (!entry->Pins++) {
        _idleResources -= entry->Resources;
        _idleSamplers -= entry->Samplers;
        --_idleGroups;
    }
    return true;
}
bool BindingRegistry::Retain(DescriptorBlockId id, uint64_t stamp, bool& newly) {
    const auto entry = Find(id);
    if (!entry || entry->Pins == UINT32_MAX) return false;
    if (entry->LastStamp == stamp) {
        newly = false;
        return true;
    }
    entry->LastStamp = stamp;
    newly = true;
    if (!entry->Pins++) {
        _idleResources -= entry->Resources;
        _idleSamplers -= entry->Samplers;
        --_idleGroups;
    }
    return true;
}
bool BindingRegistry::Release(DescriptorBlockId id) {
    const auto entry = Find(id);
    if (!entry || !entry->Pins) return false;
    if (!--entry->Pins) {
        entry->LastUse = ++_clock;
        _idleResources += entry->Resources;
        _idleSamplers += entry->Samplers;
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
    _idleResources -= entry.Resources;
    _idleSamplers -= entry.Samplers;
    --_idleGroups;
    const auto found = _lookup.find(entry.Hash);
    std::erase(found->second, index);
    if (found->second.empty()) _lookup.erase(found);
    for (const auto id : entry.Dependencies) {
        const auto dependents = _dependents.find(id);
        if (dependents != _dependents.end() && --dependents->second == 0) _dependents.erase(dependents);
    }
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
    return _dependents.contains(id);
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
