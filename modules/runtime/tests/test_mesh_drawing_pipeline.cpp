#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <type_traits>

#include <radray/runtime/mesh_pass_processor.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/scene.h>

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
    render::BufferDescriptor GetDesc() const noexcept override {
        ++DescReads;
        return Desc;
    }
    render::Device* GetDevice() const noexcept override { std::abort(); }
    render::BufferDescriptor Desc;
    mutable size_t DescReads{0};
};

class CpuPipeline final : public render::GraphicsPipelineState {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
};

class CpuParameterSet final : public render::ShaderParameterSet {
public:
    bool IsValid() const noexcept override {
        ++ValidationCalls;
        return Valid;
    }
    void Destroy() noexcept override {}
    bool Set(uint32_t, uint32_t, render::ShaderParameterValue) noexcept override { return true; }
    bool FlushWrites() noexcept override { return true; }
    void ResetBindingsForReuse() noexcept override {}
    bool Valid{true};
    mutable size_t ValidationCalls{0};
};

class CpuEncoder final : public render::GraphicsCommandEncoder {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::CommandBuffer* GetCommandBuffer() const noexcept override { std::abort(); }
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    void SetStencilReference(uint8_t value) noexcept override { Stencils.push_back(value); }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* value) noexcept override { Pipelines.push_back(value); }
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> values) noexcept override {
        EXPECT_FALSE(Pipelines.empty());
        VertexCalls.emplace_back(values.begin(), values.end());
        Vertices.insert(Vertices.end(), values.begin(), values.end());
    }
    void BindIndexBuffer(render::IndexBufferView value) noexcept override { Indices.push_back(value); }
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets) noexcept override {
        EXPECT_FALSE(Pipelines.empty());
        Parameters.push_back({group, set, {}});
        Parameters.back().DynamicOffsets.assign(offsets.begin(), offsets.end());
    }
    bool SetPushConstants(uint32_t id, uint32_t offset, std::span<const byte> bytes) noexcept override {
        Pushes.push_back({id, offset, bytes});
        return !FailPush;
    }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t firstIndex, int32_t offset, uint32_t firstInstance) noexcept override {
        Draws.push_back({count, instances, firstIndex, offset, firstInstance});
    }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override { std::abort(); }
    void DrawIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { std::abort(); }
    void DrawIndexedIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { std::abort(); }

    vector<render::GraphicsPipelineState*> Pipelines;
    vector<render::VertexBufferBinding> Vertices;
    vector<vector<render::VertexBufferBinding>> VertexCalls;
    vector<render::IndexBufferView> Indices;
    vector<MeshShaderParameterBinding> Parameters;
    vector<MeshPushConstantBinding> Pushes;
    vector<uint8_t> Stencils;
    vector<render::DrawIndexedIndirectArguments> Draws;
    bool FailPush{false};
};

class RecordingProcessor final : public MeshPassProcessor {
public:
    using MeshPassProcessor::BuildMeshDrawCommands;

    RecordingProcessor(MeshPassDrawListContext& output, render::GraphicsPipelineState& pipeline) noexcept
        : MeshPassProcessor(output), Pipeline(&pipeline) {}
    void AddMeshBatch(const MeshBatchContext& context) override {
        Calls.push_back(context.BatchHandle);
        EXPECT_TRUE(context.Material);
        auto push = Push;
        const Nullable<const MaterialParameterValue*> parameter = context.Material ? context.Material->GetParameter(0) : nullptr;
        const Nullable<const float*> brightness = parameter ? std::get_if<float>(parameter.Get()) : nullptr;
        if (!push && brightness) push = MeshPushConstantBinding{0, 0, std::as_bytes(std::span{brightness.Get(), 1})};
        EXPECT_TRUE(BuildMeshDrawCommands(context, {.Pipeline = Pipeline, .BaseSortKey = SortKey, .PushConstants = push, .AllowDeferredInstanceStream = AllowDeferred}));
    }
    render::GraphicsPipelineState* Pipeline;
    vector<SparseSetHandle> Calls;
    uint64_t SortKey{0};
    std::optional<MeshPushConstantBinding> Push{};
    bool AllowDeferred{false};
};

class MeshDrawingPipeline : public testing::Test {
protected:
    void SetUp() override {
        Initializer.Pipeline = &Pipeline;
        Initializer.VertexBuffers.push_back({3, {&Vertices, 8, 48}});
        Batch.Elements.push_back({.IndexBuffer = &Indices, .IndexBufferOffset = 4, .FirstIndex = 2, .IndexCount = 3, .VertexOffset = -1});
        Input.Layouts.push_back({3, 12, render::VertexStepMode::Vertex});
        Input.Attributes.push_back({3, 0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0});
        Input.Streams.push_back({&Vertices, 8, 48, 3});
        Batch.VertexInput = &Input;
        Material = World.AddMaterial({});
        Batch.Material = Material;
        PrimitiveHandle = World.AddPrimitive();
    }
    SparseSetHandle AddBatch() { return World.AddMeshBatch(PrimitiveHandle, Batch); }
    void Sync(RecordingProcessor& processor) {
        const MeshPassRegistration pass{{0}, &processor};
        World.SyncCachedMeshDrawCommands(std::span{&pass, 1});
    }
    SparseSetHandle CommandHandle(SparseSetHandle batch) { return World.Meshes.Get(batch).CachedCommands[0].CommandHandle; }
    void ExpectCachedPush(SparseSetHandle batch, float expected) {
        const auto& push = World.GetCachedDrawList({0})->Commands.Get(CommandHandle(batch)).PushConstants;
        ASSERT_TRUE(push);
        ASSERT_EQ(push->Data.size(), sizeof(float));
        float value;
        std::memcpy(&value, push->Data.data(), sizeof(value));
        EXPECT_FLOAT_EQ(value, expected);
    }
    CpuPipeline Pipeline;
    CpuBuffer Vertices{128, render::BufferUse::Vertex};
    CpuBuffer Indices{128, render::BufferUse::Index};
    MeshDrawCommand Initializer;
    MeshBatch Batch;
    PrimitiveSceneInfo Primitive;
    DynamicMeshPassDrawListContext Output;
    VertexInput Input;
    Scene World;
    SparseSetHandle Material;
    SparseSetHandle PrimitiveHandle;
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
    array<float, 20> constants{};
    constants[0] = 3.0f;
    const auto bytes = std::as_bytes(std::span{constants});
    Initializer.PushConstants = MeshPushConstantCommandBinding{4, 0, {}};
    Initializer.PushConstants->Data.assign(bytes.begin(), bytes.end());
    auto& first = Output.AddCommand(Initializer);
    Output.FinalizeCommand({Primitive, Batch, {}, {}}, 0, first);
    Batch.Elements[0].FirstIndex = 8;
    Initializer.VertexBuffers[0].View.Offset = 24;
    Initializer.PushConstants->Data[0] = byte{1};
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
    float copied;
    std::memcpy(&copied, Output.Commands[0].PushConstants->Data.data(), sizeof(copied));
    EXPECT_FLOAT_EQ(copied, 3.0f);
    Output.Commands[0].PushConstants->Data[0] = byte{2};
    EXPECT_EQ(Output.Commands[1].PushConstants->Data[0], byte{1});
}

