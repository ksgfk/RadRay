#include <gtest/gtest.h>

#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/render_framework/point_light_scene_proxy.h>
#include <radray/runtime/render_framework/directional_light_scene_proxy.h>
#include <radray/runtime/render_framework/scene.h>

using namespace radray;

TEST(LightComponentTest, PointLightCreatesTypedSceneProxy) {
    PointLightComponent component;
    component.SetWorldLocation(Eigen::Vector3f{1.0f, 2.0f, 3.0f});
    component.SetLightColor(Eigen::Vector3f{0.25f, 0.5f, 1.0f});
    component.SetIntensity(4.0f);
    component.SetAttenuationRadius(250.0f);
    component.SetLightFalloffExponent(6.0f);
    component.SetUseInverseSquaredFalloff(false);
    component.SetSourceRadius(2.0f);
    component.SetSoftSourceRadius(3.0f);
    component.SetSourceLength(4.0f);

    unique_ptr<LightSceneProxy> proxy = component.CreateSceneProxy();
    ASSERT_NE(proxy, nullptr);
    EXPECT_EQ(proxy->GetLightType(), LightType::Point);

    ASSERT_EQ(proxy->GetLightType(), LightType::Point);
    const auto* pointProxy = static_cast<const PointLightSceneProxy*>(proxy.get());
    EXPECT_TRUE(pointProxy->IsLocalLight());
    EXPECT_FALSE(pointProxy->IsInverseSquared());
    EXPECT_FLOAT_EQ(pointProxy->GetRadius(), 250.0f);
    EXPECT_FLOAT_EQ(pointProxy->GetInvRadius(), 1.0f / 250.0f);
    EXPECT_FLOAT_EQ(pointProxy->GetFalloffExponent(), 6.0f);
    EXPECT_FLOAT_EQ(pointProxy->GetSourceRadius(), 2.0f);
    EXPECT_FLOAT_EQ(pointProxy->GetSoftSourceRadius(), 3.0f);
    EXPECT_FLOAT_EQ(pointProxy->GetSourceLength(), 4.0f);
    EXPECT_TRUE(proxy->GetOrigin().isApprox(Eigen::Vector3f{1.0f, 2.0f, 3.0f}));
    EXPECT_TRUE(proxy->GetColor().isApprox(Eigen::Vector3f{1.0f, 2.0f, 4.0f}));

    LightRenderParameters params{};
    proxy->GetLightRenderParameters(params);
    EXPECT_TRUE(params.WorldPosition.isApprox(Eigen::Vector3f{1.0f, 2.0f, 3.0f}));
    EXPECT_TRUE(params.Color.isApprox(Eigen::Vector3f{1.0f, 2.0f, 4.0f}));
    EXPECT_FLOAT_EQ(params.InvRadius, 1.0f / 250.0f);
    EXPECT_FLOAT_EQ(params.FalloffExponent, 6.0f);
    EXPECT_FLOAT_EQ(params.SourceRadius, 2.0f);
    EXPECT_FLOAT_EQ(params.SoftSourceRadius, 3.0f);
    EXPECT_FLOAT_EQ(params.SourceLength, 4.0f);
}

TEST(LightComponentTest, SceneOwnsLightSceneProxy) {
    Scene scene;
    PointLightComponent component;

    LightSceneProxy* proxy = scene.AddLight(&component);
    ASSERT_NE(proxy, nullptr);
    EXPECT_EQ(component.GetSceneProxy(), nullptr);
    ASSERT_EQ(scene.Lights().size(), 1u);
    EXPECT_EQ(scene.Lights()[0].get(), proxy);
    EXPECT_EQ(proxy->GetLightType(), LightType::Point);

    scene.RemoveLight(proxy);
    EXPECT_TRUE(scene.Lights().empty());
}

TEST(LightComponentTest, DirectionalLightCreatesTypedSceneProxy) {
    DirectionalLightComponent component;
    component.SetLightColor(Eigen::Vector3f{1.0f, 0.5f, 0.25f});
    component.SetIntensity(3.0f);
    component.SetCascadeCount(3);
    component.SetShadowDistance(120.0f);
    component.SetCascadeSplitLambda(0.6f);
    component.SetShadowMapResolution(1024);
    component.SetShadowDepthBias(0.005f);
    component.SetShadowNormalBias(2.0f);
    component.SetShadowSoftMode(1);
    // 让 +Z 指向 (0, -1, 0): 光照方向为竖直向下。
    component.SetWorldRotation(
        Eigen::Quaternionf::FromTwoVectors(Eigen::Vector3f::UnitZ(), Eigen::Vector3f{0.0f, -1.0f, 0.0f}));

    unique_ptr<LightSceneProxy> proxy = component.CreateSceneProxy();
    ASSERT_NE(proxy, nullptr);
    EXPECT_EQ(proxy->GetLightType(), LightType::Directional);
    EXPECT_FALSE(proxy->IsLocalLight());

    const auto* dirProxy = static_cast<const DirectionalLightSceneProxy*>(proxy.get());
    EXPECT_EQ(dirProxy->GetCascadeCount(), 3u);
    EXPECT_FLOAT_EQ(dirProxy->GetShadowDistance(), 120.0f);
    EXPECT_FLOAT_EQ(dirProxy->GetCascadeSplitLambda(), 0.6f);
    EXPECT_EQ(dirProxy->GetShadowMapResolution(), 1024u);
    EXPECT_FLOAT_EQ(dirProxy->GetShadowDepthBias(), 0.005f);
    EXPECT_FLOAT_EQ(dirProxy->GetShadowNormalBias(), 2.0f);
    EXPECT_EQ(dirProxy->GetShadowSoftMode(), 1u);
    // 光照方向 (归一化竖直向下)。
    EXPECT_TRUE(proxy->GetDirection().isApprox(Eigen::Vector3f{0.0f, -1.0f, 0.0f}));
    // 颜色 = lightColor * intensity。
    EXPECT_TRUE(proxy->GetColor().isApprox(Eigen::Vector3f{3.0f, 1.5f, 0.75f}));
}

TEST(LightComponentTest, DirectionalLightClampsCascadeConfig) {
    DirectionalLightComponent component;
    component.SetCascadeCount(99);            // 超出上限, clamp 到 4
    component.SetCascadeSplitLambda(5.0f);    // 超出 [0,1], clamp 到 1
    component.SetShadowSoftMode(7);           // 超出上限, clamp 到 2

    unique_ptr<LightSceneProxy> proxy = component.CreateSceneProxy();
    const auto* dirProxy = static_cast<const DirectionalLightSceneProxy*>(proxy.get());
    EXPECT_EQ(dirProxy->GetCascadeCount(), 4u);
    EXPECT_FLOAT_EQ(dirProxy->GetCascadeSplitLambda(), 1.0f);
    EXPECT_EQ(dirProxy->GetShadowSoftMode(), 2u);
}
