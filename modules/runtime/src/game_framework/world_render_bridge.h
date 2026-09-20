#pragma once

#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render_scene/scene_writer.h>

namespace radray {

class World;
class RenderSystem;

/// World-owned GT adapter. It never owns flights, assets, or RT scene state.
class WorldRenderBridge {
public:
    WorldRenderBridge(World& world, RenderSystem& renderer);
    ~WorldRenderBridge() noexcept;
    WorldRenderBridge(const WorldRenderBridge&) = delete;
    WorldRenderBridge& operator=(const WorldRenderBridge&) = delete;

    void Initialize();
    void Disconnect();
    void Create(SceneComponent& component);
    void Destroy(SceneComponent& component);
    void Queue(SceneComponent& component, RenderDirtyFlag flag);
    void Collect();
    void CheckCanModify() const noexcept;
    SceneId GetSceneId() const noexcept { return _writer.GetSceneId(); }
    RenderConnectionState GetState() const noexcept { return _state; }

private:
    void Remove(SceneComponent& component) noexcept;

    World& _world;
    RenderSystem& _renderer;
    SceneWriter& _writer;
    vector<SceneComponent*> _updates;
    bool _collecting{false};
    RenderConnectionState _state{RenderConnectionState::Connecting};
};

}  // namespace radray
