#include <radray/runtime/components/scene_component.h>

#include <algorithm>
#include <cmath>
#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

void SceneComponent::MarkRenderDirty(RenderDirtyFlag flag) {
    const auto registration = GetRegistrationState();
    const auto lifecycle = GetLifecycle();
    if (!_renderConnection.IsValid() || (registration != ComponentRegistration::Registering && registration != ComponentRegistration::Registered)) return;
    if (lifecycle != ObjectLifecycle::Live && lifecycle != ObjectLifecycle::Initializing) return;
    if (auto world = GetWorld()) world->EnqueueRenderDirty(*this, flag);
}
void SceneComponent::MarkRenderStateDirty() {
    CheckCanModify();
    MarkRenderDirty(RenderDirtyFlag::State);
}
void SceneComponent::MarkRenderTransformDirty() {
    CheckCanModify();
    MarkRenderDirty(RenderDirtyFlag::Transform);
}
void SceneComponent::MarkRenderDynamicDataDirty() {
    CheckCanModify();
    MarkRenderDirty(RenderDirtyFlag::DynamicData);
}

void SceneComponent::NotifyTransformChanged() {
    const bool leaf = _children.empty();
    if (leaf) {
        _worldDirty = true;
    } else {
        InvalidateWorldSubtree();
    }
    auto world = GetWorld();
    if (world) world->BeginCallback();
    auto guard = MakeScopeGuard([world]() noexcept { if (world) world->EndCallback(); });
    if (leaf) {
        if (_autoMarkTransformDirty) MarkRenderDirty(RenderDirtyFlag::Transform);
        OnTransformChanged();
    } else {
        NotifySubtree();
    }
}

void SceneComponent::NotifySubtree() {
    if (!IsLive()) return;
    const size_t count = _children.size();
    if (_autoMarkTransformDirty) MarkRenderDirty(RenderDirtyFlag::Transform);
    OnTransformChanged();
    for (size_t i = 0; i < count && IsLive(); ++i) {
        _children[i]->NotifySubtree();
    }
}

void SceneComponent::InvalidateWorldSubtree() noexcept {
    _worldDirty = true;
    for (auto* child : _children) {
        child->InvalidateWorldSubtree();
    }
}

SceneComponent::~SceneComponent() noexcept {
    if (_renderConnection.IsValid()) RADRAY_ABORT("SceneComponent requires explicit render disconnection");
    UnlinkHierarchy(nullptr);
}

void SceneComponent::SetRelativeLocation(const Eigen::Vector3f& location) noexcept {
    CheckCanModify();
    if (_relativeLocation == location) return;
    _relativeLocation = location;
    NotifyTransformChanged();
}
void SceneComponent::SetRelativeRotation(const Eigen::Quaternionf& rotation) noexcept {
    CheckCanModify();
    if (_relativeRotation.coeffs() == rotation.coeffs()) return;
    _relativeRotation = rotation;
    NotifyTransformChanged();
}
void SceneComponent::SetRelativeScale(const Eigen::Vector3f& scale) noexcept {
    CheckCanModify();
    if (_relativeScale == scale) return;
    _relativeScale = scale;
    NotifyTransformChanged();
}
Eigen::Matrix4f SceneComponent::ComputeLocalMatrix() const noexcept {
    return ComposeTransform<float>(_relativeLocation, _relativeRotation, _relativeScale);
}
void SceneComponent::RefreshWorldMatrix() const noexcept {
    // 窗口固定，超过窗口的链分多轮从顶部收敛：深层级不会按深度消耗栈，也不做堆分配。
    constexpr size_t window = 32;
    const SceneComponent* chain[window];
    while (_worldDirty) {
        size_t height = 0;
        for (auto node = this; node != nullptr && node->_worldDirty; node = node->_parent.Get()) {
            chain[height % window] = node;
            ++height;
        }
        // 本轮只处理链上最高的 window 个节点，自上而下 compose；每个节点的 parent 此时已干净或为空。
        for (size_t index = height, bottom = height - std::min(height, window); index-- > bottom;) {
            const SceneComponent* node = chain[index % window];
            const Eigen::Matrix4f local = node->ComputeLocalMatrix();
            if (node->_parent)
                node->_worldMatrix = node->_parent->_worldMatrix * local;
            else
                node->_worldMatrix = local;
            node->_worldDirty = false;
        }
    }
}
Eigen::Matrix4f SceneComponent::GetWorldMatrix() const noexcept {
    if (_worldDirty) RefreshWorldMatrix();
    return _worldMatrix;
}
Eigen::Vector3f SceneComponent::GetWorldLocation() const noexcept { return GetWorldMatrix().block<3, 1>(0, 3); }
Eigen::Quaternionf SceneComponent::GetWorldRotation() const noexcept {
    const Eigen::Affine3f aff{GetWorldMatrix()};
    return Eigen::Quaternionf{aff.rotation()};
}
Eigen::Vector3f SceneComponent::GetWorldScale() const noexcept {
    const Eigen::Matrix4f matrix = GetWorldMatrix();
    return {matrix.block<3, 1>(0, 0).norm(), matrix.block<3, 1>(0, 1).norm(), matrix.block<3, 1>(0, 2).norm()};
}
void SceneComponent::SetWorldLocation(const Eigen::Vector3f& location) noexcept {
    CheckCanModify();
    if (_parent) {
        const Eigen::Vector4f local = _parent->GetWorldMatrix().inverse() * Eigen::Vector4f{location.x(), location.y(), location.z(), 1.0f};
        SetRelativeLocation(local.head<3>());
    } else
        SetRelativeLocation(location);
}
void SceneComponent::SetWorldRotation(const Eigen::Quaternionf& rotation) noexcept {
    CheckCanModify();
    SetRelativeRotation(_parent ? _parent->GetWorldRotation().conjugate() * rotation : rotation);
}

