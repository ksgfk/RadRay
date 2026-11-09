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
    void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) override { BuildMeshDrawCommands(context, State, output); }

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
    void Build(const MeshDrawCommandSharedState& state) {
        Processor.State = state;
        Processor.AddMeshBatch(Context(), Output);
    }

    FakePipeline Pipeline;
    FakeBuffer Vertices{128, render::BufferUse::Vertex};
    FakeBuffer Indices{128, render::BufferUse::Index};
    VertexInput Input;
    PrimitiveSceneInfo Primitive;
    MeshBatch Batch;
    vector<VisibleMeshDrawCommand> Visible;
    DynamicMeshPassDrawListContext Output{Visible};
    BuildingProcessor Processor;
};

TEST_F(MeshPassProcessorTest, DynamicAddPreservesAssembledGeometryAndUsesSelectedInstanceRange) {
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexBufferOffset = 8, .IndexStride = 2, .FirstIndex = 4, .IndexCount = 6, .VertexOffset = -1, .FirstInstance = 2, .InstanceCount = 5});
    MeshDrawCommand initializer;
    initializer.Pipeline = &Pipeline;
    initializer.ValidElementMask = 2;
    initializer.IndexBuffer = {&Indices, 16, 4};
    initializer.FirstIndex = 6;
    initializer.IndexCount = 9;
    initializer.VertexOffset = -2;
    auto& command = Output.AddCommand(Context(), 1, initializer);
    EXPECT_EQ(command.Pipeline.Get(), &Pipeline);
    EXPECT_EQ(command.IndexBuffer.Target, &Indices);
    EXPECT_EQ(command.IndexBuffer.Offset, 16u);
    EXPECT_EQ(command.IndexBuffer.Stride, 4u);
    EXPECT_EQ(command.FirstIndex, 6u);
    EXPECT_EQ(command.IndexCount, 9u);
    EXPECT_EQ(command.VertexOffset, -2);
    ASSERT_EQ(Visible.size(), 1u);
    EXPECT_EQ(Visible[0].FirstInstance, 2u);
    EXPECT_EQ(Visible[0].InstanceCount, 5u);
}

TEST_F(MeshPassProcessorTest, DynamicAddAppendsVisibleCommand) {
    Batch.Elements[0].FirstInstance = 3;
    MeshDrawCommand initializer;
    Output.AddCommand(Context(), 0, initializer, 42);
    ASSERT_EQ(Visible.size(), 1u);
    EXPECT_FALSE(Visible[0].CachedCommand);
    Output.FinalizeVisibleCommands();
    EXPECT_EQ(Visible[0].CachedCommand.Get(), &Output.Commands[0]);
    EXPECT_EQ(Visible[0].SortKey, uint64_t{42} << 32);
    EXPECT_EQ(Visible[0].FirstInstance, 3u);
    EXPECT_EQ(Visible[0].InstanceCount, 1u);
}

TEST_F(MeshPassProcessorTest, CachedAddRecordsEntryOnTargetBatch) {
    CachedPassMeshDrawList list;
    CachedPassMeshDrawListContext cached{list, 2};
    Batch.Elements[0].FirstInstance = 4;
    MeshDrawCommand initializer;
    initializer.Pipeline = &Pipeline;
    initializer.IndexBuffer = {&Indices, 8, 2};
    initializer.IndexCount = 6;
    initializer.FirstIndex = 4;
    initializer.VertexOffset = -1;
    const auto& command = cached.AddCommand(Context(), 0, initializer, 9);
    EXPECT_EQ(command.IndexBuffer.Target, &Indices);
    EXPECT_EQ(command.IndexBuffer.Offset, 8u);
    EXPECT_EQ(command.IndexBuffer.Stride, 2u);
    EXPECT_EQ(command.IndexCount, 6u);
    EXPECT_EQ(command.FirstIndex, 4u);
    EXPECT_EQ(command.VertexOffset, -1);
    ASSERT_EQ(list.GetCommands().size(), 1u);
    const auto& info = *list.GetBatchSlot(2);
    EXPECT_TRUE(list.GetCommand(2));
    EXPECT_EQ(info.StateSortKey, 9u);
    EXPECT_EQ(info.FirstInstance, 4u);
}

