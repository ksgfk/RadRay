#include <gtest/gtest.h>
#include "test_asset_fakes.h"

namespace radray {
namespace {
TEST(ShaderVertexInput, ResolvesVariantLocationsWithoutGpuStreams) {
    VertexInput mesh;
    mesh.Layouts = {{0, 12, gpu::VertexStepMode::Vertex}};
    mesh.Attributes = {{0, 0, "POSITION", 0, gpu::VertexFormat::FLOAT32X3},
                       {0, 0, "NORMAL", 0, gpu::VertexFormat::FLOAT32X3}};
    ASSERT_TRUE(ValidateVertexInputLayout(mesh));
    const array signature{shader::ShaderVertexAttributeSignatureDesc{"NORMAL", 0, 5, shader::ShaderVertexComponentType::Float, 3}};
    ResolvedVertexInputState result;
    ASSERT_TRUE(ResolveVertexInputState(mesh, signature, result));
    ASSERT_EQ(result.Attributes.size(), 1u);
    EXPECT_EQ(result.Attributes[0].Semantic, "NORMAL");
    EXPECT_EQ(result.Attributes[0].Location, 5u);
    auto invalid = signature;
    invalid[0].ComponentCount = 4;
    EXPECT_FALSE(ResolveVertexInputState(mesh, invalid, result));
    EXPECT_EQ(result.Attributes[0].Location, 5u);
    invalid[0].ComponentCount = 3;
    invalid[0].ComponentKind = shader::ShaderVertexComponentType::UnsignedInteger;
    EXPECT_FALSE(ResolveVertexInputState(mesh, invalid, result));
    mesh.Layouts[0].ArrayStride = 0;
    EXPECT_FALSE(ValidateVertexInputLayout(mesh));
}

TEST(ShaderVertexInput, EmptyVertexSignatureProducesNoAttributes) {
    VertexInput mesh;
    mesh.Layouts = {{0, 12, gpu::VertexStepMode::Vertex}};
    mesh.Attributes = {{0, 0, "POSITION", 0, gpu::VertexFormat::FLOAT32X3}};
    shader::ShaderVariantTargetInterfaceDesc interface;
    interface.Stages = gpu::ShaderStage::Vertex;
    ResolvedVertexInputState result;
    EXPECT_TRUE(PrepareMaterialVertexInput(interface, mesh, result));
    EXPECT_TRUE(result.Attributes.empty());
    interface.Stages = gpu::ShaderStage::Compute;
    EXPECT_FALSE(PrepareMaterialVertexInput(interface, mesh, result));
}
}  // namespace
}  // namespace radray