TEST_F(MeshDrawingPipeline, IncrementalSyncBuildsOnlyDirtyBatchesAndPreservesHandles) {
    RecordingProcessor processor{Output, Pipeline};
    const array batches{AddBatch(), AddBatch(), AddBatch()};
    Sync(processor);
    EXPECT_EQ(processor.Calls, (vector<SparseSetHandle>{batches.begin(), batches.end()}));
    EXPECT_EQ(&processor.DrawListContext(), &Output);
    const auto stable = CommandHandle(batches[2]);
    const auto previous = CommandHandle(batches[0]);
    auto edited = World.Meshes.Get(batches[0]);
    edited.Elements[0].FirstIndex = 5;
    ASSERT_TRUE(World.UpdateMeshBatch(batches[0], std::move(edited)));
    ASSERT_TRUE(World.RemoveMeshBatch(PrimitiveHandle, batches[1]));
    processor.Calls.clear();
    Sync(processor);
    EXPECT_EQ(processor.Calls, (vector<SparseSetHandle>{batches[0]}));
    EXPECT_EQ(CommandHandle(batches[2]), stable);
    EXPECT_NE(CommandHandle(batches[0]), previous);
    const VisibleMeshDrawCommand visible{.CommandHandle = stable};
    const auto resolved = ResolveVisibleMeshDrawCommand(visible, World.GetCachedDrawList({0}));
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->FirstIndex, 2u);
    EXPECT_FALSE(ResolveVisibleMeshDrawCommand({.CommandHandle = previous}, World.GetCachedDrawList({0})));
}

TEST_F(MeshDrawingPipeline, VisibleCommandsSurviveSubsequentCacheMutationsViaHandles) {
    RecordingProcessor processor{Output, Pipeline};
    const auto first = AddBatch();
    const auto retained = AddBatch();
    Sync(processor);
    vector<VisibleMeshDrawCommand> visible;
    const VisibleMeshBatch request{.Batch = retained};
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands({0}, std::span{&request, 1}, visible));
    const auto handle = visible[0].CommandHandle;
    vector<SparseSetHandle> added;
    for (uint32_t index = 0; index < 512; ++index) {
        const auto handle = AddBatch();
        ASSERT_TRUE(handle.IsValid());
        added.push_back(handle);
    }
    Sync(processor);
    for (const auto handle : added) ASSERT_TRUE(World.RemoveMeshBatch(PrimitiveHandle, handle));
    ASSERT_TRUE(World.RemoveMeshBatch(PrimitiveHandle, first));
    const auto resolved = ResolveVisibleMeshDrawCommand(visible[0], World.GetCachedDrawList({0}));
    ASSERT_TRUE(resolved);
    EXPECT_EQ(visible[0].CommandHandle, handle);
    EXPECT_EQ(resolved->IndexBuffer.Target, &Indices);
    EXPECT_TRUE(ValidateMeshDrawCommand(*resolved, visible[0]));
}

TEST_F(MeshDrawingPipeline, MaterialUpdateInvalidatesOnlyReferencingBatches) {
    MaterialResource resource;
    const array<MaterialParameter, 1> declarations{{{"Brightness", 0.5f}}};
    const auto definition = radray::Material::Create(resource, declarations);
    ASSERT_TRUE(definition);
    MaterialRenderProxy initialProxy;
    initialProxy.Update(*definition);
    ASSERT_TRUE(World.UpdateMaterial(Material, initialProxy));
    MaterialInstance instance{*definition};
    ASSERT_TRUE(instance.SetParameter(0, 0.8f));
    MaterialRenderProxy otherProxy;
    otherProxy.Update(instance);
    RecordingProcessor processor{Output, Pipeline};
    const auto first = AddBatch();
    const auto second = AddBatch();
    const auto otherMaterial = World.AddMaterial(std::move(otherProxy));
    Batch.Material = otherMaterial;
    const auto other = AddBatch();
    Sync(processor);
    ExpectCachedPush(first, 0.5f);
    ExpectCachedPush(second, 0.5f);
    ExpectCachedPush(other, 0.8f);
    const auto firstPrevious = CommandHandle(first);
    const auto secondPrevious = CommandHandle(second);
    const auto stable = CommandHandle(other);
    processor.Calls.clear();
    ASSERT_TRUE(instance.SetParameter(0, 1.0f));
    MaterialRenderProxy updated;
    updated.Update(instance);
    ASSERT_TRUE(World.UpdateMaterial(Material, std::move(updated)));
    World.MarkMaterialDirty(Material);
    EXPECT_EQ(World.GetDirtyBatches().size(), 2u);
    Sync(processor);
    EXPECT_EQ(processor.Calls, (vector<SparseSetHandle>{first, second}));
    EXPECT_EQ(CommandHandle(other), stable);
    EXPECT_NE(CommandHandle(first), firstPrevious);
    EXPECT_NE(CommandHandle(second), secondPrevious);
    ExpectCachedPush(first, 1.0f);
    ExpectCachedPush(second, 1.0f);
    ExpectCachedPush(other, 0.8f);
    EXPECT_FALSE(World.RemoveMaterial(Material));
    ASSERT_TRUE(World.RemovePrimitive(PrimitiveHandle));
    EXPECT_EQ(World.GetCachedDrawList({0})->Commands.Count(), 0u);
    EXPECT_TRUE(World.RemoveMaterial(Material));
    EXPECT_TRUE(World.RemoveMaterial(otherMaterial));
}

