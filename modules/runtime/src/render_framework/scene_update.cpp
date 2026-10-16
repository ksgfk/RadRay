#include <radray/runtime/render_framework/scene_update.h>
#include <algorithm>

namespace radray {

void SceneUpdateBatch::Clear() noexcept {
    AssetExits.clear();
    RemoveTransforms.clear();
    CreateTransforms.clear();
    TransformParents.clear();
    LocalTransforms.clear();
    RemoveShapes.clear();
    CreateShapes.clear();
    MeshStates.clear();
    Transforms.clear();
    Materials.clear();
    Parameters.clear();
    ParameterBytes.clear();
    _materialLists.clear();
    _schemas.clear();
    _identity = Guid::NewGuid();
    DrawStates.clear();
    LightsChanged = false;
    Lights.Clear();
}
SceneMaterialListId SceneUpdateBatch::RegisterMaterials(std::span<const MaterialBinding> values) {
    for (uint32_t i = 0; i < _materialLists.size(); ++i)
        if (std::ranges::equal(_materialLists[i], values)) return {_identity, i};
    const auto index = uint32_t(_materialLists.size());
    _materialLists.emplace_back(values.begin(), values.end());
    return {_identity, index};
}
std::optional<InstanceSchemaId> SceneUpdateBatch::RegisterSchema(const InstanceParameterLayout& layout) {
    if (!layout.IsValid()) return std::nullopt;
    for (uint32_t i = 0; i < _schemas.size(); ++i)
        if (_schemas[i].Name == layout.Name) return _schemas[i] == layout ? std::optional{InstanceSchemaId{_identity, i}} : std::nullopt;
    const auto index = uint32_t(_schemas.size());
    _schemas.push_back(layout);
    return InstanceSchemaId{_identity, index};
}
void SceneUpdateBatch::SetMaterials(ShapeId id, std::span<const MaterialBinding> values) {
    Materials.push_back({id, RegisterMaterials(values)});
}
bool SceneUpdateBatch::SetInstanceParameters(ShapeId id, const InstanceParameterLayout& layout, std::span<const byte> values) {
    if (values.size() != layout.Stride || values.size() > UINT32_MAX - ParameterBytes.size()) return false;
    const auto schema = RegisterSchema(layout);
    if (!schema) return false;
    Parameters.push_back({id, *schema, uint32_t(ParameterBytes.size()), uint32_t(values.size())});
    ParameterBytes.insert(ParameterBytes.end(), values.begin(), values.end());
    return true;
}
std::optional<std::span<const MaterialBinding>> SceneUpdateBatch::GetMaterials(SceneMaterialListId id) const noexcept {
    return id.Packet == _identity && id.Index < _materialLists.size() ? std::optional{std::span<const MaterialBinding>{_materialLists[id.Index]}} : std::nullopt;
}
Nullable<const InstanceParameterLayout*> SceneUpdateBatch::GetSchema(InstanceSchemaId id) const noexcept {
    return id.Packet == _identity && id.Index < _schemas.size() ? &_schemas[id.Index] : nullptr;
}
std::span<const byte> SceneUpdateBatch::GetParameterValues(const ShapeParameterUpdate& update) const noexcept {
    if (update.Schema.Packet != _identity || update.Offset > ParameterBytes.size() || update.Size > ParameterBytes.size() - update.Offset) return {};
    return {ParameterBytes.data() + update.Offset, update.Size};
}

}  // namespace radray
