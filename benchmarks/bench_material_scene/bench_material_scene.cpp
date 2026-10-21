#include "bench_material_scene.h"
#include "material_scene_workload.h"
#include <benchmark/benchmark.h>
#include <fmt/format.h>
#include <radray/runtime/render_framework/renderer.h>
#include <radray/runtime/render_framework/renderer_frame.h>
#include <radray/runtime/frame_timeline.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::benchmarking {
namespace {
/// Drives the public Renderer API directly: one material, one view, and `Count` draws that each carry a
/// distinct MaterialConstants override, so every draw resolves its material group through the frame memo
/// with a distinct input list. This is the shape that would expose a linear memo scan as O(draws^2).
class GroupMemoWorkload {
public:
    GroupMemoWorkload(render::RenderBackend backend, uint32_t count) : _timeline(1), _count(count) {
        const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
        GpuSystemDescriptor descriptor{.FlightDataCount = 1};
        if (backend == render::RenderBackend::Vulkan) {
            render::VulkanDeviceDescriptor vk;
            vk.Queues = std::span{&queue, 1};
            descriptor.Device = vk;
        }
        descriptor.RhiValidation.IsEnabled = false;
        RuntimeStartupResult startup;
        _gpu = GpuSystem::TryCreate(descriptor, _timeline, startup);
        if (!_gpu) {
            _error = startup.Reason;
            return;
        }
        _services = make_unique<RenderSystem>(_gpu->GetDevice(), RADRAY_BENCH_SHADER_DIR, vector<std::filesystem::path>{RADRAY_SHADERLIB_DIR});
        if (!_services->OnInitialize()) {
            _error = "RenderSystem initialization failed";
            return;
        }
        _renderer = make_unique<Renderer>(*_gpu, *_services);
        MaterialTemplateDescriptor definition;
        definition.Name = "OverrideScan";
        definition.Program.SourceName = "override_scan.hlsl";
        definition.Inputs = {{"ViewData", MaterialInputSource::ViewConstants}, {"Tint", MaterialInputSource::MaterialConstants}};
        const auto program = _services->GetOrCreateShaderProgram(definition.Program);
        if (!program) {
            _error = "Override scan shader compilation failed";
            return;
        }
        definition.RegisteredProgram = program->GetId();
        definition.Program = {};
        const auto templateId = _renderer->RegisterTemplate(definition);
        if (!templateId) {
            _error = _renderer->GetLastError();
            return;
        }
        _template = *templateId;
        const array<float, 4> tint{1, 0, 0, 1};
        const auto tintBytes = std::as_bytes(std::span{tint});
        RendererMaterialDescriptor material{{_template}, {vector<byte>{tintBytes.begin(), tintBytes.end()}}, {}, {}};
        const auto materialId = _renderer->RegisterMaterial(material);
        if (!materialId) {
            _error = _renderer->GetLastError();
            return;
        }
        _material = *materialId;
        _overrides.reserve(_count);
        for (uint32_t i = 0; i < _count; ++i) {
            const array<float, 4> unique{float(i % 97) / 97.f, float(i % 89) / 89.f, float(i % 83) / 83.f, 1};
            const auto bytes = std::as_bytes(std::span{unique});
            _overrides.emplace_back(bytes.begin(), bytes.end());
        }
    }
    const string& Error() const noexcept { return _error; }
    bool BeginFrame() {
        _context.emplace(_gpu->BeginFrameRecord(0, {}, {}, false));
        _builder = _renderer->BeginFrame(*_context);
        if (!_builder) {
            _error = _renderer->GetLastError();
            return false;
        }
        const array<float, 4> view{0, 0, 0, 0};
        const auto viewBytes = std::as_bytes(std::span{view});
        const auto viewId = _builder->AddView({{vector<byte>{viewBytes.begin(), viewBytes.end()}}});
        if (!viewId) {
            _error = _renderer->GetLastError();
            return false;
        }
        const auto pass = _builder->AddPass({*viewId, _template, {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}}});
        if (!pass) {
            _error = _renderer->GetLastError();
            return false;
        }
        _pass = *pass;
        return true;
    }
    bool AddDraws() {
        for (uint32_t i = 0; i < _count; ++i) {
            RendererDrawDescriptor draw;
            draw.Count = 3;
            draw.Viewport = {0, 0, 16, 16, 0, 1};
            draw.Scissor = {0, 0, 16, 16};
            RendererDrawValues values;
            values.Constants.push_back(_overrides[i]);
            if (!_builder->AddDraw(_pass, _material, draw, std::move(values))) {
                _error = _renderer->GetLastError();
                return false;
            }
        }
        return true;
    }
    bool Compile() {
        if (!_builder->Compile()) {
            _error = _renderer->GetLastError();
            return false;
        }
        return true;
    }
    void EndFrame() {
        _context->SubmitFrame();
        _gpu->WaitAndRetireFlights();
        FlightCompletion completion;
        while (_timeline.TryReadCompletion(completion)) _gpu->ReleaseFrameResourcesGT(completion);
        _renderer->ProcessCompletions();
        _builder.reset();
        _context.reset();
    }
    uint32_t Count() const noexcept { return _count; }

private:
    FrameTimeline _timeline;
    unique_ptr<GpuSystem> _gpu;
    unique_ptr<RenderSystem> _services;
    unique_ptr<Renderer> _renderer;
    std::optional<AppFrameContext> _context;
    std::optional<FrameBuilder> _builder;
    vector<vector<byte>> _overrides;
    TemplateId _template;
    MaterialId _material;
    FramePassId _pass;
    uint32_t _count{0};
    string _error;
};
}  // namespace
}  // namespace radray::benchmarking

