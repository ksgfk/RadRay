#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include "stage_b_test_support.h"
#include "runtime_profile_support.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>

#include <gtest/gtest.h>
#include <fmt/format.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/viewport.h>

namespace radray {
namespace {

constexpr uint32_t kRecordDraws = 1024;
constexpr uint32_t kRecordSize = 16;

struct RecordedGroup {
    uint32_t Group;
    render::ShaderParameterSet* Set;
    vector<render::ShaderParameterDynamicOffset> Offsets;
    bool operator==(const RecordedGroup&) const = default;
};
struct RecordedVertex {
    uint32_t Binding;
    render::Buffer* Buffer;
    uint64_t Offset, Size;
    bool operator==(const RecordedVertex&) const = default;
};
struct RecordedDraw {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    vector<RecordedGroup> Groups;
    vector<RecordedVertex> Vertices;
    Nullable<render::Buffer*> IndexBuffer{nullptr};
    uint32_t IndexOffset{}, IndexStride{};
    array<float, 6> Viewport{};
    array<int64_t, 4> Scissor{};
    array<int64_t, 5> Arguments{};
    bool operator==(const RecordedDraw&) const = default;
};

class RecordTraceEncoder final : public render::GraphicsCommandEncoder {
public:
    explicit RecordTraceEncoder(render::CommandBuffer& commands) : _commands(commands) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::CommandBuffer* GetCommandBuffer() const noexcept override { return &_commands; }
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set,
                                std::span<const render::ShaderParameterDynamicOffset> offsets) noexcept override {
        _groups.insert_or_assign(group, RecordedGroup{group, set, {offsets.begin(), offsets.end()}});
    }
    bool SetPushConstants(render::BindingHandle, std::span<const byte>) noexcept override {
        UnexpectedCommand = true;
        return false;
    }
    void SetViewport(Viewport value) noexcept override { _state.Viewport = {value.X, value.Y, value.Width, value.Height, value.MinDepth, value.MaxDepth}; }
    void SetScissor(Rect value) noexcept override { _state.Scissor = {value.X, value.Y, value.Width, value.Height}; }
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> values) noexcept override {
        for (const auto& value : values)
            _vertices.insert_or_assign(value.Binding, RecordedVertex{value.Binding, value.View.Target, value.View.Offset, value.View.Size});
    }
    void BindIndexBuffer(render::IndexBufferView value) noexcept override {
        _state.IndexBuffer = value.Target;
        _state.IndexOffset = value.Offset;
        _state.IndexStride = value.Stride;
    }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* value) noexcept override {
        if (_state.Pipeline.Get() != value) _groups.clear();
        _state.Pipeline = value;
    }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override { UnexpectedCommand = true; }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance) noexcept override {
        _state.Arguments = {count, instances, first, offset, firstInstance};
        _state.Groups.clear();
        _state.Vertices.clear();
        for (const auto& [key, value] : _groups) _state.Groups.push_back(value);
        for (const auto& [key, value] : _vertices) _state.Vertices.push_back(value);
        Draws.push_back(_state);
    }
    void DrawIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { UnexpectedCommand = true; }
    void DrawIndexedIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { UnexpectedCommand = true; }
    vector<RecordedDraw> Draws;
    bool UnexpectedCommand{false};

private:
    render::CommandBuffer& _commands;
    RecordedDraw _state;
    map<uint32_t, RecordedGroup> _groups;
    map<uint32_t, RecordedVertex> _vertices;
};

struct RecordFixture {
    GpuMesh::DrawData Geometry;
    array<PreparedShaderGroup, 2> Colors;
    PreparedShaderGroup View, Object;
    array<uint8_t, 3> GroupOrder{0, 1, 2};
    array<render::GraphicsPipelineState*, 2> Pipelines{};
    RenderSceneSnapshot Snapshot;
    RendererList List;
    render::RenderBackend Backend;
};

void SetRecordViewport(render::GraphicsCommandEncoder& encoder, render::RenderBackend backend) {
    encoder.SetViewport(MakeViewport(backend, kRecordSize, kRecordSize));
    encoder.SetScissor({0, 0, kRecordSize, kRecordSize});
}

