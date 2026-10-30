#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <random>

#include <radray/basic_math.h>

using namespace radray;

namespace {

/// ComposeTransform 的参考实现：Eigen 表达式模板版本（手写展开前的原始语义）。
Eigen::Matrix4f ReferenceCompose(const Eigen::Vector3f& translation, const Eigen::Quaternionf& rotation, const Eigen::Vector3f& scale) {
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    m.block<3, 3>(0, 0) = rotation.toRotationMatrix() * Eigen::Scaling(scale).toDenseMatrix();
    m.block<3, 1>(0, 3) = translation;
    return m;
}

}  // namespace

TEST(BasicMathTest, AffineTransformRoundTripPreservesArbitraryAffineMatrices) {
    std::mt19937 generator{20260922u};
    std::uniform_real_distribution<float> uniform{-4.0f, 4.0f};
    for (size_t iteration = 0; iteration < 2048; ++iteration) {
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 3; ++row) {
                matrix(row, column) = uniform(generator);
            }
        }
        const AffineTransform packed{matrix};
        EXPECT_TRUE(packed.ToMatrix().isApprox(matrix, 0.0f)) << iteration;
    }
}

TEST(BasicMathTest, AffineTransformIdentityAndSingularScaleRemainRepresentable) {
    EXPECT_TRUE(AffineTransform{}.ToMatrix().isIdentity());
    Eigen::Matrix4f singular = Eigen::Matrix4f::Identity();
    singular(0, 0) = 0;
    singular(1, 1) = -2;
    singular(0, 1) = 3;
    singular(2, 3) = 7;
    const AffineTransform value{singular};
    EXPECT_TRUE(value.ToMatrix().isApprox(singular, 0.0f));
}

TEST(BasicMathTest, ComposeTransformMatchesReferenceForRandomInputs) {
    std::mt19937 generator{20260921u};
    std::uniform_real_distribution<float> uniform{-4.0f, 4.0f};
    for (int iteration = 0; iteration < 4096; ++iteration) {
        const Eigen::Vector3f translation{uniform(generator), uniform(generator), uniform(generator)};
        Eigen::Quaternionf rotation{uniform(generator), uniform(generator), uniform(generator), uniform(generator)};
        if (rotation.coeffs().norm() < 1e-3f) rotation = Eigen::Quaternionf::Identity();
        const Eigen::Vector3f scale{uniform(generator), uniform(generator), uniform(generator)};
        const Eigen::Matrix4f composed = ComposeTransform(translation, rotation, scale);
        const Eigen::Matrix4f reference = ReferenceCompose(translation, rotation, scale);
        ASSERT_TRUE(composed.isApprox(reference, 0.0f)) << "iteration " << iteration;
        for (int i = 0; i < 16; ++i) {
            ASSERT_FLOAT_EQ(composed.data()[i], reference.data()[i]) << "iteration " << iteration << " element " << i;
        }
    }
}

TEST(BasicMathTest, ComposeTransformCoversEdgeCases) {
    const Eigen::Vector3f zero = Eigen::Vector3f::Zero();
    const Eigen::Vector3f one = Eigen::Vector3f::Ones();
    const Eigen::Quaternionf identity = Eigen::Quaternionf::Identity();
    // 默认 TRS 合成单位矩阵。
    ASSERT_TRUE(ComposeTransform(zero, identity, one).isApprox(Eigen::Matrix4f::Identity()));
    // 负缩放（ReverseCulling 路径依赖）与非单位四元数（与 Eigen 参考一致，不做归一化）。
    const Eigen::Vector3f negative{-1.0f, 1.0f, 1.0f};
    ASSERT_TRUE(ComposeTransform(zero, identity, negative).isApprox(ReferenceCompose(zero, identity, negative)));
    const Eigen::Quaternionf nonUnit{0.5f, 0.5f, 0.5f, 0.5f};
    ASSERT_TRUE(ComposeTransform(one, nonUnit, one).isApprox(ReferenceCompose(one, nonUnit, one)));
    // 平移只进最后一列，齐次行保持 (0, 0, 0, 1)。
    const Eigen::Matrix4f translated = ComposeTransform(Eigen::Vector3f{1, 2, 3}, identity, one);
    EXPECT_FLOAT_EQ(translated(0, 3), 1.0f);
    EXPECT_FLOAT_EQ(translated(1, 3), 2.0f);
    EXPECT_FLOAT_EQ(translated(2, 3), 3.0f);
    EXPECT_FLOAT_EQ(translated(3, 0), 0.0f);
    EXPECT_FLOAT_EQ(translated(3, 1), 0.0f);
    EXPECT_FLOAT_EQ(translated(3, 2), 0.0f);
    EXPECT_FLOAT_EQ(translated(3, 3), 1.0f);
}

