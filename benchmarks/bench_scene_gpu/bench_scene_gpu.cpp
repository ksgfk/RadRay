#include "bench_scene_gpu.h"
#include "scene_gpu_workload.h"
#include <benchmark/benchmark.h>
#include <fmt/format.h>

void radray::benchmarking::RegisterSceneGpuBenchmarks() {
    using Load = radray::benchmarking::SceneGpuLoad;
    struct Scenario {
        const char* Name;
        uint32_t Changes;
        Load Kind;
    };
    const radray::array<Scenario, 8> scenarios{{{"changed:0", 0, Load::Transforms}, {"changed:1", 1, Load::Transforms}, {"changed:100", 100, Load::Transforms}, {"changed:10000", 10000, Load::Transforms}, {"parent", 1, Load::Parent}, {"churn", 10, Load::Churn}, {"first_use", 0, Load::FirstUse}, {"growth", 0, Load::Growth}}};
    for (auto backend : {radray::render::RenderBackend::D3D12, radray::render::RenderBackend::Vulkan})
        for (uint32_t flights : {1u, 2u, 3u})
            for (const auto scenario : scenarios)
                for (uint32_t views : {1u, 3u})
                    for (uint32_t phase = 0; phase < 3; ++phase) {
                        const bool cold = scenario.Kind == Load::FirstUse || scenario.Kind == Load::Growth;
                        if (cold && phase != 1) continue;
                        const radray::array<std::string_view, 3> phases{"CpuSync", "PrepareAndRecord", "SubmitFrame"};
                        const auto name = fmt::format("SceneGpu/{}/{}/F{}/{}/views:{}", backend == radray::render::RenderBackend::D3D12 ? "d3d12" : "vulkan", phases[phase], flights, scenario.Name, views);
                        benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                            radray::benchmarking::SceneGpuWorkload workload{backend, flights, scenario.Changes, views, scenario.Kind};
                            if (!workload.IsValid()) {
                                state.SkipWithError(workload.Error());
                                return;
                            }
                            for (auto _ : state) {
                                if (phase == 0) {
                                    state.PauseTiming();
                                    const auto flight = workload.BeginUpdate();
                                    state.ResumeTiming();
                                    workload.SealScene(flight);
                                    state.PauseTiming();
                                    auto frame = workload.BeginRecord(flight);
                                    state.ResumeTiming();
                                    workload.Consume(frame);
                                    state.PauseTiming();
                                    workload.Prepare(frame);
                                    workload.Submit(frame);
                                    state.ResumeTiming();
                                    continue;
                                }
                                if (phase == 1) state.PauseTiming();
                                if (cold) workload.RestartColdScene();
                                auto frame = workload.Begin();
                                if (phase == 1) state.ResumeTiming();
                                workload.Prepare(frame);
                                if (phase == 1) state.PauseTiming();
                                workload.Submit(frame);
                                if (phase == 1) state.ResumeTiming();
                            }
                            workload.Drain();
                            if (!workload.Error().empty()) state.SkipWithError(workload.Error());
                            state.counters["upload_bytes/frame"] = double(workload.UploadedBytes) / state.iterations();
                            state.counters["upload_ranges/frame"] = double(workload.UploadRanges) / state.iterations();
                            state.counters["gpu_ms/frame"] = workload.GpuMilliseconds / state.iterations();
                            state.SetItemsProcessed(state.iterations());
                        })->UseRealTime()
                            ->Unit(benchmark::kMicrosecond);
                    }
}

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    radray::benchmarking::RegisterSceneGpuBenchmarks();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
