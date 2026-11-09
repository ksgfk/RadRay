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
    struct TestCommand : MeshDrawCommand {
        MeshDrawCommandBindings Bindings;
    };
    bool Validate(const TestCommand& command, const VisibleMeshDrawCommand& visible = {}, const MeshPassSubmitArgs& args = {}) {
        return ValidateMeshDrawCommand(command, command.Bindings, visible, args);
    }
    TestCommand MakeCommand() {
        TestCommand command;
        command.Pipeline = &Pipeline;
        command.Bindings.VertexBuffers.push_back({0, {&Vertices, 0, 48}});
        command.IndexBuffer = {&Indices, 0, 4};
        command.IndexCount = 3;
        return command;
    }
    // 在绑定槽 1 添加默认逐实例流，提交参数可以替换它。
    TestCommand MakeInstancedCommand() {
        auto command = MakeCommand();
        command.Bindings.VertexBuffers.push_back({1, {&Instances, 0, 64}});
        command.Bindings.InstanceStreamBinding = 1;
        return command;
    }

    FakePipeline Pipeline;
    FakeBuffer Vertices{128, render::BufferUse::Vertex};
    FakeBuffer Instances{128, render::BufferUse::Vertex};
    FakeBuffer Indices{128, render::BufferUse::Index};
    FakeParameterSet Parameters;
};

TEST_F(MeshDrawCommandTest, AcceptsCompleteCommand) {
    EXPECT_TRUE(Validate(MakeCommand()));
}

TEST_F(MeshDrawCommandTest, RejectsIndexRangePastBufferEnd) {
    auto command = MakeCommand();
    command.FirstIndex = 30;
    EXPECT_FALSE(Validate(command));
    command.FirstIndex = 29;
    EXPECT_TRUE(Validate(command));
}

TEST_F(MeshDrawCommandTest, RejectsIndexStrideOtherThanTwoOrFour) {
    auto command = MakeCommand();
    command.IndexBuffer.Stride = 3;
    EXPECT_FALSE(Validate(command));
}

TEST_F(MeshDrawCommandTest, RejectsIndexOffsetNotAlignedToStride) {
    auto command = MakeCommand();
    command.IndexBuffer.Offset = 2;
    EXPECT_FALSE(Validate(command));
}

TEST_F(MeshDrawCommandTest, RejectsInstanceRangePastUint32) {
    const VisibleMeshDrawCommand visible{.FirstInstance = std::numeric_limits<uint32_t>::max(), .InstanceCount = 2};
    EXPECT_FALSE(Validate(MakeCommand(), visible));
}

TEST_F(MeshDrawCommandTest, RejectsDuplicateVertexBinding) {
    auto command = MakeCommand();
    command.Bindings.VertexBuffers.push_back({0, {&Vertices, 48, 48}});
    EXPECT_FALSE(Validate(command));
}

TEST_F(MeshDrawCommandTest, RejectsDuplicateParameterGroup) {
    auto command = MakeCommand();
    command.Bindings.ParameterSets.push_back({1, &Parameters, {}});
    command.Bindings.ParameterSets.push_back({1, &Parameters, {}});
    EXPECT_FALSE(Validate(command));
}

TEST_F(MeshDrawCommandTest, RejectsCommandGroupAlsoUsedByPass) {
    auto command = MakeCommand();
    command.Bindings.ParameterSets.push_back({1, &Parameters, {}});
    const MeshShaderParameterBinding pass{1, &Parameters, {}};
    EXPECT_FALSE(Validate(command, {}, {.ParameterSets = std::span{&pass, 1}}));
}

TEST_F(MeshDrawCommandTest, RejectsPushConstantsNotFourByteAligned) {
    auto command = MakeCommand();
    command.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 2, {byte{}, byte{}, byte{}, byte{}}};
    EXPECT_FALSE(Validate(command));
    command.Bindings.PushConstants->Offset = 0;
    command.Bindings.PushConstants->Data.pop_back();
    EXPECT_FALSE(Validate(command));
}

