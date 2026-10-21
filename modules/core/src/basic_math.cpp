#include <radray/basic_math.h>

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

}  // namespace radray
