#include <radray/runtime/components/scene_component.h>

#include <algorithm>
#include <cmath>
#include <radray/logger.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

void SceneComponent::NotifyTransformChanged() {
    if (const auto world = GetWorld()) {
        world->QueueTransform(*this);
    } else if (_transformSubscribers != 0) {
        vector<SceneComponent*> pending{this};
        while (!pending.empty()) {
            auto* node = pending.back();
            pending.pop_back();
            const auto count = node->_children.size();
            if (node->_transformNotificationEnabled) node->OnTransformChanged();
            for (size_t i = count; i > 0; --i) {
                auto* child = node->_children[i - 1];
                if (child->_transformSubscribers != 0) pending.push_back(child);
            }
        }
    }
}

SceneComponent::~SceneComponent() noexcept { UnlinkHierarchy(); }

void SceneComponent::SetRelativeLocation(const Eigen::Vector3f& location) noexcept {
    CheckCanModify();
    auto& value = LocalValue();
    if (std::equal(std::begin(value.Translation), std::end(value.Translation), location.data())) return;
    std::copy_n(location.data(), 3, value.Translation);
    NotifyTransformChanged();
}
void SceneComponent::SetRelativeRotation(const Eigen::Quaternionf& rotation) noexcept {
    CheckCanModify();
    auto& value = LocalValue();
    if (std::equal(std::begin(value.Rotation), std::end(value.Rotation), rotation.coeffs().data())) return;
    std::copy_n(rotation.coeffs().data(), 4, value.Rotation);
    NotifyTransformChanged();
}
void SceneComponent::SetRelativeScale(const Eigen::Vector3f& scale) noexcept {
    CheckCanModify();
    auto& value = LocalValue();
    if (std::equal(std::begin(value.Scale), std::end(value.Scale), scale.data())) return;
    std::copy_n(scale.data(), 3, value.Scale);
    NotifyTransformChanged();
}
void SceneComponent::SetRelativeTransform(const LocalTransform& local) noexcept {
    CheckCanModify();
    auto& value = LocalValue();
    if (std::equal(std::begin(value.Translation), std::end(value.Translation), local.Translation) &&
        std::equal(std::begin(value.Rotation), std::end(value.Rotation), local.Rotation) &&
        std::equal(std::begin(value.Scale), std::end(value.Scale), local.Scale)) return;
    value = local;
    NotifyTransformChanged();
}
void SceneComponent::AdjustTransformSubscribers(int64_t delta) noexcept {
    if (delta == 0) return;
    for (Nullable<SceneComponent*> node{this}; node; node = node->_parent) {
        const int64_t count = static_cast<int64_t>(node->_transformSubscribers) + delta;
        if (count < 0 || count > std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Invalid transform subscriber count");
        node->_transformSubscribers = static_cast<uint32_t>(count);
    }
}
void SceneComponent::SetTransformNotificationEnabled(bool enabled) noexcept {
    CheckCanModify();
    if (_transformNotificationEnabled == enabled) return;
    _transformNotificationEnabled = enabled;
    AdjustTransformSubscribers(enabled ? 1 : -1);
}
Eigen::Matrix4f SceneComponent::GetWorldTransform() const noexcept {
    Eigen::Matrix4f result = LocalValue().ToMatrix();
    for (auto parent = _parent; parent; parent = parent->_parent) {
        const Eigen::Matrix4f local = parent->LocalValue().ToMatrix();
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
    location = GetRelativeLocation();
    rotation = GetRelativeRotation();
    scale = GetRelativeScale();
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
        parent->AdjustTransformSubscribers(_transformSubscribers);
    }
    LocalValue() = LocalTransform{location, rotation, scale};
    if (const auto world = GetWorld()) world->UpdateComponentTransformParent(*this);
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
    if (const auto world = GetWorld()) {
        world->UpdateComponentTransformParent(*this);
        world->QueueTransform(*this);
    }
    while (!_children.empty()) {
        auto* child = _children.back();
        if (const auto world = child->GetWorld()) world->RemoveTransform(*child);
        child->UnlinkParent();
        if (const auto world = child->GetWorld()) {
            world->UpdateComponentTransformParent(*child);
            world->QueueTransform(*child);
        }
    }
    _children.clear();
}

void SceneComponent::UnlinkParent() noexcept {
    if (!_parent) return;
    _parent->AdjustTransformSubscribers(-static_cast<int64_t>(_transformSubscribers));
    auto& siblings = _parent->_children;
    auto* moved = siblings.back();
    siblings[_childIndex] = moved;
    moved->_childIndex = _childIndex;
    siblings.pop_back();
    _parent = nullptr;
    _childIndex = std::numeric_limits<uint32_t>::max();
}

}  // namespace radray
