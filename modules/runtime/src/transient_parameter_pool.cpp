#include <radray/runtime/transient_parameter_pool.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <tuple>
#include <type_traits>
#include <variant>

#include <radray/hash.h>

namespace radray {
namespace {

constexpr uint32_t kEmptyBucket = std::numeric_limits<uint32_t>::max();

}  // namespace

TransientParameterSetPool::TransientParameterSetPool(gpu::Device& device) noexcept : _device(device) {}

Nullable<gpu::ShaderParameterSet*> TransientParameterSetPool::AcquireUninitializedParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout) {
    if (!layout) return nullptr;
    auto& group = _groups[layout.get()];
    if (!group.Layout) group.Layout = layout;
    return AcquireUninitialized(group);
}

Nullable<gpu::ShaderParameterSet*> TransientParameterSetPool::AcquireUninitialized(Group& group) {
    if (group.Used == group.Sets.size()) {
        auto set = _device.CreateShaderParameterSet({group.Layout.get()});
        if (!set) return nullptr;
        group.Sets.push_back(set.Release());
    }
    return group.Sets[group.Used++].get();
}

Nullable<gpu::ShaderParameterSet*> TransientParameterSetPool::AcquireParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout,
                                                                                 std::span<const TransientParameterBinding> bindings) {
    if (!layout) return nullptr;
    const auto location = [](const TransientParameterBinding& b) noexcept {
        return std::tie(b.BindingId, b.ArrayElement);
    };
    auto activeBindings = bindings;
    for (size_t i = 1; i < bindings.size(); ++i) {
        if (location(bindings[i - 1]) < location(bindings[i])) continue;
        if (location(bindings[i - 1]) == location(bindings[i])) return nullptr;
        _scratch.assign(bindings.begin(), bindings.end());
        std::sort(_scratch.begin(), _scratch.end(), [&](const auto& a, const auto& b) { return location(a) < location(b); });
        if (std::adjacent_find(_scratch.begin(), _scratch.end(), [&](const auto& a, const auto& b) { return location(a) == location(b); }) != _scratch.end()) return nullptr;
        activeBindings = _scratch;
        break;
    }
    auto& group = _groups[layout.get()];
    if (!group.Layout) group.Layout = layout;

    HashCode hashBuilder;
    for (const auto& binding : activeBindings) {
        hashBuilder.Add(binding.BindingId);
        hashBuilder.Add(binding.ArrayElement);
        hashBuilder.Add(binding.Value.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_pointer_v<T>) {
                hashBuilder.Add(value);
            } else {
                hashBuilder.Add(value.Target);
                hashBuilder.Add(value.Range.Offset);
                hashBuilder.Add(value.Range.Size);
                if constexpr (std::is_same_v<T, gpu::ShaderBufferBinding>) hashBuilder.Add(value.StructureByteStride);
                else hashBuilder.Add(static_cast<uint32_t>(value.Format));
            }
        }, binding.Value);
    }
    const size_t hash = hashBuilder.ToHashCode();

    if (!group.Buckets.empty()) {
        const size_t mask = group.Buckets.size() - 1;
        for (size_t slot = hash & mask;; slot = (slot + 1) & mask) {
            const uint32_t entryIdx = group.Buckets[slot];
            if (entryIdx == kEmptyBucket) break;
            const auto& entry = group.Entries[entryIdx];
            if (entry.Hash == hash && entry.Count == activeBindings.size() &&
                (entry.Count == 0 || std::equal(activeBindings.begin(), activeBindings.end(), group.KeyBindings.data() + entry.Offset))) {
                return entry.Set;
            }
        }
    }

    auto set = AcquireUninitialized(group);
    if (!set) return nullptr;
    const bool written = std::all_of(activeBindings.begin(), activeBindings.end(), [&](const auto& b) { return set->Set(b.BindingId, b.ArrayElement, b.Value); });
    if (!written || !set->FlushWrites()) {
        set->ResetBindingsForReuse();
        --group.Used;
        return nullptr;
    }

    const size_t nextEntries = group.Entries.size() + 1;
    if (group.Buckets.empty() || nextEntries * 2 > group.Buckets.size()) {
        const size_t newBucketCount = std::max<size_t>(16, std::bit_ceil(nextEntries * 2));
        group.Buckets.assign(newBucketCount, kEmptyBucket);
        const size_t mask = group.Buckets.size() - 1;
        for (uint32_t i = 0; i < static_cast<uint32_t>(group.Entries.size()); ++i) {
            size_t slot = group.Entries[i].Hash & mask;
            while (group.Buckets[slot] != kEmptyBucket) slot = (slot + 1) & mask;
            group.Entries[i].Bucket = slot;
            group.Buckets[slot] = i;
        }
    }

    const uint32_t offset = static_cast<uint32_t>(group.KeyBindings.size());
    const uint32_t count = static_cast<uint32_t>(activeBindings.size());
    group.KeyBindings.insert(group.KeyBindings.end(), activeBindings.begin(), activeBindings.end());
    const size_t mask = group.Buckets.size() - 1;
    size_t slot = hash & mask;
    while (group.Buckets[slot] != kEmptyBucket) slot = (slot + 1) & mask;
    const uint32_t entryIdx = static_cast<uint32_t>(group.Entries.size());
    group.Entries.push_back({offset, count, hash, slot, set.Get()});
    group.Buckets[slot] = entryIdx;
    return set;
}

void TransientParameterSetPool::BeginFrame() noexcept {
    _scratch.clear();
    for (auto it = _groups.begin(); it != _groups.end();) {
        auto& group = it->second;
        for (const auto& entry : group.Entries) {
            group.Buckets[entry.Bucket] = kEmptyBucket;
        }
        group.Entries.clear();
        group.KeyBindings.clear();
        for (size_t i = 0; i < group.Used; ++i) group.Sets[i]->ResetBindingsForReuse();
        group.Peaks[_windowIndex % group.Peaks.size()] = group.Used;
        group.Used = 0;
        const auto peak = *std::max_element(group.Peaks.begin(), group.Peaks.end());
        group.Sets.resize(peak);
        if (!peak) it = _groups.erase(it);
        else ++it;
    }
    ++_windowIndex;
}

std::optional<UploadAllocation> AllocateDynamicUniform(DynamicBufferArena& arena, std::span<const byte> bytes) {
    const auto& caps = arena.GetDevice().GetCapabilities();
    if (bytes.empty() || (caps.Limits.MaxUniformBufferRange && bytes.size() > caps.Limits.MaxUniformBufferRange)) return std::nullopt;
    const auto alignment = std::max<uint64_t>(1, caps.Limits.CBufferOffsetAlignment);
    const auto padding = (alignment - bytes.size() % alignment) % alignment;
    if (padding > std::numeric_limits<uint64_t>::max() - bytes.size()) return std::nullopt;
    auto allocation = arena.Allocate(bytes.size(), alignment, bytes.size() + padding, gpu::BufferState::CBuffer);
    if (allocation) std::memcpy(allocation->Data.Get(), bytes.data(), bytes.size());
    return allocation;
}

}  // namespace radray
