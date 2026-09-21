#include <gtest/gtest.h>

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