// This reference knows the fixture's two states, one geometry, and three dynamic groups.
// It consumes the same resolved PSOs and sets as the runtime without interpreting a renderer list.
void RecordReference(const RecordFixture& fixture, render::GraphicsCommandEncoder& encoder) {
    RADRAY_PROFILE_SCOPE_N("RecordReference");
    Nullable<render::GraphicsPipelineState*> lastPipeline{nullptr};
    uint32_t lastColor = UINT32_MAX;
    for (uint32_t index = 0; index < kRecordDraws; ++index) {
        auto* pipeline = fixture.Pipelines[(index / 256) % 2];
        const uint32_t color = (index / 2) % 2;
        if (lastPipeline.Get() != pipeline) {
            encoder.BindGraphicsPipelineState(pipeline);
            encoder.BindVertexBuffers(fixture.Geometry.VertexBuffers);
            encoder.BindIndexBuffer(fixture.Geometry.Ibv);
            lastPipeline = pipeline;
            const PreparedShaderGroup* groups[]{&fixture.View, &fixture.Colors[color], &fixture.Object};
            for (const auto at : fixture.GroupOrder) {
                const auto& group = *groups[at];
                encoder.BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets);
            }
            lastColor = color;
        }
        if (color != lastColor) {
            const auto& group = fixture.Colors[color];
            encoder.BindShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets);
            lastColor = color;
        }
        encoder.DrawIndexed(6, 1, 0, 0, 0);
    }
}

class RuntimeRecordReference : public testing::TestWithParam<render::RenderBackend> {};

