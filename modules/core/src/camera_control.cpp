#include <radray/camera_control.h>

#include <cmath>
#include <algorithm>
#include <numbers>

namespace radray {

static float NormalizeAngle(float angle) {
    constexpr float PI = std::numbers::pi_v<float>;
    constexpr float TWO_PI = 2.0f * PI;
    angle = std::fmod(angle, TWO_PI);
    if (angle > PI) angle -= TWO_PI;
    if (angle < -PI) angle += TWO_PI;
    return angle;
}

void CameraControl::Orbit(Eigen::Vector3f& position, Eigen::Quaternionf& rotation) noexcept {
    if (!IsOrbiting) return;
    Eigen::Vector2f delta = CurrentMousePos - LastMousePos;
    if (delta.squaredNorm() < 1e-6f) {
        LastMousePos = CurrentMousePos;
        return;
    }
    if (UseTrackball) {
        Eigen::Vector3f axis(-delta.y(), delta.x(), 0.0f);
        float angle = delta.norm() * OrbitSensitivity;
        angle = NormalizeAngle(angle);
        if (std::abs(angle) > 1e-6f) {
            Eigen::Vector3f worldAxis = rotation * axis.normalized();
            Eigen::Quaternionf deltaRotation(Eigen::AngleAxisf(angle, worldAxis));
            rotation = deltaRotation * rotation;
            rotation.normalize();
            Eigen::Vector3f localPos = position - OrbitCenter;
            localPos = deltaRotation * localPos;
            position = OrbitCenter + localPos;
            Distance = localPos.norm();
        }
    } else {
        float yawAngle = -delta.x() * OrbitSensitivity;
        yawAngle = NormalizeAngle(yawAngle);
        Eigen::Quaternionf yawRotation(Eigen::AngleAxisf(yawAngle, Eigen::Vector3f::UnitY()));
        Eigen::Vector3f right = rotation * Eigen::Vector3f::UnitX();
        float pitchAngle = -delta.y() * OrbitSensitivity;
        pitchAngle = NormalizeAngle(pitchAngle);
        Eigen::Quaternionf pitchRotation(Eigen::AngleAxisf(pitchAngle, right));
        Eigen::Quaternionf deltaRotation = yawRotation * pitchRotation;
        rotation = deltaRotation * rotation;
        rotation.normalize();
        Eigen::Vector3f localPos = position - OrbitCenter;
        localPos = deltaRotation * localPos;
        position = OrbitCenter + localPos;
        Distance = localPos.norm();
    }
    LastMousePos = CurrentMousePos;
}

void CameraControl::Pan(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept {
    if (!IsPanning) return;
    Eigen::Vector2f delta = CurrentMousePos - LastMousePos;
    if (delta.squaredNorm() < 1e-6f) {
        LastMousePos = CurrentMousePos;
        return;
    }
    Eigen::Vector3f right = rotation * Eigen::Vector3f::UnitX();
    Eigen::Vector3f up = rotation * Eigen::Vector3f::UnitY();
    float panSpeed = PanSensitivity * Distance * 0.5f;
    Eigen::Vector3f panVector = right * (-delta.x() * panSpeed) + up * (delta.y() * panSpeed);
    position += panVector;
    OrbitCenter += panVector;
    LastMousePos = CurrentMousePos;
}

void CameraControl::Dolly(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept {
    if (!IsDollying && std::abs(WheelDelta) < 1e-6f) return;
    Eigen::Vector3f forward = rotation * Eigen::Vector3f(0, 0, -1);
    float zoomFactor = WheelDelta * DollySensitivity;
    if (InvertZoom) zoomFactor = -zoomFactor;
    float moveDistance = zoomFactor * Distance;
    float newDistance = Distance - moveDistance;
    newDistance = Clamp(newDistance, MinDistance, MaxDistance);
    float actualMove = Distance - newDistance;
    Distance = newDistance;
    position += forward * actualMove;
    WheelDelta = 0.0f;
}

void CameraControl::Reset() noexcept {
    CurrentMousePos = Eigen::Vector2f::Zero();
    LastMousePos = Eigen::Vector2f::Zero();
    WheelDelta = 0.0f;
    IsOrbiting = false;
    IsPanning = false;
    IsDollying = false;
}

void CameraControl::SetOrbitTarget(const Eigen::Vector3f& target) noexcept {
    OrbitCenter = target;
}

void CameraControl::UpdateDistance(const Eigen::Vector3f& position) noexcept {
    Distance = (position - OrbitCenter).norm();
    Distance = Clamp(Distance, MinDistance, MaxDistance);
}

}  // namespace radray
