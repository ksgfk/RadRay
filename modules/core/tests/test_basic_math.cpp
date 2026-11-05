#include <radray/basic_math.h>

#include <limits>
#include <random>

#include <gtest/gtest.h>

namespace radray {
namespace {

using Vec3 = Eigen::Vector3f;
using Point = array<float, 3>;

Eigen::Matrix4f ReferenceCompose(const Vec3& translation, const Eigen::Quaternionf& rotation, const Vec3& scale) {
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    m.block<3, 3>(0, 0) = rotation.toRotationMatrix() * Eigen::Scaling(scale).toDenseMatrix();
    m.block<3, 1>(0, 3) = translation;
    return m;
}

Eigen::Vector3f Project(const Eigen::Matrix4f& m, const Vec3& p) {
    const Eigen::Vector4f clip = m * Eigen::Vector4f{p.x(), p.y(), p.z(), 1.0f};
    return clip.head<3>() / clip.w();
}

TEST(BasicMathTest, AlignRoundsUpToPowerOfTwoMultiple) {
    EXPECT_EQ(Align(0, 16), 0u);
    EXPECT_EQ(Align(1, 16), 16u);
    EXPECT_EQ(Align(16, 16), 16u);
    EXPECT_EQ(Align(17, 16), 32u);
}

TEST(BasicMathTest, AffineTransformDefaultsToIdentity) {
    EXPECT_TRUE(AffineTransform{}.ToMatrix().isIdentity());
}

TEST(BasicMathTest, AffineTransformRoundTripsShearNegativeAndZeroScale) {
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix(0, 0) = 0;
    matrix(1, 1) = -2;
    matrix(0, 1) = 3;
    matrix(2, 0) = 0.5f;
    matrix.block<3, 1>(0, 3) = Vec3{7, -8, 9};

    const AffineTransform packed{matrix};

    EXPECT_EQ(packed.ToMatrix(), matrix);
    EXPECT_FLOAT_EQ(packed(0, 1), 3);
    EXPECT_FLOAT_EQ(packed(3, 3), 1);
}

TEST(BasicMathTest, ComposeTransformMatchesTranslationRotationScale) {
    std::mt19937 generator{20260921u};
    std::uniform_real_distribution<float> uniform{-4.0f, 4.0f};
    for (int iteration = 0; iteration < 64; ++iteration) {
        const Vec3 translation{uniform(generator), uniform(generator), uniform(generator)};
        const Eigen::Quaternionf rotation = Eigen::Quaternionf{uniform(generator), uniform(generator), uniform(generator), uniform(generator)}.normalized();
        const Vec3 scale{uniform(generator), uniform(generator), uniform(generator)};

        const Eigen::Matrix4f composed = ComposeTransform(translation, rotation, scale);

        EXPECT_TRUE(composed.isApprox(ReferenceCompose(translation, rotation, scale), 1e-5f)) << iteration;
    }
}

TEST(BasicMathTest, DecomposeTransformInvertsComposeForPositiveScale) {
    const Vec3 translation{1, -2, 3};
    const Eigen::Quaternionf rotation{Eigen::AngleAxisf{0.7f, Vec3{1, 2, 3}.normalized()}};
    const Vec3 scale{2, 0.5f, 3};

    Vec3 outTranslation;
    Eigen::Quaternionf outRotation;
    Vec3 outScale;
    DecomposeTransform(ComposeTransform(translation, rotation, scale), outTranslation, outRotation, outScale);

    EXPECT_TRUE(outTranslation.isApprox(translation));
    EXPECT_TRUE(outRotation.toRotationMatrix().isApprox(rotation.toRotationMatrix(), 1e-5f));
    EXPECT_TRUE(outScale.isApprox(scale, 1e-5f));
}

TEST(BasicMathTest, PerspectiveLHMapsNearToZeroAndFarToOne) {
    const Eigen::Matrix4f projection = PerspectiveLH(Radian(90.0f), 1.0f, 0.1f, 100.0f);

    EXPECT_NEAR(Project(projection, Vec3{0, 0, 0.1f}).z(), 0.0f, 1e-6f);
    EXPECT_NEAR(Project(projection, Vec3{0, 0, 100.0f}).z(), 1.0f, 1e-6f);
    EXPECT_NEAR(Project(projection, Vec3{10, 10, 10}).x(), 1.0f, 1e-6f);
}

TEST(BasicMathTest, OrthoLHMapsBoxToNdc) {
    const Eigen::Matrix4f projection = OrthoLH(-2.0f, 6.0f, -1.0f, 3.0f, 1.0f, 11.0f);

    EXPECT_TRUE(Project(projection, Vec3{-2, -1, 1}).isApprox(Vec3{-1, -1, 0}));
    EXPECT_TRUE(Project(projection, Vec3{6, 3, 11}).isApprox(Vec3{1, 1, 1}));
}

TEST(BasicMathTest, LookAtLHPutsTargetOnPositiveZAndRightOnPositiveX) {
    const Eigen::Matrix4f view = LookAtLH(Vec3{0, 0, -5}, Vec3::Zero().eval(), Vec3::UnitY().eval());

    EXPECT_TRUE(Project(view, Vec3::Zero()).isApprox(Vec3{0, 0, 5}));
    EXPECT_GT(Project(view, Vec3::UnitX()).x(), 0.0f);
    EXPECT_GT(Project(view, Vec3::UnitY()).y(), 0.0f);
}

TEST(BasicMathTest, AabbDefaultIsEmptyWithZeroMeasurements) {
    const Aabb bounds;

    EXPECT_TRUE(bounds.IsEmpty());
    EXPECT_TRUE(bounds.GetCenter().isZero());
    EXPECT_TRUE(bounds.GetSize().isZero());
    EXPECT_TRUE(bounds.GetExtent().isZero());
}

TEST(BasicMathTest, AabbUnorderedOrNonFiniteEndpointsAreEmpty) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();