TEST_F(MeshPassProcessorTest, BuildEmitsOneCommandPerBatch) {
    Batch.Elements.push_back(Element(3));
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexCount = 0});
    Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexCount = 3, .InstanceCount = 0});
    Build({.Pipeline = &Pipeline});
    ASSERT_EQ(Output.Commands.size(), 1u);
    EXPECT_EQ(Output.Commands[0].FirstIndex, 0u);
    EXPECT_EQ(Output.Commands[0].ValidElementMask, 3u);
    Output.FinalizeVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 3u);
}

TEST_F(MeshPassProcessorTest, ProcessorPublishesToEachExplicitOutput) {
    Processor.State = {.Pipeline = &Pipeline, .CustomStateSortKey = 17};
    CachedPassMeshDrawList cachedList;
    CachedPassMeshDrawListContext cached{cachedList, 2};
    Processor.AddMeshBatch(Context(), cached);
    Batch.Elements[0].FirstIndex = 3;
    Processor.AddMeshBatch(Context(), Output);
    Batch.Elements[0].FirstIndex = 6;
    Processor.AddMeshBatch(Context(), Output);

    ASSERT_EQ(cachedList.GetCommands().size(), 1u);
    EXPECT_EQ(cachedList.GetCommand(2)->FirstIndex, 0u);
    EXPECT_EQ(cachedList.GetBatchSlot(2)->StateSortKey, 17u);
    EXPECT_EQ(cachedList.GetCommand(2)->BindingStorage.Get(), &cachedList.GetBindings());
    ASSERT_EQ(Output.Commands.size(), 2u);
    EXPECT_EQ(Output.Commands[0].FirstIndex, 3u);
    EXPECT_EQ(Output.Commands[1].FirstIndex, 6u);
    EXPECT_EQ(Output.Commands[0].BindingStorage.Get(), &Output.GetBindingStorage());
    Output.FinalizeVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 3u);
    EXPECT_EQ(encoder.Draws[1].FirstIndex, 6u);
}

TEST_F(MeshPassProcessorTest, BatchOwnsOneEntryPerNonemptyPool) {
    for (uint32_t index = 1; index < 7; ++index) Batch.Elements.push_back(Element(index * 3));
    FakeParameterSet parameters;
    const MeshShaderParameterBinding binding{1, &parameters, {}};
    const array<byte, 4> bytes{byte{1}, byte{2}, byte{3}, byte{4}};
    Build({.Pipeline = &Pipeline, .ParameterSets = std::span{&binding, 1}, .PushConstants = MeshPushConstantBinding{0, 0, bytes}});
    ASSERT_EQ(Output.Commands.size(), 1u);
    const auto& command = Output.Commands[0];
    EXPECT_EQ(command.ValidElementMask, 127u);
    EXPECT_EQ(Output.GetBindingStorage().GetPayloadCount(), 1u);
    EXPECT_EQ(Output.GetBindingStorage().GetVertexBufferCount(), 1u);
    EXPECT_EQ(Output.GetBindingStorage().GetExtraElementsCount(), 1u);
    const auto payload = Output.GetBindingStorage().GetPayload(command.PayloadIndex);
    ASSERT_TRUE(payload);
    const auto elements = Output.GetBindingStorage().GetExtraElements(command.ExtraElementsIndex);
    ASSERT_TRUE(elements);
    ASSERT_EQ(elements->ExtraElements.size(), 6u);
    EXPECT_EQ(elements->ExtraElements.back().FirstIndex, 18u);
    EXPECT_EQ(elements->ExtraElements.back().ElementIndex, 6u);
}