bool SceneComponent::ComputeAttachmentTransform(Nullable<SceneComponent*> parent, AttachmentRule rule,
                                                Eigen::Vector3f& location, Eigen::Quaternionf& rotation, Eigen::Vector3f& scale) const noexcept {
    for (auto ancestor = parent; ancestor; ancestor = ancestor->_parent) {
        if (ancestor.Get() == this || !ancestor->IsLive()) return false;
    }
    location = _relativeLocation;
    rotation = _relativeRotation;
    scale = _relativeScale;
    if (rule == AttachmentRule::KeepLocal) return true;
    Eigen::Matrix4f local = GetWorldMatrix();
    if (parent) {
        const Eigen::Matrix4f parentWorld = parent->GetWorldMatrix();
        if (!parentWorld.allFinite() || std::abs(parentWorld.determinant()) < 1e-8f) return false;
        local = parentWorld.inverse() * local;
    }
    if (!local.allFinite()) return false;
    location = local.block<3, 1>(0, 3);
    Eigen::Matrix3f basis = local.block<3, 3>(0, 0);
    for (int i = 0; i < 3; ++i) {
        scale[i] = basis.col(i).norm();
        if (scale[i] < 1e-8f) return false;
        basis.col(i) /= scale[i];
    }
    if (!(basis.transpose() * basis).isApprox(Eigen::Matrix3f::Identity(), 1e-5f)) return false;
    if (basis.determinant() < 0) {
        basis.col(0) *= -1;
        scale[0] *= -1;
    }
    rotation = Eigen::Quaternionf{basis}.normalized();
    return ComposeTransform<float>(location, rotation, scale).isApprox(local, 1e-5f);
}

bool SceneComponent::ReparentNow(Nullable<SceneComponent*> parent, AttachmentRule rule) noexcept {
    Eigen::Vector3f location, scale;
    Eigen::Quaternionf rotation;
    if (!ComputeAttachmentTransform(parent, rule, location, rotation, scale)) return false;
    if (_parent == parent) return true;
    if (_parent) std::erase(_parent->_children, this);
    _parent = parent;
    if (parent) parent->_children.push_back(this);
    _relativeLocation = location;
    _relativeRotation = rotation;
    _relativeScale = scale;
    NotifyTransformChanged();
    return true;
}

bool SceneComponent::CanJoinWorld(const Actor& owner, const World& world) const noexcept {
    const auto accepts = [&](const SceneComponent* linked) {
        return linked->GetOwner().Get() == &owner || (linked->GetWorld().Get() == &world && linked->IsLive());
    };
    if (_parent && !accepts(_parent.Get())) return false;
    return std::all_of(_children.begin(), _children.end(), accepts);
}

void SceneComponent::AttachTo(SceneComponent* parent) noexcept {
    CheckCanModify();
    parent->CheckCanModify();
    if (GetWorld() || parent->GetWorld()) RADRAY_ABORT("Registered attachment requires RequestReparent or an initial parent");
    if (!ReparentNow(parent, AttachmentRule::KeepLocal)) RADRAY_ABORT("Invalid draft attachment");
}
void SceneComponent::DetachFromParent() noexcept {
    CheckCanModify();
    if (GetWorld()) RADRAY_ABORT("Registered detachment requires RequestReparent");
    ReparentNow(nullptr, AttachmentRule::KeepLocal);
}
LifecycleRequestResult SceneComponent::RequestReparent(Nullable<SceneComponent*> parent, AttachmentRule rule) {
    CheckCanModify();
    auto world = GetWorld();
    if (!world) return LifecycleRequestResult::Invalid;
    Eigen::Vector3f location, scale;
    Eigen::Quaternionf rotation;
    if (!ComputeAttachmentTransform(parent, rule, location, rotation, scale)) return LifecycleRequestResult::Invalid;
    return world->QueueReparent(*this, parent, rule);
}
void SceneComponent::UnlinkHierarchy(Nullable<vector<SceneComponent*>*> detachedChildren) noexcept {
    if (_parent) {
        std::erase(_parent->_children, this);
        _parent = nullptr;
    }
    _worldDirty = true;
    for (auto* child : _children) {
        child->_parent = nullptr;
        child->InvalidateWorldSubtree();
        if (detachedChildren) detachedChildren->push_back(child);
    }
    _children.clear();
}

}  // namespace radray
