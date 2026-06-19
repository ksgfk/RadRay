#include <radray/runtime/components/scene_component.h>

#include <algorithm>

namespace radray {

SceneComponent::~SceneComponent() noexcept {
    DetachFromParent();
    while (!_children.empty()) {
        _children.back()->DetachFromParent();
    }
}

RuntimeTypeId SceneComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<SceneComponent>;
}

void SceneComponent::SetRelativeLocation(const Eigen::Vector3f& location) noexcept {
    _relativeLocation = location;
    OnTransformChanged();
}

void SceneComponent::SetRelativeRotation(const Eigen::Quaternionf& rotation) noexcept {
    _relativeRotation = rotation;
    OnTransformChanged();
}

void SceneComponent::SetRelativeScale(const Eigen::Vector3f& scale) noexcept {
    _relativeScale = scale;
    OnTransformChanged();
}

Eigen::Matrix4f SceneComponent::ComputeLocalMatrix() const noexcept {
    return ComposeTransform<float>(_relativeLocation, _relativeRotation, _relativeScale);
}

Eigen::Matrix4f SceneComponent::GetWorldMatrix() const noexcept {
    Eigen::Matrix4f local = ComputeLocalMatrix();
    if (_parent) {
        return _parent.Get()->GetWorldMatrix() * local;
    }
    return local;
}

Eigen::Vector3f SceneComponent::GetWorldLocation() const noexcept {
    return GetWorldMatrix().block<3, 1>(0, 3);
}

Eigen::Quaternionf SceneComponent::GetWorldRotation() const noexcept {
    Eigen::Affine3f aff{GetWorldMatrix()};
    return Eigen::Quaternionf{aff.rotation()};
}

Eigen::Vector3f SceneComponent::GetWorldScale() const noexcept {
    const Eigen::Matrix4f m = GetWorldMatrix();
    return Eigen::Vector3f{
        m.block<3, 1>(0, 0).norm(),
        m.block<3, 1>(0, 1).norm(),
        m.block<3, 1>(0, 2).norm()};
}

void SceneComponent::SetWorldLocation(const Eigen::Vector3f& location) noexcept {
    if (_parent) {
        Eigen::Matrix4f parentInv = _parent.Get()->GetWorldMatrix().inverse();
        Eigen::Vector4f localPos = parentInv * Eigen::Vector4f{location.x(), location.y(), location.z(), 1.0f};
        _relativeLocation = localPos.head<3>();
    } else {
        _relativeLocation = location;
    }
    OnTransformChanged();
}

void SceneComponent::SetWorldRotation(const Eigen::Quaternionf& rotation) noexcept {
    if (_parent) {
        Eigen::Quaternionf parentWorldRot = _parent.Get()->GetWorldRotation();
        _relativeRotation = parentWorldRot.conjugate() * rotation;
    } else {
        _relativeRotation = rotation;
    }
    OnTransformChanged();
}

void SceneComponent::AttachTo(SceneComponent* parent) noexcept {
    if (parent == this || parent == _parent.Get()) {
        return;
    }
    DetachFromParent();
    _parent = parent;
    parent->_children.push_back(this);
    OnTransformChanged();
}

void SceneComponent::DetachFromParent() noexcept {
    if (!_parent) {
        return;
    }
    auto& siblings = _parent.Get()->_children;
    auto it = std::find(siblings.begin(), siblings.end(), this);
    if (it != siblings.end()) {
        siblings.erase(it);
    }
    _parent = nullptr;
    OnTransformChanged();
}

}  // namespace radray
