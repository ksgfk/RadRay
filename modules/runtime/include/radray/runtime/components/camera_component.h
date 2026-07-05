#pragma once

#include <radray/basic_math.h>
#include <radray/runtime/components/scene_component.h>

namespace radray {

class CameraComponent;

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

    RuntimeTypeId GetTypeId() const noexcept override;

    /// 设置透视投影参数。fovYRadians 为竖直视场角(弧度)。
    void SetPerspective(float fovYRadians, float nearZ, float farZ) noexcept;

    float GetFovY() const noexcept { return _fovY; }
    float GetNearZ() const noexcept { return _nearZ; }
    float GetFarZ() const noexcept { return _farZ; }

    /// View 矩阵:世界变换的逆。相机看向自身 +Z(左手)。
    Eigen::Matrix4f ComputeViewMatrix() const noexcept;

    /// Proj 矩阵:左手透视。aspect = width / height。
    Eigen::Matrix4f ComputeProjMatrix(float aspect) const noexcept;

    /// ViewProj = Proj * View。aspect = width / height。
    Eigen::Matrix4f ComputeViewProjMatrix(float aspect) const noexcept;

    /// 相机世界位置 (等于 SceneComponent::GetWorldLocation)。
    Eigen::Vector3f GetEyePosition() const noexcept;

private:
    float _fovY{Radian(60.0f)};
    float _nearZ{0.1f};
    float _farZ{100.0f};
};

template <>
struct RuntimeTypeTrait<CameraComponent> {
    static constexpr RuntimeTypeId value{0x98a4682f, 0x6b73, 0x4c6d, 0x8a, 0xbf, 0x44, 0x8e, 0x58, 0x1f, 0x7d, 0xf2};
};

}  // namespace radray
