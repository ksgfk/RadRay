#include <radray/runtime/game_framework/world.h>

#include <algorithm>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/game_framework/actor.h>

namespace radray {

World::World() = default;

World::World(Application* app)
    : _app(app) {
}

World::~World() noexcept {
    // 销毁所有 Actor(每个都会触发 UnregisterAllComponents)
    while (!_actors.empty()) {
        DestroyActor(_actors.back().get());
    }
}

void World::DestroyActor(Actor* actor) {
    CheckCanModify();
    auto it = std::find_if(_actors.begin(), _actors.end(),
                           [actor](const unique_ptr<Actor>& ptr) {
                               return ptr.get() == actor;
                           });
    if (it == _actors.end()) {
        return;
    }
    actor->UnregisterAllComponents();
    actor->OnDestroyed();
    actor->_world = nullptr;
    _actors.erase(it);
}

void World::Tick(float deltaTime) {
    CheckCanModify();
    for (auto& actor : _actors) {
        actor->Tick(deltaTime);
    }
}

Actor* World::SpawnActor(unique_ptr<Actor> actor) {
    CheckCanModify();
    if (actor == nullptr) {
        return nullptr;
    }
    Actor* raw = actor.get();
    raw->_world = this;
    _actors.push_back(std::move(actor));
    raw->RegisterAllComponents();
    raw->OnSpawned();
    return raw;
}

void World::CheckCanModify() const noexcept {
    if (_isFlushingRenderUpdates) RADRAY_ABORT("Cannot mutate World during render collection");
}

void World::QueueRenderUpdate(SceneComponent& component, RenderDirtyFlag flag) {
    CheckCanModify();
    if (component._renderQueueIndex == std::numeric_limits<size_t>::max()) {
        _renderUpdates.push_back(&component);
        component._renderQueueIndex = _renderUpdates.size() - 1;
    }
    component._renderDirty |= flag;
}

void World::RemoveRenderUpdate(SceneComponent& component) noexcept {
    const size_t index = component._renderQueueIndex;
    if (index != std::numeric_limits<size_t>::max()) {
        SceneComponent* moved = _renderUpdates.back();
        _renderUpdates[index] = moved;
        moved->_renderQueueIndex = index;
        _renderUpdates.pop_back();
    }
    component._renderQueueIndex = std::numeric_limits<size_t>::max();
    component._renderDirty = {};
}

void World::FlushRenderUpdates(SceneUpdateBatch& batch) {
    CheckCanModify();
    if (!batch.Empty()) RADRAY_ABORT("Scene batch must be empty before collection");
    _isFlushingRenderUpdates = true;
    auto guard = MakeScopeGuard([this]() noexcept { _isFlushingRenderUpdates = false; });
    batch.RemovePrimitives.insert(batch.RemovePrimitives.end(), _removedPrimitives.begin(), _removedPrimitives.end());
    while (!_renderUpdates.empty()) {
        SceneComponent& component = *_renderUpdates.back();
        component.CollectRenderUpdates(batch, component._renderDirty);
        RemoveRenderUpdate(component);
    }
    _removedPrimitives.clear();
}

void World::UpdateRenderAssetUse(std::optional<AssetId> previous, Nullable<const StaticMesh*> next) {
    if (previous && next && *previous == next->GetAssetId()) {
        const auto it = _renderAssetUses.find(*previous);
        if (it == _renderAssetUses.end() || it->second.Mesh != next.Get()) RADRAY_ABORT("Render asset identity changed");
        return;
    }
    if (previous) {
        const auto it = _renderAssetUses.find(*previous);
        if (it == _renderAssetUses.end()) RADRAY_ABORT("Missing render asset use");
        if (--it->second.Count == 0) _renderAssetUses.erase(it);
    }
    if (next) {
        const auto [it, inserted] = _renderAssetUses.try_emplace(next->GetAssetId(), RenderAssetUse{0, next.Get()});
        if (!inserted && it->second.Mesh != next.Get()) RADRAY_ABORT("Render asset identity changed");
        ++it->second.Count;
    }
}

void World::RetainRenderAssets(Nullable<AssetManager*> assets, vector<StreamingAssetRef<StaticMesh>>& refs) const {
    if (!refs.empty()) RADRAY_ABORT("Flight asset refs must be empty before collection");
    if (!_renderAssetUses.empty() && !assets) RADRAY_ABORT("Render assets require an AssetManager");
    refs.reserve(_renderAssetUses.size());
    for (const auto& [id, use] : _renderAssetUses) {
        auto ref = assets->Find<StaticMesh>(id);
        if (ref.Get().Get() != use.Mesh) RADRAY_ABORT("Render asset is missing or its identity changed");
        refs.push_back(std::move(ref));
    }
}

PrimitiveId World::AllocatePrimitiveId() {
    CheckCanModify();
    if (!_freePrimitiveIndices.empty()) {
        const uint32_t index = _freePrimitiveIndices.back();
        _freePrimitiveIndices.pop_back();
        return {index, _primitiveGenerations[index]};
    }
    if (_primitiveGenerations.size() >= std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Primitive IDs exhausted");
    const auto index = static_cast<uint32_t>(_primitiveGenerations.size());
    _primitiveGenerations.push_back(1);
    return {index, 1};
}

void World::ReleasePrimitiveId(PrimitiveId id) {
    CheckCanModify();
    if (!id.IsValid() || id.Index >= _primitiveGenerations.size() || _primitiveGenerations[id.Index] != id.Generation) RADRAY_ABORT("Invalid primitive ID release");
    if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Primitive generation exhausted");
    ++_primitiveGenerations[id.Index];
    _freePrimitiveIndices.push_back(id.Index);
}

}  // namespace radray
