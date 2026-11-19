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
        Input.Layouts.push_back({0, 12, gpu::VertexStepMode::Vertex});
        Input.Attributes.push_back({0, 0, "POSITION", 0, gpu::VertexFormat::FLOAT32X3});
        Input.Streams.push_back({&Vertices, 0, 48, 0});
    }
    void AddInstanceLayout(uint32_t binding, uint32_t location) {
        Input.Layouts.push_back({binding, 16, gpu::VertexStepMode::Instance});
        Input.Attributes.push_back({binding, 0, "INSTANCE", location, gpu::VertexFormat::FLOAT32X4});
    }

    FakeBuffer Vertices{128, gpu::BufferUse::Vertex};
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
    FakeBuffer Vertices{128, gpu::BufferUse::Vertex};
    FakeBuffer Instances{128, gpu::BufferUse::Vertex};
    FakeBuffer Indices{128, gpu::BufferUse::Index};
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
    const MeshPassSubmitArgs args{.InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 0, 64}}};
    EXPECT_FALSE(Validate(command));
    EXPECT_TRUE(Validate(command, {}, args));
}

TEST_F(MeshDrawCommandTest, RejectsInstanceStreamForUndeclaredSlot) {
    const MeshPassSubmitArgs args{.InstanceStream = gpu::VertexBufferBinding{0, {&Instances, 0, 64}}};
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
    EXPECT_EQ(parameters[0].DynamicOffsets[5], (gpu::ShaderParameterDynamicOffset{5, 0, 80}));
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
    EXPECT_EQ(Encoder.Pushes.size(), 1u);
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
    EXPECT_EQ(Encoder.Pipelines, (vector<gpu::GraphicsPipelineState*>{&Pipeline, &other}));
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
    const MeshPassSubmitArgs args{.InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 16, 48}}};
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
    const MeshPassSubmitArgs args{.InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 16, 48}}};
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
    const MeshPassSubmitArgs args{.InstanceStream = gpu::VertexBufferBinding{255, {&Instances, 16, 48}}};
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
    const MeshPassSubmitArgs args{.InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 16, 48}}};
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
    vector<test::FakePipelineLayout> layouts(20);
    vector<FakePipeline> pipelines(520);
    for (size_t i = 0; i < layouts.size(); ++i) {
        pipelines[i].Layout = &layouts[i];
    }
    for (size_t i = 0; i < pipelines.size(); ++i) {
        MeshDrawCommand command;
        command.Pipeline = &pipelines[i];
        const auto key = storage.GetStateSortKey(command);
        if (i < layouts.size()) {
            EXPECT_NE(key >> 28, 0u);
        } else {
            EXPECT_EQ(key >> 28, 0u);
        }
        EXPECT_NE((key >> 19) & 0x1ffu, 0u);
        EXPECT_EQ(key & 0x7ffffu, 0u);
    }
    const auto basePipelineBits = storage.GetStateSortKey(MeshDrawCommand{.Pipeline = &pipelines[0]}) & 0xfff80000u;
    for (uint32_t i = 0; i < 600; ++i) {
        MeshDrawCommand command;
        command.Pipeline = &pipelines[0];
        MeshDrawCommandBindings bindings;
        bindings.ParameterSets.push_back({i, &Parameters, {}});
        storage.Store(command, bindings);
        const auto key = storage.GetStateSortKey(command);
        EXPECT_EQ(key & 0xfff80000u, basePipelineBits);
        EXPECT_NE((key >> 10) & 0x1ffu, 0u);
        EXPECT_EQ(key & 0x3ffu, 0u);
    }
    for (uint32_t i = 0; i < 300; ++i) {
        MeshDrawCommand command;
        command.Pipeline = &pipelines[0];
        command.IndexBuffer = {&Indices, i * 4u, 4u};
        command.StencilReference = static_cast<uint8_t>(i & 0xffu);
        const auto key = storage.GetStateSortKey(command);
        EXPECT_EQ(key & 0xfffffc00u, basePipelineBits);
        EXPECT_NE((key >> 2) & 0xffu, 0u);
        if (command.StencilReference == 0) {
            EXPECT_EQ(key & 0x3u, 0u);
        } else {
            EXPECT_NE(key & 0x3u, 0u);
        }
    }
}

