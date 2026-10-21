#include <radray/runtime/components/static_mesh_component.h>

#include <utility>
#include <algorithm>

#include <radray/runtime/game_framework/world.h>

namespace radray {

StaticMeshComponent::~StaticMeshComponent() noexcept = default;

void StaticMeshComponent::SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) {
    CheckCanModify();
    if (_mesh == mesh) {
        return;
    }
    _readyWait.reset();
    _mesh = std::move(mesh);
    MarkRenderStateDirty();
    StartMeshReadyWait();
}

void StaticMeshComponent::SetMaterials(vector<StreamingAssetRef<Material>> materials) {
    CheckCanModify();
    if (_materials == materials) return;
    _materialReadyWait.reset();
    _materials = std::move(materials);
    _materialsDirty = true;
    MarkRenderDynamicDataDirty();
    StartMaterialReadyWaits();
}

bool StaticMeshComponent::SetInstanceParameters(const InstanceParameterLayout& layout, std::span<const byte> values) {
    CheckCanModify();
    if (!layout.IsValid() || values.size() != layout.Stride) return false;
    auto found = std::find_if(_parameters.begin(), _parameters.end(), [&](const auto& p) { return p.Layout.Name == layout.Name; });
    if (found == _parameters.end())
        _parameters.push_back({layout, vector<byte>{values.begin(), values.end()}, true});
    else {
        if (found->Layout != layout) return false;
        if (std::equal(values.begin(), values.end(), found->Values.begin())) return true;
        found->Values.assign(values.begin(), values.end());
        found->Dirty = true;
    }
    MarkRenderDynamicDataDirty();
    return true;
}

void StaticMeshComponent::OnRenderStateCreated() {
    StartMeshReadyWait();
    StartMaterialReadyWaits();
}

void StaticMeshComponent::OnRenderStateDestroyed() {
    _readyWait.reset();
    _materialReadyWait.reset();
}

void StaticMeshComponent::StartMeshReadyWait() {
    if (GetShapeId().IsValid() && _mesh.IsValid() && !_mesh.IsCompleted()) {
        _readyWait = make_unique<TaskScope>();
        _readyWait->Spawn(WaitForMeshReady(_mesh, GetRenderSceneId(), GetShapeId()));
    }
}

task<void> StaticMeshComponent::WaitForMeshReady(StreamingAssetRef<StaticMesh> mesh, SceneId scene, ShapeId registration) {
    if (co_await mesh) {
        if (IsLive() && IsRegistered() && GetRenderSceneId() == scene && GetShapeId() == registration && _mesh == mesh && mesh.IsReady()) {
            MarkRenderStateDirty();
        }
    }
}

void StaticMeshComponent::StartMaterialReadyWaits() {
    if (!GetShapeId().IsValid()) return;
    for (const auto& material : _materials) {
        if (material.IsValid() && !material.IsCompleted()) {
            if (!_materialReadyWait) _materialReadyWait = make_unique<TaskScope>();
            _materialReadyWait->Spawn(WaitForMaterialReady(material, GetRenderSceneId(), GetShapeId()));
        }
    }
}

task<void> StaticMeshComponent::WaitForMaterialReady(StreamingAssetRef<Material> material, SceneId scene, ShapeId registration) {
    if (co_await material) {
        if (IsLive() && IsRegistered() && GetRenderSceneId() == scene && GetShapeId() == registration &&
            std::find(_materials.begin(), _materials.end(), material) != _materials.end() && material.IsReady()) {
            _materialsDirty = true;
            MarkRenderDynamicDataDirty();
        }
    }
}

void StaticMeshComponent::CollectPrimitiveUpdates(ShapeCapture& capture, RenderDirtyFlags dirty) {
    if (dirty.HasFlag(RenderDirtyFlag::State)) {
        capture.SetStaticMesh(_mesh, GetSceneTransformId());
    }
    if (dirty.HasFlag(RenderDirtyFlag::State) || _materialsDirty) {
        capture.SetMaterials(_materials);
        _materialsDirty = false;
    }
    for (auto& parameters : _parameters)
        if (dirty.HasFlag(RenderDirtyFlag::State) || parameters.Dirty) {
            if (!capture.SetInstanceParameters(parameters.Layout, parameters.Values)) RADRAY_ABORT("Invalid captured instance parameters");
            parameters.Dirty = false;
        }
}

}  // namespace radray
