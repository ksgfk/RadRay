#pragma once

// 场景存储契约见 docs/architecture/runtime.md。

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

class MeshPassFrameDrawList;

class VisibleMeshBatch {
public:
    MeshBatchHandle Batch{};
    // 第 i 位选择 Batch.Elements[i]；不存在或为空的元素会被过滤。
    uint32_t ElementMask{std::numeric_limits<uint32_t>::max()};
    uint32_t VisibilitySortKey{0};
    std::optional<uint32_t> FirstInstanceOverride{};
    std::optional<uint32_t> InstanceCountOverride{};

    static constexpr uint32_t MaskForElement(uint32_t index) noexcept {
        return index < 32 ? (uint32_t{1} << index) : 0;
    }
};

// 由渲染线程拥有的存储；每个 Scene 拥有独立缓存；外部修改必须发生在 RunFrame 之外。
// 缓存同步后，Extract/Prepare/Record 仅借用只读数据。
class Scene {
public:
    void Reserve(uint32_t primitives, uint32_t batches, uint32_t materials);
    Nullable<const PrimitiveSceneInfo*> GetPrimitive(PrimitiveHandle handle) const noexcept;
    Nullable<const MeshBatch*> GetMeshBatch(MeshBatchHandle handle) const noexcept;
    Nullable<const MaterialRenderProxy*> GetMaterial(MaterialHandle handle) const noexcept;
    uint32_t GetPrimitiveCount() const noexcept { return _primitives.Count(); }
    uint32_t GetMeshBatchCount() const noexcept { return _meshes.Count(); }
    uint32_t GetMaterialCount() const noexcept { return _materials.Count(); }
    bool IsBatchDirty(MeshBatchHandle handle) const noexcept;
    PrimitiveHandle GetOwningPrimitive(MeshBatchHandle handle) const noexcept;

    MaterialHandle AddMaterial(MaterialRenderProxy proxy);
    bool UpdateMaterial(MaterialHandle materialHandle, MaterialRenderProxy proxy);
    // 被引用的材质不可删除；须先删除引用它的 Batch，或改为引用其他材质。
    bool RemoveMaterial(MaterialHandle materialHandle);

    PrimitiveHandle AddPrimitive(PrimitiveSceneInfo primitive = {});
    bool RemovePrimitive(PrimitiveHandle primitiveHandle);
    bool UpdatePrimitiveTransform(PrimitiveHandle primitiveHandle, const AffineTransform& transform);
    bool UpdatePrimitiveBounds(PrimitiveHandle primitiveHandle, const Aabb& bounds);
    MeshBatchHandle AddMeshBatch(PrimitiveHandle primitiveHandle, MeshBatch batch);
    bool UpdateMeshBatch(MeshBatchHandle batchHandle, MeshBatch batch);
    bool RemoveMeshBatch(MeshBatchHandle batchHandle);
    bool RemoveMeshBatch(PrimitiveHandle primitiveHandle, MeshBatchHandle batchHandle);

    void MarkBatchDirty(MeshBatchHandle batchHandle);
    void MarkMaterialDirty(MaterialHandle materialHandle);
    bool HasDirtyBatches() const noexcept { return _dirtyCount != 0; }
    // 同步之前，队列中可能仍包含已删除的句柄。
    std::span<const MeshBatchHandle> GetDirtyBatches() const noexcept { return _dirtyBatches; }
    Nullable<const CachedPassMeshDrawList*> GetPassCache(MeshPassId pass) const noexcept;
    Nullable<const CachedPassMeshDrawList*> GetCachedDrawList(MeshPassId pass) const noexcept { return GetPassCache(pass); }

    // Pass ID 必须是唯一、紧凑的索引；登记变化会重建全部 Batch。
    // Processor 必须非空；登记违约终止程序，构建输入由调用方保证（helper 仅 Debug 校验）。
    // 正常返回后无脏 Batch；调用必须串行。
    void SyncCachedMeshDrawCommands(std::span<const MeshPassRegistration> passes);
    // 向输出追加命令；输入无效时回滚本次调用追加的命令。
    // 输出借用缓存指针；修改本 Scene 或其缓存前必须完成录制。
    bool GatherVisibleMeshDrawCommands(MeshPassId pass, std::span<const VisibleMeshBatch> visibleBatches,
                                       vector<VisibleMeshDrawCommand>& outCommands) const;
    bool GatherVisiblePrimitives(MeshPassId pass, std::span<const PrimitiveHandle> visiblePrimitives,
                                 vector<VisibleMeshDrawCommand>& outCommands) const;
    // 向帧列表追加借用的缓存指针，遵守相同的生命周期与回滚规则。
    // RHI 对象的借用持续到命令分配器重置。
    bool GatherVisibleMeshDrawCommands(MeshPassId pass, std::span<const VisibleMeshBatch> visibleBatches,
                                       MeshPassFrameDrawList& outDrawList) const;
    bool GatherVisiblePrimitives(MeshPassId pass, std::span<const PrimitiveHandle> visiblePrimitives,
                                 MeshPassFrameDrawList& outDrawList) const;
    void Clear() noexcept;

private:
    void InvalidateBatchCachedCommands(uint32_t batchIndex) noexcept;
    void AddUniqueDirtyBatch(MeshBatchHandle batchHandle);
    void UnlinkMaterial(MeshBatchHandle batchHandle) noexcept;
    void LinkMaterial(MeshBatchHandle batchHandle);
    bool ValidateBatch(const MeshBatch& batch) const noexcept;

    SparseSet<PrimitiveSceneInfo> _primitives;
    SparseSet<MeshBatch> _meshes;
    SparseSet<MaterialRenderProxy> _materials;
    vector<CachedPassMeshDrawList> _cachedDrawLists;
    vector<PrimitiveHandle> _batchOwners;
    vector<uint8_t> _batchDirty;
    vector<uint32_t> _primitivePositions;
    vector<uint32_t> _materialPositions;
    vector<vector<MeshBatchHandle>> _materialBatches;
    vector<MeshBatchHandle> _dirtyBatches;
    vector<MeshPassRegistration> _registeredPasses;
    uint32_t _dirtyCount{0};
    uint32_t _reservedBatches{0};
    bool _initializedCache{false};
};

}  // namespace radray
