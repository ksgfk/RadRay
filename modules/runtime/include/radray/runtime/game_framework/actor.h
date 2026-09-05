#pragma once

#include <concepts>
#include <span>
#include <type_traits>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/runtime/components/actor_component.h>

namespace radray {

class World;
class SceneComponent;

template <class T>
concept ComponentQueryTarget =
    std::is_class_v<std::remove_cv_t<T>> &&
    std::same_as<T, std::remove_reference_t<T>> &&
    requires { sizeof(std::remove_cv_t<T>); };

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
        AddComponent(std::move(comp));
        return ptr;
    }

    ActorComponent* AddComponent(unique_ptr<ActorComponent> component);

    template <class T>
    requires ComponentQueryTarget<T>
    Nullable<T*> FindComponent() noexcept {
        for (const unique_ptr<ActorComponent>& component : _ownedComponents) {
            if (auto* result = dynamic_cast<T*>(component.get()); result != nullptr) {
                return result;
            }
        }
        return nullptr;
    }

    template <class T>
    requires ComponentQueryTarget<T>
    Nullable<const T*> FindComponent() const noexcept {
        for (const unique_ptr<ActorComponent>& component : _ownedComponents) {
            const ActorComponent* object = component.get();
            if (const auto* result = dynamic_cast<const T*>(object); result != nullptr) {
                return result;
            }
        }
        return nullptr;
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

    void RegisterAllComponents();
    void UnregisterAllComponents();

    Nullable<World*> _world{nullptr};
    Nullable<SceneComponent*> _rootComponent{nullptr};
    vector<unique_ptr<ActorComponent>> _ownedComponents;
};

}  // namespace radray
