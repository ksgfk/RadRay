#include <radray/runtime/local_transform.h>

#include <algorithm>

namespace radray {

LocalTransform::LocalTransform(const Eigen::Vector3f& translation, const Eigen::Quaternionf& rotation, const Eigen::Vector3f& scale) noexcept {
    std::copy_n(rotation.coeffs().data(), 4, Rotation);
    std::copy_n(translation.data(), 3, Translation);
    std::copy_n(scale.data(), 3, Scale);
}
Eigen::Matrix4f LocalTransform::ToMatrix() const noexcept {
    return ComposeTransform(Eigen::Vector3f{Translation[0], Translation[1], Translation[2]},
                            Eigen::Quaternionf{Rotation[3], Rotation[0], Rotation[1], Rotation[2]},
                            Eigen::Vector3f{Scale[0], Scale[1], Scale[2]});
}

}  // namespace radray