TEST_F(MeshPassProcessorTest, BuildCopiesSharedStateAndVertexStreams) {
    FakeParameterSet parameters;
    const MeshShaderParameterBinding binding{1, &parameters, {}};
    Build({.Pipeline = &Pipeline, .StencilReference = 7, .CustomStateSortKey = 20, .ParameterSets = std::span{&binding, 1}});
    ASSERT_EQ(Output.Commands.size(), 1u);
    const auto& command = Output.Commands[0];
    EXPECT_EQ(command.Pipeline.Get(), &Pipeline);
    EXPECT_EQ(command.StencilReference, 7);
    EXPECT_EQ(Visible[0].SortKey, uint64_t{20} << 32);
    const auto payload = Output.GetBindingStorage().GetPayload(command.PayloadIndex);
    ASSERT_TRUE(payload);
    ASSERT_EQ(payload->ParameterSets.size(), 1u);
    EXPECT_EQ(payload->ParameterSets[0].ParameterSet.Get(), &parameters);
    const auto vertices = Output.GetBindingStorage().GetVertexBuffers(command.VertexBuffersIndex);
    ASSERT_TRUE(vertices);
    ASSERT_EQ(vertices->Bindings.size(), 1u);
    EXPECT_EQ(vertices->Bindings[0].View.Target, &Vertices);
    EXPECT_EQ(vertices->Bindings[0].View.Size, 48u);
}

TEST_F(MeshPassProcessorTest, BuildCopiesPushConstantBytes) {
    float value = 0.5f;
    const MeshPushConstantBinding push{3, 0, std::as_bytes(std::span{&value, 1})};
    Build({.Pipeline = &Pipeline, .PushConstants = push});
    value = -1.0f;
    ASSERT_EQ(Output.Commands.size(), 1u);
    const auto copied = Output.GetBindingStorage().GetPayload(Output.Commands[0].PayloadIndex)->PushConstants;
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
    Build({.Pipeline = &Pipeline});
    EXPECT_TRUE(Output.Commands.empty());
}

TEST_F(MeshPassProcessorTest, BuildRequiresInstanceStreamUnlessDeferred) {
    AddInstanceLayout();
    EXPECT_FALSE(ValidateVertexInput(Input, false));
    Build({.Pipeline = &Pipeline, .AllowDeferredInstanceStream = true});
    ASSERT_EQ(Output.Commands.size(), 1u);
    EXPECT_EQ(Output.GetBindingStorage().GetVertexBuffers(Output.Commands[0].VertexBuffersIndex)->InstanceStreamBinding, 1u);
}

TEST_F(MeshPassProcessorTest, DynamicClearReleasesBindingsAndAllowsRebuild) {
    FakeParameterSet parameters;
    const MeshShaderParameterBinding binding{1, &parameters, {}};
    const array<byte, 4> bytes{};
    const MeshDrawCommandSharedState state{.Pipeline = &Pipeline, .ParameterSets = std::span{&binding, 1}, .PushConstants = MeshPushConstantBinding{0, 0, bytes}};
    Build(state);
    Output.FinalizeVisibleCommands();
    Visible.clear();
    Output.Clear();
    auto& storage = Output.GetBindingStorage();
    EXPECT_TRUE(storage.GetPayloadCount() == 0);
    Build(state);
    EXPECT_TRUE(ValidateMeshDrawCommand(Output.Commands[0], storage));
}

TEST_F(MeshPassProcessorTest, SparseElementsKeepSourceBitsIncludingBit31) {
    Batch.Elements.clear();
    for (uint32_t index = 0; index < 32; ++index) Batch.Elements.emplace_back();
    Batch.Elements[4] = Element(3);
    Batch.Elements[31] = Element(9);
    Build({.Pipeline = &Pipeline});
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
    Batch.Elements.clear();
    for (uint32_t index = 0; index < 32; ++index) Batch.Elements.emplace_back();
    Batch.Elements[31] = Element(6);
    Build({.Pipeline = &Pipeline});
    EXPECT_TRUE(Output.GetBindingStorage().GetPayloadCount() == 0);
    EXPECT_TRUE(Output.GetBindingStorage().GetExtraElementsCount() == 0);
    Output.FinalizeVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 1u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 6u);
}