TEST_F(MeshDrawCommandTest, StateSortKeyIsConsistentAcrossPoolsAndClustersLayoutIbStencil) {
    test::FakePipelineLayout layoutA;
    FakePipeline psoA1{&layoutA};
    FakePipeline psoA2{&layoutA};
    FakeBuffer indicesB{128, gpu::BufferUse::Index};

    MeshDrawCommandBindingStorage cachedStorage;
    MeshDrawCommandBindingStorage dynamicStorage;
    // 在两个池里先插入不同数量的干扰项，使相同内容的局部索引不同。
    {
        auto noise = MakeCommand();
        noise.Bindings.ParameterSets.push_back({3, &Parameters, {}});
        MeshDrawCommand noiseCmd = noise;
        dynamicStorage.Store(noiseCmd, noise.Bindings);
    }

    auto makeShared = [&](gpu::GraphicsPipelineState* pso, gpu::IndexBufferView ib, uint8_t stencil) {
        auto cmd = MakeCommand();
        cmd.Pipeline = pso;
        cmd.IndexBuffer = ib;
        cmd.StencilReference = stencil;
        cmd.Bindings.ParameterSets.push_back({1, &Parameters, {{0, 0, 16}}});
        cmd.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{1}, byte{2}, byte{3}, byte{4}}};
        return cmd;
    };

    auto srcCached = makeShared(&psoA1, {&Indices, 0, 4}, 2);
    auto srcDynamic = makeShared(&psoA1, {&Indices, 0, 4}, 2);
    MeshDrawCommand cachedCmd = srcCached;
    MeshDrawCommand dynamicCmd = srcDynamic;
    cachedStorage.Store(cachedCmd, srcCached.Bindings);
    dynamicStorage.Store(dynamicCmd, srcDynamic.Bindings);
    ASSERT_NE(cachedCmd.PayloadIndex, dynamicCmd.PayloadIndex);
    EXPECT_EQ(cachedStorage.GetStateSortKey(cachedCmd), dynamicStorage.GetStateSortKey(dynamicCmd));

    // 同 Layout 的不同 PSO 共享高 4 位 Layout 桶。
    auto srcA2 = makeShared(&psoA2, {&Indices, 0, 4}, 2);
    MeshDrawCommand cmdA2 = srcA2;
    cachedStorage.Store(cmdA2, srcA2.Bindings);
    const auto keyA1 = cachedStorage.GetStateSortKey(cachedCmd);
    const auto keyA2 = cachedStorage.GetStateSortKey(cmdA2);
    EXPECT_EQ(keyA1 >> 28, keyA2 >> 28);
    EXPECT_NE(keyA1 >> 28, 0u);

    // 仅 IB 变化时只影响 Geometry 位域（bits 2..9），不影响 Layout/Pipeline/Payload/Stencil。
    auto srcOtherIb = makeShared(&psoA1, {&indicesB, 0, 4}, 2);
    MeshDrawCommand cmdOtherIb = srcOtherIb;
    cachedStorage.Store(cmdOtherIb, srcOtherIb.Bindings);
    auto srcNoIb = makeShared(&psoA1, {nullptr, 0, 0}, 2);
    MeshDrawCommand cmdNoIb = srcNoIb;
    cachedStorage.Store(cmdNoIb, srcNoIb.Bindings);
    const auto keyOtherIb = cachedStorage.GetStateSortKey(cmdOtherIb);
    const auto keyNoIb = cachedStorage.GetStateSortKey(cmdNoIb);
    EXPECT_EQ(keyA1 & 0xfffffc03u, keyOtherIb & 0xfffffc03u);
    EXPECT_EQ(keyA1 & 0xfffffc03u, keyNoIb & 0xfffffc03u);
    EXPECT_NE((keyA1 >> 2) & 0xffu, (keyNoIb >> 2) & 0xffu);

    // 仅 StencilReference 变化（0..3）精确落在低 2 位且不污染高位。
    for (uint8_t s = 0; s < 4; ++s) {
        auto srcS = makeShared(&psoA1, {&Indices, 0, 4}, s);
        MeshDrawCommand cmdS = srcS;
        cachedStorage.Store(cmdS, srcS.Bindings);
        const auto keyS = cachedStorage.GetStateSortKey(cmdS);
        EXPECT_EQ(keyS & 0xfffffffcu, keyA1 & 0xfffffffcu);
        EXPECT_EQ(keyS & 0x3u, s);
    }
}

TEST_F(MeshDrawCommandTest, NonIndexedSharedBufferWithDifferentStridesInternsSeparatelyAndValidatesInstanceBounds) {
    MeshDrawCommandBindingStorage storage;
    TestCommand source16;
    source16.Pipeline = &Pipeline;
    source16.IndexBuffer = {nullptr, 0, 0};
    source16.IndexCount = 3;
    source16.Bindings.VertexBuffers = {{0, {&Vertices, 0, 48}}, {1, {&Instances, 0, 64}}};
    source16.Bindings.VertexStreamLayouts = {
        {0, 12, gpu::VertexStepMode::Vertex},
        {1, 16, gpu::VertexStepMode::Instance}};
    source16.Bindings.InstanceStreamBinding = 1;

    TestCommand source32 = source16;
    source32.Bindings.VertexStreamLayouts[1].ArrayStride = 32;

    MeshDrawCommand cmd16 = source16;
    MeshDrawCommand cmd32 = source32;
    storage.Store(cmd16, source16.Bindings);
    storage.Store(cmd32, source32.Bindings);

    EXPECT_NE(cmd16.VertexBuffersIndex, cmd32.VertexBuffersIndex);

    // 64 B 实例流：绘制第 3 个实例（FirstInstance=2, InstanceCount=1 或 InstanceCount=3）：
    // stride 16 需要 48 B <= 64 B（放行），stride 32 需要 96 B > 64 B（拒绝）。
    const VisibleMeshDrawCommand thirdInstance{.CachedCommand = &cmd16, .FirstInstance = 2, .InstanceCount = 1};
    const VisibleMeshDrawCommand thirdInstance32{.CachedCommand = &cmd32, .FirstInstance = 2, .InstanceCount = 1};
    EXPECT_TRUE(ValidateMeshDrawCommand(cmd16, storage, thirdInstance));
    EXPECT_FALSE(ValidateMeshDrawCommand(cmd32, storage, thirdInstance32));

    const VisibleMeshDrawCommand threeInstances16{.CachedCommand = &cmd16, .FirstInstance = 0, .InstanceCount = 3};
    const VisibleMeshDrawCommand threeInstances32{.CachedCommand = &cmd32, .FirstInstance = 0, .InstanceCount = 3};
    EXPECT_TRUE(ValidateMeshDrawCommand(cmd16, storage, threeInstances16));
    EXPECT_FALSE(ValidateMeshDrawCommand(cmd32, storage, threeInstances32));

    RecordingEncoder encoder16;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder16, std::span{&thirdInstance, 1}));
    ASSERT_EQ(encoder16.NonIndexedDraws.size(), 1u);
    EXPECT_EQ(encoder16.NonIndexedDraws[0].VertexCount, 3u);
    EXPECT_EQ(encoder16.NonIndexedDraws[0].FirstInstance, 2u);
    EXPECT_TRUE(encoder16.IndexBuffers.empty());

#if RADRAY_IS_DEBUG
    RecordingEncoder encoder32;
    EXPECT_FALSE(SubmitMeshDrawCommands(encoder32, std::span{&thirdInstance32, 1}));
    EXPECT_TRUE(encoder32.Order.empty());
