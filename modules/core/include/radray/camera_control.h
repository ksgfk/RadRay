#pragma once

#include <radray/basic_math.h>

namespace radray {

class CameraControl {
public:
    void Orbit(Eigen::Vector3f& position, Eigen::Quaternionf& rotation) noexcept;
    void Pan(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept;
    void Dolly(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept;
    void Reset() noexcept;
    void SetOrbitTarget(const Eigen::Vector3f& target) noexcept;
    void UpdateDistance(const Eigen::Vector3f& position) noexcept;

public:
    Eigen::Vector2f CurrentMousePos{0, 0};
    Eigen::Vector2f LastMousePos{0, 0};
    float WheelDelta{0.0f};

    bool IsOrbiting{false};
    bool IsPanning{false};
    bool IsDollying{false};

    Eigen::Vector3f OrbitCenter{0, 0, 0};

    float Distance{5.0f};
    float MinDistance{0.1f};
    float MaxDistance{1000.0f};

    float OrbitSensitivity{1.0f};
    float PanSensitivity{1.0f};
    float DollySensitivity{0.1f};

    bool UseTrackball{false};
    bool InvertZoom{false};
    bool AutoPerspective{true};
};

}  // namespace radray
