#include <radray/runtime/components/primitive_component.h>

#include <radray/logger.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>

namespace radray {

PrimitiveComponent::~PrimitiveComponent() noexcept {
    DestroyRenderState();
}

void PrimitiveComponent::OnRegister() {
    CreateRenderState();
}

void PrimitiveComponent::OnUnregister() {
    DestroyRenderState();
}

void PrimitiveComponent::MarkRenderStateDirty() {
    if (!IsRegistered()) {
        return;
    }

    if (_renderStateCreated) {
        DestroyRenderState();
    }
    if (ShouldCreateRenderState()) {
        CreateRenderState();
    }
}

bool PrimitiveComponent::ShouldCreateRenderState() const {
    return true;
}

unique_ptr<PrimitiveSceneProxy> PrimitiveComponent::CreateSceneProxy() {
    return nullptr;
}

void PrimitiveComponent::OnTransformChanged() {
    if (_sceneProxy != nullptr) {
        const auto& transform = GetWorldMatrix();
        _sceneProxy->SetLocalToWorld(transform);
        RADRAY_ASSERT(!transform.allFinite() || _sceneProxy->GetLocalToWorld().isApprox(transform));
    }
}

Scene* PrimitiveComponent::GetScene() const noexcept {
    Nullable<World*> world = GetWorld();
    if (!world) {
        return nullptr;
    }
    return world.Get()->GetScene();
}

void PrimitiveComponent::CreateRenderState() {
    if (_renderStateCreated || !ShouldCreateRenderState()) {
        return;
    }

    Scene* scene = GetScene();
    if (scene != nullptr) {
        _sceneProxy = scene->AddPrimitive(CreateSceneProxy()).Get();
    }
    _renderStateCreated = _sceneProxy != nullptr;
}

void PrimitiveComponent::DestroyRenderState() noexcept {
    if (!_renderStateCreated) {
        return;
    }

    Scene* scene = GetScene();
    if (scene != nullptr && _sceneProxy != nullptr) {
        scene->RemovePrimitive(_sceneProxy);
    }
    _sceneProxy = nullptr;
    _renderStateCreated = false;
}

}  // namespace radray
