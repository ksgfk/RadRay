#include <radray/runtime/components/instanced_static_mesh_component.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <radray/profiler.h>

namespace radray {
namespace {
template <class T>
void ReserveAppend(vector<T>& values, size_t additions) {
    if (additions <= values.capacity() - values.size()) return;
    if (additions > values.max_size() - values.size()) RADRAY_ABORT("Instance collection capacity exhausted");
    const auto growth = values.capacity() + std::min(values.capacity(), values.max_size() - values.capacity());
    values.reserve(std::max(values.size() + additions, growth));
}
}  // namespace
InstancedStaticMeshComponent::~InstancedStaticMeshComponent() noexcept = default;
bool InstancedStaticMeshComponent::ValidTransform(const Eigen::Matrix4f& value) noexcept {
    return value.allFinite() && value(3, 0) == 0 && value(3, 1) == 0 && value(3, 2) == 0 && value(3, 3) == 1;
}
bool InstancedStaticMeshComponent::Contains(InstanceHandle handle) const noexcept {
    return _instances.IsAlive({handle.Index, handle.Generation});
}
std::optional<Eigen::Matrix4f> InstancedStaticMeshComponent::GetInstanceTransform(InstanceHandle handle) const noexcept {
    const auto* row = _instances.TryGet({handle.Index, handle.Generation});
    return row ? std::optional{_local[*row]} : std::nullopt;
}
ShapeId InstancedStaticMeshComponent::GetShapeId(InstanceHandle handle) const noexcept {
    const auto* row = _instances.TryGet({handle.Index, handle.Generation});
    return row ? _shapes[*row] : ShapeId{};
}
void InstancedStaticMeshComponent::QueueDirty(size_t row, InstanceDirtyFlags flags) {
    if (!_dirtyFlags[row]) _dirty.push_back(_handles[row]);
    _dirtyFlags[row] |= flags;
}
void InstancedStaticMeshComponent::SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) {
    CheckCanModify();
    if (_mesh == mesh) return;
    _readyWait.reset();
    _mesh = std::move(mesh);
    MarkRenderStateDirty();
    StartReadyWaits();
}
void InstancedStaticMeshComponent::SetMaterials(vector<StreamingAssetRef<Material>> materials) {
    CheckCanModify();
    if (_materials == materials) return;
    _readyWait.reset();
    _materials = std::move(materials);
    _materialsDirty = true;
    MarkRenderDynamicDataDirty();
    StartReadyWaits();
}
bool InstancedStaticMeshComponent::AddInstances(std::span<const InstanceInit> values, std::span<InstanceHandle> result) {
    CheckCanModify();
    if (values.size() != result.size() || values.size() > UINT32_MAX - _instances.Count()) return false;
    for (const auto& value : values)
        if (!ValidTransform(value.LocalToComponent)) return false;
    const auto total = _local.size() + values.size();
    for (const auto& p : _parameters)
        if (total > UINT32_MAX / p.Layout.Stride) return false;
    _instances.Reserve(static_cast<uint32_t>(std::min(std::bit_ceil(static_cast<uint64_t>(total)), uint64_t{UINT32_MAX})));
    ReserveAppend(_handles, values.size());
    ReserveAppend(_local, values.size());
    ReserveAppend(_shapes, values.size());
    ReserveAppend(_draw, values.size());
    ReserveAppend(_dirtyFlags, values.size());
    ReserveAppend(_dirty, values.size());
    for (auto& p : _parameters) {
        p.Values.resize(total * p.Layout.Stride);
        p.Queued.resize(total);
        ReserveAppend(p.Dirty, values.size());
    }
    for (size_t i = 0; i < values.size(); ++i) {
        const auto row = static_cast<uint32_t>(_local.size());
        const auto id = _instances.Emplace(row);
        const InstanceHandle handle{id.Index, id.Generation};
        result[i] = handle;
        _handles.push_back(handle);
        _local.push_back(values[i].LocalToComponent);
        _shapes.push_back({});
        _draw.push_back(values[i].Draw);
        _dirtyFlags.emplace_back();
        QueueDirty(row, InstanceDirtyFlag::NewInstance);
        for (auto& p : _parameters) {
            p.Queued[row] = 1;
            p.Dirty.push_back(handle);
        }
    }
    if (!values.empty()) MarkRenderDynamicDataDirty();
    return true;
}
bool InstancedStaticMeshComponent::UpdateInstances(std::span<const InstanceUpdate> values) {
    CheckCanModify();
    for (const auto& value : values)
        if (!Contains(value.Handle) || !ValidTransform(value.LocalToComponent)) return false;
    for (const auto& value : values) {
        const auto row = _instances.Get({value.Handle.Index, value.Handle.Generation});
        if (!(_local[row].array() == value.LocalToComponent.array()).all()) {
            _local[row] = value.LocalToComponent;
            QueueDirty(row, InstanceDirtyFlag::TransformDirty);
        }
        if (value.Draw && _draw[row] != *value.Draw) {
            _draw[row] = *value.Draw;
            QueueDirty(row, InstanceDirtyFlag::DrawDirty);
        }
    }
    if (!_dirty.empty()) MarkRenderDynamicDataDirty();
    return true;
}
bool InstancedStaticMeshComponent::RemoveInstances(std::span<const InstanceHandle> handles) {
    CheckCanModify();
    vector<InstanceHandle> sorted{handles.begin(), handles.end()};
    for (auto handle : handles)
        if (!Contains(handle) || handle.Generation == UINT32_MAX) return false;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return false;
    ReserveAppend(_removed, handles.size());
    for (auto handle : handles) {
        const auto row = _instances.Get({handle.Index, handle.Generation});
        const auto last = static_cast<uint32_t>(_local.size() - 1);
        if (_shapes[row].IsValid()) _removed.push_back(_shapes[row]);
        if (row != last) {
            _handles[row] = _handles[last];
            _local[row] = _local[last];
            _shapes[row] = _shapes[last];
            _draw[row] = _draw[last];
            _dirtyFlags[row] = _dirtyFlags[last];
            _instances.Get({_handles[row].Index, _handles[row].Generation}) = row;
        }
        for (auto& p : _parameters) {
            if (row != last) {
                std::memcpy(p.Values.data() + size_t{row} * p.Layout.Stride, p.Values.data() + size_t{last} * p.Layout.Stride, p.Layout.Stride);
                p.Queued[row] = p.Queued[last];
            }
            p.Values.resize(size_t{last} * p.Layout.Stride);
            p.Queued.pop_back();
        }
        _handles.pop_back();
        _local.pop_back();
        _shapes.pop_back();
        _draw.pop_back();
        _dirtyFlags.pop_back();
        _instances.Destroy({handle.Index, handle.Generation});
    }
    if (!handles.empty()) MarkRenderDynamicDataDirty();
    return true;
}
bool InstancedStaticMeshComponent::SetInstanceParameters(std::span<const InstanceHandle> handles, const InstanceParameterLayout& layout, std::span<const byte> values) {
    CheckCanModify();
    if (!layout.IsValid() || _local.size() > UINT32_MAX / layout.Stride || handles.size() > UINT32_MAX / layout.Stride || values.size() != handles.size() * layout.Stride) return false;
    for (auto handle : handles)
        if (!Contains(handle)) return false;
    auto found = std::find_if(_parameters.begin(), _parameters.end(), [&](const auto& p) { return p.Layout.Name == layout.Name; });
    if (found == _parameters.end()) {
        _parameters.push_back({layout, vector<byte>(_local.size() * layout.Stride), _handles, vector<uint8_t>(_local.size(), 1)});
        found = _parameters.end() - 1;
    } else if (found->Layout != layout)
        return false;
    for (size_t i = 0; i < handles.size(); ++i) {
        const auto row = _instances.Get({handles[i].Index, handles[i].Generation});
        auto* destination = found->Values.data() + size_t{row} * layout.Stride;
        const auto* source = values.data() + i * layout.Stride;
        if (std::memcmp(destination, source, layout.Stride) == 0) continue;
        std::memcpy(destination, source, layout.Stride);
        if (!found->Queued[row]) {
            found->Queued[row] = 1;
            found->Dirty.push_back(handles[i]);
        }
    }
    if (!found->Dirty.empty()) MarkRenderDynamicDataDirty();
    return true;
}
void InstancedStaticMeshComponent::CreateRenderState(SceneWriter& writer) {
    _connected = true;
    ++_connection;
    _domain = writer.CreateBatchDomain();
    _materialsDirty = true;
    MarkRenderStateDirty();
    StartReadyWaits();
}
void InstancedStaticMeshComponent::DestroyRenderState(SceneWriter& writer) {
    _readyWait.reset();
    _connected = false;
    ++_connection;
    writer.RemoveShapes(_removed);
    _removed.clear();
    _newIds.clear();
    for (auto id : _shapes)
        if (id.IsValid()) _newIds.push_back(id);
    writer.RemoveShapes(_newIds);
    _newIds.clear();
    std::fill(_shapes.begin(), _shapes.end(), ShapeId{});
}
void InstancedStaticMeshComponent::StartReadyWaits() {
    if (!_connected) return;
    if (_mesh.IsValid() && !_mesh.IsCompleted()) {
        if (!_readyWait) _readyWait = make_unique<TaskScope>();
        _readyWait->Spawn(WaitForMeshReady(_mesh, _connection));
    }
    for (const auto& material : _materials)
        if (material.IsValid() && !material.IsCompleted()) {
            if (!_readyWait) _readyWait = make_unique<TaskScope>();
            _readyWait->Spawn(WaitForMaterialReady(material, _connection));
        }
}
task<void> InstancedStaticMeshComponent::WaitForMeshReady(StreamingAssetRef<StaticMesh> mesh, uint64_t connection) {
    if (co_await mesh)
        if (IsLive() && IsRegistered() && _connected && _connection == connection && _mesh == mesh && mesh.IsReady()) MarkRenderStateDirty();
}
task<void> InstancedStaticMeshComponent::WaitForMaterialReady(StreamingAssetRef<Material> material, uint64_t connection) {
    if (co_await material)
        if (IsLive() && IsRegistered() && _connected && _connection == connection && material.IsReady() &&
            std::find(_materials.begin(), _materials.end(), material) != _materials.end()) {
            _materialsDirty = true;
            MarkRenderDynamicDataDirty();
        }
}
void InstancedStaticMeshComponent::CollectRenderUpdates(SceneCapture& capture, RenderDirtyFlags dirty) {
    RADRAY_PROFILE_SCOPE_N("InstancedStaticMeshComponent::Collect");
    capture.RemoveShapes(_removed);
    _removed.clear();
    const bool state = dirty.HasFlag(RenderDirtyFlag::State), parent = dirty.HasFlag(RenderDirtyFlag::Transform);
    _rows.clear();
    if (state || parent) {
        ReserveAppend(_rows, _local.size());
        for (uint32_t row = 0; row < _local.size(); ++row) _rows.push_back(row);
    } else
        for (auto handle : _dirty) {
            const auto* row = _instances.TryGet({handle.Index, handle.Generation});
            if (row) _rows.push_back(*row);
        }
    _newIds.clear();
    for (auto row : _rows)
        if (!_shapes[row].IsValid()) _newIds.push_back({});
    if (!capture.CreateShapes(_newIds)) RADRAY_ABORT("Instance shape capacity exhausted");
    size_t next = 0;
    for (auto row : _rows)
        if (!_shapes[row].IsValid()) {
            _shapes[row] = _newIds[next++];
            _dirtyFlags[row] |= InstanceDirtyFlag::NewInstance;
        }
    _meshIds.clear();
    _worldMatrices.clear();
    _transforms.clear();
    _drawUpdates.clear();
    const Eigen::Matrix4f componentToWorld = GetWorldMatrix();
    for (auto row : _rows) {
        const auto flags = _dirtyFlags[row];
        if (state || flags.HasFlag(InstanceDirtyFlag::NewInstance)) {
            _meshIds.push_back(_shapes[row]);
            _worldMatrices.push_back(componentToWorld * _local[row]);
        } else if (parent || flags.HasFlag(InstanceDirtyFlag::TransformDirty))
            _transforms.push_back({_shapes[row], AffineTransform{componentToWorld * _local[row]}});
        if (state || (flags & (InstanceDirtyFlag::NewInstance | InstanceDirtyFlag::DrawDirty))) {
            auto draw = _draw[row];
            draw.BatchDomain = _domain;
            draw.ExplicitInstancing = true;
            _drawUpdates.push_back({_shapes[row], draw});
        }
        _dirtyFlags[row] = {};
    }
    if (!_meshIds.empty()) capture.SetStaticMeshes(_meshIds, _mesh, _worldMatrices);
    capture.SetTransforms(_transforms);
    capture.SetDrawStates(_drawUpdates);
    if (state || _materialsDirty) {
        if (!_shapes.empty()) capture.SetMaterials(_shapes, _materials);
        _materialsDirty = false;
    } else if (!_meshIds.empty())
        capture.SetMaterials(_meshIds, _materials);
    for (auto& p : _parameters) {
        if (state) {
            if (!capture.SetInstanceParameters(_shapes, p.Layout, p.Values)) RADRAY_ABORT("Invalid captured instance parameters");
            std::fill(p.Queued.begin(), p.Queued.end(), 0);
        } else {
            _parameterIds.clear();
            _parameterBytes.clear();
            ReserveAppend(_parameterIds, p.Dirty.size());
            ReserveAppend(_parameterBytes, p.Dirty.size() * p.Layout.Stride);
            for (auto handle : p.Dirty) {
                const auto* row = _instances.TryGet({handle.Index, handle.Generation});
                if (!row) continue;
                _parameterIds.push_back(_shapes[*row]);
                const auto offset = size_t{*row} * p.Layout.Stride;
                _parameterBytes.insert(_parameterBytes.end(), p.Values.begin() + offset, p.Values.begin() + offset + p.Layout.Stride);
                p.Queued[*row] = 0;
            }
            if (!_parameterIds.empty() && !capture.SetInstanceParameters(_parameterIds, p.Layout, _parameterBytes)) RADRAY_ABORT("Invalid captured instance parameters");
        }
        p.Dirty.clear();
    }
    _dirty.clear();
}
}  // namespace radray
