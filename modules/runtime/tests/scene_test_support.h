#pragma once

#include <cstdlib>

#include <radray/runtime/application.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/world_manager.h>

namespace radray::test {

/// CPU test drivers explicitly finish the same protocol before destroying their World.
class ScopedWorld : public World {
public:
    using World::World;
    ~ScopedWorld() noexcept { ShutdownWorld(); }
};

class ScopedWorldManager : public WorldManager {
public:
    using WorldManager::WorldManager;
    ~ScopedWorldManager() noexcept { Shutdown(); }
};

inline SceneId ConnectWorld(World& world, RenderSystem& renderer) {
    world.RequestRenderConnection(&renderer);
    world.FinalizeWorldGT();
    return *world.GetRenderSceneId();
}

inline void DisconnectWorld(World& world) {
    world.RequestRenderConnection(nullptr);
    world.FinalizeWorldGT();
}

inline void ConsumeFrame(RenderSystem& renderer, uint32_t flight) {
    renderer.PublishFrameGT(flight);
    renderer.ConsumeRenderUpdates(flight, renderer.GetUpdateSequence(flight));
}

inline void CompleteFrame(RenderSystem& renderer, uint32_t flight, bool workCompleted = true) {
    renderer.OnFlightCompletedGT({.FlightIndex = flight, .GpuWorkCompleted = workCompleted, .FrameSerial = renderer.GetFrameSerial(flight)});
}

inline const SceneUpdateBatch& SceneBatch(const RenderSystem& renderer, SceneId id, uint32_t flight) {
    for (const auto& entry : renderer.GetFrameUpdatesRT(flight)) {
        if (entry.Id == id) return entry.Updates;
    }
    RADRAY_ABORT("Missing test scene batch");
    std::abort();
}

inline void PrepareScene(World& world, RenderSystem& renderer, uint32_t flight) {
    world.FinalizeWorldGT();
    world.CollectRenderUpdates();
    renderer.SealFrameGT(flight);
}

/// Captures a copy for isolated RenderScene assertions and completes the real transport.
inline void CollectScene(World& world, RenderSystem& renderer, SceneUpdateBatch& output) {
    PrepareScene(world, renderer, 0);
    output = SceneBatch(renderer, *world.GetRenderSceneId(), 0);
    ConsumeFrame(renderer, 0);
    CompleteFrame(renderer, 0);
}

}  // namespace radray::test
