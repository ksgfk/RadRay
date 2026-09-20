#include "world_render_bridge.h"

#include <radray/scope_guard.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/render_system.h>

namespace radray {

WorldRenderBridge::WorldRenderBridge(World& world, RenderSystem& renderer)
    : _world(world), _renderer(renderer), _writer(renderer.ClaimSceneWriterGT(renderer.CreateSceneGT())) {}

WorldRenderBridge::~WorldRenderBridge() noexcept {
    if (_state != RenderConnectionState::Disconnected) RADRAY_ABORT("Bridge requires explicit Disconnect");
}

void WorldRenderBridge::CheckCanModify() const noexcept {
    if (_collecting) RADRAY_ABORT("Cannot mutate World during render collection");
}

void WorldRenderBridge::Initialize() {
    const size_t actors = _world.GetActors().size();
    for (size_t i = 0; i < actors; ++i) {
        auto* actor = _world.GetActors()[i].get();
        const size_t components = actor->GetOwnedComponents().size();
        for (size_t j = 0; j < components && actor->IsLive(); ++j) {
            auto* component = actor->GetOwnedComponents()[j].get();
            if (auto scene = dynamic_cast<SceneComponent*>(component); scene && scene->IsRegistered() && scene->IsLive()) Create(*scene);
        }
    }
    _state = RenderConnectionState::Connected;
}

void WorldRenderBridge::Disconnect() {
    if (_state == RenderConnectionState::Disconnected) return;
    CheckCanModify();
    _state = RenderConnectionState::Disconnecting;
    const size_t actors = _world.GetActors().size();
    for (size_t i = 0; i < actors; ++i) {
        auto* actor = _world.GetActors()[i].get();
        const size_t components = actor->GetOwnedComponents().size();
        for (size_t j = 0; j < components; ++j) {
            auto* component = actor->GetOwnedComponents()[j].get();
            if (auto scene = dynamic_cast<SceneComponent*>(component)) Destroy(*scene);
        }
    }
    _renderer.ReleaseSceneWriterGT(GetSceneId());
    _renderer.DestroySceneGT(GetSceneId());
    _state = RenderConnectionState::Disconnected;
}

void WorldRenderBridge::Create(SceneComponent& component) {
    CheckCanModify();
    if ((_state != RenderConnectionState::Connecting && _state != RenderConnectionState::Connected) || !component.IsLive()) return;
    if (component._renderConnection) {
        if (*component._renderConnection != GetSceneId()) RADRAY_ABORT("Component has another render connection");
        return;
    }
    component._renderConnection = GetSceneId();
    _world.BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world.EndCallback(); });
    component.CreateRenderState(_writer);
}

void WorldRenderBridge::Destroy(SceneComponent& component) {
    CheckCanModify();
    if (!component._renderConnection) return;
    if (*component._renderConnection != GetSceneId()) RADRAY_ABORT("Stale component render connection");
    component._renderConnection.reset();
    Remove(component);
    _world.BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world.EndCallback(); });
    component.DestroyRenderState(_writer);
    Remove(component);
}

void WorldRenderBridge::Queue(SceneComponent& component, RenderDirtyFlag flag) {
    CheckCanModify();
    if ((_state != RenderConnectionState::Connecting && _state != RenderConnectionState::Connected) || !component.IsLive() || !component._renderConnection) return;
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
    _renderer.SetCollecting(true);
    auto guard = MakeScopeGuard([this]() noexcept { _renderer.SetCollecting(false); _collecting = false; });
    while (!_updates.empty()) {
        auto& component = *_updates.back();
        if (component.IsLive()) {
            component.CollectRenderUpdates(_writer, component._renderDirty);
        }
        Remove(component);
    }
}

}  // namespace radray
