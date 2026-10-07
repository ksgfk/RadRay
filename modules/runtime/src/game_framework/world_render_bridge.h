#pragma once

#include <radray/runtime/components/render_component.h>
#include <radray/runtime/render_framework/scene_writer.h>

namespace radray {

class World;
class SceneManager;

/// World-owned GT adapter. It never owns flights, assets, or RT scene state.
class WorldRenderBridge {
public:
    WorldRenderBridge(World& world, SceneManager& scenes);
    ~WorldRenderBridge() noexcept;
    WorldRenderBridge(const WorldRenderBridge&) = delete;
    WorldRenderBridge& operator=(const WorldRenderBridge&) = delete;

    void Initialize();
    void Disconnect();
    void Create(RenderComponent& component);
    void Destroy(RenderComponent& component);
    void CreateTransform(SceneComponent& component);
    void DestroyTransform(SceneComponent& component);
    void ReparentTransform(SceneComponent& component);
    void Queue(RenderComponent& component, RenderDirtyFlag flag);
    void Collect();
    SceneId GetSceneId() const noexcept { return _writer.GetSceneId(); }
    SceneManager* GetSceneManager() const noexcept { return &_scenes; }
    RenderConnectionState GetState() const noexcept { return _state; }

private:
    static constexpr size_t kCollectSortThreshold = 16384;
    static constexpr uint32_t kNotQueued = std::numeric_limits<uint32_t>::max();
    void RemoveUpdate(RenderComponent& component) noexcept;

    World& _world;
    SceneManager& _scenes;
    SceneWriter& _writer;
    vector<RenderComponent*> _sources;
    vector<SceneComponent*> _transformSources;
    vector<SceneComponent*> _creationChain;
    vector<RenderComponent*> _updates;
    RenderConnectionState _state{RenderConnectionState::Connecting};
};

}  // namespace radray
