#include <radray/runtime/pipeline_layout_cache.h>

#include <algorithm>
#include <radray/hash.h>
#include <radray/logger.h>

namespace radray {
namespace {
void HashSampler(HashCode& hash, const render::SamplerDescriptor& s) {
    hash.Add(s.AddressS);
    hash.Add(s.AddressT);
    hash.Add(s.AddressR);
    hash.Add(s.MinFilter);
    hash.Add(s.MagFilter);
    hash.Add(s.MipmapFilter);
    hash.Add(s.LodMin);
    hash.Add(s.LodMax);
    hash.Add(s.Compare.has_value());
    if (s.Compare) hash.Add(*s.Compare);
    hash.Add(s.AnisotropyClamp);
    hash.Add(s.MipLodBias);
    hash.Add(s.BorderColor);
    hash.Add(s.Reduction);
    hash.Add(s.UnnormalizedCoordinates);
}
size_t HashBindings(std::span<const render::BindingDescriptor> bindings) {
    HashCode hash;
    hash.Add(bindings.size());
    for (const auto& b : bindings) {
        hash.Add(b.BindingId);
        hash.Add(b.Kind);
        hash.Add(b.Count);
        hash.Add(b.Stages.value());
        hash.Add(b.D3D12.has_value());
        hash.Add(b.Vulkan.has_value());
        if (b.D3D12) {
            hash.Add(b.D3D12->RegisterType);
            hash.Add(b.D3D12->Register);
            hash.Add(b.D3D12->Space);
            hash.Add(b.D3D12->Placement);
        }
        if (b.Vulkan) {
            hash.Add(b.Vulkan->Set);
            hash.Add(b.Vulkan->Binding);
            hash.Add(b.Vulkan->Placement);
        }
        hash.Add(b.ImmutableSampler.has_value());
        if (b.ImmutableSampler) HashSampler(hash, *b.ImmutableSampler);
    }
    return hash.ToHashCode();
}
bool SamePush(const render::PushConstantDescriptor& a, const render::PushConstantDescriptor& b) {
    return a.PushId == b.PushId && a.Size == b.Size && a.Stages == b.Stages && a.D3D12 == b.D3D12 && a.Vulkan == b.Vulkan;
}
}  // namespace

class PipelineLayoutCache::Impl {
public:
    struct GroupEntry {
        vector<render::BindingDescriptor> Key;
        unique_ptr<render::BindingGroupLayout> Object;
    };
    struct LayoutEntry {
        vector<render::PipelineLayoutGroupDescriptor> Groups;
        vector<render::PushConstantDescriptor> Pushes;
        unique_ptr<render::PipelineLayout> Object;
    };
    explicit Impl(render::Device& device) : Device(device), Backend(device.GetBackend()) {}
    render::Device& Device;
    render::RenderBackend Backend;
    unordered_map<size_t, vector<unique_ptr<GroupEntry>>> Groups;
    unordered_map<size_t, vector<unique_ptr<LayoutEntry>>> Layouts;
};

PipelineLayoutCache::PipelineLayoutCache(render::Device& device) : _impl(make_unique<Impl>(device)) {}
PipelineLayoutCache::~PipelineLayoutCache() noexcept = default;

Nullable<render::PipelineLayout*> PipelineLayoutCache::Acquire(const ShaderPipelineLayoutDesc& desc) {
    string diagnostic;
    if (!ValidateShaderPipelineLayoutDesc(desc, &diagnostic)) {
        RADRAY_ERR_LOG("Invalid cached layout: {}", diagnostic);
        return nullptr;
    }
    const auto backend = _impl->Backend;
    if (backend != render::RenderBackend::D3D12 && backend != render::RenderBackend::Vulkan) return nullptr;
    const bool dx = backend == render::RenderBackend::D3D12;
    auto candidate = make_unique<Impl::LayoutEntry>();
    for (const auto& group : desc.Groups) {
        vector<render::BindingDescriptor> key;
        for (const auto& entry : group.Bindings) {
            auto binding = entry.Binding;
            if (dx ? !binding.D3D12 : !binding.Vulkan) continue;
            if (dx)
                binding.Vulkan.reset();
            else
                binding.D3D12.reset();
            key.push_back(binding);
        }
        if (key.empty()) continue;
        std::sort(key.begin(), key.end(), [](const auto& a, const auto& b) { return a.BindingId < b.BindingId; });
        auto& bucket = _impl->Groups[HashBindings(key)];
        Nullable<render::BindingGroupLayout*> layout{nullptr};
        for (const auto& cached : bucket)
            if (cached->Key == key) {
                layout = cached->Object.get();
                break;
            }
        if (!layout) {
            auto created = _impl->Device.CreateBindingGroupLayout({key});
            if (!created) return nullptr;
            auto entry = make_unique<Impl::GroupEntry>();
            entry->Key = std::move(key);
            entry->Object = created.Release();
            layout = entry->Object.get();
            bucket.push_back(std::move(entry));
        }
        candidate->Groups.push_back({group.GroupSlot, layout.Get()});
    }
    for (const auto& entry : desc.PushConstants) {
        auto push = entry.Push;
        if (dx ? !push.D3D12 : !push.Vulkan) continue;
        if (dx)
            push.Vulkan.reset();
        else
            push.D3D12.reset();
        candidate->Pushes.push_back(push);
    }
    std::sort(candidate->Groups.begin(), candidate->Groups.end(), [](const auto& a, const auto& b) { return a.GroupSlot < b.GroupSlot; });
    std::sort(candidate->Pushes.begin(), candidate->Pushes.end(), [](const auto& a, const auto& b) { return a.PushId < b.PushId; });
    HashCode hash;
    hash.Add(candidate->Groups.size());
    for (const auto& group : candidate->Groups) {
        hash.Add(group.GroupSlot);
        hash.Add(group.Layout);
    }
    hash.Add(candidate->Pushes.size());
    for (const auto& push : candidate->Pushes) {
        hash.Add(push.PushId);
        hash.Add(push.Size);
        hash.Add(push.Stages.value());
        if (push.D3D12) {
            hash.Add(push.D3D12->Register);
            hash.Add(push.D3D12->Space);
        }
        if (push.Vulkan) hash.Add(push.Vulkan->Offset);
    }
    auto& bucket = _impl->Layouts[hash.ToHashCode()];
    for (const auto& entry : bucket)
        if (std::equal(entry->Groups.begin(), entry->Groups.end(), candidate->Groups.begin(), candidate->Groups.end(),
                       [](const auto& a, const auto& b) { return a.GroupSlot == b.GroupSlot && a.Layout == b.Layout; }) &&
            std::equal(entry->Pushes.begin(), entry->Pushes.end(), candidate->Pushes.begin(), candidate->Pushes.end(), SamePush)) return entry->Object.get();
    auto created = _impl->Device.CreatePipelineLayout({candidate->Groups, candidate->Pushes});
    if (!created) return nullptr;
    candidate->Object = created.Release();
    auto* result = candidate->Object.get();
    bucket.push_back(std::move(candidate));
    return result;
}

}  // namespace radray