TEST_F(MeshDrawingPipeline, TransformAndPassParametersUpdateWithoutRebuildingCache) {
    static_assert(std::is_trivially_copyable_v<VisibleMeshDrawCommand>);
    static_assert(std::is_standard_layout_v<VisibleMeshDrawCommand>);
    Input.Layouts.push_back({4, 16, render::VertexStepMode::Instance});
    Input.Attributes.push_back({4, 0, "INSTANCE", 0, render::VertexFormat::FLOAT32X4, 1});
    RecordingProcessor processor{Output, Pipeline};
    processor.AllowDeferred = true;
    array<float, 4> constants{0.2f, 0.4f, 0.6f, 1.0f};
    processor.Push = MeshPushConstantBinding{4, 0, std::as_bytes(std::span{constants})};
    const auto batch = AddBatch();
    Sync(processor);
    const auto stable = CommandHandle(batch);
    processor.Calls.clear();
    constants.fill(-1.0f);
    array<CpuParameterSet, 2> passParameters;
    CpuBuffer firstInstances{256, render::BufferUse::Vertex};
    CpuBuffer secondInstances{256, render::BufferUse::Vertex};
    const array<render::Buffer*, 2> instanceBuffers{&firstInstances, &secondInstances};
    for (uint32_t frame = 0; frame < 2; ++frame) {
        World.Primitives.Get(PrimitiveHandle).LocalToWorld = AffineTransform{ComposeTransform(
            Eigen::Vector3f{static_cast<float>(frame + 1), 0, 0}, Eigen::Quaternionf::Identity(), Eigen::Vector3f::Ones().eval())};
        const VisibleMeshBatch request{.Batch = batch, .FirstInstanceOverride = 7 + frame, .InstanceCountOverride = 3};
        vector<VisibleMeshDrawCommand> visible;
        ASSERT_TRUE(World.GatherVisibleMeshDrawCommands({0}, std::span{&request, 1}, visible));
        CpuEncoder encoder;
        const MeshShaderParameterBinding passBinding{.GroupIndex = 0, .ParameterSet = &passParameters[frame], .DynamicOffsets = {}};
        const MeshPassSubmitArgs submitArgs{
            .ParameterSets = std::span{&passBinding, 1},
            .InstanceStream = render::VertexBufferBinding{4, {instanceBuffers[frame], frame * 16, 192}}};
        ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible, World.GetCachedDrawList({0}), {}, submitArgs));
        ASSERT_EQ(encoder.Draws.size(), 1u);
        EXPECT_EQ(encoder.Draws[0].FirstInstance, 7u + frame);
        EXPECT_EQ(encoder.Draws[0].InstanceCount, 3u);
        ASSERT_EQ(encoder.Pushes.size(), 1u);
        EXPECT_EQ(encoder.Pushes[0].PushConstantId, 4u);
        array<float, 4> copied;
        ASSERT_EQ(encoder.Pushes[0].Data.size(), sizeof(copied));
        std::memcpy(copied.data(), encoder.Pushes[0].Data.data(), sizeof(copied));
        EXPECT_EQ(copied, (array<float, 4>{0.2f, 0.4f, 0.6f, 1.0f}));
        ASSERT_EQ(encoder.Parameters.size(), 1u);
        EXPECT_EQ(encoder.Parameters[0].ParameterSet.Get(), &passParameters[frame]);
        ASSERT_EQ(encoder.Vertices.size(), 2u);
        EXPECT_EQ(encoder.Vertices[1].Binding, 4u);
        EXPECT_EQ(encoder.Vertices[1].View.Target, instanceBuffers[frame]);
        EXPECT_EQ(encoder.Vertices[1].View.Offset, frame * 16);
        Sync(processor);
        EXPECT_EQ(CommandHandle(batch), stable);
        EXPECT_FALSE(World.HasDirtyBatches());
    }
    EXPECT_TRUE(processor.Calls.empty());
}

TEST_F(MeshDrawingPipeline, StaticAndDynamicPathsMergeAndSort) {
    RecordingProcessor processor{Output, Pipeline};
    processor.SortKey = 20;
    AddBatch();
    Sync(processor);
    vector<MeshPassFrameDrawList> lists(1);
    ASSERT_TRUE(World.GatherVisiblePrimitives({0}, std::span{&PrimitiveHandle, 1}, lists[0].VisibleCommands));
    lists.resize(16);
    auto& pass = lists[0];
    processor.SetDrawListContext(pass.DynamicContext);
    processor.SortKey = 10;
    processor.AddMeshBatch({Primitive, Batch, {}, {}, &World.Materials.Get(Material)});
    pass.SortVisibleCommands();
    ASSERT_EQ(pass.VisibleCommands.size(), 2u);
    EXPECT_EQ(pass.VisibleCommands[0].Source, MeshDrawCommandSource::Dynamic);
    EXPECT_EQ(pass.VisibleCommands[1].Source, MeshDrawCommandSource::Cached);
    for (const auto& visible : pass.VisibleCommands) {
        const auto command = ResolveVisibleMeshDrawCommand(visible, World.GetCachedDrawList({0}), pass.DynamicContext.Commands);
        ASSERT_TRUE(command);
        EXPECT_TRUE(ValidateMeshDrawCommand(*command, visible));
    }
    CpuEncoder encoder;
    CpuParameterSet parameters;
    const MeshShaderParameterBinding passBinding{0, &parameters, {}};
    ASSERT_TRUE(pass.Submit(encoder, World.GetCachedDrawList({0}), {.ParameterSets = std::span{&passBinding, 1}}));
    EXPECT_EQ(encoder.Draws.size(), 2u);
    EXPECT_EQ(encoder.Pipelines.size(), 1u);
    ASSERT_EQ(encoder.Parameters.size(), 1u);
    EXPECT_EQ(encoder.Parameters[0].ParameterSet.Get(), &parameters);
    pass.Clear();
    EXPECT_TRUE(pass.VisibleCommands.empty());
    EXPECT_TRUE(pass.DynamicContext.Commands.empty());
}

TEST_F(MeshDrawingPipeline, SceneRejectsInvalidEditsAndPreservesExistingCommands) {
    RecordingProcessor processor{Output, Pipeline};
    const auto batch = AddBatch();
    Sync(processor);
    const auto stable = CommandHandle(batch);
    auto invalid = Batch;
    invalid.Elements[0].IndexCount = std::numeric_limits<uint32_t>::max();
    EXPECT_FALSE(World.AddMeshBatch(PrimitiveHandle, invalid).IsValid());
    EXPECT_FALSE(World.UpdateMeshBatch(batch, invalid));
    invalid = Batch;
    invalid.Material = {};
    EXPECT_FALSE(World.UpdateMeshBatch(batch, invalid));
    EXPECT_FALSE(World.RemoveMeshBatch(World.AddPrimitive(), batch));
    EXPECT_EQ(CommandHandle(batch), stable);
    EXPECT_FALSE(World.HasDirtyBatches());
    vector<VisibleMeshDrawCommand> visible(1);
    const array<VisibleMeshBatch, 2> requests{{{.Batch = batch}, {.Batch = {}}}};
    EXPECT_FALSE(World.GatherVisibleMeshDrawCommands({0}, requests, visible));
    EXPECT_EQ(visible.size(), 1u);
    World.Clear();
    EXPECT_FALSE(ResolveVisibleMeshDrawCommand({.CommandHandle = stable}, World.GetCachedDrawList({0})));
}

