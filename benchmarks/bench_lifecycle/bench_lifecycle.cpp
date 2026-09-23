// Measurement boundaries and invocation: docs/guide/build-test.md
#include <benchmark/benchmark.h>
#include "scene_sync_workload.h"
#include "scene_test_support.h"

#include <fmt/format.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>

namespace radray {
namespace {

unique_ptr<StaticMesh> MakeMesh() {
    const array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource mesh;
    mesh.Bins.emplace_back(std::as_bytes(std::span{positions}));
    mesh.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 3, 0, 4};
    mesh.Primitives.push_back(std::move(primitive));
    return make_unique<StaticMesh>(std::move(mesh), vector<StaticMeshSection>{}, Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), GpuMesh{});
}

class LifecycleWorkload {
public:
    LifecycleWorkload() : Renderer(&App, 1) { test::ConnectWorld(World, Renderer); }

    void Populate(uint32_t count, uint32_t depth, bool unique) {
        Nullable<SceneComponent*> previous;
        StreamingAssetRef<StaticMesh> shared;
        for (uint32_t i = 0; i < count; ++i) {
            if (unique || !shared) shared = Assets.AddReady<StaticMesh>(AssetId{i + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, MakeMesh());
            auto* actor = World.SpawnActor();
            auto* component = actor->AddSceneComponent<StaticMeshComponent>(i % depth == 0 ? nullptr : previous, AttachmentRule::KeepLocal);
            component->SetStaticMesh(shared);
            previous = component;
        }
    }

    void Sync() {
        test::PrepareScene(World, Renderer, 0);
        test::ConsumeFrame(Renderer, 0);
        test::CompleteFrame(Renderer, 0, false);
        Assets.Pump();
    }

    void Remove(uint32_t count) {
        const auto actors = World.GetActors();
        for (uint32_t i = 0; i < count; ++i) World.DestroyActor(actors[i].get());
        Sync();
    }

    Application App;
    AssetManager Assets;
    RenderSystem Renderer;
    test::ScopedWorld World;
};

void RegisterLifecycleBenchmarks() {
    for (uint32_t count : {10000u, 100000u}) {
        for (uint32_t depth : {1u, 8u}) {
            for (bool unique : {false, true}) {
                const auto suffix = fmt::format("count:{}/depth:{}/unique:{}", count, depth, unique);
                benchmark::RegisterBenchmark(fmt::format("Lifecycle/CreateActors/{}", suffix).c_str(), [=](benchmark::State& state) {
                    for (auto _ : state) {
                        state.PauseTiming();
                        auto workload = make_unique<LifecycleWorkload>();
                        state.ResumeTiming();
                        workload->Populate(count, depth, unique);
                        benchmark::DoNotOptimize(workload->World.GetActors().data());
                        state.PauseTiming();
                        workload.reset();
                        state.ResumeTiming();
                    }
                    state.SetItemsProcessed(state.iterations() * count);
                })->Unit(benchmark::kMicrosecond);
                for (uint32_t removed : {1u, count / 100, count / 2}) {
                    benchmark::RegisterBenchmark(fmt::format("Lifecycle/DeleteAndSync/{}/removed:{}", suffix, removed).c_str(), [=](benchmark::State& state) {
                        for (auto _ : state) {
                            state.PauseTiming();
                            auto workload = make_unique<LifecycleWorkload>();
                            workload->Populate(count, depth, unique);
                            workload->Sync();
                            state.ResumeTiming();
                            workload->Remove(removed);
                            benchmark::DoNotOptimize(workload->World.GetActors().data());
                            state.PauseTiming();
                            workload.reset();
                            state.ResumeTiming();
                        }
                        state.SetItemsProcessed(state.iterations() * removed);
                    })->Unit(benchmark::kMicrosecond);
                }
                for (uint32_t flights : {1u, 2u, 3u}) {
                    for (bool threaded : {false, true}) {
                        for (uint32_t changes : {0u, 1u, count / 100, count}) {
                            for (uint32_t views : {1u, 3u}) {
                                const auto name = fmt::format("Lifecycle/Frame/{}/F{}/{}/changes:{}/views:{}", suffix, flights, threaded ? "threaded" : "single", changes, views);
                                const test::Scenario scenario{.Name = name, .Shapes = count, .Changes = changes, .Depth = depth, .Sequential = true, .UniqueAssets = unique, .Views = views};
                                benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                                    test::SceneSyncWorkload workload{scenario, flights, threaded};
                                    constexpr int64_t batchFrames = 256;
                                    while (state.KeepRunningBatch(batchFrames)) workload.RunFrames(batchFrames);
                                    benchmark::DoNotOptimize(workload.Visible());
                                    state.SetItemsProcessed(state.iterations());
                                })->MeasureProcessCPUTime()->UseRealTime()->Unit(benchmark::kMicrosecond);
                            }
                        }
                    }
                }
            }
        }
    }
}

}  // namespace
}  // namespace radray

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    radray::RegisterLifecycleBenchmarks();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
