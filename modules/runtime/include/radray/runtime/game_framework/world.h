#pragma once

#include <concepts>
#include <optional>
#include <span>

#include <radray/runtime_type.h>
#include <radray/types.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render_scene/scene_id.h>

namespace radray {

class Application;
class Actor;
class RenderSystem;
class WorldRenderBridge;

/// 顶层容器。管理所有 Actor 及其组件生命周期。
/// 对应 UE5 的 UWorld。
class World {
public:
    World();
    explicit World(Application* app);
    World(const World&) = delete;
    World(World&&) = delete;
    World& operator=(const World&) = delete;
    World& operator=(World&&) = delete;
    ~World() noexcept;

    Actor* SpawnActor(unique_ptr<Actor> actor);

    template <class T = Actor, class... Args>
    requires std::derived_from<T, Actor>
    T* SpawnActor(Args&&... args) {
        unique_ptr<Actor> actor = make_unique<T>(std::forward<Args>(args)...);
        return static_cast<T*>(SpawnActor(std::move(actor)));
    }

    void DestroyActor(Actor* actor);
    void Tick(float deltaTime);

    void SetTickEnabled(bool enabled) noexcept {
        CheckCanModify();
        _tickEnabled = enabled;
    }
    bool IsTickEnabled() const noexcept { return _tickEnabled; }
    void CheckCanModify() const noexcept;
    Nullable<Application*> GetApplication() const noexcept { return _app; }
    std::optional<SceneId> GetRenderSceneId() const noexcept;
    /// GT only. Connection changes do not invoke game registration callbacks.
    SceneId AttachToRendering(RenderSystem& renderer);
    void DetachFromRendering();
    void CollectRenderUpdates();

    std::span<const unique_ptr<Actor>> GetActors() const noexcept { return _actors; }

private:
    friend class Actor;
    friend class SceneComponent;
    friend class WorldRenderBridge;

    void CreateComponentRenderState(SceneComponent& component);
    void DestroyComponentRenderState(SceneComponent& component);
    void QueueRenderUpdate(SceneComponent& component, RenderDirtyFlag flag);

    Nullable<Application*> _app{nullptr};
    vector<unique_ptr<Actor>> _actors;
    unique_ptr<WorldRenderBridge> _renderBridge;
    bool _tickEnabled{true};
};

template <>
struct RuntimeTypeTrait<World> {
    static constexpr RuntimeTypeId value{0x95e6ee0d, 0xb66e, 0x4ab4, 0x9b, 0xba, 0xb6, 0x37, 0xf6, 0xee, 0xe3, 0x62};
};

}  // namespace radray
