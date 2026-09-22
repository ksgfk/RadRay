#include "world_render_bridge.h"

#include <algorithm>
#include <radray/profiler.h>
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

void WorldRenderBridge::Initialize() {
    const size_t actors = _world.GetActors().size();
    for (size_t i = 0; i < actors; ++i) {
        auto* actor = _world.GetActors()[i].get();
        const size_t components = actor->GetOwnedComponents().size();
        for (size_t j = 0; j < components && actor->IsLive(); ++j) {
            auto* component = actor->GetOwnedComponents()[j].get();
            if (auto source = dynamic_cast<RenderComponent*>(component); source && source->IsRegistered() && source->IsLive()) Create(*source);
        }
    }
    _state = RenderConnectionState::Connected;
}

void WorldRenderBridge::Disconnect() {
    if (_state == RenderConnectionState::Disconnected) return;
    _state = RenderConnectionState::Disconnecting;
    while (!_sources.empty()) Destroy(*_sources.back());
    _renderer.ReleaseSceneWriterGT(GetSceneId());
    _renderer.DestroySceneGT(GetSceneId());
    _state = RenderConnectionState::Disconnected;
}

void WorldRenderBridge::Create(RenderComponent& component) {
    if ((_state != RenderConnectionState::Connecting && _state != RenderConnectionState::Connected) || !component.IsLive()) return;
    if (component._renderIndex != kNotQueued) return;
    if (_sources.size() == kNotQueued) RADRAY_ABORT("Too many render sources");
    component._renderIndex = static_cast<uint32_t>(_sources.size());
    _sources.push_back(&component);
    _world.BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world.EndCallback(); });
    component.CreateRenderState(_writer);
}

void WorldRenderBridge::Destroy(RenderComponent& component) {
    const auto index = component._renderIndex;
    if (index == kNotQueued) return;
    RemoveUpdate(component);
    _sources[index] = _sources.back();
    _sources[index]->_renderIndex = index;
    _sources.pop_back();
    component._renderIndex = kNotQueued;
    _world.BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world.EndCallback(); });
    component.DestroyRenderState(_writer);
}

void WorldRenderBridge::Queue(RenderComponent& component, RenderDirtyFlag flag) {
    if (!component._renderDirty) {
        component._renderQueueIndex = static_cast<uint32_t>(_updates.size());
        _updates.push_back(&component);
    }
    component._renderDirty |= flag;
}

void WorldRenderBridge::RemoveUpdate(RenderComponent& component) noexcept {
    if (component._renderDirty) {
        const auto index = component._renderQueueIndex;
        auto* moved = _updates.back();
        _updates[index] = moved;
        moved->_renderQueueIndex = index;
        _updates.pop_back();
    }
    component._renderDirty = {};
}

void WorldRenderBridge::Collect() {
    RADRAY_PROFILE_SCOPE_N("WorldRenderBridge::Collect");
    if (_updates.empty()) return;
    _renderer.SetCollecting(true);
    auto guard = MakeScopeGuard([this]() noexcept { _renderer.SetCollecting(false); });
    if (_updates.size() >= kCollectSortThreshold) {
        std::sort(_updates.begin(), _updates.end(), [](const RenderComponent* lhs, const RenderComponent* rhs) noexcept {
            return reinterpret_cast<uintptr_t>(lhs) < reinterpret_cast<uintptr_t>(rhs);
        });
    }
    SceneCapture capture{_writer};
    for (auto* component : _updates) {
        if (component->IsLive()) {
            component->CollectRenderUpdates(capture, component->_renderDirty);
        }
        component->_renderDirty = {};
    }
    _updates.clear();
}

}  // namespace radray
