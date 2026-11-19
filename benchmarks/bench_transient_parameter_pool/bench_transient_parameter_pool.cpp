#include <benchmark/benchmark.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <tuple>
#include <type_traits>
#include <variant>

#include <radray/gpu/external_resource.h>
#include <radray/gpu/types.h>
#include <radray/hash.h>
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
    void* Map(uint64_t, uint64_t) noexcept override { return nullptr; }
    void Unmap() noexcept override {}
    void FlushMappedRange(gpu::BufferRange) noexcept override {}
    void InvalidateMappedRange(gpu::BufferRange) noexcept override {}
    gpu::BufferDescriptor GetDesc() const noexcept override { return _desc; }
    gpu::Device* GetDevice() const noexcept override { return nullptr; }

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
    gpu::DeviceDetail GetDetail() const noexcept override { return _caps.Detail; }
    const gpu::GpuDeviceCapabilities& GetCapabilities() const noexcept override { return _caps; }
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

// 对照实现：与 TransientParameterSetPool 完全同构，仅在 BeginFrame() 采用全表清零（std::fill）。
class FullTableClearParameterPool final {
public:
    explicit FullTableClearParameterPool(gpu::Device& device) noexcept : _device(device) {}

    Nullable<gpu::ShaderParameterSet*> AcquireParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout,
                                                           std::span<const TransientParameterBinding> bindings) {
        if (!layout) return nullptr;
        _scratch.assign(bindings.begin(), bindings.end());
        const auto location = [](const TransientParameterBinding& b) noexcept {
            return std::tie(b.BindingId, b.ArrayElement);
        };
        bool sorted = true;
        for (size_t i = 1; i < _scratch.size(); ++i) {
            if (!(location(_scratch[i - 1]) < location(_scratch[i]))) {
                sorted = false;
                break;
            }
        }
        if (!sorted) {
            std::sort(_scratch.begin(), _scratch.end(), [&](const auto& a, const auto& b) noexcept {
                return location(a) < location(b);
            });
        }
        HashCode hashBuilder;
        for (size_t i = 0; i < _scratch.size(); ++i) {
            const auto& binding = _scratch[i];
            if (i != 0 && location(_scratch[i - 1]) == location(binding)) return nullptr;
            hashBuilder.Add(binding.BindingId);
            hashBuilder.Add(binding.ArrayElement);
            hashBuilder.Add(binding.Value.index());
            std::visit(
                [&](const auto& value) {
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

        if (const auto it = _groups.find(layout.get()); it != _groups.end()) {
            const auto& group = it->second;
            if (!group.Buckets.empty()) {
                const size_t mask = group.Buckets.size() - 1;
                for (size_t slot = hash & mask;; slot = (slot + 1) & mask) {
                    const uint32_t entryIdx = group.Buckets[slot];
                    if (entryIdx == kEmptyBucket) break;
                    const auto& entry = group.Entries[entryIdx];
                    if (entry.Hash == hash && entry.Count == _scratch.size() &&
                        (entry.Count == 0 || std::equal(_scratch.begin(), _scratch.end(), group.KeyBindings.data() + entry.Offset))) {
                        return entry.Set;
                    }
                }
            }
        }

        auto& group = _groups[layout.get()];
        if (!group.Layout) group.Layout = layout;
        if (group.Used == group.Sets.size()) {
            auto created = _device.CreateShaderParameterSet({layout.get()});
            if (!created) return nullptr;
            group.Sets.push_back(created.Release());
        }
        auto* set = group.Sets[group.Used++].get();
        const bool written = std::all_of(_scratch.begin(), _scratch.end(), [&](const auto& b) { return set->Set(b.BindingId, b.ArrayElement, b.Value); });
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
                group.Buckets[slot] = i;
            }
        }

        const uint32_t offset = static_cast<uint32_t>(group.KeyBindings.size());
        const uint32_t count = static_cast<uint32_t>(_scratch.size());
        group.KeyBindings.insert(group.KeyBindings.end(), _scratch.begin(), _scratch.end());
        const size_t mask = group.Buckets.size() - 1;
        size_t slot = hash & mask;
        while (group.Buckets[slot] != kEmptyBucket) slot = (slot + 1) & mask;
        const uint32_t entryIdx = static_cast<uint32_t>(group.Entries.size());
        group.Entries.push_back({offset, count, hash, set});
        group.Buckets[slot] = entryIdx;
        return set;
    }

    void BeginFrame() noexcept {
        _scratch.clear();
        for (auto it = _groups.begin(); it != _groups.end();) {
            auto& group = it->second;
            std::fill(group.Buckets.begin(), group.Buckets.end(), kEmptyBucket);
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
    struct Entry {
        uint32_t Offset{0};
        uint32_t Count{0};
        size_t Hash{0};
        gpu::ShaderParameterSet* Set{nullptr};
    };
    struct Group {
        shared_ptr<gpu::BindingGroupLayout> Layout;
        vector<unique_ptr<gpu::ShaderParameterSet>> Sets;
        vector<TransientParameterBinding> KeyBindings;
        vector<Entry> Entries;
        vector<uint32_t> Buckets;
        array<size_t, 8> Peaks{};
        size_t Used{0};
    };

    gpu::Device& _device;
    unordered_map<gpu::BindingGroupLayout*, Group> _groups;
    vector<TransientParameterBinding> _scratch;
    size_t _windowIndex{0};
};

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

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
