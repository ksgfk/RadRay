#include <radray/camera_control.h>

namespace radray {

void CameraControl::Orbit(Eigen::Vector3f& position, Eigen::Quaternionf& rotation) noexcept {
    if (CanFly) {
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector2f delta = NowPos - LastPos;
        Eigen::AngleAxisf rotX(delta.x() * OrbitSpeed, Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf rotY(delta.y() * OrbitSpeed, right);
        rotation = rotX * rotY * rotation;
    }
    if (CanOrbit) {
        Eigen::Vector3f pos = position;
        Eigen::Vector3f front = (rotation * Eigen::Vector3f{0, 0, 1}).normalized();
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector3f target = pos + front * Distance;
        Eigen::Vector2f delta = NowPos - LastPos;
        Eigen::Vector3f local = pos - target;
        Eigen::AngleAxisf rotX(delta.x() * OrbitSpeed, Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf rotY(delta.y() * OrbitSpeed, right);
        local = rotX * rotY * local;
        position = local + target;
        rotation = rotX * rotY * rotation;
    }
    if (CanFly || CanOrbit) {
        LastPos = NowPos;
    }
}

void CameraControl::PanXY(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept {
    if (CanPanXY) {
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector3f up = (rotation * Eigen::Vector3f{0, 1, 0}).normalized();
        Eigen::Vector2f delta = NowPos - LastPos;
        Eigen::Vector3f pos = position;
        float speed = PanXYSpeed * Distance * 0.35f;
        position = pos + up * (delta.y() * speed) + right * (-delta.x() * speed);
    }
    if (CanPanXY) {
        LastPos = NowPos;
    }
}

void CameraControl::PanZ(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept {
    float dist;
    if (CanPanZWithDist) {
        dist = PanDelta * PanZSpeed;
    } else {
        if (PanDelta > 0) {
            if (Distance > MinDistance) {
                dist = std::min(PanDelta * PanZSpeed, Distance - MinDistance);
                dist *= Distance;
                Distance = std::max(MinDistance, Distance - dist);
            } else {
                dist = 0;
                Distance = MinDistance;
            }
        } else {
            dist = PanDelta * PanZSpeed;
            dist *= Distance;
            Distance = Distance - dist;
        }
    }
    Eigen::Vector3f front = (rotation * Eigen::Vector3f{0, 0, 1}).normalized();
    Eigen::Vector3f pos = position;
    position = pos + front * dist;
}

}  // namespace radray
