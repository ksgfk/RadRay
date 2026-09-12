#include "foundation_graph_fixture.h"
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/scope_guard.h>

namespace radray {
struct ReadyWorkspaceTestAccess {
    static const void* Storage(const PreparedRendererList& ready) { return ready.StorageOwner.get(); }
    static size_t DrawCount(const PreparedRendererList& ready) { return ready.Draws.size(); }
    static const void* Draws(const PreparedRendererList& ready) { return ready.Draws.data(); }
    static std::span<const render::ShaderParameterDynamicOffset> Offsets(const PreparedRendererList& ready) {
        return ready.LocalGroups.empty() ? std::span<const render::ShaderParameterDynamicOffset>{} : std::span<const render::ShaderParameterDynamicOffset>{ready.LocalGroups.front().DynamicOffsets};
    }
};
namespace {
using ReadyRows = vector<std::optional<PreparedRendererList>>;
struct WorkspaceFlight {
    explicit WorkspaceFlight(render::Device& device) : Groups(&device) {}
    FrameDrawResources Groups;
    array<RendererList, 3> Lists;
    ReadyRows Ready;
    void Clear() {
        Ready.clear();
        for (auto& list : Lists) list.ResetForReuse();
        Groups.ClearSets();
    }
    ~WorkspaceFlight() { Clear(); }
};
class ReadyWorkspaceTest : public test::FoundationGraphGpuTest {
protected:
    bool Initialize() {
        auto& device = *Context.Device;
        render::ShaderProgramLayoutRecipe recipe;
        for (const auto name : {"A", "B", "C"}) {
            const render::ShaderLayoutSelector selector{.DeclarationName = name, .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
            recipe.D3D12.BufferPlacements.push_back({selector, render::D3D12BufferPlacement::RootDescriptor});
            recipe.Vulkan.BufferDescriptors.push_back({selector, render::VulkanBufferDescriptorPlacement::Dynamic});
        }
        auto program = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
struct Data { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Data> A : register(b0);
VK_BINDING(1, 0) ConstantBuffer<Data> B : register(b1);
VK_BINDING(2, 0) ConstantBuffer<Data> C : register(b2);
[shader("vertex")] float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1); }
[shader("pixel")] float PSMain() : SV_Target0 { return A.Value.x + B.Value.x + C.Value.x; }
)hlsl",
                                                       recipe);
        if (!program) return false;
        Program = program.Release();
        const array<float, 9> vertices{-1, -1, 0, 3, -1, 0, -1, 3, 0};
        const array<uint32_t, 3> indices{0, 1, 2};
        auto vertex = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{vertices}), render::BufferUse::Vertex);
        auto index = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
        if (!vertex || !index) return false;
        Vertex = vertex.Release();
        Index = index.Release();
        Geometry.VertexBuffers = {{0, {Vertex.get(), 0, sizeof(vertices)}}};
        Geometry.Ibv = {Index.get(), 0, 4};
        Geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
        Geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
        Layout = Layouts.Intern(Geometry.VertexLayout);
        return Layout.IsValid();
    }
    bool Fill(WorkspaceFlight& flight, array<uint32_t, 3> counts, uint64_t serial, bool legacy = false) {
        for (auto& list : flight.Lists) list.ResetForReuse();
        if (!flight.Groups.BeginFrame(Writes)) return false;
        ShaderParameterStorage values{&Program->GetParameterLayout(), 0};
        if (!values.SetFloat4("A.Value", {.1f + .1f * float(serial % 2), 0, 0, 0}) ||
            !values.SetFloat4("B.Value", {.2f, 0, 0, 0}) || !values.SetFloat4("C.Value", {.3f, 0, 0, 0})) return false;
        const auto group = flight.Groups.PrepareGroup(*Program, 0, values);
        if (!group || group->DynamicOffsets.size() != 3) return false;
        for (uint32_t i = 0; i < 3; ++i) {
            for (uint32_t draw = 0; draw < counts[i]; ++draw) {
                MeshDrawCommand command;
                command.Program = Program.get();
                command.Geometry = &Geometry;
                command.LayoutId = Layout;
                command.IndexCount = 3;
                command.PipelineState.Primitive.Cull = render::CullMode::None;
                command.PipelineState.DepthStencil.DepthTestEnable = command.PipelineState.DepthStencil.DepthWriteEnable = false;
                command.Groups.push_back(*group);
                if (!flight.Lists[i].AppendDynamic(std::move(command), legacy ? nullptr : &flight.Groups)) return false;
            }
        }
        return true;
    }
    RgTextureValue AddDraw(RenderGraph& graph, WorkspaceFlight& flight) {
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "ready workspace");
        struct Payload {
            WorkspaceFlight* Flight;
            render::RenderBackend Backend;
        };
        graph.AddRasterPass<Payload>("workspace lists", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data.Flight = &flight; data.Backend = GetParam();
            builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
            data.Flight->Ready.clear();
            for (const auto& list : data.Flight->Lists) {
                auto ready = PrepareRendererList(list, context);
                if (!ready) return false;
                data.Flight->Ready.push_back(std::move(ready));
            }
            return true; }, +[](const Payload& data, RenderGraphRasterContext& context) {
            context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 4, 4));
            context.Encoder().SetScissor({0, 0, 4, 4});
            for (size_t i = 0; i < data.Flight->Ready.size(); ++i) {
                DrawExecutionStats stats;
                RecordRendererList(*data.Flight->Ready[i], context, stats);
                EXPECT_TRUE(stats.Succeeded());
                EXPECT_EQ(stats.Draws, data.Flight->Lists[i].GetDrawCount());
            } });
        return color;
    }
    void CheckValue(const RgReadbackTicket& readback, float expected) {
        vector<byte> bytes;
        ASSERT_TRUE(readback.Read(bytes));
        ASSERT_GE(bytes.size(), 4u);
        float value;
        std::memcpy(&value, bytes.data(), 4);
        EXPECT_NEAR(value, expected, 1e-6f);
    }
    void TearDown() override {
        if (Resources) Resources->Clear();
        Program.reset();
        Index.reset();
        Vertex.reset();
        test::FoundationGraphGpuTest::TearDown();
    }
    unique_ptr<ShaderProgram> Program;
    unique_ptr<render::Buffer> Vertex, Index;
    GpuMesh::DrawData Geometry;
    PrimitiveVertexLayoutRegistry Layouts;
    PrimitiveVertexLayoutId Layout;
};