TEST_F(MeshDrawCommandTest, DeclaredInstanceStreamMustBeSuppliedWhenMissing) {
    auto command = MakeCommand();
    command.Bindings.InstanceStreamBinding = 1;
    const MeshPassSubmitArgs args{.InstanceStream = render::VertexBufferBinding{1, {&Instances, 0, 64}}};
    EXPECT_FALSE(Validate(command));
    EXPECT_TRUE(Validate(command, {}, args));
}

TEST_F(MeshDrawCommandTest, RejectsInstanceStreamForUndeclaredSlot) {
    const MeshPassSubmitArgs args{.InstanceStream = render::VertexBufferBinding{0, {&Instances, 0, 64}}};
    EXPECT_FALSE(Validate(MakeCommand(), {}, args));
    EXPECT_FALSE(Validate(MakeInstancedCommand(), {}, args));
}

TEST_F(MeshDrawCommandTest, BindingStorageOwnsOverflowPayloads) {
    auto source = MakeCommand();
    source.Bindings.PushConstants = MeshPushConstantCommandBinding{3, 4, {}};
    for (uint32_t index = 0; index < 80; ++index) source.Bindings.PushConstants->Data.push_back(byte{0x37});
    source.Bindings.ParameterSets.push_back({1, &Parameters, {}});
    for (uint32_t index = 0; index < 6; ++index) source.Bindings.ParameterSets[0].DynamicOffsets.push_back({index, 0, index * 16});
    MeshDrawCommandBindingStorage storage;
    MeshDrawCommand command = source;
    storage.Store(command, source.Bindings);
    source.Bindings = {};
    const auto push = storage.GetPayload(command.PayloadIndex)->PushConstants;
    const auto& parameters = storage.GetPayload(command.PayloadIndex)->ParameterSets;
    ASSERT_TRUE(push);
    EXPECT_EQ(push->PushConstantId, 3u);
    EXPECT_EQ(push->Offset, 4u);
    EXPECT_EQ((vector<byte>{push->Data.begin(), push->Data.end()}), (vector<byte>(80, byte{0x37})));
    ASSERT_EQ(parameters[0].DynamicOffsets.size(), 6u);
    EXPECT_EQ(parameters[0].DynamicOffsets[5], (render::ShaderParameterDynamicOffset{5, 0, 80}));
    EXPECT_TRUE(ValidateMeshDrawCommand(command, storage));
}

TEST_F(MeshDrawCommandTest, InternedBindingsRemainAliveUntilLastOwnerIsDestroyed) {
    auto source = MakeCommand();
    source.Bindings.ParameterSets.push_back({1, &Parameters, {}});
    MeshDrawCommandBindingStorage storage;
    MeshDrawCommand first = source;
    MeshDrawCommand second = source;
    storage.Store(first, source.Bindings);
    storage.Store(second, source.Bindings);
    EXPECT_EQ(first.VertexBuffersIndex, second.VertexBuffersIndex);
    EXPECT_EQ(first.PayloadIndex, second.PayloadIndex);
    storage.Destroy(first);
    EXPECT_TRUE(ValidateMeshDrawCommand(second, storage));
    EXPECT_EQ(storage.GetPayloadCount(), 1u);
    storage.Destroy(second);
    EXPECT_EQ(storage.GetPayloadCount(), 0u);
    EXPECT_FALSE(storage.GetPayload(second.PayloadIndex));
    MeshDrawCommand replacement = source;
    storage.Store(replacement, source.Bindings);
    EXPECT_TRUE(ValidateMeshDrawCommand(replacement, storage));
}

TEST_F(MeshDrawCommandTest, EmptyBindingsAllocateNoPoolEntries) {
    MeshDrawCommand command;
    MeshDrawCommandBindingStorage storage;
    storage.Store(command, {});
    EXPECT_FALSE((command.PayloadIndex != kInvalidBindingIndex));
    EXPECT_TRUE(storage.GetPayloadCount() == 0);
}

class SubmitMeshDrawCommandsTest : public MeshDrawCommandTest {
protected:
    VisibleMeshDrawCommand Dynamic(const TestCommand& command) {
        auto& stored = Commands.emplace_back(static_cast<const MeshDrawCommand&>(command));
        Storage.Store(stored, command.Bindings);
        return {.CachedCommand = &stored};
    }

