#include <radray/runtime/scene.h>

#include <algorithm>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/frame_slot.h>

namespace radray {

bool Scene::ValidateBatch(const MeshBatch& batch) const noexcept {
    if (!_materials.IsAlive(batch.Material) || batch.Elements.size() > 32) return false;
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

Nullable<const PrimitiveSceneInfo*> Scene::GetPrimitive(SparseSetHandle handle) const noexcept { return _primitives.TryGet(handle); }
Nullable<const MeshBatch*> Scene::GetMeshBatch(SparseSetHandle handle) const noexcept { return _meshes.TryGet(handle); }
Nullable<const MaterialRenderProxy*> Scene::GetMaterial(SparseSetHandle handle) const noexcept { return _materials.TryGet(handle); }
bool Scene::IsBatchDirty(SparseSetHandle handle) const noexcept { return _meshes.IsAlive(handle) && _batchDirty[handle.Index]; }
SparseSetHandle Scene::GetOwningPrimitive(SparseSetHandle handle) const noexcept { return _meshes.IsAlive(handle) ? _batchOwners[handle.Index] : SparseSetHandle{}; }

SparseSetHandle Scene::AddMaterial(MaterialRenderProxy proxy) {
    const auto handle = _materials.Emplace(std::move(proxy));
    if (_materialBatches.size() <= handle.Index) _materialBatches.resize(size_t{handle.Index} + 1);
    return handle;
}
bool Scene::UpdateMaterial(SparseSetHandle handle, MaterialRenderProxy proxy) {
    if (!_materials.IsAlive(handle)) return false;
    _materials.Get(handle) = std::move(proxy);
    MarkMaterialDirty(handle);
    return true;
}
bool Scene::RemoveMaterial(SparseSetHandle handle) {
    if (!_materials.IsAlive(handle) || !_materialBatches[handle.Index].empty()) return false;
    _materials.Destroy(handle);
    return true;
}
SparseSetHandle Scene::AddPrimitive(PrimitiveSceneInfo primitive) {
    // Scene links are output only; batches must be added through AddMeshBatch.
    if (!primitive.Meshes.empty()) return {};
    return _primitives.Emplace(std::move(primitive));
}
bool Scene::UpdatePrimitiveTransform(SparseSetHandle handle, const AffineTransform& transform) {
    if (!_primitives.IsAlive(handle)) return false;
    _primitives.Get(handle).LocalToWorld = transform;
    return true;
}
bool Scene::UpdatePrimitiveBounds(SparseSetHandle handle, const Aabb& bounds) {
    if (!_primitives.IsAlive(handle)) return false;
    _primitives.Get(handle).LocalBounds = bounds;
    return true;
}
bool Scene::RemovePrimitive(SparseSetHandle handle) {
    if (!_primitives.IsAlive(handle)) return false;
    auto& primitive = _primitives.Get(handle);
    while (!primitive.Meshes.empty()) RemoveMeshBatch(primitive.Meshes.back());
    _primitives.Destroy(handle);
    return true;
}
void Scene::LinkMaterial(SparseSetHandle handle) {
    auto& references = _materialBatches[_meshes.Get(handle).Material.Index];
    _materialPositions[handle.Index] = static_cast<uint32_t>(references.size());
    references.push_back(handle);
}
void Scene::UnlinkMaterial(SparseSetHandle handle) noexcept {
    auto& references = _materialBatches[_meshes.Get(handle).Material.Index];
    const auto position = _materialPositions[handle.Index];
    references[position] = references.back();
    _materialPositions[references.back().Index] = position;
    references.pop_back();
}
SparseSetHandle Scene::AddMeshBatch(SparseSetHandle primitive, MeshBatch batch) {
    if (!_primitives.IsAlive(primitive) || !ValidateBatch(batch)) return {};
    const auto handle = _meshes.Emplace(std::move(batch));
    const auto count = size_t{handle.Index} + 1;
    if (_batchOwners.size() < count) {
        _batchOwners.resize(count);
        _batchDirty.resize(count);
        _primitivePositions.resize(count);
        _materialPositions.resize(count);
    }
    _batchOwners[handle.Index] = primitive;
    _batchDirty[handle.Index] = false;
    auto& meshes = _primitives.Get(primitive).Meshes;
    _primitivePositions[handle.Index] = static_cast<uint32_t>(meshes.size());
    meshes.push_back(handle);
    LinkMaterial(handle);
    AddUniqueDirtyBatch(handle);
    return handle;
}
bool Scene::UpdateMeshBatch(SparseSetHandle handle, MeshBatch batch) {
    if (!_meshes.IsAlive(handle) || !ValidateBatch(batch)) return false;
    UnlinkMaterial(handle);
    _meshes.Get(handle) = std::move(batch);
    LinkMaterial(handle);
    AddUniqueDirtyBatch(handle);
    return true;
}
bool Scene::RemoveMeshBatch(SparseSetHandle primitive, SparseSetHandle batch) {
    return GetOwningPrimitive(batch) == primitive && _primitives.IsAlive(primitive) && RemoveMeshBatch(batch);
}
bool Scene::RemoveMeshBatch(SparseSetHandle handle) {
    if (!_meshes.IsAlive(handle)) return false;
    InvalidateBatchCachedCommands(handle.Index);
    UnlinkMaterial(handle);
    auto& meshes = _primitives.Get(_batchOwners[handle.Index]).Meshes;
    const auto position = _primitivePositions[handle.Index];
    meshes[position] = meshes.back();
    _primitivePositions[meshes.back().Index] = position;
    meshes.pop_back();
    if (_batchDirty[handle.Index]) --_dirtyCount;
    _batchDirty[handle.Index] = false;
    _batchOwners[handle.Index] = {};
    _meshes.Destroy(handle);
    return true;
}
void Scene::AddUniqueDirtyBatch(SparseSetHandle handle) {
    if (!_meshes.IsAlive(handle) || _batchDirty[handle.Index]) return;
    _batchDirty[handle.Index] = true;
    ++_dirtyCount;
    _dirtyBatches.push_back(handle);
}
void Scene::MarkBatchDirty(SparseSetHandle handle) { AddUniqueDirtyBatch(handle); }
void Scene::MarkMaterialDirty(SparseSetHandle handle) {
    if (!_materials.IsAlive(handle)) return;
    for (const auto batch : _materialBatches[handle.Index]) AddUniqueDirtyBatch(batch);
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
            RADRAY_ERR_LOG("Scene: mesh pass ID must be a compact index");
            return;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (passes[previous].Pass == passes[index].Pass) {
                RADRAY_ERR_LOG("Scene: duplicate mesh pass ID");
                return;
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
        for (uint32_t i = 0; i < _meshes.Count(); ++i) AddUniqueDirtyBatch(_meshes.GetHandleAt(i));
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
    size_t retryCount = 0;
    for (size_t index = 0; index < _dirtyBatches.size(); ++index) {
        const auto handle = _dirtyBatches[index];
        if (!IsBatchDirty(handle)) continue;
        InvalidateBatchCachedCommands(handle.Index);
        const auto& batch = _meshes.Get(handle);
        const auto primitive = _batchOwners[handle.Index];
        const MeshBatchContext context{_primitives.Get(primitive), batch, primitive, handle, &_materials.Get(batch.Material)};
        bool failed = false;
        for (const auto& pass : passes) {
            CachedPassMeshDrawListContext output{_cachedDrawLists[pass.Pass.Value], handle.Index};
            output._validatedBatch = &batch;
            auto& previous = pass.Processor->DrawListContext();
            pass.Processor->SetDrawListContext(output);
            pass.Processor->AddMeshBatch(context);
            pass.Processor->SetDrawListContext(previous);
            failed |= output._buildFailed;
        }
        if (failed) {
            InvalidateBatchCachedCommands(handle.Index);
            _dirtyBatches[retryCount++] = handle;
        } else {
            _batchDirty[handle.Index] = false;
            --_dirtyCount;
        }
    }
    _dirtyBatches.resize(retryCount);
}

bool Scene::GatherVisibleMeshDrawCommands(MeshPassId pass, std::span<const VisibleMeshBatch> visibleBatches, vector<VisibleMeshDrawCommand>& output) const {
    RADRAY_PROFILE_SCOPE_N("RadRay.Scene.GatherVisibleHandles");
    RADRAY_PROFILE_PLOT("RadRay.Scene.VisibleBatches", static_cast<int64_t>(visibleBatches.size()));
    const auto initialSize = output.size();
    const auto required = initialSize + visibleBatches.size();
    if (output.capacity() < required) output.reserve(std::max(required, output.capacity() + output.capacity() / 2));
    const auto cache = GetPassCache(pass);
    for (const auto& visible : visibleBatches) {
        if (!_meshes.IsAlive(visible.Batch) || _batchDirty[visible.Batch.Index]) {
            output.resize(initialSize);
            return false;
        }
        const auto slot = cache ? cache->GetBatchSlot(visible.Batch.Index) : nullptr;
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

bool Scene::GatherVisiblePrimitives(MeshPassId pass, std::span<const SparseSetHandle> visiblePrimitives,
                                    vector<VisibleMeshDrawCommand>& outCommands) const {
    const auto initialSize = outCommands.size();
    for (const auto handle : visiblePrimitives) {
        if (!_primitives.IsAlive(handle)) {
            outCommands.resize(initialSize);
            return false;
        }
        for (const auto batch : _primitives.Get(handle).Meshes) {
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

bool Scene::GatherVisiblePrimitives(MeshPassId pass, std::span<const SparseSetHandle> visiblePrimitives,
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
