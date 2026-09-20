#include "scene_test_support.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <semaphore>
#include <thread>
#include <fmt/format.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc-stats.h>
#endif

namespace radray {
namespace {

unique_ptr<StaticMesh> MakeBenchmarkMesh() {
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

array<int64_t, 2> AllocationTotals() {
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_stats_t stats;
    mi_stats_init(&stats);
    if (mi_stats_get(&stats)) return {stats.malloc_normal_count.total + stats.malloc_huge_count.total,
                                      stats.malloc_normal.total + stats.malloc_huge.total};
#endif
    return {-1, -1};
}

double ElapsedMs(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void MeasureCase(uint32_t count, uint32_t flights, uint32_t depth, bool uniqueAssets, bool threaded) {
    Application app;
    AssetManager assets;
    RenderSystem renderer{&app, flights};
    test::ScopedWorld world;
    const auto sceneId = test::ConnectWorld(world, renderer);
    vector<StaticMeshComponent*> components;
    components.reserve(count);
    StreamingAssetRef<StaticMesh> shared;
    const auto setupAllocations = AllocationTotals();
    const auto setupStart = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < count; ++i) {
        if (uniqueAssets || !shared) shared = assets.AddReady<StaticMesh>(AssetId{i + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, MakeBenchmarkMesh());
        auto* actor = world.SpawnActor();
        Nullable<SceneComponent*> parent = i % depth == 0 ? nullptr : components.back();
        auto* component = actor->AddSceneComponent<StaticMeshComponent>(parent, AttachmentRule::KeepLocal);
        component->SetStaticMesh(shared);
        components.push_back(component);
    }
    const auto setupMs = ElapsedMs(setupStart);
    const auto afterSetupAllocations = AllocationTotals();
    const bool allocationsTracked = afterSetupAllocations[0] > setupAllocations[0];
    uint32_t flight = 0, views = 1;
    uint64_t serial = 0, visible = 0;
    bool stop = false;
    double applyMs = 0, viewMs = 0;
    std::binary_semaphore ready{0}, done{0};
    const auto apply = [&] {
        auto start = std::chrono::steady_clock::now();
        renderer.ConsumeRenderUpdates(flight, serial);
        applyMs = ElapsedMs(start);
        start = std::chrono::steady_clock::now();
        auto scene = renderer.GetSceneRT(sceneId);
        auto lease = scene->AcquireRead();
        visible = 0;
        for (uint32_t view = 0; view < views; ++view) {
            for (const auto id : scene->GetStaticMeshes()) {
                const auto mesh = scene->GetStaticMesh(id);
                if (mesh->WorldBoundsMax.x() >= -float(view + 1)) ++visible;
            }
        }
        viewMs = ElapsedMs(start);
        // Merge the worker's allocator statistics while GT is waiting at the handoff.
        if (threaded) (void)AllocationTotals();
    };
    std::thread worker;
    if (threaded) worker = std::thread{[&] { for (;;) { ready.acquire(); if (stop) break; apply(); done.release(); } }};
    const auto consume = [&] {
        renderer.PublishFrameGT(flight);
        serial = renderer.GetUpdateSequence(flight);
        if (threaded) {
            ready.release();
            done.acquire();
        } else
            apply();
    };
    for (flight = 0; flight < flights; ++flight) {
        test::PrepareScene(world, renderer, flight);
        consume();
        test::CompleteFrame(renderer, flight);
    }
    uint32_t iteration = 0;
    for (const uint32_t changes : {0u, 1u, count / 100, count}) {
        for (const uint32_t viewCount : {1u, 3u}) {
            views = viewCount;
            array<vector<double>, 6> samples;
            for (auto& sample : samples) sample.reserve(7);
            int64_t allocations = allocationsTracked ? 0 : -1, allocationBytes = allocationsTracked ? 0 : -1;
            uint64_t transformUpdates = 0;
            const uint32_t expectedTransforms = std::min(count, ((changes + depth - 1) / depth) * depth);
            const uint32_t warmup = flights + 1;
            for (uint32_t sample = 0; sample < warmup + 7; ++sample) {
                flight = iteration++ % flights;
                const auto allocBefore = AllocationTotals();
                const auto start = std::chrono::steady_clock::now();
                for (uint32_t i = 0; i < changes; ++i) components[i]->SetRelativeLocation({float(iteration), 0, 0});
                world.Tick(0);
                const auto tick = ElapsedMs(start);
                auto phase = std::chrono::steady_clock::now();
                world.FinalizeWorldGT();
                const auto lifecycle = ElapsedMs(phase);
                phase = std::chrono::steady_clock::now();
                world.CollectRenderUpdates();
                renderer.SealFrameGT(flight);
                const auto capture = ElapsedMs(phase);
                const auto& batch = test::SceneBatch(renderer, sceneId, flight);
                const auto updates = batch.Transforms.size();
                const bool onlyTransforms = batch.CreateShapes.empty() && batch.RemoveShapes.empty() &&
                                            batch.MeshStates.empty() && !batch.LightsChanged;
                consume();
                phase = std::chrono::steady_clock::now();
                test::CompleteFrame(renderer, flight);
                assets.Pump();
                const auto retirement = ElapsedMs(phase);
                const auto allocAfter = AllocationTotals();
                if (sample >= warmup) {
                    transformUpdates += updates;
                    const array values{tick, lifecycle, capture, applyMs, viewMs, retirement};
                    for (size_t k = 0; k < values.size(); ++k) samples[k].push_back(values[k]);
                    if (allocationsTracked) {
                        allocations += allocAfter[0] - allocBefore[0];
                        allocationBytes += allocAfter[1] - allocBefore[1];
                    }
                }
                EXPECT_EQ(visible, uint64_t{count} * views);
                EXPECT_TRUE(onlyTransforms);
                EXPECT_EQ(updates, expectedTransforms);
            }
            string times;
            for (auto& sample : samples) {
                std::sort(sample.begin(), sample.end());
                times += fmt::format(",{:.6f},{:.6f}", sample[3], sample[6]);
            }
            fmt::print("LIFECYCLE_PERF,{},{},{},{},{},{},{}{},{},{},{},{}\n", count, flights, depth, uniqueAssets, threaded, changes, views,
                       times, allocations, allocationBytes, transformUpdates, transformUpdates * sizeof(ShapeTransformUpdate));
        }
    }
    if (threaded) {
        stop = true;
        ready.release();
        worker.join();
    }
    // Time deletion separately and verify the surviving owners retain their order.
    for (const uint32_t removals : {1u, count / 100, count / 2}) {
        const size_t available = world.GetActors().size();
        const size_t removedCount = std::min<size_t>(removals, available);
        vector<ActorId> survivors;
        survivors.reserve(available - removedCount);
        for (size_t i = removedCount; i < available; ++i) survivors.push_back(world.GetActors()[i]->GetId());
        for (size_t i = 0; i < removedCount; ++i) world.DestroyActor(world.GetActors()[i].get());
        const auto start = std::chrono::steady_clock::now();
        world.FinalizeWorldGT();
        const auto lifecycleMs = ElapsedMs(start);
        EXPECT_EQ(world.GetActors().size(), survivors.size());
        for (size_t i = 0; i < std::min(survivors.size(), world.GetActors().size()); ++i) {
            EXPECT_EQ(world.GetActors()[i]->GetId(), survivors[i]);
        }
        world.CollectRenderUpdates();
        renderer.SealFrameGT(flight);
        const auto removedPrimitives = test::SceneBatch(renderer, sceneId, flight).RemoveShapes.size();
        EXPECT_EQ(removedPrimitives, removedCount);
        renderer.PublishFrameGT(flight);
        renderer.ConsumeRenderUpdates(flight, renderer.GetUpdateSequence(flight));
        const auto retirementStart = std::chrono::steady_clock::now();
        test::CompleteFrame(renderer, flight);
        assets.Pump();
        fmt::print("LIFECYCLE_DELETE,{},{},{},{},{},{},{:.6f},{:.6f},{},{},{}\n", count, flights, depth, uniqueAssets, threaded, removals, lifecycleMs, ElapsedMs(retirementStart),
                   available, world.GetActors().size(), removedPrimitives);
    }
    fmt::print("LIFECYCLE_SETUP,{},{},{},{},{},{:.6f},{},{},{}\n", count, flights, depth, uniqueAssets, threaded, setupMs,
               allocationsTracked ? afterSetupAllocations[0] - setupAllocations[0] : -1,
               allocationsTracked ? afterSetupAllocations[1] - setupAllocations[1] : -1, sizeof(Actor) + sizeof(StaticMeshComponent));
}

TEST(LifecyclePerformance, Matrix) {
    if (std::getenv("RADRAY_RUN_LIFECYCLE_BENCHMARK") == nullptr) GTEST_SKIP() << "Set RADRAY_RUN_LIFECYCLE_BENCHMARK=1 for the measured matrix";
    fmt::print("LIFECYCLE_PERF_HEADER,count,flights,depth,unique_assets,threaded,changes,views,tick_median_ms,tick_p95_ms,lifecycle_median_ms,lifecycle_p95_ms,capture_median_ms,capture_p95_ms,apply_median_ms,apply_p95_ms,view_median_ms,view_p95_ms,retirement_median_ms,retirement_p95_ms,allocations,allocation_bytes,transform_updates,transform_bytes\n");
    fmt::print("LIFECYCLE_DELETE_HEADER,count,flights,depth,unique_assets,threaded,removals,lifecycle_ms,retirement_ms,actors_before,actors_after,removed_primitives\n");
    fmt::print("LIFECYCLE_SETUP_HEADER,count,flights,depth,unique_assets,threaded,setup_ms,allocations,allocation_bytes,actor_component_bytes\n");
    for (uint32_t count : {10000u, 100000u})
        for (uint32_t flights : {1u, 2u, 3u})
            for (uint32_t depth : {1u, 8u})
                for (bool unique : {false, true})
                    for (bool threaded : {false, true}) MeasureCase(count, flights, depth, unique, threaded);
}

TEST(LifecycleScale, SharedMeshViewsAndTransformCapture) {
    for (uint32_t count : {10000u, 100000u}) {
        Application app;
        AssetManager assets;
        RenderSystem renderer{&app, 1};
        test::ScopedWorld world;
        const auto scene = test::ConnectWorld(world, renderer);
        const auto mesh = assets.AddReady<StaticMesh>(AssetId{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, MakeBenchmarkMesh());
        ASSERT_TRUE(mesh->IsValid());
        ASSERT_EQ(mesh->GetSections().size(), 1u);
        EXPECT_EQ(mesh->GetSections()[0].IndexCount, 3u);
        const auto* sections = mesh->GetSections().data();
        for (uint32_t i = 0; i < count; ++i) world.SpawnActor()->AddComponent<StaticMeshComponent>()->SetStaticMesh(mesh);
        test::PrepareScene(world, renderer, 0);
        EXPECT_EQ(test::SceneBatch(renderer, scene, 0).MeshStates.size(), count);
        for (const auto& state : test::SceneBatch(renderer, scene, 0).MeshStates) {
            EXPECT_EQ(state.Mesh.Sections.data(), sections);
            EXPECT_EQ(state.Mesh.RenderMesh.Get(), &mesh->GetRenderMesh());
        }
        test::ConsumeFrame(renderer, 0);
        test::CompleteFrame(renderer, 0);
        auto component = world.GetActors()[0]->FindComponent<StaticMeshComponent>();
        for (int i = 0; i < 100; ++i) component->SetRelativeLocation({float(i), 0, 0});
        test::PrepareScene(world, renderer, 0);
        ASSERT_EQ(test::SceneBatch(renderer, scene, 0).Transforms.size(), 1u);
        EXPECT_TRUE(test::SceneBatch(renderer, scene, 0).MeshStates.empty());
        EXPECT_EQ(mesh->GetSections().data(), sections);
        EXPECT_FLOAT_EQ(test::SceneBatch(renderer, scene, 0).Transforms[0].LocalToWorld(0, 3), 99);
    }
}

}  // namespace
}  // namespace radray
