#pragma once

#include <concepts>
#include <span>

#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

class Application;
class Actor;
class Scene;

/// 顶层容器。管理所有 Actor，持有唯一的 Scene。
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
        return static_cast<T*>(SpawnActor(make_unique<T>(std::forward<Args>(args)...)));
    }

    void DestroyActor(Actor* actor);
    void Tick(float deltaTime);

    Scene* GetScene() const noexcept { return _scene; }
    Application* GetApplication() const noexcept { return _app; }

    std::span<const unique_ptr<Actor>> GetActors() const noexcept { return _actors; }

private:
    void ReleaseScene() noexcept;

    Application* _app{nullptr};
    Scene* _scene{nullptr};
    vector<unique_ptr<Actor>> _actors;
};

template <>
struct RuntimeTypeTrait<World> {
    static constexpr RuntimeTypeId value{0x95e6ee0d, 0xb66e, 0x4ab4, 0x9b, 0xba, 0xb6, 0x37, 0xf6, 0xee, 0xe3, 0x62};
};

}  // namespace radray
