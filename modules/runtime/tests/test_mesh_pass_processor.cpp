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
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexBufferOffset = 8, .IndexStride = 2, .FirstIndex = 4,
                              .IndexCount = 6, .VertexOffset = -1, .FirstInstance = 2, .InstanceCount = 5});
    MeshDrawCommand initializer;
    initializer.Pipeline = &Pipeline;
    auto& command = Output.AddCommand(initializer);
    Output.FinalizeCommand(Context(), 1, command);
    EXPECT_EQ(command.Pipeline.Get(), &Pipeline);
    EXPECT_EQ(command.IndexBuffer.Target, &Indices);
    EXPECT_EQ(command.IndexBuffer.Offset, 8u);
    EXPECT_EQ(command.IndexBuffer.Stride, 2u);
    EXPECT_EQ(command.FirstIndex, 4u);
    EXPECT_EQ(command.IndexCount, 6u);
    EXPECT_EQ(command.VertexOffset, -1);
    EXPECT_EQ(command.FirstInstance, 2u);
    EXPECT_EQ(command.InstanceCount, 5u);
}

TEST_F(MeshPassProcessorTest, DynamicFinalizeAppendsVisibleCommand) {
    Batch.Elements[0].FirstInstance = 3;
    MeshDrawCommand initializer;
    initializer.BaseSortKey = 42;
    auto& command = Output.AddCommand(initializer);
    Output.FinalizeCommand(Context(), 0, command);
    ASSERT_EQ(Visible.size(), 1u);
    EXPECT_EQ(Visible[0].Source, MeshDrawCommandSource::Dynamic);
    EXPECT_EQ(ResolveVisibleMeshDrawCommand(Visible[0], nullptr, Output.Commands).Get(), &Output.Commands[0]);
    EXPECT_EQ(Visible[0].SortKey, 42u);
    EXPECT_EQ(Visible[0].FirstInstance, 3u);
    EXPECT_EQ(Visible[0].InstanceCount, 1u);
}

TEST_F(MeshPassProcessorTest, CachedFinalizeRecordsEntryOnTargetBatch) {
    CachedPassMeshDrawList list;
    CachedPassMeshDrawListContext cached{{2}, list, Batch};
    Batch.Elements[0].FirstInstance = 4;
    MeshDrawCommand initializer;
    initializer.BaseSortKey = 9;
    auto& command = cached.AddCommand(initializer);
    cached.FinalizeCommand(Context(), 0, command);
    ASSERT_EQ(Batch.CachedCommands.size(), 1u);
    const auto& info = Batch.CachedCommands[0];
    EXPECT_EQ(info.Pass, MeshPassId{2});
    EXPECT_EQ(info.ElementIndex, 0u);
    EXPECT_TRUE(list.Commands.IsAlive(info.CommandHandle));
    EXPECT_EQ(info.BaseSortKey, 9u);
    EXPECT_EQ(info.FirstInstance, 4u);
    EXPECT_EQ(list.Commands.Get(info.CommandHandle).FirstInstance, 4u);
}

TEST_F(MeshPassProcessorTest, BuildEmitsOneCommandPerDrawableElement) {
    Batch.Elements.push_back(Element(3));
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexCount = 0});
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexCount = 3, .InstanceCount = 0});
    ASSERT_TRUE(Build({.Pipeline = &Pipeline}));
    ASSERT_EQ(Output.Commands.size(), 2u);
    EXPECT_EQ(Output.Commands[0].FirstIndex, 0u);
    EXPECT_EQ(Output.Commands[1].FirstIndex, 3u);
}

TEST_F(MeshPassProcessorTest, BuildCopiesSharedStateAndVertexStreams) {
    FakeParameterSet parameters;
    const MeshShaderParameterBinding binding{1, &parameters, {}};
    ASSERT_TRUE(Build({.Pipeline = &Pipeline, .StencilReference = 7, .BaseSortKey = 20, .ParameterSets = std::span{&binding, 1}}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    const auto& command = Output.Commands[0];
    EXPECT_EQ(command.Pipeline.Get(), &Pipeline);
    EXPECT_EQ(command.StencilReference, 7);
    EXPECT_EQ(command.BaseSortKey, 20u);
    ASSERT_EQ(command.ParameterSets.size(), 1u);
    EXPECT_EQ(command.ParameterSets[0].ParameterSet.Get(), &parameters);
    ASSERT_EQ(command.VertexBuffers.size(), 1u);
    EXPECT_EQ(command.VertexBuffers[0].View.Target, &Vertices);
    EXPECT_EQ(command.VertexBuffers[0].View.Size, 48u);
}

TEST_F(MeshPassProcessorTest, BuildCopiesPushConstantBytes) {
    float value = 0.5f;
    const MeshPushConstantBinding push{3, 0, std::as_bytes(std::span{&value, 1})};
    ASSERT_TRUE(Build({.Pipeline = &Pipeline, .PushConstants = push}));
    value = -1.0f;
    ASSERT_EQ(Output.Commands.size(), 1u);
    const auto& copied = Output.Commands[0].PushConstants;
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
    EXPECT_EQ(Output.Commands[0].InstanceStreamBinding, 1u);
}

}  // namespace
}  // namespace radray
