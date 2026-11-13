#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstring>

#include <radray/runtime/rdg.h>

#include "../../modules/runtime/tests/test_fakes.h"

namespace {
using namespace radray;
using namespace radray::render;

struct Probe {
    uint64_t Copies{0}, CopyBytes{0}, Barriers{0}, Creates{0}, Maps{0}, Flushes{0};
};

class MemoryProbe final : public std::pmr::memory_resource {
public:
    explicit MemoryProbe(std::pmr::memory_resource& upstream) noexcept : _upstream(upstream) {}
    size_t Live{0}, Peak{0}, Allocations{0};

private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        auto* result = _upstream.allocate(bytes, alignment);
        Live += AccountedSize(bytes, alignment);
        Peak = std::max(Peak, Live);
        ++Allocations;
        return result;
    }
    void do_deallocate(void* data, size_t bytes, size_t alignment) override {
        Live -= AccountedSize(bytes, alignment);
        _upstream.deallocate(data, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    size_t AccountedSize(size_t bytes, size_t alignment) const noexcept {
        alignment = std::max(alignment, alignof(std::max_align_t));
        return (std::max(bytes, size_t{1}) + alignment - 1) & ~(alignment - 1);
    }
    std::pmr::memory_resource& _upstream;
};

class Command final : public test::FakeCommandBuffer {
public:
    explicit Command(Probe& probe) noexcept : _probe(probe) {}
    void CopyBufferToBuffer(Buffer*, uint64_t, Buffer*, uint64_t, uint64_t size) noexcept override {
        ++_probe.Copies;
        _probe.CopyBytes += size;
    }
    void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept override { _probe.Barriers += barriers.size(); }

private:
    Probe& _probe;
};

class Device final : public test::FakeDevice {
public:
    explicit Device(Probe& probe) noexcept : _probe(probe) {
        EnableHostBuffers = true;
        OnCreateBuffer = [&probe](const BufferDescriptor&) { ++probe.Creates; };
        OnMap = [&probe] { ++probe.Maps; };
        OnFlush = [&probe](BufferRange) { ++probe.Flushes; };
    }
    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandAllocator*) noexcept override { return make_unique<Command>(_probe); }

private:
    Probe& _probe;
};

enum class Stage { Build,
                   Compile,
                   Prepare,
                   Record,
                   Frame,
                   Recycle };
enum class Source { Borrow,
                    Snapshot,
                    Generate,
                    Dynamic };
struct Workload {
    uint32_t Count;
    uint64_t BlockSize;
    uint32_t AlivePercent{100};
    Source Kind{Source::Borrow};
    uint32_t Pattern{0};
    bool Fresh{false};
    uint32_t VisibleCount{1};
};

struct Generator {
    uint64_t Size;
    MeshPassId Pass{std::numeric_limits<uint32_t>::max()};
};
uint64_t Layout(Generator& data, RDGUploadPrepareContext& context) noexcept {
    return data.Pass.Value == std::numeric_limits<uint32_t>::max() ? data.Size : context.GetPassDrawList(data.Pass).VisibleCommands.size() * data.Size;
}
bool Write(const Generator&, RDGUploadWriteContext& context) noexcept {
    std::memset(context.Bytes.data(), 0x35, context.Bytes.size());
    return true;
}

void Run(benchmark::State& state, Stage stage, Workload load, bool memoryProbe, bool& failed) {
    Probe probe;
    auto device = make_shared<Device>(probe);
    DeviceContext context{device, &device->Queue};
    RDGTransientResourcePool pool{*device};
    UploadArena uploads{*device, 64 * 1024};
    HeapMemoryResource heap;
    MemoryProbe memory{heap};
    auto scheduler = FrameScheduler::Create(context, make_unique<test::CallbackPipeline>(), 1, memoryProbe ? Nullable<std::pmr::memory_resource*>{&memory} : nullptr).Unwrap();
    vector<byte> bytes(static_cast<size_t>(load.BlockSize), byte{0x35});
    uint64_t measuredCopies = 0, measuredBytes = 0, measuredBarriers = 0, measuredCreates = 0, measuredMaps = 0;
    size_t measuredAllocations = 0;
    const auto execute = [&](bool measured) {
        if (measured && stage == Stage::Recycle) state.ResumeTiming();
        auto& slot = scheduler->BeginFrame();
        if (measured && stage == Stage::Recycle) state.PauseTiming();
        const size_t allocationsBefore = memory.Allocations;
        if (load.Kind == Source::Dynamic) slot.GetPassDrawList({0}).VisibleCommands.resize(load.VisibleCount);
        pool.BeginFrame();
        uploads.Reset();
        if (load.Fresh) {
            pool.Clear();
            uploads.Clear();
        }
        const auto before = probe;
        const auto start = [&](Stage value) { if (measured && stage == value) state.ResumeTiming(); };
        const auto stop = [&](Stage value) {
            if (measured && stage == value) state.PauseTiming();
        };
        start(Stage::Frame);
        start(Stage::Build);
        RDGBuilder graph{slot, context.GetRDGConstraints(), {.FullValidation = true}};
        RDGBufferHandle shared;
        if (load.Pattern) shared = graph.CreateBuffer(BufferDescriptor{.Size = load.BlockSize * load.Count,
                                                                       .Usage = BufferUse::CopyDestination | BufferUse::CopySource});
        for (uint32_t i = 0; i < load.Count; ++i) {
            RDGUploadSource source;
            if (load.Kind == Source::Borrow)
                source = RDGUploadSource::BorrowedBytes(bytes);
            else if (load.Kind == Source::Snapshot)
                source = RDGUploadSource::SnapshotBytes(bytes);
            else
                source = RDGUploadSource::Generated(load.Kind == Source::Dynamic ? load.BlockSize * 16 : load.BlockSize,
                                                    Generator{load.BlockSize, {load.Kind == Source::Dynamic ? 0u : std::numeric_limits<uint32_t>::max()}}, &Write, load.Kind == Source::Dynamic ? &Layout : nullptr);
            RDGBufferHandle target;
            if (load.Kind == Source::Dynamic) {
                target = graph.CreateBuffer(RDGDynamicBufferDescriptor{load.BlockSize * 16, BufferUse::CopySource}, std::move(source));
            } else {
                target = load.Pattern ? shared : graph.CreateBuffer(BufferDescriptor{.Size = load.BlockSize, .Usage = BufferUse::CopyDestination});
                graph.UploadBuffer(target, load.Pattern == 1 ? i * load.BlockSize : 0, std::move(source));
            }
            if (i * 100 / load.Count < load.AlivePercent) graph.MarkOutput(target);
            if (load.Pattern == 3 && i + 1 < load.Count) {
                graph.AddCopyPass("consumer", uint32_t{0}, +[](const uint32_t&, RDGCopyPassContext&) noexcept { return true; }).ReadBuffer(target).NeverCull();
            }
        }
        stop(Stage::Build);
        start(Stage::Compile);
        const auto compiled = graph.Compile();
        stop(Stage::Compile);
        if (!compiled) return false;
        start(Stage::Prepare);
        const bool prepared = compiled->Prepare(slot, pool, uploads);
        stop(Stage::Prepare);
        if (!prepared) return false;
        auto command = slot.AcquireCommandBuffer();
        if (!command) return false;
        start(Stage::Record);
        command->Begin();
        const bool recorded = compiled->Record(*command);
        command->End();
        stop(Stage::Record);
        if (!recorded) return false;
        compiled->CommitSubmittedStates();
        stop(Stage::Frame);
        scheduler->RunFrame();
        if (measured) {
            measuredCopies += probe.Copies - before.Copies;
            measuredBytes += probe.CopyBytes - before.CopyBytes;
            measuredBarriers += probe.Barriers - before.Barriers;
            measuredCreates += probe.Creates - before.Creates;
            measuredMaps += probe.Maps - before.Maps;
            measuredAllocations += memory.Allocations - allocationsBefore;
        }
        return true;
    };
    // 预热不包含在任意阶段计时中；Fresh 用例每轮显式释放页。
    if (!execute(false)) {
        state.SkipWithError("warmup failed");
        failed = true;
        return;
    }
    for (auto _ : state) {
        state.PauseTiming();
        if (!execute(true)) {
            state.ResumeTiming();
            state.SkipWithError("RDG phase failed");
            failed = true;
            break;
        }
        state.ResumeTiming();
    }
    scheduler->BeginFrame();
    scheduler->WaitIdle();
    const auto iterations = std::max<int64_t>(1, state.iterations());
    state.counters["logical_uploads"] = load.Count;
    state.counters["physical_copies"] = double(measuredCopies) / iterations;
    state.counters["copy_bytes"] = double(measuredBytes) / iterations;
    state.counters["barrier_descriptors"] = double(measuredBarriers) / iterations;
    state.counters["resource_creates"] = double(measuredCreates) / iterations;
    state.counters["maps"] = double(measuredMaps) / iterations;
    state.counters["retained_upload_capacity"] = double(uploads.GetCapacity());
    if (memoryProbe) {
        state.SetLabel("memory probe; timings include instrumentation");
        state.counters["peak_storage_bytes"] = double(memory.Peak);
        state.counters["retained_storage_bytes"] = double(memory.Live);
        state.counters["upstream_allocations"] = double(measuredAllocations) / iterations;
    }
    scheduler.reset();
    if (memory.Live != 0) {
        state.SkipWithError("slot storage outlived scheduler");
        failed = true;
    }
}

// 独立组件测量使用与图相同的来源和最终映射页；不把这些时间相加冒充完整 Prepare。
void RunPrepareComponent(benchmark::State& state, uint32_t component, bool& failed) {
    Probe probe;
    auto device = make_shared<Device>(probe);
    DeviceContext deviceContext{device, &device->Queue};
    auto scheduler = FrameScheduler::Create(deviceContext, make_unique<test::CallbackPipeline>(), 1).Unwrap();
    auto& slot = scheduler->BeginFrame();
    RDGUploadPrepareContext context{slot, {}};
    UploadArena uploads{*device, 64 * 1024};
    vector<RDGUploadSource> sources;
    for (uint32_t i = 0; i < 64; ++i) sources.push_back(RDGUploadSource::Generated(4096, Generator{80}, &Write, &Layout));
    const auto fill = [&]() {
        uploads.Reset();
        for (auto& source : sources) {
            const auto allocation = uploads.Allocate(80, 256, 256, BufferState::CBuffer);
            if (!allocation) return false;
            RDGUploadWriteContext write{{allocation->Data.Get(), 80}, 80};
            if (!source.Write(write)) return false;
        }
        return true;
    };
    if (!fill()) {
        state.SkipWithError("allocation failed");
        failed = true;
        return;
    }
    for (auto _ : state) {
        if (component == 0) {
            uint64_t total = 0;
            for (auto& source : sources) total += source.PrepareLayout(context);
            benchmark::DoNotOptimize(total);
        } else if (component == 1) {
            if (!fill()) {
                state.SkipWithError("fill failed");
                failed = true;
                break;
            }
            benchmark::ClobberMemory();
        } else if (!uploads.Flush()) {
            state.SkipWithError("flush failed");
            failed = true;
            break;
        }
    }
    state.counters["logical_uploads"] = 64;
    state.counters["effective_bytes"] = 64 * 80;
    state.counters["retained_upload_capacity"] = double(uploads.GetCapacity());
}

}  // namespace