TEST(BasicMathTest, AabbAccumulatesPointsAndBoxesWithoutIncludingOrigin) {
    Aabb bounds;
    EXPECT_TRUE(bounds.IsEmpty());
    EXPECT_TRUE(bounds.GetCenter().isZero());
    EXPECT_TRUE(bounds.GetSize().isZero());
    EXPECT_TRUE(bounds.GetExtent().isZero());
    bounds.Expand(Eigen::Vector3f{-5, -4, -3});
    EXPECT_FALSE(bounds.IsEmpty());
    EXPECT_EQ(bounds.Min, bounds.Max);
    bounds.Expand(Eigen::Vector3f{-8, -2, -6});
    EXPECT_EQ(bounds.Min, (array<float, 3>{-8, -4, -6}));
    EXPECT_EQ(bounds.Max, (array<float, 3>{-5, -2, -3}));
    EXPECT_FALSE(bounds.Contains(Eigen::Vector3f::Zero().eval()));
    EXPECT_TRUE(bounds.GetCenter().isApprox(Eigen::Vector3f{-6.5f, -3, -4.5f}));
    EXPECT_TRUE(bounds.GetSize().isApprox(Eigen::Vector3f{3, 2, 3}));
    EXPECT_TRUE(bounds.GetExtent().isApprox(Eigen::Vector3f{1.5f, 1, 1.5f}));
    const Aabb other{{-10, -3, -2}, {-9, 0, -1}};
    bounds.Expand(other);
    bounds.Expand(bounds);
    EXPECT_EQ(bounds.Min, (array<float, 3>{-10, -4, -6}));
    EXPECT_EQ(bounds.Max, (array<float, 3>{-5, 0, -1}));
    EXPECT_TRUE(bounds.Contains(other));
    Aabb copy;
    copy.Expand(bounds);
    EXPECT_EQ(copy.Min, bounds.Min);
    EXPECT_EQ(copy.Max, bounds.Max);
}

TEST(BasicMathTest, AabbContainmentAndIntersectionIncludeBoundaryContact) {
    const Aabb bounds{{0, 0, 0}, {2, 2, 2}};
    EXPECT_TRUE(bounds.Contains(Eigen::Vector3f{0, 2, 1}));
    EXPECT_FALSE(bounds.Contains(Eigen::Vector3f{std::nextafter(2.0f, 3.0f), 1, 1}));
    EXPECT_TRUE(bounds.Contains(Aabb{{1, 1, 1}, {1, 1, 1}}));
    const Aabb overlap{{-1, 1, -1}, {1, 3, 1}};
    EXPECT_FALSE(bounds.Contains(overlap));
    EXPECT_TRUE(bounds.Intersects(overlap));
    EXPECT_TRUE(overlap.Intersects(bounds));
    const auto intersection = bounds.Intersection(overlap);
    EXPECT_EQ(intersection.Min, (array<float, 3>{0, 1, 0}));
    EXPECT_EQ(intersection.Max, (array<float, 3>{1, 2, 1}));
    const Aabb touching{{2, 2, 2}, {3, 3, 3}};
    EXPECT_TRUE(bounds.Intersects(touching));
    const auto point = bounds.Intersection(touching);
    EXPECT_FALSE(point.IsEmpty());
    EXPECT_EQ(point.Min, (array<float, 3>{2, 2, 2}));
    EXPECT_EQ(point.Max, point.Min);
    const Aabb disjoint{{3, 0, 0}, {4, 2, 2}};
    EXPECT_FALSE(bounds.Intersects(disjoint));
    EXPECT_TRUE(bounds.Intersection(disjoint).IsEmpty());
}

