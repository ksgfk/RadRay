#include <radray/runtime/components/primitive_component.h>

#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/renderer/scene.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>

namespace radray {

PrimitiveComponent::~PrimitiveComponent() noexcept = default;

void PrimitiveComponent::OnRegister() {
    SceneComponent::OnRegister();
    auto proxy = CreateSceneProxy();
    if (proxy) {
        proxy->SetWorldMatrix(GetWorldMatrix());
        _sceneProxy = proxy.get();
        GetScene()->AddPrimitive(std::move(proxy));
    }
}

void PrimitiveComponent::OnUnregister() {
    if (_sceneProxy) {
        GetScene()->RemovePrimitive(_sceneProxy);
        _sceneProxy = nullptr;
    }
    SceneComponent::OnUnregister();
}

void PrimitiveComponent::OnTransformChanged() {
    if (_sceneProxy) {
        _sceneProxy->SetWorldMatrix(GetWorldMatrix());
    }
}

void PrimitiveComponent::RecreateSceneProxy() {
    if (!IsRegistered()) {
        return;
    }
    if (_sceneProxy) {
        GetScene()->RemovePrimitive(_sceneProxy);
        _sceneProxy = nullptr;
    }
    auto proxy = CreateSceneProxy();
    if (proxy) {
        proxy->SetWorldMatrix(GetWorldMatrix());
        _sceneProxy = proxy.get();
        GetScene()->AddPrimitive(std::move(proxy));
    }
}

Scene* PrimitiveComponent::GetScene() const noexcept {
    return GetWorld().Get()->GetScene();
}

}  // namespace radray
