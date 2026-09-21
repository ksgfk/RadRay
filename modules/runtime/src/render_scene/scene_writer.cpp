#include <radray/runtime/render_scene/scene_writer.h>

#include <radray/profiler.h>

namespace radray {

SceneWriter::SceneWriter(SceneId id, uint32_t flightCount) : _id(id), _assets(flightCount) {}
SceneWriter::~SceneWriter() noexcept = default;

SceneWriter::ShapeState& SceneWriter::GetShape(ShapeId id) {
    if (_closing || !_shapes.IsAlive({id.Index, id.Generation})) RADRAY_ABORT("Invalid scene writer shape");
    return _shapes.Get({id.Index, id.Generation});
}

void SceneWriter::Queue(ShapeState& state) {
    if (state.DirtyIndex != std::numeric_limits<size_t>::max()) return;
    state.DirtyIndex = _dirtyShapes.size();
    _dirtyShapes.push_back(state.Id);
}

void SceneWriter::Unqueue(ShapeState& state) {
    if (state.DirtyIndex == std::numeric_limits<size_t>::max()) return;
    const auto moved = _dirtyShapes.back();
    _dirtyShapes[state.DirtyIndex] = moved;
    _shapes.Get({moved.Index, moved.Generation}).DirtyIndex = state.DirtyIndex;
    _dirtyShapes.pop_back();
    state.DirtyIndex = std::numeric_limits<size_t>::max();
}

ShapeId SceneWriter::CreateShape() {
    if (_closing) RADRAY_ABORT("Scene writer is closing");
    const auto id = _shapes.Emplace();
    auto& state = _shapes.Get({id.Index, id.Generation});
    state.Id = {id.Index, id.Generation};
    if (!_claimed) Queue(state);
    return state.Id;
}

void SceneWriter::RemoveShape(ShapeId id) {
    auto& state = GetShape(id);
    if (state.Sent) _removedShapes.push_back(id);
    if (state.Asset) _assets.RemoveUse(*state.Asset);
    Unqueue(state);
    if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Shape generation exhausted");
    _shapes.Destroy({id.Index, id.Generation});
}

void SceneWriter::SetStaticMesh(ShapeId id, const StreamingAssetRef<StaticMesh>& mesh, const Eigen::Matrix4f& localToWorld) {
    auto& state = GetShape(id);
    StaticMeshStateUpdate update;
    update.Id = id;
    update.LocalToWorld = localToWorld;
    update.Mesh.MeshAssetId = mesh.GetAssetId();
    std::optional<AssetId> next;
    if (auto asset = mesh.Get(); asset && asset->IsValid()) {
        update.Mesh.RenderMesh = &asset->GetRenderMesh();
        update.Mesh.LocalBoundsMin = asset->GetBoundsMin();
        update.Mesh.LocalBoundsMax = asset->GetBoundsMax();
        update.Mesh.Sections = asset->GetSections();
        _assets.AddUse(mesh.AsAny());
        next = mesh.GetAssetId();
    }
    if (state.Asset) _assets.RemoveUse(*state.Asset);
    state.Asset = next;
    state.Mesh = std::move(update);
    state.Transform.reset();
    state.HasMesh = true;
    Queue(state);
}

void SceneWriter::SetTransform(ShapeId id, const Eigen::Matrix4f& localToWorld) {
    auto& state = GetShape(id);
    if (!state.HasMesh) RADRAY_ABORT("Transform requires a static mesh state");
    if (state.Mesh)
        state.Mesh->LocalToWorld = localToWorld;
    else
        state.Transform = localToWorld;
    Queue(state);
}

LightId SceneWriter::CreateLight() {
    if (_closing) RADRAY_ABORT("Scene writer is closing");
    const auto id = _lightIds.Emplace();
    return {id.Index, id.Generation};
}

void SceneWriter::RemoveLight(LightId id) {
    const SparseSetHandle handle{id.Index, id.Generation};
    if (_closing || !_lightIds.IsAlive(handle)) RADRAY_ABORT("Invalid scene writer light");
    if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Light generation exhausted");
    if (_lights.Remove(id)) _lightsDirty = true;
    _lightIds.Destroy(handle);
}

void SceneWriter::SetLight(const LightData& light) {
    const auto id = std::visit([](const auto& value) { return value.Common.Id; }, light);
    if (_closing || !_lightIds.IsAlive({id.Index, id.Generation})) RADRAY_ABORT("Invalid scene writer light");
    _lights.Set(light);
    _lightsDirty = true;
}

void SceneWriter::Flush(SceneUpdateBatch& batch, uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("SceneWriter::Flush");
    if (!batch.Empty()) RADRAY_ABORT("Scene batch must be empty before collection");
    batch.RemoveShapes.insert(batch.RemoveShapes.end(), _removedShapes.begin(), _removedShapes.end());
    _removedShapes.clear();
    while (!_dirtyShapes.empty()) {
        const auto id = _dirtyShapes.back();
        auto& state = _shapes.Get({id.Index, id.Generation});
        if (!state.Sent) batch.CreateShapes.push_back(state.Id);
        if (state.Mesh)
            batch.MeshStates.push_back(std::move(*state.Mesh));
        else if (state.Transform)
            batch.Transforms.push_back({state.Id, *state.Transform});
        state.Mesh.reset();
        state.Transform.reset();
        state.Sent = true;
        Unqueue(state);
    }
    if (_lightsDirty) {
        batch.LightsChanged = true;
        batch.Lights.Assign(_lights);
        _lightsDirty = false;
    }
    _assets.SealRetirements(flightIndex);
    RADRAY_PROFILE_PLOT("SceneTransforms", static_cast<int64_t>(batch.Transforms.size()));
    RADRAY_PROFILE_PLOT("SceneMeshStates", static_cast<int64_t>(batch.MeshStates.size()));
}

}  // namespace radray
