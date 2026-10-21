#include "descriptor_probe.h"

#include <algorithm>
#include <fmt/format.h>
#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::example {
void DescriptorProbe::BeginFrame() { SemanticSlices.clear(); }
void DescriptorProbe::ObserveData(std::string_view semantic, render::ShaderBufferBinding binding) {
    auto& slices = SemanticSlices[string{semantic}];
    if (std::find(slices.begin(), slices.end(), binding) == slices.end()) slices.push_back(binding);
    uint64_t extra = 0;
    for (const auto& item : SemanticSlices) extra += item.second.size() - 1;
    PeakSemanticExtraSlices = std::max(PeakSemanticExtraSlices, extra);
}
void DescriptorProbe::Snapshot(JsonRef output) const {
    output.AddUint("sets_live", Total.LiveSets);
    output.AddUint("sets_peak", Total.PeakSets);
    output.AddUint("sets_created", Total.Created);
    output.AddUint("set_calls", Total.Set);
    output.AddUint("flush_calls", Total.Flush);
    output.AddUint("allocation_failures", Total.Failed);
    output.AddUint("resource_live", Total.Resource);
    output.AddUint("sampler_live", Total.Sampler);
    output.AddUint("resource_peak", Total.PeakResource);
    output.AddUint("sampler_peak", Total.PeakSampler);
    output.AddUint("resource_allocated", Total.AllocatedResource);
    output.AddUint("sampler_allocated", Total.AllocatedSampler);
    output.AddUint("resource_freed", Total.FreedResource);
    output.AddUint("sampler_freed", Total.FreedSampler);
    auto types = output.AddArray("vulkan_types_live");
    for (const auto& [type, count] : Total.Types) {
        auto item = types.AppendObject();
        item.AddUint("type", type);
        item.AddUint("count", count);
        item.AddUint("peak", Total.PeakTypes.at(type));
        item.AddUint("allocated", Total.AllocatedTypes.at(type));
        const auto freed = Total.FreedTypes.find(type);
        item.AddUint("freed", freed == Total.FreedTypes.end() ? 0 : freed->second);
    }
    unordered_set<string> keys;
    uint64_t duplicate = 0, resource = 0, sampler = 0;
    for (const auto* set : Live) {
        auto values = set->ValuesKey();
        if (values.empty()) continue;
        if (!keys.insert(set->Shape() + values).second) {
            ++duplicate;
            resource += set->ResourceCount();
            sampler += set->SamplerCount();
        }
    }
    output.AddUint("equivalent_extra_sets", duplicate);
    output.AddUint("equivalent_extra_resource", resource);
    output.AddUint("equivalent_extra_sampler", sampler);
    uint64_t slices = 0;
    for (const auto& item : SemanticSlices) slices += item.second.size() - 1;
    output.AddUint("semantic_extra_slices", slices);
    output.AddUint("semantic_extra_slices_peak", PeakSemanticExtraSlices);
}

