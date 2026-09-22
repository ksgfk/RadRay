#include <radray/runtime/render_scene/scene_writer.h>

#include <radray/profiler.h>
#include <cstdlib>

namespace radray {

SceneWriter::SceneWriter(SceneId id, uint32_t flightCount) : _id(id), _assets(flightCount) {}
SceneWriter::~SceneWriter() noexcept = default;

SceneWriter::ShapeState& SceneWriter::GetShape(ShapeId id) {
    if (auto* state = _shapes.TryGet({id.Index, id.Generation}); state != nullptr && !_closing) {
        return *state;
    }
    RADRAY_ABORT("Invalid scene writer shape");
    std::abort();
}

SceneWriter::ShapeEdit& SceneWriter::GetEdit(ShapeId id) {
    if (id.Index >= _edits.size()) _edits.resize(static_cast<size_t>(id.Index) + 1);
    return _edits[id.Index];
}

void SceneWriter::EnableEditing() {
    if (_editing) return;
    _editing = true;
    for (size_t i = 0; i < _pending.CreateShapes.size(); ++i) GetEdit(_pending.CreateShapes[i]).CreateIndex = i;
    for (size_t i = 0; i < _pending.MeshStates.size(); ++i) {
        auto& edit = GetEdit(_pending.MeshStates[i].Id);
        edit.UpdateIndex = i;
        edit.PendingMesh = true;
    }
    for (size_t i = 0; i < _pending.Transforms.size(); ++i) GetEdit(_pending.Transforms[i].Id).UpdateIndex = i;
}

void SceneWriter::BeginCapture() {
    if (!_claimed || _closing) RADRAY_ABORT("Capture requires a World-owned scene");
    if (_captured) EnableEditing();
    _captured = true;
}

void SceneWriter::QueueCreate(ShapeId id, ShapeState& state) {
    if (state.Sent) return;
    if (_editing) {
        auto& edit = GetEdit(id);
        if (edit.CreateIndex != kNotQueued) return;
        edit.CreateIndex = _pending.CreateShapes.size();
    }
    _pending.CreateShapes.push_back(id);
}

void SceneWriter::CancelCreate(ShapeEdit& edit) {
    if (edit.CreateIndex == kNotQueued) return;
    const auto moved = _pending.CreateShapes.back();
    _pending.CreateShapes[edit.CreateIndex] = moved;
    _edits[moved.Index].CreateIndex = edit.CreateIndex;
    _pending.CreateShapes.pop_back();
    edit.CreateIndex = kNotQueued;
}

template <class T>
void SceneWriter::CancelUpdate(vector<T>& updates, ShapeEdit& edit) {
    if (edit.UpdateIndex == kNotQueued) return;
    if (edit.UpdateIndex != updates.size() - 1) {
        auto& moved = updates[edit.UpdateIndex];
        moved = std::move(updates.back());
        _edits[moved.Id.Index].UpdateIndex = edit.UpdateIndex;
    }
    updates.pop_back();
    edit.UpdateIndex = kNotQueued;
}

ShapeId SceneWriter::CreateShape() {
    if (_closing) RADRAY_ABORT("Scene writer is closing");
    const auto handle = _shapes.Emplace();
    const ShapeId id{handle.Index, handle.Generation};
    if (!_claimed) {
        EnableEditing();
        QueueCreate(id, _shapes.Get(handle));
    }
    return id;
}

void SceneWriter::RemoveShape(ShapeId id) {
    auto& state = GetShape(id);
    if (state.Sent) _pending.RemoveShapes.push_back(id);
    if (state.BoundAsset) _assets.RemoveUse(*state.BoundAsset);
    if (_captured) EnableEditing();
    if (_editing) {
        auto& edit = GetEdit(id);
        CancelCreate(edit);
        if (edit.PendingMesh)
            CancelUpdate(_pending.MeshStates, edit);
        else
            CancelUpdate(_pending.Transforms, edit);
        edit = {};
    }
    if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Shape generation exhausted");
    _shapes.Destroy({id.Index, id.Generation});
}

void SceneWriter::SetStaticMesh(ShapeId id, const StreamingAssetRef<StaticMesh>& mesh, const Eigen::Matrix4f& localToWorld) {
    auto& state = GetShape(id);
    EnableEditing();
    WriteStaticMesh(id, state, mesh, localToWorld);
    QueueCreate(id, state);
}

void SceneWriter::WriteStaticMesh(ShapeId id, ShapeState& state, const StreamingAssetRef<StaticMesh>& mesh, const AffineTransform& localToWorld) {
    StaticMeshDescription next;
    next.MeshAssetId = mesh.GetAssetId();
    if (auto asset = mesh.Get(); asset && asset->IsValid()) {
        next.RenderData = &asset->GetRenderData();
        _assets.AddUse(mesh.AsAny());
    }
    if (state.BoundAsset) _assets.RemoveUse(*state.BoundAsset);
    state.BoundAsset = next.RenderData ? std::optional{next.MeshAssetId} : std::nullopt;
    if (_editing) {
        auto& edit = GetEdit(id);
        if (!edit.PendingMesh) CancelUpdate(_pending.Transforms, edit);
        if (edit.UpdateIndex == kNotQueued) {
            edit.UpdateIndex = _pending.MeshStates.size();
            _pending.MeshStates.push_back({id, next, localToWorld});
        } else {
            _pending.MeshStates[edit.UpdateIndex] = {id, next, localToWorld};
        }
        edit.PendingMesh = true;
    } else {
        _pending.MeshStates.push_back({id, next, localToWorld});
    }
    state.HasMesh = true;
}

void SceneWriter::SetTransform(ShapeId id, const Eigen::Matrix4f& localToWorld) {
    auto& state = GetShape(id);
    EnableEditing();
    WriteTransform(id, state, localToWorld);
}

void SceneWriter::WriteTransform(ShapeId id, ShapeState& state, const AffineTransform& localToWorld) {
    if (!state.HasMesh) RADRAY_ABORT("Transform requires a static mesh state");
    if (_editing) {
        auto& edit = GetEdit(id);
        if (edit.PendingMesh) {
            _pending.MeshStates[edit.UpdateIndex].LocalToWorld = localToWorld;
        } else if (edit.UpdateIndex != kNotQueued) {
            _pending.Transforms[edit.UpdateIndex].LocalToWorld = localToWorld;
        } else {
            edit.UpdateIndex = _pending.Transforms.size();
            _pending.Transforms.push_back({id, localToWorld});
        }
    } else {
        _pending.Transforms.push_back({id, localToWorld});
    }
}

LightId SceneWriter::CreateLight() {
    if (_closing) RADRAY_ABORT("Scene writer is closing");
    const auto id = _lightIds.Emplace();
    return {id.Index, id.Generation};
}

void SceneWriter::RemoveLight(LightId id) {
    const SparseSetHandle handle{id.Index, id.Generation};
    auto* slot = _lightIds.TryGet(handle);
    if (_closing || !slot) RADRAY_ABORT("Invalid scene writer light");
    if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Light generation exhausted");
    RemoveLightData(*slot);
    _lightIds.Destroy(handle);
}

void SceneWriter::RemoveLightData(LightSlot& slot) {
    if (slot.Row == kNoLightRow) return;
    const auto remove = [&](auto& table) {
        if (slot.Row != table.Ids.size() - 1) {
            const auto moved = table.Ids.back();
            table.Ids[slot.Row] = moved;
            table.Data[slot.Row] = std::move(table.Data.back());
            _lightIds.Get({moved.Index, moved.Generation}).Row = slot.Row;
        }
        table.Ids.pop_back();
        table.Data.pop_back();
    };
    switch (slot.Type) {
        case 0: remove(_lights.DirectionalLights); break;
        case 1: remove(_lights.PointLights); break;
        case 2: remove(_lights.SpotLights); break;
        case 3: remove(_lights.RectLights); break;
        default: RADRAY_ABORT("Invalid light table");
    }
    slot.Row = kNoLightRow;
    _pending.LightsChanged = true;
}

void SceneWriter::SetLight(LightId id, const LightData& light) {
    auto* slot = _lightIds.TryGet({id.Index, id.Generation});
    if (_closing || !slot) RADRAY_ABORT("Invalid scene writer light");
    if (slot->Type != light.index()) RemoveLightData(*slot);
    std::visit([&](const auto& value) {
        auto& table = _lights.GetTable<std::decay_t<decltype(value)>>();
        if (slot->Row == kNoLightRow) {
            slot->Row = static_cast<uint32_t>(table.Ids.size());
            slot->Type = static_cast<uint32_t>(light.index());
            table.Ids.push_back(id);
            table.Data.push_back(value);
        } else {
            table.Data[slot->Row] = value;
        }
    },
               light);
    _pending.LightsChanged = true;
}

void SceneWriter::Flush(SceneUpdateBatch& batch, uint32_t flightIndex) {
    RADRAY_PROFILE_SCOPE_N("SceneWriter::Flush");
    if (!batch.Empty()) RADRAY_ABORT("Scene batch must be empty before collection");
    for (const auto id : _pending.CreateShapes) {
        auto& state = _shapes.Get({id.Index, id.Generation});
        state.Sent = true;
        if (_editing) _edits[id.Index] = {};
    }
    if (_editing) {
        for (const auto& update : _pending.MeshStates) _edits[update.Id.Index] = {};
        for (const auto& update : _pending.Transforms) _edits[update.Id.Index] = {};
    }
    if (_claimed) _editing = false;
    _captured = false;
    if (_pending.LightsChanged) _pending.Lights.Assign(_lights);
    std::swap(batch, _pending);
    _assets.SealRetirements(flightIndex);
    RADRAY_PROFILE_PLOT("SceneTransforms", static_cast<int64_t>(batch.Transforms.size()));
    RADRAY_PROFILE_PLOT("SceneMeshStates", static_cast<int64_t>(batch.MeshStates.size()));
}

}  // namespace radray
