#include <gtest/gtest.h>

#include <cstring>

#include <radray/runtime/mesh_pass_processor.h>

#include "test_fakes.h"

namespace radray {
namespace {

using test::FakeBuffer;
using test::FakeParameterSet;
using test::FakePipeline;

class BuildingProcessor final : public MeshPassProcessor {
public:
    using MeshPassProcessor::BuildMeshDrawCommands;
    using MeshPassProcessor::MeshPassProcessor;

    void AddMeshBatch(const MeshBatchContext& context) override { BuildMeshDrawCommands(context, State); }

    MeshDrawCommandSharedState State{};
};

class MeshPassProcessorTest : public testing::Test {
protected:
    MeshPassProcessorTest() {
        Input.Layouts.push_back({0, 12, render::VertexStepMode::Vertex});
        Input.Attributes.push_back({0, 0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0});
        Input.Streams.push_back({&Vertices, 0, 48, 0});
        Batch.VertexInput = &Input;
        Batch.Elements.push_back(Element(0));
    }
    MeshBatchElement Element(uint32_t firstIndex) {
        return {.IndexBuffer = &Indices, .FirstIndex = firstIndex, .IndexCount = 3};
    }
    void AddInstanceLayout() {
        Input.Layouts.push_back({1, 16, render::VertexStepMode::Instance});
        Input.Attributes.push_back({1, 0, "INSTANCE", 0, render::VertexFormat::FLOAT32X4, 1});
    }
    MeshBatchContext Context() const { return {Primitive, Batch}; }
    bool Build(const MeshDrawCommandSharedState& state) { return Processor.BuildMeshDrawCommands(Context(), state); }

    FakePipeline Pipeline;
    FakeBuffer Vertices{128, render::BufferUse::Vertex};
    FakeBuffer Indices{128, render::BufferUse::Index};
    VertexInput Input;
    PrimitiveSceneInfo Primitive;
    MeshBatch Batch;
    vector<VisibleMeshDrawCommand> Visible;
    DynamicMeshPassDrawListContext Output{Visible};
    BuildingProcessor Processor{Output};
};

TEST_F(MeshPassProcessorTest, FinalizeCopiesSelectedElementDrawArguments) {
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexBufferOffset = 8, .IndexStride = 2, .FirstIndex = 4, .IndexCount = 6, .VertexOffset = -1, .FirstInstance = 2, .InstanceCount = 5});
    MeshDrawCommand initializer;
    initializer.Pipeline = &Pipeline;
    initializer.ValidElementMask = 2;
    auto& command = Output.AddCommand(initializer);
    Output.FinalizeCommand(Context(), 1, command);
    EXPECT_EQ(command.Pipeline.Get(), &Pipeline);
    EXPECT_EQ(command.IndexBuffer.Target, &Indices);
    EXPECT_EQ(command.IndexBuffer.Offset, 8u);
    EXPECT_EQ(command.IndexBuffer.Stride, 2u);
    EXPECT_EQ(command.FirstIndex, 4u);
    EXPECT_EQ(command.IndexCount, 6u);
    EXPECT_EQ(command.VertexOffset, -1);
    ASSERT_EQ(Visible.size(), 1u);
    EXPECT_EQ(Visible[0].FirstInstance, 2u);
    EXPECT_EQ(Visible[0].InstanceCount, 5u);
}

TEST_F(MeshPassProcessorTest, DynamicFinalizeAppendsVisibleCommand) {
    Batch.Elements[0].FirstInstance = 3;
    MeshDrawCommand initializer;
    auto& command = Output.AddCommand(initializer, 42);
    Output.FinalizeCommand(Context(), 0, command);
    ASSERT_EQ(Visible.size(), 1u);
    EXPECT_FALSE(Visible[0].CachedCommand);
    Output.FinalizeVisibleCommands();
    EXPECT_EQ(Visible[0].CachedCommand.Get(), &Output.Commands[0]);
    EXPECT_EQ(Visible[0].SortKey, 42u);
    EXPECT_EQ(Visible[0].FirstInstance, 3u);
    EXPECT_EQ(Visible[0].InstanceCount, 1u);
}