    MeshDrawCommandBindingStorage Storage;
    deque<MeshDrawCommand> Commands;
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
    command.Bindings.ParameterSets.push_back({1, &Parameters, {}});
    command.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{}, byte{}, byte{}, byte{}}};
    const array commands{command};
    const auto visible = Dynamic(commands[0]);
    FakeParameterSet passParameters;
    const MeshShaderParameterBinding pass{0, &passParameters, {}};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}, {.ParameterSets = std::span{&pass, 1}}));
    const vector<std::string_view> expected{"pipeline", "parameters", "vertex", "stencil", "parameters", "push", "index", "draw"};
    EXPECT_EQ(Encoder.Order, expected);
    ASSERT_EQ(Encoder.ParameterBinds.size(), 2u);
    EXPECT_EQ(Encoder.ParameterBinds[0].Set, &passParameters);
    EXPECT_EQ(Encoder.ParameterBinds[1].Set, &Parameters);
}

TEST_F(SubmitMeshDrawCommandsTest, BindsUnchangedStateOncePerCall) {
    auto command = MakeCommand();
    command.Bindings.ParameterSets.push_back({1, &Parameters, {}});
    command.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{}, byte{}, byte{}, byte{}}};
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

TEST_F(SubmitMeshDrawCommandsTest, VertexSlotsBindWithoutOverwritingUnchangedSlots) {
    auto first = MakeCommand();
    first.Bindings.VertexBuffers = {{2, {&Vertices, 0, 32}}, {0, {&Vertices, 32, 32}}, {1, {&Vertices, 64, 32}}};
    auto second = first;
    second.Bindings.VertexBuffers[0].View.Offset = 16;
    second.Bindings.VertexBuffers[1].View.Offset = 48;
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
    ASSERT_EQ(Encoder.VertexCalls.size(), 2u);
    ASSERT_EQ(Encoder.VertexCalls[0].size(), 1u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].Binding, 1u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].View.Offset, 16u);
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
    command.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{}, byte{}, byte{}, byte{}}};
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

TEST_F(SubmitMeshDrawCommandsTest, EqualHandlesFromDifferentPoolsDoNotSkipBindings) {
    auto source = MakeCommand();
    source.Bindings.ParameterSets.push_back({1, &Parameters, {{0, 0, 16}}});
    const auto first = Dynamic(source);
    source.Bindings.VertexBuffers[0].View.Offset = 16;
    source.Bindings.ParameterSets[0].DynamicOffsets[0].Offset = 32;
    MeshDrawCommandBindingStorage otherStorage;
    MeshDrawCommand other = source;
    otherStorage.Store(other, source.Bindings);
    ASSERT_EQ(first.CachedCommand->PayloadIndex, other.PayloadIndex);
    const array visible{first, VisibleMeshDrawCommand{.CachedCommand = &other}, first};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    ASSERT_EQ(Encoder.VertexCalls.size(), 3u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].View.Offset, 0u);
    EXPECT_EQ(Encoder.VertexCalls[1][0].View.Offset, 16u);
    EXPECT_EQ(Encoder.VertexCalls[2][0].View.Offset, 0u);
    ASSERT_EQ(Encoder.ParameterBinds.size(), 3u);
    EXPECT_EQ(Encoder.ParameterBinds[0].DynamicOffsets[0].Offset, 16u);
    EXPECT_EQ(Encoder.ParameterBinds[1].DynamicOffsets[0].Offset, 32u);
    EXPECT_EQ(Encoder.ParameterBinds[2].DynamicOffsets[0].Offset, 16u);
}

TEST_F(SubmitMeshDrawCommandsTest, SharedHandlesRebindAfterPipelineChangeAndAcrossCalls) {
    auto source = MakeInstancedCommand();
    source.Bindings.ParameterSets.push_back({1, &Parameters, {{0, 0, 16}}});
    const auto first = Dynamic(source);
    FakePipeline otherPipeline;
    auto other = *first.CachedCommand;
    other.Pipeline = &otherPipeline;
    const array visible{first, first, VisibleMeshDrawCommand{.CachedCommand = &other}, first};
    const MeshPassSubmitArgs args{.InstanceStream = render::VertexBufferBinding{1, {&Instances, 16, 48}}};
    for (uint32_t call = 0; call < 2; ++call) ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible, args));
    EXPECT_EQ(Encoder.Draws.size(), 8u);
    EXPECT_EQ(Encoder.Pipelines.size(), 6u);
    EXPECT_EQ(Encoder.ParameterBinds.size(), 6u);
    ASSERT_EQ(Encoder.VertexCalls.size(), 12u);
    for (size_t index = 0; index < Encoder.VertexCalls.size(); index += 2) {
        EXPECT_EQ(Encoder.VertexCalls[index][0].Binding, 1u);
        EXPECT_EQ(Encoder.VertexCalls[index][0].View.Offset, 16u);
    }
}

