#include <radray/runtime/components/scene_component.h>

#include <algorithm>
#include <cmath>
#include <radray/logger.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

void SceneComponent::NotifyTransformChanged() {
    if (const auto world = GetWorld()) {
        world->QueueTransform(*this);
    } else {
        vector<SceneComponent*> pending{this};
        while (!pending.empty()) {
            auto* node = pending.back();
            pending.pop_back();
            const auto count = node->_children.size();
            node->OnTransformChanged();
            for (size_t i = count; i > 0; --i) pending.push_back(node->_children[i - 1]);
        }
    }
}

SceneComponent::~SceneComponent() noexcept { UnlinkHierarchy(); }

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
Eigen::Matrix4f SceneComponent::GetWorldTransform() const noexcept {
    Eigen::Matrix4f result = ComposeTransform(_relativeLocation, _relativeRotation, _relativeScale);
    for (auto parent = _parent; parent; parent = parent->_parent) {
        const Eigen::Matrix4f local = ComposeTransform(parent->_relativeLocation, parent->_relativeRotation, parent->_relativeScale);
        result = (local * result).eval();
    }
    return result;
}
Eigen::Matrix4f SceneComponent::GetWorldMatrix() const noexcept { return GetWorldTransform(); }
Eigen::Vector3f SceneComponent::GetWorldLocation() const noexcept {
    const Eigen::Matrix4f matrix = GetWorldTransform();
    const float* transform = matrix.data();
    return {transform[12], transform[13], transform[14]};
}
Eigen::Quaternionf SceneComponent::GetWorldRotation() const noexcept {
    const Eigen::Affine3f aff{GetWorldTransform()};
    return Eigen::Quaternionf{aff.rotation()};
}
Eigen::Vector3f SceneComponent::GetWorldScale() const noexcept {
    const Eigen::Matrix4f& matrix = GetWorldTransform();
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
    if (const auto world = GetWorld()) world->RemoveTransform(*this);
    UnlinkParent();
    _parent = parent;
    if (parent) {
        if (parent->_children.size() == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Too many attached children");
        _childIndex = static_cast<uint32_t>(parent->_children.size());
        parent->_children.push_back(this);
    }
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
void SceneComponent::UnlinkHierarchy() noexcept {
    if (const auto world = GetWorld()) world->RemoveTransform(*this);
    UnlinkParent();
    if (const auto world = GetWorld()) world->QueueTransform(*this);
    for (auto* child : _children) {
        if (const auto world = child->GetWorld()) world->RemoveTransform(*child);
        child->_parent = nullptr;
        child->_childIndex = std::numeric_limits<uint32_t>::max();
        if (const auto world = child->GetWorld()) world->QueueTransform(*child);
    }
    _children.clear();
}

void SceneComponent::UnlinkParent() noexcept {
    if (!_parent) return;
    auto& siblings = _parent->_children;
    auto* moved = siblings.back();
    siblings[_childIndex] = moved;
    moved->_childIndex = _childIndex;
    siblings.pop_back();
    _parent = nullptr;
    _childIndex = std::numeric_limits<uint32_t>::max();
}

}  // namespace radray
