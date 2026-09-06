#include <gtest/gtest.h>
#include <cmath>
#include <radray/runtime/components/spot_light_component.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/culling.h>

namespace radray {
namespace {

TEST(SpotLight, S05ConeShadowSnapshotAndConservativeCulling) {
    SpotLightComponent component;
    ASSERT_TRUE(component.SetConeAngles(.2f, .6f));
    component.SetAttenuationRadius(3);
    component.SetCastShadow(false);
    EXPECT_FALSE(component.SetConeAngles(.7f, .3f));
    EXPECT_FALSE(component.SetConeAngles(0, 2));
    EXPECT_FALSE(component.SetConeAngles(0, std::numeric_limits<float>::quiet_NaN()));
    component.SetAttenuationRadius(std::numeric_limits<float>::infinity());
    EXPECT_FLOAT_EQ(component.GetAttenuationRadius(), 3);
    Scene scene;
    auto* proxy = scene.AddLight(&component);
    ASSERT_NE(proxy, nullptr);
    RenderSceneSnapshot snapshot;
    vector<StreamingAssetRefAny> retained;
    ASSERT_TRUE(BuildRenderSceneSnapshot(scene, snapshot, retained));
    ASSERT_EQ(snapshot.Lights.size(), 1u);
    const auto& light = snapshot.Lights[0];
    EXPECT_EQ(light.Type, LightType::Spot);
    EXPECT_FALSE(light.CastShadow);
    EXPECT_NEAR(light.Parameters.SpotAngles.x(), std::cos(.6f), 1e-6f);
    EXPECT_NEAR(light.Parameters.SpotAngles.y(), 1 / (std::cos(.2f) - std::cos(.6f)), 1e-5f);
    EXPECT_NEAR(light.Parameters.Direction.norm(), 1, 1e-6f);
    EXPECT_FLOAT_EQ(light.WorldBounds.Radius, 3);
    ResolvedRenderView view;
    view.View = view.ViewProjection = Eigen::Matrix4f::Identity();
    view.WorldPosition.setZero();
    CullingResults culling;
    ASSERT_TRUE(Cull({&snapshot, &view}, culling));
    EXPECT_EQ(culling.Lights.size(), 1u);
    EXPECT_EQ(culling.Stats.UnsupportedLights, 0u);
    snapshot.Lights[0].WorldBounds.Center.x() = 100;
    ASSERT_TRUE(Cull({&snapshot, &view}, culling));
    EXPECT_TRUE(culling.Lights.empty());
    snapshot.Lights[0].WorldBounds.Radius = -1;
    ASSERT_TRUE(Cull({&snapshot, &view}, culling));
    EXPECT_EQ(culling.Stats.InvalidLightBounds, 1u);
    for (uint32_t i = 0; i < 16; ++i) {
        const float angle = float(i) * 6.2831853f / 16;
        const Eigen::Vector3f boundary{3 * std::sin(.6f) * std::cos(angle), 3 * std::sin(.6f) * std::sin(angle), 3 * std::cos(.6f)};
        EXPECT_LE(boundary.norm(), 3.000001f);
    }
}

TEST(SpotLight, S06InvalidLocalParametersAndLayerFilteringAreDeterministic) {
    SpotLightComponent component;
    component.SetAttenuationRadius(3);
    for (const float radius : {-1.f, std::numeric_limits<float>::quiet_NaN()}) {
        component.SetAttenuationRadius(radius);
        EXPECT_FLOAT_EQ(component.GetAttenuationRadius(), 3);
    }
    component.SetAttenuationRadius(0);
    EXPECT_FLOAT_EQ(component.GetAttenuationRadius(), 0);
    RenderLightData valid;
    valid.Type = LightType::Spot;
    valid.LayerMask = 1;
    valid.WorldBounds = {{0, 0, .5f}, 3};
    valid.Parameters.WorldPosition = valid.WorldBounds.Center;
    valid.Parameters.SpotAngles = {std::cos(.6f), 1 / (std::cos(.2f) - std::cos(.6f))};
    ResolvedRenderView view;
    view.View = view.ViewProjection = Eigen::Matrix4f::Identity();
    view.WorldPosition.setZero();
    view.LayerMask = 1;
    for (uint32_t scenario = 0; scenario < 8; ++scenario) {
        RenderSceneSnapshot scene;
        scene.Lights.push_back(valid);
        auto& light = scene.Lights.back();
        if (scenario < 3) light.WorldBounds.Radius = scenario == 0 ? -1 : scenario == 1 ? 0
                                                                                        : std::numeric_limits<float>::quiet_NaN();
        if (scenario == 3) light.Parameters.Direction.x() = std::numeric_limits<float>::infinity();
        if (scenario == 4) light.Parameters.Direction.setZero();
        if (scenario == 5) light.Parameters.SpotAngles = {-1, 0};
        if (scenario == 6) light.Parameters.Color.x() = std::numeric_limits<float>::quiet_NaN();
        if (scenario == 7) light.LayerMask = 2;
        scene.Lights.push_back({.Type = LightType::Directional, .LayerMask = 1});
        for (uint32_t repeat = 0; repeat < 3; ++repeat) {
            CullingResults result;
            ASSERT_TRUE(Cull({&scene, &view}, result));
            ASSERT_EQ(result.Lights.size(), 1u);
            EXPECT_EQ(result.Lights[0].Light, 1u);
            EXPECT_EQ(result.Stats.InvalidLightBounds, scenario < 3 ? 1u : 0u);
            EXPECT_EQ(result.Stats.InvalidLightParameters, scenario >= 3 && scenario < 7 ? 1u : 0u);
            EXPECT_EQ(result.Stats.LightLayerRejected, scenario == 7 ? 1u : 0u);
        }
    }
}

}  // namespace
}  // namespace radray