#endif
}

TEST_F(SubmitMeshDrawCommandsTest, NonIndexedInstanceStreamOobRejectedBeforeRecording) {
    TestCommand source;
    source.Pipeline = &Pipeline;
    source.IndexBuffer = {nullptr, 0, 0};
    source.IndexCount = 3;
    source.Bindings.VertexBuffers = {{0, {&Vertices, 0, 48}}};
    source.Bindings.VertexStreamLayouts = {
        {0, 12, gpu::VertexStepMode::Vertex},
        {1, 16, gpu::VertexStepMode::Instance}};
    source.Bindings.InstanceStreamBinding = 1;

    auto visible = Dynamic(source);
    visible.FirstInstance = 1;
    visible.InstanceCount = 1;
    // 实例流仅容纳 1 条 16 B 记录，FirstInstance=1, InstanceCount=1 越界。
    const MeshPassSubmitArgs smallInstanceArgs{.InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 0, 16}}};
    EXPECT_FALSE(ValidateMeshDrawCommand(*visible.CachedCommand, Storage, visible, smallInstanceArgs));
#if RADRAY_IS_DEBUG
    EXPECT_FALSE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}, smallInstanceArgs));
    EXPECT_TRUE(Encoder.Order.empty());
#endif
    const MeshPassSubmitArgs sufficientInstanceArgs{.InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 0, 32}}};
    EXPECT_TRUE(ValidateMeshDrawCommand(*visible.CachedCommand, Storage, visible, sufficientInstanceArgs));
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, std::span{&visible, 1}, sufficientInstanceArgs));
    ASSERT_EQ(Encoder.NonIndexedDraws.size(), 1u);
    EXPECT_EQ(Encoder.NonIndexedDraws[0].FirstInstance, 1u);
}

TEST_F(SubmitMeshDrawCommandsTest, NonIndexedDrawLeavesIndexBufferStateCacheIntact) {
    auto indexed = MakeCommand();
    TestCommand nonIndexed;
    nonIndexed.Pipeline = &Pipeline;
    nonIndexed.IndexBuffer = {nullptr, 0, 0};
    nonIndexed.IndexCount = 3;
    nonIndexed.FirstIndex = 1;
    nonIndexed.Bindings.VertexBuffers = {{0, {&Vertices, 0, 48}}};
    nonIndexed.Bindings.VertexStreamLayouts = {{0, 12, gpu::VertexStepMode::Vertex}};

    const auto v0 = Dynamic(indexed);
    const auto v1 = Dynamic(nonIndexed);
    const auto v2 = Dynamic(indexed);
    const array visible{v0, v1, v2};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    EXPECT_EQ(Encoder.IndexBuffers.size(), 1u);
    EXPECT_EQ(Encoder.Draws.size(), 2u);
    ASSERT_EQ(Encoder.NonIndexedDraws.size(), 1u);
    EXPECT_EQ(Encoder.NonIndexedDraws[0].VertexCount, 3u);
    EXPECT_EQ(Encoder.NonIndexedDraws[0].FirstVertex, 1u);
}

TEST_F(SubmitMeshDrawCommandsTest, IndirectCountAndBuildMatchContiguousMergeAndInstanceCountOverride) {
    TestCommand source = MakeCommand();
    source.ValidElementMask = 7;
    source.Bindings.ExtraElements = {
        {{&Indices, 0, 4}, 3, 3, 0, 1},
        {{&Indices, 0, 4}, 3, 6, 0, 2}};
    source.Bindings.ContiguousMergeMask = 3;
    source.Bindings.AllElementsSharePrimaryIndexBuffer = true;

    auto visible = Dynamic(source);
    visible.InstanceCount = 1;
    visible.UseElementInstanceCount = false;

    MeshPassIndirectDrawCounts counts{};
    PreparedMeshPassIndirectDraws prepared;
    ASSERT_TRUE(CountMeshPassIndirectDraws(std::span{&visible, 1}, counts));
    ASSERT_TRUE(BuildMeshPassIndirectDraws(std::span{&visible, 1}, prepared));
    EXPECT_EQ(counts.IndexedArgumentCount, 1u);
    EXPECT_EQ(counts.NonIndexedArgumentCount, 0u);
    ASSERT_EQ(prepared.Batches.size(), 1u);
    ASSERT_EQ(prepared.IndexedArguments.size(), 1u);
    EXPECT_EQ(prepared.IndexedArguments[0].IndexCount, 9u);
    EXPECT_EQ(prepared.IndexedArguments[0].InstanceCount, 1u);

    visible.InstanceCount = 2;
    ASSERT_TRUE(CountMeshPassIndirectDraws(std::span{&visible, 1}, counts));
    ASSERT_TRUE(BuildMeshPassIndirectDraws(std::span{&visible, 1}, prepared));
    EXPECT_EQ(counts.IndexedArgumentCount, 3u);
    EXPECT_EQ(counts.NonIndexedArgumentCount, 0u);
    ASSERT_EQ(prepared.Batches.size(), 1u);
    EXPECT_EQ(prepared.Batches[0].ArgumentCount, 3u);
    ASSERT_EQ(prepared.IndexedArguments.size(), 3u);
    for (uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(prepared.IndexedArguments[i].IndexCount, 3u);
        EXPECT_EQ(prepared.IndexedArguments[i].FirstIndex, i * 3);
        EXPECT_EQ(prepared.IndexedArguments[i].InstanceCount, 2u);
    }
}

