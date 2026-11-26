#include <benchmark/benchmark.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <radray/gpu/external_resource.h>
#include <radray/gpu/types.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/runtime/transient_parameter_pool.h>
#include <radray/types.h>

namespace radray {
namespace {

constexpr uint32_t kPeakKeys = 65536;
constexpr uint32_t kEmptyBucket = std::numeric_limits<uint32_t>::max();

class BenchBuffer final : public gpu::Buffer {
public:
    explicit BenchBuffer(uint64_t size) noexcept : _desc{.Size = size, .Usage = gpu::BufferUse::CBuffer} {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::BufferMemoryProperties GetMemoryProperties() const noexcept override { return {.IsDeviceLocal = true}; }
    Nullable<void*> GetMappedData() noexcept override { return nullptr; }
    bool FlushMappedRange(gpu::BufferRange) noexcept override { return false; }
    bool InvalidateMappedRange(gpu::BufferRange) noexcept override { return false; }
    gpu::BufferDescriptor GetDesc() const noexcept override { return _desc; }

private:
    gpu::BufferDescriptor _desc;
};

class BenchLayout final : public gpu::BindingGroupLayout {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::BindingGroupLayoutDescriptor GetDesc() const noexcept override { return {}; }
};

class BenchParameterSet final : public gpu::ShaderParameterSet {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Set(uint32_t, uint32_t, gpu::ShaderParameterValue) noexcept override { return true; }
    bool FlushWrites() noexcept override { return true; }
    void ResetBindingsForReuse() noexcept override { ++Resets; }