    EXPECT_TRUE((Aabb{{2, 0, 0}, {1, 1, 1}}).IsEmpty());
    EXPECT_TRUE((Aabb{{nan, 0, 0}, {1, 1, 1}}).IsEmpty());
    EXPECT_TRUE((Aabb{{0, 0, 0}, {infinity, 1, 1}}).IsEmpty());
    EXPECT_FALSE((Aabb{{1, 1, 1}, {1, 1, 1}}).IsEmpty());
}

TEST(BasicMathTest, AabbExpandFromEmptyDoesNotIncludeOrigin) {
    Aabb bounds;

    bounds.Expand(Vec3{-5, -4, -3});
    bounds.Expand(Vec3{-8, -2, -6});

    EXPECT_EQ(bounds.Min, (Point{-8, -4, -6}));
    EXPECT_EQ(bounds.Max, (Point{-5, -2, -3}));
    EXPECT_TRUE(bounds.GetCenter().isApprox(Vec3{-6.5f, -3, -4.5f}));
    EXPECT_TRUE(bounds.GetSize().isApprox(Vec3{3, 2, 3}));
    EXPECT_TRUE(bounds.GetExtent().isApprox(Vec3{1.5f, 1, 1.5f}));
}

TEST(BasicMathTest, AabbExpandByBoxCoversBoth) {
    Aabb bounds{{0, 0, 0}, {1, 1, 1}};

    bounds.Expand(Aabb{{-2, 0.5f, 0}, {0.5f, 3, 0.5f}});

    EXPECT_EQ(bounds.Min, (Point{-2, 0, 0}));
    EXPECT_EQ(bounds.Max, (Point{1, 3, 1}));
}

TEST(BasicMathTest, AabbExpandIgnoresNonFinitePointsAndEmptyBoxes) {
    const Aabb original{{1, 2, 3}, {4, 5, 6}};
    Aabb bounds = original;

    bounds.Expand(Aabb{});
    bounds.Expand(Aabb{{2, 0, 0}, {1, 1, 1}});
    bounds.Expand(Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0});
    bounds.Expand(Vec3{0, std::numeric_limits<float>::infinity(), 0});

    EXPECT_EQ(bounds.Min, original.Min);
    EXPECT_EQ(bounds.Max, original.Max);
}

TEST(BasicMathTest, AabbContainsIncludesBoundary) {
    const Aabb bounds{{0, 0, 0}, {2, 2, 2}};

    EXPECT_TRUE(bounds.Contains(Vec3{0, 2, 1}));
    EXPECT_FALSE(bounds.Contains(Vec3{std::nextafter(2.0f, 3.0f), 1, 1}));
    EXPECT_TRUE(bounds.Contains(Aabb{{1, 1, 1}, {1, 1, 1}}));
    EXPECT_TRUE(bounds.Contains(bounds));
    EXPECT_FALSE(bounds.Contains(Aabb{{-1, 1, -1}, {1, 3, 1}}));
}

TEST(BasicMathTest, AabbEmptyBoxContainsAndIntersectsNothing) {
    const Aabb empty;
    const Aabb valid{{1, 2, 3}, {4, 5, 6}};

    EXPECT_FALSE(empty.Contains(Vec3{1, 2, 3}));
    EXPECT_FALSE(empty.Contains(Aabb{}));
    EXPECT_FALSE(valid.Contains(empty));
    EXPECT_FALSE(empty.Intersects(valid));
    EXPECT_TRUE(valid.Intersection(empty).IsEmpty());
}

