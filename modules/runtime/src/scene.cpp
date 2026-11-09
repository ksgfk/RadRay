#include <radray/runtime/scene.h>

#include <algorithm>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/frame_slot.h>

namespace radray {

bool Scene::ValidateBatch(const MeshBatch& batch) const noexcept {
    if (!_materials.IsAlive(batch.Material._handle) || batch.Elements.size() > 32) return false;
    const bool draws = std::any_of(batch.Elements.begin(), batch.Elements.end(), [](const MeshBatchElement& element) { return element.IndexCount != 0 && element.InstanceCount != 0; });
    if (draws && (!batch.VertexInput || !ValidateVertexInput(*batch.VertexInput, true))) return false;
    for (const auto& element : batch.Elements) {
        if (element.IndexCount == 0 || element.InstanceCount == 0) continue;
        if (!element.IndexBuffer || !element.IndexBuffer->IsValid() || (element.IndexStride != 2 && element.IndexStride != 4) ||
            element.IndexBufferOffset % element.IndexStride != 0 ||
            uint64_t{element.FirstInstance} + element.InstanceCount - 1 > std::numeric_limits<uint32_t>::max()) return false;
        const auto desc = element.IndexBuffer->GetDesc();
        if (!desc.Usage.HasFlag(render::BufferUse::Index) ||
            uint64_t{element.IndexBufferOffset} + (uint64_t{element.FirstIndex} + element.IndexCount) * element.IndexStride > desc.Size) return false;
    }
    return true;
}

void Scene::Reserve(uint32_t primitives, uint32_t batches, uint32_t materials) {
    _primitives.ReserveGeometric(primitives);
    _meshes.ReserveGeometric(batches);
    _materials.ReserveGeometric(materials);
    if (batches > _reservedBatches) {
        const auto grown = std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), uint64_t{_reservedBatches} + _reservedBatches / 2);
        _reservedBatches = static_cast<uint32_t>(std::max<uint64_t>(batches, grown));
    }
    batches = _reservedBatches;
    _batchOwners.reserve(batches);
    _batchDirty.reserve(batches);
    _primitivePositions.reserve(batches);
    _materialPositions.reserve(batches);
    if (materials > _materialBatches.capacity()) _materialBatches.reserve(std::max<size_t>(materials, _materialBatches.capacity() + _materialBatches.capacity() / 2));
    _dirtyBatches.reserve(batches);
    for (auto& cache : _cachedDrawLists) cache.Reserve(_reservedBatches);
}

Nullable<const PrimitiveSceneInfo*> Scene::GetPrimitive(PrimitiveHandle handle) const noexcept { return _primitives.TryGet(handle._handle); }
Nullable<const MeshBatch*> Scene::GetMeshBatch(MeshBatchHandle handle) const noexcept { return _meshes.TryGet(handle._handle); }
Nullable<const MaterialRenderProxy*> Scene::GetMaterial(MaterialHandle handle) const noexcept { return _materials.TryGet(handle._handle); }
bool Scene::IsBatchDirty(MeshBatchHandle handle) const noexcept { return _meshes.IsAlive(handle._handle) && _batchDirty[handle.GetIndex()]; }
PrimitiveHandle Scene::GetOwningPrimitive(MeshBatchHandle handle) const noexcept { return _meshes.IsAlive(handle._handle) ? _batchOwners[handle.GetIndex()] : PrimitiveHandle{}; }