TEST_P(ReadyWorkspaceTest, ThreeFlightsRetainReadyScratchAndSpilledOffsetsFor1000Frames) {
    ASSERT_TRUE(Initialize());
    array<unique_ptr<WorkspaceFlight>, 3> flights;
    for (auto& flight : flights) flight = make_unique<WorkspaceFlight>(*Context.Device);
    array<size_t, 3> capacity{};
    array<array<const void*, 3>, 3> owners{}, draws{}, offsets{};
    for (uint32_t frame = 0; frame < 1024; ++frame) {
        const uint32_t slot = frame % 3;
        auto& flight = *flights[slot];
        flight.Ready.clear();
        Writes.Reset();
        Resources->BeginFlight(frame + 1, Writes);
        const array<uint32_t, 3> counts = frame % 2 ? array<uint32_t, 3>{48, 8, 16} : array<uint32_t, 3>{8, 48, 16};
        ASSERT_TRUE(Fill(flight, counts, frame));
        RenderGraph graph{*Context.Device, *Resources, *Registry, "ready capacity", kPerformanceRenderGraphRuntimeOptions};
        const auto color = AddDraw(graph, flight);
        const auto readback = frame == 1023 ? graph.ReadbackTexture("ready pixel", color) : RgReadbackTicket{};
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 72u);
        for (uint32_t list = 0; list < 3; ++list) {
            const auto& ready = *flight.Ready[list];
            const auto nativeOffsets = ReadyWorkspaceTestAccess::Offsets(ready);
            ASSERT_EQ(nativeOffsets.size(), 3u);
            EXPECT_EQ(ReadyWorkspaceTestAccess::DrawCount(ready), counts[list]);
            const void* owner = ReadyWorkspaceTestAccess::Storage(ready);
            for (uint32_t earlier = 0; earlier < list; ++earlier)
                EXPECT_NE(owner, ReadyWorkspaceTestAccess::Storage(*flight.Ready[earlier]));
            if (frame < 24) {
                owners[slot][list] = owner;
                draws[slot][list] = ReadyWorkspaceTestAccess::Draws(ready);
                offsets[slot][list] = nativeOffsets.data();
            } else {
                EXPECT_EQ(owner, owners[slot][list]);
                EXPECT_EQ(ReadyWorkspaceTestAccess::Draws(ready), draws[slot][list]);
                EXPECT_EQ(nativeOffsets.data(), offsets[slot][list]);
            }
        }
        if (frame < 24)
            capacity[slot] = flight.Groups.GetCacheCapacityBytes();
        else
            EXPECT_EQ(flight.Groups.GetCacheCapacityBytes(), capacity[slot]);
        if (frame == 1023) CheckValue(readback, .7f);
    }
}