TEST_F(MeshPassProcessorTest, BatchBindsSharedStateOnceAndDrawsSelectedIndexBuffers) {
    FakeBuffer otherIndices{128, render::BufferUse::Index};
    Batch.Elements.push_back({.IndexBuffer = &otherIndices, .FirstIndex = 3, .IndexCount = 6, .VertexOffset = -2});
    Batch.Elements.push_back(Element(9));
    const array<byte, 4> bytes{};
    Build({.Pipeline = &Pipeline, .PushConstants = MeshPushConstantBinding{0, 0, bytes}});
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
    Build({.Pipeline = &Pipeline});
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
    Build({.Pipeline = &Pipeline});
    ASSERT_EQ(Output.Commands.size(), 1u);
    auto& storage = Output.GetBindingStorage();
    EXPECT_EQ(storage.GetVertexBufferCount(), 1u);
    EXPECT_TRUE(storage.GetPayloadCount() == 0);
    EXPECT_TRUE(storage.GetExtraElementsCount() == 0);
    const auto vertices = storage.GetVertexBuffers(Output.Commands[0].VertexBuffersIndex);
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

TEST_F(MeshPassProcessorTest, MergingContiguousSingleInstanceListElementsRequiresOptIn) {
    Batch.Elements.push_back(Element(3));
    Batch.Elements.push_back(Element(6));
    Build({.Pipeline = &Pipeline, .AllowElementMerging = true});
    Output.FinalizeVisibleCommands();
    const auto elements = Output.GetBindingStorage().GetExtraElements(Output.Commands[0].ExtraElementsIndex);
    ASSERT_TRUE(elements);
    EXPECT_EQ(elements->ContiguousMergeMask, 3u);
    EXPECT_TRUE(elements->AllElementsSharePrimaryIndexBuffer);
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 1u);
    EXPECT_EQ(encoder.Draws[0].IndexCount, 9u);
}

TEST_F(MeshPassProcessorTest, MergingRespectsVisibilityHolesAndInstanceOverrides) {
    Batch.Elements.push_back(Element(3));
    Batch.Elements.push_back(Element(6));
    Build({.Pipeline = &Pipeline, .AllowElementMerging = true});
    Output.FinalizeVisibleCommands();
    auto visible = Visible[0];
    visible.ElementMask = 5;
    test::RecordingEncoder holes;
    ASSERT_TRUE(SubmitMeshDrawCommands(holes, std::span{&visible, 1}));
    ASSERT_EQ(holes.Draws.size(), 2u);
    EXPECT_EQ(holes.Draws[1].FirstIndex, 6u);
    visible.ElementMask = 7;
    visible.InstanceCount = 2;
    visible.UseElementInstanceCount = false;
    test::RecordingEncoder instanced;
    ASSERT_TRUE(SubmitMeshDrawCommands(instanced, std::span{&visible, 1}));
    EXPECT_EQ(instanced.Draws.size(), 3u);
    visible.InstanceCount = 1;
    visible.FirstInstance = 12;
    visible.UseElementFirstInstance = false;
    test::RecordingEncoder single;
    ASSERT_TRUE(SubmitMeshDrawCommands(single, std::span{&visible, 1}));
    ASSERT_EQ(single.Draws.size(), 1u);
    EXPECT_EQ(single.Draws[0].FirstInstance, 12u);
}

TEST_F(MeshPassProcessorTest, MergingRejectsStripMisalignmentDifferentInstancesAndBaseVertices) {
    Batch.Elements.push_back(Element(3));
    const auto verifySeparate = [&]() {
        Build({.Pipeline = &Pipeline, .AllowElementMerging = true});
        Output.FinalizeVisibleCommands();
        test::RecordingEncoder encoder;
        ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
        EXPECT_EQ(encoder.Draws.size(), 2u);
        Visible.clear();
        Output.Clear();
    };
    Batch.Topology = render::PrimitiveTopology::TriangleStrip;
    verifySeparate();
    Batch.Topology = render::PrimitiveTopology::TriangleList;
    Batch.Elements[0].IndexCount = Batch.Elements[1].FirstIndex = 2;
    verifySeparate();
    Batch.Elements[0] = Element(0);
    Batch.Elements[1] = Element(3);
    Batch.Elements[0].InstanceCount = Batch.Elements[1].InstanceCount = 2;
    verifySeparate();
    Batch.Elements[0].InstanceCount = Batch.Elements[1].InstanceCount = 1;
    Batch.Elements[1].FirstInstance = 1;
    verifySeparate();
    Batch.Elements[1].FirstInstance = 0;
    Batch.Elements[1].VertexOffset = 1;
    verifySeparate();
}

