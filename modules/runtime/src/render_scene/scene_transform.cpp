#include <radray/runtime/render_scene/scene_transform.h>

#include <algorithm>
#include <radray/profiler.h>

namespace radray {

LocalTransform::LocalTransform(const Eigen::Vector3f& translation, const Eigen::Quaternionf& rotation, const Eigen::Vector3f& scale) noexcept {
    std::copy_n(rotation.coeffs().data(), 4, Rotation);
    std::copy_n(translation.data(), 3, Translation);
    std::copy_n(scale.data(), 3, Scale);
}
Eigen::Matrix4f LocalTransform::ToMatrix() const noexcept {
    return ComposeTransform(Eigen::Vector3f{Translation[0], Translation[1], Translation[2]},
                            Eigen::Quaternionf{Rotation[3], Rotation[0], Rotation[1], Rotation[2]},
                            Eigen::Vector3f{Scale[0], Scale[1], Scale[2]});
}

void SceneTransform::BeginApply() {
    if (_epoch == std::numeric_limits<uint32_t>::max()) {
        std::fill(_seedEpoch.begin(), _seedEpoch.end(), 0);
        std::fill(_affected.begin(), _affected.end(), 0);
        _epoch = 0;
    }
    ++_epoch;
    _seeds.clear();
    _changed.clear();
}
uint32_t SceneTransform::Allocate() {
    uint32_t row;
    if (_free.empty()) {
        if (_parent.size() == kNoRow) RADRAY_ABORT("Too many scene transforms");
        row = static_cast<uint32_t>(_parent.size());
        _parent.push_back(kNoRow);
        _children.emplace_back();
        _local.emplace_back();
        _world.emplace_back();
        _firstShape.push_back(kNoRow);
        _seedEpoch.push_back(0);
        _affected.push_back(0);
        _alive.push_back(1);
    } else {
        row = _free.back();
        _free.pop_back();
        _parent[row] = kNoRow;
        _children[row] = {};
        _firstShape[row] = kNoRow;
        _alive[row] = 1;
    }
    return row;
}
void SceneTransform::Seed(uint32_t row) {
    if (_seedEpoch[row] == _epoch) return;
    _seedEpoch[row] = _epoch;
    _seeds.push_back(row);
}
void SceneTransform::Detach(uint32_t row) {
    const auto parent = _parent[row];
    auto& links = _children[row];
    if (parent != kNoRow && _children[parent].First == row) _children[parent].First = links.Next;
    if (links.Previous != kNoRow) _children[links.Previous].Next = links.Next;
    if (links.Next != kNoRow) _children[links.Next].Previous = links.Previous;
    links.Next = links.Previous = kNoRow;
    _parent[row] = kNoRow;
}
void SceneTransform::Release(uint32_t row) {
    if (!_alive[row] || _firstShape[row] != kNoRow) RADRAY_ABORT("Cannot remove a bound transform");
    Detach(row);
    while (_children[row].First != kNoRow) {
        const auto child = _children[row].First;
        Detach(child);
        Seed(child);
    }
    _alive[row] = 0;
    _free.push_back(row);
}
void SceneTransform::Create(TransformId id, const LocalTransform& local) {
    if (!id.IsValid()) RADRAY_ABORT("Invalid transform creation");
    if (id.Index >= _slots.size()) _slots.resize(static_cast<size_t>(id.Index) + 1);
    auto& slot = _slots[id.Index];
    if (slot.Row != kNoRow || id.Generation < slot.Generation) RADRAY_ABORT("Stale transform creation");
    slot.Generation = id.Generation;
    slot.Row = Allocate();
    _local[slot.Row] = local.ToMatrix();
    Seed(slot.Row);
}
uint32_t SceneTransform::GetRow(TransformId id) const noexcept {
    if (!id.IsValid() || id.Index >= _slots.size() || _slots[id.Index].Generation != id.Generation || _slots[id.Index].Row == kNoRow)
        RADRAY_ABORT("Invalid scene transform identity");
    return _slots[id.Index].Row;
}
void SceneTransform::Remove(TransformId id) {
    Release(GetRow(id));
    auto& slot = _slots[id.Index];
    slot.Row = kNoRow;
    if (slot.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Transform generation exhausted");
    ++slot.Generation;
}
void SceneTransform::SetParent(TransformId id, TransformId parent) {
    const auto row = GetRow(id);
    const auto next = parent.IsValid() ? GetRow(parent) : kNoRow;
    if (_parent[row] == next) return;
    Detach(row);
    _parent[row] = next;
    if (next != kNoRow) {
        auto& first = _children[next].First;
        _children[row].Next = first;
        if (first != kNoRow) _children[first].Previous = row;
        first = row;
    }
    Seed(row);
}
void SceneTransform::SetLocal(TransformId id, const LocalTransform& local) {
    const auto row = GetRow(id);
    _local[row] = local.ToMatrix();
    Seed(row);
}
uint32_t SceneTransform::CreateStandalone(const AffineTransform& matrix) {
    const auto row = Allocate();
    SetStandalone(row, matrix);
    return row;
}
void SceneTransform::RemoveStandalone(uint32_t row) { Release(row); }
void SceneTransform::SetStandalone(uint32_t row, const AffineTransform& matrix) {
    _local[row] = matrix.ToMatrix();
    Seed(row);
}
void SceneTransform::EvaluateSubtree(uint32_t row) {
    _work.push_back(row);
    while (!_work.empty()) {
        const auto node = _work.back();
        _work.pop_back();
        if (_parent[node] == kNoRow)
            _world[node] = _local[node];
        else
            _world[node].noalias() = _world[_parent[node]] * _local[node];
        _affected[node] = _epoch;
        _changed.push_back(node);
        for (auto child = _children[node].First; child != kNoRow; child = _children[child].Next) _work.push_back(child);
    }
}
std::span<const uint32_t> SceneTransform::Evaluate() {
    RADRAY_PROFILE_SCOPE_N("SceneTransform::Evaluate");
    std::erase_if(_seeds, [this](uint32_t row) { return !_alive[row]; });
    if (_seeds.empty()) return _changed;
    if (_seeds.size() >= 256 && _seeds.size() >= (_parent.size() + 7) / 8) {
        _seeds.clear();
        for (size_t row = 0; row < _parent.size(); ++row) {
            if (_alive[row] && _seedEpoch[row] == _epoch) _seeds.push_back(static_cast<uint32_t>(row));
        }
    }
    if (_seeds.size() == 1 || std::all_of(_seeds.begin(), _seeds.end(), [this](uint32_t row) { return _parent[row] == kNoRow; })) {
        for (auto row : _seeds) EvaluateSubtree(row);
    } else {
        for (auto row : _seeds) {
            if (_affected[row] == _epoch) continue;
            _affected[row] = _epoch;
            _work.push_back(row);
            while (!_work.empty()) {
                const auto node = _work.back();
                _work.pop_back();
                for (auto child = _children[node].First; child != kNoRow; child = _children[child].Next) {
                    if (_affected[child] == _epoch) continue;
                    _affected[child] = _epoch;
                    _work.push_back(child);
                }
            }
        }
        for (auto row : _seeds) {
            if (_parent[row] == kNoRow || _affected[_parent[row]] != _epoch) EvaluateSubtree(row);
        }
    }
    return _changed;
}

}  // namespace radray
