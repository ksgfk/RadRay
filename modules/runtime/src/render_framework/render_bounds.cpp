#include <radray/runtime/render_framework/render_bounds.h>

#include <algorithm>
#include <cmath>

// Hot per-primitive paths are written against raw component arrays instead of Eigen expressions:
// in unoptimized builds every Eigen operator is an out-of-line call chain, and these functions run
// once per primitive per frame (snapshot transform, culling).

namespace radray {

bool AxisAlignedBounds::IsFiniteValid() const noexcept {
    const float* lo = Min.data();
    const float* hi = Max.data();
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(lo[i]) || !std::isfinite(hi[i]) || !(lo[i] <= hi[i])) return false;
    }
    return true;
}

bool SphereBounds::IsFiniteValid() const noexcept {
    const float* c = Center.data();
    return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]) && std::isfinite(Radius) && Radius >= 0;
}

AxisAlignedBounds TransformBounds(const AxisAlignedBounds& local, const Eigen::Matrix4f& localToWorld) noexcept {
    if (!local.IsFiniteValid()) return {};
    // Column-major: element (r, c) lives at m[c * 4 + r].
    const float* m = localToWorld.data();
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) return {};
    }
    // Bottom row must be affine; mirrors Eigen isApprox(row3, {0,0,0,1}, 1e-6f):
    // |a-b|^2 <= prec^2 * min(|a|^2, |b|^2) with |b|^2 == 1.
    {
        const float r0 = m[3], r1 = m[7], r2 = m[11], r3 = m[15];
        const float diff = r0 * r0 + r1 * r1 + r2 * r2 + (r3 - 1.0f) * (r3 - 1.0f);
        const float rowSq = r0 * r0 + r1 * r1 + r2 * r2 + r3 * r3;
        if (!(diff <= 1e-12f * std::min(rowSq, 1.0f))) return {};
    }
    const float* lo = local.Min.data();
    const float* hi = local.Max.data();
    float localCenter[3], localExtent[3];
    for (int i = 0; i < 3; ++i) {
        localCenter[i] = lo[i] * 0.5f + hi[i] * 0.5f;
        localExtent[i] = hi[i] * 0.5f - lo[i] * 0.5f;
    }
    AxisAlignedBounds result;
    float* outMin = result.Min.data();
    float* outMax = result.Max.data();
    for (int r = 0; r < 3; ++r) {
        float center = m[12 + r];
        float extent = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float v = m[c * 4 + r];
            center += v * localCenter[c];
            extent += std::abs(v) * localExtent[c];
        }
        outMin[r] = center - extent;
        outMax[r] = center + extent;
    }
    return result.IsFiniteValid() ? result : AxisAlignedBounds{};
}

}  // namespace radray
