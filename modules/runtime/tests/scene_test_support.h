#pragma once

#include <cstdlib>

#include <radray/runtime/application.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray::test {

inline const SceneUpdateBatch& SceneBatch(const RenderSystem& renderer, SceneId id, uint32_t flight) {
    for (const auto& entry : renderer.GetFrameUpdatesRT(flight)) {
        if (entry.Id == id) return entry.Updates;
    }
    RADRAY_ABORT("Missing test scene batch");
    std::abort();
}

inline void PrepareScene(World& world, RenderSystem& renderer, uint32_t flight) {
    world.CollectRenderUpdates();
    renderer.SealFrameGT(flight);
}

/// Captures a copy for isolated RenderScene assertions and completes the real transport.
inline void CollectScene(World& world, RenderSystem& renderer, SceneUpdateBatch& output) {
    PrepareScene(world, renderer, 0);
    output = SceneBatch(renderer, *world.GetRenderSceneId(), 0);
    renderer.ConsumeRenderUpdates(0);
    renderer.OnFlightCompletedGT({.FlightIndex = 0, .GpuWorkCompleted = true});
}

}  // namespace radray::test
