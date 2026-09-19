#include <radray/runtime/game_framework/world.h>

#include <algorithm>

#include <radray/logger.h>
#include <radray/runtime/game_framework/actor.h>
#include "world_render_bridge.h"

namespace radray {

World::World() = default;

World::World(Application* app)
    : _app(app) {
}

World::~World() noexcept {
    DetachFromRendering();
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
    if (!_tickEnabled) return;
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
    if (_renderBridge) _renderBridge->CheckCanModify();
}

SceneId World::AttachToRendering(RenderSystem& renderer) {
    CheckCanModify();
    if (_renderBridge) RADRAY_ABORT("World is already connected to rendering");
    _renderBridge = make_unique<WorldRenderBridge>(*this, renderer);
    _renderBridge->Initialize();
    return _renderBridge->GetSceneId();
}

void World::DetachFromRendering() {
    CheckCanModify();
    if (_renderBridge) {
        _renderBridge->Disconnect();
        _renderBridge.reset();
    }
}

std::optional<SceneId> World::GetRenderSceneId() const noexcept {
    return _renderBridge ? std::optional<SceneId>{_renderBridge->GetSceneId()} : std::nullopt;
}

void World::CollectRenderUpdates() {
    if (_renderBridge) _renderBridge->Collect();
}

void World::CreateComponentRenderState(SceneComponent& component) {
    if (_renderBridge) _renderBridge->Create(component);
}

void World::DestroyComponentRenderState(SceneComponent& component) {
    if (_renderBridge) _renderBridge->Destroy(component);
}

void World::QueueRenderUpdate(SceneComponent& component, RenderDirtyFlag flag) {
    CheckCanModify();
    if (_renderBridge) _renderBridge->Queue(component, flag);
}

}  // namespace radray