MaterialHandle Scene::AddMaterial(MaterialRenderProxy proxy) {
    const MaterialHandle handle{_materials.Emplace(std::move(proxy))};
    if (_materialBatches.size() <= handle.GetIndex()) _materialBatches.resize(size_t{handle.GetIndex()} + 1);
    return handle;
}
bool Scene::UpdateMaterial(MaterialHandle handle, MaterialRenderProxy proxy) {
    if (!_materials.IsAlive(handle._handle)) return false;
    _materials.Get(handle._handle) = std::move(proxy);
    MarkMaterialDirty(handle);
    return true;
}
bool Scene::RemoveMaterial(MaterialHandle handle) {
    if (!_materials.IsAlive(handle._handle) || !_materialBatches[handle.GetIndex()].empty()) return false;
    _materials.Destroy(handle._handle);
    return true;
}
PrimitiveHandle Scene::AddPrimitive(PrimitiveSceneInfo primitive) {
    // Scene 关联列表仅作为输出；Batch 必须通过 AddMeshBatch 添加。
    if (!primitive.Meshes.empty()) return {};
    return PrimitiveHandle{_primitives.Emplace(std::move(primitive))};
}
bool Scene::UpdatePrimitiveTransform(PrimitiveHandle handle, const AffineTransform& transform) {
    if (!_primitives.IsAlive(handle._handle)) return false;
    _primitives.Get(handle._handle).LocalToWorld = transform;
    return true;
}
bool Scene::UpdatePrimitiveBounds(PrimitiveHandle handle, const Aabb& bounds) {
    if (!_primitives.IsAlive(handle._handle)) return false;
    _primitives.Get(handle._handle).LocalBounds = bounds;
    return true;
}
bool Scene::RemovePrimitive(PrimitiveHandle handle) {
    if (!_primitives.IsAlive(handle._handle)) return false;
    auto& primitive = _primitives.Get(handle._handle);
    while (!primitive.Meshes.empty()) RemoveMeshBatch(primitive.Meshes.back());
    _primitives.Destroy(handle._handle);
    return true;
}
void Scene::LinkMaterial(MeshBatchHandle handle) {
    auto& references = _materialBatches[_meshes.Get(handle._handle).Material.GetIndex()];
    _materialPositions[handle.GetIndex()] = static_cast<uint32_t>(references.size());
    references.push_back(handle);
}
void Scene::UnlinkMaterial(MeshBatchHandle handle) noexcept {
    auto& references = _materialBatches[_meshes.Get(handle._handle).Material.GetIndex()];
    const auto position = _materialPositions[handle.GetIndex()];
    references[position] = references.back();
    _materialPositions[references.back().GetIndex()] = position;
    references.pop_back();
}
MeshBatchHandle Scene::AddMeshBatch(PrimitiveHandle primitive, MeshBatch batch) {
    if (!_primitives.IsAlive(primitive._handle) || !ValidateBatch(batch)) return {};
    const MeshBatchHandle handle{_meshes.Emplace(std::move(batch))};
    const auto count = size_t{handle.GetIndex()} + 1;
    if (_batchOwners.size() < count) {
        _batchOwners.resize(count);
        _batchDirty.resize(count);
        _primitivePositions.resize(count);
        _materialPositions.resize(count);
    }
    _batchOwners[handle.GetIndex()] = primitive;
    _batchDirty[handle.GetIndex()] = false;
    auto& meshes = _primitives.Get(primitive._handle).Meshes;
    _primitivePositions[handle.GetIndex()] = static_cast<uint32_t>(meshes.size());
    meshes.push_back(handle);
    LinkMaterial(handle);
    AddUniqueDirtyBatch(handle);
    return handle;
}
bool Scene::UpdateMeshBatch(MeshBatchHandle handle, MeshBatch batch) {
    if (!_meshes.IsAlive(handle._handle) || !ValidateBatch(batch)) return false;
    UnlinkMaterial(handle);
    _meshes.Get(handle._handle) = std::move(batch);
    LinkMaterial(handle);
    AddUniqueDirtyBatch(handle);
    return true;
}
bool Scene::RemoveMeshBatch(PrimitiveHandle primitive, MeshBatchHandle batch) {
    return GetOwningPrimitive(batch) == primitive && _primitives.IsAlive(primitive._handle) && RemoveMeshBatch(batch);
}
bool Scene::RemoveMeshBatch(MeshBatchHandle handle) {
    if (!_meshes.IsAlive(handle._handle)) return false;
    InvalidateBatchCachedCommands(handle.GetIndex());
    UnlinkMaterial(handle);
    auto& meshes = _primitives.Get(_batchOwners[handle.GetIndex()]._handle).Meshes;
    const auto position = _primitivePositions[handle.GetIndex()];
    meshes[position] = meshes.back();
    _primitivePositions[meshes.back().GetIndex()] = position;
    meshes.pop_back();
    if (_batchDirty[handle.GetIndex()]) --_dirtyCount;
    _batchDirty[handle.GetIndex()] = false;
    _batchOwners[handle.GetIndex()] = {};
    _meshes.Destroy(handle._handle);
    return true;
}
void Scene::AddUniqueDirtyBatch(MeshBatchHandle handle) {
    if (!_meshes.IsAlive(handle._handle) || _batchDirty[handle.GetIndex()]) return;
    _batchDirty[handle.GetIndex()] = true;
    ++_dirtyCount;
    _dirtyBatches.push_back(handle);
}
void Scene::MarkBatchDirty(MeshBatchHandle handle) { AddUniqueDirtyBatch(handle); }
void Scene::MarkMaterialDirty(MaterialHandle handle) {
    if (!_materials.IsAlive(handle._handle)) return;
    for (const auto batch : _materialBatches[handle.GetIndex()]) AddUniqueDirtyBatch(batch);
}
void Scene::InvalidateBatchCachedCommands(uint32_t batchIndex) noexcept {
    for (auto& cache : _cachedDrawLists) cache.DestroyCommand(batchIndex);
}
Nullable<const CachedPassMeshDrawList*> Scene::GetPassCache(MeshPassId pass) const noexcept {
    return pass.Value < _cachedDrawLists.size() && _cachedDrawLists[pass.Value]._registered ? &_cachedDrawLists[pass.Value] : nullptr;
}
void Scene::SyncCachedMeshDrawCommands(std::span<const MeshPassRegistration> passes) {
    RADRAY_PROFILE_SCOPE_N("RadRay.Scene.SyncCachedCommands");
    for (size_t index = 0; index < passes.size(); ++index) {
        if (passes[index].Pass.Value == std::numeric_limits<uint32_t>::max()) {
            RADRAY_ABORT("Scene: mesh pass ID must be a compact index");
        }
        if (!passes[index].Processor) RADRAY_ABORT("Scene: mesh pass processor must not be null");
        for (size_t previous = 0; previous < index; ++previous) {
            if (passes[previous].Pass == passes[index].Pass) {
                RADRAY_ABORT("Scene: duplicate mesh pass ID");
            }
        }
    }
    const bool changed = passes.size() != _registeredPasses.size() ||
                         std::any_of(passes.begin(), passes.end(), [&](const auto& pass) {
                             return std::none_of(_registeredPasses.begin(), _registeredPasses.end(), [&](const auto& old) {
                                 return old.Pass == pass.Pass && old.Processor == pass.Processor && old.SortMode == pass.SortMode;
                             });
                         });
    if (!_initializedCache || changed) {
        for (auto& cache : _cachedDrawLists) cache.Clear();
        for (uint32_t i = 0; i < _meshes.Count(); ++i) AddUniqueDirtyBatch(MeshBatchHandle{_meshes.GetHandleAt(i)});
        _registeredPasses.assign(passes.begin(), passes.end());
        _initializedCache = true;
    }
    for (const auto& pass : passes) {
        if (pass.Pass.Value >= _cachedDrawLists.size()) _cachedDrawLists.resize(size_t{pass.Pass.Value} + 1);
        auto& cache = _cachedDrawLists[pass.Pass.Value];
        cache.Reserve(_reservedBatches);
        cache._sortMode = pass.SortMode;
        cache._registered = true;
    }
    RADRAY_PROFILE_PLOT("RadRay.Scene.DirtyBatches", static_cast<int64_t>(_dirtyCount));
    for (const auto handle : _dirtyBatches) {
        if (!IsBatchDirty(handle)) continue;
        InvalidateBatchCachedCommands(handle.GetIndex());
        const auto& batch = _meshes.Get(handle._handle);
        const auto primitive = _batchOwners[handle.GetIndex()];
        const MeshBatchContext context{_primitives.Get(primitive._handle), batch, primitive, handle, &_materials.Get(batch.Material._handle)};
        for (const auto& pass : passes) {
            CachedPassMeshDrawListContext output{_cachedDrawLists[pass.Pass.Value], handle.GetIndex(), &batch};
            pass.Processor->AddMeshBatch(context, output);
        }
        _batchDirty[handle.GetIndex()] = false;
    }
    _dirtyBatches.clear();
    _dirtyCount = 0;
}