int main(int argc, char** argv) {
    bool memoryProbe = false;
    int arguments = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--rdg_memory_probe")
            memoryProbe = true;
        else
            argv[arguments++] = argv[i];
    }
    argc = arguments;
    argv[argc] = nullptr;
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    bool failed = false;
    const vector<std::pair<Stage, std::string_view>> stages{std::pair{Stage::Build, "Build"}, {Stage::Compile, "Compile"}, {Stage::Prepare, "Prepare"}, {Stage::Record, "Record"}, {Stage::Frame, "Frame"}, {Stage::Recycle, "Recycle"}};
    const vector<std::pair<Workload, std::string_view>> workloads{
        std::pair{Workload{1, 1024 * 1024}, "LargeBorrow"},
        {Workload{1, 1024 * 1024, 100, Source::Snapshot}, "LargeSnapshot"},
        {Workload{64, 1024}, "Chunks64"},
        {Workload{1024, 64}, "Chunks1024"},
        {Workload{4096, 16}, "Chunks4096"},
        {Workload{1024, 64, 10}, "Alive10"},
        {Workload{1024, 64, 0}, "Alive0"},
        {Workload{64, 80, 100, Source::Generate}, "Generate80"},
        {Workload{64, 80, 100, Source::Dynamic}, "Dynamic80"},
        {Workload{64, 80, 100, Source::Dynamic, 0, false, 8}, "DynamicVisible8"},
        {Workload{64, 1024, 100, Source::Borrow, 1}, "Disjoint"},
        {Workload{64, 1024, 100, Source::Borrow, 2}, "Overlap"},
        {Workload{64, 1024, 100, Source::Borrow, 3}, "Consumers"},
        {Workload{64, 1024, 100, Source::Borrow, 0, true}, "Fresh"}};
    for (const auto& [stage, stageName] : stages) {
        if (memoryProbe && stage != Stage::Frame) continue;
        for (const auto& [load, name] : workloads) {
            const auto label = radray::format("RDGUpload/{}/{}", name, stageName);
            benchmark::RegisterBenchmark(label.c_str(), [&, stage, load](benchmark::State& state) { Run(state, stage, load, memoryProbe, failed); })->UseRealTime();
        }
    }
    const array<std::string_view, 3> components{"PrepareLayout", "AllocateFill", "Flush"};
    for (uint32_t i = 0; i < components.size(); ++i) {
        if (memoryProbe) break;
        const auto label = radray::format("RDGUploadComponent/{}", components[i]);
        benchmark::RegisterBenchmark(label.c_str(), [&, i](benchmark::State& state) { RunPrepareComponent(state, i, failed); })->UseRealTime();
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return failed ? 1 : 0;
}