TEST_F(MeshDrawingPipeline, DeferredInstanceStreamsAreValidatedAndBoundPerFrame) {
    Input.Layouts.push_back({4, 16, render::VertexStepMode::Instance});
    Input.Attributes.push_back({4, 0, "INSTANCE", 0, render::VertexFormat::FLOAT32X4, 1});
    EXPECT_FALSE(ValidateVertexInput(Input));
    EXPECT_TRUE(ValidateVertexInput(Input, true));
    RecordingProcessor processor{Output, Pipeline};
    processor.AllowDeferred = true;
    const auto batch = AddBatch();
    Sync(processor);
    CpuBuffer instances{128, render::BufferUse::Vertex};
    const VisibleMeshBatch request{.Batch = batch};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands({0}, std::span{&request, 1}, visible));
    const auto command = ResolveVisibleMeshDrawCommand(visible[0], World.GetCachedDrawList({0}));
    ASSERT_TRUE(command);
    CpuEncoder encoder;
    const MeshPassSubmitArgs wrongSlot{.InstanceStream = render::VertexBufferBinding{3, {&instances, 16, 48}}};
    const MeshPassSubmitArgs invalidBuffer{.InstanceStream = render::VertexBufferBinding{4, {&Indices, 0, 48}}};
    EXPECT_FALSE(ValidateMeshDrawCommand(*command, visible[0]));
    EXPECT_FALSE(ValidateMeshDrawCommand(*command, visible[0], wrongSlot));
    EXPECT_FALSE(ValidateMeshDrawCommand(*command, visible[0], invalidBuffer));
#if RADRAY_IS_DEBUG
    EXPECT_FALSE(SubmitMeshDrawCommands(encoder, visible, World.GetCachedDrawList({0})));
    EXPECT_FALSE(SubmitMeshDrawCommands(encoder, visible, World.GetCachedDrawList({0}), {}, wrongSlot));
    EXPECT_FALSE(SubmitMeshDrawCommands(encoder, visible, World.GetCachedDrawList({0}), {}, invalidBuffer));
#endif
    EXPECT_TRUE(encoder.Pipelines.empty());
    EXPECT_TRUE(encoder.Vertices.empty());
    const MeshPassSubmitArgs submitArgs{.InstanceStream = render::VertexBufferBinding{4, {&instances, 16, 48}}};
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible, World.GetCachedDrawList({0}), {}, submitArgs));
    ASSERT_EQ(encoder.Vertices.size(), 2u);
    EXPECT_EQ(encoder.Vertices[1].View.Target, &instances);
    Input.Streams.push_back({&instances, 0, 16, 3});
    EXPECT_FALSE(ValidateVertexInput(Input, true));
}

TEST_F(MeshDrawingPipeline, ResolvesHandlesAndPropagatesPushBindingFailure) {
    auto& command = Output.AddCommand(Initializer);
    Output.FinalizeCommand({Primitive, Batch}, 0, command);
    vector<VisibleMeshDrawCommand> visible{{.Source = MeshDrawCommandSource::Dynamic, .CommandHandle = {0, 0}},
                                           {.Source = MeshDrawCommandSource::Dynamic, .CommandHandle = {99, 0}}};
    CpuEncoder handleEncoder;
    EXPECT_FALSE(SubmitMeshDrawCommands(handleEncoder, visible, nullptr, Output.Commands));
#if RADRAY_IS_DEBUG
    EXPECT_TRUE(handleEncoder.Pipelines.empty());
    EXPECT_TRUE(handleEncoder.Draws.empty());
#else
    EXPECT_EQ(handleEncoder.Pipelines.size(), 1u);
    EXPECT_EQ(handleEncoder.Draws.size(), 1u);
#endif
    CpuEncoder encoder;
    visible.resize(1);
    const auto expectInvalid = [&] {
        EXPECT_FALSE(ValidateMeshDrawCommand(command, visible[0]));
#if RADRAY_IS_DEBUG
        EXPECT_FALSE(SubmitMeshDrawCommands(encoder, visible, nullptr, Output.Commands));
#endif
    };
    command.PushConstants = MeshPushConstantCommandBinding{};
    expectInvalid();
    command.PushConstants->Data = {byte{}, byte{}, byte{}, byte{}};
    command.PushConstants->Offset = 2;
    expectInvalid();
    command.PushConstants->Offset = 0;
    command.PushConstants->Data.pop_back();
    expectInvalid();
    command.PushConstants->Data.push_back(byte{});
    command.PushConstants->Offset = std::numeric_limits<uint32_t>::max() - 3;
    expectInvalid();
    EXPECT_TRUE(encoder.Pipelines.empty());
    EXPECT_TRUE(encoder.Pushes.empty());
    command.PushConstants->Offset = 0;
    encoder.FailPush = true;
    EXPECT_FALSE(SubmitMeshDrawCommands(encoder, visible, nullptr, Output.Commands));
    EXPECT_EQ(encoder.Pushes.size(), 1u);
    EXPECT_TRUE(encoder.Draws.empty());
}

TEST_F(MeshDrawingPipeline, RejectsAmbiguousDeferredInstanceLayouts) {
    CpuBuffer instances{128, render::BufferUse::Vertex};
    Input.Layouts.push_back({4, 16, render::VertexStepMode::Instance});
    Input.Attributes.push_back({4, 0, "INSTANCE", 0, render::VertexFormat::FLOAT32X4, 1});
    Input.Layouts.push_back({5, 16, render::VertexStepMode::Instance});
    Input.Attributes.push_back({5, 0, "INSTANCE", 1, render::VertexFormat::FLOAT32X4, 2});
    EXPECT_FALSE(ValidateVertexInput(Input, true));
    Input.Streams.push_back({&instances, 0, 64, 4});
    EXPECT_FALSE(ValidateVertexInput(Input, true));
    Input.Streams.push_back({&instances, 64, 64, 5});
    EXPECT_TRUE(ValidateVertexInput(Input));
    EXPECT_TRUE(ValidateVertexInput(Input, true));
}

TEST_F(MeshDrawingPipeline, DefaultInstanceStreamCanBeReplacedOnlyAtDeclaredSlot) {
    CpuBuffer instances{128, render::BufferUse::Vertex};
    Input.Layouts.push_back({4, 16, render::VertexStepMode::Instance});
    Input.Attributes.push_back({4, 0, "INSTANCE", 0, render::VertexFormat::FLOAT32X4, 1});
    Input.Streams.push_back({&instances, 0, 64, 4});
    RecordingProcessor processor{Output, Pipeline};
    processor.AllowDeferred = true;
    const auto batch = AddBatch();
    Sync(processor);
    vector<VisibleMeshDrawCommand> visible;
    const VisibleMeshBatch request{.Batch = batch};
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands({0}, std::span{&request, 1}, visible));
    auto& command = World.GetCachedDrawList({0})->Commands.Get(CommandHandle(batch));
    EXPECT_EQ(command.InstanceStreamBinding, 4u);
    CpuEncoder defaultEncoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(defaultEncoder, visible, World.GetCachedDrawList({0})));
    ASSERT_EQ(defaultEncoder.Vertices.size(), 2u);
    EXPECT_EQ(defaultEncoder.Vertices[1].View.Offset, 0u);
    const MeshPassSubmitArgs submitArgs{.InstanceStream = render::VertexBufferBinding{4, {&instances, 16, 48}}};
    CpuEncoder overrideEncoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(overrideEncoder, visible, World.GetCachedDrawList({0}), {}, submitArgs));
    ASSERT_EQ(overrideEncoder.Vertices.size(), 2u);
    EXPECT_EQ(overrideEncoder.Vertices[1].View.Offset, 16u);
    auto edited = command;
    edited.VertexBuffers[1].View = {};
    EXPECT_FALSE(ValidateMeshDrawCommand(edited, visible[0]));
    EXPECT_TRUE(ValidateMeshDrawCommand(edited, visible[0], submitArgs));
    edited.VertexBuffers.push_back(edited.VertexBuffers[1]);
    EXPECT_FALSE(ValidateMeshDrawCommand(edited, visible[0], submitArgs));
    edited = command;
    edited.InstanceStreamBinding.reset();
    EXPECT_TRUE(ValidateMeshDrawCommand(edited, visible[0]));
    EXPECT_FALSE(ValidateMeshDrawCommand(edited, visible[0], submitArgs));
}

