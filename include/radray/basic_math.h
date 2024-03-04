#pragma once

#include <cmath>
#include <Eigen/Dense>
#include <radray/types.h>
#include <radray/logger.h>
#include <spdlog/fmt/ostr.h>

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

}  // namespace radray

template <typename T>
requires(std::is_base_of_v<Eigen::DenseBase<T>, T>)
struct fmt::formatter<T> : fmt::ostream_formatter {};

template <typename T>
struct fmt::formatter<Eigen::WithFormat<T>> : fmt::ostream_formatter {};

template <typename T, int Rows, int Cols>
struct fmt::formatter<Eigen::Matrix<T, Rows, Cols>> : fmt::formatter<std::string> {
  auto format(Eigen::Matrix<T, Rows, Cols> const& val, format_context& ctx) const -> decltype(ctx.out()) {
    Eigen::IOFormat matFmt{Eigen::FullPrecision, 0, ", ", "\n", "", "", "[", "]"};
    fmt::formatter<Eigen::WithFormat<Eigen::Matrix<T, Rows, Cols>>> fmtIns{};
    return fmtIns.format(val.format(matFmt), ctx);
  }
};

template <typename T, int Size>
struct fmt::formatter<Eigen::Vector<T, Size>> : fmt::formatter<std::string> {
  auto format(Eigen::Vector<T, Size> const& val, format_context& ctx) const -> decltype(ctx.out()) {
    Eigen::IOFormat matFmt{Eigen::FullPrecision, Eigen::DontAlignCols, "", ", ", "", "", "<", ">"};
    fmt::formatter<Eigen::WithFormat<Eigen::Vector<T, Size>>> fmtIns{};
    return fmtIns.format(val.format(matFmt), ctx);
  }
};

template <typename T>
struct fmt::formatter<Eigen::Quaternion<T>> : fmt::formatter<std::string> {
  auto format(Eigen::Quaternion<T> const& val, format_context& ctx) const -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "<{}, {}, {}, {}>", val.x(), val.y(), val.z(), val.w());
  }
};
