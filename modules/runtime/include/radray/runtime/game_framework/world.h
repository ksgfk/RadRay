#pragma once

#include <concepts>
#include <span>

#include <radray/types.h>

namespace radray {

class Actor;
namespace srp {
class Scene;
}

/// 顶层容器。管理所有 Actor，持有唯一的 Scene。
/// 对应 UE5 的 UWorld。
class World {
public:
    World();
    World(const World&) = delete;
    World(World&&) = delete;
    World& operator=(const World&) = delete;
    World& operator=(World&&) = delete;
    ~World() noexcept;

    template <class T = Actor, class... Args>
    requires std::derived_from<T, Actor>
    T* SpawnActor(Args&&... args) {
        auto actor = make_unique<T>(std::forward<Args>(args)...);
        T* ptr = actor.get();
        AddActorInternal(std::move(actor));
        return ptr;
    }

    void DestroyActor(Actor* actor);
    void Tick(float deltaTime);

    srp::Scene* GetScene() const noexcept { return _scene.get(); }
    std::span<const unique_ptr<Actor>> GetActors() const noexcept { return _actors; }

private:
    void AddActorInternal(unique_ptr<Actor> actor);

    unique_ptr<srp::Scene> _scene;
    vector<unique_ptr<Actor>> _actors;
};

}  // namespace radray