TEST_F(MeshDrawingPipeline, PreflightsPassParametersAndRebindsAfterPipelineChanges) {
    CpuParameterSet passParameters;
    CpuParameterSet commandParameters;
    CpuPipeline otherPipeline;
    CpuBuffer instances{128, render::BufferUse::Vertex};
    Initializer.InstanceStreamBinding = 4;
    Initializer.VertexBuffers.push_back({4, {&instances, 0, 64}});
    Initializer.ParameterSets.push_back({2, &commandParameters, {}});
    vector<VisibleMeshDrawCommand> visible;
    Output.BindVisibleCommands(visible);
    for (uint32_t index = 0; index < 4; ++index) {
        Initializer.Pipeline = index == 2 ? &otherPipeline : &Pipeline;
        auto& command = Output.AddCommand(Initializer);
        Output.FinalizeCommand({Primitive, Batch}, 0, command);
    }
    array<MeshShaderParameterBinding, 2> bindings{{{0, &passParameters, {{1, 0, 256}}}, {1, &passParameters, {}}}};
    const MeshPassSubmitArgs submitArgs{.ParameterSets = bindings, .InstanceStream = render::VertexBufferBinding{4, {&instances, 16, 64}}};
    CpuEncoder encoder;
    const auto expectInvalid = [&] {
        EXPECT_FALSE(ValidateMeshDrawCommand(Output.Commands.back(), visible.back(), submitArgs));
#if RADRAY_IS_DEBUG
        EXPECT_FALSE(SubmitMeshDrawCommands(encoder, visible, nullptr, Output.Commands, submitArgs));
#endif
    };
    passParameters.Valid = false;
    expectInvalid();
    passParameters.Valid = true;
    bindings[1].ParameterSet = nullptr;
    expectInvalid();
    bindings[1].ParameterSet = &passParameters;
    bindings[1].GroupIndex = 0;
    expectInvalid();
    bindings[1].GroupIndex = 1;
    bindings[0].DynamicOffsets.push_back({1, 0, 512});
    expectInvalid();
    bindings[0].DynamicOffsets.pop_back();
    Output.Commands.back().ParameterSets.push_back({1, &commandParameters, {}});
    expectInvalid();
    Output.Commands.back().ParameterSets.pop_back();
    EXPECT_TRUE(encoder.Pipelines.empty());
    EXPECT_TRUE(encoder.Parameters.empty());
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible, nullptr, Output.Commands, submitArgs));
    EXPECT_EQ(encoder.Pipelines, (vector<render::GraphicsPipelineState*>{&Pipeline, &otherPipeline, &Pipeline}));
    ASSERT_EQ(encoder.Parameters.size(), 9u);
    const array<uint32_t, 9> expectedGroups{0, 1, 2, 0, 1, 2, 0, 1, 2};
    for (size_t index = 0; index < expectedGroups.size(); ++index) EXPECT_EQ(encoder.Parameters[index].GroupIndex, expectedGroups[index]);
    EXPECT_EQ(encoder.Parameters[0].DynamicOffsets[0].Offset, 256u);
    EXPECT_EQ(encoder.Draws.size(), 4u);
    ASSERT_EQ(encoder.VertexCalls.size(), 3u);
    for (const auto& call : encoder.VertexCalls) {
        ASSERT_EQ(call.size(), 2u);
        EXPECT_EQ(call[0].Binding, 3u);
        EXPECT_EQ(call[1].Binding, 4u);
        EXPECT_EQ(call[1].View.Offset, 16u);
    }
    EXPECT_EQ(encoder.Indices.size(), 1u);
    EXPECT_EQ(encoder.Stencils.size(), 1u);
}

TEST_F(MeshDrawingPipeline, ValidatesAndSubmitsStaticParametersAndInstanceRanges) {
    CpuParameterSet parameters;
    Initializer.ParameterSets.push_back({2, &parameters, {{3, 0, 256}}});
    Initializer.StencilReference = 7;
    Batch.Elements[0].FirstInstance = 2;
    Batch.Elements[0].InstanceCount = 4;
    vector<VisibleMeshDrawCommand> visible;
    Output.BindVisibleCommands(visible);
    auto& command = Output.AddCommand(Initializer);
    Output.FinalizeCommand({Primitive, Batch}, 0, command);
    CpuEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible, nullptr, Output.Commands));
    EXPECT_EQ(encoder.Draws[0].FirstInstance, 2u);
    EXPECT_EQ(encoder.Draws[0].InstanceCount, 4u);
    EXPECT_EQ(encoder.Stencils, (vector<uint8_t>{7}));
    ASSERT_EQ(encoder.Parameters.size(), 1u);
    EXPECT_EQ(encoder.Parameters[0].GroupIndex, 2u);
    EXPECT_EQ(encoder.Parameters[0].DynamicOffsets[0].Offset, 256u);
    command.ParameterSets[0].DynamicOffsets.push_back({3, 0, 512});
    EXPECT_FALSE(ValidateMeshDrawCommand(command, visible[0]));
    command.ParameterSets[0].DynamicOffsets.pop_back();
    const MeshShaderParameterBinding conflictingBinding{.GroupIndex = 2, .ParameterSet = &parameters, .DynamicOffsets = {}};
    EXPECT_FALSE(ValidateMeshDrawCommand(command, visible[0], {.ParameterSets = std::span{&conflictingBinding, 1}}));
    visible[0].FirstInstance = std::numeric_limits<uint32_t>::max();
    EXPECT_FALSE(ValidateMeshDrawCommand(command, visible[0]));
}