TEST_F(MeshPassProcessorTest, MergingSupportsPointAndLineListsAndHighestAdjacentBits) {
    for (const auto topology : {render::PrimitiveTopology::PointList, render::PrimitiveTopology::LineList}) {
        Batch.Topology = topology;
        Batch.Elements.clear();
        for (uint32_t i = 0; i < 32; ++i) Batch.Elements.emplace_back();
        Batch.Elements[30] = Element(0);
        Batch.Elements[31] = Element(2);
        Batch.Elements[30].IndexCount = Batch.Elements[31].IndexCount = 2;
        Build({.Pipeline = &Pipeline, .AllowElementMerging = true});
        Output.FinalizeVisibleCommands();
        test::RecordingEncoder encoder;
        ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
        ASSERT_EQ(encoder.Draws.size(), 1u);
        EXPECT_EQ(encoder.Draws[0].IndexCount, 4u);
        Visible.clear();
        Output.Clear();
    }
}

TEST_F(MeshPassProcessorTest, CumulativeMergeCountCannotOverflowEvenWhenEachPairFits) {
    Indices.Desc.Size = uint64_t{1} << 36;
    Batch.Topology = render::PrimitiveTopology::PointList;
    Batch.Elements[0].IndexCount = 0x60000000;
    Batch.Elements.push_back(Element(0x60000000));
    Batch.Elements[1].IndexCount = 0x60000000;
    Batch.Elements.push_back(Element(0xc0000000));
    Batch.Elements[2].IndexCount = 0x60000000;
    Build({.Pipeline = &Pipeline, .AllowElementMerging = true});
    Output.FinalizeVisibleCommands();
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Draws[0].IndexCount, 0xc0000000u);
    EXPECT_EQ(encoder.Draws[1].IndexCount, 0x60000000u);
}

TEST_F(MeshPassProcessorTest, FirstSelectedDrawBindsIndexBufferEvenWhenPrimaryIsInvisible) {
    Batch.Elements.push_back(Element(3));
    Build({.Pipeline = &Pipeline, .AllowElementMerging = true});
    Output.FinalizeVisibleCommands();
    Visible[0].ElementMask = 2;
    test::RecordingEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, Visible));
    ASSERT_EQ(encoder.IndexBuffers.size(), 1u);
    EXPECT_EQ(encoder.IndexBuffers[0].Target, &Indices);
    ASSERT_EQ(encoder.Draws.size(), 1u);
    EXPECT_EQ(encoder.Draws[0].FirstIndex, 3u);
}

TEST_F(MeshPassProcessorTest, DefaultStateKeysClusterEqualBindingsAndKeepBucketsBounded) {
    Vertices.Desc.Size = 1 << 20;
    for (uint32_t i = 0; i < 4100; ++i) {
        Input.Streams[0].Offset = i * 16;
        Build({.Pipeline = &Pipeline});
        const auto state = static_cast<uint32_t>(Visible.back().SortKey >> 32);
        EXPECT_EQ(state >> 24, 1u);
        EXPECT_EQ((state >> 12) & 4095, 0u);
        EXPECT_NE(state & 4095, 0u);
    }
    const auto key = Visible.back().SortKey;
    Build({.Pipeline = &Pipeline});
    EXPECT_EQ(Visible.back().SortKey, key);
    EXPECT_EQ(Output.Commands.back().VertexBuffersIndex, Output.Commands[4099].VertexBuffersIndex);
}

}  // 匿名命名空间
}  // namespace radray
