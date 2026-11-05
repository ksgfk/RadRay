#include <radray/runtime/scene.h>

#include <algorithm>

#include <radray/logger.h>

namespace radray {

bool Scene::ValidateBatch(const MeshBatch& batch) const noexcept {
    if (!Materials.IsAlive(batch.Material) || batch.Elements.size() > std::numeric_limits<uint32_t>::max()) return false;
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

SparseSetHandle Scene::AddMaterial(MaterialRenderProxy proxy) { return Materials.Emplace(std::move(proxy)); }

bool Scene::UpdateMaterial(SparseSetHandle materialHandle, MaterialRenderProxy proxy) {
    if (!Materials.IsAlive(materialHandle)) return false;
    Materials.Get(materialHandle) = std::move(proxy);
    MarkMaterialDirty(materialHandle);
    return true;
}

bool Scene::RemoveMaterial(SparseSetHandle materialHandle) {
    if (!Materials.IsAlive(materialHandle)) return false;
    for (const auto& batch : Meshes.Values()) {
        if (batch.Material == materialHandle) return false;
    }
    Materials.Destroy(materialHandle);
    return true;
}

SparseSetHandle Scene::AddPrimitive(PrimitiveSceneInfo primitive) {
    for (size_t index = 0; index < primitive.Meshes.size(); ++index) {
        const auto handle = primitive.Meshes[index];
        if (!Meshes.IsAlive(handle) || ResolveOwningPrimitive(handle).IsValid() || !ValidateBatch(Meshes.Get(handle))) return {};
        if (std::find(primitive.Meshes.begin(), primitive.Meshes.begin() + index, handle) != primitive.Meshes.begin() + index) return {};
    }
    const auto primitiveHandle = Primitives.Emplace(std::move(primitive));
    for (const auto handle : Primitives.Get(primitiveHandle).Meshes) {
        Meshes.Get(handle).OwningPrimitive = primitiveHandle;
        AddUniqueDirtyBatch(handle);
    }
    return primitiveHandle;
}

bool Scene::RemovePrimitive(SparseSetHandle primitiveHandle) {
    if (!Primitives.IsAlive(primitiveHandle)) return false;
    auto& primitive = Primitives.Get(primitiveHandle);
    while (!primitive.Meshes.empty()) {
        const auto handle = primitive.Meshes.back();
        if (!Meshes.IsAlive(handle)) {
            primitive.Meshes.pop_back();
            continue;
        }
        RemoveMeshBatch(primitiveHandle, handle);
    }
    Primitives.Destroy(primitiveHandle);
    return true;
}

SparseSetHandle Scene::AddMeshBatch(SparseSetHandle primitiveHandle, MeshBatch batch) {
    if (!Primitives.IsAlive(primitiveHandle) || !ValidateBatch(batch)) {
        RADRAY_ERR_LOG("Scene: invalid primitive, material or mesh geometry");
        return {};
    }
    batch.OwningPrimitive = primitiveHandle;
    batch.Dirty = false;
    batch.CachedCommands.clear();
    const auto handle = Meshes.Emplace(std::move(batch));
    Primitives.Get(primitiveHandle).Meshes.push_back(handle);
    AddUniqueDirtyBatch(handle);
    return handle;
}

bool Scene::UpdateMeshBatch(SparseSetHandle batchHandle, MeshBatch batch) {
    if (!Meshes.IsAlive(batchHandle) || !ResolveOwningPrimitive(batchHandle).IsValid() || !ValidateBatch(batch)) return false;
    auto& target = Meshes.Get(batchHandle);
    batch.OwningPrimitive = target.OwningPrimitive;
    batch.Dirty = target.Dirty;
    // Keep the old handles until synchronization; gathering dirty batches is rejected.
    batch.CachedCommands = std::move(target.CachedCommands);
    target = std::move(batch);
    AddUniqueDirtyBatch(batchHandle);
    return true;
}

bool Scene::RemoveMeshBatch(SparseSetHandle primitiveHandle, SparseSetHandle batchHandle) {
    if (!Primitives.IsAlive(primitiveHandle) || !Meshes.IsAlive(batchHandle)) return false;
    auto& handles = Primitives.Get(primitiveHandle).Meshes;
    const auto found = std::find(handles.begin(), handles.end(), batchHandle);
    if (found == handles.end()) return false;
    auto& batch = Meshes.Get(batchHandle);
    InvalidateBatchCachedCommands(batch);
    handles.erase(found);
    if (batch.Dirty) {
        std::erase(_dirtyBatches, batchHandle);
        batch.Dirty = false;
    }
    Meshes.Destroy(batchHandle);
    return true;
}

void Scene::AddUniqueDirtyBatch(SparseSetHandle batchHandle) {
    if (!Meshes.IsAlive(batchHandle)) return;
    auto& batch = Meshes.Get(batchHandle);
    if (!batch.Dirty) {
        batch.Dirty = true;
        _dirtyBatches.push_back(batchHandle);
    }
}

void Scene::MarkBatchDirty(SparseSetHandle batchHandle) {
    if (Meshes.IsAlive(batchHandle)) AddUniqueDirtyBatch(batchHandle);
}

void Scene::MarkMaterialDirty(SparseSetHandle materialHandle) {
    if (!Materials.IsAlive(materialHandle)) return;
    for (const auto& primitive : Primitives.Values()) {
        for (const auto handle : primitive.Meshes) {
            if (Meshes.IsAlive(handle) && Meshes.Get(handle).Material == materialHandle) AddUniqueDirtyBatch(handle);
        }
    }
}

SparseSetHandle Scene::FindOwningPrimitive(SparseSetHandle batchHandle) const noexcept {
    for (uint32_t index = 0; index < Primitives.Count(); ++index) {
        const auto entry = Primitives.GetHandleAt(index);
        const auto& primitive = Primitives.Get(entry);
        if (std::find(primitive.Meshes.begin(), primitive.Meshes.end(), batchHandle) != primitive.Meshes.end()) return entry;
    }
    return {};
}

SparseSetHandle Scene::ResolveOwningPrimitive(SparseSetHandle batchHandle) noexcept {
    if (!Meshes.IsAlive(batchHandle)) return {};
    auto& batch = Meshes.Get(batchHandle);
    if (Primitives.IsAlive(batch.OwningPrimitive)) return batch.OwningPrimitive;
    batch.OwningPrimitive = FindOwningPrimitive(batchHandle);
    return batch.OwningPrimitive;
}

void Scene::InvalidateBatchCachedCommands(MeshBatch& batch) noexcept {
    for (const auto& info : batch.CachedCommands) {
        if (info.Pass.Value < CachedDrawLists.size()) {
            auto& commands = CachedDrawLists[info.Pass.Value].Commands;
            if (commands.IsAlive(info.CommandHandle)) commands.Destroy(info.CommandHandle);
        }
    }
    batch.CachedCommands.clear();
}

Nullable<const CachedPassMeshDrawList*> Scene::GetCachedDrawList(MeshPassId pass) const noexcept {
    return pass.Value < CachedDrawLists.size() ? &CachedDrawLists[pass.Value] : nullptr;
}

void Scene::SyncCachedMeshDrawCommands(std::span<const MeshPassRegistration> passes) {
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
    const bool registrationsChanged = passes.size() != _registeredPasses.size() ||
                                      std::any_of(passes.begin(), passes.end(), [&](const MeshPassRegistration& pass) {
                                          return std::none_of(_registeredPasses.begin(), _registeredPasses.end(), [&](const MeshPassRegistration& previous) {
                                              return previous.Pass == pass.Pass && previous.Processor == pass.Processor;
                                          });
                                      });
    if (!_initializedCache || registrationsChanged) {
        _dirtyBatches.clear();
        for (auto& batch : Meshes.Values()) {
            batch.Dirty = false;
            batch.OwningPrimitive = {};
            InvalidateBatchCachedCommands(batch);
        }
        for (uint32_t index = 0; index < Primitives.Count(); ++index) {
            const auto primitiveHandle = Primitives.GetHandleAt(index);
            for (const auto handle : Primitives.Get(primitiveHandle).Meshes) {
                if (Meshes.IsAlive(handle)) {
                    Meshes.Get(handle).OwningPrimitive = primitiveHandle;
                    AddUniqueDirtyBatch(handle);
                }
            }
        }
        _registeredPasses.assign(passes.begin(), passes.end());
        _initializedCache = true;
    }
    for (const auto& pass : passes) {
        if (pass.Pass.Value >= CachedDrawLists.size()) CachedDrawLists.resize(size_t{pass.Pass.Value} + 1);
    }
    auto dirty = std::move(_dirtyBatches);
    _dirtyBatches.clear();
    for (const auto handle : dirty) {
        if (!Meshes.IsAlive(handle)) continue;
        auto& batch = Meshes.Get(handle);
        batch.Dirty = false;
        InvalidateBatchCachedCommands(batch);
        const auto primitiveHandle = ResolveOwningPrimitive(handle);
        if (!primitiveHandle.IsValid() || !ValidateBatch(batch)) {
            RADRAY_ERR_LOG("Scene: dirty mesh batch has invalid associations or geometry: {}", handle);
            AddUniqueDirtyBatch(handle);
            continue;
        }
        const MeshBatchContext context{Primitives.Get(primitiveHandle), batch, primitiveHandle, handle, &Materials.Get(batch.Material)};
        for (const auto& pass : passes) {
            CachedPassMeshDrawListContext output{pass.Pass, CachedDrawLists[pass.Pass.Value], batch};
            auto& previousContext = pass.Processor->DrawListContext();
            pass.Processor->SetDrawListContext(output);
            pass.Processor->AddMeshBatch(context);
            pass.Processor->SetDrawListContext(previousContext);
        }
    }
}

bool Scene::GatherVisibleMeshDrawCommands(MeshPassId pass, std::span<const VisibleMeshBatch> visibleBatches,
                                          vector<VisibleMeshDrawCommand>& outCommands) const {
    const auto initialSize = outCommands.size();
    const auto drawList = GetCachedDrawList(pass);
    for (const auto& visible : visibleBatches) {
        if (!Meshes.IsAlive(visible.Batch)) {
            outCommands.resize(initialSize);
            return false;
        }
        const auto& batch = Meshes.Get(visible.Batch);
        if (batch.Dirty || (visible.ElementIndex != std::numeric_limits<uint32_t>::max() && visible.ElementIndex >= batch.Elements.size())) {
            outCommands.resize(initialSize);
            return false;
        }
        for (const auto& info : batch.CachedCommands) {
            if (info.Pass != pass || (visible.ElementIndex != std::numeric_limits<uint32_t>::max() && info.ElementIndex != visible.ElementIndex)) continue;
            if (!drawList || !drawList->Commands.IsAlive(info.CommandHandle)) {
                outCommands.resize(initialSize);
                return false;
            }
            outCommands.push_back({.Source = MeshDrawCommandSource::Cached,
                                   .CommandHandle = info.CommandHandle,
                                   .SortKey = info.BaseSortKey | visible.VisibilitySortKey,
                                   .FirstInstance = visible.FirstInstanceOverride.value_or(info.FirstInstance),
                                   .InstanceCount = visible.InstanceCountOverride.value_or(info.InstanceCount)});
        }
    }
    return true;
}

bool Scene::GatherVisiblePrimitives(MeshPassId pass, std::span<const SparseSetHandle> visiblePrimitives,
                                    vector<VisibleMeshDrawCommand>& outCommands) const {
    const auto initialSize = outCommands.size();
    for (const auto handle : visiblePrimitives) {
        if (!Primitives.IsAlive(handle)) {
            outCommands.resize(initialSize);
            return false;
        }
        for (const auto batch : Primitives.Get(handle).Meshes) {
            const VisibleMeshBatch visible{.Batch = batch};
            if (!GatherVisibleMeshDrawCommands(pass, std::span{&visible, 1}, outCommands)) {
                outCommands.resize(initialSize);
                return false;
            }
        }
    }
    return true;
}

void Scene::Clear() noexcept {
    for (auto& drawList : CachedDrawLists) drawList.Commands.Clear();
    Meshes.Clear();
    Primitives.Clear();
    Materials.Clear();
    _dirtyBatches.clear();
    _registeredPasses.clear();
    _initializedCache = false;
}

}  // namespace radray