TEST_F(SubmitMeshDrawCommandsTest, WideVertexOffsetsAndHighSlotsRemainExact) {
    Vertices.Desc.Size = (uint64_t{1} << 32) + 128;
    auto source = MakeCommand();
    source.Bindings.VertexBuffers = {{31, {&Vertices, (uint64_t{1} << 32) + 16, 48}}};
    const auto first = Dynamic(source);
    source.Bindings.VertexBuffers[0].View.Offset += 16;
    const auto second = Dynamic(source);
    const array visible{first, first, second, first};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    ASSERT_EQ(Encoder.VertexCalls.size(), 3u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].Binding, 31u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].View.Offset, (uint64_t{1} << 32) + 16);
    EXPECT_EQ(Encoder.VertexCalls[1][0].View.Offset, (uint64_t{1} << 32) + 32);
    EXPECT_EQ(Encoder.VertexCalls[2][0].View.Offset, (uint64_t{1} << 32) + 16);
}

TEST_F(SubmitMeshDrawCommandsTest, ExtendedInstanceBindingDoesNotTruncate) {
    auto source = MakeCommand();
    source.Bindings.InstanceStreamBinding = 255;
    const auto visible = Dynamic(source);
    const MeshPassSubmitArgs args{.InstanceStream = render::VertexBufferBinding{255, {&Instances, 16, 48}}};
    EXPECT_TRUE(ValidateMeshDrawCommand(*visible.CachedCommand, Storage, visible, args));
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}, args));
    ASSERT_EQ(Encoder.VertexCalls.size(), 2u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].Binding, 255u);
    EXPECT_EQ(Encoder.VertexCalls[1][0].Binding, 0u);
}

TEST_F(SubmitMeshDrawCommandsTest, EmptyElementMaskDoesNotBindOrDraw) {
    auto visible = Dynamic(MakeCommand());
    visible.ElementMask = 0;
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}));
    EXPECT_TRUE(Encoder.Order.empty());
}

TEST_F(MeshDrawCommandTest, CachedListMoveRebindsPayloadIdentityAndCanBeReused) {
    auto source = MakeCommand();
    source.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{1}, byte{2}, byte{3}, byte{4}}};
    vector<CachedPassMeshDrawList> lists(1);
    PrimitiveSceneInfo primitive;
    MeshBatch batch;
    batch.Elements.push_back({.IndexBuffer = &Indices, .IndexCount = 3});
    const auto add = [&](CachedPassMeshDrawList& list, uint32_t index) {
        CachedPassMeshDrawListContext context{list, index};
        MeshDrawCommand initializer = source;
        context.GetBindingStorage().Store(initializer, source.Bindings);
        context.AddCommand({primitive, batch}, 0, initializer);
    };
    add(lists[0], 0);
    add(lists[0], 1);
    lists.resize(16);
    CachedPassMeshDrawList moved;
    moved = std::move(lists[0]);
    const auto& command = *moved.GetCommand(0);
    EXPECT_EQ(command.BindingStorage.Get(), &moved.GetBindings());
    EXPECT_TRUE(ValidateMeshDrawCommand(command, moved.GetBindings()));
    const VisibleMeshDrawCommand visible{.CachedCommand = &command};
    RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, std::span{&visible, 1}));
    ASSERT_EQ(encoder.Pushes.size(), 1u);
    EXPECT_EQ(encoder.Pushes[0], (vector<byte>{byte{1}, byte{2}, byte{3}, byte{4}}));
    add(lists[0], 0);
    EXPECT_TRUE(ValidateMeshDrawCommand(*lists[0].GetCommand(0), lists[0].GetBindings()));
}

