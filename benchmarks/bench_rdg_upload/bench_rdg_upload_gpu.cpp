#include <benchmark/benchmark.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <radray/profiler.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/gpu_env.h>
#include <radray/runtime/gpu_resource.h>

namespace {
using namespace radray;
using namespace radray::render;
using Clock = std::chrono::steady_clock;

class WaitPipeline final : public FramePipeline {
public:
    void Extract(FrameSlot&) noexcept override {}
    void Prepare(FrameSlot&) noexcept override {}
    void Record(FrameSlot&) noexcept override {}
    void Submit(FrameSlot&) noexcept override {}
    void Wait(FrameSlot& slot) noexcept override { slot.GetQueue().Wait(); }
};

struct Sample {
    double PrepareUs, RecordUs, GpuUs;
};

double Microseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

double Percentile(vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(fraction * (values.size() - 1))];
}

void Run(benchmark::State& state, RenderBackend backend, uint32_t warmup, const string& rawPath, bool& failed) {
    const auto fail = [&](const char* message) { state.SkipWithError(message); failed = true; };
    auto environment = Environment::Create(backend, false, "RDG upload GPU benchmark");
    if (!environment) return fail("environment creation failed");
    auto context = DeviceContext::Create(*environment);
    if (!context) return fail("device creation failed");
    auto& device = context->GetDevice();
    auto& queue = context->GetQueue();
    auto buffer = GpuBuffer::Create(device, {.Size = 2048, .Usage = BufferUse::CopySource});
    auto texture = GpuTexture::Create(device, {.Width = 32, .Height = 16, .Format = TextureFormat::RGBA8_UNORM});
    auto observed = device.CreateBuffer({.Size = 2048, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead});
    auto queries = device.CreateQueryPool({.Count = 2});
    auto timestamps = device.CreateBuffer({.Size = 16, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead});
    if (!buffer || !texture || !observed || !queries || !timestamps) return fail("resource creation failed");
    const auto calibration = queries->GetTimestampCalibration(&queue);
    if (calibration.TickPeriodNs <= 0) return fail("timestamps unavailable");
    RDGTransientResourcePool pool{device};
    UploadArena uploads{device};
    auto scheduler = FrameScheduler::Create(*context, make_unique<WaitPipeline>(), 1).Unwrap();
    vector<Sample> samples;
    samples.reserve(static_cast<size_t>(state.max_iterations));
    const array<byte, 4> pixel{byte{19}, byte{29}, byte{37}, byte{255}};
    const auto execute = [&](Sample& sample) {
        auto& slot = scheduler->BeginFrame();
        pool.BeginFrame();
        uploads.Reset();
        RDGBuilder graph{slot, context->GetRDGConstraints(), {.FullValidation = true}};
        const auto dst = graph.ImportPersistentBorrowed(*buffer);
        const auto image = graph.ImportPersistentBorrowed(*texture);
        const auto out = graph.ImportBuffer(observed.Get(), BufferState::HostRead, BufferState::HostRead);
        for (uint32_t i = 0; i < 512; ++i) {
            graph.UploadBuffer(dst, i * 4, RDGUploadSource::BorrowedBytes(pixel));
            graph.UploadTexture(image, {.X = i % 32, .Y = i / 32, .Width = 1, .Height = 1}, RDGTextureUploadSource::BorrowedBytes(pixel, 4));
            if (i == 255) {
                struct Copy {
                    RDGBufferHandle Source, Destination;
                };
                graph.AddCopyPass("middle consumer", Copy{dst, out}, +[](const Copy& data, RDGCopyPassContext& ctx) noexcept {
                    const auto source = ctx.ResolveBuffer(data.Source), destination = ctx.ResolveBuffer(data.Destination);
                    ctx.GetCommandBuffer().CopyBufferToBuffer(destination.Buffer.Get(), 0, source.Buffer.Get(), 0, 1024);
                    return true; }).ReadBuffer(dst).WriteBuffer(out).NeverCull();
            }
        }
        graph.MarkOutput(dst);
        graph.MarkOutput(image);
        const auto compiled = graph.Compile();
        if (!compiled) return false;
        const auto prepareBegin = Clock::now();
        const bool prepared = compiled->Prepare(slot, pool, uploads);
        const auto prepareEnd = Clock::now();
        if (!prepared) return false;
        auto command = slot.AcquireCommandBuffer();
        if (!command) return false;
        command->Begin();
        command->ResetQueryPool(queries.Get(), 0, 2);
        const ResourceBarrierDescriptor begin = BarrierBufferDescriptor{timestamps.Get(), BufferState::HostRead, BufferState::CopyDestination};
        command->ResourceBarrier(std::span{&begin, 1});
        command->WriteTimestamp({queries.Get(), QueryPipelineStage::Bottom, 0});
        command->PushDebugGroup("RDG benchmark frame");
        const auto recordBegin = Clock::now();
        const bool recorded = compiled->Record(*command);
        const auto recordEnd = Clock::now();
        command->PopDebugGroup();
        command->WriteTimestamp({queries.Get(), QueryPipelineStage::Bottom, 1});
        command->ResolveQueryData({queries.Get(), 0, 2, timestamps.Get(), 0});
        const ResourceBarrierDescriptor end = BarrierBufferDescriptor{timestamps.Get(), BufferState::CopyDestination, BufferState::HostRead};
        command->ResourceBarrier(std::span{&end, 1});
        command->End();
        if (!recorded) return false;
        auto* raw = command.Get();
        queue.Submit({.CmdBuffers = std::span{&raw, 1}});
        compiled->CommitSubmittedStates();
        scheduler->RunFrame();
        queue.Wait();
        auto* mapped = timestamps->Map(0, 16);
        if (!mapped) return false;
        timestamps->InvalidateMappedRange({0, 16});
        array<uint64_t, 2> ticks{};
        std::memcpy(ticks.data(), mapped, 16);
        timestamps->Unmap();
        if (ticks[1] <= ticks[0]) return false;
        sample = {Microseconds(prepareBegin, prepareEnd), Microseconds(recordBegin, recordEnd),
                  static_cast<double>(ticks[1] - ticks[0]) * calibration.TickPeriodNs / 1000.0};
        RADRAY_PROFILE_FRAME();
        return true;
    };
    for (uint32_t i = 0; i < warmup; ++i) {
        Sample sample{};
        if (!execute(sample)) return fail("warmup failed");
    }
    for (auto _ : state) {
        Sample sample{};
        if (!execute(sample)) {
            fail("frame failed");
            break;
        }
        samples.push_back(sample);
        state.SetIterationTime(sample.GpuUs / 1e6);
    }
    scheduler->WaitIdle();
    if (failed || samples.empty()) return;
    vector<double> prepare, record, gpu;
    for (const auto& sample : samples) {
        prepare.push_back(sample.PrepareUs);
        record.push_back(sample.RecordUs);
        gpu.push_back(sample.GpuUs);
    }
    for (const auto& [name, fraction] : array{std::pair{"p50", 0.50}, std::pair{"p95", 0.95}, std::pair{"p99", 0.99}}) {
        state.counters[format("prepare_{}_us", name).c_str()] = Percentile(prepare, fraction);
        state.counters[format("record_{}_us", name).c_str()] = Percentile(record, fraction);
        state.counters[format("gpu_{}_us", name).c_str()] = Percentile(gpu, fraction);
    }
    state.counters["uploads"] = 1024;
    state.counters["upload_bytes"] = 4096;
    state.counters["outer_timestamp_writes"] = 2;
    state.counters["outer_query_resolves"] = 1;
    if (!rawPath.empty()) {
        std::ofstream output{rawPath.c_str()};
        output << "prepare_us,record_us,gpu_us\n";
        for (const auto& sample : samples) output << format("{},{},{}\n", sample.PrepareUs, sample.RecordUs, sample.GpuUs).c_str();
        if (!output) fail("raw sample output failed");
    }
}
}  // namespace

