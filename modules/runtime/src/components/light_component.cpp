#include <radray/runtime/components/light_component.h>

#include <algorithm>

#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {

LightComponent::~LightComponent() noexcept {
    DestroyRenderState();
}

RuntimeTypeId LightComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<LightComponent>;
}

void LightComponent::OnRegister() {
    CreateRenderState();
}

void LightComponent::OnUnregister() {
    DestroyRenderState();
}

void LightComponent::MarkRenderStateDirty() {
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

bool LightComponent::ShouldCreateRenderState() const {
    return _affectsWorld && _intensity > 0.0f;
}

unique_ptr<LightSceneProxy> LightComponent::CreateSceneProxy() const {
    return make_unique<LightSceneProxy>(*this);
}

Eigen::Vector4f LightComponent::GetLightPosition() const noexcept {
    Eigen::Vector3f worldLocation = GetWorldLocation();
    return Eigen::Vector4f{worldLocation.x(), worldLocation.y(), worldLocation.z(), 1.0f};
}

Eigen::Vector3f LightComponent::GetLightDirection() const noexcept {
    Eigen::Vector3f direction = GetWorldRotation() * Eigen::Vector3f::UnitZ();
    if (direction.squaredNorm() <= 1e-8f) {
        return Eigen::Vector3f::UnitZ();
    }
    return direction.normalized();
}

void LightComponent::SetIntensity(float intensity) noexcept {
    _intensity = std::max(intensity, 0.0f);
    MarkRenderStateDirty();
}

void LightComponent::SetLightColor(const Eigen::Vector3f& color) noexcept {
    _lightColor = color.cwiseMax(Eigen::Vector3f::Zero());
    MarkRenderStateDirty();
}

void LightComponent::SetAffectsWorld(bool affectsWorld) noexcept {
    _affectsWorld = affectsWorld;
    MarkRenderStateDirty();
}

void LightComponent::SetCastShadow(bool castShadow) noexcept {
    _castShadow = castShadow;
    MarkRenderStateDirty();
}

void LightComponent::OnTransformChanged() {
    MarkRenderStateDirty();
}

Scene* LightComponent::GetScene() const noexcept {
    Nullable<World*> world = GetWorld();
    if (!world) {
        return nullptr;
    }
    return world.Get()->GetScene();
}

void LightComponent::CreateRenderState() {
    if (_renderStateCreated || !ShouldCreateRenderState()) {
        return;
    }

    Scene* scene = GetScene();
    if (scene != nullptr) {
        _sceneProxy = scene->AddLight(this);
    }
    _renderStateCreated = _sceneProxy != nullptr;
}

void LightComponent::DestroyRenderState() noexcept {
    if (!_renderStateCreated) {
        return;
    }

    Scene* scene = GetScene();
    if (scene != nullptr && _sceneProxy != nullptr) {
        scene->RemoveLight(_sceneProxy);
    }
    _sceneProxy = nullptr;
    _renderStateCreated = false;
}

}  // namespace radray
