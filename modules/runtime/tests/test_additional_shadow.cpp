#include <gtest/gtest.h>

#include <cmath>

#include <radray/runtime/renderer/scene_renderer.h>
#include <radray/runtime/renderer/light_scene_proxy.h>
#include <radray/runtime/renderer/scene.h>

using namespace radray;

namespace {

// Project a world point through a slice's ViewProj and return NDC (post perspective divide).
Eigen::Vector3f ProjectToNdc(const SceneView& view, const Eigen::Vector3f& world) {
    const Eigen::Vector4f clip = view.ViewProjMatrix * Eigen::Vector4f{world.x(), world.y(), world.z(), 1.0f};
    return clip.head<3>() / clip.w();
}

unique_ptr<LightSceneProxy> MakeSpot(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& direction,
    float range,
    float innerAngle,
    float outerAngle,
    bool castShadow = true) {
    auto light = make_unique<LightSceneProxy>();
    light->SetLightType(LightType::Spot);
    light->SetPosition(position);
    light->SetDirection(direction);
    light->SetRange(range);
    light->SetSpotInnerAngle(innerAngle);
    light->SetSpotOuterAngle(outerAngle);
    light->SetCastShadow(castShadow);
    return light;
}

unique_ptr<LightSceneProxy> MakePoint(const Eigen::Vector3f& position, float range, bool castShadow = true) {
    auto light = make_unique<LightSceneProxy>();
    light->SetLightType(LightType::Point);
    light->SetPosition(position);
    light->SetRange(range);
    light->SetCastShadow(castShadow);
    return light;
}

}  // namespace

