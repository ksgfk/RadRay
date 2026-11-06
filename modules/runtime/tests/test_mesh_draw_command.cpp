#include <gtest/gtest.h>

#include <limits>

#include <radray/runtime/mesh_pass_processor.h>

#include "test_fakes.h"

namespace radray {
namespace {

using test::FakeBuffer;
using test::FakeParameterSet;
using test::FakePipeline;
using test::RecordingEncoder;

class VertexInputTest : public testing::Test {
protected:
    VertexInputTest() {
        Input.Layouts.push_back({0, 12, render::VertexStepMode::Vertex});
        Input.Attributes.push_back({0, 0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0});
        Input.Streams.push_back({&Vertices, 0, 48, 0});
    }
    void AddInstanceLayout(uint32_t binding, uint32_t location) {
        Input.Layouts.push_back({binding, 16, render::VertexStepMode::Instance});
        Input.Attributes.push_back({binding, 0, "INSTANCE", location, render::VertexFormat::FLOAT32X4, location});
    }

    FakeBuffer Vertices{128, render::BufferUse::Vertex};
    VertexInput Input;
};

TEST_F(VertexInputTest, AcceptsLayoutsWithBoundStreams) {
    EXPECT_TRUE(ValidateVertexInput(Input));
}

TEST_F(VertexInputTest, RejectsLayoutWithoutStream) {
    Input.Streams.clear();
    EXPECT_FALSE(ValidateVertexInput(Input));
    EXPECT_FALSE(ValidateVertexInput(Input, true));
}

TEST_F(VertexInputTest, RejectsDuplicateStreamBinding) {
    Input.Streams.push_back({&Vertices, 48, 48, 0});
    EXPECT_FALSE(ValidateVertexInput(Input));
}

TEST_F(VertexInputTest, DeferredModeAllowsOneMissingInstanceStream) {
    AddInstanceLayout(1, 1);
    EXPECT_FALSE(ValidateVertexInput(Input));
    EXPECT_TRUE(ValidateVertexInput(Input, true));
}

TEST_F(VertexInputTest, DeferredModeRejectsMissingStreamWithSeveralInstanceLayouts) {
    AddInstanceLayout(1, 1);
    AddInstanceLayout(2, 2);
    Input.Streams.push_back({&Vertices, 0, 64, 1});
    EXPECT_FALSE(ValidateVertexInput(Input, true));
}

TEST_F(VertexInputTest, AcceptsSeveralInstanceLayoutsWhenAllStreamsBound) {
    AddInstanceLayout(1, 1);
    AddInstanceLayout(2, 2);
    Input.Streams.push_back({&Vertices, 0, 64, 1});
    Input.Streams.push_back({&Vertices, 64, 64, 2});
    EXPECT_TRUE(ValidateVertexInput(Input));
    EXPECT_TRUE(ValidateVertexInput(Input, true));
}

class MeshDrawCommandTest : public testing::Test {
protected:
    MeshDrawCommand MakeCommand() {
        MeshDrawCommand command;
        command.Pipeline = &Pipeline;
        command.VertexBuffers.push_back({0, {&Vertices, 0, 48}});
        command.IndexBuffer = {&Indices, 0, 4};
        command.IndexCount = 3;
        return command;
    }
    // Adds a default per-instance stream at binding 1 that submit arguments may replace.
    MeshDrawCommand MakeInstancedCommand() {
        auto command = MakeCommand();
        command.VertexBuffers.push_back({1, {&Instances, 0, 64}});
        command.InstanceStreamBinding = 1;
        return command;
    }

