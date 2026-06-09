#pragma once

#include <span>

#include <radray/basic_math.h>
#include <radray/nullable.h>
#include <radray/runtime/components/actor_component.h>

namespace radray {

/// 有空间变换的组件。能形成父子 Attach 层级。
/// 对应 UE5 的 USceneComponent。
class SceneComponent : public ActorComponent {
public:
    SceneComponent() noexcept = default;
    ~SceneComponent() noexcept override;

    bool IsSceneComponent() const noexcept override { return true; }

    // ─── Transform ───

    /// 相对于父组件的变换
    const Eigen::Vector3f& GetRelativeLocation() const noexcept { return _relativeLocation; }
    const Eigen::Quaternionf& GetRelativeRotation() const noexcept { return _relativeRotation; }
    const Eigen::Vector3f& GetRelativeScale() const noexcept { return _relativeScale; }

    void SetRelativeLocation(const Eigen::Vector3f& location) noexcept;
    void SetRelativeRotation(const Eigen::Quaternionf& rotation) noexcept;
    void SetRelativeScale(const Eigen::Vector3f& scale) noexcept;

    /// 世界空间变换（递归计算 parent chain）
    Eigen::Vector3f GetWorldLocation() const noexcept;
    Eigen::Quaternionf GetWorldRotation() const noexcept;
    Eigen::Vector3f GetWorldScale() const noexcept;
    Eigen::Matrix4f GetWorldMatrix() const noexcept;

    /// 直接设置世界位置（反算出 relative）
    void SetWorldLocation(const Eigen::Vector3f& location) noexcept;
    void SetWorldRotation(const Eigen::Quaternionf& rotation) noexcept;

    // ─── Attach 层级 ───

    /// 将此组件挂到 parent 下
    void AttachTo(SceneComponent* parent) noexcept;

    /// 从父组件脱离
    void DetachFromParent() noexcept;

    Nullable<SceneComponent*> GetAttachParent() const noexcept { return _parent; }
    std::span<SceneComponent* const> GetAttachChildren() const noexcept { return _children; }

protected:
    /// Transform 变更后调用，派生类可覆写以标记渲染状态脏
    virtual void OnTransformChanged() {}

private:
    Eigen::Matrix4f ComputeLocalMatrix() const noexcept;

    // Relative transform（相对于 parent）
    Eigen::Quaternionf _relativeRotation{Eigen::Quaternionf::Identity()};
    Eigen::Vector3f _relativeLocation{Eigen::Vector3f::Zero()};
    Eigen::Vector3f _relativeScale{Eigen::Vector3f::Ones()};


    // Attach 层级
    Nullable<SceneComponent*> _parent{nullptr};
    vector<SceneComponent*> _children;  // non-owning, 所有权在 Actor::_ownedComponents
};

}  // namespace radray
