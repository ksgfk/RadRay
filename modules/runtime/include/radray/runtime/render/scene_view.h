#pragma once

#include <cstdint>

#include <radray/basic_math.h>

namespace radray::srp {

/// 渲染一帧所用的视图参数(最小化,对应 SRP 的 per-view 常量来源)。
/// 仅承载相机矩阵与视口尺寸;pass 据此填自己的 space0。
/// 等价于旧 `radray::SceneView`,迁移到本框架命名空间下。
struct SceneView {
    Eigen::Matrix4f ViewMatrix{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f ProjMatrix{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f ViewProjMatrix{Eigen::Matrix4f::Identity()};
    Eigen::Vector3f EyePosition{Eigen::Vector3f::Zero()};
    uint32_t ViewportWidth{0};
    uint32_t ViewportHeight{0};
};

}  // namespace radray::srp