TEST_P(ReadyWorkspaceTest, LiveLeaseSurvivesAnotherEpochAndPoolGrowth) {
    ASSERT_TRUE(Initialize());
    WorkspaceFlight flight{*Context.Device};
    ASSERT_TRUE(Fill(flight, {8, 8, 8}, 0));
    {
        auto graph = MakeGraph("initial ready lease");
        AddDraw(graph, flight);
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    }
    auto previous = std::move(flight.Ready);
    array<const void*, 3> owners{}, offsets{};
    array<vector<render::ShaderParameterDynamicOffset>, 3> savedOffsets;
    for (size_t i = 0; i < previous.size(); ++i) {
        owners[i] = ReadyWorkspaceTestAccess::Storage(*previous[i]);
        const auto values = ReadyWorkspaceTestAccess::Offsets(*previous[i]);
        offsets[i] = values.data();
        savedOffsets[i].assign(values.begin(), values.end());
    }
    Writes.Reset();
    Resources->BeginFlight(2, Writes);
    flight.Groups.ClearSets();
    ASSERT_TRUE(Fill(flight, {128, 2, 32}, 1));
    {
        auto graph = MakeGraph("new epoch ready lease");
        const auto output = AddDraw(graph, flight);
        auto readback = graph.ReadbackTexture("new epoch pixel", output);
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        CheckValue(readback, .7f);
    }
    for (size_t i = 0; i < previous.size(); ++i) {
        EXPECT_EQ(ReadyWorkspaceTestAccess::Storage(*previous[i]), owners[i]);
        EXPECT_EQ(ReadyWorkspaceTestAccess::DrawCount(*previous[i]), 8u);
        const auto values = ReadyWorkspaceTestAccess::Offsets(*previous[i]);
        EXPECT_EQ(values.data(), offsets[i]);
        EXPECT_TRUE(std::equal(values.begin(), values.end(), savedOffsets[i].begin(), savedOffsets[i].end()));
        for (const auto& ready : flight.Ready) EXPECT_NE(ReadyWorkspaceTestAccess::Storage(*ready), owners[i]);
    }
}

TEST_P(ReadyWorkspaceTest, MovedFromReadyCannotRecordAndLegacyFallbackStillDraws) {
    ASSERT_TRUE(Initialize());
    WorkspaceFlight flight{*Context.Device};
    for (const bool legacy : {false, true}) {
        flight.Clear();
        Resources->BeginFlight(legacy ? 2 : 1, Writes);
        ASSERT_TRUE(Fill(flight, {8, 8, 8}, 0, legacy));
        RenderGraph graph{*Context.Device, *Resources, *Registry, "moved ready", kPerformanceRenderGraphRuntimeOptions};
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
        struct Payload {
            WorkspaceFlight* Flight;
            std::optional<PreparedRendererList> Original, Moved;
            render::RenderBackend Backend;
        };
        graph.AddRasterPass<Payload>("move ready", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data.Flight = &flight; data.Backend = GetParam(); builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
            data.Original = PrepareRendererList(data.Flight->Lists[0], context);
            if (!data.Original) return false;
            data.Moved = std::move(data.Original);
            auto other = PrepareRendererList(data.Flight->Lists[1], context);
            return other.has_value(); }, +[](const Payload& data, RenderGraphRasterContext& context) {
            context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 4, 4)); context.Encoder().SetScissor({0, 0, 4, 4});
            DrawExecutionStats moved;
            RecordRendererList(*data.Moved, context, moved);
            EXPECT_EQ(moved.Draws, 8u);
            EXPECT_TRUE(moved.Succeeded());
            DrawExecutionStats original;
            RecordRendererList(*data.Original, context, original);
            EXPECT_EQ(original.Draws, 0u);
            EXPECT_FALSE(original.Succeeded()); });
        EXPECT_FALSE(Run(graph));
        EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 8u);
        EXPECT_EQ(graph.GetFirstErrorCode(), "RasterExecution");
    }
}