TEST_F(SubmitMeshDrawCommandsTest, IndirectCountAndBuildRejectNullAndMalformedElementsAndAcceptZeroDraws) {
    MeshPassIndirectDrawCounts counts{99, 99};
    PreparedMeshPassIndirectDraws prepared;
    EXPECT_TRUE(CountMeshPassIndirectDraws({}, counts));
    EXPECT_EQ(counts.IndexedArgumentCount, 0u);
    EXPECT_EQ(counts.NonIndexedArgumentCount, 0u);
    EXPECT_TRUE(BuildMeshPassIndirectDraws({}, prepared));
    EXPECT_TRUE(prepared.Batches.empty());

    auto zeroInst = Dynamic(MakeCommand());
    zeroInst.InstanceCount = 0;
    EXPECT_TRUE(CountMeshPassIndirectDraws(std::span{&zeroInst, 1}, counts));
    EXPECT_EQ(counts.IndexedArgumentCount, 0u);
    EXPECT_TRUE(BuildMeshPassIndirectDraws(std::span{&zeroInst, 1}, prepared));
    EXPECT_TRUE(prepared.Batches.empty());

    // 默认构造（InstanceCount=1, CachedCommand=nullptr）须返回 false。
    const VisibleMeshDrawCommand nullVisible{};
    EXPECT_FALSE(CountMeshPassIndirectDraws(std::span{&nullVisible, 1}, counts));
    EXPECT_FALSE(BuildMeshPassIndirectDraws(std::span{&nullVisible, 1}, prepared));

    // ValidElementMask = 0b0101 但缺失 ExtraElements 须在展开前拒绝。
    auto badMask = MakeCommand();
    badMask.ValidElementMask = 5;
    const auto badVisible = Dynamic(badMask);
    EXPECT_FALSE(CountMeshPassIndirectDraws(std::span{&badVisible, 1}, counts));
    EXPECT_FALSE(BuildMeshPassIndirectDraws(std::span{&badVisible, 1}, prepared));
}

TEST_F(SubmitMeshDrawCommandsTest, IndirectBuildMergesExtraElementsAcrossUnboundStorages) {
    auto source = MakeCommand();
    source.Bindings.VertexBuffers.clear();
    source.ValidElementMask = 3;
    source.Bindings.ExtraElements.push_back({source.IndexBuffer, 3, 3, 0, 1});
    const auto first = Dynamic(source);
    MeshDrawCommandBindingStorage otherStorage;
    MeshDrawCommand other = source;
    otherStorage.Store(other, source.Bindings);
    ASSERT_NE(first.CachedCommand->BindingStorage.Get(), other.BindingStorage.Get());
    ASSERT_EQ(other.VertexBuffersIndex, kInvalidBindingIndex);
    ASSERT_EQ(other.PayloadIndex, kInvalidBindingIndex);
    const array visible{first, VisibleMeshDrawCommand{.CachedCommand = &other}};
    PreparedMeshPassIndirectDraws prepared;
    ASSERT_TRUE(BuildMeshPassIndirectDraws(visible, prepared));
    ASSERT_EQ(prepared.Batches.size(), 1u);
    EXPECT_EQ(prepared.Batches[0].ArgumentCount, 4u);
}

TEST_F(SubmitMeshDrawCommandsTest, IndirectBuildIsolatesCrossPoolCommandsInOrder) {
    auto source = MakeCommand();
    source.Bindings.ParameterSets.push_back({1, &Parameters, {{0, 0, 16}}});
    const auto poolA = Dynamic(source);

    source.Bindings.VertexBuffers[0].View.Offset = 16;
    source.Bindings.ParameterSets[0].DynamicOffsets[0].Offset = 32;
    MeshDrawCommandBindingStorage otherStorage;
    MeshDrawCommand poolBCommand = source;
    otherStorage.Store(poolBCommand, source.Bindings);
    ASSERT_EQ(poolA.CachedCommand->VertexBuffersIndex, poolBCommand.VertexBuffersIndex);
    ASSERT_EQ(poolA.CachedCommand->PayloadIndex, poolBCommand.PayloadIndex);

    const array visible{poolA, VisibleMeshDrawCommand{.CachedCommand = &poolBCommand}, poolA};
    PreparedMeshPassIndirectDraws prepared;
    ASSERT_TRUE(BuildMeshPassIndirectDraws(visible, prepared));
    ASSERT_EQ(prepared.Batches.size(), 3u);
    EXPECT_EQ(prepared.Batches[0].CommandIndex, 0u);
    EXPECT_EQ(prepared.Batches[1].CommandIndex, 1u);
    EXPECT_EQ(prepared.Batches[2].CommandIndex, 2u);
    EXPECT_EQ(prepared.IndexedArguments.size(), 3u);
}

