#include <cmath>
#include <Eigen/Dense>
#include <radray/types.h>

namespace radray {

template <typename T>
requires(std::is_floating_point_v<T>)
T Lerp(T a, T b, T t) noexcept {
    return std::fma(b, t, std::fma(-a, t, a));  // a + (b - a) * t
}

template <typename LhsDerived, typename RhsDerived>
typename Eigen::ScalarBinaryOpTraits<typename LhsDerived::Scalar, typename RhsDerived::Scalar>::ReturnType AbsDot(const Eigen::MatrixBase<LhsDerived>& lhs, const Eigen::MatrixBase<RhsDerived>& rhs) noexcept {
    return std::abs(lhs.dot(rhs));
}

using Vector2f = Eigen::Vector<float, 2>;
using Vector3f = Eigen::Vector<float, 3>;
using Vector4f = Eigen::Vector<float, 4>;
using Vector2d = Eigen::Vector<double, 2>;
using Vector3d = Eigen::Vector<double, 3>;
using Vector4d = Eigen::Vector<double, 4>;
using Vector2i = Eigen::Vector<int, 2>;
using Vector3i = Eigen::Vector<int, 3>;
using Vector4i = Eigen::Vector<int, 4>;

}  // namespace radray