bool Scene::GatherVisibleMeshDrawCommands(MeshPassId pass, std::span<const VisibleMeshBatch> visibleBatches, vector<VisibleMeshDrawCommand>& output) const {
    RADRAY_PROFILE_SCOPE_N("RadRay.Scene.GatherVisibleHandles");
    RADRAY_PROFILE_PLOT("RadRay.Scene.VisibleBatches", static_cast<int64_t>(visibleBatches.size()));
    const auto initialSize = output.size();
    const auto required = initialSize + visibleBatches.size();
    if (output.capacity() < required) output.reserve(std::max(required, output.capacity() + output.capacity() / 2));
    const auto cache = GetPassCache(pass);
    for (const auto& visible : visibleBatches) {
        if (!_meshes.IsAlive(visible.Batch._handle) || _batchDirty[visible.Batch.GetIndex()]) {
            output.resize(initialSize);
            return false;
        }
        const auto slot = cache ? cache->GetBatchSlot(visible.Batch.GetIndex()) : nullptr;
        if (!slot || slot->CommandDenseIndex == kInvalidBindingIndex) continue;
        const auto& command = cache->_commands[slot->CommandDenseIndex];
        const auto mask = visible.ElementMask & command.ValidElementMask;
        if (!mask) continue;
        output.push_back({.SortKey = ComposeMeshDrawSortKey(cache->_sortMode, slot->StateSortKey, visible.VisibilitySortKey),
                          .CachedCommand = &command,
                          .FirstInstance = visible.FirstInstanceOverride.value_or(slot->FirstInstance),
                          .InstanceCount = visible.InstanceCountOverride.value_or(slot->InstanceCount),
                          .ElementMask = mask,
                          .UseElementFirstInstance = !visible.FirstInstanceOverride.has_value(),
                          .UseElementInstanceCount = !visible.InstanceCountOverride.has_value()});
    }
    return true;
}