TEST_F(SubmitMeshDrawCommandsTest, IndirectSubmitPreflightRejectsBeforeRecordingAndReportsMidRecordPushFailure) {
    auto indexed = MakeCommand();
    indexed.Bindings.InstanceStreamBinding = 1;
    indexed.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{1}, byte{2}, byte{3}, byte{4}}};

    TestCommand nonIndexed;
    nonIndexed.Pipeline = &Pipeline;
    nonIndexed.IndexBuffer = {nullptr, 0, 0};
    nonIndexed.IndexCount = 3;
    nonIndexed.Bindings.VertexBuffers = {{0, {&Vertices, 0, 48}}};
    nonIndexed.Bindings.VertexStreamLayouts = {
        {0, 12, gpu::VertexStepMode::Vertex},
        {1, 16, gpu::VertexStepMode::Instance}};
    nonIndexed.Bindings.InstanceStreamBinding = 1;
    nonIndexed.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{5}, byte{6}, byte{7}, byte{8}}};

    const auto v0 = Dynamic(indexed);
    auto v1 = Dynamic(nonIndexed);
    const array visible{v0, v1};
    PreparedMeshPassIndirectDraws prepared;
    ASSERT_TRUE(BuildMeshPassIndirectDraws(visible, prepared));
    ASSERT_EQ(prepared.Batches.size(), 2u);

    FakeBuffer indirectBuf{256, gpu::BufferUse::Indirect};
    FakeBuffer wrongUsageBuf{256, gpu::BufferUse::Vertex};
    const RDGBufferSlice validIndexed{&indirectBuf, 0, sizeof(gpu::DrawIndexedIndirectArguments)};
    const RDGBufferSlice validNonIndexed{&indirectBuf, 64, sizeof(gpu::DrawIndirectArguments)};
    const MeshPassSubmitArgs validArgs{.InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 0, 32}}};

    gpu::GpuDeviceCapabilities caps{};
    caps.Features.IndirectDraw = true;
    caps.Features.IndirectFirstInstance = true;
    caps.Limits.MaxDrawIndirectCount = 16;

    // 1. 截断的 visibleCommands span -> 0 绑定 0 绘制。
    {
        RecordingEncoder enc;
        EXPECT_FALSE(SubmitMeshPassIndirectDraws(enc, std::span{visible}.first(1), prepared, validIndexed, validNonIndexed, caps, validArgs));
        EXPECT_TRUE(enc.Order.empty());
    }
    // 2. 回退批次 CPU 参数数组少 1 条 -> 0 绑定 0 绘制。
    {
        auto shortCpu = prepared;
        shortCpu.NonIndexedArguments.clear();
        auto fallbackCaps = caps;
        fallbackCaps.Features.IndirectDraw = false;
        RecordingEncoder enc;
        EXPECT_FALSE(SubmitMeshPassIndirectDraws(enc, visible, shortCpu, {}, {}, fallbackCaps, validArgs));
        EXPECT_TRUE(enc.Order.empty());
    }
    // 3. 第 1 批有效而第 2 批无索引切片少 1 条或缺少 Indirect 用途 -> 0 绑定 0 绘制。
    {
        RecordingEncoder encShort;
        const RDGBufferSlice shortNonIndexed{&indirectBuf, 64, sizeof(gpu::DrawIndirectArguments) - 1};
        EXPECT_FALSE(SubmitMeshPassIndirectDraws(encShort, visible, prepared, validIndexed, shortNonIndexed, caps, validArgs));
        EXPECT_TRUE(encShort.Order.empty());

        RecordingEncoder encWrongUsage;
        const RDGBufferSlice badUsageNonIndexed{&wrongUsageBuf, 0, sizeof(gpu::DrawIndirectArguments)};
        EXPECT_FALSE(SubmitMeshPassIndirectDraws(encWrongUsage, visible, prepared, validIndexed, badUsageNonIndexed, caps, validArgs));
        EXPECT_TRUE(encWrongUsage.Order.empty());
    }
    // 4. 无索引实例流越界（仅容纳 1 条记录，FirstInstance=1, InstanceCount=1）在 MDI 路径被前置拒绝。
#if RADRAY_IS_DEBUG
    {
        auto oobVisible = visible;
        oobVisible[1].FirstInstance = 1;
        oobVisible[1].InstanceCount = 1;
        PreparedMeshPassIndirectDraws oobPrepared;
        ASSERT_TRUE(BuildMeshPassIndirectDraws(oobVisible, oobPrepared));
        const MeshPassSubmitArgs smallInstArgs{.InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 0, 16}}};
        RecordingEncoder encOob;
        EXPECT_FALSE(SubmitMeshPassIndirectDraws(encOob, oobVisible, oobPrepared, validIndexed, validNonIndexed, caps, smallInstArgs));
        EXPECT_TRUE(encOob.Order.empty());
    }
#endif
    // 5. 预检全部通过，第 2 批 SetPushConstants 失败 -> 立即返回 false。
    {
        RecordingEncoder encPushFail;
        encPushFail.FailPushConstantsOnCall = 2;
        EXPECT_FALSE(SubmitMeshPassIndirectDraws(encPushFail, visible, prepared, validIndexed, validNonIndexed, caps, validArgs));
        EXPECT_EQ(encPushFail.IndexedIndirectDraws.size(), 1u);
        EXPECT_TRUE(encPushFail.IndirectDraws.empty());
    }
    // 6. MaxDrawIndirectCount == 0 时允许空切片并走直接绘制回退返回 true。
    {
        auto zeroMaxCaps = caps;
        zeroMaxCaps.Limits.MaxDrawIndirectCount = 0;
        RecordingEncoder encZeroMax;
        ASSERT_TRUE(SubmitMeshPassIndirectDraws(encZeroMax, visible, prepared, {}, {}, zeroMaxCaps, validArgs));
        EXPECT_EQ(encZeroMax.Draws.size(), 1u);
        EXPECT_EQ(encZeroMax.NonIndexedDraws.size(), 1u);
        EXPECT_TRUE(encZeroMax.IndexedIndirectDraws.empty());
        EXPECT_TRUE(encZeroMax.IndirectDraws.empty());
    }
}

TEST_F(SubmitMeshDrawCommandsTest, PushConstantsDeduplicateAcrossIdenticalInterleavedAndCrossPoolCommands) {
    auto cmdA = MakeCommand();
    cmdA.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{1}, byte{2}, byte{3}, byte{4}}};
    auto cmdNone = MakeCommand();
    auto cmdB = MakeCommand();
    cmdB.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {byte{9}, byte{8}, byte{7}, byte{6}}};

    const auto vA = Dynamic(cmdA);
    const auto vNone = Dynamic(cmdNone);
    const auto vB = Dynamic(cmdB);

    MeshDrawCommandBindingStorage otherStorage;
    MeshDrawCommand crossPoolA = cmdA;
    otherStorage.Store(crossPoolA, cmdA.Bindings);
    const VisibleMeshDrawCommand vCrossPoolA{.CachedCommand = &crossPoolA};

    // 序列：A -> A（跳过）-> None（保留 A）-> A（跳过）-> 跨池同字节 A（跳过）-> B（写入）-> A（写入）。
    const array visible{vA, vA, vNone, vA, vCrossPoolA, vB, vA};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    EXPECT_EQ(Encoder.Draws.size(), 7u);
    ASSERT_EQ(Encoder.Pushes.size(), 3u);
    EXPECT_EQ(Encoder.Pushes[0][0], byte{1});
    EXPECT_EQ(Encoder.Pushes[1][0], byte{9});
    EXPECT_EQ(Encoder.Pushes[2][0], byte{1});
}

