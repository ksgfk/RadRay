#pragma once

#include <concepts>
#include <limits>
#include <span>
#include <type_traits>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/sparse_set.h>
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
    requires std::derived_from<T, ActorComponent> && std::constructible_from<T, Args...>
    T* AddComponent(Args&&... args) {
        auto comp = make_unique<T>(std::forward<Args>(args)...);
        return static_cast<T*>(AddComponent(std::move(comp)));
    }

    ActorComponent* AddComponent(unique_ptr<ActorComponent> component);
    /// The initial edge is established before registration or render callbacks.
    ActorComponent* AddComponent(unique_ptr<ActorComponent> component, Nullable<SceneComponent*> initialParent, AttachmentRule rule);

    template <class T, class... Args>
    requires std::derived_from<T, SceneComponent>
    T* AddSceneComponent(Nullable<SceneComponent*> initialParent, AttachmentRule rule, Args&&... args) {
        return static_cast<T*>(AddComponent(make_unique<T>(std::forward<Args>(args)...), initialParent, rule));
    }

    template <class T>
    requires ComponentQueryTarget<T>
    Nullable<T*> FindComponent() noexcept {
        for (const unique_ptr<ActorComponent>& component : _ownedComponents) {
            if (!component->IsLive()) continue;
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
            if (!component->IsLive()) continue;
            const ActorComponent* object = component.get();
            if (const auto* result = dynamic_cast<const T*>(object); result != nullptr) {
                return result;
            }
        }
        return nullptr;
    }

    LifecycleRequestResult RemoveComponent(ActorComponent* component);
    Nullable<ActorComponent*> FindLive(ComponentId id) const noexcept;

    /// 设置 RootComponent。传 nullptr 清除。必须是本 Actor 拥有的 SceneComponent。
    void SetRootComponent(Nullable<SceneComponent*> component) noexcept;
    LifecycleRequestResult RequestSetRootComponent(Nullable<SceneComponent*> component);
    Nullable<SceneComponent*> GetRootComponent() const noexcept { return _rootComponent; }

    // ─── 生命周期 ───

    /// User hook. Component dispatch is owned by World and follows this hook.
    /// Default is not scheduled; overrides must `SetTickEnabled(true)`.
    virtual void Tick(float deltaTime) { (void)deltaTime; }

    void SetTickEnabled(bool enabled) noexcept;
    bool IsTickEnabled() const noexcept { return _tickEnabled; }

    ActorId GetId() const noexcept { return _id; }
    ObjectLifecycle GetLifecycle() const noexcept { return _lifecycle; }
    bool IsLive() const noexcept;
    uint64_t GetFirstTickEpoch() const noexcept { return _firstTickEpoch; }

    Nullable<World*> GetWorld() const noexcept { return _world; }
    std::span<const unique_ptr<ActorComponent>> GetOwnedComponents() const noexcept { return _ownedComponents; }

protected:
    virtual void OnSpawned() {}
    virtual void OnDestroyed() {}

private:
    friend class World;
    friend class WorldManager;
    friend class ActorComponent;

    void RegisterComponent(ActorComponent& component);
    void UnregisterComponent(ActorComponent& component);
    void RegisterAllComponents();
    void DispatchTick(float deltaTime, uint64_t epoch);
    void NoteTickingComponent(int32_t delta) noexcept;
    /// S1 preparation only: invalidate identities and transfer owners; no hooks or hierarchy changes.
    void PrepareComponentDestruction(std::span<const ComponentId> ids, vector<unique_ptr<ActorComponent>>& retired);
    void PrepareComponentTeardown() noexcept;
    /// Called only after World has marked this Actor and its components Destroying.
    void Teardown();
    Nullable<ActorComponent*> ResolveIncludingPending(ComponentId id) const noexcept;

    Nullable<World*> _world{nullptr};
    Nullable<SceneComponent*> _rootComponent{nullptr};
    vector<unique_ptr<ActorComponent>> _ownedComponents;
    SparseSet<ActorComponent*> _componentIds;
    ActorId _id;
    ObjectLifecycle _lifecycle{ObjectLifecycle::Initializing};
    uint64_t _firstTickEpoch{0};
    size_t _tickingIndex{std::numeric_limits<size_t>::max()};
    uint32_t _tickingComponents{0};
    bool _spawned{false};
    bool _tickEnabled{false};
};

}  // namespace radray
