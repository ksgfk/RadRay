#include <radray/runtime/render_framework/scene.h>
#include "render_memory_measure.h"

#include <algorithm>
#include <limits>

#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_scene_snapshot.h>

namespace radray {

constexpr uint32_t kInvalidPacked = std::numeric_limits<uint32_t>::max();

uint32_t Scene::SlotTable::Allocate() {
    if (!Free.empty()) {
        const uint32_t slot = Free.back();
        Free.pop_back();
        Packed[slot] = 0;
        return slot;
    }
    const uint32_t slot = static_cast<uint32_t>(Generation.size());
    Generation.push_back(1);
    Packed.push_back(kInvalidPacked);
    return slot;
}

void Scene::SlotTable::Release(uint32_t slot) noexcept {
    if (slot >= Generation.size()) return;
    if (Generation[slot] != std::numeric_limits<uint32_t>::max()) {
        ++Generation[slot];
        Free.push_back(slot);
    }
    Packed[slot] = kInvalidPacked;
}

Scene::~Scene() noexcept = default;

Scene::Scene() : _draws(make_unique<CpuDrawStore>()), _renderState(make_unique<SceneRenderState>()) {}

RenderMemoryStats Scene::GetMemoryStats() const noexcept {
    RenderMemoryStats result;
    result.ObjectBytes = sizeof(*this);
    result.LiveEntries = _primitiveProxies.size() + _lightProxies.size();
    result.OwnerReferences = result.LiveEntries;
    result.DirtyEntries = _dirtySlots.size() + _committedChanges.size();
    detail::MeasureVector(result, _primitiveProxies);
    detail::MeasureVector(result, _primitiveIds);
    detail::MeasureMap(result, _primitiveByPointer);
    detail::MeasureVector(result, _primitiveSlots.Generation);
    detail::MeasureVector(result, _primitiveSlots.Packed);
    detail::MeasureVector(result, _primitiveSlots.Free);
    detail::MeasureVector(result, _renderSlots);
    detail::MeasureVector(result, _dirtySlots);
    detail::MeasureVector(result, _renderObservers);
    detail::MeasureVector(result, _committedChanges);
    detail::MeasureVector(result, _lightProxies);
    detail::MeasureVector(result, _lightIds);
    detail::MeasureMap(result, _lightByPointer);
    detail::MeasureVector(result, _lightSlots.Generation);
    detail::MeasureVector(result, _lightSlots.Packed);
    detail::MeasureVector(result, _lightSlots.Free);
    return result;
}

Nullable<PrimitiveSceneProxy*> Scene::AddPrimitive(unique_ptr<PrimitiveSceneProxy> proxy) {
    if (proxy == nullptr) return nullptr;
    PrimitiveSceneProxy* raw = proxy.get();
    const uint32_t slot = _primitiveSlots.Allocate();
    const SceneObjectId id{slot, _primitiveSlots.Generation[slot]};
    _primitiveSlots.Packed[slot] = static_cast<uint32_t>(_primitiveProxies.size());
    _primitiveProxies.push_back(std::move(proxy));
    _primitiveIds.push_back(id);
    _primitiveByPointer.emplace(raw, id);
    raw->_scene = this;
    _renderSlots.resize(_primitiveSlots.Generation.size());
    auto& renderSlot = _renderSlots[slot];
    renderSlot.PendingId = id;
    renderSlot.Pending = PrimitiveDirtyKind::Structure | PrimitiveDirtyKind::MaterialAssignment |
                         PrimitiveDirtyKind::TransformOrBounds | PrimitiveDirtyKind::Filter |
                         PrimitiveDirtyKind::MotionReset;
    EnqueueRenderSlot(slot);
    return raw;
}

void Scene::RemovePrimitive(PrimitiveSceneProxy* proxy) noexcept {
    if (proxy == nullptr) return;
    auto it = std::find_if(_primitiveProxies.begin(), _primitiveProxies.end(),
                           [proxy](const unique_ptr<PrimitiveSceneProxy>& candidate) {
                               return candidate.get() == proxy;
                           });
    if (it == _primitiveProxies.end()) return;
    const auto packed = static_cast<uint32_t>(it - _primitiveProxies.begin());
    const SceneObjectId id = _primitiveIds[packed];
    auto& renderSlot = _renderSlots[id.Slot];
    if (renderSlot.CommittedGeneration == id.Generation) renderSlot.RemovedId = id;
    renderSlot.PendingId = {};
    renderSlot.Pending = {};
    EnqueueRenderSlot(id.Slot);
    if (renderSlot.Observed) {
        std::erase(_renderObservers, id);
        renderSlot.Observed = false;
    }
    proxy->_scene = nullptr;
    _primitiveByPointer.erase(proxy);
    _primitiveSlots.Release(id.Slot);
    _primitiveProxies.erase(it);
    _primitiveIds.erase(_primitiveIds.begin() + packed);
    for (size_t index = packed; index < _primitiveIds.size(); ++index)
        _primitiveSlots.Packed[_primitiveIds[index].Slot] = static_cast<uint32_t>(index);
}

Nullable<LightSceneProxy*> Scene::AddLight(unique_ptr<LightSceneProxy> proxy) {
    if (proxy == nullptr) return nullptr;
    LightSceneProxy* raw = proxy.get();
    const uint32_t slot = _lightSlots.Allocate();
    const SceneObjectId id{slot, _lightSlots.Generation[slot]};
    _lightSlots.Packed[slot] = static_cast<uint32_t>(_lightProxies.size());
    _lightProxies.push_back(std::move(proxy));
    _lightIds.push_back(id);
    _lightByPointer.emplace(raw, id);
    return raw;
}

void Scene::RemoveLight(LightSceneProxy* proxy) noexcept {
    if (proxy == nullptr) return;
    auto it = std::find_if(_lightProxies.begin(), _lightProxies.end(),
                           [proxy](const unique_ptr<LightSceneProxy>& candidate) {
                               return candidate.get() == proxy;
                           });
    if (it == _lightProxies.end()) return;
    const auto packed = static_cast<uint32_t>(it - _lightProxies.begin());
    const SceneObjectId id = _lightIds[packed];
    _lightByPointer.erase(proxy);
    _lightSlots.Release(id.Slot);
    _lightProxies.erase(it);
    _lightIds.erase(_lightIds.begin() + packed);
    for (size_t index = packed; index < _lightIds.size(); ++index)
        _lightSlots.Packed[_lightIds[index].Slot] = static_cast<uint32_t>(index);
}

SceneObjectId Scene::GetPrimitiveId(const PrimitiveSceneProxy* proxy) const noexcept {
    const auto found = _primitiveByPointer.find(proxy);
    return found == _primitiveByPointer.end() ? SceneObjectId{} : found->second;
}

Nullable<PrimitiveSceneProxy*> Scene::FindPrimitive(SceneObjectId id) const noexcept {
    if (!id.IsValid() || id.Slot >= _primitiveSlots.Generation.size() ||
        _primitiveSlots.Generation[id.Slot] != id.Generation || _primitiveSlots.Packed[id.Slot] == kInvalidPacked)
        return nullptr;
    return _primitiveProxies[_primitiveSlots.Packed[id.Slot]].get();
}

void Scene::EnqueueRenderSlot(uint32_t slot) const {
    auto& state = _renderSlots[slot];
    if (state.Enqueued) return;
    state.Enqueued = true;
    _dirtySlots.push_back(slot);
}

void Scene::MarkRenderDirty(SceneObjectId id, PrimitiveDirtyFlags flags) noexcept {
    const auto proxy = FindPrimitive(id);
    if (!proxy || !flags) return;
    auto& state = _renderSlots[id.Slot];
    state.PendingId = id;
    state.Pending |= flags;
    EnqueueRenderSlot(id.Slot);
}

void Scene::ObserveRenderDependencies() const {
    for (size_t index = 0; index < _renderObservers.size();) {
        const SceneObjectId id = _renderObservers[index];
        const auto proxy = FindPrimitive(id);
        auto& state = _renderSlots[id.Slot];
        if (!proxy) {
            state.Observed = false;
            _renderObservers[index] = _renderObservers.back();
            _renderObservers.pop_back();
            continue;
        }
        const bool legacy = !proxy->UsesRenderChangeNotifications();
        if (legacy)
            ++_commitStats.LegacyProxiesObserved;
        else
            ++_commitStats.PendingResourcesObserved;
        const uint64_t renderRevision = proxy->GetRenderDataRevision();
        const uint64_t transformRevision = proxy->GetTransformRevision();
        PrimitiveDirtyFlags dirty;
        if (legacy) dirty |= PrimitiveDirtyKind::MaterialAssignment | PrimitiveDirtyKind::Filter;
        if (renderRevision == 0 || renderRevision != state.RenderRevision)
            dirty |= PrimitiveDirtyKind::Structure | PrimitiveDirtyKind::MaterialAssignment | PrimitiveDirtyKind::Filter;
        if (transformRevision == 0 || transformRevision != state.TransformRevision)
            dirty |= PrimitiveDirtyKind::TransformOrBounds;
        if (dirty) {
            state.PendingId = id;
            state.Pending |= dirty;
            EnqueueRenderSlot(id.Slot);
        }
        if (!legacy && !proxy->HasPendingRenderResources()) {
            state.Observed = false;
            _renderObservers[index] = _renderObservers.back();
            _renderObservers.pop_back();
        } else {
            ++index;
        }
    }
}

std::span<const ScenePrimitiveChange> Scene::BeginRenderCommit(uint64_t serial) const {
    if (_lastCommitSerial == serial) return _committedChanges;
    if (!_commitComplete) {
        for (const auto& change : _committedChanges) {
            auto& state = _renderSlots[change.Id.Slot];
            if (change.Dirty.HasFlag(PrimitiveDirtyKind::Removed)) {
                if (!state.RemovedId.IsValid()) state.RemovedId = change.Id;
            } else if (FindPrimitive(change.Id)) {
                state.PendingId = change.Id;
                state.Pending |= change.Dirty;
            }
            EnqueueRenderSlot(change.Id.Slot);
        }
    }
    _commitStats = {};
    ObserveRenderDependencies();
    _committedChanges.clear();
    _commitStats.DirtySlots = _dirtySlots.size();
    for (const uint32_t slot : _dirtySlots) {
        auto& state = _renderSlots[slot];
        if (state.RemovedId.IsValid())
            _committedChanges.push_back({state.RemovedId, PrimitiveDirtyKind::Removed});
        const auto proxy = FindPrimitive(state.PendingId);
        if (proxy) {
            _committedChanges.push_back({state.PendingId, state.Pending});
            state.RenderRevision = proxy->GetRenderDataRevision();
            state.TransformRevision = proxy->GetTransformRevision();
            if (!state.Observed && (!proxy->UsesRenderChangeNotifications() || proxy->HasPendingRenderResources())) {
                state.Observed = true;
                _renderObservers.push_back(state.PendingId);
            }
        }
        state.Pending = {};
        state.PendingId = state.RemovedId = {};
        state.Enqueued = false;
    }
    _dirtySlots.clear();
    _lastCommitSerial = serial;
    _commitComplete = false;
    return _committedChanges;
}

bool Scene::CompleteRenderCommit(uint64_t serial, bool success) const noexcept {
    if (_lastCommitSerial != serial) return false;
    if (!success || _commitComplete) return true;
    for (const auto& change : _committedChanges) {
        auto& state = _renderSlots[change.Id.Slot];
        if (change.Dirty.HasFlag(PrimitiveDirtyKind::Removed)) {
            if (state.CommittedGeneration == change.Id.Generation) state.CommittedGeneration = 0;
        } else {
            state.CommittedGeneration = change.Id.Generation;
        }
    }
    _commitComplete = true;
    return true;
}

SceneObjectId Scene::GetLightId(const LightSceneProxy* proxy) const noexcept {
    const auto found = _lightByPointer.find(proxy);
    return found == _lightByPointer.end() ? SceneObjectId{} : found->second;
}

Nullable<LightSceneProxy*> Scene::FindLight(SceneObjectId id) const noexcept {
    if (!id.IsValid() || id.Slot >= _lightSlots.Generation.size() ||
        _lightSlots.Generation[id.Slot] != id.Generation || _lightSlots.Packed[id.Slot] == kInvalidPacked)
        return nullptr;
    return _lightProxies[_lightSlots.Packed[id.Slot]].get();
}

}  // namespace radray
