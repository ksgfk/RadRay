#pragma once

#include <concepts>
#include <span>

#include <radray/runtime_type.h>
#include <radray/types.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {

class Application;
class Actor;

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
        return static_cast<T*>(SpawnActor(make_unique<T>(std::forward<Args>(args)...)));
    }

    void DestroyActor(Actor* actor);
    void Tick(float deltaTime);

    /// GT only, after Tick. The batch must be empty and must be delivered in order.
    void FlushRenderUpdates(SceneUpdateBatch& batch);
    /// GT, after Flush and before publish. Finds each used asset once; never starts loads.
    void RetainRenderAssets(Nullable<AssetManager*> assets, vector<StreamingAssetRef<StaticMesh>>& refs) const;
    /// Collection callbacks may read the World but must not mutate it.
    void CheckCanModify() const noexcept;

    Application* GetApplication() const noexcept { return _app; }

    std::span<const unique_ptr<Actor>> GetActors() const noexcept { return _actors; }

private:
    friend class Actor;
    friend class SceneComponent;
    friend class PrimitiveComponent;
    friend class StaticMeshComponent;

    void QueueRenderUpdate(SceneComponent& component, RenderDirtyFlag flag);
    void RemoveRenderUpdate(SceneComponent& component) noexcept;
    PrimitiveId AllocatePrimitiveId();
    void ReleasePrimitiveId(PrimitiveId id);
    void UpdateRenderAssetUse(std::optional<AssetId> previous, Nullable<const StaticMesh*> next);

    struct RenderAssetUse {
        size_t Count;
        const StaticMesh* Mesh;
    };

    Application* _app{nullptr};
    vector<unique_ptr<Actor>> _actors;
    vector<SceneComponent*> _renderUpdates;
    vector<PrimitiveId> _removedPrimitives;
    vector<uint32_t> _primitiveGenerations;
    vector<uint32_t> _freePrimitiveIndices;
    unordered_map<AssetId, RenderAssetUse> _renderAssetUses;
    bool _isFlushingRenderUpdates{false};
};

template <>
struct RuntimeTypeTrait<World> {
    static constexpr RuntimeTypeId value{0x95e6ee0d, 0xb66e, 0x4ab4, 0x9b, 0xba, 0xb6, 0x37, 0xf6, 0xee, 0xe3, 0x62};
};

}  // namespace radray