    uint64_t Resets{0};
};

class BenchDevice final : public gpu::Device {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool IsDeviceLost() const noexcept override { return false; }
    gpu::GpuBackend GetBackend() noexcept override { return gpu::GpuBackend::D3D12; }
    Nullable<unique_ptr<gpu::ExternalResourceExtension>> CreateExternalResourceExtension() noexcept override { return nullptr; }
    const gpu::GpuDeviceCapabilities& GetCapabilities() const noexcept override { return _caps; }
    gpu::GpuMemoryBudget QueryMemoryBudget() const noexcept override { return {}; }
    gpu::TextureSupport QueryTextureSupport(const gpu::TextureSupportQuery&) const noexcept override { return {}; }
    gpu::AttachmentResolveSupport QueryAttachmentResolveSupport(gpu::TextureFormat, uint32_t) const noexcept override { return {}; }
    Nullable<gpu::CommandQueue*> GetCommandQueue(gpu::QueueType, uint32_t) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::CommandAllocator>> CreateCommandAllocator(gpu::CommandQueue*) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::CommandBuffer>> CreateCommandBuffer(gpu::CommandAllocator*) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::Fence>> CreateFence() noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::QueryPool>> CreateQueryPool(const gpu::QueryPoolDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::SwapChain>> CreateSwapChain(const gpu::SwapChainDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::Buffer>> CreateBuffer(const gpu::BufferDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::Texture>> CreateTexture(const gpu::TextureDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::TextureView>> CreateTextureView(const gpu::TextureViewDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::Shader>> CreateShader(const gpu::ShaderDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::BindingGroupLayout>> CreateBindingGroupLayout(const gpu::BindingGroupLayoutDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::PipelineLayout>> CreatePipelineLayout(const gpu::PipelineLayoutDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::ShaderParameterSet>> CreateShaderParameterSet(const gpu::ShaderParameterSetDescriptor&) noexcept override {
        ++SetCreates;
        return make_unique<BenchParameterSet>();
    }
    Nullable<unique_ptr<gpu::GraphicsPipelineState>> CreateGraphicsPipelineState(const gpu::GraphicsPipelineStateDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::ComputePipelineState>> CreateComputePipelineState(const gpu::ComputePipelineStateDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<gpu::Sampler>> CreateSampler(const gpu::SamplerDescriptor&) noexcept override { return nullptr; }
    Nullable<gpu::Sampler*> GetOrCreateSampler(const gpu::SamplerDescriptor&) noexcept override { return nullptr; }

    uint64_t SetCreates{0};

private:
    gpu::GpuDeviceCapabilities _caps{};
};

enum class AcquirePath { Baseline,
                         Sorted,
                         Mru };

// Benchmark-only alternatives share allocation, collision handling and frame reuse.
template <AcquirePath Path, bool FullClear = false>
class ReferenceParameterPool final {
    struct Entry {
        uint32_t Offset{0};
        uint32_t Count{0};
        size_t Hash{0};
        size_t Bucket{0};
        gpu::ShaderParameterSet* Set{nullptr};
    };
    struct NoMru {};
    struct Mru {
        uint32_t LastEntryIndex{kEmptyBucket};
    };
    struct Group : std::conditional_t<Path == AcquirePath::Mru, Mru, NoMru> {
        shared_ptr<gpu::BindingGroupLayout> Layout;
        vector<unique_ptr<gpu::ShaderParameterSet>> Sets;
        vector<TransientParameterBinding> KeyBindings;
        vector<Entry> Entries;
        vector<uint32_t> Buckets;
        array<size_t, 8> Peaks{};
        size_t Used{0};
    };

public:
    explicit ReferenceParameterPool(gpu::Device& device) noexcept : _device(device) {}

    Nullable<gpu::ShaderParameterSet*> AcquireParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout,
                                                           std::span<const TransientParameterBinding> bindings) {
        if (!layout) return nullptr;
        auto activeBindings = bindings;
        const auto location = [](const TransientParameterBinding& b) { return std::tie(b.BindingId, b.ArrayElement); };
        const auto less = [&](const auto& a, const auto& b) { return location(a) < location(b); };
        if constexpr (Path == AcquirePath::Baseline) {
            _scratch.assign(bindings.begin(), bindings.end());
            for (size_t i = 1; i < _scratch.size(); ++i) {
                if (!less(_scratch[i - 1], _scratch[i])) {
                    std::sort(_scratch.begin(), _scratch.end(), less);
                    break;
                }
            }
            activeBindings = _scratch;
        } else {
            for (size_t i = 1; i < bindings.size(); ++i) {
                if (less(bindings[i - 1], bindings[i])) continue;
                if (location(bindings[i - 1]) == location(bindings[i])) return nullptr;
                _scratch.assign(bindings.begin(), bindings.end());
                std::sort(_scratch.begin(), _scratch.end(), less);
                if (std::adjacent_find(_scratch.begin(), _scratch.end(), [&](const auto& a, const auto& b) { return location(a) == location(b); }) != _scratch.end()) return nullptr;
                activeBindings = _scratch;
                break;
            }
        }
        const auto matches = [&](const Group& group, const Entry& entry) {
            return entry.Count == activeBindings.size() &&
                   (entry.Count == 0 || std::equal(activeBindings.begin(), activeBindings.end(), group.KeyBindings.data() + entry.Offset));
        };
        Nullable<Group*> current{nullptr};
        if constexpr (Path != AcquirePath::Baseline) {
            current = &GetGroup(layout);
            if constexpr (Path == AcquirePath::Mru) {
                if (current->LastEntryIndex != kEmptyBucket) {
                    const auto& last = current->Entries[current->LastEntryIndex];
                    if (matches(*current, last)) return last.Set;
                }
            }
        }
        HashCode hashBuilder;
        for (size_t i = 0; i < activeBindings.size(); ++i) {
            const auto& binding = activeBindings[i];
            if constexpr (Path == AcquirePath::Baseline) {
                if (i && location(activeBindings[i - 1]) == location(binding)) return nullptr;
            }
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
                    if constexpr (std::is_same_v<T, gpu::ShaderBufferBinding>)
                        hashBuilder.Add(value.StructureByteStride);
                    else
                        hashBuilder.Add(static_cast<uint32_t>(value.Format));
                }
            },
                       binding.Value);
        }
        const size_t hash = hashBuilder.ToHashCode();
        if constexpr (Path == AcquirePath::Baseline) {
            if (const auto it = _groups.find(layout.get()); it != _groups.end()) current = &it->second;
        }
        if (current && !current->Buckets.empty()) {
            const size_t mask = current->Buckets.size() - 1;
            for (size_t slot = hash & mask;; slot = (slot + 1) & mask) {
                const uint32_t idx = current->Buckets[slot];
                if (idx == kEmptyBucket) break;
                const auto& entry = current->Entries[idx];
                if (entry.Hash == hash && matches(*current, entry)) {
                    if constexpr (Path == AcquirePath::Mru) current->LastEntryIndex = idx;
                    return entry.Set;
                }
            }
        }
        if constexpr (Path == AcquirePath::Baseline) current = &GetGroup(layout);
        const auto set = AcquireSet(*current);
        if (!set) return nullptr;
        if constexpr (Path == AcquirePath::Baseline) current = &_groups.at(layout.get());
        auto& group = *current;
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
        if constexpr (Path == AcquirePath::Mru) group.LastEntryIndex = entryIdx;
        return set;
    }

    void BeginFrame() noexcept {
        _scratch.clear();
        for (auto it = _groups.begin(); it != _groups.end();) {
            auto& group = it->second;
            if constexpr (FullClear) {
                std::fill(group.Buckets.begin(), group.Buckets.end(), kEmptyBucket);
            } else {
                for (const auto& entry : group.Entries) group.Buckets[entry.Bucket] = kEmptyBucket;
            }
            if constexpr (Path == AcquirePath::Mru) group.LastEntryIndex = kEmptyBucket;
            group.Entries.clear();
            group.KeyBindings.clear();
            for (size_t i = 0; i < group.Used; ++i) group.Sets[i]->ResetBindingsForReuse();
            group.Peaks[_windowIndex % group.Peaks.size()] = group.Used;
            group.Used = 0;
            const auto peak = *std::max_element(group.Peaks.begin(), group.Peaks.end());
            group.Sets.resize(peak);
            if (!peak)
                it = _groups.erase(it);
            else
                ++it;
        }
        ++_windowIndex;
    }

private:
    Group& GetGroup(const shared_ptr<gpu::BindingGroupLayout>& layout) {
        auto& group = _groups[layout.get()];
        if (!group.Layout) group.Layout = layout;
        return group;
    }

    Nullable<gpu::ShaderParameterSet*> AcquireSet(Group& group) {
        if (group.Used == group.Sets.size()) {
            auto created = _device.CreateShaderParameterSet({group.Layout.get()});
            if (!created) return nullptr;
            group.Sets.push_back(created.Release());
        }
        return group.Sets[group.Used++].get();
    }

    gpu::Device& _device;
    unordered_map<gpu::BindingGroupLayout*, Group> _groups;
    vector<TransientParameterBinding> _scratch;
    size_t _windowIndex{0};
};

using FullTableClearParameterPool = ReferenceParameterPool<AcquirePath::Sorted, true>;

vector<TransientParameterBinding> BuildUniqueKeys(gpu::Buffer& buffer, uint32_t count) {
    vector<TransientParameterBinding> keys;
    keys.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        keys.push_back({0, 0, gpu::ShaderBufferBinding{&buffer, {uint64_t{i} * 256, 256}, 0}});
    }
    return keys;
}

template <class Pool>
void RunSparseAfterPeak(benchmark::State& state, bool includeSingleAcquire) {
    BenchDevice device;
    BenchBuffer buffer{uint64_t{kPeakKeys} * 256};
    auto layout = make_shared<BenchLayout>();
    const auto keys = BuildUniqueKeys(buffer, kPeakKeys);
    Pool pool{device};

    // 1. 峰值帧：写入 65,536 个唯一键（使桶表扩容至 131,072 槽）。
    for (uint32_t i = 0; i < kPeakKeys; ++i) {
        if (!pool.AcquireParameterSet(layout, std::span{&keys[i], 1})) {
            state.SkipWithError("warmup spike acquire failed");
            return;
        }
    }
    pool.BeginFrame();

    // 2. 连续 9 个稀疏帧（每帧 1 个键），使 65,536 峰值退出 8 帧滑动窗口（Sets 收缩至 1，Buckets 保持 131,072）。
    for (uint32_t f = 0; f < 9; ++f) {
        if (!pool.AcquireParameterSet(layout, std::span{&keys[0], 1})) {
            state.SkipWithError("window drain acquire failed");
            return;
        }
        pool.BeginFrame();
    }

    const uint64_t createsBefore = device.SetCreates;
    constexpr uint32_t kBatch = 256;

    for (auto _ : state) {
        if (includeSingleAcquire) {
            const auto start = std::chrono::steady_clock::now();
            for (uint32_t i = 0; i < kBatch; ++i) {
                auto set = pool.AcquireParameterSet(layout, std::span{&keys[0], 1});
                benchmark::DoNotOptimize(set);
                pool.BeginFrame();
            }
            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count() / static_cast<double>(kBatch);
            state.SetIterationTime(seconds);
        } else {
            double accumulated = 0.0;
            for (uint32_t i = 0; i < kBatch; ++i) {
                if (!pool.AcquireParameterSet(layout, std::span{&keys[0], 1})) {
                    state.SkipWithError("sparse acquire failed");
                    return;
                }
                const auto start = std::chrono::steady_clock::now();
                pool.BeginFrame();
                const auto end = std::chrono::steady_clock::now();
                accumulated += std::chrono::duration<double>(end - start).count();
            }
            state.SetIterationTime(accumulated / static_cast<double>(kBatch));
        }
    }

    if (device.SetCreates != createsBefore) {
        state.SkipWithError("unexpected ShaderParameterSet creation during steady sparse frames");
        return;
    }
    state.counters["peak_keys"] = static_cast<double>(kPeakKeys);
    state.counters["steady_keys"] = 1.0;
    state.counters["bucket_slots"] = static_cast<double>(kPeakKeys * 2);
}

template <class Pool>
void RunSustainedHighLoad(benchmark::State& state) {
    BenchDevice device;
    BenchBuffer buffer{uint64_t{kPeakKeys} * 256};
    auto layout = make_shared<BenchLayout>();
    const auto keys = BuildUniqueKeys(buffer, kPeakKeys);
    Pool pool{device};

    // 预热参数集池与桶表容量，使计时循环内无首次创建与扩容。
    for (uint32_t i = 0; i < kPeakKeys; ++i) {
        if (!pool.AcquireParameterSet(layout, std::span{&keys[i], 1})) {
            state.SkipWithError("sustained warmup acquire failed");
            return;
        }
    }
    pool.BeginFrame();
    const uint64_t createsBefore = device.SetCreates;

    for (auto _ : state) {
        for (uint32_t i = 0; i < kPeakKeys; ++i) {
            if (!pool.AcquireParameterSet(layout, std::span{&keys[i], 1})) {
                state.SkipWithError("sustained acquire failed");
                return;
            }
        }
        const auto start = std::chrono::steady_clock::now();
        pool.BeginFrame();
        const auto end = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
    }

    if (device.SetCreates != createsBefore) {
        state.SkipWithError("unexpected ShaderParameterSet creation during sustained high load");
        return;
    }
    state.counters["steady_keys"] = static_cast<double>(kPeakKeys);
    state.counters["bucket_slots"] = static_cast<double>(kPeakKeys * 2);
}

enum class KeySequence { Repeat,
                         Alternate,
                         Insert };

template <class Pool>
void RunAcquires(benchmark::State& state, uint32_t bindingCount, bool reverse, KeySequence sequence) {
    constexpr size_t kBatch = 1000;
    const size_t keyCount = sequence == KeySequence::Insert ? kBatch : (sequence == KeySequence::Alternate ? 2 : 1);
    BenchDevice device;
    BenchBuffer buffer{(kBatch + 1) * 256};
    auto layout = make_shared<BenchLayout>();
    vector<TransientParameterBinding> keys;
    keys.reserve(keyCount * bindingCount);
    for (size_t k = 0; k < keyCount; ++k) {
        for (uint32_t i = 0; i < bindingCount; ++i) {
            const auto binding = reverse ? bindingCount - 1 - i : i;
            const uint64_t offset = binding + 1 == bindingCount ? k * 256 : 0;
            keys.push_back({binding, 0, gpu::ShaderBufferBinding{&buffer, {offset, 256}, 0}});
        }
    }
    const auto key = [&](size_t k) { return std::span<const TransientParameterBinding>{keys}.subspan(k * bindingCount, bindingCount); };
    Pool pool{device};
    for (size_t i = 0; i < keyCount; ++i) {
        if (!pool.AcquireParameterSet(layout, key(i))) {
            state.SkipWithError("key warmup failed");
            return;
        }
    }
    if (device.SetCreates != keyCount) {
        state.SkipWithError("distinct keys did not produce distinct sets");
        return;
    }
    for (auto _ : state) {
        if (sequence == KeySequence::Insert) pool.BeginFrame();
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < kBatch; ++i) {
            const size_t k = sequence == KeySequence::Insert ? i : (sequence == KeySequence::Alternate ? i % 2 : 0);
            const auto set = pool.AcquireParameterSet(layout, key(k));
            if (!set) {
                state.SkipWithError("timed acquire failed");
                return;
            }
            auto* result = set.Get();
            benchmark::DoNotOptimize(result);
        }
        state.SetIterationTime(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    if (device.SetCreates != keyCount) state.SkipWithError("unexpected creation during timed acquisitions");
    state.SetItemsProcessed(state.iterations() * kBatch);
}

template <class Pool>
void RegisterAcquires(std::string_view version) {
    for (uint32_t count : {1u, 4u, 8u}) {
        for (bool reverse : {false, true}) {
            if (count == 1 && reverse) continue;
            for (const auto& [sequence, name] : {std::pair{KeySequence::Repeat, "Repeat"}, {KeySequence::Alternate, "Alternate"}, {KeySequence::Insert, "Insert"}}) {
                const auto label = radray::format("ParameterAcquire/{}/{}/{}/{}", version, name, count, reverse ? "Unsorted" : "Sorted");
                benchmark::RegisterBenchmark(label.c_str(), [=](benchmark::State& state) { RunAcquires<Pool>(state, count, reverse, sequence); })->UseManualTime();
            }
        }
    }
}

}  // namespace
}  // namespace radray

int main(int argc, char** argv) {
    using namespace radray;
    benchmark::RegisterBenchmark("TransientParameterPool/SparseAfterPeak65536/BeginFrame/PerEntryClear",
                                 [](benchmark::State& s) { RunSparseAfterPeak<TransientParameterSetPool>(s, false); })
        ->UseManualTime()
        ->Unit(benchmark::kNanosecond);
    benchmark::RegisterBenchmark("TransientParameterPool/SparseAfterPeak65536/BeginFrame/FullTableClear",
                                 [](benchmark::State& s) { RunSparseAfterPeak<FullTableClearParameterPool>(s, false); })
        ->UseManualTime()
        ->Unit(benchmark::kNanosecond);
    benchmark::RegisterBenchmark("TransientParameterPool/SparseAfterPeak65536/Acquire1AndBeginFrame/PerEntryClear",
                                 [](benchmark::State& s) { RunSparseAfterPeak<TransientParameterSetPool>(s, true); })
        ->UseManualTime()
        ->Unit(benchmark::kNanosecond);
    benchmark::RegisterBenchmark("TransientParameterPool/SparseAfterPeak65536/Acquire1AndBeginFrame/FullTableClear",
                                 [](benchmark::State& s) { RunSparseAfterPeak<FullTableClearParameterPool>(s, true); })
        ->UseManualTime()
        ->Unit(benchmark::kNanosecond);
    benchmark::RegisterBenchmark("TransientParameterPool/SustainedHighLoad65536/BeginFrame/PerEntryClear",
                                 [](benchmark::State& s) { RunSustainedHighLoad<TransientParameterSetPool>(s); })
        ->UseManualTime()
        ->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark("TransientParameterPool/SustainedHighLoad65536/BeginFrame/FullTableClear",
                                 [](benchmark::State& s) { RunSustainedHighLoad<FullTableClearParameterPool>(s); })
        ->UseManualTime()
        ->Unit(benchmark::kMicrosecond);

    RegisterAcquires<ReferenceParameterPool<AcquirePath::Baseline>>("Baseline");
    RegisterAcquires<ReferenceParameterPool<AcquirePath::Sorted>>("R3a");
    RegisterAcquires<ReferenceParameterPool<AcquirePath::Mru>>("R3b");
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
