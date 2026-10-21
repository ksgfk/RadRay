#include <radray/runtime/components/camera_component.h>

namespace radray {

void CameraComponent::SetPerspective(float fovYRadians, float nearZ, float farZ) noexcept {
    CheckCanModify();
    _projection = SceneProjection::Perspective;
    _fovY = fovYRadians;
    _nearZ = nearZ;
    _farZ = farZ;
}

void CameraComponent::SetOrthographic(float height, float nearZ, float farZ) noexcept {
    CheckCanModify();
    _projection = SceneProjection::Orthographic;
    _orthographicHeight = height;
    _nearZ = nearZ;
    _farZ = farZ;
}

Eigen::Matrix4f CameraComponent::ComputeViewMatrix() const noexcept {
    return LookAt(GetWorldRotation(), GetWorldLocation());
}

/// Proj 矩阵:左手透视。aspect = width / height。
Eigen::Matrix4f CameraComponent::ComputeProjMatrix(float aspect) const noexcept {
    if (_projection == SceneProjection::Orthographic) return OrthoLH<float>(-_orthographicHeight * aspect * 0.5f, _orthographicHeight * aspect * 0.5f, -_orthographicHeight * 0.5f, _orthographicHeight * 0.5f, _nearZ, _farZ);
    return PerspectiveLH<float>(_fovY, aspect, _nearZ, _farZ);
}

Eigen::Matrix4f CameraComponent::ComputeViewProjMatrix(float aspect) const noexcept {
    return ComputeProjMatrix(aspect) * ComputeViewMatrix();
}

Eigen::Vector3f CameraComponent::GetEyePosition() const noexcept {
    return GetWorldLocation();
}

}  // namespace radray
