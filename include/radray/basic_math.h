#pragma once

#include <cmath>
#include <numbers>
#include <Eigen/Dense>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

template <typename T>
requires(std::is_integral_v<T>)
constexpr bool IsPowerOf2(T v) noexcept {
    return v && !(v & (v - 1));
}

constexpr uint64_t CalcAlign(uint64_t value, uint64_t align) noexcept {
    return (value + (align - 1)) & ~(align - 1);
}

// 因为是2的幂次，二进制表示时最高位为1，其余位为0
// 因此通过将所有位置成1再加1，就可以得到2的幂次
// 通过最高位是1，右移一位，则此高位变成1，从而做或，使得次高位和最高为为1
// |= 2，则是将前4位变成1
// |= 4，前8位
// |= 8，前16位
// |= 16，前32位
constexpr uint32_t RoundUpPow2(uint32_t v) noexcept {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}
constexpr uint64_t RoundUpPow2(uint64_t v) noexcept {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

template <typename T>
requires(std::is_floating_point_v<T>)
constexpr T Degree(T value) noexcept { return value * (T(180) / std::numbers::pi_v<T>); }

template <typename T>
requires(std::is_floating_point_v<T>)
constexpr T Radian(T value) noexcept { return value * (std::numbers::pi_v<T> / T(180)); }

template <typename T>
requires(std::is_floating_point_v<T>)
T Lerp(T a, T b, T t) noexcept {
    return std::fma(b, t, std::fma(-a, t, a));  // a + (b - a) * t
}

template <typename LhsDerived, typename RhsDerived>
typename Eigen::ScalarBinaryOpTraits<typename LhsDerived::Scalar, typename RhsDerived::Scalar>::ReturnType AbsDot(const Eigen::MatrixBase<LhsDerived>& lhs, const Eigen::MatrixBase<RhsDerived>& rhs) noexcept {
    return std::abs(lhs.dot(rhs));
}

template <typename T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> PerspectiveLH(T fovy, T aspect, T zNear, T zFar) noexcept {
    T tanHalfFovy = std::tan(fovy / T(2));
    Eigen::Matrix<T, 4, 4> result = Eigen::Matrix<T, 4, 4>::Zero();
    result.coeffRef(0, 0) = T(1) / (aspect * tanHalfFovy);
    result.coeffRef(1, 1) = T(1) / tanHalfFovy;
    result.coeffRef(2, 2) = zFar / (zFar - zNear);
    result.coeffRef(3, 2) = T(1);
    result.coeffRef(2, 3) = -(zFar * zNear) / (zFar - zNear);
    return result;
}

template <typename T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> LookAtFrontLH(const Eigen::Vector<T, 3>& eye, const Eigen::Vector<T, 3>& front, const Eigen::Vector<T, 3>& up) noexcept {
    Eigen::Vector<T, 3> f = front;
    Eigen::Vector<T, 3> s = up.cross(f).normalized();
    Eigen::Vector<T, 3> u = f.cross(s);
    Eigen::Matrix<T, 4, 4> result;
    result << s.x(), s.y(), s.z(), -s.dot(eye),
        u.x(), u.y(), u.z(), -u.dot(eye),
        f.x(), f.y(), f.z(), -f.dot(eye),
        0, 0, 0, 1;
    return result;
}

template <typename T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> LookAtLH(const Eigen::Vector<T, 3>& eye, const Eigen::Vector<T, 3>& center, const Eigen::Vector<T, 3>& up) noexcept {
    Eigen::Vector<T, 3> front = (center - eye).normalized();
    return LookAtFrontLH(eye, front, up);
}

template <typename T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 3, 3> LookRotation(const Eigen::Vector<T, 3>& forward, const Eigen::Vector<T, 3>& up) noexcept {
    Eigen::Vector<T, 3> f = forward;
    Eigen::Vector<T, 3> s = up.cross(forward).normalized();
    Eigen::Vector<T, 3> u = f.cross(s);
    Eigen::Matrix<T, 3, 3> mat;
    mat << s, u, f;
    return mat;
}

template <typename T>
requires(std::is_floating_point_v<T>)
void DecomposeTransform(const Eigen::Matrix<T, 4, 4>& m, Eigen::Vector<T, 3>& translation, Eigen::Quaternion<T>& rotation, Eigen::Vector<T, 3>& scale) noexcept {
    Eigen::Affine3f aff(m);
    Eigen::Matrix3f rot, sc;
    aff.computeRotationScaling(&rot, &sc);
    translation = aff.translation();
    rotation = Eigen::Quaternion<T>{rot};
    scale = sc.diagonal();
}

}  // namespace radray

template <typename T>
requires(std::is_base_of_v<Eigen::DenseBase<T>, T>)
struct std::formatter<T> : public radray::OStreamFormatter<char> {};

template <typename T>
struct std::formatter<Eigen::WithFormat<T>> : public radray::OStreamFormatter<char> {};

template <typename T, int Rows, int Cols>
struct std::formatter<Eigen::Matrix<T, Rows, Cols>> : public std::formatter<std::string> {
    auto format(Eigen::Matrix<T, Rows, Cols> const& val, format_context& ctx) const -> decltype(ctx.out()) {
        Eigen::IOFormat matFmt{Eigen::FullPrecision, 0, ", ", "\n", "", "", "[", "]"};
        return std::formatter<Eigen::WithFormat<Eigen::Matrix<T, Rows, Cols>>>::format(val.format(matFmt), ctx);
    }
};

template <typename T, int Size>
struct std::formatter<Eigen::Vector<T, Size>> : public std::formatter<std::string> {
    auto format(Eigen::Vector<T, Size> const& val, format_context& ctx) const -> decltype(ctx.out()) {
        Eigen::IOFormat matFmt{Eigen::FullPrecision, Eigen::DontAlignCols, "", ", ", "", "", "<", ">"};
        return std::formatter<Eigen::WithFormat<Eigen::Vector<T, Size>>>::format(val.format(matFmt), ctx);
    }
};

template <typename T>
struct std::formatter<Eigen::Quaternion<T>> : public std::formatter<std::string> {
    auto format(Eigen::Quaternion<T> const& val, format_context& ctx) const -> decltype(ctx.out()) {
        return std::format_to(ctx.out(), "<{}, {}, {}, {}>", val.x(), val.y(), val.z(), val.w());
    }
};
