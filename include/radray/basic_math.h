#pragma once

#include <cmath>
#include <numbers>
#include <sstream>
#include <ostream>
#include <format>

#include <Eigen/Dense>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

template <class T>
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

template <class T>
requires(std::is_floating_point_v<T>)
constexpr T Degree(T value) noexcept { return value * (T(180) / std::numbers::pi_v<T>); }

template <class T>
requires(std::is_floating_point_v<T>)
constexpr T Radian(T value) noexcept { return value * (std::numbers::pi_v<T> / T(180)); }

template <class T>
requires(std::is_floating_point_v<T>)
T Lerp(T a, T b, T t) noexcept {
    return std::fma(b, t, std::fma(-a, t, a));  // a + (b - a) * t
}

template <class LhsDerived, class RhsDerived>
class Eigen::ScalarBinaryOpTraits<class LhsDerived::Scalar, class RhsDerived::Scalar>::ReturnType AbsDot(const Eigen::MatrixBase<LhsDerived>& lhs, const Eigen::MatrixBase<RhsDerived>& rhs) noexcept {
    return std::abs(lhs.dot(rhs));
}

template <class T>
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

template <class T>
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

template <class T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> LookAtLH(const Eigen::Vector<T, 3>& eye, const Eigen::Vector<T, 3>& center, const Eigen::Vector<T, 3>& up) noexcept {
    Eigen::Vector<T, 3> front = (center - eye).normalized();
    return LookAtFrontLH(eye, front, up);
}

template <class T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 3, 3> LookRotation(const Eigen::Vector<T, 3>& forward, const Eigen::Vector<T, 3>& up) noexcept {
    Eigen::Vector<T, 3> f = forward;
    Eigen::Vector<T, 3> s = up.cross(forward).normalized();
    Eigen::Vector<T, 3> u = f.cross(s);
    Eigen::Matrix<T, 3, 3> mat;
    mat << s, u, f;
    return mat;
}

template <class T>
requires(std::is_floating_point_v<T>)
void DecomposeTransform(const Eigen::Matrix<T, 4, 4>& m, Eigen::Vector<T, 3>& translation, Eigen::Quaternion<T>& rotation, Eigen::Vector<T, 3>& scale) noexcept {
    Eigen::Affine3f aff(m);
    Eigen::Matrix3f rot, sc;
    aff.computeRotationScaling(&rot, &sc);
    translation = aff.translation();
    rotation = Eigen::Quaternion<T>{rot};
    scale = sc.diagonal();
}

template <class Type, int Size>
std::string to_string(const Eigen::Vector<Type, Size>& v) {
    Eigen::IOFormat efmt{Eigen::FullPrecision, Eigen::DontAlignCols, "", ", ", "", "", "<", ">"};
    std::basic_stringbuf<char> buf{};
    std::basic_ostream<char> output{&buf};
    output << v.format(efmt);
    output.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    return buf.str();
}

template <class Type, int Rows, int Cols>
std::string to_string(const Eigen::Matrix<Type, Rows, Cols>& v) {
    Eigen::IOFormat efmt{Eigen::FullPrecision, 0, ", ", "\n", "", "", "[", "]"};
    std::basic_stringbuf<char> buf{};
    std::basic_ostream<char> output{&buf};
    output << v.format(efmt);
    output.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    return buf.str();
}

template <class Type>
std::string to_string(const Eigen::Quaternion<Type>& v) {
    return std::format("<{}, {}, {}, {}>", v.x(), v.y(), v.z(), v.w());
}

}  // namespace radray

template <class Type, int Size, class CharT>
struct std::formatter<Eigen::Vector<Type, Size>, CharT> : std::formatter<string, CharT> {
    template <class FormatContext>
    auto format(Eigen::Vector<Type, Size> const& val, FormatContext& ctx) const {
        return formatter<string, CharT>::format(radray::to_string<Type, Size>(val), ctx);
    }
};

template <class Type, int Rows, int Cols, class CharT>
struct std::formatter<Eigen::Matrix<Type, Rows, Cols>, CharT> : std::formatter<string, CharT> {
    template <class FormatContext>
    auto format(Eigen::Matrix<Type, Rows, Cols> const& val, FormatContext& ctx) const {
        return formatter<string, CharT>::format(radray::to_string<Type, Rows, Cols>(val), ctx);
    }
};

template <class Type, class CharT>
struct std::formatter<Eigen::Quaternion<Type>, CharT> : std::formatter<string, CharT> {
    template <class FormatContext>
    auto format(Eigen::Quaternion<Type> const& val, FormatContext& ctx) const {
        return formatter<string, CharT>::format(radray::to_string<Type>(val), ctx);
    }
};