bool Scene::GatherVisiblePrimitives(MeshPassId pass, std::span<const PrimitiveHandle> visiblePrimitives,
                                    vector<VisibleMeshDrawCommand>& outCommands) const {
    const auto initialSize = outCommands.size();
    for (const auto handle : visiblePrimitives) {
        if (!_primitives.IsAlive(handle._handle)) {
            outCommands.resize(initialSize);
            return false;
        }
        for (const auto batch : _primitives.Get(handle._handle).Meshes) {
            const VisibleMeshBatch visible{.Batch = batch};
            if (!GatherVisibleMeshDrawCommands(pass, std::span{&visible, 1}, outCommands)) {
                outCommands.resize(initialSize);
                return false;
            }
        }
    }
    return true;
}

bool Scene::GatherVisibleMeshDrawCommands(MeshPassId pass, std::span<const VisibleMeshBatch> visibleBatches,
                                          MeshPassFrameDrawList& outDrawList) const {
    const auto cache = GetPassCache(pass);
    if (cache) outDrawList.SetSortMode(cache->GetSortMode());
    return GatherVisibleMeshDrawCommands(pass, visibleBatches, outDrawList.VisibleCommands);
}

bool Scene::GatherVisiblePrimitives(MeshPassId pass, std::span<const PrimitiveHandle> visiblePrimitives,
                                    MeshPassFrameDrawList& outDrawList) const {
    const auto cache = GetPassCache(pass);
    if (cache) outDrawList.SetSortMode(cache->GetSortMode());
    return GatherVisiblePrimitives(pass, visiblePrimitives, outDrawList.VisibleCommands);
}

void Scene::Clear() noexcept {
    for (auto& cache : _cachedDrawLists) cache.Clear();
    _meshes.Clear();
    _primitives.Clear();
    _materials.Clear();
    _batchOwners.clear();
    _batchDirty.clear();
    _primitivePositions.clear();
    _materialPositions.clear();
    for (auto& references : _materialBatches) references.clear();
    _dirtyBatches.clear();
    _registeredPasses.clear();
    _dirtyCount = 0;
    _initializedCache = false;
}

}  // namespace radray
