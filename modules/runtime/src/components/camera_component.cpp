#include <radray/runtime/components/camera_component.h>

#include <radray/runtime/renderer/scene_renderer.h>

namespace radray {

RuntimeTypeId CameraComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<CameraComponent>;
}

void CameraComponent::SetPerspective(float fovYRadians, float nearZ, float farZ) noexcept {
    _fovY = fovYRadians;
    _nearZ = nearZ;
    _farZ = farZ;
}

Eigen::Matrix4f CameraComponent::ComputeViewMatrix() const noexcept {
    return LookAt(GetWorldRotation(), GetWorldLocation());
}

/// Proj 矩阵:左手透视。aspect = width / height。
Eigen::Matrix4f CameraComponent::ComputeProjMatrix(float aspect) const noexcept {
    return PerspectiveLH<float>(_fovY, aspect, _nearZ, _farZ);
}

void CameraComponent::FillSceneView(SceneView& view, uint32_t width, uint32_t height) const noexcept {
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    view.ViewMatrix = ComputeViewMatrix();
    view.ProjMatrix = ComputeProjMatrix(aspect);
    view.ViewProjMatrix = view.ProjMatrix * view.ViewMatrix;
    view.EyePosition = GetWorldLocation();
    view.ViewportWidth = width;
    view.ViewportHeight = height;
}

}  // namespace radray
