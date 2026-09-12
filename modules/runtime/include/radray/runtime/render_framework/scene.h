#pragma once

#include <span>

#include <radray/types.h>
#include <radray/runtime/render_framework/cpu_draw_record.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>

namespace radray {

class SceneRenderState;
struct ScenePrimitiveChange {
    SceneObjectId Id{};
    PrimitiveDirtyFlags Dirty{};
};

struct SceneCommitStats {
    uint64_t DirtySlots{0};
    uint64_t LegacyProxiesObserved{0};
    uint64_t PendingResourcesObserved{0};
};

/// Game-thread proxy registry and unique long-term CPU render identity.
/// Registration transfers ownership; snapshots retain asset owners separately.
class Scene {
public:
    Scene();
    Scene(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) = delete;
    ~Scene() noexcept;

    Nullable<PrimitiveSceneProxy*> AddPrimitive(unique_ptr<PrimitiveSceneProxy> proxy);
    void RemovePrimitive(PrimitiveSceneProxy* proxy) noexcept;
    Nullable<LightSceneProxy*> AddLight(unique_ptr<LightSceneProxy> proxy);
    void RemoveLight(LightSceneProxy* proxy) noexcept;

    SceneObjectId GetPrimitiveId(const PrimitiveSceneProxy* proxy) const noexcept;
    Nullable<PrimitiveSceneProxy*> FindPrimitive(SceneObjectId id) const noexcept;
    SceneObjectId GetLightId(const LightSceneProxy* proxy) const noexcept;
    Nullable<LightSceneProxy*> FindLight(SceneObjectId id) const noexcept;

    void MarkRenderDirty(SceneObjectId id, PrimitiveDirtyFlags flags) noexcept;
    /// GT cutoff: apply the returned batch to the CPU catalog synchronously before more authoring.
    /// The same serial returns the same batch; subsequent mutations belong to the next serial.
    std::span<const ScenePrimitiveChange> BeginRenderCommit(uint64_t serial) const;
    /// A failed batch is merged into the next epoch; acknowledge only after CPU publication succeeds.
    bool CompleteRenderCommit(uint64_t serial, bool success) const noexcept;
    size_t GetPendingRenderChangeCount() const noexcept { return _dirtySlots.size(); }
    const SceneCommitStats& GetCommitStats() const noexcept { return _commitStats; }
    /// Registry containers only; draw-store, publication storage and authoring proxy objects are separate.
    RenderMemoryStats GetMemoryStats() const noexcept;
    CpuDrawStore& GetDrawStore() const noexcept { return *_draws; }
    SceneRenderState& GetRenderState() const noexcept { return *_renderState; }

    std::span<const unique_ptr<PrimitiveSceneProxy>> Primitives() const noexcept { return _primitiveProxies; }
    std::span<const unique_ptr<LightSceneProxy>> Lights() const noexcept { return _lightProxies; }

private:
    struct SlotTable {
        vector<uint32_t> Generation;
        vector<uint32_t> Packed;
        vector<uint32_t> Free;
        uint32_t Allocate();
        void Release(uint32_t slot) noexcept;
    };

    vector<unique_ptr<PrimitiveSceneProxy>> _primitiveProxies;
    vector<SceneObjectId> _primitiveIds;
    unordered_map<const PrimitiveSceneProxy*, SceneObjectId> _primitiveByPointer;
    SlotTable _primitiveSlots;

    struct RenderSlot {
        SceneObjectId PendingId{}, RemovedId{};
        PrimitiveDirtyFlags Pending{};
        uint32_t CommittedGeneration{0};
        uint64_t RenderRevision{0}, TransformRevision{0};
        bool Enqueued{false}, Observed{false};
    };
    void EnqueueRenderSlot(uint32_t slot) const;
    void ObserveRenderDependencies() const;
    mutable vector<RenderSlot> _renderSlots;
    mutable vector<uint32_t> _dirtySlots;
    mutable vector<SceneObjectId> _renderObservers;
    mutable vector<ScenePrimitiveChange> _committedChanges;
    mutable std::optional<uint64_t> _lastCommitSerial;
    mutable bool _commitComplete{true};
    mutable SceneCommitStats _commitStats;
    unique_ptr<CpuDrawStore> _draws;
    unique_ptr<SceneRenderState> _renderState;

    vector<unique_ptr<LightSceneProxy>> _lightProxies;
    vector<SceneObjectId> _lightIds;
    unordered_map<const LightSceneProxy*, SceneObjectId> _lightByPointer;
    SlotTable _lightSlots;
};

}  // namespace radray
