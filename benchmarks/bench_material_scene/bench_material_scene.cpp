#include "bench_material_scene.h"
#include "material_scene_workload.h"
#include <benchmark/benchmark.h>
#include <fmt/format.h>

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
                        for (const bool complete : {false, true}) {
                            const auto name = fmt::format("MaterialScene/{}/{}/{}/N{}/M1/V{}/dirty{}", mode == 0 ? "Direct" : mode == 2 ? "Manual"
                                                                                                                                        : "ManualTint",
                                                          backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", complete ? "FullFrame" : "PrepareRecord", count, views, percent);
                            benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                                MaterialSceneWorkload workload{backend, count, 1, views, percent, 0, mode};
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
                                workload.Drain();
                                const auto frames = double(workload.Frames);
                                state.counters["draws/frame"] = frames ? workload.Draws / frames : 0;
                                state.counters["host_bytes/frame"] = frames ? workload.UploadBytes / frames : 0;
                                state.counters["gpu_ms/frame"] = frames ? workload.GpuMs / frames : 0;
                                state.counters["actors"] = static_cast<double>(workload.ActorCount());
                            })->UseRealTime()
                                ->Unit(benchmark::kMicrosecond);
                        }
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (const uint32_t count : {1000u, 10000u, 100000u})
            for (const uint32_t materials : {1u, 100u, count})
                for (const uint32_t views : {1u, 3u})
                    for (const uint32_t percent : {0u, 1u, 10u, 100u})
                        for (const bool complete : {false, true}) {
                            const auto name = fmt::format("MaterialScene/{}/{}/N{}/M{}/V{}/dirty{}", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", complete ? "FullFrame" : "PrepareRecord", count, materials, views, percent);
                            benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                                MaterialSceneWorkload workload{backend, count, materials, views, percent};
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
                                workload.Drain();
                                const auto frames = double(workload.Frames);
                                state.counters["draws/frame"] = frames ? workload.Draws / frames : 0;
                                state.counters["host_bytes/frame"] = frames ? workload.UploadBytes / frames : 0;
                                state.counters["gpu_ms/frame"] = frames ? workload.GpuMs / frames : 0;
                                state.SetItemsProcessed(state.iterations());
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