void radray::benchmarking::RegisterMaterialSceneBenchmarks() {
    for (uint32_t stage = 1; stage <= 3; ++stage) {
        const auto name = fmt::format("MaterialScene/Churn/stage{}", stage);
        benchmark::RegisterBenchmark(name.c_str(), [stage](benchmark::State& state) {
            MaterialSceneWorkload workload{render::RenderBackend::D3D12, 1000, 1, 1, 10, stage};
            if (!workload.Error().empty()) {
                state.SkipWithError(workload.Error().c_str());
                return;
            }
            for (auto _ : state) {
                const auto flight = workload.BeginUpdate();
                workload.Sync(flight);
                auto frame = workload.BeginRecord(flight);
                workload.PrepareAndRecord(frame);
                workload.Submit(frame);
                if (!workload.Error().empty()) {
                    state.SkipWithError(workload.Error().c_str());
                    break;
                }
            }
            workload.Drain();
        })->UseRealTime()
            ->Unit(benchmark::kMicrosecond);
    }
    for (uint32_t stage = 1; stage <= 3; ++stage) {
        const auto name = fmt::format("MaterialScene/UniqueAssetChurn/stage{}", stage);
        benchmark::RegisterBenchmark(name.c_str(), [stage](benchmark::State& state) {
            MaterialSceneWorkload workload{render::RenderBackend::D3D12, 1000, 1, 1, 10, stage, 1, true};
            if (!workload.Error().empty()) {
                state.SkipWithError(workload.Error().c_str());
                return;
            }
            for (auto _ : state) {
                auto flight = workload.BeginUpdate();
                workload.Sync(flight);
                auto frame = workload.BeginRecord(flight);
                workload.PrepareAndRecord(frame);
                workload.Submit(frame);
                if (!workload.Error().empty()) {
                    state.SkipWithError(workload.Error().c_str());
                    break;
                }
            }
            workload.Drain();
        })->UseRealTime()
            ->Unit(benchmark::kMicrosecond);
    }
    for (const uint32_t mode : {0u, 2u, 3u})
        for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
            for (const uint32_t count : {1000u, 10000u, 100000u})
                for (const uint32_t views : {1u, 3u})
                    for (const uint32_t percent : {0u, 1u, 10u, 100u})
                        for (const bool complete : {false, true})
                            for (const uint32_t flights : {1u, 2u, 3u, 4u}) {
                                const auto name = fmt::format("MaterialScene/{}/{}/{}/N{}/M1/V{}/dirty{}", mode == 0 ? "Direct" : mode == 2 ? "Manual"
                                                                                                                                            : "ManualTint",
                                                              backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", complete ? "FullFrame" : "PrepareRecord", count, views, percent);
                                const auto flightName = flights == 2 ? name : fmt::format("{}/F{}", name, flights);
                                benchmark::RegisterBenchmark(flightName.c_str(), [=](benchmark::State& state) {
                                    MaterialSceneWorkload workload{backend, count, 1, views, percent, 0, mode, false, flights};
                                    if (!workload.Error().empty()) {
                                        state.SkipWithError(workload.Error().c_str());
                                        return;
                                    }
                                    for (auto _ : state) {
                                        if (!complete) state.PauseTiming();
                                        auto flight = workload.BeginUpdate();
                                        workload.Sync(flight);
                                        auto frame = workload.BeginRecord(flight);
                                        if (!complete) state.ResumeTiming();
                                        workload.PrepareAndRecord(frame);
                                        if (!complete) state.PauseTiming();
                                        workload.Submit(frame);
                                        if (!complete) state.ResumeTiming();
                                        if (!workload.Error().empty()) {
                                            state.SkipWithError(workload.Error().c_str());
                                            break;
                                        }
                                    }
                                    const auto pendingUsage = workload.GetNativeAllocatorUsage();
                                    workload.Drain();
                                    const auto idleUsage = workload.GetNativeAllocatorUsage();
                                    state.counters["native_block_bytes"] = double(idleUsage[0]);
                                    state.counters["native_allocation_bytes"] = double(idleUsage[1]);
                                    state.counters["native_block_count"] = double(idleUsage[2]);
                                    state.counters["native_allocation_count"] = double(idleUsage[3]);
                                    state.counters["native_allocation_bytes_pending"] = double(pendingUsage[1]);
                                    const auto frames = double(workload.Frames);
                                    state.counters["draws/frame"] = frames ? workload.Draws / frames : 0;
                                    state.counters["host_bytes/frame"] = frames ? workload.UploadBytes / frames : 0;
                                    state.counters["gpu_ms/frame"] = frames ? workload.GpuMs / frames : 0;
                                    state.counters["scene_buffer_bytes"] = double(workload.GetSceneAllocatedBytes());
                                    state.counters["actors"] = static_cast<double>(workload.ActorCount());
                                })->UseRealTime()
                                    ->Unit(benchmark::kMicrosecond);
                            }
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (const uint32_t count : {1000u, 10000u, 100000u})
            for (const uint32_t materials : {1u, 100u, count})
                for (const uint32_t views : {1u, 3u})
                    for (const uint32_t percent : {0u, 1u, 10u, 100u})
                        for (const bool complete : {false, true})
                            for (const uint32_t flights : {1u, 2u, 3u, 4u}) {
                                const auto name = fmt::format("MaterialScene/{}/{}/N{}/M{}/V{}/dirty{}", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", complete ? "FullFrame" : "PrepareRecord", count, materials, views, percent);
                                const auto flightName = flights == 2 ? name : fmt::format("{}/F{}", name, flights);
                                benchmark::RegisterBenchmark(flightName.c_str(), [=](benchmark::State& state) {
                                    MaterialSceneWorkload workload{backend, count, materials, views, percent, 0, 1, false, flights};
                                    if (!workload.Error().empty()) {
                                        state.SkipWithError(workload.Error().c_str());
                                        return;
                                    }
                                    for (auto _ : state) {
                                        if (!complete) state.PauseTiming();
                                        const auto flight = workload.BeginUpdate();
                                        workload.Sync(flight);
                                        auto frame = workload.BeginRecord(flight);
                                        if (!complete) state.ResumeTiming();
                                        workload.PrepareAndRecord(frame);
                                        if (!complete) state.PauseTiming();
                                        workload.Submit(frame);
                                        if (!complete) state.ResumeTiming();
                                        if (!workload.Error().empty()) {
                                            state.SkipWithError(workload.Error().c_str());
                                            break;
                                        }
                                    }
                                    const auto pendingUsage = workload.GetNativeAllocatorUsage();
                                    workload.Drain();
                                    const auto idleUsage = workload.GetNativeAllocatorUsage();
                                    state.counters["native_block_bytes"] = double(idleUsage[0]);
                                    state.counters["native_allocation_bytes"] = double(idleUsage[1]);
                                    state.counters["native_block_count"] = double(idleUsage[2]);
                                    state.counters["native_allocation_count"] = double(idleUsage[3]);
                                    state.counters["native_allocation_bytes_pending"] = double(pendingUsage[1]);
                                    const auto frames = double(workload.Frames);
                                    state.counters["draws/frame"] = frames ? workload.Draws / frames : 0;
                                    state.counters["host_bytes/frame"] = frames ? workload.UploadBytes / frames : 0;
                                    state.counters["gpu_ms/frame"] = frames ? workload.GpuMs / frames : 0;
                                    state.counters["scene_buffer_bytes"] = double(workload.GetSceneAllocatedBytes());
                                    state.SetItemsProcessed(state.iterations());
                                })->UseRealTime()
                                    ->Unit(benchmark::kMicrosecond);
                            }
    // Phase-isolated cases: PauseTiming/ResumeTiming covers one frame part so fence waits,
    // submission and recording do not inflate the number.
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (const uint32_t materials : {1u, 7u, 32u, 100u, 300u}) {
            const auto name = fmt::format("MaterialScene/CompileOnly/{}/N1000/M{}/V3/F3", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", materials);
            benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                MaterialSceneWorkload workload{backend, 1000, materials, 3, 0, 0, 1, false, 3};
                if (!workload.Error().empty()) {
                    state.SkipWithError(workload.Error().c_str());
                    return;
                }
                for (auto _ : state) {
                    state.PauseTiming();
                    const auto flight = workload.BeginUpdate();
                    workload.Sync(flight);
                    auto frame = workload.BeginRecord(flight);
                    workload.PrepareAndRecord(frame, &state, PreparePhase::Compile);
                    workload.Submit(frame);
                    state.ResumeTiming();
                    if (!workload.Error().empty()) {
                        state.SkipWithError(workload.Error().c_str());
                        break;
                    }
                }
                workload.Drain();
                const auto frames = double(workload.Frames);
                state.counters["draws/frame"] = frames ? workload.Draws / frames : 0;
            })->UseRealTime()
                ->Unit(benchmark::kMicrosecond);
        }
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (const uint32_t materials : {1u, 100u}) {
            const auto name = fmt::format("MaterialScene/CompletionCpu/{}/N1000/M{}/V3/F3", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", materials);
            benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                MaterialSceneWorkload workload{backend, 1000, materials, 3, 0, 0, 1, false, 3};
                if (!workload.Error().empty()) {
                    state.SkipWithError(workload.Error().c_str());
                    return;
                }
                for (auto _ : state) {
                    state.PauseTiming();
                    const auto flight = workload.BeginUpdate(&state);
                    workload.Sync(flight);
                    auto frame = workload.BeginRecord(flight);
                    workload.PrepareAndRecord(frame);
                    workload.Submit(frame);
                    state.ResumeTiming();
                    if (!workload.Error().empty()) {
                        state.SkipWithError(workload.Error().c_str());
                        break;
                    }
                }
                workload.Drain();
                const auto frames = double(workload.Frames);
                state.counters["draws/frame"] = frames ? workload.Draws / frames : 0;
            })->UseRealTime()
                ->Unit(benchmark::kMicrosecond);
        }
    // Scale scan: one distinct MaterialConstants override per draw, so the frame memo resolves `draws`
    // unique material groups per frame. A linear memo scan is O(draws^2) here; the flat index is O(draws).
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (const uint32_t count : {100u, 1000u, 4000u}) {
            const auto name = fmt::format("MaterialScene/OverrideChurn/{}/N{}/M1/V1/F1", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", count);
            benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                GroupMemoWorkload workload{backend, count};
                if (!workload.Error().empty()) {
                    state.SkipWithError(workload.Error().c_str());
                    return;
                }
                for (auto _ : state) {
                    state.PauseTiming();
                    if (!workload.BeginFrame() || !workload.AddDraws()) {
                        state.ResumeTiming();
                        state.SkipWithError(workload.Error().c_str());
                        break;
                    }
                    state.ResumeTiming();
                    const bool compiled = workload.Compile();
                    state.PauseTiming();
                    workload.EndFrame();
                    if (!compiled) {
                        state.ResumeTiming();
                        state.SkipWithError(workload.Error().c_str());
                        break;
                    }
                }
                state.counters["draws/frame"] = double(workload.Count());
            })->UseRealTime()
                ->Unit(benchmark::kMicrosecond);
        }
}
int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    radray::benchmarking::RegisterMaterialSceneBenchmarks();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
