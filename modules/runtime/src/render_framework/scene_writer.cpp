#include <radray/runtime/render_framework/scene_writer.h>

#include <radray/profiler.h>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <radray/hash.h>
#include <cstring>

namespace radray {
namespace {
template <class T>
void ReserveAppend(vector<T>& values, size_t additions) {
    if (additions <= values.capacity() - values.size()) return;
    if (additions > values.max_size() - values.size()) RADRAY_ABORT("Scene pending capacity exhausted");
    const auto growth = values.capacity() + std::min(values.capacity(), values.max_size() - values.capacity());
    values.reserve(std::max(values.size() + additions, growth));
}
}  // namespace

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
    if (_captured) EnableEditing();
    RemoveShapeState(id, state);
}
void SceneWriter::RemoveShapeState(ShapeId id, ShapeState& state) {
    if (state.DrawUpdateIndex != kNotQueued) {
        const auto index = state.DrawUpdateIndex;
        if (index + 1 != _pending.DrawStates.size()) {
            _pending.DrawStates[index] = _pending.DrawStates.back();
            GetShape(_pending.DrawStates[index].Id).DrawUpdateIndex = index;
        }
        _pending.DrawStates.pop_back();
    }
    while (!state.ParameterUpdateIndices.empty()) {
        const auto index = state.ParameterUpdateIndices.back().Index;
        ReclaimParameter(_pending.Parameters[index]);
        if (index + 1 != _pending.Parameters.size()) {
            _pending.Parameters[index] = std::move(_pending.Parameters.back());
            auto& moved = _pending.Parameters[index];
            auto& indices = GetShape(moved.Id).ParameterUpdateIndices;
            const auto found = std::find_if(indices.begin(), indices.end(), [&](const auto& value) { return value.Schema == moved.Schema; });
            found->Index = index;
        }
        _pending.Parameters.pop_back();
        state.ParameterUpdateIndices.pop_back();
    }
    if (state.Sent) _pending.RemoveShapes.push_back(id);
    if (state.BoundAsset) _assets.RemoveUse(*state.BoundAsset);
    ReleaseMaterials(state);
    if (state.MaterialUpdateIndex != kNotQueued) {
        const auto index = state.MaterialUpdateIndex;
        if (index + 1 != _pending.Materials.size()) {
            _pending.Materials[index] = std::move(_pending.Materials.back());
            GetShape(_pending.Materials[index].Id).MaterialUpdateIndex = index;
        }
        _pending.Materials.pop_back();
    }
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

void SceneWriter::WriteStaticMesh(ShapeId id, ShapeState& state, const StreamingAssetRef<StaticMesh>& mesh, const AffineTransform& localToWorld, TransformId transform) {
    StaticMeshDescription next;
    next.MeshAssetId = mesh.GetAssetId();
    if (auto asset = mesh.Get(); asset && asset->IsValid()) {
        next.RenderData = &asset->GetRenderData();
    }
    if (state.BoundAsset != (next.RenderData ? std::optional{next.MeshAssetId} : std::nullopt)) {
        if (next.RenderData) _assets.AddUse(mesh.AsAny());
        if (state.BoundAsset) _assets.RemoveUse(*state.BoundAsset);
    }
    state.BoundAsset = next.RenderData ? std::optional{next.MeshAssetId} : std::nullopt;
    WriteMeshState(id, state, next, localToWorld, transform);
}
void SceneWriter::WriteMeshState(ShapeId id, ShapeState& state, StaticMeshDescription next, const AffineTransform& localToWorld, TransformId transform) {
    if (_editing) {
        auto& edit = GetEdit(id);
        if (!edit.PendingMesh) CancelUpdate(_pending.Transforms, edit);
        if (edit.UpdateIndex == kNotQueued) {
            edit.UpdateIndex = _pending.MeshStates.size();
            _pending.MeshStates.push_back({id, next, localToWorld, transform});
        } else {
            _pending.MeshStates[edit.UpdateIndex] = {id, next, localToWorld, transform};
        }
        edit.PendingMesh = true;
    } else {
        _pending.MeshStates.push_back({id, next, localToWorld, transform});
    }
    state.HasMesh = true;
}

void SceneWriter::SetStaticMeshes(std::span<const ShapeId> ids, const StreamingAssetRef<StaticMesh>& mesh, std::span<const Eigen::Matrix4f> transforms) {
    EnableEditing();
    GatherStaticMeshes(ids, mesh, transforms);
}
void SceneWriter::GatherStaticMeshes(std::span<const ShapeId> ids, const StreamingAssetRef<StaticMesh>& mesh, std::span<const Eigen::Matrix4f> transforms) {
    if (ids.size() != transforms.size()) RADRAY_ABORT("Batch mesh transforms require one matrix per shape");
    StaticMeshDescription next{mesh.GetAssetId(), nullptr};
    const auto asset = mesh.Get();
    if (asset && asset->IsValid()) next.RenderData = &asset->GetRenderData();
    const auto bound = next.RenderData ? std::optional{next.MeshAssetId} : std::nullopt;
    size_t additions = 0;
    for (auto id : ids) {
        auto& state = GetShape(id);
        if (state.BoundAsset == bound) continue;
        if (state.BoundAsset) _assets.RemoveUse(*state.BoundAsset);
        state.BoundAsset = bound;
        ++additions;
    }
    if (next.RenderData) _assets.AddUses(mesh.AsAny(), additions);
    ReserveAppend(_pending.MeshStates, ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        auto& state = GetShape(ids[i]);
        QueueCreate(ids[i], state);
        WriteMeshState(ids[i], state, next, AffineTransform{transforms[i]}, {});
    }
}

void SceneWriter::ReleaseMaterials(ShapeState& state) {
    auto use = state.BoundMaterials;
    state.BoundMaterials = nullptr;
    if (!use || --use->Count) return;
    for (const auto& binding : use->Bindings)
        if (binding.Data) _assets.RemoveUse(binding.Id);
    auto it = _materialBindings.find(use->Hash);
    std::erase_if(it->second, [&](const auto& entry) { return entry.get() == use.Get(); });
    if (it->second.empty()) _materialBindings.erase(it);
}
void SceneWriter::SetMaterials(ShapeId id, std::span<const StreamingAssetRef<Material>> materials) {
    SetMaterials(std::span{&id, 1}, materials);
}
void SceneWriter::SetMaterials(std::span<const ShapeId> ids, std::span<const StreamingAssetRef<Material>> materials) {
    if (ids.empty()) return;
    const auto bindingAt = [&](size_t index) {
        const auto& reference = materials[index];
        const auto asset = reference.Get();
        const bool ready = asset && asset->IsValid();
        return MaterialBinding{reference.GetAssetId(), ready ? &asset->GetRenderData() : nullptr};
    };
    const auto same = [&](const MaterialBindings& bindings) {
        if (bindings.size() != materials.size()) return false;
        for (size_t i = 0; i < materials.size(); ++i)
            if (bindings[i] != bindingAt(i)) return false;
        return true;
    };
    bool unchanged = true;
    for (auto id : ids) {
        auto& state = GetShape(id);
        if (!state.HasMesh) RADRAY_ABORT("materials require a static mesh state");
        unchanged &= state.BoundMaterials && same(state.BoundMaterials->Bindings);
    }
    if (unchanged) return;
    HashCode hash;
    for (size_t i = 0; i < materials.size(); ++i) {
        const auto binding = bindingAt(i);
        hash.Add(binding.Id);
        hash.Add(binding.Data.Get());
    }
    const auto code = hash.ToHashCode();
    auto& bucket = _materialBindings[code];
    Nullable<MaterialUse*> use{nullptr};
    for (const auto& entry : bucket)
        if (same(entry->Bindings)) {
            use = entry.get();
            break;
        }
    if (!use) {
        vector<MaterialBinding> bindings;
        bindings.reserve(materials.size());
        for (size_t i = 0; i < materials.size(); ++i) {
            const auto binding = bindingAt(i);
            if (binding.Data) _assets.AddUse(materials[i].AsAny());
            bindings.push_back(binding);
        }
        auto created = make_unique<MaterialUse>();
        created->Bindings = MaterialBindings{std::move(bindings)};
        created->Hash = code;
        use = created.get();
        bucket.push_back(std::move(created));
    }
    // Protect the destination while old groups are released, including equal hash buckets.
    use->Count += ids.size();
    ReserveAppend(_pending.Materials, ids.size());
    for (auto id : ids) {
        auto& state = GetShape(id);
        ReleaseMaterials(state);
        state.BoundMaterials = use;
        ShapeMaterialUpdate update{id, _pending.RegisterMaterials(use->Bindings)};
        if (state.MaterialUpdateIndex == kNotQueued) {
            state.MaterialUpdateIndex = _pending.Materials.size();
            _pending.Materials.push_back(std::move(update));
        } else
            _pending.Materials[state.MaterialUpdateIndex] = std::move(update);
    }
}
bool SceneWriter::CreateShapes(std::span<ShapeId> result) {
    if (_closing || result.size() > UINT32_MAX - _shapes.Count()) return false;
    const auto capacity = std::min(std::bit_ceil(uint64_t{_shapes.Count()} + result.size()), uint64_t{UINT32_MAX});
    _shapes.Reserve(static_cast<uint32_t>(capacity));
    ReserveAppend(_pending.CreateShapes, result.size());
    if (!_claimed) EnableEditing();
    for (auto& id : result) {
        const auto handle = _shapes.Emplace();
        id = {handle.Index, handle.Generation};
        if (!_claimed) {
            GetEdit(id).CreateIndex = _pending.CreateShapes.size();
            _pending.CreateShapes.push_back(id);
        }
    }
    return true;
}
void SceneWriter::RemoveShapes(std::span<const ShapeId> ids) {
    if (ids.empty()) return;
    if (_captured) EnableEditing();
    ReserveAppend(_pending.RemoveShapes, ids.size());
    for (auto id : ids) RemoveShapeState(id, GetShape(id));
}
void SceneWriter::SetTransforms(std::span<const ShapeTransformUpdate> values) {
    if (values.empty()) return;
    EnableEditing();
    GatherTransforms(values);
}
void SceneWriter::GatherTransforms(std::span<const ShapeTransformUpdate> values) {
    ReserveAppend(_pending.Transforms, values.size());
    for (const auto& value : values) WriteTransform(value.Id, GetShape(value.Id), value.LocalToWorld);
}

void SceneWriter::SetTransform(ShapeId id, const Eigen::Matrix4f& localToWorld) {
    auto& state = GetShape(id);
    EnableEditing();
    WriteTransform(id, state, localToWorld);
}

void SceneWriter::ReclaimParameter(const ShapeParameterUpdate& update) {
    auto& payload = _parameterPayloads.at(update.Schema.Index);
    if (size_t{update.Offset} + update.Size == _pending.ParameterBytes.size())
        _pending.ParameterBytes.resize(update.Offset);
    else
        payload.Free.push_back(update.Offset);
}
bool SceneWriter::SetInstanceParameters(ShapeId id, const InstanceParameterLayout& layout, std::span<const byte> values) {
    return SetInstanceParameters(std::span{&id, 1}, layout, values);
}
bool SceneWriter::SetInstanceParameters(std::span<const ShapeId> ids, const InstanceParameterLayout& layout, std::span<const byte> values) {
    if (!layout.IsValid() || ids.size() > UINT32_MAX / layout.Stride || values.size() != ids.size() * layout.Stride) return false;
    for (auto id : ids)
        if (!GetShape(id).HasMesh) return false;
    auto [it, inserted] = _parameterSchemas.try_emplace(layout.Name);
    if (inserted)
        it->second = layout;
    else if (it->second != layout)
        return false;
    const auto schema = _pending.RegisterSchema(it->second);
    if (!schema) return false;
    auto& payload = _parameterPayloads[schema->Index];
    if (values.size() > UINT32_MAX - _pending.ParameterBytes.size()) return false;
    ReserveAppend(_pending.ParameterBytes, values.size());
    ReserveAppend(_pending.Parameters, ids.size());
    size_t source = 0;
    for (auto id : ids) {
        auto& indices = GetShape(id).ParameterUpdateIndices;
        const auto found = std::find_if(indices.begin(), indices.end(), [&](const auto& value) { return value.Schema == *schema; });
        uint32_t offset;
        if (found == indices.end()) {
            if (payload.Free.empty()) {
                offset = static_cast<uint32_t>(_pending.ParameterBytes.size());
                _pending.ParameterBytes.resize(size_t{offset} + layout.Stride);
            } else {
                offset = payload.Free.back();
                payload.Free.pop_back();
            }
            indices.push_back({*schema, _pending.Parameters.size()});
            _pending.Parameters.push_back({id, *schema, offset, layout.Stride});
        } else
            offset = _pending.Parameters[found->Index].Offset;
        std::memcpy(_pending.ParameterBytes.data() + offset, values.data() + source, layout.Stride);
        source += layout.Stride;
    }
    return true;
}

void SceneWriter::SetDrawState(ShapeId id, ShapeDrawState value) {
    WriteDrawState({id, value});
}
uint64_t SceneWriter::CreateBatchDomain() {
    if (_closing || _nextBatchDomain == UINT64_MAX) RADRAY_ABORT("Scene batch domain exhausted");
    return _nextBatchDomain++;
}
void SceneWriter::SetDrawStates(std::span<const ShapeDrawUpdate> values) {
    ReserveAppend(_pending.DrawStates, values.size());
    for (const auto& value : values) WriteDrawState(value);
}
void SceneWriter::WriteDrawState(const ShapeDrawUpdate& update) {
    auto& state = GetShape(update.Id);
    if (state.DrawUpdateIndex == kNotQueued) {
        state.DrawUpdateIndex = _pending.DrawStates.size();
        _pending.DrawStates.push_back(update);
    } else
        _pending.DrawStates[state.DrawUpdateIndex] = update;
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

void SceneWriter::Flush(SceneUpdateBatch& batch, uint32_t flightIndex, bool requireExitConfirmation) {
    RADRAY_PROFILE_SCOPE_N("SceneWriter::Flush");
    if (!batch.Empty()) RADRAY_ABORT("Scene batch must be empty before collection");
    for (const auto& value : _pending.CreateTransforms) {
        auto& state = _transforms.Get({value.Id.Index, value.Id.Generation});
        state.Sent = true;
        state.CreateIndex = kNotQueued;
    }
    if (_transformEditEpoch == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Transform edit epoch exhausted");
    ++_transformEditEpoch;
    for (const auto id : _pending.CreateShapes) {
        auto& state = _shapes.Get({id.Index, id.Generation});
        state.Sent = true;
        if (_editing) _edits[id.Index] = {};
    }
    if (_editing) {
        for (const auto& update : _pending.MeshStates) _edits[update.Id.Index] = {};
        for (const auto& update : _pending.Transforms) _edits[update.Id.Index] = {};
    }
    for (const auto& update : _pending.Materials) GetShape(update.Id).MaterialUpdateIndex = kNotQueued;
    for (const auto& update : _pending.Parameters) GetShape(update.Id).ParameterUpdateIndices.clear();
    for (const auto& update : _pending.DrawStates) GetShape(update.Id).DrawUpdateIndex = kNotQueued;
    if (_claimed) _editing = false;
    _captured = false;
    if (_pending.LightsChanged) _pending.Lights.Assign(_lights);
    std::swap(batch, _pending);
    _parameterPayloads.clear();
    batch.AssetExits = _assets.SealRetirements(flightIndex, requireExitConfirmation, _closing);
    RADRAY_PROFILE_PLOT("SceneTransforms", static_cast<int64_t>(batch.Transforms.size()));
    RADRAY_PROFILE_PLOT("SceneMeshStates", static_cast<int64_t>(batch.MeshStates.size()));
}

TransformId SceneWriter::CreateTransform(TransformId parent, const LocalTransform& local) {
    if (_closing) RADRAY_ABORT("Scene writer is closing");
    const auto handle = _transforms.Emplace();
    const TransformId id{handle.Index, handle.Generation};
    auto& state = _transforms.Get(handle);
    state.CreateIndex = _pending.CreateTransforms.size();
    state.Parent = parent;
    _pending.CreateTransforms.push_back({id, parent, local});
    return id;
}
void SceneWriter::RemoveTransform(TransformId id) {
    auto& state = _transforms.Get({id.Index, id.Generation});
    BeginTransformEdit(state);
    const auto cancel = [&](auto& values, size_t TransformState::* member) {
        const auto index = state.*member;
        if (index == kNotQueued) return;
        if (index != values.size() - 1) {
            values[index] = std::move(values.back());
            const auto moved = values[index].Id;
            _transforms.Get({moved.Index, moved.Generation}).*member = index;
        }
        values.pop_back();
    };
    cancel(_pending.CreateTransforms, &TransformState::CreateIndex);
    cancel(_pending.LocalTransforms, &TransformState::LocalIndex);
    cancel(_pending.TransformParents, &TransformState::ParentIndex);
    if (state.Sent) _pending.RemoveTransforms.push_back(id);
    if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Transform generation exhausted");
    _transforms.Destroy({id.Index, id.Generation});
}
void SceneWriter::ReserveLocalTransforms(size_t count) {
    const size_t required = _pending.LocalTransforms.size() + count;
    if (required > _pending.LocalTransforms.capacity())
        _pending.LocalTransforms.reserve(std::max(required, _pending.LocalTransforms.capacity() * 2));
}
void SceneWriter::WriteLocalTransform(const LocalTransformUpdate& update) {
    if (!update.Id.IsValid()) return;
    auto& state = _transforms.Get({update.Id.Index, update.Id.Generation});
    if (!state.Sent) {
        _pending.CreateTransforms[state.CreateIndex].Local = update.Local;
        return;
    }
    BeginTransformEdit(state);
    if (state.LocalIndex == kNotQueued) {
        state.LocalIndex = _pending.LocalTransforms.size();
        _pending.LocalTransforms.push_back(update);
    } else {
        _pending.LocalTransforms[state.LocalIndex].Local = update.Local;
    }
}
void SceneWriter::SetLocalTransforms(std::span<const LocalTransformUpdate> updates) {
    ReserveLocalTransforms(updates.size());
    for (const auto& update : updates) WriteLocalTransform(update);
}
void SceneWriter::GatherLocalTransforms(std::span<const LocalTransformUpdate> values, std::span<const uint32_t> indices) {
    ReserveLocalTransforms(indices.size());
    for (const auto index : indices) WriteLocalTransform(values[index]);
}
void SceneWriter::SetTransformParent(TransformId id, TransformId parent) {
    auto& state = _transforms.Get({id.Index, id.Generation});
    if (state.Parent == parent) return;
    if (!state.Sent) {
        _pending.CreateTransforms[state.CreateIndex].Parent = parent;
    } else {
        BeginTransformEdit(state);
        if (state.ParentIndex == kNotQueued) {
            state.ParentIndex = _pending.TransformParents.size();
            _pending.TransformParents.push_back({id, parent});
        } else {
            _pending.TransformParents[state.ParentIndex].Parent = parent;
        }
    }
    state.Parent = parent;
}

}  // namespace radray