TEST_F(MeshDrawCommandTest, RejectsMismatchedElementMaskAndPayload) {
    auto source = MakeCommand();
    source.ValidElementMask = 5;
    EXPECT_FALSE(Validate(source));
    source.Bindings.ExtraElements.push_back({{&Indices, 0, 4}, 3, 3, 0, 1});
    EXPECT_FALSE(Validate(source));
    source.Bindings.ExtraElements[0].ElementIndex = 2;
    EXPECT_TRUE(Validate(source));
    source.Bindings.ExtraElements[0].FirstIndex = 31;
    EXPECT_FALSE(Validate(source));
}

TEST_F(SubmitMeshDrawCommandsTest, VertexBindingGapsSplitOnlyAtMissingSlots) {
    auto source = MakeCommand();
    source.Bindings.VertexBuffers = {{3, {&Vertices, 64, 32}}, {0, {&Vertices, 0, 32}}, {2, {&Vertices, 32, 32}}};
    const auto visible = Dynamic(source);
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}));
    ASSERT_EQ(Encoder.VertexCalls.size(), 2u);
    ASSERT_EQ(Encoder.VertexCalls[0].size(), 1u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].Binding, 0u);
    ASSERT_EQ(Encoder.VertexCalls[1].size(), 2u);
    EXPECT_EQ(Encoder.VertexCalls[1][0].Binding, 2u);
    EXPECT_EQ(Encoder.VertexCalls[1][1].Binding, 3u);
}

TEST_F(SubmitMeshDrawCommandsTest, InstanceOverrideSplitsAContiguousGroupWithoutRebindingIt) {
    auto source = MakeCommand();
    source.Bindings.VertexBuffers = {{0, {&Vertices, 0, 32}}, {1, {&Instances, 0, 32}}, {2, {&Vertices, 32, 32}}, {3, {&Vertices, 64, 32}}};
    source.Bindings.InstanceStreamBinding = 1;
    const auto first = Dynamic(source);
    auto second = *first.CachedCommand;
    second.FirstIndex = 3;
    const array visible{first, VisibleMeshDrawCommand{.CachedCommand = &second}};
    const MeshPassSubmitArgs args{.InstanceStream = render::VertexBufferBinding{1, {&Instances, 16, 48}}};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible, args));
    ASSERT_EQ(Encoder.VertexCalls.size(), 3u);
    ASSERT_EQ(Encoder.VertexCalls[0].size(), 1u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].Binding, 1u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].View.Offset, 16u);
    ASSERT_EQ(Encoder.VertexCalls[1].size(), 1u);
    EXPECT_EQ(Encoder.VertexCalls[1][0].Binding, 0u);
    ASSERT_EQ(Encoder.VertexCalls[2].size(), 2u);
    EXPECT_EQ(Encoder.VertexCalls[2][0].Binding, 2u);
    EXPECT_EQ(Encoder.VertexCalls[2][1].Binding, 3u);
    ASSERT_EQ(Encoder.Draws.size(), 2u);
    EXPECT_EQ(Encoder.Draws[1].FirstIndex, 3u);
}

TEST_F(SubmitMeshDrawCommandsTest, WholeVertexGroupsFromDifferentPoolsDoNotAlias) {
    auto source = MakeCommand();
    source.Bindings.VertexBuffers = {{0, {&Vertices, 0, 32}}, {1, {&Vertices, 32, 32}}, {2, {&Vertices, 64, 32}}};
    const auto first = Dynamic(source);
    for (auto& binding : source.Bindings.VertexBuffers) binding.View.Offset += 16;
    MeshDrawCommandBindingStorage otherStorage;
    MeshDrawCommand other = source;
    otherStorage.Store(other, source.Bindings);
    ASSERT_EQ(first.CachedCommand->VertexBuffersIndex, other.VertexBuffersIndex);
    const array visible{first, VisibleMeshDrawCommand{.CachedCommand = &other}, first};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    ASSERT_EQ(Encoder.VertexCalls.size(), 3u);
    for (const auto& call : Encoder.VertexCalls) ASSERT_EQ(call.size(), 3u);
    EXPECT_EQ(Encoder.VertexCalls[0][0].View.Offset, 0u);
    EXPECT_EQ(Encoder.VertexCalls[1][0].View.Offset, 16u);
    EXPECT_EQ(Encoder.VertexCalls[2][0].View.Offset, 0u);
}

