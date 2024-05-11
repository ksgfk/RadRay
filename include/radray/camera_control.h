#pragma once

#include <radray/basic_math.h>

namespace radray {

class CameraControl {
public:
    void Orbit(Eigen::Vector3f& position, Eigen::Quaternionf& rotation) noexcept;
    void PanXY(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept;
    void PanZ(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept;

public:
    Eigen::Vector2f nowPos{};
    Eigen::Vector2f lastPos{};
    float panDelta{};
    bool canFly{};
    bool canOrbit{};
    bool canPanXY{};
    bool canPanZWithDist{};
    float distance{1.0f};
    float minDistance{0.05f};
    float orbitSpeed{0.002f};
    float panXYSpeed{0.002f};
    float panZSpeed{0.05f};
};

}  // namespace radray
