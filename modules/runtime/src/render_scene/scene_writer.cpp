#include <radray/runtime/render_scene/scene_writer.h>

namespace radray {

SceneWriter::SceneWriter(SceneId id, uint32_t flightCount) : _id(id), _assets(flightCount) {}
SceneWriter::~SceneWriter() noexcept = default;

SceneWriter::PrimitiveState& SceneWriter::GetPrimitive(PrimitiveId id) {
    if (_closing || !_primitives.IsAlive(id)) RADRAY_ABORT("Invalid scene writer primitive");
    return _primitives.Get(id);
}

void SceneWriter::Queue(PrimitiveState& state) {
    if (state.DirtyIndex != std::numeric_limits<size_t>::max()) return;
    state.DirtyIndex = _dirty.size();
    _dirty.push_back(state.Id);
}

void SceneWriter::Unqueue(PrimitiveState& state) {
    if (state.DirtyIndex == std::numeric_limits<size_t>::max()) return;
    const auto moved = _dirty.back();
    _dirty[state.DirtyIndex] = moved;
    _primitives.Get(moved).DirtyIndex = state.DirtyIndex;
    _dirty.pop_back();
    state.DirtyIndex = std::numeric_limits<size_t>::max();
}

PrimitiveId SceneWriter::CreatePrimitive() {
    if (_closing) RADRAY_ABORT("Scene writer is closing");
    const auto id = _primitives.Emplace();
    auto& state = _primitives.Get(id);
    state.Id = id;
    if (!_claimed) Queue(state);
    return id;
}

void SceneWriter::RemovePrimitive(PrimitiveId id) {
    auto& state = GetPrimitive(id);
    if (state.Sent) _removed.push_back(id);
    if (state.Asset) _assets.RemoveUse(*state.Asset);
    Unqueue(state);
    if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Primitive generation exhausted");
    _primitives.Destroy(id);
}

void SceneWriter::SetStaticMesh(PrimitiveId id, const StreamingAssetRef<StaticMesh>& mesh, const Eigen::Matrix4f& localToWorld) {
    auto& state = GetPrimitive(id);
    if (state.HasLight) RADRAY_ABORT("Cannot change primitive type");
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

void SceneWriter::SetTransform(PrimitiveId id, const Eigen::Matrix4f& localToWorld) {
    auto& state = GetPrimitive(id);
    if (!state.HasMesh) RADRAY_ABORT("Transform requires a static mesh state");
    if (state.Mesh)
        state.Mesh->LocalToWorld = localToWorld;
    else
        state.Transform = localToWorld;
    Queue(state);
}

void SceneWriter::SetLight(const LightStateUpdate& light) {
    auto& state = GetPrimitive(light.Id);
    if (state.HasMesh) RADRAY_ABORT("Cannot change primitive type");
    state.Light = light;
    state.HasLight = true;
    Queue(state);
}

void SceneWriter::Flush(SceneUpdateBatch& batch, uint32_t flightIndex) {
    if (!batch.Empty()) RADRAY_ABORT("Scene batch must be empty before collection");
    batch.RemovePrimitives.insert(batch.RemovePrimitives.end(), _removed.begin(), _removed.end());
    _removed.clear();
    while (!_dirty.empty()) {
        auto& state = _primitives.Get(_dirty.back());
        if (!state.Sent) batch.CreatePrimitives.push_back(state.Id);
        if (state.Mesh)
            batch.MeshStates.push_back(std::move(*state.Mesh));
        else if (state.Transform)
            batch.Transforms.push_back({state.Id, *state.Transform});
        if (state.Light) batch.Lights.push_back(*state.Light);
        state.Mesh.reset();
        state.Transform.reset();
        state.Light.reset();
        state.Sent = true;
        Unqueue(state);
    }
    _assets.SealRetirements(flightIndex);
}

}  // namespace radray