TEST(BasicMathTest, AabbEmptyAndInvalidInputsHaveDefinedBehavior) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const Aabb valid{{1, 2, 3}, {4, 5, 6}};
    const array invalid{Aabb{}, Aabb{{2, 0, 0}, {1, 1, 1}},
                        Aabb{{nan, 0, 0}, {1, 1, 1}}, Aabb{{0, 0, 0}, {infinity, 1, 1}}};
    for (auto bounds : invalid) {
        EXPECT_TRUE(bounds.IsEmpty());
        EXPECT_FALSE(bounds.Contains(Eigen::Vector3f{1, 2, 3}));
        EXPECT_FALSE(bounds.Contains(valid));
        EXPECT_FALSE(valid.Contains(bounds));
        EXPECT_FALSE(bounds.Intersects(valid));
        EXPECT_TRUE(valid.Intersection(bounds).IsEmpty());
        EXPECT_TRUE(bounds.Transform(AffineTransform{}).IsEmpty());
        bounds.Expand(Eigen::Vector3f{1, 2, 3});
        EXPECT_FALSE(bounds.IsEmpty());
        EXPECT_EQ(bounds.Min, (array<float, 3>{1, 2, 3}));
        EXPECT_EQ(bounds.Max, bounds.Min);
    }
    Aabb bounds = valid;
    for (const auto& empty : invalid) bounds.Expand(empty);
    bounds.Expand(Eigen::Vector3f{nan, 0, 0});
    bounds.Expand(Eigen::Vector3f{0, infinity, 0});
    EXPECT_EQ(bounds.Min, valid.Min);
    EXPECT_EQ(bounds.Max, valid.Max);
    EXPECT_FALSE(bounds.Contains(Eigen::Vector3f{nan, 0, 0}));
    EXPECT_FALSE(Aabb{}.Contains(Aabb{}));
}

TEST(BasicMathTest, AabbCenterAndExtentAvoidIntermediateOverflow) {
    const float largest = std::numeric_limits<float>::max();
    const Aabb bounds{{-largest, largest / 2, 0}, {largest, largest, 0}};
    EXPECT_FALSE(bounds.IsEmpty());
    const auto center = bounds.GetCenter();
    const auto extent = bounds.GetExtent();
    EXPECT_TRUE(center.allFinite());
    EXPECT_TRUE(extent.allFinite());
    EXPECT_FLOAT_EQ(center.x(), 0);
    EXPECT_FLOAT_EQ(extent.x(), largest);
    EXPECT_FLOAT_EQ(center.y(), largest * 0.75f);
    EXPECT_FLOAT_EQ(extent.y(), largest * 0.25f);
}

TEST(BasicMathTest, AabbAffineTransformMatchesEightCornerReference) {
    std::mt19937 generator{20261031u};
    std::uniform_real_distribution<float> uniform{-4.0f, 4.0f};
    for (size_t iteration = 0; iteration < 256; ++iteration) {
        SCOPED_TRACE(iteration);
        const Eigen::Vector3f min{uniform(generator), uniform(generator), uniform(generator)};
        const Eigen::Vector3f size{std::abs(uniform(generator)), std::abs(uniform(generator)), std::abs(uniform(generator))};
        const Aabb bounds{min, min + size};
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
        if (iteration == 1) {
            matrix.block<3, 3>(0, 0).setZero();
            matrix.block<3, 1>(0, 3) = Eigen::Vector3f{1, -2, 3};
        } else if (iteration == 2) {
            const Eigen::Quaternionf rotation{Eigen::AngleAxisf{0.7f, Eigen::Vector3f::UnitY()}};
            matrix = ComposeTransform(Eigen::Vector3f{2, -3, 1}, rotation, Eigen::Vector3f{-2, 0.5f, 3});
        } else if (iteration > 2) {
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 3; ++row) matrix(row, column) = uniform(generator);
            }
        }
        const auto actual = bounds.Transform(AffineTransform{matrix});
        ASSERT_FALSE(actual.IsEmpty());
        Eigen::Vector3d expectedMin = Eigen::Vector3d::Constant(std::numeric_limits<double>::infinity());
        Eigen::Vector3d expectedMax = -expectedMin;
        for (size_t cornerIndex = 0; cornerIndex < 8; ++cornerIndex) {
            Eigen::Vector4d corner{0, 0, 0, 1};
            for (size_t axis = 0; axis < 3; ++axis) {
                corner[axis] = (cornerIndex & (size_t{1} << axis)) ? bounds.Max[axis] : bounds.Min[axis];
            }
            const Eigen::Vector4d transformed = matrix.cast<double>() * corner;
            expectedMin = expectedMin.cwiseMin(transformed.head<3>());
            expectedMax = expectedMax.cwiseMax(transformed.head<3>());
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            EXPECT_NEAR(actual.Min[axis], expectedMin[axis], 2e-5);
            EXPECT_NEAR(actual.Max[axis], expectedMax[axis], 2e-5);
        }
    }
}
