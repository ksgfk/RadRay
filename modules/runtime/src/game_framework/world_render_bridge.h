#pragma once

#include <radray/runtime/components/render_component.h>
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
    void Create(RenderComponent& component);
    void Destroy(RenderComponent& component);
    void CreateTransform(SceneComponent& component);
    void DestroyTransform(SceneComponent& component);
    void Queue(RenderComponent& component, RenderDirtyFlag flag);
    void Collect();
    SceneId GetSceneId() const noexcept { return _writer.GetSceneId(); }
    RenderSystem* GetRenderer() const noexcept { return &_renderer; }
    RenderConnectionState GetState() const noexcept { return _state; }

private:
    static constexpr size_t kCollectSortThreshold = 16384;
    static constexpr uint32_t kNotQueued = std::numeric_limits<uint32_t>::max();
    void RemoveUpdate(RenderComponent& component) noexcept;

    World& _world;
    RenderSystem& _renderer;
    SceneWriter& _writer;
    vector<RenderComponent*> _sources;
    vector<SceneComponent*> _transformSources;
    vector<SceneComponent*> _creationChain;
    vector<RenderComponent*> _updates;
    RenderConnectionState _state{RenderConnectionState::Connecting};
};

}  // namespace radray