    FakePipeline Pipeline;
    FakeBuffer Vertices{128, render::BufferUse::Vertex};
    FakeBuffer Instances{128, render::BufferUse::Vertex};
    FakeBuffer Indices{128, render::BufferUse::Index};
    FakeParameterSet Parameters;
};

TEST_F(MeshDrawCommandTest, AcceptsCompleteCommand) {
    EXPECT_TRUE(ValidateMeshDrawCommand(MakeCommand()));
}

TEST_F(MeshDrawCommandTest, RejectsIndexRangePastBufferEnd) {
    auto command = MakeCommand();
    command.FirstIndex = 30;
    EXPECT_FALSE(ValidateMeshDrawCommand(command));
    command.FirstIndex = 29;
    EXPECT_TRUE(ValidateMeshDrawCommand(command));
}

TEST_F(MeshDrawCommandTest, RejectsIndexStrideOtherThanTwoOrFour) {
    auto command = MakeCommand();
    command.IndexBuffer.Stride = 3;
    EXPECT_FALSE(ValidateMeshDrawCommand(command));
}

TEST_F(MeshDrawCommandTest, RejectsIndexOffsetNotAlignedToStride) {
    auto command = MakeCommand();
    command.IndexBuffer.Offset = 2;
    EXPECT_FALSE(ValidateMeshDrawCommand(command));
}

TEST_F(MeshDrawCommandTest, RejectsInstanceRangePastUint32) {
    const VisibleMeshDrawCommand visible{.FirstInstance = std::numeric_limits<uint32_t>::max(), .InstanceCount = 2};
    EXPECT_FALSE(ValidateMeshDrawCommand(MakeCommand(), visible));
}

TEST_F(MeshDrawCommandTest, RejectsDuplicateVertexBinding) {
    auto command = MakeCommand();
    command.VertexBuffers.push_back({0, {&Vertices, 48, 48}});
    EXPECT_FALSE(ValidateMeshDrawCommand(command));
}

TEST_F(MeshDrawCommandTest, RejectsDuplicateParameterGroup) {
    auto command = MakeCommand();
    command.ParameterSets.push_back({1, &Parameters, {}});
    command.ParameterSets.push_back({1, &Parameters, {}});
    EXPECT_FALSE(ValidateMeshDrawCommand(command));
}

TEST_F(MeshDrawCommandTest, RejectsCommandGroupAlsoUsedByPass) {
    auto command = MakeCommand();
    command.ParameterSets.push_back({1, &Parameters, {}});
    const MeshShaderParameterBinding pass{1, &Parameters, {}};
    EXPECT_FALSE(ValidateMeshDrawCommand(command, {}, {.ParameterSets = std::span{&pass, 1}}));
}

TEST_F(MeshDrawCommandTest, RejectsPushConstantsNotFourByteAligned) {
    auto command = MakeCommand();
    command.PushConstants = MeshPushConstantCommandBinding{0, 2, {byte{}, byte{}, byte{}, byte{}}};
    EXPECT_FALSE(ValidateMeshDrawCommand(command));
    command.PushConstants->Offset = 0;
    command.PushConstants->Data.pop_back();
    EXPECT_FALSE(ValidateMeshDrawCommand(command));
}

TEST_F(MeshDrawCommandTest, DeclaredInstanceStreamMustBeSuppliedWhenMissing) {
    auto command = MakeCommand();
    command.InstanceStreamBinding = 1;
    const MeshPassSubmitArgs args{.InstanceStream = render::VertexBufferBinding{1, {&Instances, 0, 64}}};
    EXPECT_FALSE(ValidateMeshDrawCommand(command));
    EXPECT_TRUE(ValidateMeshDrawCommand(command, {}, args));
}

TEST_F(MeshDrawCommandTest, RejectsInstanceStreamForUndeclaredSlot) {
    const MeshPassSubmitArgs args{.InstanceStream = render::VertexBufferBinding{0, {&Instances, 0, 64}}};
    EXPECT_FALSE(ValidateMeshDrawCommand(MakeCommand(), {}, args));
    EXPECT_FALSE(ValidateMeshDrawCommand(MakeInstancedCommand(), {}, args));
}

class SubmitMeshDrawCommandsTest : public MeshDrawCommandTest {
protected:
    VisibleMeshDrawCommand Dynamic(const MeshDrawCommand& command) const {
        return {.Source = MeshDrawCommandSource::Dynamic, .CachedCommand = &command};
    }

