#pragma once

#include <limits>
#include <span>

#include <radray/basic_math.h>
#include <radray/nullable.h>
#include <radray/runtime/components/actor_component.h>
#include <radray/runtime/game_framework/world_transform_store.h>

namespace radray {

class SceneCapture;

/// 有空间变换的组件。能形成父子 Attach 层级。
/// 对应 UE5 的 USceneComponent。
class SceneComponent : public ActorComponent {
public:
    SceneComponent() noexcept = default;
    ~SceneComponent() noexcept override;

    // ─── 变换 ───

    /// 相对于父组件的变换
    Eigen::Vector3f GetRelativeLocation() const noexcept {
        const auto& value = LocalValue();
        return {value.Translation[0], value.Translation[1], value.Translation[2]};
    }
    Eigen::Quaternionf GetRelativeRotation() const noexcept {
        const auto& value = LocalValue();
        return {value.Rotation[3], value.Rotation[0], value.Rotation[1], value.Rotation[2]};
    }
    Eigen::Vector3f GetRelativeScale() const noexcept {
        const auto& value = LocalValue();
        return {value.Scale[0], value.Scale[1], value.Scale[2]};
    }
    LocalTransform GetRelativeTransform() const noexcept { return LocalValue(); }

    void SetRelativeLocation(const Eigen::Vector3f& location) noexcept;
    void SetRelativeRotation(const Eigen::Quaternionf& rotation) noexcept;
    void SetRelativeScale(const Eigen::Vector3f& scale) noexcept;
    void SetRelativeTransform(const LocalTransform& local) noexcept;
    WorldTransformId GetWorldTransformId() const noexcept { return _worldTransformId; }
    void SetTransformNotificationEnabled(bool enabled) noexcept;
    bool IsTransformNotificationEnabled() const noexcept { return _transformNotificationEnabled; }

    /// Immediate GT value, evaluated from this node to the root without a persistent cache.
    Eigen::Vector3f GetWorldLocation() const noexcept;
    Eigen::Quaternionf GetWorldRotation() const noexcept;
    Eigen::Vector3f GetWorldScale() const noexcept;
    Eigen::Matrix4f GetWorldMatrix() const noexcept;
    Eigen::Matrix4f GetWorldTransform() const noexcept;
    TransformId GetSceneTransformId() const noexcept { return _sceneTransformId; }

    /// 直接设置世界位置（反算出 relative）
    void SetWorldLocation(const Eigen::Vector3f& location) noexcept;
    void SetWorldRotation(const Eigen::Quaternionf& rotation) noexcept;

    // ─── Attach 层级 ───

    /// 将此组件挂到 parent 下
    void AttachTo(SceneComponent* parent) noexcept;

    /// 从父组件脱离
    void DetachFromParent() noexcept;
    LifecycleRequestResult RequestReparent(Nullable<SceneComponent*> parent, AttachmentRule rule = AttachmentRule::KeepLocal);

    Nullable<SceneComponent*> GetAttachParent() const noexcept { return _parent; }
    /// Unordered children; detach swaps in the last child. The borrow expires on hierarchy mutation.
    std::span<SceneComponent* const> GetAttachChildren() const noexcept { return _children; }

protected:
    /// Opt in with SetTransformNotificationEnabled. Registered changes dispatch at the next boundary.
    virtual void OnTransformChanged() {}
    virtual void CollectRenderTransform(SceneCapture& capture) { (void)capture; }

private:
    friend class Actor;
    friend class World;
    friend class WorldRenderBridge;
    friend class WorldTransformStore;

    struct TransformDirtyState {
        static constexpr uint32_t kNotQueued = 0x7fffffffu;
        uint32_t Epoch{0};
        uint32_t Index{kNotQueued};
    };

    bool ReparentNow(Nullable<SceneComponent*> parent, AttachmentRule rule) noexcept;
    bool CanJoinWorld(const Actor& owner, const World& world) const noexcept;
    bool ComputeAttachmentTransform(Nullable<SceneComponent*> parent, AttachmentRule rule, Eigen::Vector3f& location, Eigen::Quaternionf& rotation, Eigen::Vector3f& scale) const noexcept;
    void UnlinkHierarchy() noexcept;
    void UnlinkParent() noexcept;

    void NotifyTransformChanged();
    void AdjustTransformSubscribers(int64_t delta) noexcept;
    LocalTransform& LocalValue() noexcept { return *_local; }
    const LocalTransform& LocalValue() const noexcept { return *_local; }

    LocalTransform _draftLocal;
    LocalTransform* _local{&_draftLocal};
    WorldTransformId _worldTransformId;
    uint32_t _transformSubscribers{0};
    bool _transformNotificationEnabled{false};
    TransformId _sceneTransformId;
    uint32_t _sceneTransformIndex{std::numeric_limits<uint32_t>::max()};
    TransformDirtyState _transformDirty;
    uint32_t _renderTransformRootIndex{std::numeric_limits<uint32_t>::max()};
    uint32_t _renderTransformCaptureEpoch{0};
    Nullable<SceneComponent*> _parent{nullptr};
    uint32_t _childIndex{std::numeric_limits<uint32_t>::max()};
    vector<SceneComponent*> _children;  // non-owning, 所有权在 Actor::_ownedComponents
};

template <>
struct RuntimeTypeTrait<SceneComponent> {
    static constexpr RuntimeTypeId value{0x2aab6ba6, 0xd4d6, 0x40c5, 0x99, 0xae, 0xc7, 0x32, 0x4d, 0x36, 0xe7, 0x5b};
};

}  // namespace radray
