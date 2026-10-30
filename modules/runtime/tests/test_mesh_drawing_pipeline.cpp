#include <gtest/gtest.h>

#include <cstdlib>

#include <radray/runtime/mesh_pass_processor.h>

namespace radray {
namespace {

class CpuBuffer final : public render::Buffer {
public:
    CpuBuffer(uint64_t size, render::BufferUses usage) : Desc{.Size = size, .Usage = usage} {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void* Map(uint64_t, uint64_t) noexcept override { std::abort(); }
    void Unmap() noexcept override { std::abort(); }
    void FlushMappedRange(render::BufferRange) noexcept override { std::abort(); }
    void InvalidateMappedRange(render::BufferRange) noexcept override { std::abort(); }
    render::BufferDescriptor GetDesc() const noexcept override { return Desc; }
    render::Device* GetDevice() const noexcept override { std::abort(); }
    render::BufferDescriptor Desc;
};

class CpuPipeline final : public render::GraphicsPipelineState {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
};

class MeshDrawingPipeline : public testing::Test {
protected:
    void SetUp() override {
        Initializer.Pipeline = &Pipeline;
        Initializer.VertexBuffers.push_back({3, {&Vertices, 8, 48}});
        Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexBufferOffset = 4, .FirstIndex = 2, .IndexCount = 3, .VertexOffset = -1});
    }
    CpuPipeline Pipeline;
    CpuBuffer Vertices{128, render::BufferUse::Vertex};
    CpuBuffer Indices{128, render::BufferUse::Index};
    MeshDrawCommand Initializer;
    MeshBatch Batch;
    PrimitiveSceneInfo Primitive;
    DynamicMeshPassDrawListContext Output;
};

TEST_F(MeshDrawingPipeline, FinalizesSelectedElementAndPreservesSharedBindings) {
    auto second = Batch.Elements[0];
    second.IndexBufferOffset = 12;
    second.IndexStride = 2;
    second.FirstIndex = 8;
    second.IndexCount = 6;
    second.VertexOffset = 4;
    Batch.Elements.push_back(second);
    auto& command = Output.AddCommand(Initializer);
    Output.FinalizeCommand({Primitive, Batch, {}, {}}, 1, command);
    EXPECT_EQ(command.Pipeline.Get(), &Pipeline);
    ASSERT_EQ(command.VertexBuffers.size(), 1);
    EXPECT_EQ(command.VertexBuffers[0].Binding, 3);
    EXPECT_EQ(command.VertexBuffers[0].View.Target, &Vertices);
    EXPECT_EQ(command.VertexBuffers[0].View.Offset, 8);
    EXPECT_EQ(command.VertexBuffers[0].View.Size, 48);
    EXPECT_EQ(command.IndexBuffer.Target, &Indices);
    EXPECT_EQ(command.IndexBuffer.Offset, 12);
    EXPECT_EQ(command.IndexBuffer.Stride, 2);
    EXPECT_EQ(command.FirstIndex, 8);
    EXPECT_EQ(command.IndexCount, 6);
    EXPECT_EQ(command.VertexOffset, 4);
}

TEST_F(MeshDrawingPipeline, CommandsOwnIndependentSnapshots) {
    auto& first = Output.AddCommand(Initializer);
    Output.FinalizeCommand({Primitive, Batch, {}, {}}, 0, first);
    Batch.Elements[0].FirstIndex = 8;
    Initializer.VertexBuffers[0].View.Offset = 24;
    auto& second = Output.AddCommand(Initializer);
    Output.FinalizeCommand({Primitive, Batch, {}, {}}, 0, second);
    Batch.Elements.clear();
    Initializer.VertexBuffers.clear();
    ASSERT_EQ(Output.Commands.size(), 2);
    EXPECT_EQ(Output.Commands[0].FirstIndex, 2);
    EXPECT_EQ(Output.Commands[1].FirstIndex, 8);
    ASSERT_EQ(Output.Commands[0].VertexBuffers.size(), 1);
    ASSERT_EQ(Output.Commands[1].VertexBuffers.size(), 1);
    EXPECT_EQ(Output.Commands[0].VertexBuffers[0].View.Offset, 8);
    Output.Commands[0].VertexBuffers[0].View.Offset = 16;
    EXPECT_EQ(Output.Commands[1].VertexBuffers[0].View.Offset, 24);
}

}  // namespace
}  // namespace radray