TEST_F(MeshPassProcessorTest, CachedFinalizeRecordsEntryOnTargetBatch) {
    CachedPassMeshDrawList list;
    CachedPassMeshDrawListContext cached{{2}, list, Batch};
    Batch.Elements[0].FirstInstance = 4;
    MeshDrawCommand initializer;
    auto& command = cached.AddCommand(initializer, 9);
    cached.FinalizeCommand(Context(), 0, command);
    ASSERT_EQ(Batch.CachedCommands.size(), 1u);
    const auto& info = Batch.CachedCommands[0];
    EXPECT_EQ(info.Pass, MeshPassId{2});
    EXPECT_TRUE(list.Commands.IsAlive(info.CommandHandle));
    EXPECT_EQ(info.BaseSortKey, 9u);
    EXPECT_EQ(info.FirstInstance, 4u);
}

TEST_F(MeshPassProcessorTest, BuildEmitsOneCommandPerBatch) {
    Batch.Elements.push_back(Element(3));
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexCount = 0});
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexCount = 3, .InstanceCount = 0});
    ASSERT_TRUE(Build({.Pipeline = &Pipeline}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    EXPECT_EQ(Output.Commands[0].FirstIndex, 0u);
    EXPECT_EQ(Output.Commands[0].ValidElementMask, 3u);
    Output.FinalizeVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 3u);
}

