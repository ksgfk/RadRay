#include <gtest/gtest.h>

#include <optional>
#include <string_view>
#include <utility>

#include <radray/runtime/shader_program.h>

#include "../../shader/tests/shader_manifest_fixtures.h"

// manifest -> RHI 描述 的打包逻辑测试。
//
// 这些用例覆盖 runtime 层的 program 构建辅助逻辑, 产出物是创建 RHI 对象所需的描述。

namespace radray {
namespace {

using test::kForwardManifest;
using test::kImGuiManifest;

ShaderAssetDesc ParseOk(std::string_view json) {
    ShaderAssetDiagnostic diag{};
    std::optional<ShaderAssetDesc> desc = ParseShaderAssetDesc(json, diag);
    EXPECT_TRUE(desc.has_value()) << diag.ToString();
    return desc.has_value() ? std::move(desc.value()) : ShaderAssetDesc{};
}

}  // namespace

TEST(ShaderLayoutBindingTest, BuildsPipelineLayoutWithoutReflection) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    ShaderPipelineLayoutStorage storage = BuildPipelineLayoutStorage(desc.Passes.front());
    const render::PipelineLayoutDescriptor layout = storage.Get();

    ASSERT_EQ(layout.ParameterSets.size(), 1u);
    EXPECT_EQ(layout.ParameterSets[0].GroupIndex, 1u);
    ASSERT_EQ(layout.ParameterSets[0].Entries.size(), 2u);
    EXPECT_EQ(layout.ParameterSets[0].Entries[0].Binding, 0u);
    EXPECT_EQ(layout.ParameterSets[0].Entries[0].Type, render::ShaderParameterBindingType::Texture);
    EXPECT_EQ(layout.ParameterSets[0].Entries[0].Count, 1u);
    EXPECT_EQ(layout.ParameterSets[0].Entries[1].Type, render::ShaderParameterBindingType::Sampler);
    EXPECT_TRUE(layout.ParameterSets[0].Entries[1].ImmutableSampler.has_value());

    ASSERT_TRUE(layout.PushConstant.has_value());
    EXPECT_EQ(layout.PushConstant->Size, 16u);
    EXPECT_EQ(layout.PushConstant->Location.Group, 0u);
    EXPECT_EQ(layout.PushConstant->Location.Binding, 0u);
    EXPECT_EQ(layout.PushConstant->Stages, render::ShaderStages{render::ShaderStage::Vertex});
}

TEST(ShaderLayoutBindingTest, RootDescriptorResidencyFoldsIntoDynamicBindingType) {
    const ShaderAssetDesc desc = ParseOk(kForwardManifest);
    ShaderPipelineLayoutStorage storage = BuildPipelineLayoutStorage(desc.Passes.front());
    const render::PipelineLayoutDescriptor layout = storage.Get();

    ASSERT_EQ(layout.ParameterSets.size(), 3u);
    ASSERT_EQ(layout.ParameterSets[0].Entries.size(), 1u);
    EXPECT_EQ(
        layout.ParameterSets[0].Entries[0].Type,
        render::ShaderParameterBindingType::DynamicCBuffer);
    EXPECT_EQ(
        layout.ParameterSets[1].Entries[0].Type,
        render::ShaderParameterBindingType::DynamicCBuffer);
    EXPECT_EQ(
        layout.ParameterSets[1].Entries[1].Type,
        render::ShaderParameterBindingType::Texture);
    EXPECT_FALSE(layout.PushConstant.has_value());
}

TEST(ShaderLayoutBindingTest, PipelineLayoutSpansStayValidAfterStorageMove) {
    const ShaderAssetDesc desc = ParseOk(kForwardManifest);
    ShaderPipelineLayoutStorage storage = BuildPipelineLayoutStorage(desc.Passes.front());
    ShaderPipelineLayoutStorage moved = std::move(storage);

    const render::PipelineLayoutDescriptor layout = moved.Get();
    ASSERT_EQ(layout.ParameterSets.size(), 3u);
    ASSERT_EQ(layout.ParameterSets[2].Entries.size(), 7u);
    EXPECT_EQ(layout.ParameterSets[2].Entries[6].Type, render::ShaderParameterBindingType::Sampler);
}

TEST(ShaderLayoutBindingTest, BuildsVertexInputState) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    ASSERT_TRUE(desc.Passes.front().VertexInput.has_value());
    ShaderVertexInputStorage storage =
        BuildVertexInputStorage(desc.Passes.front().VertexInput.value());
    const render::VertexInputState state = storage.Get();

    ASSERT_EQ(state.Buffers.size(), 1u);
    EXPECT_EQ(state.Buffers[0].ArrayStride, 20u);
    EXPECT_EQ(state.Buffers[0].StepMode, render::VertexStepMode::Vertex);
    ASSERT_EQ(state.Attributes.size(), 3u);
    EXPECT_EQ(state.Attributes[0].Semantic, "POSITION");
    EXPECT_EQ(state.Attributes[0].Format, render::VertexFormat::FLOAT32X2);
    EXPECT_EQ(state.Attributes[0].Location, 0u);
    EXPECT_EQ(state.Attributes[2].Location, 2u);
    EXPECT_EQ(state.Attributes[2].Offset, 16u);
    EXPECT_EQ(state.Attributes[2].Format, render::VertexFormat::UNORM8X4);
}

TEST(ShaderLayoutBindingTest, MakeShaderDescriptorForwardsBytecodeFields) {
    ShaderBytecode bytecode{};
    bytecode.Data = vector<byte>{byte{1}, byte{2}, byte{3}, byte{4}};
    bytecode.Category = render::ShaderBlobCategory::SPIRV;
    bytecode.Stage = render::ShaderStage::Pixel;

    const render::ShaderDescriptor desc = MakeShaderDescriptor(bytecode);
    ASSERT_EQ(desc.Source.size(), bytecode.Data.size());
    EXPECT_EQ(desc.Source.data(), bytecode.Data.data());
    EXPECT_EQ(desc.Category, render::ShaderBlobCategory::SPIRV);
    EXPECT_TRUE(desc.Stages.HasFlag(render::ShaderStage::Pixel));
}

TEST(ShaderLayoutBindingTest, VertexInputSemanticsStayValidAfterStorageMove) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    ShaderVertexInputStorage storage =
        BuildVertexInputStorage(desc.Passes.front().VertexInput.value());
    ShaderVertexInputStorage moved = std::move(storage);

    const render::VertexInputState state = moved.Get();
    ASSERT_EQ(state.Attributes.size(), 3u);
    EXPECT_EQ(state.Attributes[1].Semantic, "TEXCOORD");
}

}  // namespace radray
