#pragma once

#include <radray/basic_math.h>

namespace radray {

class CameraControl {
public:
    void Orbit(Eigen::Vector3f& position, Eigen::Quaternionf& rotation) noexcept;
    void PanXY(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept;
    void PanZ(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept;

public:
    Eigen::Vector2f NowPos{};
    Eigen::Vector2f LastPos{};
    float PanDelta{};
    bool CanFly{};
    bool CanOrbit{};
    bool CanPanXY{};
    bool CanPanZWithDist{};
    float Distance{1.0f};
    float MinDistance{0.05f};
    float OrbitSpeed{0.002f};
    float PanXYSpeed{0.002f};
    float PanZSpeed{0.05f};
};

}  // namespace radray