    RecordingEncoder Encoder;
};

TEST_F(SubmitMeshDrawCommandsTest, DrawsWithCommandGeometryAndVisibleInstances) {
    auto command = MakeCommand();
    command.FirstIndex = 6;
    command.VertexOffset = -2;
    const array commands{command};
    auto visible = Dynamic(commands[0]);
    visible.FirstInstance = 5;
    visible.InstanceCount = 3;
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}));
    ASSERT_EQ(Encoder.Draws.size(), 1u);
    const auto& draw = Encoder.Draws[0];
    EXPECT_EQ(draw.IndexCount, 3u);
    EXPECT_EQ(draw.InstanceCount, 3u);
    EXPECT_EQ(draw.FirstIndex, 6u);
    EXPECT_EQ(draw.VertexOffset, -2);
    EXPECT_EQ(draw.FirstInstance, 5u);
}

TEST_F(SubmitMeshDrawCommandsTest, BindsStateInDocumentedOrder) {
    auto command = MakeCommand();
    command.ParameterSets.push_back({1, &Parameters, {}});
    command.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{}, byte{}, byte{}, byte{}}};
    const array commands{command};
    const auto visible = Dynamic(commands[0]);
    FakeParameterSet passParameters;
    const MeshShaderParameterBinding pass{0, &passParameters, {}};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}, {.ParameterSets = std::span{&pass, 1}}));
    const vector<std::string_view> expected{"pipeline", "parameters", "vertex", "index", "stencil", "parameters", "push", "draw"};
    EXPECT_EQ(Encoder.Order, expected);
    ASSERT_EQ(Encoder.ParameterBinds.size(), 2u);
    EXPECT_EQ(Encoder.ParameterBinds[0].Set, &passParameters);
    EXPECT_EQ(Encoder.ParameterBinds[1].Set, &Parameters);
}

TEST_F(SubmitMeshDrawCommandsTest, BindsUnchangedStateOncePerCall) {
    auto command = MakeCommand();
    command.ParameterSets.push_back({1, &Parameters, {}});
    command.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{}, byte{}, byte{}, byte{}}};
    const array commands{command, command};
    const array visible{Dynamic(commands[0]), Dynamic(commands[1])};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    EXPECT_EQ(Encoder.Pipelines.size(), 1u);
    EXPECT_EQ(Encoder.VertexCalls.size(), 1u);
    EXPECT_EQ(Encoder.IndexBuffers.size(), 1u);
    EXPECT_EQ(Encoder.Stencils.size(), 1u);
    EXPECT_EQ(Encoder.ParameterBinds.size(), 1u);
    EXPECT_EQ(Encoder.Pushes.size(), 2u);
    EXPECT_EQ(Encoder.Draws.size(), 2u);
}

TEST_F(SubmitMeshDrawCommandsTest, PipelineChangeRebindsPassParametersAndVertexBuffers) {
    FakePipeline other;
    auto second = MakeCommand();
    second.Pipeline = &other;
    const array commands{MakeCommand(), second};
    const array visible{Dynamic(commands[0]), Dynamic(commands[1])};
    FakeParameterSet passParameters;
    const MeshShaderParameterBinding pass{0, &passParameters, {}};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible, {.ParameterSets = std::span{&pass, 1}}));
    EXPECT_EQ(Encoder.Pipelines, (vector<render::GraphicsPipelineState*>{&Pipeline, &other}));
    EXPECT_EQ(Encoder.ParameterBinds.size(), 2u);
    EXPECT_EQ(Encoder.VertexCalls.size(), 2u);
    EXPECT_EQ(Encoder.IndexBuffers.size(), 1u);
}