TEST_F(MeshDrawingPipeline, SharedSubmeshesBindGeometryAndInstanceStreamOncePerSubmit) {
    CpuBuffer instances{128, render::BufferUse::Vertex};
    Input.Layouts.push_back({4, 16, render::VertexStepMode::Instance});
    Input.Attributes.push_back({4, 0, "INSTANCE", 0, render::VertexFormat::FLOAT32X4, 1});
    Batch.Elements.push_back(Batch.Elements[0]);
    Batch.Elements[1].FirstIndex = 5;
    RecordingProcessor processor{Output, Pipeline};
    processor.AllowDeferred = true;
    const array<byte, 4> push{};
    processor.Push = MeshPushConstantBinding{0, 0, push};
    const auto batch = AddBatch();
    Sync(processor);
    vector<VisibleMeshDrawCommand> visible;
    const VisibleMeshBatch request{.Batch = batch};
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands({0}, std::span{&request, 1}, visible));
    CpuParameterSet parameters;
    const MeshShaderParameterBinding passBinding{0, &parameters, {}};
    const MeshPassSubmitArgs args{.ParameterSets = std::span{&passBinding, 1}, .InstanceStream = render::VertexBufferBinding{4, {&instances, 0, 128}}};
    Vertices.DescReads = Indices.DescReads = instances.DescReads = 0;
    for (uint32_t submit = 0; submit < 2; ++submit) {
        CpuEncoder encoder;
        ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible, World.GetCachedDrawList({0}), {}, args));
        ASSERT_EQ(encoder.Draws.size(), 2u);
        EXPECT_EQ(encoder.Draws[0].FirstIndex, 2u);
        EXPECT_EQ(encoder.Draws[1].FirstIndex, 5u);
        ASSERT_EQ(encoder.VertexCalls.size(), 1u);
        ASSERT_EQ(encoder.VertexCalls[0].size(), 2u);
        EXPECT_EQ(encoder.VertexCalls[0][0].Binding, 3u);
        EXPECT_EQ(encoder.VertexCalls[0][1].Binding, 4u);
        EXPECT_EQ(encoder.Indices.size(), 1u);
        EXPECT_EQ(encoder.Stencils.size(), 1u);
        EXPECT_EQ(encoder.Parameters.size(), 1u);
        EXPECT_EQ(encoder.Pushes.size(), 2u);
    }
#if RADRAY_IS_DEBUG
    EXPECT_EQ(parameters.ValidationCalls, 2u);
    EXPECT_EQ(instances.DescReads, 2u);
#else
    EXPECT_EQ(parameters.ValidationCalls, 0u);
    EXPECT_EQ(Vertices.DescReads, 0u);
    EXPECT_EQ(Indices.DescReads, 0u);
    EXPECT_EQ(instances.DescReads, 0u);
#endif
}

TEST_F(MeshDrawingPipeline, SparseDirtyVertexSlotsNeverOverwriteAnUnchangedMiddleSlot) {
    Initializer.VertexBuffers = {{2, {&Vertices, 0, 32}}, {0, {&Vertices, 32, 32}}, {1, {&Vertices, 64, 32}}};
    vector<VisibleMeshDrawCommand> visible;
    Output.BindVisibleCommands(visible);
    for (uint32_t draw = 0; draw < 3; ++draw) {
        if (draw == 1) {
            Initializer.VertexBuffers[0].View.Offset = 16;
            Initializer.VertexBuffers[1].View.Offset = 48;
        }
        auto& command = Output.AddCommand(Initializer);
        Output.FinalizeCommand({Primitive, Batch}, 0, command);
    }
    CpuEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible, nullptr, Output.Commands));
    ASSERT_EQ(encoder.VertexCalls.size(), 3u);
    ASSERT_EQ(encoder.VertexCalls[0].size(), 3u);
    for (uint32_t slot = 0; slot < 3; ++slot) EXPECT_EQ(encoder.VertexCalls[0][slot].Binding, slot);
    ASSERT_EQ(encoder.VertexCalls[1].size(), 1u);
    ASSERT_EQ(encoder.VertexCalls[2].size(), 1u);
    EXPECT_EQ(encoder.VertexCalls[1][0].Binding, 0u);
    EXPECT_EQ(encoder.VertexCalls[2][0].Binding, 2u);
    EXPECT_EQ(encoder.Draws.size(), 3u);
}

TEST_F(MeshDrawingPipeline, StateChangesCompareEveryBufferAndParameterBindingField) {
    CpuBuffer otherVertices{128, render::BufferUse::Vertex};
    CpuBuffer otherIndices{128, render::BufferUse::Index};
    CpuParameterSet parameters;
    CpuParameterSet otherParameters;
    Initializer.ParameterSets.push_back({2, &parameters, {{1, 0, 256}, {2, 0, 512}}});
    vector<VisibleMeshDrawCommand> visible;
    Output.BindVisibleCommands(visible);
    for (uint32_t draw = 0; draw < 13; ++draw) {
        if (draw == 1) Initializer.VertexBuffers[0].View.Target = &otherVertices;
        if (draw == 2) Initializer.VertexBuffers[0].View.Offset += 4;
        if (draw == 3) Initializer.VertexBuffers[0].View.Size -= 4;
        if (draw == 4) Batch.Elements[0].IndexBuffer = &otherIndices;
        if (draw == 5) Batch.Elements[0].IndexBufferOffset += 4;
        if (draw == 6) Batch.Elements[0].IndexStride = 2;
        if (draw == 7) Initializer.StencilReference = 5;
        if (draw == 8) Initializer.ParameterSets[0].ParameterSet = &otherParameters;
        if (draw == 9) Initializer.ParameterSets[0].DynamicOffsets[1].Offset += 256;
        if (draw == 10) Initializer.ParameterSets[0].DynamicOffsets[1].BindingId = 3;
        if (draw == 11) Initializer.ParameterSets[0].DynamicOffsets[1].ArrayIndex = 1;
        if (draw == 12) Initializer.ParameterSets[0].DynamicOffsets.pop_back();
        auto& command = Output.AddCommand(Initializer);
        Output.FinalizeCommand({Primitive, Batch}, 0, command);
    }
    CpuEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible, nullptr, Output.Commands));
    EXPECT_EQ(encoder.Pipelines.size(), 1u);
    ASSERT_EQ(encoder.VertexCalls.size(), 4u);
    EXPECT_EQ(encoder.VertexCalls[1][0].View.Target, &otherVertices);
    EXPECT_EQ(encoder.VertexCalls[2][0].View.Offset, 12u);
    EXPECT_EQ(encoder.VertexCalls[3][0].View.Size, 44u);
    ASSERT_EQ(encoder.Indices.size(), 4u);
    EXPECT_EQ(encoder.Indices[1].Target, &otherIndices);
    EXPECT_EQ(encoder.Indices[2].Offset, 8u);
    EXPECT_EQ(encoder.Indices[3].Stride, 2u);
    EXPECT_EQ(encoder.Stencils, (vector<uint8_t>{0, 5}));
    ASSERT_EQ(encoder.Parameters.size(), 6u);
    EXPECT_EQ(encoder.Parameters[1].ParameterSet.Get(), &otherParameters);
    EXPECT_EQ(encoder.Parameters[2].DynamicOffsets[1].Offset, 768u);
    EXPECT_EQ(encoder.Parameters[3].DynamicOffsets[1].BindingId, 3u);
    EXPECT_EQ(encoder.Parameters[4].DynamicOffsets[1].ArrayIndex, 1u);
    EXPECT_EQ(encoder.Parameters[5].DynamicOffsets.size(), 1u);
    EXPECT_EQ(encoder.Draws.size(), 13u);
}