// A spot light claims exactly one atlas slice and reports itself in the light records.
TEST(AdditionalShadowTest, SpotClaimsSingleSlice) {
    Scene scene;
    scene.AddLight(MakeSpot({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f, Radian(20.0f), Radian(30.0f)));

    AdditionalShadowData data;
    const bool enabled = BuildAdditionalShadows(scene, 1024, ShadowSoftMode::Medium, data);

    EXPECT_TRUE(enabled);
    EXPECT_TRUE(data.Enabled);
    EXPECT_EQ(data.SliceCount, 1u);
    ASSERT_EQ(data.Lights.size(), 1u);
    EXPECT_EQ(data.Lights[0].Kind, AdditionalShadowKind::Spot);
    EXPECT_EQ(data.Lights[0].FirstSlice, 0u);
    EXPECT_EQ(data.Lights[0].SliceCount, 1u);
}

// A point light claims six consecutive atlas slices (one per cube face).
TEST(AdditionalShadowTest, PointClaimsSixSlices) {
    Scene scene;
    scene.AddLight(MakePoint({1.0f, 2.0f, 3.0f}, 15.0f));

    AdditionalShadowData data;
    BuildAdditionalShadows(scene, 1024, ShadowSoftMode::Medium, data);

    EXPECT_EQ(data.SliceCount, 6u);
    ASSERT_EQ(data.Lights.size(), 1u);
    EXPECT_EQ(data.Lights[0].Kind, AdditionalShadowKind::Point);
    EXPECT_EQ(data.Lights[0].FirstSlice, 0u);
    EXPECT_EQ(data.Lights[0].SliceCount, AdditionalShadowData::PointFaceCount);
}

// Mixed lights pack into contiguous, non-overlapping slice ranges in registration order.
TEST(AdditionalShadowTest, MixedLightsPackContiguously) {
    Scene scene;
    scene.AddLight(MakeSpot({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f, Radian(20.0f), Radian(30.0f)));
    scene.AddLight(MakePoint({1.0f, 2.0f, 3.0f}, 15.0f));
    scene.AddLight(MakeSpot({-3.0f, 4.0f, 1.0f}, {0.3f, -1.0f, 0.0f}, 25.0f, Radian(15.0f), Radian(25.0f)));

    AdditionalShadowData data;
    BuildAdditionalShadows(scene, 1024, ShadowSoftMode::Low, data);

    ASSERT_EQ(data.Lights.size(), 3u);
    EXPECT_EQ(data.Lights[0].FirstSlice, 0u);  // spot
    EXPECT_EQ(data.Lights[1].FirstSlice, 1u);  // point (1..6)
    EXPECT_EQ(data.Lights[2].FirstSlice, 7u);  // spot
    EXPECT_EQ(data.SliceCount, 8u);
}

// Non-shadow-casting and non-additional lights are excluded.
TEST(AdditionalShadowTest, SkipsNonCastersAndDirectional) {
    Scene scene;
    auto directional = make_unique<LightSceneProxy>();
    directional->SetLightType(LightType::Directional);
    directional->SetCastShadow(true);
    scene.AddLight(std::move(directional));
    scene.AddLight(MakeSpot({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f, Radian(20.0f), Radian(30.0f), /*castShadow*/ false));

    AdditionalShadowData data;
    const bool enabled = BuildAdditionalShadows(scene, 1024, ShadowSoftMode::Hard, data);

    EXPECT_FALSE(enabled);
    EXPECT_FALSE(data.Enabled);
    EXPECT_EQ(data.SliceCount, 0u);
    EXPECT_TRUE(data.Lights.empty());
}

// The atlas is capped at MaxSlices: a caster that does not fit is dropped whole.
TEST(AdditionalShadowTest, AtlasCapDropsOverflowingLight) {
    Scene scene;
    // 2 point lights = 12 slices; a third would need 18 > 16, so it is dropped.
    scene.AddLight(MakePoint({0.0f, 1.0f, 0.0f}, 10.0f));
    scene.AddLight(MakePoint({2.0f, 1.0f, 0.0f}, 10.0f));
    scene.AddLight(MakePoint({4.0f, 1.0f, 0.0f}, 10.0f));

    AdditionalShadowData data;
    BuildAdditionalShadows(scene, 1024, ShadowSoftMode::Medium, data);

    EXPECT_EQ(data.Lights.size(), 2u);
    EXPECT_EQ(data.SliceCount, 12u);
    EXPECT_LE(data.SliceCount, AdditionalShadowData::MaxSlices);
}

// A spot slice projects its own world position to the near plane and points down +Z (light space).
TEST(AdditionalShadowTest, SpotSliceProjectsAlongAxis) {
    const Eigen::Vector3f position{0.0f, 10.0f, 0.0f};
    const Eigen::Vector3f direction{0.0f, -1.0f, 0.0f};
    const float range = 20.0f;
    AdditionalShadowSlice slice = BuildSpotShadowSlice(
        position, direction, Radian(30.0f), range, 1024, 1.0f, 1.0f, ShadowSoftMode::Medium);

    // A point straight ahead, halfway to the range, lands near the center of the shadow map.
    const Eigen::Vector3f ahead = position + direction * (range * 0.5f);
    const Eigen::Vector3f ndc = ProjectToNdc(slice.View, ahead);
    EXPECT_NEAR(ndc.x(), 0.0f, 1e-3f);
    EXPECT_NEAR(ndc.y(), 0.0f, 1e-3f);
    EXPECT_GT(ndc.z(), 0.0f);
    EXPECT_LT(ndc.z(), 1.0f);

    // Spot uses -direction as the "toward light" bias direction.
    EXPECT_NEAR(slice.LightDirectionForBias.x(), 0.0f, 1e-5f);
    EXPECT_NEAR(slice.LightDirectionForBias.y(), 1.0f, 1e-5f);
    EXPECT_NEAR(slice.LightDirectionForBias.z(), 0.0f, 1e-5f);
    // Negative biases push the caster vertex toward the light, matching the directional path sign.
    EXPECT_LT(slice.DepthBias, 0.0f);
    EXPECT_LT(slice.NormalBias, 0.0f);
}

// Each of the six point faces projects a point along its forward axis to the center, and forces normalBias 0.
TEST(AdditionalShadowTest, PointFacesProjectAlongForward) {
    const Eigen::Vector3f position{0.0f, 0.0f, 0.0f};
    const float range = 12.0f;
    for (uint32_t face = 0; face < AdditionalShadowData::PointFaceCount; ++face) {
        AdditionalShadowSlice slice = BuildPointShadowFaceSlice(position, face, range, 1024, 1.0f, ShadowSoftMode::Medium);
        const Eigen::Vector3f forward = PointShadowFaceForward(face);
        const Eigen::Vector3f ahead = position + forward * (range * 0.5f);
        const Eigen::Vector3f ndc = ProjectToNdc(slice.View, ahead);
        EXPECT_NEAR(ndc.x(), 0.0f, 1e-3f) << "face " << face;
        EXPECT_NEAR(ndc.y(), 0.0f, 1e-3f) << "face " << face;
        EXPECT_GT(ndc.z(), 0.0f) << "face " << face;
        EXPECT_LT(ndc.z(), 1.0f) << "face " << face;
        EXPECT_EQ(slice.NormalBias, 0.0f) << "face " << face;
    }
}

// The six face forwards are the signed unit axes in +X,-X,+Y,-Y,+Z,-Z order.
TEST(AdditionalShadowTest, PointFaceForwardOrdering) {
    EXPECT_TRUE(PointShadowFaceForward(0).isApprox(Eigen::Vector3f{1.0f, 0.0f, 0.0f}));
    EXPECT_TRUE(PointShadowFaceForward(1).isApprox(Eigen::Vector3f{-1.0f, 0.0f, 0.0f}));
    EXPECT_TRUE(PointShadowFaceForward(2).isApprox(Eigen::Vector3f{0.0f, 1.0f, 0.0f}));
    EXPECT_TRUE(PointShadowFaceForward(3).isApprox(Eigen::Vector3f{0.0f, -1.0f, 0.0f}));
    EXPECT_TRUE(PointShadowFaceForward(4).isApprox(Eigen::Vector3f{0.0f, 0.0f, 1.0f}));
    EXPECT_TRUE(PointShadowFaceForward(5).isApprox(Eigen::Vector3f{0.0f, 0.0f, -1.0f}));
}

// Kernel radius grows with soft mode (Hard < Low < Medium), matching the directional path.
TEST(AdditionalShadowTest, KernelRadiusByMode) {
    EXPECT_FLOAT_EQ(AdditionalShadowKernelRadius(ShadowSoftMode::Hard), 1.0f);
    EXPECT_FLOAT_EQ(AdditionalShadowKernelRadius(ShadowSoftMode::Low), 1.5f);
    EXPECT_FLOAT_EQ(AdditionalShadowKernelRadius(ShadowSoftMode::Medium), 2.5f);
}
