#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/scene.h>

#include "../../modules/runtime/tests/test_fakes.h"

namespace {
using namespace radray;
using namespace radray::gpu;

struct Probe {
    uint64_t Copies{0}, CopyBytes{0}, Barriers{0}, Creates{0}, Maps{0}, Flushes{0};
};

class CountingGraphicsEncoder final : public gpu::GraphicsCommandEncoder {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    gpu::CommandBuffer* GetCommandBuffer() const noexcept override { return nullptr; }
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    void SetStencilReference(uint8_t value) noexcept override { StencilSum += value; }
    void BindGraphicsPipelineState(gpu::GraphicsPipelineState*) noexcept override { ++PipelineBinds; }
    void BindVertexBuffers(std::span<const gpu::VertexBufferBinding> values) noexcept override { VertexBinds += values.size(); }
    void BindIndexBuffer(gpu::IndexBufferView) noexcept override { ++IndexBinds; }
    void BindShaderParameterSet(uint32_t, gpu::ShaderParameterSet*, std::span<const gpu::ShaderParameterDynamicOffset>) noexcept override { ++ParameterBinds; }
    bool SetPushConstants(uint32_t, uint32_t, std::span<const byte>) noexcept override {
        ++PushBinds;
        return true;
    }
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t, int32_t, uint32_t) noexcept override {
        ++IndexedDraws;
        DrawnIndices += uint64_t{indexCount} * instanceCount;
    }
    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t, uint32_t) noexcept override {
        ++NonIndexedDraws;
        DrawnIndices += uint64_t{vertexCount} * instanceCount;
    }
    void DrawIndirect(gpu::Buffer*, uint64_t, uint32_t count) noexcept override { IndirectDraws += count; }
    void DrawIndexedIndirect(gpu::Buffer*, uint64_t, uint32_t count) noexcept override { IndirectDraws += count; }

    uint64_t PipelineBinds{0};
    uint64_t VertexBinds{0};
    uint64_t IndexBinds{0};
    uint64_t ParameterBinds{0};
    uint64_t PushBinds{0};
    uint64_t StencilSum{0};
    uint64_t IndexedDraws{0};
    uint64_t NonIndexedDraws{0};
    uint64_t IndirectDraws{0};
    uint64_t DrawnIndices{0};
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
    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassBeginDescriptor&) noexcept override {
        return make_unique<CountingGraphicsEncoder>();
    }

private:
    Probe& _probe;
};

