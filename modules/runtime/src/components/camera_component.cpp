#include <radray/runtime/components/camera_component.h>

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

}  // namespace radray