DescriptorProbeSet::DescriptorProbeSet(unique_ptr<render::ShaderParameterSet> native, render::RenderBackend backend,
                                       const render::ShaderParameterSetDescriptor& desc, DescriptorProbe& probe)
    : _native(std::move(native)), _backend(backend), _layout(desc.Layout), _probe(probe) {
    vector<string> shape;
#if defined(RADRAY_ENABLE_D3D12)
    if (backend == render::RenderBackend::D3D12) {
        const auto* set = static_cast<render::d3d12::ShaderParameterSetD3D12*>(_native.get());
        const auto* group = set->_layout->FindParameterGroup(desc.GroupIndex).Get();
        for (size_t i = 0; i < set->_tables.size(); ++i) {
            const auto& table = group->Tables[i];
            const auto count = set->_tables[i].Descriptors.GetLength();
            (table.HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? _sampler : _resource) += count;
            auto key = fmt::format("table:{},{},{};", uint32_t(table.HeapType), uint32_t(table.Visibility), table.DescriptorCount);
            for (const auto& range : table.Ranges)
                key += fmt::format("range:{},{},{},{},{},{};", uint32_t(range.Type), range.Count, range.Binding, range.Space, range.Offset, uint32_t(range.Flags));
            for (const auto& slot : table.Slots)
                key += fmt::format("slot:{},{},{},{},{},{};", slot.Offset, slot.Namespace, slot.Binding, slot.ArrayElement, uint32_t(slot.LogicalKind), uint32_t(slot.Placement));
            shape.push_back(std::move(key));
        }
        for (const auto& entry : group->Entries)
            shape.push_back(fmt::format("entry:{},{},{},{},{};", entry.Namespace, entry.Binding, uint32_t(entry.LogicalKind), uint32_t(entry.Placement), entry.Count));
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (backend == render::RenderBackend::Vulkan) {
        const auto* set = static_cast<render::vulkan::ShaderParameterSetVulkan*>(_native.get());
        for (const auto& entry : set->_layout->_parameterSetLayouts[desc.GroupIndex]) {
            _types[uint32_t(entry.DescriptorType)] += entry.Count;
            (entry.DescriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ? _sampler : _resource) += entry.Count;
            shape.push_back(fmt::format("entry:{},{},{},{},{},{};", entry.Binding, uint32_t(entry.LogicalKind), uint32_t(entry.DescriptorType), entry.Count, entry.Stages.value(), entry.ImmutableSamplerIndex));
        }
    }
#endif
    std::sort(shape.begin(), shape.end());
    for (const auto& part : shape) _shape += part;
    auto& total = _probe.Total;
    ++total.Created;
    ++total.LiveSets;
    total.Resource += _resource;
    total.Sampler += _sampler;
    total.AllocatedResource += _resource;
    total.AllocatedSampler += _sampler;
    total.PeakSets = std::max(total.PeakSets, total.LiveSets);
    total.PeakResource = std::max(total.PeakResource, total.Resource);
    total.PeakSampler = std::max(total.PeakSampler, total.Sampler);
    for (const auto& [type, count] : _types) {
        total.Types[type] += count;
        total.AllocatedTypes[type] += count;
        total.PeakTypes[type] = std::max(total.PeakTypes[type], total.Types[type]);
    }
    _probe.Live.push_back(this);
}
DescriptorProbeSet::~DescriptorProbeSet() noexcept { Destroy(); }
void DescriptorProbeSet::Destroy() noexcept {
    if (!_native) return;
    _native.reset();
    auto& total = _probe.Total;
    --total.LiveSets;
    total.Resource -= _resource;
    total.Sampler -= _sampler;
    total.FreedResource += _resource;
    total.FreedSampler += _sampler;
    for (const auto& [type, count] : _types) {
        total.Types[type] -= count;
        total.FreedTypes[type] += count;
    }
    std::erase(_probe.Live, this);
}
bool DescriptorProbeSet::Set(render::BindingHandle binding, uint32_t element, render::ShaderParameterValue value) noexcept {
    ++_probe.Total.Set;
    if (!_native->Set(binding, element, value)) return false;
    std::span<const render::BackendBindingName> names;
#if defined(RADRAY_ENABLE_D3D12)
    if (_backend == render::RenderBackend::D3D12) names = static_cast<render::d3d12::RootSigD3D12*>(_layout)->_bindingNames;
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (_backend == render::RenderBackend::Vulkan) names = static_cast<render::vulkan::PipelineLayoutVulkan*>(_layout)->_bindingNames;
#endif
    const auto record = render::FindBackendBindingRecord(names, reinterpret_cast<uintptr_t>(_layout), binding);
    if (!record) return false;
    const auto slot = fmt::format("{},{},{}", record->Namespace, record->Location.Binding, element);
    const auto found = std::find_if(_values.begin(), _values.end(), [&](const auto& entry) { return entry.Slot == slot; });
    if (found == _values.end())
        _values.push_back({slot, value});
    else
        found->Data = value;
    return true;
}
bool DescriptorProbeSet::FlushWrites() noexcept {
    ++_probe.Total.Flush;
    _sealed = _native->FlushWrites();
    return _sealed;
}
void DescriptorProbeSet::ResetBindingsForReuse() noexcept {
    _native->ResetBindingsForReuse();
    _values.clear();
    _sealed = false;
}
string DescriptorProbeSet::ValuesKey() const {
    if (!_sealed) return {};
    vector<string> values;
    for (const auto& item : _values) {
        auto key = item.Slot + "=";
        if (const auto* b = std::get_if<render::ShaderBufferBinding>(&item.Data))
            key += fmt::format("buffer:{},{},{},{}", fmt::ptr(b->Target), b->Range.Offset, b->Range.Size, b->StructureByteStride);
        else if (const auto* t = std::get_if<render::TextureView*>(&item.Data))
            key += fmt::format("view:{}", fmt::ptr(*t));
        else if (const auto* s = std::get_if<render::Sampler*>(&item.Data))
            key += fmt::format("sampler:{}", fmt::ptr(*s));
        else
            return {};  // This frozen workload contains no texel buffer bindings.
        values.push_back(std::move(key));
    }
    std::sort(values.begin(), values.end());
    string result{"values:"};
    for (const auto& value : values) result += value + ";";
    return result;
}
Nullable<unique_ptr<render::ShaderParameterSet>> DescriptorProbeDevice::CreateShaderParameterSet(const render::ShaderParameterSetDescriptor& desc) noexcept {
    auto result = _native->CreateShaderParameterSet(desc);
    if (!result) {
        ++_probe.Total.Failed;
        return nullptr;
    }
    return make_unique<DescriptorProbeSet>(result.Release(), GetBackend(), desc, _probe);
}
void DescriptorProbeEncoder::BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets) noexcept {
    _native->BindShaderParameterSet(group, static_cast<DescriptorProbeSet*>(set)->Native(), offsets);
}
}  // namespace radray::example
