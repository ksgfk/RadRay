// Measurement boundaries and invocation: docs/guide/build-test.md
#include <benchmark/benchmark.h>
#include "scene_sync_workload.h"

#include <fmt/format.h>

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    for (const auto& scenario : radray::test::SceneSyncScenarios(false)) {
        for (uint32_t flights : {1u, 2u, 3u}) {
            for (bool threaded : {false, true}) {
                const auto name = fmt::format("SceneSync/{}/F{}/{}", scenario.Name, flights, threaded ? "threaded" : "single");
                benchmark::RegisterBenchmark(name.c_str(), [scenario, flights, threaded](benchmark::State& state) {
                    radray::test::SceneSyncWorkload workload{scenario, flights, threaded};
                    constexpr int64_t batchFrames = 256;
                    while (state.KeepRunningBatch(batchFrames)) workload.RunFrames(batchFrames);
                    state.SetItemsProcessed(state.iterations());
                })->MeasureProcessCPUTime()->UseRealTime()->Unit(benchmark::kMicrosecond);
            }
        }
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