TEST_F(SubmitMeshDrawCommandsTest, VertexSlotsBindAsContiguousRanges) {
    auto first = MakeCommand();
    first.VertexBuffers = {{2, {&Vertices, 0, 32}}, {0, {&Vertices, 32, 32}}, {1, {&Vertices, 64, 32}}};
    auto second = first;
    second.VertexBuffers[0].View.Offset = 16;
    second.VertexBuffers[1].View.Offset = 48;
    const array commands{first, second};
    const array visible{Dynamic(commands[0]), Dynamic(commands[1])};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    ASSERT_EQ(Encoder.VertexCalls.size(), 3u);
    ASSERT_EQ(Encoder.VertexCalls[0].size(), 3u);
    for (uint32_t slot = 0; slot < 3; ++slot) EXPECT_EQ(Encoder.VertexCalls[0][slot].Binding, slot);
    ASSERT_EQ(Encoder.VertexCalls[1].size(), 1u);
    EXPECT_EQ(Encoder.VertexCalls[1][0].Binding, 0u);
    ASSERT_EQ(Encoder.VertexCalls[2].size(), 1u);
    EXPECT_EQ(Encoder.VertexCalls[2][0].Binding, 2u);
}

TEST_F(SubmitMeshDrawCommandsTest, SubmitInstanceStreamReplacesDeclaredSlot) {
    const array commands{MakeInstancedCommand()};
    const auto visible = Dynamic(commands[0]);
    const MeshPassSubmitArgs args{.InstanceStream = render::VertexBufferBinding{1, {&Instances, 16, 48}}};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}, args));
    ASSERT_EQ(Encoder.VertexCalls.size(), 1u);
    ASSERT_EQ(Encoder.VertexCalls[0].size(), 2u);
    EXPECT_EQ(Encoder.VertexCalls[0][1].Binding, 1u);
    EXPECT_EQ(Encoder.VertexCalls[0][1].View.Offset, 16u);
}

TEST_F(SubmitMeshDrawCommandsTest, SkipsZeroInstanceCommands) {
    FakePipeline other;
    auto skipped = MakeCommand();
    skipped.Pipeline = &other;
    const array commands{MakeCommand(), skipped, MakeCommand()};
    array visible{Dynamic(commands[0]), Dynamic(commands[1]), Dynamic(commands[2])};
    visible[1].InstanceCount = 0;
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    EXPECT_EQ(Encoder.Draws.size(), 2u);
    EXPECT_EQ(Encoder.Pipelines.size(), 1u);
}

TEST_F(SubmitMeshDrawCommandsTest, ReturnsFalseForNullCommandPointer) {
    const array commands{MakeCommand()};
    const array visible{Dynamic(commands[0]), VisibleMeshDrawCommand{}};
    EXPECT_FALSE(SubmitMeshDrawCommands(Encoder, visible));
}

TEST_F(SubmitMeshDrawCommandsTest, ReturnsFalseWhenPushConstantsFail) {
    auto command = MakeCommand();
    command.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{}, byte{}, byte{}, byte{}}};
    const array commands{command};
    const auto visible = Dynamic(commands[0]);
    Encoder.FailPushConstants = true;
    EXPECT_FALSE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}));
    EXPECT_TRUE(Encoder.Draws.empty());
}

TEST_F(SubmitMeshDrawCommandsTest, DebugPreflightRejectsInvalidListBeforeRecording) {
#if RADRAY_IS_DEBUG
    auto invalid = MakeCommand();
    invalid.IndexBuffer.Stride = 3;
    const array commands{MakeCommand(), invalid};
    const array visible{Dynamic(commands[0]), Dynamic(commands[1])};
    EXPECT_FALSE(SubmitMeshDrawCommands(Encoder, visible));
    EXPECT_TRUE(Encoder.Order.empty());
#else
    GTEST_SKIP() << "Release submission trusts validated commands";
#endif
}

}  // namespace
}  // namespace radray
