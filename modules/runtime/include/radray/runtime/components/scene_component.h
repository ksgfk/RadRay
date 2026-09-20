#pragma once

#include <limits>
#include <optional>
#include <span>

#include <radray/basic_math.h>
#include <radray/enum_flags.h>
#include <radray/nullable.h>
#include <radray/runtime/components/actor_component.h>
#include <radray/runtime/render_scene/scene_id.h>

namespace radray {

class SceneWriter;
class WorldRenderBridge;

enum class RenderDirtyFlag : uint8_t {
    State = 1,
    Transform = 2,
    DynamicData = 4,
};

template <>
struct is_flags<RenderDirtyFlag> : std::true_type {};
using RenderDirtyFlags = EnumFlags<RenderDirtyFlag>;
inline auto format_as(RenderDirtyFlag value) noexcept { return EnumFlagBitName(value); }

/// 有空间变换的组件。能形成父子 Attach 层级。
/// 对应 UE5 的 USceneComponent。
class SceneComponent : public ActorComponent {
public:
    SceneComponent() noexcept = default;
    ~SceneComponent() noexcept override;

    // ─── 变换 ───

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
    LifecycleRequestResult RequestReparent(Nullable<SceneComponent*> parent, AttachmentRule rule = AttachmentRule::KeepLocal);

    Nullable<SceneComponent*> GetAttachParent() const noexcept { return _parent; }
    std::span<SceneComponent* const> GetAttachChildren() const noexcept { return _children; }

    /// GT only. Repeated marks merge until collection; unregistered components are ignored.
    void MarkRenderStateDirty();
    void MarkRenderTransformDirty();
    void MarkRenderDynamicDataDirty();

protected:
    /// 本节点或祖先的世界变换变更后调用，派生类可覆写以标记渲染状态脏。
    virtual void OnTransformChanged() {}
    /// Invoked when a registered component connects to a scene; default does not enqueue.
    virtual void CreateRenderState(SceneWriter& writer) { (void)writer; }
    /// Invoked after removing queued updates; independent of game registration.
    virtual void DestroyRenderState(SceneWriter& writer) { (void)writer; }
    /// Capture owned values or retained immutable asset views. Must not mutate World/components or recursively flush.
    virtual void CollectRenderUpdates(SceneWriter& writer, RenderDirtyFlags dirty) {
        (void)writer;
        (void)dirty;
    }

private:
    friend class Actor;
    friend class World;
    friend class WorldRenderBridge;

    bool ReparentNow(Nullable<SceneComponent*> parent, AttachmentRule rule) noexcept;
    bool CanJoinWorld(const Actor& owner, const World& world) const noexcept;
    bool ComputeAttachmentTransform(Nullable<SceneComponent*> parent, AttachmentRule rule, Eigen::Vector3f& location, Eigen::Quaternionf& rotation, Eigen::Vector3f& scale) const noexcept;
    void UnlinkHierarchy(Nullable<vector<SceneComponent*>*> detachedChildren) noexcept;

    void MarkRenderDirty(RenderDirtyFlag flag);
    Eigen::Matrix4f ComputeLocalMatrix() const noexcept;
    void NotifyTransformChanged();

    // Relative transform（相对于 parent）
    Eigen::Quaternionf _relativeRotation{Eigen::Quaternionf::Identity()};
    Eigen::Vector3f _relativeLocation{Eigen::Vector3f::Zero()};
    Eigen::Vector3f _relativeScale{Eigen::Vector3f::Ones()};

    // Attach 层级
    Nullable<SceneComponent*> _parent{nullptr};
    vector<SceneComponent*> _children;  // non-owning, 所有权在 Actor::_ownedComponents
    RenderDirtyFlags _renderDirty;
    size_t _renderQueueIndex{std::numeric_limits<size_t>::max()};
    std::optional<SceneId> _renderConnection;
};

template <>
struct RuntimeTypeTrait<SceneComponent> {
    static constexpr RuntimeTypeId value{0x2aab6ba6, 0xd4d6, 0x40c5, 0x99, 0xae, 0xc7, 0x32, 0x4d, 0x36, 0xe7, 0x5b};
};

}  // namespace radray
