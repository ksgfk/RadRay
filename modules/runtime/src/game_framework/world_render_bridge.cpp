#include "world_render_bridge.h"

#include <radray/scope_guard.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/render_system.h>

namespace radray {

WorldRenderBridge::WorldRenderBridge(World& world, RenderSystem& renderer)
    : _world(world), _renderer(renderer), _writer(renderer.ClaimSceneWriterGT(renderer.CreateSceneGT())) {}

WorldRenderBridge::~WorldRenderBridge() noexcept { Disconnect(); }

void WorldRenderBridge::CheckCanModify() const noexcept {
    if (_collecting) RADRAY_ABORT("Cannot mutate World during render collection");
}

void WorldRenderBridge::Initialize() {
    for (const auto& actor : _world.GetActors()) {
        for (const auto& component : actor->GetOwnedComponents()) {
            if (auto scene = dynamic_cast<SceneComponent*>(component.get()); scene && scene->IsRegistered()) Create(*scene);
        }
    }
}

void WorldRenderBridge::Disconnect() {
    if (!_connected) return;
    CheckCanModify();
    _connected = false;
    for (const auto& actor : _world.GetActors()) {
        for (const auto& component : actor->GetOwnedComponents()) {
            if (auto scene = dynamic_cast<SceneComponent*>(component.get()); scene && scene->IsRegistered()) Destroy(*scene);
        }
    }
    _renderer.ReleaseSceneWriterGT(GetSceneId());
    _renderer.DestroySceneGT(GetSceneId());
}

void WorldRenderBridge::Create(SceneComponent& component) {
    CheckCanModify();
    component.CreateRenderState(_writer);
}

void WorldRenderBridge::Destroy(SceneComponent& component) {
    CheckCanModify();
    Remove(component);
    component.DestroyRenderState(_writer);
    Remove(component);
}

void WorldRenderBridge::Queue(SceneComponent& component, RenderDirtyFlag flag) {
    CheckCanModify();
    if (!_connected) return;
    if (component._renderQueueIndex == std::numeric_limits<size_t>::max()) {
        component._renderQueueIndex = _updates.size();
        _updates.push_back(&component);
    }
    component._renderDirty |= flag;
}

void WorldRenderBridge::Remove(SceneComponent& component) noexcept {
    const auto index = component._renderQueueIndex;
    if (index != std::numeric_limits<size_t>::max()) {
        auto moved = _updates.back();
        _updates[index] = moved;
        moved->_renderQueueIndex = index;
        _updates.pop_back();
    }
    component._renderQueueIndex = std::numeric_limits<size_t>::max();
    component._renderDirty = {};
}

void WorldRenderBridge::Collect() {
    CheckCanModify();
    _collecting = true;
    auto guard = MakeScopeGuard([this]() noexcept { _collecting = false; });
    while (!_updates.empty()) {
        auto& component = *_updates.back();
        component.CollectRenderUpdates(_writer, component._renderDirty);
        Remove(component);
    }
}

}  // namespace radray