int main(int argc, char** argv) {
    RenderBackend backend = RenderBackend::D3D12;
    uint32_t samples = 1000, warmup = 200;
    string rawPath;
    bool waitForCapture = false;
    int retained = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--rdg_backend=vulkan")
            backend = RenderBackend::Vulkan;
        else if (arg == "--rdg_backend=d3d12")
            backend = RenderBackend::D3D12;
        else if (arg == "--rdg_capture_wait")
            waitForCapture = true;
        else if (arg.starts_with("--rdg_raw="))
            rawPath = arg.substr(10);
        else if (arg.starts_with("--rdg_samples=") || arg.starts_with("--rdg_warmup=")) {
            const auto value = arg.substr(arg.find('=') + 1);
            auto& target = arg.starts_with("--rdg_samples=") ? samples : warmup;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), target);
            if (error != std::errc{} || end != value.data() + value.size() || target == 0) return 2;
        } else
            argv[retained++] = argv[i];
    }
    argc = retained;
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 2;
    RADRAY_PROFILE_FRAME();
    if (waitForCapture) {
        std::puts("Waiting for capture; send newline after Tracy connects.");
        std::fflush(stdout);
        if (std::getchar() == EOF) return 2;
    }
    bool failed = false;
    benchmark::RegisterBenchmark("RDGUploadGpu", [&](benchmark::State& state) { Run(state, backend, warmup, rawPath, failed); })
        ->Iterations(samples)
        ->UseManualTime()
        ->Unit(benchmark::kMicrosecond);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return failed ? 1 : 0;
}