TEST(BasicMathTest, AabbIntersectionOfOverlappingBoxes) {
    const Aabb a{{0, 0, 0}, {2, 2, 2}};
    const Aabb b{{-1, 1, -1}, {1, 3, 1}};

    const Aabb intersection = a.Intersection(b);

    EXPECT_TRUE(a.Intersects(b));
    EXPECT_TRUE(b.Intersects(a));
    EXPECT_EQ(intersection.Min, (Point{0, 1, 0}));
    EXPECT_EQ(intersection.Max, (Point{1, 2, 1}));
}

TEST(BasicMathTest, AabbTouchingBoxesIntersectInDegenerateBox) {
    const Aabb a{{0, 0, 0}, {2, 2, 2}};
    const Aabb corner{{2, 2, 2}, {3, 3, 3}};

    const Aabb intersection = a.Intersection(corner);

    EXPECT_TRUE(a.Intersects(corner));
    EXPECT_FALSE(intersection.IsEmpty());
    EXPECT_EQ(intersection.Min, (Point{2, 2, 2}));
    EXPECT_EQ(intersection.Max, (Point{2, 2, 2}));
}

TEST(BasicMathTest, AabbDisjointBoxesHaveEmptyIntersection) {
    const Aabb a{{0, 0, 0}, {2, 2, 2}};
    const Aabb b{{3, 0, 0}, {4, 2, 2}};

    EXPECT_FALSE(a.Intersects(b));
    EXPECT_TRUE(a.Intersection(b).IsEmpty());
}

TEST(BasicMathTest, AabbCenterAndExtentAvoidIntermediateOverflow) {
    const float largest = std::numeric_limits<float>::max();
    const Aabb bounds{{-largest, largest / 2, 0}, {largest, largest, 0}};

    const Vec3 center = bounds.GetCenter();
    const Vec3 extent = bounds.GetExtent();

    EXPECT_FLOAT_EQ(center.x(), 0);
    EXPECT_FLOAT_EQ(extent.x(), largest);
    EXPECT_FLOAT_EQ(center.y(), largest * 0.75f);
    EXPECT_FLOAT_EQ(extent.y(), largest * 0.25f);
}

TEST(BasicMathTest, AabbTransformEnclosesAllTransformedCorners) {
    std::mt19937 generator{20261031u};
    std::uniform_real_distribution<float> uniform{-4.0f, 4.0f};
    const Aabb bounds{{-1, 0.5f, 2}, {3, 1.5f, 2.25f}};
    vector<Eigen::Matrix4f> matrices;
    Eigen::Matrix4f collapse = Eigen::Matrix4f::Identity();
    collapse.block<3, 3>(0, 0).setZero();
    collapse.block<3, 1>(0, 3) = Vec3{1, -2, 3};
    matrices.push_back(collapse);
    matrices.push_back(ComposeTransform(Vec3{2, -3, 1}, Eigen::Quaternionf{Eigen::AngleAxisf{0.7f, Vec3::UnitY()}}, Vec3{-2, 0.5f, 3}));
    for (int i = 0; i < 16; ++i) {
        Eigen::Matrix4f sheared = Eigen::Matrix4f::Identity();
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 3; ++row) sheared(row, column) = uniform(generator);
        }
        matrices.push_back(sheared);
    }

    for (const Eigen::Matrix4f& matrix : matrices) {
        const Aabb actual = bounds.Transform(AffineTransform{matrix});

        Eigen::Vector3d expectedMin = Eigen::Vector3d::Constant(std::numeric_limits<double>::infinity());
        Eigen::Vector3d expectedMax = -expectedMin;
        for (size_t corner = 0; corner < 8; ++corner) {
            Eigen::Vector4d point{0, 0, 0, 1};
            for (size_t axis = 0; axis < 3; ++axis) point[axis] = (corner & (size_t{1} << axis)) ? bounds.Max[axis] : bounds.Min[axis];
            const Eigen::Vector3d transformed = (matrix.cast<double>() * point).head<3>();
            expectedMin = expectedMin.cwiseMin(transformed);
            expectedMax = expectedMax.cwiseMax(transformed);
        }
        ASSERT_FALSE(actual.IsEmpty());
        for (size_t axis = 0; axis < 3; ++axis) {
            EXPECT_NEAR(actual.Min[axis], expectedMin[axis], 2e-5);
            EXPECT_NEAR(actual.Max[axis], expectedMax[axis], 2e-5);
        }
    }
}

TEST(BasicMathTest, AabbTransformOfEmptyBoxIsEmpty) {
    EXPECT_TRUE(Aabb{}.Transform(AffineTransform{}).IsEmpty());
}

}  // namespace
}  // namespace radray
