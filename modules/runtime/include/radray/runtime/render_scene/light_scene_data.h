#pragma once

#include <variant>
#include <type_traits>

#include <Eigen/Core>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/runtime/render_scene/scene_id.h>

namespace radray {

enum class LightType : uint8_t { Directional,
                                 Point,
                                 Spot,
                                 Rect };

/// Type-independent parameters only; identity lives in the table's id column and shadow bias in the
/// types that capture it.
struct LightCommonData {
    Eigen::Vector3f Color{Eigen::Vector3f::Ones()};
    float Intensity{1};
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
    float ShadowDepthBias{0}, ShadowNormalBias{0};
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

/// Transient single-light capture, paired with its identity by the caller. Persistent storage uses
/// the typed tables below.
using LightData = std::variant<DirectionalLightData, PointLightData, SpotLightData, RectLightData>;

/// One light type's dense table: Ids[row] owns Data[row]. Removal swaps in the last entry, so a row
/// is not an identity. Both columns always have the same length.
template <class T>
struct LightTable {
    vector<LightId> Ids;
    vector<T> Data;

    size_t Size() const noexcept { return Data.size(); }
    bool Empty() const noexcept { return Data.empty(); }
};

/// Owned world-space CPU values. Mutations may invalidate rows and borrows.
struct LightSceneData {
    LightTable<DirectionalLightData> DirectionalLights;
    LightTable<PointLightData> PointLights;
    LightTable<SpotLightData> SpotLights;
    LightTable<RectLightData> RectLights;

    template <class T>
    LightTable<T>& GetTable() noexcept {
        if constexpr (std::is_same_v<T, DirectionalLightData>)
            return DirectionalLights;
        else if constexpr (std::is_same_v<T, PointLightData>)
            return PointLights;
        else if constexpr (std::is_same_v<T, SpotLightData>)
            return SpotLights;
        else {
            static_assert(std::is_same_v<T, RectLightData>);
            return RectLights;
        }
    }

    void Clear() noexcept;
    size_t Count() const noexcept { return DirectionalLights.Size() + PointLights.Size() + SpotLights.Size() + RectLights.Size(); }
    bool Empty() const noexcept { return Count() == 0; }
    /// Replaces all lights from a complete snapshot with unique, valid identities; retains capacity.
    void Assign(const LightSceneData& lights) noexcept;
    /// Inserts or replaces one light, including changing its type while preserving its identity.
    void Set(LightId id, const LightData& light) noexcept;
    bool Remove(LightId id) noexcept;
    Nullable<const LightCommonData*> GetLight(LightId id) const noexcept;
    Nullable<const DirectionalLightData*> GetDirectionalLight(LightId id) const noexcept;
    Nullable<const PointLightData*> GetPointLight(LightId id) const noexcept;
    Nullable<const SpotLightData*> GetSpotLight(LightId id) const noexcept;
    Nullable<const RectLightData*> GetRectLight(LightId id) const noexcept;
};

}  // namespace radray
