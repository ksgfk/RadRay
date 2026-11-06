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
    // Bit i selects Batch.Elements[i]; nonexistent/empty elements are filtered out.
    uint32_t ElementMask{std::numeric_limits<uint32_t>::max()};
    uint32_t VisibilitySortKey{0};
    std::optional<uint32_t> FirstInstanceOverride{};
    std::optional<uint32_t> InstanceCountOverride{};

    static constexpr uint32_t MaskForElement(uint32_t index) noexcept {
        return index < 32 ? (uint32_t{1} << index) : 0;
    }
};

// See docs/architecture/runtime.md for the scene storage contract.
// Render-thread-owned storage; each Scene owns independent caches. External mutations must
// occur outside RunFrame. After cache synchronization, Extract/Record only borrow read-only data.
struct Scene {
    void Reserve(uint32_t primitives, uint32_t batches, uint32_t materials);
    Nullable<const PrimitiveSceneInfo*> GetPrimitive(SparseSetHandle handle) const noexcept;
    Nullable<const MeshBatch*> GetMeshBatch(SparseSetHandle handle) const noexcept;
    Nullable<const MaterialRenderProxy*> GetMaterial(SparseSetHandle handle) const noexcept;
    uint32_t GetPrimitiveCount() const noexcept { return _primitives.Count(); }
    uint32_t GetMeshBatchCount() const noexcept { return _meshes.Count(); }
    uint32_t GetMaterialCount() const noexcept { return _materials.Count(); }
    bool IsBatchDirty(SparseSetHandle handle) const noexcept;
    SparseSetHandle GetOwningPrimitive(SparseSetHandle handle) const noexcept;

    SparseSetHandle AddMaterial(MaterialRenderProxy proxy);
    bool UpdateMaterial(SparseSetHandle materialHandle, MaterialRenderProxy proxy);
    // Referenced materials cannot be removed; remove or retarget their batches first.
    bool RemoveMaterial(SparseSetHandle materialHandle);

    SparseSetHandle AddPrimitive(PrimitiveSceneInfo primitive = {});
    bool RemovePrimitive(SparseSetHandle primitiveHandle);
    bool UpdatePrimitiveTransform(SparseSetHandle primitiveHandle, const AffineTransform& transform);
    bool UpdatePrimitiveBounds(SparseSetHandle primitiveHandle, const Aabb& bounds);
    SparseSetHandle AddMeshBatch(SparseSetHandle primitiveHandle, MeshBatch batch);
    bool UpdateMeshBatch(SparseSetHandle batchHandle, MeshBatch batch);
    bool RemoveMeshBatch(SparseSetHandle batchHandle);
    bool RemoveMeshBatch(SparseSetHandle primitiveHandle, SparseSetHandle batchHandle);

    void MarkBatchDirty(SparseSetHandle batchHandle);
    void MarkMaterialDirty(SparseSetHandle materialHandle);
    bool HasDirtyBatches() const noexcept { return _dirtyCount != 0; }
    // Queue entries may include deleted handles until synchronization.
    std::span<const SparseSetHandle> GetDirtyBatches() const noexcept { return _dirtyBatches; }
    Nullable<const CachedPassMeshDrawList*> GetPassCache(MeshPassId pass) const noexcept;
    Nullable<const CachedPassMeshDrawList*> GetCachedDrawList(MeshPassId pass) const noexcept { return GetPassCache(pass); }

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
    void InvalidateBatchCachedCommands(uint32_t batchIndex) noexcept;
    void AddUniqueDirtyBatch(SparseSetHandle batchHandle);
    void UnlinkMaterial(SparseSetHandle batchHandle) noexcept;
    void LinkMaterial(SparseSetHandle batchHandle);
    bool ValidateBatch(const MeshBatch& batch) const noexcept;

    SparseSet<PrimitiveSceneInfo> _primitives;
    SparseSet<MeshBatch> _meshes;
    SparseSet<MaterialRenderProxy> _materials;
    vector<CachedPassMeshDrawList> _cachedDrawLists;
    vector<SparseSetHandle> _batchOwners;
    vector<uint8_t> _batchDirty;
    vector<uint32_t> _primitivePositions;
    vector<uint32_t> _materialPositions;
    vector<vector<SparseSetHandle>> _materialBatches;
    vector<SparseSetHandle> _dirtyBatches;
    vector<MeshPassRegistration> _registeredPasses;
    uint32_t _dirtyCount{0};
    uint32_t _reservedBatches{0};
    bool _initializedCache{false};
};

}  // namespace radray
