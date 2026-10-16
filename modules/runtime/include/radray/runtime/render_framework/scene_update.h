#pragma once

#include <radray/basic_math.h>
#include <radray/runtime/render_framework/scene_id.h>
#include <radray/runtime/render_framework/scene_transform.h>
#include <radray/runtime/render_framework/light_scene_data.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/render_framework/material.h>
#include <radray/runtime/render_framework/instance_parameters.h>
#include <radray/runtime/render_framework/render_asset_lifetime.h>

namespace radray {

/// Instance binding; geometry is shared and borrowed from a GT-owned scene asset.
struct StaticMeshDescription {
    AssetId MeshAssetId;
    Nullable<const StaticMeshRenderData*> RenderData{nullptr};

    Nullable<const GpuMesh*> GetRenderMesh() const noexcept { return RenderData ? &RenderData->Mesh : nullptr; }
    std::span<const StaticMeshSection> GetSections() const noexcept { return RenderData ? std::span<const StaticMeshSection>{RenderData->Sections} : std::span<const StaticMeshSection>{}; }
};

struct StaticMeshStateUpdate {
    ShapeId Id;
    StaticMeshDescription Mesh{};
    AffineTransform LocalToWorld{};
    TransformId Transform{};
};

struct ShapeMaterialUpdate {
    ShapeId Id;
    SceneMaterialListId Materials;
};

struct ShapeTransformUpdate {
    ShapeId Id;
    AffineTransform LocalToWorld{};
};
struct ShapeDrawState {
    uint32_t VisibilityMask{UINT32_MAX}, SortGroup{1}, Order{0};
    /// Zero is the ordinary automatic domain. Explicit collections reserve their own nonzero domain.
    uint64_t BatchDomain{0};
    bool ExplicitInstancing{false};
    friend bool operator==(const ShapeDrawState&, const ShapeDrawState&) = default;
};
struct ShapeDrawUpdate {
    ShapeId Id;
    ShapeDrawState State;
};
struct ShapeDrawRevision {
    uint64_t Geometry{0}, Materials{0}, Transform{0}, Parameters{0}, Order{0};
};

static_assert(sizeof(ShapeTransformUpdate) == 56);

/// Sealed on GT together with asset retirements before the runner publishes the flight to RT.
struct SceneUpdateBatch {
    vector<AssetExitRequest> AssetExits;
    vector<TransformId> RemoveTransforms;
    vector<TransformCreate> CreateTransforms;
    vector<TransformParentUpdate> TransformParents;
    vector<LocalTransformUpdate> LocalTransforms;
    vector<ShapeId> RemoveShapes;
    vector<ShapeId> CreateShapes;
    vector<StaticMeshStateUpdate> MeshStates;
    vector<ShapeTransformUpdate> Transforms;
    vector<ShapeMaterialUpdate> Materials;
    vector<ShapeParameterUpdate> Parameters;
    vector<byte> ParameterBytes;
    vector<ShapeDrawUpdate> DrawStates;
    /// False preserves the RT light set; true replaces it, including with an empty list.
    bool LightsChanged{false};
    LightSceneData Lights;

    void Clear() noexcept;
    SceneMaterialListId RegisterMaterials(std::span<const MaterialBinding> values);
    std::optional<InstanceSchemaId> RegisterSchema(const InstanceParameterLayout& layout);
    void SetMaterials(ShapeId id, std::span<const MaterialBinding> values);
    bool SetInstanceParameters(ShapeId id, const InstanceParameterLayout& layout, std::span<const byte> values);
    std::optional<std::span<const MaterialBinding>> GetMaterials(SceneMaterialListId id) const noexcept;
    Nullable<const InstanceParameterLayout*> GetSchema(InstanceSchemaId id) const noexcept;
    std::span<const byte> GetParameterValues(const ShapeParameterUpdate& update) const noexcept;
    bool Empty() const noexcept { return AssetExits.empty() && RemoveTransforms.empty() && CreateTransforms.empty() && TransformParents.empty() && LocalTransforms.empty() && RemoveShapes.empty() && CreateShapes.empty() && MeshStates.empty() && Transforms.empty() && Materials.empty() && Parameters.empty() && DrawStates.empty() && !LightsChanged && Lights.Empty() && _materialLists.empty() && _schemas.empty(); }

private:
    Guid _identity{Guid::NewGuid()};
    vector<vector<MaterialBinding>> _materialLists;
    vector<InstanceParameterLayout> _schemas;
};

struct SceneFrameUpdate {
    SceneId Id;
    bool Create{false};
    bool Destroy{false};
    SceneUpdateBatch Updates;
};

}  // namespace radray