TEST_F(MeshDrawingPipeline, ZeroInstanceCommandsDoNotRecordOrChangeCachedState) {
    CpuPipeline otherPipeline;
    vector<VisibleMeshDrawCommand> visible;
    Output.BindVisibleCommands(visible);
    for (uint32_t draw = 0; draw < 3; ++draw) {
        Initializer.Pipeline = draw == 1 ? &otherPipeline : &Pipeline;
        auto& command = Output.AddCommand(Initializer);
        Output.FinalizeCommand({Primitive, Batch}, 0, command);
    }
    visible[1].InstanceCount = 0;
    CpuEncoder encoder;
    ASSERT_TRUE(SubmitMeshDrawCommands(encoder, visible, nullptr, Output.Commands));
    EXPECT_EQ(encoder.Pipelines.size(), 1u);
    EXPECT_EQ(encoder.VertexCalls.size(), 1u);
    EXPECT_EQ(encoder.Indices.size(), 1u);
    EXPECT_EQ(encoder.Stencils.size(), 1u);
    EXPECT_EQ(encoder.Draws.size(), 2u);
    visible[1].CommandHandle = {99, 0};
    CpuEncoder invalidHandleEncoder;
#if RADRAY_IS_DEBUG
    EXPECT_FALSE(SubmitMeshDrawCommands(invalidHandleEncoder, visible, nullptr, Output.Commands));
    EXPECT_TRUE(invalidHandleEncoder.Pipelines.empty());
#else
    EXPECT_TRUE(SubmitMeshDrawCommands(invalidHandleEncoder, visible, nullptr, Output.Commands));
    EXPECT_EQ(invalidHandleEncoder.Draws.size(), 2u);
#endif
}

TEST_F(MeshDrawingPipeline, BuildRejectsInvalidSharedStateOrAnyElementBeforeEmitting) {
    RecordingProcessor processor{Output, Pipeline};
    Batch.Elements.insert(Batch.Elements.begin(), MeshBatchElement{});
    Batch.Elements.push_back(Batch.Elements[1]);
    const MeshBatchContext context{Primitive, Batch, {}, {}, &World.Materials.Get(Material)};
    const auto expectInvalid = [&] {
        EXPECT_FALSE(processor.BuildMeshDrawCommands(context, {.Pipeline = &Pipeline}));
        EXPECT_TRUE(Output.Commands.empty());
    };
    Batch.Elements[2].IndexStride = 3;
    expectInvalid();
    Batch.Elements[2].IndexStride = 4;
    Batch.Elements[2].IndexBufferOffset = 3;
    expectInvalid();
    Batch.Elements[2].IndexBufferOffset = 4;
    Batch.Elements[2].IndexBuffer = &Vertices;
    expectInvalid();
    Batch.Elements[2].IndexBuffer = &Indices;
    Batch.Elements[2].IndexCount = std::numeric_limits<uint32_t>::max();
    expectInvalid();
    Batch.Elements[2].IndexCount = 3;
    Batch.Elements[2].FirstInstance = std::numeric_limits<uint32_t>::max();
    Batch.Elements[2].InstanceCount = 2;
    expectInvalid();
    Batch.Elements[2].InstanceCount = 0;
    CpuParameterSet parameters;
    const array<MeshShaderParameterBinding, 2> duplicateGroups{{{1, &parameters, {}}, {1, &parameters, {}}}};
    EXPECT_FALSE(processor.BuildMeshDrawCommands(context, {.Pipeline = &Pipeline, .ParameterSets = duplicateGroups}));
    array<byte, 3> invalidPush{};
    EXPECT_FALSE(processor.BuildMeshDrawCommands(context, {.Pipeline = &Pipeline, .PushConstants = MeshPushConstantBinding{0, 0, invalidPush}}));
    EXPECT_TRUE(Output.Commands.empty());
    ASSERT_TRUE(processor.BuildMeshDrawCommands(context, {.Pipeline = &Pipeline}));
    ASSERT_EQ(Output.Commands.size(), 1u);
    EXPECT_EQ(Output.Commands[0].FirstIndex, 2u);
}

TEST_F(MeshDrawingPipeline, BatchUpdatesPreserveSceneLinksAndQueueEachBatchOnce) {
    RecordingProcessor processor{Output, Pipeline};
    vector<SparseSetHandle> owners;
    vector<SparseSetHandle> batches;
    vector<SparseSetHandle> retained;
    for (uint32_t index = 0; index < 128; ++index) {
        const auto owner = World.AddPrimitive();
        owners.push_back(owner);
        batches.push_back(World.AddMeshBatch(owner, Batch));
        retained.push_back(World.AddMeshBatch(owner, Batch));
    }
    Sync(processor);
    vector<SparseSetHandle> oldCommands;
    vector<SparseSetHandle> retainedCommands;
    for (size_t index = 0; index < batches.size(); ++index) {
        oldCommands.push_back(CommandHandle(batches[index]));
        retainedCommands.push_back(CommandHandle(retained[index]));
        EXPECT_FALSE(World.Meshes.Get(batches[index]).Dirty);
    }
    for (uint32_t update = 0; update < 3; ++update) {
        Batch.OwningPrimitive = PrimitiveHandle;
        Batch.Elements[0].FirstIndex = update + 3;
        for (size_t index = 0; index < batches.size(); ++index) {
            ASSERT_TRUE(World.UpdateMeshBatch(batches[index], Batch));
            World.MarkBatchDirty(batches[index]);
            EXPECT_EQ(World.Meshes.Get(batches[index]).OwningPrimitive, owners[index]);
            EXPECT_TRUE(World.Meshes.Get(batches[index]).Dirty);
            EXPECT_EQ(CommandHandle(batches[index]), oldCommands[index]);
        }
        EXPECT_EQ(World.GetDirtyBatches().size(), batches.size());
    }
    vector<VisibleMeshDrawCommand> visible(1);
    const array<VisibleMeshBatch, 2> requests{{{.Batch = retained[0]}, {.Batch = batches[0]}}};
    EXPECT_FALSE(World.GatherVisibleMeshDrawCommands({0}, requests, visible));
    EXPECT_EQ(visible.size(), 1u);
    ASSERT_TRUE(World.RemoveMeshBatch(owners[0], batches[0]));
    EXPECT_EQ(World.GetDirtyBatches().size(), batches.size() - 1);
    processor.Calls.clear();
    Sync(processor);
    EXPECT_EQ(processor.Calls.size(), batches.size() - 1);
    EXPECT_FALSE(World.HasDirtyBatches());
    for (size_t index = 1; index < batches.size(); ++index) {
        EXPECT_FALSE(World.Meshes.Get(batches[index]).Dirty);
        EXPECT_NE(CommandHandle(batches[index]), oldCommands[index]);
        EXPECT_EQ(CommandHandle(retained[index]), retainedCommands[index]);
    }
}

