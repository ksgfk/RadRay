#pragma once

#include <concepts>
#include <optional>
#include <span>
#include <thread>

#include <radray/runtime_type.h>
#include <radray/types.h>
#include <radray/sparse_set.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render_scene/scene_id.h>

namespace radray {

class Application;
class Actor;
class RenderSystem;
class WorldRenderBridge;
class WorldManager;
class RenderComponent;
enum class RenderDirtyFlag : uint8_t;

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
    requires std::derived_from<T, Actor> && std::constructible_from<T, Args...>
    T* SpawnActor(Args&&... args) {
        unique_ptr<Actor> actor = make_unique<T>(std::forward<Args>(args)...);
        return static_cast<T*>(SpawnActor(std::move(actor)));
    }

    LifecycleRequestResult DestroyActor(Actor* actor);
    LifecycleRequestResult DestroyActor(ActorId actor);
    Nullable<Actor*> FindLive(ActorId id) const noexcept;
    Nullable<ActorComponent*> FindLive(ComponentId id) const noexcept;
    WorldId GetId() const noexcept { return _id; }
    bool IsLive() const noexcept { return _lifecycle == ObjectLifecycle::Live; }
    ObjectLifecycle GetLifecycle() const noexcept { return _lifecycle; }
    uint64_t GetCurrentTickEpoch() const noexcept;
    /// Explicit CPU-only driver. Managed Worlds are driven by WorldManager.
    void Tick(float deltaTime);
    void FinalizeWorldGT();
    void ShutdownWorld();

    void SetTickEnabled(bool enabled) noexcept {
        CheckCanModify();
        _tickEnabled = enabled;
    }
    bool IsTickEnabled() const noexcept { return _tickEnabled; }
    void CheckCanModify() const noexcept;
    Nullable<Application*> GetApplication() const noexcept { return _app; }
    std::optional<SceneId> GetRenderSceneId() const noexcept;
    LifecycleRequestResult RequestRenderConnection(Nullable<RenderSystem*> renderer);
    LifecycleRequestResult RequestReconnect();
    RenderConnectionState GetRenderConnectionState() const noexcept;
    Nullable<RenderSystem*> GetRequestedRenderConnection() const noexcept;
    /// Explicit CPU-only collection; managed Worlds collect through their driver.
    void CollectRenderUpdates();

    std::span<const unique_ptr<Actor>> GetActors() const noexcept { return _actors; }

private:
    friend class Actor;
    friend class SceneComponent;
    friend class RenderComponent;
    friend class WorldRenderBridge;
    friend class WorldManager;
    friend class ActorComponent;

    struct ReparentRequest {
        ComponentId Child;
        std::optional<ComponentId> Parent;
        AttachmentRule Rule;
    };
    struct RootRequest {
        ActorId Actor;
        std::optional<ComponentId> Root;
    };
    struct ConnectionRequest {
        Nullable<RenderSystem*> Target{nullptr};
        bool Reconnect{false};
    };
    struct LifecycleBatch {
        vector<ActorId> Actors;
        vector<ComponentId> Components;
        vector<ReparentRequest> Reparents;
        vector<RootRequest> Roots;
        std::optional<ConnectionRequest> Connection;
        bool Empty() const noexcept { return Actors.empty() && Components.empty() && Reparents.empty() && Roots.empty() && !Connection; }
        void Clear() noexcept {
            Actors.clear();
            Components.clear();
            Reparents.clear();
            Roots.clear();
            Connection.reset();
        }
    };

    void CheckDriverIdle() const noexcept;
    void BeginCallback() noexcept;
    void EndCallback() noexcept;
    void DispatchTick(float deltaTime, uint64_t epoch);
    void FreezeLifecycle();
    void PrepareLifecycle();
    void ExecuteLifecycle();
    void Teardown();
    void Collect();
    void DispatchTransforms(bool notify);
    void QueueTransform(SceneComponent& component);
    void RemoveTransform(SceneComponent& component) noexcept;
    void DisconnectNow();
    void QueueComponentDestruction(ActorComponent& component);
    LifecycleRequestResult QueueReparent(SceneComponent& child, Nullable<SceneComponent*> parent, AttachmentRule rule);
    Nullable<Actor*> ResolveIncludingPending(ActorId id) const noexcept;

    void CreateComponentRenderState(RenderComponent& component);
    void DestroyComponentRenderState(RenderComponent& component);
    void EnqueueRenderDirty(RenderComponent& component, RenderDirtyFlag flag);
    void AddTicking(Actor& actor);
    void RemoveTicking(Actor& actor);
    void RefreshTicking(Actor& actor);
    void CompactTicking();
    bool ShouldBeOnTickingList(const Actor& actor) const noexcept;

    Nullable<Application*> _app{nullptr};
    Nullable<WorldManager*> _manager{nullptr};
    WorldId _id;
    vector<unique_ptr<Actor>> _actors;
    vector<Actor*> _tickingActors;
    SparseSet<Actor*> _actorIds;
    unique_ptr<WorldRenderBridge> _renderBridge;
    LifecycleBatch _pending;
    LifecycleBatch _executing;
    vector<unique_ptr<Actor>> _retiredActors;
    vector<unique_ptr<ActorComponent>> _retiredComponents;
    vector<SceneComponent*> _transformChanges;
    vector<SceneComponent*> _transformRoots;
    mutable vector<const SceneComponent*> _transformChain;
    ObjectLifecycle _lifecycle{ObjectLifecycle::Live};
    std::thread::id _ownerThread{std::this_thread::get_id()};
    uint64_t _tickEpoch{0};
    uint64_t _transformRevision{1};
    uint64_t _renderTransformRevision{0};
    uint64_t _firstTickEpoch{1};
    uint32_t _callbackDepth{0};
    bool _ticking{false};
    bool _committing{false};
    bool _collecting{false};
    bool _stopping{false};
    bool _tickEnabled{true};
    bool _tickingStale{false};
};

template <>
struct RuntimeTypeTrait<World> {
    static constexpr RuntimeTypeId value{0x95e6ee0d, 0xb66e, 0x4ab4, 0x9b, 0xba, 0xb6, 0x37, 0xf6, 0xee, 0xe3, 0x62};
};

}  // namespace radray