TEST_F(SubmitMeshDrawCommandsTest, PipelineChangeRebindsWholeContiguousVertexGroup) {
    auto source = MakeCommand();
    source.Bindings.VertexBuffers = {{0, {&Vertices, 0, 32}}, {1, {&Vertices, 32, 32}}, {2, {&Vertices, 64, 32}}};
    const auto first = Dynamic(source);
    FakePipeline secondPipeline;
    auto second = *first.CachedCommand;
    second.Pipeline = &secondPipeline;
    const array visible{first, first, VisibleMeshDrawCommand{.CachedCommand = &second}, first};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    ASSERT_EQ(Encoder.VertexCalls.size(), 3u);
    for (const auto& call : Encoder.VertexCalls) ASSERT_EQ(call.size(), 3u);
    EXPECT_EQ(Encoder.Draws.size(), 4u);
}

TEST_F(SubmitMeshDrawCommandsTest, VisibleInstanceCountKeepsAll32Bits) {
    auto visible = Dynamic(MakeCommand());
    visible.InstanceCount = (uint32_t{1} << 24) + 3;
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}));
    ASSERT_EQ(Encoder.Draws.size(), 1u);
    EXPECT_EQ(Encoder.Draws[0].InstanceCount, visible.InstanceCount);
}

TEST_F(MeshDrawCommandTest, InterningDistinguishesFullViewsOffsetsAndPushConstantBytes) {
    MeshDrawCommandBindingStorage storage;
    auto source = MakeCommand();
    source.Bindings.ParameterSets.push_back({1, &Parameters, {{0, 0, 16}}});
    source.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{1}, byte{2}, byte{3}, byte{4}}};
    MeshDrawCommand original = source;
    storage.Store(original, source.Bindings);
    source.Bindings.ParameterSets[0].DynamicOffsets[0].Offset = 32;
    MeshDrawCommand changedOffset = source;
    storage.Store(changedOffset, source.Bindings);
    EXPECT_EQ(original.VertexBuffersIndex, changedOffset.VertexBuffersIndex);
    EXPECT_NE(original.PayloadIndex, changedOffset.PayloadIndex);
    source.Bindings.ParameterSets[0].DynamicOffsets[0].Offset = 16;
    source.Bindings.PushConstants->Data[3] = byte{5};
    source.Bindings.VertexBuffers[0].View.Offset = uint64_t{1} << 32;
    MeshDrawCommand changedBytes = source;
    storage.Store(changedBytes, source.Bindings);
    EXPECT_NE(original.VertexBuffersIndex, changedBytes.VertexBuffersIndex);
    EXPECT_NE(original.PayloadIndex, changedBytes.PayloadIndex);
    storage.Destroy(changedOffset);
    storage.Destroy(changedBytes);
    EXPECT_TRUE(ValidateMeshDrawCommand(original, storage));
}

TEST_F(MeshDrawCommandTest, PipelineAndPayloadBucketOverflowDoesNotContaminateAdjacentFields) {
    MeshDrawCommandBindingStorage storage;
    vector<FakePipeline> pipelines(260);
    for (auto& pipeline : pipelines) {
        MeshDrawCommand command;
        command.Pipeline = &pipeline;
        const auto key = storage.GetStateSortKey(command);
        EXPECT_NE(key >> 24, 0u);
        EXPECT_EQ(key & 0x00ffffff, 0u);
    }
    for (uint32_t i = 0; i < 4100; ++i) {
        MeshDrawCommand command;
        command.Pipeline = &pipelines[0];
        MeshDrawCommandBindings bindings;
        bindings.ParameterSets.push_back({i, &Parameters, {}});
        storage.Store(command, bindings);
        const auto key = storage.GetStateSortKey(command);
        EXPECT_EQ(key >> 24, 1u);
        EXPECT_NE((key >> 12) & 4095, 0u);
        EXPECT_EQ(key & 4095, 0u);
    }
}

}  // 匿名命名空间
}  // namespace radray
