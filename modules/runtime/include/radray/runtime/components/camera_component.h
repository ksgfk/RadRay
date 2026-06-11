#pragma once

#include <radray/basic_math.h>
#include <radray/runtime/components/scene_component.h>

namespace radray {

struct SceneView;

/// 相机组件。独立的 SceneComponent,从自身世界变换反算 View 矩阵,
/// 结合投影参数产出渲染所需的 SceneView。对应 UE5 的 UCameraComponent。
///
/// 设计(最小化):
/// - View 矩阵 = 世界变换的逆(由 GetWorldRotation/GetWorldLocation 直接构造)。
/// - Proj 矩阵 = 左手透视,aspect 由视口尺寸在填充时给出(相机不感知具体窗口)。
/// - 不处理后端视口差异(如 Vulkan 的 Y-flip):那是录制处的视口设置,SceneView 保持后端无关。
class CameraComponent : public SceneComponent {
public:
    CameraComponent() noexcept = default;
    ~CameraComponent() noexcept override = default;

    /// 设置透视投影参数。fovYRadians 为竖直视场角(弧度)。
    void SetPerspective(float fovYRadians, float nearZ, float farZ) noexcept {
        _fovY = fovYRadians;
        _nearZ = nearZ;
        _farZ = farZ;
    }

    float GetFovY() const noexcept { return _fovY; }
    float GetNearZ() const noexcept { return _nearZ; }
    float GetFarZ() const noexcept { return _farZ; }

    /// View 矩阵:世界变换的逆。相机看向自身 +Z(左手)。
    Eigen::Matrix4f ComputeViewMatrix() const noexcept {
        return LookAt(GetWorldRotation(), GetWorldLocation());
    }

    /// Proj 矩阵:左手透视。aspect = width / height。
    Eigen::Matrix4f ComputeProjMatrix(float aspect) const noexcept {
        return PerspectiveLH<float>(_fovY, aspect, _nearZ, _farZ);
    }

    /// 按给定视口尺寸填充 SceneView(View/Proj/ViewProj + 视口)。
    void FillSceneView(SceneView& view, uint32_t width, uint32_t height) const noexcept;

private:
    float _fovY{Radian(60.0f)};
    float _nearZ{0.1f};
    float _farZ{100.0f};
};

}  // namespace radray