TEST_P(RuntimeRecordReference, MatchesHandwrittenRhiStateAndPixelsWithAlternatingRecordOrder) {
    const auto* extended = std::getenv("RADRAY_RECORD_PROFILE");
    const bool extendedProfile = extended && std::string_view{extended} == "1";
    const uint32_t warmup = extendedProfile ? 120 : 2;
    const uint32_t samples = extendedProfile ? 1000 : 4;
    const uint32_t rounds = extendedProfile ? 5 : 1;
    profile::PrintBuildIdentity();
    fmt::print("RECORD_OPTIONS {{\"sourceRoot\":{:?},\"backend\":{:?},\"draws\":{},\"warmup\":{},\"samples\":{},\"rounds\":{},\"width\":{},\"height\":{},\"driverValidation\":false,\"rgValidation\":\"off\",\"gpuMarkers\":false,\"queueWait\":\"per-pair\",\"rendererListMode\":\"snapshot-indices\"}}\n",
               std::string_view{RADRAY_PROJECT_DIR}, GetParam() == render::RenderBackend::D3D12 ? std::string_view{"D3D12"} : std::string_view{"Vulkan"},
               kRecordDraws, warmup, samples, rounds, kRecordSize, kRecordSize);
    if (std::getenv("RADRAY_RECORD_IDENTITY_ONLY")) return;
    render::test::DeviceContext deviceContext;
    if (!render::test::TryCreateDevice(GetParam(), deviceContext, false)) GTEST_SKIP() << "Backend unavailable";
    auto& device = *deviceContext.Device;
    fmt::print("RECORD_DEVICE {{\"gpuName\":{:?}}}\n", std::string_view{device.GetDetail().GpuName});
    render::ShaderProgramLayoutRecipe recipe;
    for (const auto name : {"RecordView", "MaterialValues", "RecordObject"}) {
        const render::ShaderLayoutSelector selector{.DeclarationName = name, .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
        recipe.D3D12.BufferPlacements.push_back({selector, render::D3D12BufferPlacement::RootDescriptor});
        recipe.Vulkan.BufferDescriptors.push_back({selector, render::VulkanBufferDescriptorPlacement::Dynamic});
    }
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
struct ColorData { float4 Color; };
struct ObjectData { float4 Translation; };
VK_BINDING(0, 5) ConstantBuffer<ColorData> RecordView : register(b0, space5);
VK_BINDING(0, 1) ConstantBuffer<ColorData> MaterialValues : register(b0, space1);
VK_BINDING(0, 3) ConstantBuffer<ObjectData> RecordObject : register(b0, space3);
[shader("vertex")] float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position + RecordObject.Translation.xyz, 1);
}
[shader("pixel")] float4 PSMain() : SV_Target0 { return RecordView.Color * MaterialValues.Color; }
)hlsl";
    auto program = test::CompileStageBProgram(device, source, recipe);
    ASSERT_TRUE(program);
    ASSERT_EQ(program->GetParameterLayout().Buffers().size(), 3u);
    constexpr array<float, 12> positions{-1, -1, .5f, 1, -1, .5f, 1, 1, .5f, -1, 1, .5f};
    constexpr array<uint32_t, 6> indices{0, 2, 1, 0, 3, 2};
    auto vertex = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto indexBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertex);
    ASSERT_TRUE(indexBuffer);
    RecordFixture fixture;
    fixture.Backend = GetParam();
    fixture.Geometry.VertexBuffers = {{3, {vertex.Get(), 0, sizeof(positions)}}};
    fixture.Geometry.Ibv = {indexBuffer.Get(), 0, 4};
    fixture.Geometry.VertexLayout.Buffers = {{3, 12, render::VertexStepMode::Vertex}};
    fixture.Geometry.VertexLayout.Attributes = {{"POSITION", 0, 3, 0, render::VertexFormat::FLOAT32X3}};
    fixture.Geometry.Topology = PrimitiveTopology::TriangleList;
    HostWriteBatch writes;
    FrameDrawResources resources{&device};
    ASSERT_TRUE(resources.BeginFrame(writes));
    array<uint32_t, 3> groups{};
    for (const auto& buffer : program->GetParameterLayout().Buffers()) {
        if (buffer.Name == "RecordView")
            groups[0] = buffer.Group;
        else if (buffer.Name == "MaterialValues")
            groups[1] = buffer.Group;
        else if (buffer.Name == "RecordObject")
            groups[2] = buffer.Group;
    }
    std::sort(fixture.GroupOrder.begin(), fixture.GroupOrder.end(), [&](uint8_t left, uint8_t right) { return groups[left] < groups[right]; });
    const array<float, 4> viewValue{1, 1, 1, 1}, objectValue{};
    static constexpr byte colorWire{}, objectWire{};
    const auto viewGroup = resources.PrepareGroupId(*program, groups[0], {&fixture, &colorWire, groups[0]}, 0, std::as_bytes(std::span{viewValue}));
    const auto objectGroup = resources.PrepareGroupId(*program, groups[2], {&fixture, &objectWire, groups[2]}, 0, std::as_bytes(std::span{objectValue}));
    ASSERT_TRUE(viewGroup.IsValid());
    ASSERT_TRUE(objectGroup.IsValid());
    array<FrameShaderGroupId, 2> colorGroups;
    for (uint32_t color = 0; color < 2; ++color) {
        const float intensity = color ? .75f : .25f;
        const array<float, 4> value{intensity, intensity, intensity, intensity};
        colorGroups[color] = resources.PrepareGroupId(*program, groups[1], {&fixture, &colorWire, groups[1]}, color, std::as_bytes(std::span{value}));
        ASSERT_TRUE(colorGroups[color].IsValid());
    }
    fixture.View = resources.GetGroup(viewGroup);
    fixture.Object = resources.GetGroup(objectGroup);
    array<FrameDrawBindingId, 2> bindingIds;
    for (uint32_t color = 0; color < 2; ++color) {
        fixture.Colors[color] = resources.GetGroup(colorGroups[color]);
        const array<FrameShaderGroupId, 3> sourceIds{viewGroup, colorGroups[color], objectGroup};
        array<FrameShaderGroupId, 3> ordered;
        for (size_t at = 0; at < ordered.size(); ++at) ordered[at] = sourceIds[fixture.GroupOrder[at]];
        bindingIds[color] = resources.InternBinding(ordered);
        ASSERT_TRUE(bindingIds[color].IsValid());
    }
    fixture.Snapshot.Valid = true;
    fixture.Snapshot.PublicationRevision = 1;
    fixture.Snapshot.DrawRecords.resize(kRecordDraws);
    fixture.Snapshot.GeometryBindingPlans.emplace_back().Runs = {{0, 1}};
    PrimitiveVertexLayoutRegistry layouts;
    const auto layout = layouts.Intern(fixture.Geometry.VertexLayout);
    for (uint32_t index = 0; index < kRecordDraws; ++index) {
        auto& record = fixture.Snapshot.DrawRecords[index];
        record.Id = index;
        record.Status = DrawRecordStatus::Ready;
        record.NormalStateId = 1 + (index / 256) % 2;
        record.GeometryBindingPlan = 0;
        auto& draw = record.Description;
        draw.Program = program.Get();
        draw.Geometry = &fixture.Geometry;
        draw.LayoutId = layout;
        draw.IndexCount = 6;
        draw.PipelineState.Primitive.Cull = render::CullMode::None;
        draw.PipelineState.Primitive.FaceClockwise = (index / 256) % 2 ? render::FrontFace::CW : render::FrontFace::CCW;
        draw.PipelineState.DepthStencil.DepthTestEnable = false;
        draw.PipelineState.DepthStencil.DepthWriteEnable = false;
        ASSERT_TRUE(fixture.List.AppendStatic(fixture.Snapshot, index, resources, bindingIds[(index / 2) % 2]));
    }
    ASSERT_TRUE(fixture.List.Commands.empty());
    ASSERT_EQ(fixture.List.GetDrawCount(), kRecordDraws);
    writes.Flush(device);
    const uint64_t rowPitch = Align(uint64_t{kRecordSize * 4}, device.GetDetail().TextureDataPitchAlignment);
    array<unique_ptr<render::Buffer>, 2> readbacks;
    for (auto& readback : readbacks) {
        auto made = device.CreateBuffer({rowPitch * kRecordSize, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
        ASSERT_TRUE(made);
        readback = made.Release();
    }
    render::RenderPassRegistry registry{&device};
    RenderResourcePool pool{device, registry};
    auto commands = device.CreateCommandBuffer(deviceContext.Queue);
    ASSERT_TRUE(commands);
    bool traceVerified = false;
    struct Pair {
        uint64_t RuntimeNs{}, ReferenceNs{};
        uint32_t NextOrder{}, RuntimeOrder{UINT32_MAX}, ReferenceOrder{UINT32_MAX};
    };
    vector<Pair> timings(rounds * (warmup + samples));
    array<RenderExternalBuffer, 2> external{{{readbacks[0].get(), readbacks[0]->GetDesc(), render::BufferState::CopyDestination},
                                             {readbacks[1].get(), readbacks[1]->GetDesc(), render::BufferState::CopyDestination}}};
    for (uint32_t frame = 0; frame < timings.size(); ++frame) {
        pool.BeginFlight(frame + 1);
        RenderGraph graph{device, pool, registry, "RHI record reference", kPerformanceRenderGraphRuntimeOptions};
        struct Payload {
            RecordFixture* Fixture;
            Pair* Time;
            bool Native;
            bool* TraceVerified;
            std::optional<PreparedRendererList> Ready;
        };
        for (uint32_t order = 0; order < 2; ++order) {
            const bool native = ((frame + order) % 2) != 0;
            const uint32_t targetIndex = native ? 1 : 0;
            const auto target = graph.CreateTexture({render::TextureDimension::Dim2D, kRecordSize, kRecordSize, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, native ? "reference" : "runtime");
            graph.AddRasterPass<Payload>(native ? "HandwrittenRhi" : "RuntimeRecorder", [&](Payload& data, RenderGraphRasterBuilder& builder) {
                data = {&fixture, &timings[frame], native, &traceVerified, std::nullopt};
                builder.SetColorAttachment(0, target); }, +[](Payload& data, RenderGraphPrepareContext& context) {
                data.Ready = PrepareRendererList(data.Fixture->List, context);
                if (!data.Ready) return false;
                data.Fixture->Pipelines = {RenderGraphTestDriver::Pipeline(*data.Ready, 0), RenderGraphTestDriver::Pipeline(*data.Ready, 256)};
                return true; }, +[](const Payload& data, RenderGraphRasterContext& context) {
                auto& nativeEncoder = RenderGraphTestDriver::NativeEncoder(context);
                SetRecordViewport(nativeEncoder, data.Fixture->Backend);
                if (!*data.TraceVerified) {
                    RecordTraceEncoder runtimeTrace{*nativeEncoder.GetCommandBuffer()}, referenceTrace{*nativeEncoder.GetCommandBuffer()};
                    SetRecordViewport(runtimeTrace, data.Fixture->Backend);
                    SetRecordViewport(referenceTrace, data.Fixture->Backend);
                    DrawExecutionStats stats;
                    RenderGraphTestDriver::WithEncoder(context, runtimeTrace, [&](RenderGraphRasterContext& traced) { RecordRendererList(*data.Ready, traced, stats); });
                    RecordReference(*data.Fixture, referenceTrace);
                    EXPECT_FALSE(runtimeTrace.UnexpectedCommand);
                    EXPECT_FALSE(referenceTrace.UnexpectedCommand);
                    EXPECT_EQ(stats.Draws, kRecordDraws);
                    EXPECT_EQ(runtimeTrace.Draws.size(), kRecordDraws);
                    EXPECT_EQ(runtimeTrace.Draws, referenceTrace.Draws);
                    *data.TraceVerified = true;
                }
                DrawExecutionStats stats;
                const uint32_t actualOrder = data.Time->NextOrder++;
                if (data.Native) data.Time->ReferenceOrder = actualOrder;
                else data.Time->RuntimeOrder = actualOrder;
                const auto begin = std::chrono::steady_clock::now();
                if (data.Native) RecordReference(*data.Fixture, nativeEncoder);
                else RecordRendererList(*data.Ready, context, stats);
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
                if (data.Native) data.Time->ReferenceNs = static_cast<uint64_t>(elapsed);
                else {
                    data.Time->RuntimeNs = static_cast<uint64_t>(elapsed);
                    EXPECT_EQ(stats.Draws, kRecordDraws);
                    EXPECT_TRUE(stats.Succeeded());
                } });
            const auto destination = graph.NextVersion(graph.ImportBuffer(external[targetIndex], native ? "read reference" : "read runtime", RenderGraphExternalAccess::ObservableOutput));
            graph.AddCopyTextureToBufferPass("copy pixels", target, destination);
            graph.AddComputePass<uint32_t>("host visibility", [&](uint32_t&, RenderGraphComputeBuilder& builder) {
                builder.ReadBuffer(destination, RgBufferAccess::HostRead);
                builder.SetSideEffect(); }, +[](const uint32_t&, RenderGraphComputeContext&) {});
        }
        commands->Begin();
        const auto result = RenderGraphTestDriver::Execute(graph, *commands);
        ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
        const uint64_t observedRuntimeLists = frame == 0 ? 2 : 1;
        const auto& calls = graph.GetReport().CommandCalls;
        EXPECT_EQ(calls.DrawIndexed, observedRuntimeLists * kRecordDraws);
        EXPECT_EQ(calls.SetPipeline, observedRuntimeLists * 4);
        EXPECT_EQ(calls.VertexBuffer, observedRuntimeLists * 4);
        EXPECT_EQ(calls.IndexBuffer, observedRuntimeLists * 4);
        EXPECT_EQ(calls.SetParameters, observedRuntimeLists * (kRecordDraws / 2 + 8));
        EXPECT_EQ(timings[frame].NextOrder, 2u);
        EXPECT_EQ(timings[frame].RuntimeOrder, frame % 2);
        EXPECT_EQ(timings[frame].ReferenceOrder, 1u - frame % 2);
        commands->End();
        auto* raw = commands.Get();
        deviceContext.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
        RenderGraphTestDriver::Submitted(raw);
        for (const auto& buffer : external) {
            EXPECT_EQ(buffer.State, render::BufferState::HostRead);
            EXPECT_TRUE(buffer.ContentValid);
        }
        deviceContext.Queue->Wait();
        RenderGraphTestDriver::Completed(raw);
    }
    ASSERT_TRUE(traceVerified);
    array<vector<byte>, 2> images;
    for (uint32_t index = 0; index < 2; ++index) {
        auto& buffer = *readbacks[index];
        const auto* pixels = static_cast<const byte*>(buffer.Map(0, rowPitch * kRecordSize));
        ASSERT_NE(pixels, nullptr);
        buffer.InvalidateMappedRange({0, rowPitch * kRecordSize});
        for (uint32_t y = 0; y < kRecordSize; ++y)
            images[index].insert(images[index].end(), pixels + rowPitch * y, pixels + rowPitch * y + kRecordSize * 4);
        buffer.Unmap();
    }
    EXPECT_EQ(images[0], images[1]);
    for (const auto value : images[0]) EXPECT_NEAR(std::to_integer<uint8_t>(value), 191, 1);
    EXPECT_EQ(deviceContext.ValidationErrors.load(), 0u);
    fmt::print("RECORD_VALIDATION {{\"stateTraceAndPixelsPassed\":{},\"drawsPerPath\":{},\"traceDrawsPerPath\":{},\"pairedFrames\":{}}}\n",
               !testing::Test::HasFailure(), kRecordDraws, kRecordDraws, timings.size());
    for (uint32_t frame = 0; frame < timings.size(); ++frame) {
        const uint32_t inRound = frame % (warmup + samples);
        fmt::print("RECORD_FRAME round={} frame={} warmup={} runtimeFirst={} runtimeNs={} referenceNs={} draws={}\n",
                   frame / (warmup + samples), frame, inRound < warmup ? 1 : 0, timings[frame].RuntimeOrder == 0 ? 1 : 0,
                   timings[frame].RuntimeNs, timings[frame].ReferenceNs, kRecordDraws);
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, RuntimeRecordReference, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
