#pragma once

#include <limits>
#include <radray/basic_math.h>

namespace radray {

struct AxisAlignedBounds {
    Eigen::Vector3f Min{Eigen::Vector3f::Constant(std::numeric_limits<float>::infinity())};
    Eigen::Vector3f Max{Eigen::Vector3f::Constant(-std::numeric_limits<float>::infinity())};

    bool IsFiniteValid() const noexcept;
    Eigen::Vector3f Center() const noexcept { return Min * 0.5f + Max * 0.5f; }
    Eigen::Vector3f Extents() const noexcept { return Max * 0.5f - Min * 0.5f; }
};

struct SphereBounds {
    Eigen::Vector3f Center{Eigen::Vector3f::Zero()};
    float Radius{0};
    bool IsFiniteValid() const noexcept;
};

/// Invalid local bounds or non-finite/non-affine transforms return invalid bounds.
AxisAlignedBounds TransformBounds(const AxisAlignedBounds& local, const Eigen::Matrix4f& localToWorld) noexcept;

}  // namespace radray
