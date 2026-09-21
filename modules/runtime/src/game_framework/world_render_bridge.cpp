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
    if (component._renderConnection.IsValid()) {
        if (component._renderConnection != GetSceneId()) RADRAY_ABORT("Component has another render connection");
        return;
    }
    component._renderConnection = GetSceneId();
    _world.BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world.EndCallback(); });
    component.CreateRenderState(_writer);
}

void WorldRenderBridge::Destroy(SceneComponent& component) {
    CheckCanModify();
    if (!component._renderConnection.IsValid()) return;
    if (component._renderConnection != GetSceneId()) RADRAY_ABORT("Stale component render connection");
    component._renderConnection = {};
    Remove(component);
    _world.BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world.EndCallback(); });
    component.DestroyRenderState(_writer);
    Remove(component);
}

void WorldRenderBridge::Queue(SceneComponent& component, RenderDirtyFlag flag) {
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
    RADRAY_PROFILE_SCOPE_N("WorldRenderBridge::Collect");
    CheckCanModify();
    _collecting = true;
    _renderer.SetCollecting(true);
    auto guard = MakeScopeGuard([this]() noexcept { _renderer.SetCollecting(false); _collecting = false; });
    // 队列较大时按组件地址排序后顺序处理：Mutate 的入队顺序是随机的，排序把对组件与 writer 热状态的
    // 随机访问变成近似按分配顺序的连续访问。小队的工作集仍在缓存内，排序只是纯成本，故设阈值。
    // Collect 期间禁止入队/出队（CheckCanModify），队列稳定，可以安全整体排序。
    if (_updates.size() >= kCollectSortThreshold) {
        std::sort(_updates.begin(), _updates.end(), [](const SceneComponent* lhs, const SceneComponent* rhs) noexcept {
            return reinterpret_cast<uintptr_t>(lhs) < reinterpret_cast<uintptr_t>(rhs);
        });
    }
    for (auto* component : _updates) {
        if (component->IsLive()) {
            component->CollectRenderUpdates(_writer, component->_renderDirty);
        }
        component->_renderQueueIndex = std::numeric_limits<size_t>::max();
        component->_renderDirty = {};
    }
    _updates.clear();
}

}  // namespace radray
