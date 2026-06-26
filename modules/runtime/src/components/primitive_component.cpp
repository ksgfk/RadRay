#include <radray/runtime/components/primitive_component.h>

#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render/scene.h>
#include <radray/runtime/render/renderer.h>

namespace radray {

PrimitiveComponent::~PrimitiveComponent() noexcept = default;

RuntimeTypeId PrimitiveComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<PrimitiveComponent>;
}

void PrimitiveComponent::OnRegister() {
    SceneComponent::OnRegister();
    _renderers = BuildRenderers();
    RegisterRenderers();
}

void PrimitiveComponent::OnUnregister() {
    UnregisterRenderers();
    _renderers.clear();
    SceneComponent::OnUnregister();
}

void PrimitiveComponent::OnTransformChanged() {
    const Eigen::Matrix4f world = GetWorldMatrix();
    for (auto& renderer : _renderers) {
        renderer->SetWorldMatrix(world);
    }
}

void PrimitiveComponent::RecreateRenderers() {
    if (!IsRegistered()) {
        return;
    }
    UnregisterRenderers();
    _renderers.clear();
    _renderers = BuildRenderers();
    RegisterRenderers();
}

void PrimitiveComponent::RegisterRenderers() {
    if (_renderers.empty()) {
        return;
    }
    srp::Scene* scene = GetScene();
    const Eigen::Matrix4f world = GetWorldMatrix();
    for (auto& renderer : _renderers) {
        renderer->SetWorldMatrix(world);
        scene->AddRenderer(renderer.get());
    }
}

void PrimitiveComponent::UnregisterRenderers() {
    if (_renderers.empty()) {
        return;
    }
    srp::Scene* scene = GetScene();
    for (auto& renderer : _renderers) {
        scene->RemoveRenderer(renderer.get());
    }
}

srp::Scene* PrimitiveComponent::GetScene() const noexcept {
    return GetWorld().Get()->GetScene();
}

}  // namespace radray