TEST_F(MeshDrawingPipeline, FailedSyncRequeuesDirtyBatchUntilGeometryIsRepaired) {
    RecordingProcessor processor{Output, Pipeline};
    const auto batch = AddBatch();
    Sync(processor);
    World.Meshes.Get(batch).Elements[0].IndexStride = 3;
    World.MarkBatchDirty(batch);
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        Sync(processor);
        EXPECT_TRUE(World.Meshes.Get(batch).Dirty);
        EXPECT_TRUE(World.Meshes.Get(batch).CachedCommands.empty());
        ASSERT_EQ(World.GetDirtyBatches().size(), 1u);
        EXPECT_EQ(World.GetDirtyBatches()[0], batch);
    }
    World.Meshes.Get(batch).Elements[0].IndexStride = 4;
    Sync(processor);
    EXPECT_FALSE(World.Meshes.Get(batch).Dirty);
    EXPECT_FALSE(World.HasDirtyBatches());
    EXPECT_EQ(World.Meshes.Get(batch).CachedCommands.size(), 1u);
    ASSERT_TRUE(World.RemoveMeshBatch(PrimitiveHandle, batch));
    Batch.Dirty = true;
    Batch.OwningPrimitive = {};
    const auto replacement = AddBatch();
    EXPECT_NE(replacement, batch);
    EXPECT_EQ(World.Meshes.Get(replacement).OwningPrimitive, PrimitiveHandle);
    EXPECT_TRUE(World.Meshes.Get(replacement).Dirty);
    EXPECT_EQ(World.GetDirtyBatches().size(), 1u);
    ASSERT_TRUE(World.RemoveMeshBatch(PrimitiveHandle, replacement));
    EXPECT_FALSE(World.HasDirtyBatches());
}

TEST_F(MeshDrawingPipeline, SceneAdoptsUnownedBatchesAndResolvesDirectInsertions) {
    const auto adopted = World.Meshes.Emplace(Batch);
    PrimitiveSceneInfo adoptingPrimitive;
    adoptingPrimitive.Meshes.push_back(adopted);
    const auto owner = World.AddPrimitive(adoptingPrimitive);
    ASSERT_TRUE(owner.IsValid());
    EXPECT_EQ(World.Meshes.Get(adopted).OwningPrimitive, owner);
    EXPECT_TRUE(World.Meshes.Get(adopted).Dirty);
    EXPECT_FALSE(World.AddPrimitive(adoptingPrimitive).IsValid());
    RecordingProcessor processor{Output, Pipeline};
    Sync(processor);
    const auto inserted = World.Meshes.Emplace(Batch);
    PrimitiveSceneInfo directPrimitive;
    directPrimitive.Meshes.push_back(inserted);
    const auto directOwner = World.Primitives.Emplace(std::move(directPrimitive));
    ASSERT_TRUE(World.UpdateMeshBatch(inserted, Batch));
    EXPECT_EQ(World.Meshes.Get(inserted).OwningPrimitive, directOwner);
    Sync(processor);
    EXPECT_FALSE(World.Meshes.Get(inserted).Dirty);
    EXPECT_EQ(World.Meshes.Get(inserted).CachedCommands.size(), 1u);
    ASSERT_TRUE(World.RemovePrimitive(directOwner));
    EXPECT_FALSE(World.Meshes.IsAlive(inserted));
    EXPECT_TRUE(World.Meshes.IsAlive(adopted));
}

TEST_F(MeshDrawingPipeline, GatherUsesCachedInstanceRangesAndAppliesIndependentOverrides) {
    Batch.Elements[0].FirstInstance = 7;
    Batch.Elements[0].InstanceCount = 3;
    Batch.Elements.push_back(Batch.Elements[0]);
    Batch.Elements[1].FirstInstance = 11;
    Batch.Elements[1].InstanceCount = 2;
    const auto batch = AddBatch();
    RecordingProcessor processor{Output, Pipeline};
    Sync(processor);
    auto& commands = World.CachedDrawLists[0].Commands;
    for (auto& command : commands.Values()) {
        command.FirstInstance = 99;
        command.InstanceCount = 99;
    }
    const array<VisibleMeshBatch, 4> requests{{{.Batch = batch},
                                               {.Batch = batch, .ElementIndex = 1, .FirstInstanceOverride = 20},
                                               {.Batch = batch, .ElementIndex = 0, .InstanceCountOverride = 0},
                                               {.Batch = batch, .ElementIndex = 1, .FirstInstanceOverride = 30, .InstanceCountOverride = 4}}};
    vector<VisibleMeshDrawCommand> visible;
    ASSERT_TRUE(World.GatherVisibleMeshDrawCommands({0}, requests, visible));
    ASSERT_EQ(visible.size(), 5u);
    EXPECT_EQ(visible[0].FirstInstance, 7u);
    EXPECT_EQ(visible[0].InstanceCount, 3u);
    EXPECT_EQ(visible[1].FirstInstance, 11u);
    EXPECT_EQ(visible[1].InstanceCount, 2u);
    EXPECT_EQ(visible[2].FirstInstance, 20u);
    EXPECT_EQ(visible[2].InstanceCount, 2u);
    EXPECT_EQ(visible[3].FirstInstance, 7u);
    EXPECT_EQ(visible[3].InstanceCount, 0u);
    EXPECT_EQ(visible[4].FirstInstance, 30u);
    EXPECT_EQ(visible[4].InstanceCount, 4u);
    commands.Destroy(World.Meshes.Get(batch).CachedCommands[1].CommandHandle);
    EXPECT_FALSE(World.GatherVisibleMeshDrawCommands({0}, requests, visible));
    EXPECT_EQ(visible.size(), 5u);
}

TEST_F(MeshDrawingPipeline, PassRegistrationChangesRebuildAndRestoreContexts) {
    RecordingProcessor processor{Output, Pipeline};
    const auto batch = AddBatch();
    Sync(processor);
    const auto first = CommandHandle(batch);
    World.MarkBatchDirty(batch);
    RecordingProcessor replacement{Output, Pipeline};
    const array<MeshPassRegistration, 2> passes{{{{0}, &replacement}, {{1}, &processor}}};
    World.SyncCachedMeshDrawCommands(passes);
    EXPECT_NE(CommandHandle(batch), first);
    EXPECT_FALSE(World.Meshes.Get(batch).Dirty);
    EXPECT_FALSE(World.HasDirtyBatches());
    EXPECT_EQ(replacement.Calls.size(), 1u);
    EXPECT_EQ(&replacement.DrawListContext(), &Output);
    World.SyncCachedMeshDrawCommands({});
    EXPECT_TRUE(World.Meshes.Get(batch).CachedCommands.empty());
    EXPECT_EQ(World.GetCachedDrawList({0})->Commands.Count(), 0u);
    EXPECT_EQ(World.GetCachedDrawList({1})->Commands.Count(), 0u);
}

}  // namespace
}  // namespace radray
