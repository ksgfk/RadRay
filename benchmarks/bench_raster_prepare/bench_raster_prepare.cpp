#include "bench_raster_prepare.h"
#include <benchmark/benchmark.h>
#include <radray/runtime/render_framework/raster_graph.h>
#include <radray/runtime/render_framework/raster_scene.h>

namespace radray::benchmarking {
void RegisterRasterPrepareBenchmarks() {
    benchmark::RegisterBenchmark("RasterGraph/ConstructCompile", [](benchmark::State& state) {
        const auto count = static_cast<uint32_t>(state.range(0));
        for (auto _ : state) {
            RasterGraph graph;
            const auto first = graph.AddResource({}), second = graph.AddResource({});
            graph.AddNode({RasterNodeKind::Clear, {{first, RasterAccess::Write, render::TextureState::RenderTarget}}, {}, {}});
            for (uint32_t i = 0; i < count; ++i) {
                const auto source = i % 2 ? second : first, target = i % 2 ? first : second;
                graph.AddNode({RasterNodeKind::Fullscreen, {{source, RasterAccess::Read, render::TextureState::ShaderRead}, {target, RasterAccess::Write, render::TextureState::RenderTarget}}, {}, {}});
            }
            auto result = graph.Compile();
            benchmark::DoNotOptimize(result);
        }
        state.SetItemsProcessed(state.iterations() * count);
    })->Arg(6)
        ->Arg(64)
        ->Arg(512)
        ->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark("RasterScene/Bounds", [](benchmark::State& state) {
        const Eigen::Vector3d camera{30000000.125, -30000000.25, 0};
        const Eigen::Vector3d min = camera + Eigen::Vector3d{-.5, -.5, .1}, max = camera + Eigen::Vector3d{.5, .5, .9};
        const Eigen::Matrix4f clip = Eigen::Matrix4f::Identity();
        for (auto _ : state) benchmark::DoNotOptimize(RasterBoundsVisible(min, max, camera, clip));
        state.SetItemsProcessed(state.iterations());
    })->Unit(benchmark::kNanosecond);
    benchmark::RegisterBenchmark("RasterScene/SortGroups", [](benchmark::State& state) {
        const auto count = static_cast<uint32_t>(state.range(0));
        vector<RasterSortGroup> groups;
        vector<uint32_t> source(count * 3), target(count * 3);
        for (uint32_t i = 0; i < count; ++i) {
            groups.push_back({i * 3, 3, {double(i % 23), double(i % 7), double(i)}, 0});
            for (uint32_t j = 0; j < 3; ++j) source[i * 3 + j] = i * 3 + j;
        }
        const Eigen::Matrix4d transform = Eigen::Matrix4d::Identity();
        const Eigen::Vector3d camera = Eigen::Vector3d::Zero();
        for (auto _ : state) benchmark::DoNotOptimize(SortRasterIndices(source, groups, transform, camera, false, target));
        state.SetItemsProcessed(state.iterations() * count);
        state.SetBytesProcessed(state.iterations() * source.size() * 4);
    })->Arg(64)
        ->Arg(1024)
        ->Unit(benchmark::kMicrosecond);
}
}  // namespace radray::benchmarking
int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    radray::benchmarking::RegisterRasterPrepareBenchmarks();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
