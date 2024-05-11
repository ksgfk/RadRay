#include <radray/camera_control.h>

namespace radray {

void CameraControl::Orbit(Eigen::Vector3f& position, Eigen::Quaternionf& rotation) noexcept {
    if (canFly) {
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector2f delta = nowPos - lastPos;
        Eigen::AngleAxisf rotX(delta.x() * orbitSpeed, Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf rotY(delta.y() * orbitSpeed, right);
        rotation = rotX * rotY * rotation;
    }
    if (canOrbit) {
        Eigen::Vector3f pos = position;
        Eigen::Vector3f front = (rotation * Eigen::Vector3f{0, 0, 1}).normalized();
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector3f target = pos + front * distance;
        Eigen::Vector2f delta = nowPos - lastPos;
        Eigen::Vector3f local = pos - target;
        Eigen::AngleAxisf rotX(delta.x() * orbitSpeed, Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf rotY(delta.y() * orbitSpeed, right);
        local = rotX * rotY * local;
        position = local + target;
        rotation = rotX * rotY * rotation;
    }
    if (canFly || canOrbit) {
        lastPos = nowPos;
    }
}

void CameraControl::PanXY(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept {
    if (canPanXY) {
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector3f up = (rotation * Eigen::Vector3f{0, 1, 0}).normalized();
        Eigen::Vector2f delta = nowPos - lastPos;
        Eigen::Vector3f pos = position;
        float speed = panXYSpeed * distance * 0.35f;
        position = pos + up * (delta.y() * speed) + right * (-delta.x() * speed);
    }
    if (canPanXY) {
        lastPos = nowPos;
    }
}

void CameraControl::PanZ(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept {
    float dist;
    if (canPanZWithDist) {
        dist = panDelta * panZSpeed;
    } else {
        if (panDelta > 0) {
            if (distance > minDistance) {
                dist = std::min(panDelta * panZSpeed, distance - minDistance);
                dist *= distance;
                distance = std::max(minDistance, distance - dist);
            } else {
                dist = 0;
                distance = minDistance;
            }
        } else {
            dist = panDelta * panZSpeed;
            dist *= distance;
            distance = distance - dist;
        }
    }
    Eigen::Vector3f front = (rotation * Eigen::Vector3f{0, 0, 1}).normalized();
    Eigen::Vector3f pos = position;
    position = pos + front * dist;
}

}  // namespace radray
