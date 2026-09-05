#include <gtest/gtest.h>

#include <random>
#include <radray/runtime/render_framework/culling.h>

namespace radray {
namespace {

ResolvedRenderView TestView(const Eigen::Matrix4f& projection) {
    ResolvedRenderView view;
    view.View = Eigen::Matrix4f::Identity();
    view.Projection = view.ViewProjection = projection;
    view.WorldPosition.setZero();
    return view;
}

TEST(RenderBounds, RotationNonUniformAndNegativeScaleMatchesEightCorners) {
    const AxisAlignedBounds local{{-2, -1, 1}, {3, 4, 5}};
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform.block<3, 3>(0, 0) = Eigen::AngleAxisf{.7f, Eigen::Vector3f{1, 2, 3}.normalized()}.toRotationMatrix() * Eigen::Vector3f{-2, 3, .5f}.asDiagonal();
    transform.block<3, 1>(0, 3) = Eigen::Vector3f{7, -4, 2};
    AxisAlignedBounds reference;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        Eigen::Vector4f position{0, 0, 0, 1};
        for (uint32_t axis = 0; axis < 3; ++axis) position[axis] = (corner & (1u << axis)) ? local.Max[axis] : local.Min[axis];
        const Eigen::Vector3f world = (transform * position).head<3>();
        reference.Min = reference.Min.cwiseMin(world);
        reference.Max = reference.Max.cwiseMax(world);
    }
    const auto actual = TransformBounds(local, transform);
    ASSERT_TRUE(actual.IsFiniteValid());
    EXPECT_TRUE(actual.Min.isApprox(reference.Min, 1e-5f));
    EXPECT_TRUE(actual.Max.isApprox(reference.Max, 1e-5f));
    transform(3, 0) = .1f;
    EXPECT_FALSE(TransformBounds(local, transform).IsFiniteValid());
    transform(3, 0) = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(TransformBounds(local, transform).IsFiniteValid());
}

TEST(Culling, PerspectiveFrustumInsideOutsideIntersecting) {
    auto view = TestView(PerspectiveLH(Radian(90.0f), 1.0f, 1.0f, 10.0f));
    RenderSceneSnapshot scene;
    for (const AxisAlignedBounds bounds : {AxisAlignedBounds{{-.1f, -.1f, 2}, {.1f, .1f, 3}}, {{20, 0, 2}, {21, 1, 3}}, {{-.1f, -.1f, .5f}, {.1f, .1f, 1}}}) {
        scene.Primitives.push_back({.WorldBounds = bounds});
    }
    CullingResults result;
    ASSERT_TRUE(Cull({&scene, &view}, result));
    ASSERT_EQ(result.Primitives.size(), 2u);
    EXPECT_EQ(result.Primitives[0].Primitive, 0u);
    EXPECT_EQ(result.Primitives[1].Primitive, 2u);
    EXPECT_FLOAT_EQ(result.Primitives[0].ViewDepth, 2.5f);
    EXPECT_EQ(result.Stats.FrustumRejected, 1u);
    EXPECT_EQ(result.Stats.InputPrimitives, result.Stats.VisiblePrimitives + result.Stats.FrustumRejected + result.Stats.LayerRejected);
}

TEST(Culling, OrthographicAndZeroToOneNearPlane) {
    const auto frustum = ExtractViewFrustum(OrthoLH(-2.0f, 2.0f, -1.0f, 1.0f, 1.0f, 5.0f));
    ASSERT_TRUE(frustum);
    EXPECT_TRUE(IntersectsFrustum(*frustum, AxisAlignedBounds{{-2, -1, 1}, {2, 1, 5}}));
    for (const Eigen::Vector3f point : {Eigen::Vector3f{-3, 0, 2}, {3, 0, 2}, {0, -2, 2}, {0, 2, 2}, {0, 0, .9f}, {0, 0, 5.1f}})
        EXPECT_FALSE(IntersectsFrustum(*frustum, AxisAlignedBounds{point, point}));
    EXPECT_TRUE(IntersectsFrustum(*frustum, AxisAlignedBounds{{0, 0, 1}, {0, 0, 1}}));
}

TEST(Culling, LayerMaskDisableCullingAndInvalidBounds) {
    auto view = TestView(Eigen::Matrix4f::Identity());
    view.LayerMask = 3;
    RenderSceneSnapshot scene;
    scene.Primitives.push_back({.WorldBounds = {{20, 0, 0}, {21, 1, 1}}, .LayerMask = 1, .DisableFrustumCulling = true});
    scene.Primitives.push_back({.LayerMask = 1});
    scene.Primitives.push_back({.LayerMask = 2});
    scene.Primitives.push_back({.LayerMask = 4});
    scene.Primitives[1].LocalToWorld.setConstant(std::numeric_limits<float>::quiet_NaN());
    CullingResults result;
    ASSERT_TRUE(Cull({&scene, &view, 1}, result));
    EXPECT_EQ(result.Primitives.size(), 2u);
    EXPECT_EQ(result.Stats.LayerRejected, 2u);
    EXPECT_EQ(result.Stats.InvalidBoundsVisible, 1u);
    EXPECT_EQ(result.Stats.InvalidDepth, 1u);
    EXPECT_FLOAT_EQ(result.Primitives[1].ViewDepth, 0);
}

TEST(Culling, PointLightSphereAndDirectionalLayer) {
    auto view = TestView(Eigen::Matrix4f::Identity());
    view.LayerMask = 1;
    RenderSceneSnapshot scene;
    for (const auto sphere : {SphereBounds{{0, 0, .5f}, .1f}, {{3, 0, .5f}, .1f}, {{1.5f, 0, .5f}, .5f}, {{0, 0, 0}, -1}}) {
        RenderLightData light;
        light.Type = LightType::Point;
        light.WorldBounds = sphere;
        light.Parameters.WorldPosition = sphere.Center;
        scene.Lights.push_back(light);
    }
    scene.Lights.push_back({.Type = LightType::Directional, .LayerMask = 1});
    scene.Lights.push_back({.Type = LightType::Directional, .LayerMask = 2});
    CullingResults result;
    ASSERT_TRUE(Cull({&scene, &view}, result));
    EXPECT_EQ(result.Lights.size(), 3u);
    EXPECT_FLOAT_EQ(result.Lights[0].DistanceSquared, .25f);
    EXPECT_EQ(result.Stats.InvalidLightBounds, 1u);
    EXPECT_EQ(result.Stats.LightFrustumRejected, 1u);
    EXPECT_EQ(result.Stats.LightLayerRejected, 1u);
}

TEST(Culling, InvalidViewClearsResultsAndInfiniteFarPlaneIsInactive) {
    auto view = TestView(Eigen::Matrix4f::Identity());
    RenderSceneSnapshot scene;
    scene.Primitives.emplace_back();
    CullingResults result;
    ASSERT_TRUE(Cull({&scene, &view}, result));
    const auto capacity = result.Primitives.capacity();
    view.ViewProjection.setZero();
    EXPECT_FALSE(Cull({&scene, &view}, result));
    EXPECT_FALSE(result.Stats.Valid);
    EXPECT_FALSE(result.Scene);
    EXPECT_TRUE(result.Primitives.empty());
    EXPECT_EQ(result.Primitives.capacity(), capacity);
    auto infinite = PerspectiveLH(Radian(60.0f), 1.0f, 1.0f, 100.0f);
    infinite(2, 2) = 1;
    infinite(2, 3) = -1;
    const auto frustum = ExtractViewFrustum(infinite);
    ASSERT_TRUE(frustum);
    EXPECT_EQ(frustum->ActivePlaneMask, 0x1fu);
    EXPECT_TRUE(IntersectsFrustum(*frustum, AxisAlignedBounds{{0, 0, 1e7f}, {1, 1, 1e7f}}));
}

TEST(Culling, TwoViewsProduceIndependentResults) {
    auto first = TestView(Eigen::Matrix4f::Identity());
    auto second = first;
    second.View(0, 3) = -10;
    second.ViewProjection = second.View;
    RenderSceneSnapshot scene;
    scene.Primitives.push_back({.WorldBounds = {{0, 0, 0}, {.5f, .5f, 1}}});
    scene.Primitives.push_back({.WorldBounds = {{10, 0, 0}, {10.5f, .5f, 1}}});
    CullingResults a, b;
    ASSERT_TRUE(Cull({&scene, &first}, a));
    ASSERT_TRUE(Cull({&scene, &second}, b));
    ASSERT_EQ(a.Primitives.size(), 1u);
    ASSERT_EQ(b.Primitives.size(), 1u);
    EXPECT_EQ(a.Primitives[0].Primitive, 0u);
    EXPECT_EQ(b.Primitives[0].Primitive, 1u);
}

TEST(Culling, TenThousandRandomBoundsMatchClipCornerReference) {
    const auto projection = PerspectiveLH(Radian(70.0f), 1.7f, .3f, 80.0f);
    auto view = TestView(projection);
    std::mt19937 random{317};
    std::uniform_real_distribution<float> position{-100, 100}, extent{.01f, 6};
    RenderSceneSnapshot scene;
    vector<uint32_t> reference;
    for (uint32_t index = 0; index < 10000; ++index) {
        const Eigen::Vector3f center{position(random), position(random), position(random)};
        const Eigen::Vector3f radius{extent(random), extent(random), extent(random)};
        const AxisAlignedBounds bounds{center - radius, center + radius};
        scene.Primitives.push_back({.WorldBounds = bounds});
        array<bool, 6> allOutside{true, true, true, true, true, true};
        for (uint32_t corner = 0; corner < 8; ++corner) {
            Eigen::Vector4d point{0, 0, 0, 1};
            for (uint32_t axis = 0; axis < 3; ++axis) point[axis] = (corner & (1u << axis)) ? bounds.Max[axis] : bounds.Min[axis];
            const Eigen::Vector4d clip = projection.cast<double>() * point;
            const array<double, 6> distances{clip.w() + clip.x(), clip.w() - clip.x(), clip.w() + clip.y(), clip.w() - clip.y(), clip.z(), clip.w() - clip.z()};
            for (uint32_t plane = 0; plane < 6; ++plane) allOutside[plane] = allOutside[plane] && distances[plane] < 0;
        }
        if (std::none_of(allOutside.begin(), allOutside.end(), [](bool value) { return value; })) reference.push_back(index);
    }
    CullingResults result;
    ASSERT_TRUE(Cull({&scene, &view}, result));
    for (uint32_t primitive : reference) EXPECT_TRUE(std::any_of(result.Primitives.begin(), result.Primitives.end(), [&](const auto& value) { return value.Primitive == primitive; }));
    EXPECT_EQ(result.Stats.InputPrimitives, result.Stats.FrustumRejected + result.Stats.VisiblePrimitives);
}

}  // namespace
}  // namespace radray
