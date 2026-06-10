#pragma once

#include <radray/basic_math.h>

namespace radray {

/// Render-side mirror of a PrimitiveComponent.
/// Holds GPU resources and produces MeshDrawCommands.
/// 对应 UE5 的 FPrimitiveSceneProxy。
/// 当前为最小化占位，后续实现渲染管线时完善。
class PrimitiveSceneProxy {
public:
    PrimitiveSceneProxy() noexcept = default;
    PrimitiveSceneProxy(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy(PrimitiveSceneProxy&&) = delete;
    PrimitiveSceneProxy& operator=(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy& operator=(PrimitiveSceneProxy&&) = delete;
    virtual ~PrimitiveSceneProxy() noexcept = default;

    /// 获取世界变换矩阵
    const Eigen::Matrix4f& GetWorldMatrix() const noexcept { return _worldMatrix; }
    void SetWorldMatrix(const Eigen::Matrix4f& matrix) noexcept { _worldMatrix = matrix; }

private:
    Eigen::Matrix4f _worldMatrix{Eigen::Matrix4f::Identity()};
};

}  // namespace radray