TEST_F(MeshPassProcessorTest, BatchOwnsOneEntryPerNonemptyPool) {
    for (uint32_t index = 1; index < 7; ++index) Batch.Elements.push_back(Element(index * 3));
    FakeParameterSet parameters;
    const MeshShaderParameterBinding binding{1, &parameters, {}};
    const array<byte, 4> bytes{byte{1}, byte{2}, byte{3}, byte{4}};
    ASSERT_TRUE(Build({.Pipeline = &Pipeline, .ParameterSets = std::span{&binding, 1}, .PushConstants = MeshPushConstantBinding{0, 0, bytes}}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    const auto& command = Output.Commands[0];
    EXPECT_EQ(command.ValidElementMask, 127u);
    EXPECT_EQ(Output.GetBindingStorage().Payloads.Count(), 1u);
    EXPECT_EQ(Output.GetBindingStorage().VertexBuffersPool.Count(), 1u);
    EXPECT_EQ(Output.GetBindingStorage().ExtraElementsPool.Count(), 1u);
    const auto payload = Output.GetBindingStorage().GetPayload(command.PayloadHandle);
    ASSERT_TRUE(payload);
    const auto elements = Output.GetBindingStorage().GetExtraElements(command.ExtraElementsHandle);
    ASSERT_TRUE(elements);
    ASSERT_EQ(elements->ExtraElements.size(), 6u);
    EXPECT_EQ(elements->ExtraElements.back().FirstIndex, 18u);
    EXPECT_EQ(elements->ExtraElements.back().ElementIndex, 6u);
}

TEST_F(MeshPassProcessorTest, BuildCopiesSharedStateAndVertexStreams) {
    FakeParameterSet parameters;
    const MeshShaderParameterBinding binding{1, &parameters, {}};
    ASSERT_TRUE(Build({.Pipeline = &Pipeline, .StencilReference = 7, .BaseSortKey = 20, .ParameterSets = std::span{&binding, 1}}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    const auto& command = Output.Commands[0];
    EXPECT_EQ(command.Pipeline.Get(), &Pipeline);
    EXPECT_EQ(command.StencilReference, 7);
    EXPECT_EQ(Visible[0].SortKey, 20u);
    const auto payload = Output.GetBindingStorage().GetPayload(command.PayloadHandle);
    ASSERT_TRUE(payload);
    ASSERT_EQ(payload->ParameterSets.size(), 1u);
    EXPECT_EQ(payload->ParameterSets[0].ParameterSet.Get(), &parameters);
    const auto vertices = Output.GetBindingStorage().GetVertexBuffers(command.VertexBuffersHandle);
    ASSERT_TRUE(vertices);
    ASSERT_EQ(vertices->Bindings.size(), 1u);
    EXPECT_EQ(vertices->Bindings[0].View.Target, &Vertices);
    EXPECT_EQ(vertices->Bindings[0].View.Size, 48u);
}

TEST_F(MeshPassProcessorTest, BuildCopiesPushConstantBytes) {
    float value = 0.5f;
    const MeshPushConstantBinding push{3, 0, std::as_bytes(std::span{&value, 1})};
    ASSERT_TRUE(Build({.Pipeline = &Pipeline, .PushConstants = push}));
    value = -1.0f;
    ASSERT_EQ(Output.Commands.size(), 1u);
    const auto copied = Output.GetBindingStorage().GetPayload(Output.Commands[0].PayloadHandle)->PushConstants;
    ASSERT_TRUE(copied);
    EXPECT_EQ(copied->PushConstantId, 3u);
    ASSERT_EQ(copied->Data.size(), sizeof(float));
    float stored;
    std::memcpy(&stored, copied->Data.data(), sizeof(stored));
    EXPECT_FLOAT_EQ(stored, 0.5f);
}

TEST_F(MeshPassProcessorTest, BuildAcceptsBatchWithoutDrawableElements) {
    Batch.VertexInput = nullptr;
    Batch.Elements[0].IndexCount = 0;
    EXPECT_TRUE(Build({.Pipeline = &Pipeline}));
    EXPECT_TRUE(Output.Commands.empty());
}

TEST_F(MeshPassProcessorTest, BuildRejectsAnyInvalidElementWithoutEmitting) {
    Batch.Elements.push_back(Element(3));
    Batch.Elements[1].IndexStride = 3;
    EXPECT_FALSE(Build({.Pipeline = &Pipeline}));
    EXPECT_TRUE(Output.Commands.empty());
    EXPECT_TRUE(Visible.empty());
    EXPECT_TRUE(Output.GetBindingStorage().Payloads.Empty());
    EXPECT_TRUE(Output.GetBindingStorage().ExtraElementsPool.Empty());
}

TEST_F(MeshPassProcessorTest, BuildRejectsUnalignedPushConstants) {
    const array<byte, 3> bytes{};
    EXPECT_FALSE(Build({.Pipeline = &Pipeline, .PushConstants = MeshPushConstantBinding{0, 0, bytes}}));
    EXPECT_TRUE(Output.Commands.empty());
}

TEST_F(MeshPassProcessorTest, BuildRequiresInstanceStreamUnlessDeferred) {
    AddInstanceLayout();
    EXPECT_FALSE(Build({.Pipeline = &Pipeline}));
    ASSERT_TRUE(Build({.Pipeline = &Pipeline, .AllowDeferredInstanceStream = true}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    EXPECT_EQ(Output.GetBindingStorage().GetVertexBuffers(Output.Commands[0].VertexBuffersHandle)->InstanceStreamBinding, 1u);
}

TEST_F(MeshPassProcessorTest, DynamicClearInvalidatesAllBindingHandles) {
    FakeParameterSet parameters;
    const MeshShaderParameterBinding binding{1, &parameters, {}};
    const array<byte, 4> bytes{};
    const MeshDrawCommandSharedState state{.Pipeline = &Pipeline, .ParameterSets = std::span{&binding, 1}, .PushConstants = MeshPushConstantBinding{0, 0, bytes}};
    ASSERT_TRUE(Build(state));
    const auto old = Output.Commands[0];
    Output.FinalizeVisibleCommands();
    Visible.clear();
    Output.Clear();
    auto& storage = Output.GetBindingStorage();
    EXPECT_TRUE(storage.Payloads.Empty());
    ASSERT_TRUE(Build(state));
    EXPECT_FALSE(ValidateMeshDrawCommand(old, storage));
    EXPECT_TRUE(ValidateMeshDrawCommand(Output.Commands[0], storage));
}

TEST_F(MeshPassProcessorTest, SparseElementsKeepSourceBitsIncludingBit31) {
    Batch.Elements.assign(32, {});
    Batch.Elements[4] = Element(3);
    Batch.Elements[31] = Element(9);
    ASSERT_TRUE(Build({.Pipeline = &Pipeline}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    EXPECT_EQ(Output.Commands[0].ValidElementMask, (uint32_t{1} << 4) | (uint32_t{1} << 31));
    Output.FinalizeVisibleCommands();
    Visible[0].ElementMask = uint32_t{1} << 31;
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 1u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 9u);
    EXPECT_EQ(encoder.VertexCalls.size(), 1u);
}

TEST_F(MeshPassProcessorTest, SingleHighBitElementNeedsNoExtraElements) {
    Batch.Elements.assign(32, {});
    Batch.Elements[31] = Element(6);
    ASSERT_TRUE(Build({.Pipeline = &Pipeline}));
    EXPECT_TRUE(Output.GetBindingStorage().Payloads.Empty());
    EXPECT_TRUE(Output.GetBindingStorage().ExtraElementsPool.Empty());
    Output.FinalizeVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 1u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 6u);
}

TEST_F(MeshPassProcessorTest, RejectsMoreThan32ElementsWithoutOutput) {
    Batch.Elements.resize(33);
    EXPECT_FALSE(Build({.Pipeline = &Pipeline}));
    EXPECT_TRUE(Output.Commands.empty());
    EXPECT_TRUE(Visible.empty());
    EXPECT_TRUE(Output.GetBindingStorage().Payloads.Empty());
    EXPECT_TRUE(Output.GetBindingStorage().ExtraElementsPool.Empty());
}

TEST_F(MeshPassProcessorTest, BatchBindsSharedStateOnceAndDrawsSelectedIndexBuffers) {
    FakeBuffer otherIndices{128, render::BufferUse::Index};
    Batch.Elements.push_back({.IndexBuffer = &otherIndices, .FirstIndex = 3, .IndexCount = 6, .VertexOffset = -2});
    Batch.Elements.push_back(Element(9));
    const array<byte, 4> bytes{};
    ASSERT_TRUE(Build({.Pipeline = &Pipeline, .PushConstants = MeshPushConstantBinding{0, 0, bytes}}));
    Output.FinalizeVisibleCommands();
    Visible[0].ElementMask = 6;
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[0].IndexCount, 6u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 3u);
    EXPECT_EQ(encoder.Draws[0].VertexOffset, -2);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 9u);
    ASSERT_EQ(encoder.IndexBuffers.size(), 2u);
    EXPECT_EQ(encoder.IndexBuffers[0].Target, &otherIndices);
    EXPECT_EQ(encoder.IndexBuffers[1].Target, &Indices);
    EXPECT_EQ(encoder.Pushes.size(), 1u);
    EXPECT_EQ(encoder.VertexCalls.size(), 1u);
}

TEST_F(MeshPassProcessorTest, All32ElementsDrawFromOneDynamicCommand) {
    Indices.Desc.Size = 1024;
    Batch.Elements.clear();
    for (uint32_t index = 0; index < 32; ++index) Batch.Elements.push_back(Element(index * 3));
    Batch.Elements[31].FirstInstance = 5;
    Batch.Elements[31].InstanceCount = 2;
    ASSERT_TRUE(Build({.Pipeline = &Pipeline}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    Output.FinalizeVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 32u);
    for (uint32_t index = 0; index < 32; ++index) EXPECT_EQ(encoder.Draws[index].FirstIndex, index * 3);
    EXPECT_EQ(encoder.Draws.back().FirstInstance, 5u);
    EXPECT_EQ(encoder.Draws.back().InstanceCount, 2u);
    EXPECT_EQ(encoder.IndexBuffers.size(), 1u);
    EXPECT_EQ(encoder.VertexCalls.size(), 1u);
}

TEST_F(MeshPassProcessorTest, BuildStoresAllStreamsInOneSortedGroup) {
    Input.Layouts.push_back({1, 16, render::VertexStepMode::Vertex});
    Input.Layouts.push_back({2, 16, render::VertexStepMode::Vertex});
    Input.Attributes.push_back({1, 0, "TEXCOORD", 0, render::VertexFormat::FLOAT32X4, 1});
    Input.Attributes.push_back({2, 0, "COLOR", 0, render::VertexFormat::FLOAT32X4, 2});
    Input.Streams = {{&Vertices, 64, 32, 2}, {&Vertices, 0, 32, 0}, {&Vertices, 32, 32, 1}};
    ASSERT_TRUE(Build({.Pipeline = &Pipeline}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    auto& storage = Output.GetBindingStorage();
    EXPECT_EQ(storage.VertexBuffersPool.Count(), 1u);
    EXPECT_TRUE(storage.Payloads.Empty());
    EXPECT_TRUE(storage.ExtraElementsPool.Empty());
    const auto vertices = storage.GetVertexBuffers(Output.Commands[0].VertexBuffersHandle);
    ASSERT_TRUE(vertices);
    ASSERT_EQ(vertices->Bindings.size(), 3u);
    for (uint32_t index = 0; index < 3; ++index) {
        EXPECT_EQ(vertices->Bindings[index].Binding, index);
        EXPECT_EQ(vertices->Bindings[index].View.Offset, index * 32);
    }
    Output.FinalizeVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.VertexCalls.size(), 1u);
    EXPECT_EQ(encoder.VertexCalls[0].size(), 3u);
}

}  // namespace
}  // namespace radray
