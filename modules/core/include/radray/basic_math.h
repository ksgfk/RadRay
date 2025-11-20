#pragma once

#include <cmath>
#include <numbers>
#include <sstream>
#include <ostream>

#include <Eigen/Dense>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

template <class T>
struct IsEigenVector : std::false_type {};

template <class Scalar, int Rows, int Cols, int Options, int MaxRows, int MaxCols>
struct IsEigenVector<Eigen::Matrix<Scalar, Rows, Cols, Options, MaxRows, MaxCols>> : std::bool_constant<(Rows == 1 || Cols == 1)> {};

struct Viewport {
    float X;
    float Y;
    float Width;
    float Height;
    float MinDepth;
    float MaxDepth;
};

struct Rect {
    int32_t X;
    int32_t Y;
    uint32_t Width;
    uint32_t Height;
};

constexpr uint64_t Align(uint64_t value, uint64_t align) noexcept {
    return (value + (align - 1)) & ~(align - 1);
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

template <class T>
requires(std::is_integral_v<T> || std::is_floating_point_v<T>)
constexpr T Clamp(T value, T min, T max) noexcept {
    return std::max(min, std::min(max, value));
}

template <class LhsDerived, class RhsDerived>
typename Eigen::ScalarBinaryOpTraits<typename LhsDerived::Scalar, typename RhsDerived::Scalar>::ReturnType AbsDot(const Eigen::MatrixBase<LhsDerived>& lhs, const Eigen::MatrixBase<RhsDerived>& rhs) noexcept {
    return std::abs(lhs.dot(rhs));
}

template <class T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> PerspectiveLH(T fovy, T aspect, T zNear, T zFar) noexcept {
    T tanHalfFovy = std::tan(fovy / T(2));
    Eigen::Matrix<T, 4, 4> persp = Eigen::Matrix<T, 4, 4>::Zero();
    persp(0, 0) = T(1) / (aspect * tanHalfFovy);
    persp(1, 1) = T(1) / tanHalfFovy;
    persp(2, 2) = zFar / (zFar - zNear);
    persp(3, 2) = T(1);
    persp(2, 3) = -zNear * persp(2, 2);
    return persp;
}

template <class T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> OrthoLH(T left, T right, T bottom, T top, T zNear, T zFar) noexcept {
    Eigen::Matrix<T, 4, 4> ortho = Eigen::Matrix<T, 4, 4>::Zero();
    const T invWidth = T(1) / (right - left);
    const T invHeight = T(1) / (top - bottom);
    const T invDepth = T(1) / (zFar - zNear);

    ortho(0, 0) = T(2) * invWidth;
    ortho(1, 1) = T(2) * invHeight;
    ortho(2, 2) = invDepth;
    ortho(0, 3) = -(right + left) * invWidth;
    ortho(1, 3) = -(top + bottom) * invHeight;
    ortho(2, 3) = -zNear * invDepth;
    ortho(3, 3) = T(1);

    return ortho;
}

template <class T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> LookAtFrontLH(const Eigen::Vector<T, 3>& eye, const Eigen::Vector<T, 3>& front, const Eigen::Vector<T, 3>& up) noexcept {
    Eigen::Vector<T, 3> zAxis = front.normalized();
    Eigen::Vector<T, 3> xAxis = up.cross(zAxis).normalized();
    Eigen::Vector<T, 3> yAxis = zAxis.cross(xAxis);
    Eigen::Matrix<T, 4, 4> view = Eigen::Matrix<T, 4, 4>::Identity();
    view.template block<1, 3>(0, 0) = xAxis.transpose();
    view.template block<1, 3>(1, 0) = yAxis.transpose();
    view.template block<1, 3>(2, 0) = zAxis.transpose();
    view(0, 3) = -xAxis.dot(eye);
    view(1, 3) = -yAxis.dot(eye);
    view(2, 3) = -zAxis.dot(eye);
    return view;
}

template <class T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> LookAtLH(const Eigen::Vector<T, 3>& eye, const Eigen::Vector<T, 3>& center, const Eigen::Vector<T, 3>& up) noexcept {
    Eigen::Vector<T, 3> front = (center - eye).normalized();
    return LookAtFrontLH(eye, front, up);
}

template <class T>
requires(std::is_floating_point_v<T>)
Eigen::Matrix<T, 4, 4> LookAt(const Eigen::Quaternion<T>& quat, const Eigen::Vector<T, 3>& pos) noexcept {
    Eigen::Matrix<T, 4, 4> view = Eigen::Matrix4f::Identity();
    Eigen::Matrix<T, 3, 3> invRot = quat.conjugate().toRotationMatrix();
    view.template block<3, 3>(0, 0) = invRot;
    view.template block<3, 1>(0, 3) = -invRot * pos;
    return view;
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
string to_string(const Eigen::Vector<Type, Size>& v) {
    using str_buf = std::basic_stringbuf<char, std::char_traits<char>, allocator<char>>;
    Eigen::IOFormat efmt{Eigen::FullPrecision, Eigen::DontAlignCols, "", ", ", "", "", "<", ">"};
    str_buf buf{};
    std::basic_ostream<char> output{&buf};
    output << v.format(efmt);
    output.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    return string{buf.view()};
}

template <class Type, int Rows, int Cols>
string to_string(const Eigen::Matrix<Type, Rows, Cols>& v) {
    using str_buf = std::basic_stringbuf<char, std::char_traits<char>, allocator<char>>;
    Eigen::IOFormat efmt{Eigen::FullPrecision, 0, ", ", "\n", "", "", "[", "]"};
    str_buf buf{};
    std::basic_ostream<char> output{&buf};
    output << v.format(efmt);
    output.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    return string{buf.view()};
}

template <class Type>
string to_string(const Eigen::Quaternion<Type>& v) {
    return radray::format("<{}, {}, {}, {}>", v.x(), v.y(), v.z(), v.w());
}

}  // namespace radray

template <class Type, int Size, class CharT>
struct fmt::formatter<Eigen::Vector<Type, Size>, CharT> : fmt::formatter<radray::string, CharT> {
    template <class FormatContext>
    auto format(Eigen::Vector<Type, Size> const& val, FormatContext& ctx) const {
        return formatter<radray::string, CharT>::format(radray::to_string<Type, Size>(val), ctx);
    }
};

template <class Type, int Rows, int Cols, class CharT>
struct fmt::formatter<Eigen::Matrix<Type, Rows, Cols>, CharT> : fmt::formatter<radray::string, CharT> {
    template <class FormatContext>
    auto format(Eigen::Matrix<Type, Rows, Cols> const& val, FormatContext& ctx) const {
        return formatter<radray::string, CharT>::format(radray::to_string<Type, Rows, Cols>(val), ctx);
    }
};

template <class Type, class CharT>
struct fmt::formatter<Eigen::Quaternion<Type>, CharT> : fmt::formatter<radray::string, CharT> {
    template <class FormatContext>
    auto format(Eigen::Quaternion<Type> const& val, FormatContext& ctx) const {
        return formatter<radray::string, CharT>::format(radray::to_string<Type>(val), ctx);
    }
};
