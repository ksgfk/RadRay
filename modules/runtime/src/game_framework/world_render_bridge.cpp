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
            if (auto scene = dynamic_cast<SceneComponent*>(component); scene && scene->IsRegistered() && scene->IsLive()) CreateTransform(*scene);
            if (auto source = dynamic_cast<RenderComponent*>(component); source && source->IsRegistered() && source->IsLive()) Create(*source);
        }
    }
    _state = RenderConnectionState::Connected;
}

void WorldRenderBridge::Disconnect() {
    if (_state == RenderConnectionState::Disconnected) return;
    _state = RenderConnectionState::Disconnecting;
    while (!_sources.empty()) Destroy(*_sources.back());
    while (!_transformSources.empty()) DestroyTransform(*_transformSources.back());
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
    if (!component.UsesSceneTransform()) ++_world._legacyRenderSources;
    _world.BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world.EndCallback(); });
    component.CreateRenderState(_writer);
}

void WorldRenderBridge::Destroy(RenderComponent& component) {
    const auto index = component._renderIndex;
    if (index == kNotQueued) return;
    if (!component.UsesSceneTransform()) --_world._legacyRenderSources;
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
    if (_updates.empty() && _world._localTransformChanges.empty() && _world._renderTransformRoots.empty() && _world._renderTransformRevision == _world._transformRevision) return;
    _renderer.SetCollecting(true);
    auto guard = MakeScopeGuard([this]() noexcept { _renderer.SetCollecting(false); });
    if (_world._localTransformChanges.size() >= kCollectSortThreshold) {
        std::sort(_world._localTransformChanges.begin(), _world._localTransformChanges.end(), [](const SceneComponent* lhs, const SceneComponent* rhs) noexcept {
            return reinterpret_cast<uintptr_t>(lhs) < reinterpret_cast<uintptr_t>(rhs);
        });
    }
    for (auto* component : _world._localTransformChanges) {
        component->_localTransformQueueIndex = kNotQueued;
        if (!component->_sceneTransformId.IsValid()) continue;
        const auto parent = component->_parent ? component->_parent->_sceneTransformId : TransformId{};
        _writer.SetLocalTransform(component->_sceneTransformId, parent,
                                  {component->_relativeLocation, component->_relativeRotation, component->_relativeScale});
    }
    _world._localTransformChanges.clear();
    SceneCapture capture{_writer};
    _world.CollectTransforms(capture);
    if (_updates.size() >= kCollectSortThreshold) {
        std::sort(_updates.begin(), _updates.end(), [](const RenderComponent* lhs, const RenderComponent* rhs) noexcept {
            return reinterpret_cast<uintptr_t>(lhs) < reinterpret_cast<uintptr_t>(rhs);
        });
    }
    for (auto* component : _updates) {
        if (component->_renderDirty && component->IsLive()) {
            component->CollectRenderUpdates(capture, component->_renderDirty);
        }
        component->_renderDirty = {};
    }
    _updates.clear();
}

void WorldRenderBridge::CreateTransform(SceneComponent& component) {
    if (_state == RenderConnectionState::Disconnecting || _state == RenderConnectionState::Disconnected || !component.IsLive()) return;
    for (Nullable<SceneComponent*> node{&component}; node && !node->_sceneTransformId.IsValid(); node = node->_parent)
        _creationChain.push_back(node.Get());
    while (!_creationChain.empty()) {
        auto* node = _creationChain.back();
        _creationChain.pop_back();
        const auto parent = node->_parent ? node->_parent->_sceneTransformId : TransformId{};
        node->_sceneTransformId = _writer.CreateTransform(parent, {node->_relativeLocation, node->_relativeRotation, node->_relativeScale});
        node->_sceneTransformIndex = static_cast<uint32_t>(_transformSources.size());
        _transformSources.push_back(node);
    }
}
void WorldRenderBridge::DestroyTransform(SceneComponent& component) {
    const auto index = component._sceneTransformIndex;
    if (index == kNotQueued) return;
    _writer.RemoveTransform(component._sceneTransformId);
    _transformSources[index] = _transformSources.back();
    _transformSources[index]->_sceneTransformIndex = index;
    _transformSources.pop_back();
    component._sceneTransformIndex = kNotQueued;
    component._sceneTransformId = {};
}

}  // namespace radray