TEST_F(SubmitMeshDrawCommandsTest, SameLayoutPipelineChangePreservesParametersAndPushConstants) {
    test::FakePipelineLayout layoutA;
    test::FakePipelineLayout layoutB;
    FakePipeline psoA1{&layoutA};
    FakePipeline psoA2{&layoutA};
    FakePipeline psoB{&layoutB};
    FakeParameterSet passSet;
    FakeParameterSet matSet1;
    FakeParameterSet matSet2;

    auto makeCmd = [&](gpu::GraphicsPipelineState* pso, gpu::ShaderParameterSet* matSet, byte pushTag) {
        auto cmd = MakeInstancedCommand();
        cmd.Pipeline = pso;
        cmd.Bindings.ParameterSets.push_back({1, matSet, {}});
        cmd.Bindings.PushConstants = MeshPushConstantCommandBinding{0, 0, {pushTag, byte{0}, byte{0}, byte{0}}};
        return Dynamic(cmd);
    };

    const auto v0 = makeCmd(&psoA1, &matSet1, byte{1});
    const auto v1 = makeCmd(&psoA2, &matSet1, byte{1});  // 同 Layout、同参数集、同 Push：仅切 PSO + 重绑 VB/实例流
    const auto v2 = makeCmd(&psoA2, &matSet2, byte{2});  // 同 PSO、换参数集、换 Push
    const auto v3 = makeCmd(&psoB, &matSet2, byte{2});   // 换 Layout：重绑 Pass 参数集、命令参数集与 Push

    const MeshShaderParameterBinding passBinding{0, &passSet, {}};
    const MeshPassSubmitArgs args{
        .ParameterSets = std::span{&passBinding, 1},
        .InstanceStream = gpu::VertexBufferBinding{1, {&Instances, 0, 64}}};

    const array visible{v0, v1, v2, v3};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible, args));
    EXPECT_EQ(Encoder.Pipelines, (vector<gpu::GraphicsPipelineState*>{&psoA1, &psoA2, &psoB}));
    // 3 次 PSO 切换，每次都重绑实例流 + 主顶点流 = 6 次 BindVertexBuffers。
    EXPECT_EQ(Encoder.VertexCalls.size(), 6u);
    // ParameterBinds：v0(Pass+matSet1) + v1(0) + v2(matSet2) + v3(Pass+matSet2) = 5。
    ASSERT_EQ(Encoder.ParameterBinds.size(), 5u);
    EXPECT_EQ(Encoder.ParameterBinds[0].Set, &passSet);
    EXPECT_EQ(Encoder.ParameterBinds[1].Set, &matSet1);
    EXPECT_EQ(Encoder.ParameterBinds[2].Set, &matSet2);
    EXPECT_EQ(Encoder.ParameterBinds[3].Set, &passSet);
    EXPECT_EQ(Encoder.ParameterBinds[4].Set, &matSet2);
    // Pushes：v0(1) + v1(跳过) + v2(2) + v3(换 Layout 重写 2) = 3。
    ASSERT_EQ(Encoder.Pushes.size(), 3u);
    EXPECT_EQ(Encoder.Pushes[0][0], byte{1});
    EXPECT_EQ(Encoder.Pushes[1][0], byte{2});
    EXPECT_EQ(Encoder.Pushes[2][0], byte{2});
    EXPECT_EQ(Encoder.Draws.size(), 4u);
}

TEST_F(SubmitMeshDrawCommandsTest, SingleElementFastPathBindsAndDeduplicatesIndexBuffers) {
    FakeBuffer ibA{128, gpu::BufferUse::Index};
    FakeBuffer ibB{128, gpu::BufferUse::Index};

    auto cmd0 = MakeCommand();
    cmd0.IndexBuffer = {&ibA, 0, 4};
    cmd0.FirstIndex = 0;
    auto cmd1 = MakeCommand();
    cmd1.IndexBuffer = {&ibB, 0, 4};
    cmd1.FirstIndex = 3;
    auto cmd2 = MakeCommand();
    cmd2.IndexBuffer = {&ibB, 0, 4};
    cmd2.FirstIndex = 6;

    const auto v0 = Dynamic(cmd0);
    const auto v1 = Dynamic(cmd1);
    const auto v2 = Dynamic(cmd2);
    ASSERT_EQ(v0.CachedCommand->ExtraElementsIndex, kInvalidBindingIndex);
    ASSERT_EQ(v1.CachedCommand->ExtraElementsIndex, kInvalidBindingIndex);
    ASSERT_EQ(v2.CachedCommand->ExtraElementsIndex, kInvalidBindingIndex);

    const array visible{v0, v1, v2};
    ASSERT_TRUE(SubmitMeshDrawCommands(Encoder, visible));
    ASSERT_EQ(Encoder.IndexBuffers.size(), 2u);
    EXPECT_EQ(Encoder.IndexBuffers[0].Target, &ibA);
    EXPECT_EQ(Encoder.IndexBuffers[1].Target, &ibB);
    ASSERT_EQ(Encoder.Draws.size(), 3u);
    EXPECT_EQ(Encoder.Draws[0].FirstIndex, 0u);
    EXPECT_EQ(Encoder.Draws[1].FirstIndex, 3u);
    EXPECT_EQ(Encoder.Draws[2].FirstIndex, 6u);
}

