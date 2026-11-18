#include <radray/runtime/pipeline_layout_registry.h>

#include <algorithm>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/gpu/types.h>
#include <radray/runtime/gpu_pipeline_layout.h>
#include <radray/shader/shader_layout.h>

namespace radray {
namespace {
void HashSampler(HashCode& hash, const gpu::SamplerDescriptor& s) {
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
size_t HashBindings(std::span<const gpu::BindingDescriptor> bindings) {
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
bool SamePush(const gpu::PushConstantDescriptor& a, const gpu::PushConstantDescriptor& b) {
    return a.PushId == b.PushId && a.Size == b.Size && a.Stages == b.Stages && a.D3D12 == b.D3D12 && a.Vulkan == b.Vulkan;
}

template <class T>
class WeakLayoutIndex {
public:
    template <class Matches>
    shared_ptr<T> Find(size_t hash, Matches matches) {
        auto it = _buckets.find(hash);
        if (it == _buckets.end()) return {};
        shared_ptr<T> result;
        _size -= std::erase_if(it->second, [&](const weak_ptr<T>& ref) {
            auto owner = ref.lock();
            if (!owner) return true;
            if (!result && matches(*owner)) result = std::move(owner);
            return false;
        });
        if (it->second.empty()) _buckets.erase(it);
        return result;
    }

    void Insert(size_t hash, const shared_ptr<T>& owner) {
        _buckets[hash].emplace_back(owner);
        if (++_size < _sweepThreshold) return;
        for (auto it = _buckets.begin(); it != _buckets.end();) {
            _size -= std::erase_if(it->second, [](const weak_ptr<T>& ref) { return ref.expired(); });
            if (it->second.empty())
                it = _buckets.erase(it);
            else
                ++it;
        }
        _sweepThreshold = std::max<size_t>(64, 2 * _size);
    }

private:
    unordered_map<size_t, vector<weak_ptr<T>>> _buckets;
    size_t _size{0};
    size_t _sweepThreshold{64};
};
}  // namespace

class PipelineLayoutRegistry::Impl {
public:
    explicit Impl(gpu::Device& device) : Device(device), Backend(device.GetBackend()) {}
    gpu::Device& Device;
    gpu::RenderBackend Backend;
    struct GroupEntry {
        vector<gpu::BindingDescriptor> Key;
        unique_ptr<gpu::BindingGroupLayout> Object;
    };
    WeakLayoutIndex<GroupEntry> Groups;
    WeakLayoutIndex<GpuPipelineLayout> Layouts;
};

PipelineLayoutRegistry::PipelineLayoutRegistry(gpu::Device& device) : _impl(make_unique<Impl>(device)) {}
PipelineLayoutRegistry::~PipelineLayoutRegistry() noexcept = default;

Nullable<shared_ptr<GpuPipelineLayout>> PipelineLayoutRegistry::Acquire(const shader::ShaderPipelineLayoutDesc& desc) {
    string diagnostic;
    if (!shader::ValidateShaderPipelineLayoutDesc(desc, &diagnostic)) {
        RADRAY_ERR_LOG("Invalid pipeline layout: {}", diagnostic);
        return nullptr;
    }
    const auto backend = _impl->Backend;
    if (backend != gpu::RenderBackend::D3D12 && backend != gpu::RenderBackend::Vulkan) return nullptr;
    const bool dx = backend == gpu::RenderBackend::D3D12;
    GpuPipelineLayout::GroupOwners owners;
    vector<gpu::PipelineLayoutGroupDescriptor> groups;
    vector<gpu::PushConstantDescriptor> pushes;
    for (const auto& group : desc.Groups) {
        vector<gpu::BindingDescriptor> key;
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
        const size_t hash = HashBindings(key);
        auto entry = _impl->Groups.Find(hash, [&](const Impl::GroupEntry& cached) { return cached.Key == key; });
        if (!entry) {
            auto created = _impl->Device.CreateBindingGroupLayout({key});
            if (!created) return nullptr;
            entry = make_shared<Impl::GroupEntry>(std::move(key), created.Release());
            _impl->Groups.Insert(hash, entry);
        }
        auto* layout = entry->Object.get();
        groups.push_back({group.GroupSlot, layout});
        owners.emplace_back(group.GroupSlot, shared_ptr<gpu::BindingGroupLayout>{std::move(entry), layout});
    }
    for (const auto& entry : desc.PushConstants) {
        auto push = entry.Push;
        if (dx ? !push.D3D12 : !push.Vulkan) continue;
        if (dx)
            push.Vulkan.reset();
        else
            push.D3D12.reset();
        pushes.push_back(push);
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.GroupSlot < b.GroupSlot; });
    std::sort(pushes.begin(), pushes.end(), [](const auto& a, const auto& b) { return a.PushId < b.PushId; });
    HashCode hash;
    hash.Add(groups.size());
    for (const auto& group : groups) {
        hash.Add(group.GroupSlot);
        hash.Add(group.Layout);
    }
    hash.Add(pushes.size());
    for (const auto& push : pushes) {
        hash.Add(push.PushId);
        hash.Add(push.Size);
        hash.Add(push.Stages.value());
        if (push.D3D12) {
            hash.Add(push.D3D12->Register);
            hash.Add(push.D3D12->Space);
        }
        if (push.Vulkan) hash.Add(push.Vulkan->Offset);
    }
    const size_t layoutHash = hash.ToHashCode();
    auto cached = _impl->Layouts.Find(layoutHash, [&](const GpuPipelineLayout& entry) {
        const auto native = entry.GetLayout().GetDesc();
        return std::ranges::equal(native.Groups, groups, [](const auto& a, const auto& b) {
                   return a.GroupSlot == b.GroupSlot && a.Layout == b.Layout;
               }) && std::ranges::equal(native.PushConstants, pushes, SamePush);
    });
    if (cached) return cached;
    auto created = _impl->Device.CreatePipelineLayout({groups, pushes});
    if (!created) return nullptr;
    auto result = make_shared<GpuPipelineLayout>(GpuPipelineLayout::ConstructionKey{}, std::move(owners), created.Release());
    _impl->Layouts.Insert(layoutHash, result);
    return result;
}

}  // namespace radray