TEST_P(ReadyWorkspaceTest, EpochChangeRejectsReadyBeforeAnyDrawInOffAndFull) {
    ASSERT_TRUE(Initialize());
    WorkspaceFlight flight{*Context.Device};
    uint64_t serial = 0;
    for (const auto options : {kPerformanceRenderGraphRuntimeOptions, kDiagnosticRenderGraphRuntimeOptions}) {
        flight.Clear();
        Writes.Reset();
        Resources->BeginFlight(++serial, Writes);
        ASSERT_TRUE(Fill(flight, {8, 8, 8}, 0));
        RenderGraph graph{*Context.Device, *Resources, *Registry, "expired lease", options};
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
        struct Payload {
            WorkspaceFlight* Flight;
            HostWriteBatch* Writes;
            std::optional<PreparedRendererList> Ready;
        };
        graph.AddRasterPass<Payload>("expire ready", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data.Flight = &flight; data.Writes = &Writes; builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
            data.Ready = PrepareRendererList(data.Flight->Lists[0], context);
            if (!data.Ready) return false;
            return data.Flight->Groups.BeginFrame(*data.Writes); }, +[](const Payload& data, RenderGraphRasterContext& context) {
            DrawExecutionStats stats;
            RecordRendererList(*data.Ready, context, stats);
            EXPECT_EQ(stats.Draws, 0u);
            EXPECT_FALSE(stats.Succeeded()); });
        EXPECT_FALSE(Run(graph));
        EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 0u);
        EXPECT_EQ(graph.GetReport().CommandCalls.SetPipeline, 0u);
        EXPECT_EQ(graph.GetReport().CommandCalls.SetParameters, 0u);
    }
}

