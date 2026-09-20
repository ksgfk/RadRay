#include <radray/runtime/components/scene_component.h>

#include <algorithm>
#include <cmath>
#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

void SceneComponent::MarkRenderDirty(RenderDirtyFlag flag) {
    const auto registration = GetRegistrationState();
    if (!IsLive() || !_renderConnection || (registration != ComponentRegistration::Registering && registration != ComponentRegistration::Registered)) return;
    GetWorld()->QueueRenderUpdate(*this, flag);
}
void SceneComponent::MarkRenderStateDirty() { MarkRenderDirty(RenderDirtyFlag::State); }
void SceneComponent::MarkRenderTransformDirty() { MarkRenderDirty(RenderDirtyFlag::Transform); }
void SceneComponent::MarkRenderDynamicDataDirty() { MarkRenderDirty(RenderDirtyFlag::DynamicData); }

void SceneComponent::NotifyTransformChanged() {
    if (!IsLive()) return;
    const size_t count = _children.size();
    auto world = GetWorld();
    if (world) world->BeginCallback();
    auto guard = MakeScopeGuard([world]() noexcept { if (world) world->EndCallback(); });
    OnTransformChanged();
    for (size_t i = 0; i < count && IsLive(); ++i) {
        auto* child = _children[i];
        child->NotifyTransformChanged();
    }
}

SceneComponent::~SceneComponent() noexcept {
    if (_renderConnection) RADRAY_ABORT("SceneComponent requires explicit render disconnection");
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
Eigen::Matrix4f SceneComponent::GetWorldMatrix() const noexcept {
    const Eigen::Matrix4f local = ComputeLocalMatrix();
    return _parent ? (_parent->GetWorldMatrix() * local).eval() : local;
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
    for (auto* child : _children) {
        child->_parent = nullptr;
        if (detachedChildren) detachedChildren->push_back(child);
    }
    _children.clear();
}

}  // namespace radray
