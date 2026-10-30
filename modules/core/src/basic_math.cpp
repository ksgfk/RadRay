#include <radray/basic_math.h>

#include <algorithm>
#include <limits>
#include <numeric>

#include <radray/logger.h>

namespace radray {

AffineTransform::AffineTransform(const Eigen::Matrix4f& matrix) noexcept {
    const float* source = matrix.data();
#ifdef RADRAY_IS_DEBUG
    if (source[3] != 0 || source[7] != 0 || source[11] != 0 || source[15] != 1) RADRAY_ABORT("Transform must be affine");
#endif
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 3; ++row) Values[column * 3 + row] = source[column * 4 + row];
    }
}

Eigen::Matrix4f AffineTransform::ToMatrix() const noexcept {
    Eigen::Matrix4f result;
    float* target = result.data();
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 3; ++row) target[column * 4 + row] = Values[column * 3 + row];
        target[column * 4 + 3] = column == 3 ? 1.0f : 0.0f;
    }
    return result;
}

Aabb::Aabb() noexcept {
    Min.fill(std::numeric_limits<float>::infinity());
    Max.fill(-std::numeric_limits<float>::infinity());
}

Aabb::Aabb(const Eigen::Vector3f& min, const Eigen::Vector3f& max) noexcept
    : Min{min.x(), min.y(), min.z()}, Max{max.x(), max.y(), max.z()} {}

bool Aabb::IsEmpty() const noexcept {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(Min[axis]) || !std::isfinite(Max[axis]) || Min[axis] > Max[axis]) return true;
    }
    return false;
}

Eigen::Vector3f Aabb::GetCenter() const noexcept {
    if (IsEmpty()) return Eigen::Vector3f::Zero();
    return {std::midpoint(Min[0], Max[0]), std::midpoint(Min[1], Max[1]), std::midpoint(Min[2], Max[2])};
}

Eigen::Vector3f Aabb::GetSize() const noexcept {
    if (IsEmpty()) return Eigen::Vector3f::Zero();
    return {Max[0] - Min[0], Max[1] - Min[1], Max[2] - Min[2]};
}

Eigen::Vector3f Aabb::GetExtent() const noexcept {
    if (IsEmpty()) return Eigen::Vector3f::Zero();
    return {std::midpoint(Max[0], -Min[0]), std::midpoint(Max[1], -Min[1]), std::midpoint(Max[2], -Min[2])};
}

void Aabb::Expand(const Eigen::Vector3f& point) noexcept {
    if (!point.allFinite()) return;
    if (IsEmpty()) {
        Min = Max = {point.x(), point.y(), point.z()};
        return;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        Min[axis] = std::min(Min[axis], point[axis]);
        Max[axis] = std::max(Max[axis], point[axis]);
    }
}

void Aabb::Expand(const Aabb& bounds) noexcept {
    if (bounds.IsEmpty()) return;
    if (IsEmpty()) {
        *this = bounds;
        return;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        Min[axis] = std::min(Min[axis], bounds.Min[axis]);
        Max[axis] = std::max(Max[axis], bounds.Max[axis]);
    }
}

bool Aabb::Contains(const Eigen::Vector3f& point) const noexcept {
    if (IsEmpty() || !point.allFinite()) return false;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (point[axis] < Min[axis] || point[axis] > Max[axis]) return false;
    }
    return true;
}

bool Aabb::Contains(const Aabb& bounds) const noexcept {
    if (IsEmpty() || bounds.IsEmpty()) return false;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (bounds.Min[axis] < Min[axis] || bounds.Max[axis] > Max[axis]) return false;
    }
    return true;
}

bool Aabb::Intersects(const Aabb& bounds) const noexcept {
    if (IsEmpty() || bounds.IsEmpty()) return false;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (Max[axis] < bounds.Min[axis] || bounds.Max[axis] < Min[axis]) return false;
    }
    return true;
}

Aabb Aabb::Intersection(const Aabb& bounds) const noexcept {
    if (!Intersects(bounds)) return {};
    Aabb result;
    for (size_t axis = 0; axis < 3; ++axis) {
        result.Min[axis] = std::max(Min[axis], bounds.Min[axis]);
        result.Max[axis] = std::min(Max[axis], bounds.Max[axis]);
    }
    return result;
}

Aabb Aabb::Transform(const AffineTransform& transform) const noexcept {
    if (IsEmpty()) return {};
    Aabb result;
    for (size_t row = 0; row < 3; ++row) {
        float min = transform.Values[9 + row];
        float max = min;
        for (size_t column = 0; column < 3; ++column) {
            const float value = transform.Values[column * 3 + row];
            const float a = value * Min[column];
            const float b = value * Max[column];
            min += std::min(a, b);
            max += std::max(a, b);
        }
        result.Min[row] = min;
        result.Max[row] = max;
    }
    return result;
}

}  // namespace radray