TEST_P(ReadyWorkspaceTest, MoveAssignmentReleasesOccupiedLeaseWithoutChangingTransferredData) {
    ASSERT_TRUE(Initialize());
    WorkspaceFlight flight{*Context.Device};
    uint64_t serial = 0;
    for (const auto options : {kPerformanceRenderGraphRuntimeOptions, kDiagnosticRenderGraphRuntimeOptions}) {
        flight.Clear();
        Writes.Reset();
        Resources->BeginFlight(++serial, Writes);
        ASSERT_TRUE(Fill(flight, {8, 48, 16}, 0));
        for (uint32_t list = 1; list < flight.Lists.size(); ++list) {
            ShaderParameterStorage values{&Program->GetParameterLayout(), 0};
            ASSERT_TRUE(values.SetFloat4("A.Value", {list == 1 ? .4f : .8f, 0, 0, 0}));
            ASSERT_TRUE(values.SetFloat4("B.Value", {.2f, 0, 0, 0}));
            ASSERT_TRUE(values.SetFloat4("C.Value", {.3f, 0, 0, 0}));
            const auto group = flight.Groups.PrepareGroup(*Program, 0, values);
            ASSERT_TRUE(group);
            ASSERT_EQ(group->DynamicOffsets.size(), 3u);
            for (auto& command : flight.Lists[list].Commands) command.Groups[0] = *group;
        }
        struct MoveState {
            WorkspaceFlight* Flight;
            render::RenderBackend Backend;
            std::optional<PreparedRendererList> Source, Target, Replacement;
            array<render::ShaderParameterDynamicOffset, 3> SourceOffsets;
            const void* SourceDraws{nullptr};
        } state{&flight, GetParam()};
        RenderGraph graph{*Context.Device, *Resources, *Registry, "occupied Ready lease", options};
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "moved lease pixels");
        struct Payload {
            MoveState* State;
        };
        graph.AddRasterPass<Payload>("assign occupied Ready", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data.State = &state; builder.SetColorAttachment(0, color); }, +[](Payload& data, RenderGraphPrepareContext& context) {
            auto& current = *data.State;
            current.Source = PrepareRendererList(current.Flight->Lists[0], context);
            current.Target = PrepareRendererList(current.Flight->Lists[1], context);
            if (!current.Source || !current.Target) return false;
            const auto sourceOffsets = ReadyWorkspaceTestAccess::Offsets(*current.Source);
            const auto targetOffsets = ReadyWorkspaceTestAccess::Offsets(*current.Target);
            EXPECT_EQ(sourceOffsets.size(), 3u); EXPECT_EQ(targetOffsets.size(), 3u);
            if (sourceOffsets.size() != 3 || targetOffsets.size() != 3) return false;
            EXPECT_FALSE(std::equal(sourceOffsets.begin(), sourceOffsets.end(), targetOffsets.begin(), targetOffsets.end()));
            std::copy(sourceOffsets.begin(), sourceOffsets.end(), current.SourceOffsets.begin());
            current.SourceDraws = ReadyWorkspaceTestAccess::Draws(*current.Source);
            const auto sourceOwner = ReadyWorkspaceTestAccess::Storage(*current.Source);
            const auto releasedOwner = ReadyWorkspaceTestAccess::Storage(*current.Target);
            EXPECT_NE(sourceOwner, releasedOwner);
            EXPECT_EQ(ReadyWorkspaceTestAccess::DrawCount(*current.Source), 8u);
            EXPECT_EQ(ReadyWorkspaceTestAccess::DrawCount(*current.Target), 48u);
            *current.Target = std::move(*current.Source);
            EXPECT_EQ(ReadyWorkspaceTestAccess::Storage(*current.Source), nullptr);
            EXPECT_EQ(ReadyWorkspaceTestAccess::Storage(*current.Target), sourceOwner);
            current.Replacement = PrepareRendererList(current.Flight->Lists[2], context);
            if (!current.Replacement) return false;
            EXPECT_EQ(ReadyWorkspaceTestAccess::Storage(*current.Replacement), releasedOwner);
            EXPECT_EQ(ReadyWorkspaceTestAccess::DrawCount(*current.Replacement), 16u);
            EXPECT_EQ(ReadyWorkspaceTestAccess::DrawCount(*current.Target), 8u);
            EXPECT_EQ(ReadyWorkspaceTestAccess::Draws(*current.Target), current.SourceDraws);
            const auto transferred = ReadyWorkspaceTestAccess::Offsets(*current.Target);
            EXPECT_TRUE(std::equal(transferred.begin(), transferred.end(), current.SourceOffsets.begin(), current.SourceOffsets.end()));
            const auto replacement = ReadyWorkspaceTestAccess::Offsets(*current.Replacement);
            EXPECT_FALSE(std::equal(transferred.begin(), transferred.end(), replacement.begin(), replacement.end()));
            return true; }, +[](const Payload& data, RenderGraphRasterContext& context) {
            const auto& current = *data.State;
            context.Encoder().SetViewport(MakeViewport(current.Backend, 0, 0, 4, 4));
            context.Encoder().SetScissor({0, 0, 2, 4});
            DrawExecutionStats target;
            RecordRendererList(*current.Target, context, target);
            EXPECT_TRUE(target.Succeeded()); EXPECT_EQ(target.Draws, 8u);
            context.Encoder().SetScissor({2, 0, 2, 4});
            DrawExecutionStats replacement;
            RecordRendererList(*current.Replacement, context, replacement);
            EXPECT_TRUE(replacement.Succeeded()); EXPECT_EQ(replacement.Draws, 16u); });
        const auto readback = graph.ReadbackTexture("transferred and reused lease", color);
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 24u);
        vector<byte> pixels;
        ASSERT_TRUE(readback.Read(pixels));
        ASSERT_GE(pixels.size(), 4 * sizeof(float));
        float transferredPixel, replacementPixel;
        std::memcpy(&transferredPixel, pixels.data(), sizeof(float));
        std::memcpy(&replacementPixel, pixels.data() + 2 * sizeof(float), sizeof(float));
        EXPECT_NEAR(transferredPixel, .6f, 1e-6f);
        EXPECT_NEAR(replacementPixel, 1.3f, 1e-6f);

        RenderGraph rejected{*Context.Device, *Resources, *Registry, "moved source lease", options};
        const auto rejectedColor = rejected.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "rejected source");
        rejected.AddRasterPass<Payload>("reject moved source", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data.State = &state; builder.SetColorAttachment(0, rejectedColor); builder.SetSideEffect(); }, +[](const Payload& data, RenderGraphRasterContext& context) {
            DrawExecutionStats stats;
            RecordRendererList(*data.State->Source, context, stats);
            EXPECT_EQ(stats.Draws, 0u); EXPECT_EQ(stats.BindingFailure, 1u); EXPECT_EQ(stats.Skipped, 0u); });
        EXPECT_FALSE(Run(rejected));
        EXPECT_EQ(rejected.GetReport().CommandCalls.DrawIndexed, 0u);
        EXPECT_EQ(rejected.GetReport().CommandCalls.SetPipeline, 0u);
        EXPECT_EQ(rejected.GetReport().CommandCalls.SetParameters, 0u);
        EXPECT_EQ(rejected.GetFirstErrorCode(), "RasterExecution");
    }
}
INSTANTIATE_TEST_SUITE_P(Backends, ReadyWorkspaceTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