class Device final : public test::FakeDevice {
public:
    explicit Device(Probe& probe) noexcept : _probe(probe) {
        EnableHostBuffers = true;
        EnableTextures = true;
        Capabilities.Limits.MaxTexture2DDimension = 4096;
        Capabilities.Limits.MaxTextureArrayLayers = 256;
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
    StagingBufferPool staging{*device, 64 * 1024};
    DynamicBufferArena uploads{*device, 64 * 1024};
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
        staging.BeginFrame();
        uploads.BeginFrame();
        if (load.Fresh) {
            pool.Clear();
            staging.Clear();
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
        if (load.Pattern) shared = graph.CreateBuffer(RDGTransientBufferDescriptor{.Size = load.BlockSize * load.Count,
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
                target = graph.CreateDynamicBuffer(RDGDynamicBufferDescriptor{load.BlockSize * 16, BufferUse::CopySource}, std::move(source));
            } else {
                target = load.Pattern ? shared : graph.CreateBuffer(RDGTransientBufferDescriptor{.Size = load.BlockSize, .Usage = BufferUse::CopyDestination});
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
        const bool prepared = compiled->Prepare(slot, pool, staging, uploads);
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
    state.counters["retained_upload_capacity"] = double(uploads.GetCapacity() + staging.GetCapacity());
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
    DynamicBufferArena uploads{*device, 64 * 1024};
    vector<RDGUploadSource> sources;
    for (uint32_t i = 0; i < 64; ++i) sources.push_back(RDGUploadSource::Generated(4096, Generator{80}, &Write, &Layout));
    const auto fill = [&]() {
        uploads.BeginFrame();
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

// C1: SortVisibleCommands 基准（0 / 1 / 3 / 4 趟差异键，20,000 条可见命令）。
void RunSortVisible(benchmark::State& state, uint32_t activeBytes, bool& failed) {
    constexpr uint32_t kCount = 20000;
    vector<VisibleMeshDrawCommand> input;
    input.reserve(kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        uint64_t key = 0;
        if (activeBytes == 0) {
            key = (uint64_t{i >> 3} << 16) | uint64_t{i & 7u};
        } else if (activeBytes == 1) {
            const uint64_t b0 = (uint64_t{i} * 73u + 19u) & 0xFFu;
            key = 0x1122334455667700ULL | b0;
        } else if (activeBytes == 3) {
            const uint64_t b0 = (uint64_t{i} * 73u + 19u) & 0xFFu;
            const uint64_t b2 = (uint64_t{i} * 131u + 7u) & 0xFFu;
            const uint64_t b4 = (uint64_t{i} * 29u + 3u) & 0xFFu;
            key = 0x1122330055007700ULL | b0 | (b2 << 16) | (b4 << 32);
        } else {
            const uint64_t b0 = (uint64_t{i} * 73u + 19u) & 0xFFu;
            const uint64_t b1 = (uint64_t{i} * 53u + 11u) & 0xFFu;
            const uint64_t b4 = (uint64_t{i} * 29u + 3u) & 0xFFu;
            const uint64_t b5 = (uint64_t{i} * 17u + 5u) & 0xFFu;
            key = 0x1122000055660000ULL | b0 | (b1 << 8) | (b4 << 32) | (b5 << 40);
        }
        input.push_back({.SortKey = key, .FirstInstance = i, .InstanceCount = 1});
    }
    MeshPassFrameDrawList list;
    list.VisibleCommands.reserve(kCount);
    list.VisibleCommands.assign(input.begin(), input.end());
    list.SortVisibleCommands();
    for (size_t i = 1; i < list.VisibleCommands.size(); ++i) {
        if (list.VisibleCommands[i - 1].SortKey > list.VisibleCommands[i].SortKey) {
            state.SkipWithError("sort order check failed");
            failed = true;
            return;
        }
    }
    for (auto _ : state) {
        if (activeBytes != 0) {
            state.PauseTiming();
            list.VisibleCommands.assign(input.begin(), input.end());
            state.ResumeTiming();
        }
        list.SortVisibleCommands();
        benchmark::DoNotOptimize(list.VisibleCommands.data());
        benchmark::ClobberMemory();
    }
    state.counters["commands"] = kCount;
    state.counters["active_bytes"] = activeBytes;
}

// C2: MeshDrawCommand 展开（Count + Build）与直接提交基准（20,000 条单元素或 7 元素命令）。
void RunMeshDrawHotpath(benchmark::State& state, uint32_t elementCount, bool submitMode, bool& failed) {
    constexpr uint32_t kCommandCount = 20000;
    array<test::FakePipeline, 4> pipelines;
    array<test::FakeBuffer, 8> vertexBuffers{
        test::FakeBuffer{4096, BufferUse::Vertex}, test::FakeBuffer{4096, BufferUse::Vertex},
        test::FakeBuffer{4096, BufferUse::Vertex}, test::FakeBuffer{4096, BufferUse::Vertex},
        test::FakeBuffer{4096, BufferUse::Vertex}, test::FakeBuffer{4096, BufferUse::Vertex},
        test::FakeBuffer{4096, BufferUse::Vertex}, test::FakeBuffer{4096, BufferUse::Vertex}};
    array<test::FakeBuffer, 8> indexBuffers{
        test::FakeBuffer{4096, BufferUse::Index}, test::FakeBuffer{4096, BufferUse::Index},
        test::FakeBuffer{4096, BufferUse::Index}, test::FakeBuffer{4096, BufferUse::Index},
        test::FakeBuffer{4096, BufferUse::Index}, test::FakeBuffer{4096, BufferUse::Index},
        test::FakeBuffer{4096, BufferUse::Index}, test::FakeBuffer{4096, BufferUse::Index}};

    MeshDrawCommandBindingStorage storage;
    vector<MeshDrawCommand> commands;
    commands.reserve(kCommandCount);
    vector<VisibleMeshDrawCommand> visible;
    visible.reserve(kCommandCount);
    for (uint32_t i = 0; i < kCommandCount; ++i) {
        const uint32_t cluster = i / 64u;
        MeshDrawCommand cmd;
        cmd.Pipeline = &pipelines[cluster % pipelines.size()];
        cmd.IndexBuffer = {&indexBuffers[cluster % indexBuffers.size()], 0, 4};
        cmd.IndexCount = 3;
        cmd.FirstIndex = (i % 64u) * 3u;
        cmd.ValidElementMask = (uint32_t{1} << elementCount) - 1u;
        MeshDrawCommandBindings bindings;
        bindings.VertexBuffers.push_back({0, {&vertexBuffers[cluster % vertexBuffers.size()], 0, 4096}});
        bindings.AllElementsSharePrimaryIndexBuffer = true;
        for (uint32_t e = 1; e < elementCount; ++e) {
            bindings.ExtraElements.push_back({cmd.IndexBuffer, 3u, (e * 64u + (i % 64u)) * 3u, 0, e});
        }
        storage.Store(cmd, bindings);
        commands.push_back(cmd);
    }
    for (uint32_t i = 0; i < kCommandCount; ++i) {
        visible.push_back({.SortKey = i / 64u, .CachedCommand = &commands[i], .FirstInstance = i, .InstanceCount = 1});
    }

    PreparedMeshPassIndirectDraws prepared;
    MeshPassIndirectDrawCounts counts{};
    if (!CountMeshPassIndirectDraws(visible, counts) || !BuildMeshPassIndirectDraws(visible, prepared)) {
        state.SkipWithError("mesh draw warmup failed");
        failed = true;
        return;
    }
    if (counts.IndexedArgumentCount != kCommandCount * elementCount) {
        state.SkipWithError("unexpected expanded draw count");
        failed = true;
        return;
    }

    CountingGraphicsEncoder encoder;
    if (submitMode && !SubmitMeshDrawCommands(encoder, visible)) {
        state.SkipWithError("submit warmup failed");
        failed = true;
        return;
    }

    for (auto _ : state) {
        if (submitMode) {
            if (!SubmitMeshDrawCommands(encoder, visible)) {
                state.SkipWithError("submit failed");
                failed = true;
                break;
            }
            benchmark::DoNotOptimize(encoder.IndexedDraws);
            benchmark::DoNotOptimize(encoder.IndexBinds);
        } else {
            MeshPassIndirectDrawCounts iterCounts{};
            if (!CountMeshPassIndirectDraws(visible, iterCounts) || !BuildMeshPassIndirectDraws(visible, prepared)) {
                state.SkipWithError("indirect count/build failed");
                failed = true;
                break;
            }
            benchmark::DoNotOptimize(iterCounts.IndexedArgumentCount);
            benchmark::DoNotOptimize(prepared.IndexedArguments.data());
        }
    }
    state.counters["commands"] = kCommandCount;
    state.counters["elements_per_command"] = elementCount;
    state.counters["expanded_draws"] = kCommandCount * elementCount;
}

void RunIndirectBuild(benchmark::State& state, bool alternatingPages, bool& failed) {
    constexpr uint32_t kCommands = 20000;
    test::FakePipeline pipeline;
    test::FakeBuffer indexPage{4096, BufferUse::Index};
    array<test::FakeBuffer, 2> vertexPages{test::FakeBuffer{4096, BufferUse::Vertex}, test::FakeBuffer{4096, BufferUse::Vertex}};
    MeshDrawCommandBindingStorage storage;
    vector<MeshDrawCommand> commands(kCommands);
    vector<VisibleMeshDrawCommand> visible;
    visible.reserve(kCommands);
    for (uint32_t i = 0; i < kCommands; ++i) {
        auto& command = commands[i];
        command.Pipeline = &pipeline;
        command.IndexBuffer = {&indexPage, 0, 4};
        command.FirstIndex = (i % 64) * 3;
        command.IndexCount = 3;
        MeshDrawCommandBindings bindings;
        bindings.VertexBuffers.push_back({0, {&vertexPages[alternatingPages ? i % 2 : 0], 0, 4096}});
        storage.Store(command, bindings);
        visible.push_back({.CachedCommand = &command, .FirstInstance = i});
    }
    PreparedMeshPassIndirectDraws prepared;
    if (!BuildMeshPassIndirectDraws(visible, prepared) || prepared.IndexedArguments.size() != kCommands ||
        prepared.Batches.size() != (alternatingPages ? kCommands : 1u)) {
        failed = true;
        state.SkipWithError("unexpected indirect batch partition");
        return;
    }
    for (auto _ : state) {
        if (!BuildMeshPassIndirectDraws(visible, prepared)) {
            failed = true;
            state.SkipWithError("indirect build failed");
            break;
        }
        benchmark::DoNotOptimize(prepared.Batches.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * kCommands);
    state.counters["batches"] = static_cast<double>(prepared.Batches.size());
}

// C3: 100 页几何池 GetFreeBytes 查询与分配/释放循环基准。
void RunGeometryPoolHotpath(benchmark::State& state, bool allocFreeCycle, bool& failed) {
    constexpr uint32_t kPages = 100;
    constexpr uint32_t kPageCap = 64;
    Probe probe;
    Device device{probe};
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3}};
    auto pool = VertexBufferPool::Create(device, {.VertexStride = 12, .PageVertexCapacity = kPageCap, .Layouts = layouts, .Attributes = attributes});
    if (!pool) {
        state.SkipWithError("pool creation failed");
        failed = true;
        return;
    }
    vector<VertexBufferSubAllocation> held;
    held.reserve(kPages);
    for (uint32_t i = 0; i < kPages; ++i) {
        auto full = pool->Allocate(kPageCap - 16);
        auto tail = pool->Allocate(16);
        if (!full || !tail) {
            state.SkipWithError("pool fill failed");
            failed = true;
            return;
        }
        held.push_back(std::move(*full));
        // 先填满所有 100 页，随后释放 tail 保持每页 16 顶点空闲。
        if (i + 1 < kPages) {
            held.push_back(std::move(*tail));
        }
    }
    // 释放每页的 16 顶点尾部，制造 100 页各有空闲区间的布局。
    vector<VertexBufferSubAllocation> kept;
    kept.reserve(kPages);
    for (auto& alloc : held) {
        if (alloc.GetVertexCount() == kPageCap - 16) kept.push_back(std::move(alloc));
    }
    held.clear();
    if (pool->GetPageCount() != kPages) {
        state.SkipWithError("unexpected page count");
        failed = true;
        return;
    }

    for (auto _ : state) {
        if (!allocFreeCycle) {
            uint64_t sum = 0;
            for (uint32_t q = 0; q < 1024; ++q) {
                sum += pool->GetFreeBytes();
            }
            benchmark::DoNotOptimize(sum);
        } else {
            uint64_t sum = 0;
            for (uint32_t i = 0; i < 100; ++i) {
                auto alloc = pool->Allocate(8);
                if (!alloc) {
                    state.SkipWithError("cycle allocation failed");
                    failed = true;
                    break;
                }
                sum += alloc->GetByteOffset();
                alloc.reset();
            }
            sum += pool->GetFreeBytes();
            benchmark::DoNotOptimize(sum);
        }
    }
    state.counters["pages"] = pool->GetPageCount();
    state.counters["free_bytes"] = double(pool->GetFreeBytes());
}

// C4: 密集单 Pass（100 页几何池 + 32 纹理 + 200 次特征声明与视图解析）基准。
void RunRdgDensePass(benchmark::State& state, Stage stage, bool memoryProbe, bool& failed) {
    constexpr uint32_t kPages = 100;
    constexpr uint32_t kTextures = 32;
    constexpr uint32_t kFeatureReads = 200;
    Probe probe;
    auto device = make_shared<Device>(probe);
    DeviceContext context{device, &device->Queue};
    RDGTransientResourcePool pool{*device};
    StagingBufferPool staging{*device, 64 * 1024};
    DynamicBufferArena uploads{*device, 64 * 1024};
    HeapMemoryResource heap;
    MemoryProbe memory{heap};
    auto scheduler = FrameScheduler::Create(context, make_unique<test::CallbackPipeline>(), 1, memoryProbe ? Nullable<std::pmr::memory_resource*>{&memory} : nullptr).Unwrap();

    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3}};
    auto vbPool = VertexBufferPool::Create(*device, {.VertexStride = 12, .PageVertexCapacity = 16, .Layouts = layouts, .Attributes = attributes});
    if (!vbPool) {
        state.SkipWithError("vbPool create failed");
        failed = true;
        return;
    }
    vector<VertexBufferSubAllocation> heldPages;
    heldPages.reserve(kPages);
    for (uint32_t i = 0; i < kPages; ++i) {
        auto alloc = vbPool->Allocate(16);
        if (!alloc) {
            state.SkipWithError("vbPool fill failed");
            failed = true;
            return;
        }
        heldPages.push_back(std::move(*alloc));
    }
    vector<IntrusivePtr<GpuTexture>> textures;
    textures.reserve(kTextures);
    for (uint32_t i = 0; i < kTextures; ++i) {
        auto tex = GpuTexture::Create(*device, {.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .ArrayLayers = 1, .MipLevels = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::Resource | TextureUse::CopyDestination});
        if (!tex) {
            state.SkipWithError("texture create failed");
            failed = true;
            return;
        }
        tex->CommitSubmittedState(TextureState::ShaderRead);
        textures.push_back(tex.Release());
    }
    auto colorTex = device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();

    size_t measuredAllocations = 0;
    const auto execute = [&](bool measured) {
        auto& slot = scheduler->BeginFrame();
        const size_t allocBefore = memory.Allocations;
        pool.BeginFrame();
        staging.BeginFrame();
        uploads.BeginFrame();
        const auto start = [&](Stage value) {
            if (measured && stage == value) state.ResumeTiming();
        };
        const auto stop = [&](Stage value) {
            if (measured && stage == value) state.PauseTiming();
        };
        start(Stage::Frame);
        start(Stage::Build);
        RDGBuilder graph{slot, context.GetRDGConstraints(), {.FullValidation = true}};
        const auto color = graph.ImportTexture(colorTex.get(), TextureState::Undefined, TextureState::Common);
        struct PassData {
            array<RDGTextureHandle, kTextures> TexHandles{};
            uint32_t ResolvedCount{0};
        };
        PassData passData{};
        array<RDGBufferHandle, kPages> pageHandles{};
        for (uint32_t i = 0; i < kPages; ++i) {
            pageHandles[i] = graph.ImportPersistentOwned(heldPages[i].GetGpuBuffer());
        }
        for (uint32_t i = 0; i < kTextures; ++i) {
            passData.TexHandles[i] = graph.ImportPersistentOwned(textures[i]);
        }
        auto pass = graph.AddRasterPass(
            "dense_main", passData,
            +[](const PassData&, RDGRasterPassContext&) noexcept { return true; },
            +[](PassData& data, RDGPrepareContext& ctx) noexcept {
                uint32_t ok = 0;
                for (uint32_t i = 0; i < kFeatureReads; ++i) {
                    if (ctx.ResolveView(data.TexHandles[i % kTextures], TextureViewUsage::Resource)) ++ok;
                }
                data.ResolvedCount = ok;
                return ok == kFeatureReads;
            });
        pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        vbPool->AddPassReadAccess(graph, pass, BufferState::Vertex);
        for (uint32_t i = 0; i < kTextures; ++i) {
            pass.ReadTexture(passData.TexHandles[i], TextureState::ShaderRead);
        }
        for (uint32_t i = 0; i < kFeatureReads; ++i) {
            pass.ReadBuffer(pageHandles[i % kPages], BufferState::Vertex);
            pass.ReadTexture(passData.TexHandles[i % kTextures], TextureState::ShaderRead);
        }
        graph.MarkOutput(color);
        stop(Stage::Build);
        const auto compiled = graph.Compile();
        if (!compiled) return false;
        start(Stage::Prepare);
        const bool prepared = compiled->Prepare(slot, pool, staging, uploads);
        stop(Stage::Prepare);
        if (!prepared) return false;
        auto command = slot.AcquireCommandBuffer();
        if (!command) return false;
        command->Begin();
        const bool recorded = compiled->Record(*command);
        command->End();
        if (!recorded) return false;
        compiled->CommitSubmittedStates();
        stop(Stage::Frame);
        scheduler->RunFrame();
        if (measured) measuredAllocations += memory.Allocations - allocBefore;
        return true;
    };

    if (!execute(false)) {
        state.SkipWithError("dense pass warmup failed");
        failed = true;
        return;
    }
    for (auto _ : state) {
        state.PauseTiming();
        if (!execute(true)) {
            state.ResumeTiming();
            state.SkipWithError("dense pass failed");
            failed = true;
            break;
        }
        state.ResumeTiming();
    }
    scheduler->BeginFrame();
    scheduler->WaitIdle();
    const auto iterations = std::max<int64_t>(1, state.iterations());
    state.counters["unique_accesses"] = kPages + kTextures + 1;
    state.counters["feature_reads"] = kFeatureReads;
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

// C4: 1,024 个稀疏用户 CopyPass 各读一个高编号 Buffer[4095] 基准（含 --rdg_memory_probe 内存指标）。
void RunRdgSparseCopy1024(benchmark::State& state, Stage stage, bool memoryProbe, bool& failed) {
    constexpr uint32_t kPaddingBuffers = 4095;
    constexpr uint32_t kCopyPasses = 1024;
    Probe probe;
    auto device = make_shared<Device>(probe);
    DeviceContext context{device, &device->Queue};
    RDGTransientResourcePool pool{*device};
    StagingBufferPool staging{*device, 64 * 1024};
    DynamicBufferArena uploads{*device, 64 * 1024};
    HeapMemoryResource heap;
    MemoryProbe memory{heap};
    auto scheduler = FrameScheduler::Create(context, make_unique<test::CallbackPipeline>(), 1, memoryProbe ? Nullable<std::pmr::memory_resource*>{&memory} : nullptr).Unwrap();

    test::FakeBuffer highBuffer{64, BufferUse::CopySource};
    size_t measuredAllocations = 0;
    const auto execute = [&](bool measured) {
        auto& slot = scheduler->BeginFrame();
        const size_t allocBefore = memory.Allocations;
        pool.BeginFrame();
        staging.BeginFrame();
        uploads.BeginFrame();
        const auto start = [&](Stage value) {
            if (measured && stage == value) state.ResumeTiming();
        };
        const auto stop = [&](Stage value) {
            if (measured && stage == value) state.PauseTiming();
        };
        start(Stage::Frame);
        start(Stage::Build);
        RDGBuilder graph{slot, context.GetRDGConstraints(), {.FullValidation = true}};
        for (uint32_t i = 0; i < kPaddingBuffers; ++i) {
            (void)graph.CreateBuffer({.Size = 64, .Usage = BufferUse::CopySource});
        }
        const auto highHandle = graph.ImportBuffer(&highBuffer, BufferState::CopySource);
        for (uint32_t i = 0; i < kCopyPasses; ++i) {
            graph.AddCopyPass("sparse_copy", uint32_t{0}, +[](const uint32_t&, RDGCopyPassContext&) noexcept { return true; })
                .ReadBuffer(highHandle)
                .NeverCull();
        }
        stop(Stage::Build);
        start(Stage::Compile);
        const auto compiled = graph.Compile();
        stop(Stage::Compile);
        if (!compiled) return false;
        const bool prepared = compiled->Prepare(slot, pool, staging, uploads);
        if (!prepared) return false;
        auto command = slot.AcquireCommandBuffer();
        if (!command) return false;
        command->Begin();
        const bool recorded = compiled->Record(*command);
        command->End();
        if (!recorded) return false;
        compiled->CommitSubmittedStates();
        stop(Stage::Frame);
        scheduler->RunFrame();
        if (measured) measuredAllocations += memory.Allocations - allocBefore;
        return true;
    };

    if (!execute(false)) {
        state.SkipWithError("sparse copy warmup failed");
        failed = true;
        return;
    }
    for (auto _ : state) {
        state.PauseTiming();
        if (!execute(true)) {
            state.ResumeTiming();
            state.SkipWithError("sparse copy failed");
            failed = true;
            break;
        }
        state.ResumeTiming();
    }
    scheduler->BeginFrame();
    scheduler->WaitIdle();
    const auto iterations = std::max<int64_t>(1, state.iterations());
    state.counters["copy_passes"] = kCopyPasses;
    state.counters["high_buffer_index"] = kPaddingBuffers;
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

class SceneBenchProcessor final : public MeshPassProcessor {
public:
    explicit SceneBenchProcessor(gpu::GraphicsPipelineState& pipeline) noexcept : _pipeline(&pipeline) {}
    void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) override {
        BuildMeshDrawCommands(context, {.Pipeline = _pipeline}, output);
    }

private:
    gpu::GraphicsPipelineState* _pipeline;
};

// C5: Scene 10,000 Batch 双入口可见性收集基准。
void RunSceneGather(benchmark::State& state, bool byPrimitives, bool& failed) {
    constexpr uint32_t kCount = 10000;
    constexpr MeshPassId kPass{0};
    test::FakePipeline pipeline;
    test::FakeBuffer vertices{128, BufferUse::Vertex};
    test::FakeBuffer indices{128, BufferUse::Index};
    VertexInput input;
    input.Layouts.push_back({0, 12, VertexStepMode::Vertex});
    input.Attributes.push_back({0, 0, "POSITION", 0, VertexFormat::FLOAT32X3});
    input.Streams.push_back({&vertices, 0, 48, 0});

    Scene scene;
    scene.Reserve(kCount, kCount, 1);
    const auto material = scene.AddMaterial({});
    vector<PrimitiveHandle> primitives;
    primitives.reserve(kCount);
    vector<VisibleMeshBatch> visibleBatches;
    visibleBatches.reserve(kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        const auto prim = scene.AddPrimitive();
        MeshBatch batch;
        batch.Material = material;
        batch.VertexInput = &input;
        batch.Elements.push_back({.IndexBuffer = &indices, .FirstIndex = 0, .IndexCount = 3});
        const auto handle = scene.AddMeshBatch(prim, std::move(batch));
        if (!handle.IsValid()) {
            state.SkipWithError("scene setup failed");
            failed = true;
            return;
        }
        primitives.push_back(prim);
        visibleBatches.push_back({.Batch = handle, .VisibilitySortKey = i});
    }
    SceneBenchProcessor processor{pipeline};
    const MeshPassRegistration reg{kPass, &processor};
    scene.SyncCachedMeshDrawCommands(std::span{&reg, 1});

    vector<VisibleMeshDrawCommand> out;
    out.reserve(kCount);
    const bool warmOk = byPrimitives
                            ? scene.GatherVisiblePrimitives(kPass, primitives, out)
                            : scene.GatherVisibleMeshDrawCommands(kPass, visibleBatches, out);
    if (!warmOk || out.size() != kCount) {
        state.SkipWithError("scene gather warmup failed");
        failed = true;
        return;
    }

    for (auto _ : state) {
        out.clear();
        const bool ok = byPrimitives
                            ? scene.GatherVisiblePrimitives(kPass, primitives, out)
                            : scene.GatherVisibleMeshDrawCommands(kPass, visibleBatches, out);
        if (!ok) {
            state.SkipWithError("scene gather failed");
            failed = true;
            break;
        }
        benchmark::DoNotOptimize(out.data());
        benchmark::DoNotOptimize(out.size());
    }
    state.counters["batches"] = kCount;
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
    const vector<std::pair<Stage, std::string_view>> denseStages{std::pair{Stage::Build, "Build"}, {Stage::Prepare, "Prepare"}, {Stage::Frame, "Frame"}};
    for (const auto& [stage, stageName] : denseStages) {
        if (memoryProbe && stage != Stage::Frame) continue;
        const auto label = radray::format("RDGHotpath/DensePass/{}", stageName);
        benchmark::RegisterBenchmark(label.c_str(), [&, stage](benchmark::State& state) { RunRdgDensePass(state, stage, memoryProbe, failed); })->UseRealTime();
    }
    const vector<std::pair<Stage, std::string_view>> sparseStages{std::pair{Stage::Build, "Build"}, {Stage::Compile, "Compile"}, {Stage::Frame, "Frame"}};
    for (const auto& [stage, stageName] : sparseStages) {
        if (memoryProbe && stage != Stage::Frame) continue;
        const auto label = radray::format("RDGHotpath/SparseCopy1024/{}", stageName);
        benchmark::RegisterBenchmark(label.c_str(), [&, stage](benchmark::State& state) { RunRdgSparseCopy1024(state, stage, memoryProbe, failed); })->UseRealTime();
    }
    if (!memoryProbe) {
        const array<std::string_view, 3> components{"PrepareLayout", "AllocateFill", "Flush"};
        for (uint32_t i = 0; i < components.size(); ++i) {
            const auto label = radray::format("RDGUploadComponent/{}", components[i]);
            benchmark::RegisterBenchmark(label.c_str(), [&, i](benchmark::State& state) { RunPrepareComponent(state, i, failed); })->UseRealTime();
        }
        const array<std::pair<uint32_t, std::string_view>, 4> sortCases{
            std::pair{0u, "Sorted0Pass"}, {1u, "Odd1Pass"}, {3u, "Odd3Pass"}, {4u, "Even4Pass"}};
        for (const auto& [bytes, name] : sortCases) {
            const auto label = radray::format("RuntimeHotpath/SortVisible/{}", name);
            benchmark::RegisterBenchmark(label.c_str(), [&, bytes](benchmark::State& state) { RunSortVisible(state, bytes, failed); })->UseRealTime();
        }
        benchmark::RegisterBenchmark("RuntimeHotpath/MeshDraw/SingleElement/IndirectCountBuild", [&](benchmark::State& state) { RunMeshDrawHotpath(state, 1, false, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/MeshDraw/MultiElement7/IndirectCountBuild", [&](benchmark::State& state) { RunMeshDrawHotpath(state, 7, false, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/MeshDraw/SingleElement/Submit", [&](benchmark::State& state) { RunMeshDrawHotpath(state, 1, true, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/MeshDraw/MultiElement7/Submit", [&](benchmark::State& state) { RunMeshDrawHotpath(state, 7, true, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/IndirectBuild/SinglePage", [&](benchmark::State& state) { RunIndirectBuild(state, false, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/IndirectBuild/AlternatingPages", [&](benchmark::State& state) { RunIndirectBuild(state, true, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/GeometryPool/GetFreeBytes100Pages", [&](benchmark::State& state) { RunGeometryPoolHotpath(state, false, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/GeometryPool/AllocFreeCycle100Pages", [&](benchmark::State& state) { RunGeometryPoolHotpath(state, true, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/SceneGather/Batches10k", [&](benchmark::State& state) { RunSceneGather(state, false, failed); })->UseRealTime();
        benchmark::RegisterBenchmark("RuntimeHotpath/SceneGather/Primitives10k", [&](benchmark::State& state) { RunSceneGather(state, true, failed); })->UseRealTime();
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return failed ? 1 : 0;
}
