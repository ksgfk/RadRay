#pragma once

#include <variant>

#include <Eigen/Core>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/runtime/render_scene/scene_id.h>

namespace radray {

enum class LightType : uint8_t { Directional,
                                 Point,
                                 Spot,
                                 Rect };

struct LightCommonData {
    LightId Id;
    Eigen::Vector3f Color{Eigen::Vector3f::Ones()};
    float Intensity{1};
    float ShadowDepthBias{0}, ShadowNormalBias{0};
    bool AffectsWorld{true}, CastShadow{true};
};

struct DirectionalLightData {
    LightCommonData Common;
    Eigen::Vector3f Direction{Eigen::Vector3f::UnitZ()};
};

struct PointLightParameters {
    Eigen::Vector3f Position{Eigen::Vector3f::Zero()};
    /// World-space source axis; also used by a point light with nonzero SourceLength.
    Eigen::Vector3f Direction{Eigen::Vector3f::UnitZ()};
    float AttenuationRadius{0};
    float FalloffExponent{0};
    float SourceRadius{0}, SoftSourceRadius{0}, SourceLength{0};
    bool InverseSquaredFalloff{true};
};

struct PointLightData {
    LightCommonData Common;
    PointLightParameters Point;
};

struct SpotLightData {
    LightCommonData Common;
    PointLightParameters Point;
    float InnerConeAngle{0}, OuterConeAngle{0};
};

/// Reserved local-light data; area geometry and a RectLightComponent are not yet implemented.
struct RectLightData {
    LightCommonData Common;
    Eigen::Vector3f Position{Eigen::Vector3f::Zero()};
    Eigen::Vector3f Direction{Eigen::Vector3f::UnitZ()};
    float AttenuationRadius{0};
    float FalloffExponent{0};
    bool InverseSquaredFalloff{true};
};

/// Transient single-light capture. Persistent storage uses the typed arrays below.
using LightData = std::variant<DirectionalLightData, PointLightData, SpotLightData, RectLightData>;

/// Owned world-space CPU values. Mutations may invalidate array positions and borrows.
struct LightSceneData {
    vector<DirectionalLightData> DirectionalLights;
    vector<PointLightData> PointLights;
    vector<SpotLightData> SpotLights;
    vector<RectLightData> RectLights;

    void Clear() noexcept;
    size_t Count() const noexcept { return DirectionalLights.size() + PointLights.size() + SpotLights.size() + RectLights.size(); }
    bool Empty() const noexcept { return Count() == 0; }
    /// Replaces all lights from a complete snapshot with unique, valid identities; retains capacity.
    void Assign(const LightSceneData& lights) noexcept;
    /// Inserts or replaces one light, including changing its type while preserving its identity.
    void Set(const LightData& light) noexcept;
    bool Remove(LightId id) noexcept;
    Nullable<const LightCommonData*> GetLight(LightId id) const noexcept;
    Nullable<const DirectionalLightData*> GetDirectionalLight(LightId id) const noexcept;
    Nullable<const PointLightData*> GetPointLight(LightId id) const noexcept;
    Nullable<const SpotLightData*> GetSpotLight(LightId id) const noexcept;
    Nullable<const RectLightData*> GetRectLight(LightId id) const noexcept;
};

}  // namespace radray
