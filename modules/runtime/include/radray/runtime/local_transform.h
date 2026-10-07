#pragma once

#include <radray/basic_math.h>

namespace radray {

/// Packed local TRS. Rotation is x/y/z/w; no SIMD padding is transmitted.
struct LocalTransform {
    float Rotation[4]{0, 0, 0, 1};
    float Translation[3]{0, 0, 0};
    float Scale[3]{1, 1, 1};
    LocalTransform() noexcept = default;
    LocalTransform(const Eigen::Vector3f& translation, const Eigen::Quaternionf& rotation, const Eigen::Vector3f& scale) noexcept;
    Eigen::Matrix4f ToMatrix() const noexcept;
};
static_assert(sizeof(LocalTransform) == 40);

}  // namespace radray
