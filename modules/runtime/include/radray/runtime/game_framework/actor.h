#pragma once

#include <concepts>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>

namespace radray {

class World;
class ActorComponent;
class SceneComponent;

/// 场景中的实体。不直接持有 Transform —— 空间信息由 RootComponent 提供。
/// 对应 UE5 的 AActor。
class Actor {
public:
    Actor() noexcept = default;
    Actor(const Actor&) = delete;
    Actor(Actor&&) = delete;
    Actor& operator=(const Actor&) = delete;
    Actor& operator=(Actor&&) = delete;
    virtual ~Actor() noexcept;

    // ─── Component 管理 ───

    /// 创建组件并加入此 Actor 的 OwnedComponents。
    /// 如果 Actor 已在 World 中，立即调用 OnRegister。
    template <class T, class... Args>
    requires std::derived_from<T, ActorComponent>
    T* AddComponent(Args&&... args) {
        auto comp = make_unique<T>(std::forward<Args>(args)...);
        T* ptr = comp.get();
        AddComponentInternal(std::move(comp));
        return ptr;
    }

    void RemoveComponent(ActorComponent* component);

    /// 设置 RootComponent。传 nullptr 清除。必须是本 Actor 拥有的 SceneComponent。
    void SetRootComponent(Nullable<SceneComponent*> component) noexcept;
    Nullable<SceneComponent*> GetRootComponent() const noexcept { return _rootComponent; }

    // ─── 生命周期 ───

    virtual void Tick(float deltaTime);

    Nullable<World*> GetWorld() const noexcept { return _world; }
    std::span<const unique_ptr<ActorComponent>> GetOwnedComponents() const noexcept { return _ownedComponents; }

protected:
    virtual void OnSpawned() {}
    virtual void OnDestroyed() {}

private:
    friend class World;

    void AddComponentInternal(unique_ptr<ActorComponent> component);
    void RegisterAllComponents();
    void UnregisterAllComponents();

    Nullable<World*> _world{nullptr};
    Nullable<SceneComponent*> _rootComponent{nullptr};
    vector<unique_ptr<ActorComponent>> _ownedComponents;
};

}  // namespace radray
