#include <radray/runtime/render_framework/render_scene.h>
#include <radray/hash.h>

#include <mutex>
#include <algorithm>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>

namespace radray {

RenderScene::RenderScene() noexcept = default;
RenderScene::~RenderScene() noexcept {
    std::unique_lock lock{_readers};
    _readersDone.wait(lock, [this] { return _activeReaders == 0; });
}

RenderScene::ReadLease::ReadLease(const RenderScene* scene) noexcept : _scene(scene) {}
RenderScene::ReadLease::ReadLease(ReadLease&& other) noexcept : _scene(std::exchange(other._scene, nullptr)) {}
RenderScene::ReadLease& RenderScene::ReadLease::operator=(ReadLease&& other) noexcept {
    if (this != &other) {
        Release();
        _scene = std::exchange(other._scene, nullptr);
    }
    return *this;
}
RenderScene::ReadLease::~ReadLease() noexcept { Release(); }
void RenderScene::ReadLease::Release() noexcept {
    if (!_scene) return;
    auto scene = std::exchange(_scene, nullptr);
    std::lock_guard lock{scene->_readers};
    if (--scene->_activeReaders == 0) scene->_readersDone.notify_all();
}
RenderScene::ReadLease RenderScene::AcquireRead() const {
    std::lock_guard lock{_readers};
    ++_activeReaders;
    return ReadLease{this};
}

void RenderScene::Apply(const SceneUpdateBatch& batch, Nullable<SceneApplyChanges*> changes) noexcept {
    RADRAY_PROFILE_SCOPE_N("RenderScene::Apply");
    std::unique_lock lock{_readers};
    _readersDone.wait(lock, [this] { return _activeReaders == 0; });
    if (changes) {
        changes->Updated.clear();
        changes->Removed.clear();
        changes->Parameters.clear();
    }
    _transforms.BeginApply();
    if (!batch.RemoveShapes.empty() || !batch.CreateShapes.empty() || !batch.MeshStates.empty() || !batch.Materials.empty()) ++_structureRevision;
    for (ShapeId id : batch.RemoveShapes) {
        if (!ContainsShape(id)) RADRAY_ABORT("Invalid shape removal");
        auto& slot = _shapes[id.Index];
        ReleaseDrawTemplate(id.Index);
        ReleaseMaterialList(id.Index);
        for (auto& [name, stream] : _parameters) stream.Remove(id.Index);
        if (changes) changes->Removed.push_back(id);
        UnbindShape(id.Index);
        if (slot.MeshIndex != kNoMesh) {
            const uint32_t removed = slot.MeshIndex;
            const auto ids = _staticMeshes.GetColumns().Ids;
            const uint32_t last = static_cast<uint32_t>(ids.size() - 1);
            if (removed != last) {
                const ShapeId moved = ids[last];
                _shapes[moved.Index].MeshIndex = removed;
            }
            _staticMeshes.Remove(removed);
            slot.MeshIndex = kNoMesh;
        }
        slot.Alive = false;
        if (slot.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Shape generation exhausted");
        ++slot.Generation;
    }
    for (const auto id : batch.RemoveTransforms) _transforms.Remove(id);
    for (const auto& update : batch.CreateTransforms) _transforms.Create(update.Id, update.Local);
    for (const auto& update : batch.CreateTransforms) _transforms.SetParent(update.Id, update.Parent);
    for (const auto& update : batch.TransformParents) _transforms.SetParent(update.Id, update.Parent);
    for (const auto& update : batch.LocalTransforms) _transforms.SetLocal(update.Id, update.Local);
    for (ShapeId id : batch.CreateShapes) {
        if (!id.IsValid()) RADRAY_ABORT("Invalid shape creation");
        if (id.Index >= _shapes.size()) {
            _shapes.resize(static_cast<size_t>(id.Index) + 1);
        }
        auto& slot = _shapes[id.Index];
        if (slot.Alive || id.Generation < slot.Generation) RADRAY_ABORT("Stale shape creation");
        slot.Generation = id.Generation;
        slot.Alive = true;
        slot.Revision = {};
        slot.Draw = {};
        if (id.Index < _shapeTemplates.size()) _shapeTemplates[id.Index] = nullptr;
    }
    for (const auto& update : batch.MeshStates) {
        if (!ContainsShape(update.Id)) RADRAY_ABORT("Invalid static mesh state update");
        auto& slot = _shapes[update.Id.Index];
        ++slot.Revision.Geometry;
        if (update.Transform.IsValid()) {
            const auto row = _transforms.GetRow(update.Transform);
            if (slot.TransformRow != row || slot.OwnTransform) {
                UnbindShape(update.Id.Index);
                BindShape(update.Id.Index, row, false);
            }
        } else if (slot.OwnTransform) {
            _transforms.SetStandalone(slot.TransformRow, update.LocalToWorld);
        } else {
            UnbindShape(update.Id.Index);
            BindShape(update.Id.Index, _transforms.CreateStandalone(update.LocalToWorld), true);
        }
        if (slot.MeshIndex != kNoMesh) {
            _staticMeshes.Replace(slot.MeshIndex, update, slot.TransformRow);
        } else {
            slot.MeshIndex = static_cast<uint32_t>(_staticMeshes.GetColumns().Size());
            _staticMeshes.Add(update, slot.TransformRow);
        }
    }
    for (const auto& update : batch.Transforms) {
        if (!ContainsShape(update.Id) || _shapes[update.Id.Index].MeshIndex == kNoMesh) RADRAY_ABORT("Invalid shape transform update");
        const auto& slot = _shapes[update.Id.Index];
        if (!slot.OwnTransform) RADRAY_ABORT("A bound shape must update its local transform node");
        _transforms.SetStandalone(slot.TransformRow, update.LocalToWorld);
    }
    for (const auto& update : batch.Materials) {
        if (!ContainsShape(update.Id) || _shapes[update.Id.Index].MeshIndex == kNoMesh) RADRAY_ABORT("Invalid shape material update");
        const auto values = batch.GetMaterials(update.Materials);
        if (!values) RADRAY_ABORT("Invalid material list packet id");
        SetMaterials(update.Id, *values);
        ++_shapes[update.Id.Index].Revision.Materials;
    }
    for (const auto& update : batch.Parameters) {
        const auto values = batch.GetParameterValues(update);
        const auto layout = batch.GetSchema(update.Schema);
        if (!ContainsShape(update.Id) || !layout || !layout->IsValid() || values.size() != layout->Stride) RADRAY_ABORT("Invalid instance parameter update");
        auto [it, inserted] = _parameters.try_emplace(layout->Name);
        auto& stream = it->second;
        if (inserted)
            stream.Layout = *layout.Get();
        else if (stream.Layout != *layout.Get())
            RADRAY_ABORT("Instance schema changed within a scene generation");
        if (update.Id.Index >= stream.Rows.size()) stream.Rows.resize(size_t{update.Id.Index} + 1);
        auto& row = stream.Rows[update.Id.Index];
        if (!row || row->Id != update.Id || !std::equal(values.begin(), values.end(), stream.GetValues(update.Id.Index).begin())) {
            stream.Set(update.Id, values);
            ++_shapes[update.Id.Index].Revision.Parameters;
            if (changes) changes->Parameters.push_back({update.Id, layout->Name});
        }
    }
    for (const auto& update : batch.DrawStates) {
        if (!ContainsShape(update.Id)) RADRAY_ABORT("Invalid shape draw state");
        auto& slot = _shapes[update.Id.Index];
        if (slot.Draw != update.State) {
            slot.Draw = update.State;
            ++slot.Revision.Order;
        }
    }
    // Repeated edits use the final table value; unchanged keys do not rebuild templates.
    for (const auto& update : batch.MeshStates) UpdateDrawTemplate(update.Id);
    for (const auto& update : batch.Materials) UpdateDrawTemplate(update.Id);
    for (auto it = _materialLists.begin(); it != _materialLists.end();) {
        std::erase_if(it->second, [](const auto& list) { return list->Uses == 0; });
        if (it->second.empty()) it = _materialLists.erase(it);
        else ++it;
    }
    const auto changed = _transforms.Evaluate();
    for (auto row : changed) {
        for (auto shape = _transforms.GetFirstShape(row); shape != kNoMesh; shape = _shapes[shape].NextShape) {
            _staticMeshes.UpdateBounds(_shapes[shape].MeshIndex, _transforms.GetWorld(row));
            ++_shapes[shape].Revision.Transform;
            if (changes) changes->Updated.push_back({shape, _shapes[shape].Generation});
        }
    }
    for (const auto& update : batch.MeshStates) {
        const auto& slot = _shapes[update.Id.Index];
        if (!_transforms.WasUpdated(slot.TransformRow)) {
            _staticMeshes.UpdateBounds(slot.MeshIndex, _transforms.GetWorld(slot.TransformRow));
            if (changes) changes->Updated.push_back(update.Id);
        }
    }
    if (changes) {
        auto& ids = changes->Updated;
        std::sort(ids.begin(), ids.end(), [](ShapeId a, ShapeId b) { return a.Index < b.Index; });
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    if (!batch.LightsChanged && !batch.Lights.Empty()) RADRAY_ABORT("Light snapshot requires LightsChanged");
    if (batch.LightsChanged || !changed.empty()) UpdateLights(batch);
}

void RenderScene::ReleaseMaterialList(uint32_t shape) {
    if (shape < _shapeMaterials.size() && _shapeMaterials[shape]) {
        --_shapeMaterials[shape]->Uses;
        _shapeMaterials[shape] = nullptr;
    }
}
void RenderScene::SetMaterials(ShapeId id, std::span<const MaterialBinding> values) {
    HashCode hash;
    for (const auto& value : values) {
        hash.Add(value.Id);
        hash.Add(value.Data.Get());
    }
    auto& bucket = _materialLists[hash.ToHashCode()];
    Nullable<MaterialList*> entry{nullptr};
    for (const auto& list : bucket)
        if (std::ranges::equal(list->Values, values)) { entry = list.get(); break; }
    if (!entry) {
        auto list = make_unique<MaterialList>();
        list->Values.assign(values.begin(), values.end());
        entry = list.get();
        bucket.push_back(std::move(list));
    }
    if (id.Index >= _shapeMaterials.size()) _shapeMaterials.resize(_shapes.size());
    ReleaseMaterialList(id.Index);
    ++entry->Uses;
    _shapeMaterials[id.Index] = entry;
    _staticMeshes.SetMaterials(_shapes[id.Index].MeshIndex, entry->Values);
}
size_t RenderScene::TemplateHash::operator()(const TemplateKey& key) const noexcept {
    HashCode h;
    h.Add(key.Mesh);
    h.Add(key.Materials);
    h.Add(key.Count);
    return h.ToHashCode();
}
void RenderScene::ReleaseDrawTemplate(uint32_t index) {
    if (index >= _shapeTemplates.size()) return;
    auto& pointer = _shapeTemplates[index];
    if (!pointer) return;
    auto* entry = pointer.Get();
    pointer = nullptr;
    if (--entry->Uses == 0) _drawTemplates.erase(*entry->Key);
}
void RenderScene::UpdateDrawTemplate(ShapeId id) {
    if (!ContainsShape(id) || _shapes[id.Index].MeshIndex == kNoMesh) return;
    const auto row = _shapes[id.Index].MeshIndex;
    const auto columns = _staticMeshes.GetColumns();
    const auto& binding = columns.Bindings[row];
    const auto mesh = binding.GetRenderMesh();
    if (!mesh || mesh->Draws.empty() || binding.GetSections().empty()) {
        ReleaseDrawTemplate(id.Index);
        return;
    }
    if (id.Index >= _shapeTemplates.size()) _shapeTemplates.resize(_shapes.size(), nullptr);
    auto& pointer = _shapeTemplates[id.Index];
    const auto materials = std::span<const MaterialBinding>{columns.Materials[row]};
    const TemplateKey key{binding.RenderData.Get(), materials.data(), materials.size()};
    if (pointer && *pointer->Key == key) return;
    ReleaseDrawTemplate(id.Index);
    auto [it, inserted] = _drawTemplates.try_emplace(key);
    if (inserted) {
        it->second.Key = &it->first;
        for (const auto& section : binding.GetSections()) {
            if (!section.IndexCount || section.PrimitiveIndex >= mesh->Draws.size()) continue;
            it->second.Primitives.push_back({&mesh->Draws[section.PrimitiveIndex], &section,
                                             section.MaterialSlot < materials.size() ? materials[section.MaterialSlot] : MaterialBinding{{}, nullptr}});
        }
    }
    ++it->second.Uses;
    pointer = &it->second;
}
std::span<const RenderScene::DrawPrimitive> RenderScene::GetDrawPrimitives(ShapeId id) const noexcept {
    if (!ContainsShape(id) || id.Index >= _shapeTemplates.size() || !_shapeTemplates[id.Index]) return {};
    return _shapeTemplates[id.Index]->Primitives;
}

bool RenderScene::ContainsShape(ShapeId id) const noexcept {
    return id.IsValid() && id.Index < _shapes.size() &&
           _shapes[id.Index].Alive && _shapes[id.Index].Generation == id.Generation;
}

std::optional<StaticMeshSceneView> RenderScene::GetStaticMesh(ShapeId id) const noexcept {
    if (!ContainsShape(id) || _shapes[id.Index].MeshIndex == kNoMesh) return std::nullopt;
    return GetStaticMeshColumns().Get(_shapes[id.Index].MeshIndex);
}
Nullable<const Eigen::Matrix4f*> RenderScene::GetStaticMeshTransform(ShapeId id) const noexcept {
    if (!ContainsShape(id) || _shapes[id.Index].MeshIndex == kNoMesh) return nullptr;
    return &_transforms.GetWorld(_shapes[id.Index].TransformRow);
}
std::optional<ShapeDrawRevision> RenderScene::GetDrawRevision(ShapeId id) const noexcept {
    return ContainsShape(id) ? std::optional{_shapes[id.Index].Revision} : std::nullopt;
}
std::optional<ShapeDrawState> RenderScene::GetDrawState(ShapeId id) const noexcept {
    return ContainsShape(id) ? std::optional{_shapes[id.Index].Draw} : std::nullopt;
}

void RenderScene::UnbindShape(uint32_t index) {
    auto& slot = _shapes[index];
    if (slot.TransformRow == kNoMesh) return;
    if (_transforms.GetFirstShape(slot.TransformRow) == index) _transforms.SetFirstShape(slot.TransformRow, slot.NextShape);
    if (slot.PreviousShape != kNoMesh) _shapes[slot.PreviousShape].NextShape = slot.NextShape;
    if (slot.NextShape != kNoMesh) _shapes[slot.NextShape].PreviousShape = slot.PreviousShape;
    if (slot.OwnTransform) _transforms.RemoveStandalone(slot.TransformRow);
    slot.TransformRow = slot.NextShape = slot.PreviousShape = kNoMesh;
    slot.OwnTransform = false;
}
void RenderScene::BindShape(uint32_t index, uint32_t row, bool own) {
    auto& slot = _shapes[index];
    slot.TransformRow = row;
    slot.OwnTransform = own;
    slot.NextShape = _transforms.GetFirstShape(row);
    slot.PreviousShape = kNoMesh;
    if (slot.NextShape != kNoMesh) _shapes[slot.NextShape].PreviousShape = index;
    _transforms.SetFirstShape(row, index);
}
void RenderScene::UpdateLights(const SceneUpdateBatch& batch) {
    const auto setPose = [](auto& light, const Eigen::Matrix4f& world) {
        const Eigen::Affine3f affine{world};
        Eigen::Vector3f direction = Eigen::Quaternionf{affine.rotation()} * Eigen::Vector3f::UnitZ();
        if (direction.squaredNorm() <= 1e-8f)
            direction = Eigen::Vector3f::UnitZ();
        else
            direction.normalize();
        using T = std::decay_t<decltype(light)>;
        if constexpr (std::is_same_v<T, DirectionalLightData>) {
            light.Direction = direction;
        } else if constexpr (std::is_same_v<T, RectLightData>) {
            light.Position = Eigen::Vector3f{world.data()[12], world.data()[13], world.data()[14]};
            light.Direction = direction;
        } else {
            light.Point.Position = Eigen::Vector3f{world.data()[12], world.data()[13], world.data()[14]};
            light.Point.Direction = direction;
        }
    };
    const auto preservePose = [](auto& light, const auto& previous) {
        using T = std::decay_t<decltype(light)>;
        if constexpr (std::is_same_v<T, DirectionalLightData>) {
            light.Direction = previous.Direction;
        } else if constexpr (std::is_same_v<T, RectLightData>) {
            light.Position = previous.Position;
            light.Direction = previous.Direction;
        } else {
            light.Point.Position = previous.Point.Position;
            light.Point.Direction = previous.Point.Direction;
        }
    };
    const auto update = [&](auto& table, const auto& incoming) {
        if (!batch.LightsChanged) {
            for (auto& light : table.Data) {
                if (!light.Common.Transform.IsValid()) continue;
                const auto row = _transforms.GetRow(light.Common.Transform);
                if (_transforms.WasUpdated(row)) setPose(light, _transforms.GetWorld(row));
            }
            return;
        }
        if (incoming.Ids.size() != incoming.Data.size()) RADRAY_ABORT("Light table columns must match");
        table.Data.resize(incoming.Data.size());
        for (size_t i = 0; i < incoming.Data.size(); ++i) {
            if (!incoming.Ids[i].IsValid()) RADRAY_ABORT("Invalid light identity");
            auto value = incoming.Data[i];
            if (value.Common.Transform.IsValid()) {
                const auto row = _transforms.GetRow(value.Common.Transform);
                if (i < table.Ids.size() && table.Ids[i] == incoming.Ids[i] &&
                    table.Data[i].Common.Transform == value.Common.Transform && !_transforms.WasUpdated(row))
                    preservePose(value, table.Data[i]);
                else
                    setPose(value, _transforms.GetWorld(row));
            }
            table.Data[i] = value;
        }
        table.Ids.assign(incoming.Ids.begin(), incoming.Ids.end());
    };
    update(_lights.DirectionalLights, batch.Lights.DirectionalLights);
    update(_lights.PointLights, batch.Lights.PointLights);
    update(_lights.SpotLights, batch.Lights.SpotLights);
    update(_lights.RectLights, batch.Lights.RectLights);
}
}  // namespace radray
