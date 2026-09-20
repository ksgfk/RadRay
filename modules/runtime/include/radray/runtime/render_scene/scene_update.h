#pragma once

#include <radray/runtime/render_scene/scene_id.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

enum class LightType : uint8_t { Directional,
                                 Point,
                                 Spot,
                                 Rect };

struct LightStateUpdate {
    PrimitiveId Id;
    LightType Type{LightType::Point};
    Eigen::Vector3f Color{Eigen::Vector3f::Ones()};
    Eigen::Vector4f Position{0, 0, 0, 1};
    Eigen::Vector3f Direction{Eigen::Vector3f::UnitZ()};
    float Intensity{1};
    float AttenuationRadius{0};
    float FalloffExponent{0};
    float SourceRadius{0}, SoftSourceRadius{0}, SourceLength{0};
    float ShadowDepthBias{0}, ShadowNormalBias{0};
    float InnerConeAngle{0}, OuterConeAngle{0};
    bool AffectsWorld{true}, CastShadow{true}, InverseSquaredFalloff{true};
};

/// Immutable metadata and GPU views borrowed from GT-owned scene assets.
struct StaticMeshDescription {
    AssetId MeshAssetId;
    Nullable<const GpuMesh*> RenderMesh{nullptr};
    std::span<const StaticMeshSection> Sections;
    Eigen::Vector3f LocalBoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f LocalBoundsMax{Eigen::Vector3f::Zero()};
};

struct StaticMeshStateUpdate {
    PrimitiveId Id;
    StaticMeshDescription Mesh{};
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
};

struct PrimitiveTransformUpdate {
    PrimitiveId Id;
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
};

/// Sealed on GT together with asset retirements before the runner publishes the flight to RT.
struct SceneUpdateBatch {
    vector<PrimitiveId> RemovePrimitives;
    vector<PrimitiveId> CreatePrimitives;
    vector<StaticMeshStateUpdate> MeshStates;
    vector<PrimitiveTransformUpdate> Transforms;
    vector<LightStateUpdate> Lights;

    void Clear() noexcept;
    bool Empty() const noexcept { return RemovePrimitives.empty() && CreatePrimitives.empty() && MeshStates.empty() && Transforms.empty() && Lights.empty(); }
};

struct SceneFrameUpdate {
    SceneId Id;
    bool Create{false};
    bool Destroy{false};
    SceneUpdateBatch Updates;
};

}  // namespace radray
