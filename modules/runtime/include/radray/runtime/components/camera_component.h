#pragma once

#include <radray/basic_math.h>
#include <radray/runtime/components/scene_component.h>

namespace radray {

class CameraComponent;

/// 相机组件 (对应 UE5 的 UCameraComponent)。View = 自身世界变换的逆, Proj = 左手透视,
/// aspect 由视口在填充时给出, 故相机不感知具体窗口。
///
/// 相机不产出 viewport；后端 Y 方向由 runtime 的 MakeViewport 唯一处理，投影矩阵保持后端无关。
class CameraComponent : public SceneComponent {
public:
    CameraComponent() noexcept = default;
    ~CameraComponent() noexcept override = default;

    /// 设置透视投影参数。fovYRadians 为竖直视场角(弧度)。
    void SetPerspective(float fovYRadians, float nearZ, float farZ) noexcept;

    float GetFovY() const noexcept { return _fovY; }
    float GetNearZ() const noexcept { return _nearZ; }
    float GetFarZ() const noexcept { return _farZ; }

    /// View 矩阵:世界变换的逆。相机看向自身 +Z(左手)。
    Eigen::Matrix4f ComputeViewMatrix() const noexcept;

    /// Proj 矩阵:左手透视。aspect = 宽 / 高。
    Eigen::Matrix4f ComputeProjMatrix(float aspect) const noexcept;

    /// ViewProj = Proj * View。aspect = 宽 / 高。
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