TEST_F(SubmitMeshDrawCommandsTest, ValidatedExtraMaskAndFastPathsCoverAllElementShapes) {
    // 1. 单元素无附加存储（ExtraElementsIndex == kInvalidBindingIndex）。
    {
        auto cmd = MakeCommand();
        const auto v = Dynamic(cmd);
        ASSERT_EQ(v.CachedCommand->ExtraElementsIndex, kInvalidBindingIndex);
        MeshPassIndirectDrawCounts counts{};
        PreparedMeshPassIndirectDraws prepared;
        EXPECT_TRUE(CountMeshPassIndirectDraws(std::span{&v, 1}, counts));
        EXPECT_EQ(counts.IndexedArgumentCount, 1u);
        EXPECT_TRUE(BuildMeshPassIndirectDraws(std::span{&v, 1}, prepared));
        ASSERT_EQ(prepared.IndexedArguments.size(), 1u);
        EXPECT_EQ(prepared.IndexedArguments[0].IndexCount, 3u);
    }

    // 2. ValidElementMask = 1 且 ExtraElements 为空但 ElementInstances 含 1 项主元素实例。
    {
        auto cmd = MakeCommand();
        cmd.ValidElementMask = 1u;
        cmd.Bindings.ElementInstances.push_back({.FirstInstance = 11, .InstanceCount = 5});
        auto v = Dynamic(cmd);
        v.FirstInstance = 0;
        v.InstanceCount = 1;
        v.UseElementFirstInstance = true;
        v.UseElementInstanceCount = true;

        ASSERT_NE(v.CachedCommand->ExtraElementsIndex, kInvalidBindingIndex);
        const auto stored = v.CachedCommand->BindingStorage->GetExtraElements(v.CachedCommand->ExtraElementsIndex);
        ASSERT_TRUE(stored);
        EXPECT_TRUE(stored->StructureValid);
        EXPECT_EQ(stored->ValidatedExtraMask, 0u);

        MeshPassIndirectDrawCounts counts{};
        PreparedMeshPassIndirectDraws prepared;
        ASSERT_TRUE(CountMeshPassIndirectDraws(std::span{&v, 1}, counts));
        EXPECT_EQ(counts.IndexedArgumentCount, 1u);
        ASSERT_TRUE(BuildMeshPassIndirectDraws(std::span{&v, 1}, prepared));
        ASSERT_EQ(prepared.IndexedArguments.size(), 1u);
        EXPECT_EQ(prepared.IndexedArguments[0].FirstInstance, 11u);
        EXPECT_EQ(prepared.IndexedArguments[0].InstanceCount, 5u);

        RecordingEncoder enc;
        ASSERT_TRUE(SubmitMeshDrawCommands(enc, std::span{&v, 1}));
        ASSERT_EQ(enc.Draws.size(), 1u);
        EXPECT_EQ(enc.Draws[0].FirstInstance, 11u);
        EXPECT_EQ(enc.Draws[0].InstanceCount, 5u);
    }

    // 3. 多元素全掩码、子集掩码、连续合并伴随间隔位（线性 ordinal 同步推进）、修改主元素最低置位及非法掩码拒绝。
    {
        auto cmd = MakeCommand();
        // 位 0、位 2、位 3、位 5 有效，且位 2 与位 3 连续合并。
        cmd.ValidElementMask = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5);
        cmd.FirstIndex = 0;
        cmd.IndexCount = 3;
        cmd.Bindings.ExtraElements.push_back({cmd.IndexBuffer, 3u, 6u, 0, 2u});
        cmd.Bindings.ExtraElements.push_back({cmd.IndexBuffer, 3u, 9u, 0, 3u});
        cmd.Bindings.ExtraElements.push_back({cmd.IndexBuffer, 6u, 15u, 0, 5u});
        cmd.Bindings.ContiguousMergeMask = (1u << 2);
        cmd.Bindings.AllElementsSharePrimaryIndexBuffer = true;
        const auto vFull = Dynamic(cmd);

        const auto stored = vFull.CachedCommand->BindingStorage->GetExtraElements(vFull.CachedCommand->ExtraElementsIndex);
        ASSERT_TRUE(stored);
        EXPECT_TRUE(stored->StructureValid);
        EXPECT_EQ(stored->ValidatedExtraMask, (1u << 2) | (1u << 3) | (1u << 5));

        // 3a. 全掩码可见：位 0 (3)、位 2+3 合并 (6)、位 5 (6) => 共 3 条绘制。
        MeshPassIndirectDrawCounts counts{};
        PreparedMeshPassIndirectDraws prepared;
        ASSERT_TRUE(CountMeshPassIndirectDraws(std::span{&vFull, 1}, counts));
        EXPECT_EQ(counts.IndexedArgumentCount, 3u);
        ASSERT_TRUE(BuildMeshPassIndirectDraws(std::span{&vFull, 1}, prepared));
        ASSERT_EQ(prepared.IndexedArguments.size(), 3u);
        EXPECT_EQ(prepared.IndexedArguments[0].FirstIndex, 0u);
        EXPECT_EQ(prepared.IndexedArguments[0].IndexCount, 3u);
        EXPECT_EQ(prepared.IndexedArguments[1].FirstIndex, 6u);
        EXPECT_EQ(prepared.IndexedArguments[1].IndexCount, 6u);
        EXPECT_EQ(prepared.IndexedArguments[2].FirstIndex, 15u);
        EXPECT_EQ(prepared.IndexedArguments[2].IndexCount, 6u);

        RecordingEncoder encFull;
        ASSERT_TRUE(SubmitMeshDrawCommands(encFull, std::span{&vFull, 1}));
        ASSERT_EQ(encFull.Draws.size(), 3u);
        EXPECT_EQ(encFull.Draws[0].IndexCount, 3u);
        EXPECT_EQ(encFull.Draws[1].IndexCount, 6u);
        EXPECT_EQ(encFull.Draws[2].FirstIndex, 15u);
        EXPECT_EQ(encFull.Draws[2].IndexCount, 6u);

        // 3b. 子集掩码（位 3 与位 5，不选位 2 截断合并）：2 条绘制。
        auto vSubset = vFull;
        vSubset.ElementMask = (1u << 3) | (1u << 5);
        ASSERT_TRUE(CountMeshPassIndirectDraws(std::span{&vSubset, 1}, counts));
        EXPECT_EQ(counts.IndexedArgumentCount, 2u);
        ASSERT_TRUE(BuildMeshPassIndirectDraws(std::span{&vSubset, 1}, prepared));
        ASSERT_EQ(prepared.IndexedArguments.size(), 2u);
        EXPECT_EQ(prepared.IndexedArguments[0].FirstIndex, 9u);
        EXPECT_EQ(prepared.IndexedArguments[0].IndexCount, 3u);
        EXPECT_EQ(prepared.IndexedArguments[1].FirstIndex, 15u);
        EXPECT_EQ(prepared.IndexedArguments[1].IndexCount, 6u);

        // 3c. 命令副本修改主元素最低置位（0b0101 -> 0b0110，附加元素仍在位 2）仍应通过。
        auto cmdShift = MakeCommand();
        cmdShift.ValidElementMask = 0b0101u;
        cmdShift.Bindings.ExtraElements.push_back({cmdShift.IndexBuffer, 6u, 3u, 0, 2u});
        const auto vOrig = Dynamic(cmdShift);
        MeshDrawCommand shiftedCopy = *vOrig.CachedCommand;
        shiftedCopy.ValidElementMask = 0b0110u;
        const VisibleMeshDrawCommand vShifted{.CachedCommand = &shiftedCopy};
        ASSERT_TRUE(CountMeshPassIndirectDraws(std::span{&vShifted, 1}, counts));
        EXPECT_EQ(counts.IndexedArgumentCount, 2u);
        ASSERT_TRUE(BuildMeshPassIndirectDraws(std::span{&vShifted, 1}, prepared));
        ASSERT_EQ(prepared.IndexedArguments.size(), 2u);
        EXPECT_EQ(prepared.IndexedArguments[1].IndexCount, 6u);

        // 3d. 篡改 ValidElementMask（附加位集合不匹配）或缺失 ExtraElements 必须返回 false。
        MeshDrawCommand badMask = *vOrig.CachedCommand;
        badMask.ValidElementMask = 0b1001u;  // 附加位为 3，不匹配 ValidatedExtraMask 的位 2
        const VisibleMeshDrawCommand vBadMask{.CachedCommand = &badMask};
        EXPECT_FALSE(CountMeshPassIndirectDraws(std::span{&vBadMask, 1}, counts));
        EXPECT_FALSE(BuildMeshPassIndirectDraws(std::span{&vBadMask, 1}, prepared));

        MeshDrawCommand missingExtra = *vOrig.CachedCommand;
        missingExtra.ExtraElementsIndex = kInvalidBindingIndex;  // ValidElementMask = 0b0101 但缺失 ExtraElements
        const VisibleMeshDrawCommand vMissingExtra{.CachedCommand = &missingExtra};
        EXPECT_FALSE(CountMeshPassIndirectDraws(std::span{&vMissingExtra, 1}, counts));
        EXPECT_FALSE(BuildMeshPassIndirectDraws(std::span{&vMissingExtra, 1}, prepared));

        // 3e. 入库时 ExtraElements 非法（ElementIndex == 0 / 非严格递增 / ElementInstances 数量不匹配）预验标记为 false 并拒绝展开。
        MeshDrawCommandBindingStorage badStorage;
        {
            MeshDrawCommand badZeroIdx = MakeCommand();
            badZeroIdx.ValidElementMask = 0b0011u;
            MeshDrawCommandBindings b;
            b.ExtraElements.push_back({badZeroIdx.IndexBuffer, 3u, 3u, 0, 0u});
            badStorage.Store(badZeroIdx, b);
            const auto storedBad = badStorage.GetExtraElements(badZeroIdx.ExtraElementsIndex);
            ASSERT_TRUE(storedBad);
            EXPECT_FALSE(storedBad->StructureValid);
            const VisibleMeshDrawCommand vBad{.CachedCommand = &badZeroIdx};
            EXPECT_FALSE(CountMeshPassIndirectDraws(std::span{&vBad, 1}, counts));
        }
        {
            MeshDrawCommand badOrder = MakeCommand();
            badOrder.ValidElementMask = 0b0111u;
            MeshDrawCommandBindings b;
            b.ExtraElements.push_back({badOrder.IndexBuffer, 3u, 3u, 0, 2u});
            b.ExtraElements.push_back({badOrder.IndexBuffer, 3u, 6u, 0, 1u});
            badStorage.Store(badOrder, b);
            const auto storedBad = badStorage.GetExtraElements(badOrder.ExtraElementsIndex);
            ASSERT_TRUE(storedBad);
            EXPECT_FALSE(storedBad->StructureValid);
            const VisibleMeshDrawCommand vBad{.CachedCommand = &badOrder};
            EXPECT_FALSE(CountMeshPassIndirectDraws(std::span{&vBad, 1}, counts));
        }
        {
            MeshDrawCommand badInstSize = MakeCommand();
            badInstSize.ValidElementMask = 1u;
            MeshDrawCommandBindings b;
            b.ElementInstances.push_back({0, 1});
            b.ElementInstances.push_back({1, 1});
            badStorage.Store(badInstSize, b);
            const auto storedBad = badStorage.GetExtraElements(badInstSize.ExtraElementsIndex);
            ASSERT_TRUE(storedBad);
            EXPECT_FALSE(storedBad->StructureValid);
            const VisibleMeshDrawCommand vBad{.CachedCommand = &badInstSize};
            EXPECT_FALSE(CountMeshPassIndirectDraws(std::span{&vBad, 1}, counts));
        }
    }
}

}  // 匿名命名空间
}  // namespace radray
