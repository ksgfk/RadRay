#pragma once

#include <limits>
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

    /// 世界空间变换（读缓存；缓存脏时沿 parent chain 重建后再读）
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
    /// 本节点或祖先的世界变换变更后调用。打开 `_autoMarkTransformDirty` 的类型由框架入队 Transform dirty。
    virtual void OnTransformChanged() {}
    /// GT only. Does not re-check the owning thread; public Mark* wrappers do.
    void MarkRenderDirty(RenderDirtyFlag flag);
    void EnableTransformRenderDirty() noexcept { _autoMarkTransformDirty = true; }
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

    Eigen::Matrix4f ComputeLocalMatrix() const noexcept;
    /// 重建本节点的世界矩阵缓存：自下而上收集脏的 parent chain，再自上而下 compose。
    /// 迭代实现，栈消耗与层级深度无关；只在 `_worldDirty` 为真时调用。
    void RefreshWorldMatrix() const noexcept;
    void NotifyTransformChanged();
    /// 渲染脏标记的子树遍历：由 NotifyTransformChanged 在唯一一层 callback scope 内调用。
    /// 不做缓存标脏（入口已整树标脏），也不重复开 callback scope。
    void NotifySubtree();
    /// 把本子树（含自身）的世界矩阵缓存标记为脏；有后代的 TRS 变更与解除层级时调用。
    /// 遍历不被 Live 门禁截断：否则活着的后代会保留已失效的世界矩阵缓存。
    void InvalidateWorldSubtree() noexcept;

    // Relative transform 与入队热数据放在矩阵缓存之前，避免 Mutate 写下脏标志时带上 64 B 矩阵行。
    Eigen::Quaternionf _relativeRotation{Eigen::Quaternionf::Identity()};
    Eigen::Vector3f _relativeLocation{Eigen::Vector3f::Zero()};
    Eigen::Vector3f _relativeScale{Eigen::Vector3f::Ones()};
    mutable bool _worldDirty{false};
    bool _autoMarkTransformDirty{false};
    RenderDirtyFlags _renderDirty;
    size_t _renderQueueIndex{std::numeric_limits<size_t>::max()};
    SceneId _renderConnection{};
    Nullable<SceneComponent*> _parent{nullptr};
    vector<SceneComponent*> _children;  // non-owning, 所有权在 Actor::_ownedComponents

    // 变换缓存（memo）：无子节点的 TRS 由 NotifyTransformChanged 直接置 _worldDirty；
    // 有后代时 InvalidateWorldSubtree 整树标脏。GetWorldMatrix 脏则 compose local
    // 并沿 parent chain 迭代求值后缓存，干净时直接返回。契约见 docs/architecture/render-framework.md。
    mutable Eigen::Matrix4f _worldMatrix{Eigen::Matrix4f::Identity()};
};

template <>
struct RuntimeTypeTrait<SceneComponent> {
    static constexpr RuntimeTypeId value{0x2aab6ba6, 0xd4d6, 0x40c5, 0x99, 0xae, 0xc7, 0x32, 0x4d, 0x36, 0xe7, 0x5b};
};

}  // namespace radray
