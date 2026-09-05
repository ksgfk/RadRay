#include <radray/runtime/render_framework/render_bounds.h>

#include <cmath>

namespace radray {

bool AxisAlignedBounds::IsFiniteValid() const noexcept {
    return Min.allFinite() && Max.allFinite() && (Min.array() <= Max.array()).all();
}

bool SphereBounds::IsFiniteValid() const noexcept {
    return Center.allFinite() && std::isfinite(Radius) && Radius >= 0;
}

AxisAlignedBounds TransformBounds(const AxisAlignedBounds& local, const Eigen::Matrix4f& localToWorld) noexcept {
    if (!local.IsFiniteValid() || !localToWorld.allFinite() ||
        !localToWorld.row(3).isApprox(Eigen::RowVector4f{0, 0, 0, 1}, 1e-6f)) {
        return {};
    }
    const Eigen::Vector3f center = localToWorld.block<3, 3>(0, 0) * local.Center() + localToWorld.block<3, 1>(0, 3);
    const Eigen::Vector3f extent = localToWorld.block<3, 3>(0, 0).cwiseAbs() * local.Extents();
    AxisAlignedBounds result{center - extent, center + extent};
    return result.IsFiniteValid() ? result : AxisAlignedBounds{};
}

}  // namespace radray
