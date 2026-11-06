#pragma once

#include <limits>
#include <optional>
#include <span>

#include <radray/sparse_set.h>
#include <radray/types.h>
#include <radray/runtime/material.h>
#include <radray/runtime/mesh_batch.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/mesh_pass_processor.h>

namespace radray {

struct MeshPassFrameDrawList;

struct VisibleMeshBatch {
    SparseSetHandle Batch{};
    // The maximum value selects all elements in this pass.
    uint32_t ElementIndex{std::numeric_limits<uint32_t>::max()};
    uint32_t VisibilitySortKey{0};
    std::optional<uint32_t> FirstInstanceOverride{};
    std::optional<uint32_t> InstanceCountOverride{};
};

// See docs/architecture/runtime.md for the scene storage contract.
// Render-thread-owned storage; each Scene owns independent caches. External mutations must
// occur outside RunFrame. After cache synchronization, Extract/Record only borrow read-only data.
struct Scene {
    SparseSet<PrimitiveSceneInfo> Primitives;
    SparseSet<MeshBatch> Meshes;
    SparseSet<MaterialRenderProxy> Materials;
    vector<CachedPassMeshDrawList> CachedDrawLists;

    SparseSetHandle AddMaterial(MaterialRenderProxy proxy);
    bool UpdateMaterial(SparseSetHandle materialHandle, MaterialRenderProxy proxy);
    // Referenced materials cannot be removed; remove or retarget their batches first.
    bool RemoveMaterial(SparseSetHandle materialHandle);

    SparseSetHandle AddPrimitive(PrimitiveSceneInfo primitive = {});
    bool RemovePrimitive(SparseSetHandle primitiveHandle);
    SparseSetHandle AddMeshBatch(SparseSetHandle primitiveHandle, MeshBatch batch);
    bool UpdateMeshBatch(SparseSetHandle batchHandle, MeshBatch batch);
    bool RemoveMeshBatch(SparseSetHandle primitiveHandle, SparseSetHandle batchHandle);

    void MarkBatchDirty(SparseSetHandle batchHandle);
    void MarkMaterialDirty(SparseSetHandle materialHandle);
    bool HasDirtyBatches() const noexcept { return !_dirtyBatches.empty(); }
    std::span<const SparseSetHandle> GetDirtyBatches() const noexcept { return _dirtyBatches; }
    Nullable<const CachedPassMeshDrawList*> GetCachedDrawList(MeshPassId pass) const noexcept;

    // Pass IDs must be unique, compact indices. Registration changes rebuild all batches.
    // Processor default contexts are restored before returning. Calls must be serialized.
    void SyncCachedMeshDrawCommands(std::span<const MeshPassRegistration> passes);
    // Append to output; invalid input rolls back this call's appended commands.
    // Output borrows cache pointers: finish recording before mutating this Scene or its caches.
    bool GatherVisibleMeshDrawCommands(MeshPassId pass, std::span<const VisibleMeshBatch> visibleBatches,
                                       vector<VisibleMeshDrawCommand>& outCommands) const;
    bool GatherVisiblePrimitives(MeshPassId pass, std::span<const SparseSetHandle> visiblePrimitives,
                                 vector<VisibleMeshDrawCommand>& outCommands) const;
    // Append borrowed cached pointers to the frame list, with the same lifetime and rollback rules.
    // RHI objects remain borrowed through command allocator reset.
    bool GatherVisibleMeshDrawCommands(MeshPassId pass, std::span<const VisibleMeshBatch> visibleBatches,
                                       MeshPassFrameDrawList& outDrawList) const;
    bool GatherVisiblePrimitives(MeshPassId pass, std::span<const SparseSetHandle> visiblePrimitives,
                                 MeshPassFrameDrawList& outDrawList) const;
    void Clear() noexcept;

private:
    void InvalidateBatchCachedCommands(MeshBatch& batch) noexcept;
    SparseSetHandle FindOwningPrimitive(SparseSetHandle batchHandle) const noexcept;
    SparseSetHandle ResolveOwningPrimitive(SparseSetHandle batchHandle) noexcept;
    void AddUniqueDirtyBatch(SparseSetHandle batchHandle);
    bool ValidateBatch(const MeshBatch& batch) const noexcept;

    vector<SparseSetHandle> _dirtyBatches;
    vector<MeshPassRegistration> _registeredPasses;
    bool _initializedCache{false};
};

}  // namespace radray
