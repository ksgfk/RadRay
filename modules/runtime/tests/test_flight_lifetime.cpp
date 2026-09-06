#include "foundation_graph_fixture.h"
#include "gpu_submission_gate.h"
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/render_workload.h>
#include <radray/runtime/render_framework/renderer_list_pass_bindings.h>
#include <radray/runtime/render_framework/view_state.h>

namespace radray {
namespace {

using GpuGate = test::GpuSubmissionGate;

class FlightLifetimeTest : public test::FoundationGraphGpuTest {};

TEST_P(FlightLifetimeTest, L01L03L06B05ThreeInFlightGraphsRetainBindingsAndBoundStableDrawResources) {
    auto& device = *Context.Device;
    render::ShaderProgramLayoutRecipe recipe;
    const render::ShaderLayoutSelector native{.DeclarationName = "Native", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
    recipe.D3D12.BufferPlacements.push_back({.Selector = native, .Placement = render::D3D12BufferPlacement::RootDescriptor});
    recipe.Vulkan.BufferDescriptors.push_back({.Selector = native, .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    auto program = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
struct NativeData { uint Value; uint3 Padding; };
struct PassData { uint4 Value; };
VK_BINDING(0, 0) ConstantBuffer<NativeData> Native : register(b0);
VK_BINDING(0, 1) ConstantBuffer<PassData> Pass : register(b0, space1);
VK_BINDING(1, 1) Texture2D<float> Marker : register(t0, space1);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] uint4 PSMain() : SV_Target0 { return Pass.Value + Native.Value + (uint)Marker.Load(int3(0, 0, 0)); }
)hlsl",
                                                   recipe);
    ASSERT_TRUE(program);
    const array<float, 9> positions{-1, -1, .5f, 3, -1, .5f, -1, 3, .5f};
    const array<uint32_t, 3> indices{0, 1, 2};
    auto vertices = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto index = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertices);
    ASSERT_TRUE(index);
    GpuMesh::DrawData geometry;
    geometry.VertexBuffers = {{0, {vertices.Get(), 0, sizeof(positions)}}};
    geometry.Ibv = {index.Get(), 0, 4};
    geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    struct Flight {
        HostWriteBatch Writes;
        unique_ptr<RenderGraphFrameResources> GraphResources;
        unique_ptr<FrameDrawResources> DrawResources;
        unique_ptr<render::CommandBuffer> Command;
        unique_ptr<render::Buffer> Readback;
        RenderExternalBuffer External{};
        uint64_t Signal{0}, Serial{0}, TextureId{0}, WarmCreates{0}, WarmViews{0}, WarmCapacity{0};
    };
    array<Flight, 3> flights;
    const auto pitch = Align(uint64_t{4 * 16}, device.GetDetail().TextureDataPitchAlignment);
    for (auto& flight : flights) {
        flight.GraphResources = make_unique<RenderGraphFrameResources>(device, *Registry);
        flight.DrawResources = make_unique<FrameDrawResources>(&device);
        flight.Command = device.CreateCommandBuffer(Context.Queue).Release();
        flight.Readback = device.CreateBuffer({pitch * 4, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}}).Release();
        ASSERT_TRUE(flight.Command);
        ASSERT_TRUE(flight.Readback);
        flight.External = {flight.Readback.get(), flight.Readback->GetDesc(), render::BufferState::CopyDestination};
    }
    auto completed = device.CreateFence();
    ASSERT_TRUE(completed);
    GpuGate gate{device, *Context.Queue};
    ASSERT_TRUE(gate.Fence);
    const auto verify = [&](const Flight& flight) {
        const auto bytes = Read(*flight.Readback);
        ASSERT_EQ(bytes.size(), pitch * 4);
        array<uint32_t, 4> actual{};
        std::memcpy(actual.data(), bytes.data(), sizeof(actual));
        for (uint32_t i = 0; i < 4; ++i) EXPECT_EQ(actual[i], (i + 1) * 11 + uint32_t(flight.Serial) * 100 + 30 + uint32_t(flight.Serial));
    };
    uint64_t maxSets = 0;
    for (uint64_t frame = 1; frame <= 64 + 512; ++frame) {
        auto& flight = flights[(frame - 1) % 3];
        if (flight.Signal) {
            completed->Wait(flight.Signal);
            verify(flight);
        }
        flight.Writes.Reset();
        flight.GraphResources->BeginFlight(frame, flight.Writes);
        ASSERT_TRUE(flight.DrawResources->BeginFrame(flight.Writes));
        flight.Serial = frame;
        RendererList list;
        ShaderParameterStorage nativeValues{&program->GetParameterLayout(), 0};
        for (uint32_t drawIndex = 0; drawIndex < 1000; ++drawIndex) {
            ASSERT_TRUE(nativeValues.SetUInt("Native.Value", uint32_t(frame) * 100 + (drawIndex % 4) * 10));
            const auto group = flight.DrawResources->PrepareGroup(*program, 0, nativeValues);
            ASSERT_TRUE(group);
            MeshDrawCommand draw;
            draw.Program = program.Get();
            draw.Geometry = &geometry;
            draw.IndexCount = 3;
            draw.PipelineState.Primitive.Cull = render::CullMode::None;
            draw.PipelineState.DepthStencil.DepthTestEnable = draw.PipelineState.DepthStencil.DepthWriteEnable = false;
            draw.Groups.push_back(*group);
            ASSERT_TRUE(FinalizeMeshDrawCommand(draw));
            list.Commands.push_back(std::move(draw));
        }
        flight.Command->Begin();
        {
            RenderGraph graph{device, *flight.GraphResources, *Registry, "three flight parameter lifetime"};
            const auto marker = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource, {}}, "flight marker");
            graph.AddRasterPass<test::EmptyGraphPass>("marker", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, marker, {.Clear = {float(frame), 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
            const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::RGBA32_UINT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "integer constants");
            DrawExecutionStats stats;
            struct Draw {
                const RendererList* List;
                DrawExecutionStats* Stats;
                render::RenderBackend Backend;
                std::optional<RendererListPassBindings> Bindings;
            };
            graph.AddRasterPass<Draw>("1000 shared draws", [&](Draw& data, RenderGraphRasterBuilder& builder) {
                data.List = &list; data.Stats = &stats; data.Backend = GetParam(); builder.SetColorAttachment(0, color);
                array<uint32_t, 4> constants{11, 22, 33, 44}; string declaration{"Pass"};
                const RgParameterBinding values[]{{declaration, 0, RgCBufferParameterBinding{std::as_bytes(std::span{constants})}}, {"Marker", 0, RgTextureParameterBinding{marker}}};
                const RendererListProgramParameters parameters{program.Get(), 1, values};
                data.Bindings = RendererListPassBindings::Create(builder, list, std::span{&parameters, 1}); ASSERT_TRUE(data.Bindings);
                constants.fill(0xdeadbeef); declaration.assign(100, 'x'); }, +[](const Draw& data, RenderGraphRasterContext& context) {
                context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 4, 4)); context.Encoder().SetScissor({0, 0, 4, 4});
                SubmitRendererList(*data.List, context, context.PassState(), *data.Bindings, *data.Stats); });
            const auto host = graph.ImportBuffer(flight.External, "readback", RenderGraphExternalAccess::ObservableOutput);
            graph.AddCopyTextureToBufferPass("copy constants", color, host);
            HostRead(graph, host);
            ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *flight.Command).Success) << graph.GetReport().ToText();
            EXPECT_TRUE(stats.Succeeded());
            EXPECT_EQ(stats.Draws, 1000u);
            const auto id = graph.GetReport().Resources[1].PhysicalId;
            if (flight.TextureId)
                EXPECT_EQ(id, flight.TextureId);
            else
                flight.TextureId = id;
        }  // Graph, strings, lists and copied constants do not own native sets or pool textures.
        EXPECT_EQ(flight.GraphResources->GetParameterSetCount(), 1u);
        maxSets = std::max<uint64_t>(maxSets, flight.DrawResources->GetSetCount());
        EXPECT_LE(flight.DrawResources->GetSetCount(), 2u);
        const auto& pool = flight.GraphResources->GetPoolStats();
        if (frame >= 62 && frame <= 64) {
            flight.WarmCreates = pool.Created;
            flight.WarmViews = pool.ViewsCreated;
            flight.WarmCapacity = flight.Writes.GetStats().PageCapacityBytes;
        }
        if (frame > 64) {
            EXPECT_EQ(pool.Created, flight.WarmCreates);
            EXPECT_EQ(pool.ViewsCreated, flight.WarmViews);
            EXPECT_EQ(flight.Writes.GetStats().PageCapacityBytes, flight.WarmCapacity);
        }
        flight.Writes.Flush(device);
        flight.Command->End();
        auto* command = flight.Command.get();
        auto* signal = completed.Get();
        auto* wait = gate.Fence.get();
        uint64_t waitValue = frame;
        Context.Queue->Submit({.CmdBuffers = std::span{&command, 1}, .SignalFences = std::span{&signal, 1}, .SignalValues = std::span{&frame, 1}, .WaitFences = frame <= 3 ? std::span{&wait, 1} : std::span<render::Fence*>{}, .WaitValues = frame <= 3 ? std::span{&waitValue, 1} : std::span<uint64_t>{}});
        flight.Signal = frame;
        if (frame == 3) {
            EXPECT_EQ(completed->GetCompletedValue(), 0u);
            for (const auto& pendingFlight : flights) {
                EXPECT_EQ(pendingFlight.GraphResources->GetPoolStats().TextureCount, 2u);
                EXPECT_EQ(pendingFlight.GraphResources->GetParameterSetCount(), 1u);
            }
            for (uint64_t release = 1; release <= 3; ++release) {
                ASSERT_TRUE(gate.Release(release));
                completed->Wait(release);
                verify(flights[release - 1]);
                if (release < 3) EXPECT_EQ(completed->GetCompletedValue(), release);
            }
        }
    }
    for (const auto& flight : flights) {
        completed->Wait(flight.Signal);
        verify(flight);
    }
    RecordProperty("flights", 3);
    RecordProperty("simultaneously_blocked_submissions", 3);
    RecordProperty("draws_per_frame", 1000);
    RecordProperty("warmup_frames", 64);
    RecordProperty("measured_frames", 512);
    RecordProperty("max_native_groups_per_flight", std::to_string(maxSets));
    RecordProperty("queue_waits_in_frame_loop", 0);
}

TEST_P(FlightLifetimeTest, L04HistoryResizeRetiresAfterTheProtectingFlightAndPreservesAnotherView) {
    auto& device = *Context.Device;
    ViewStateRegistry histories{device, *Registry, 3};
    ResolvedRenderView a{}, b{};
    a.StateId = AllocateViewStateId();
    b.StateId = AllocateViewStateId();
    a.ViewProjection.setIdentity();
    b.ViewProjection.setIdentity();
    ResolvedRenderViewFamily family;
    family.OutputAvailable = true;
    family.RenderSize = family.OutputSize = {64, 32};
    family.OutputFormat = render::TextureFormat::R32_FLOAT;
    HistoryTextureRequest request;
    request.Key = "resize";
    request.BufferCount = 3;
    request.Desc.Format = family.OutputFormat;
    request.Desc.Extent.Mode = RenderExtentMode::RelativeToFamilyRenderExtent;
    request.Desc.Usage = render::TextureUse::RenderTarget | render::TextureUse::CopySource | render::TextureUse::Resource;
    array<HostWriteBatch, 3> writes;
    array<unique_ptr<RenderGraphFrameResources>, 3> resources;
    array<unique_ptr<render::CommandBuffer>, 4> commands;
    array<unique_ptr<render::Buffer>, 4> readbacks;
    array<RenderExternalBuffer, 4> imports{};
    for (auto& resource : resources) resource = make_unique<RenderGraphFrameResources>(device, *Registry);
    for (uint32_t i = 0; i < 4; ++i) {
        commands[i] = device.CreateCommandBuffer(Context.Queue).Release();
        const auto pitch = Align(uint64_t{i == 3 ? 96u : 64u} * 4, device.GetDetail().TextureDataPitchAlignment);
        readbacks[i] = device.CreateBuffer({pitch * (i == 3 ? 48 : 32), render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}}).Release();
        ASSERT_TRUE(commands[i]);
        ASSERT_TRUE(readbacks[i]);
        imports[i] = {readbacks[i].get(), readbacks[i]->GetDesc(), render::BufferState::CopyDestination};
    }
    auto complete = device.CreateFence();
    ASSERT_TRUE(complete);
    GpuGate gate{device, *Context.Queue};
    ASSERT_TRUE(gate.Fence);
    uint64_t otherGeneration = 0;
    for (uint64_t frame = 1; frame <= 4; ++frame) {
        const auto flight = uint32_t((frame - 1) % 3);
        if (frame == 4) {
            ASSERT_TRUE(gate.Release(1));
            complete->Wait(1);
        }
        resources[flight]->BeginFlight(frame, writes[flight]);
        histories.BeginFlight(flight, frame);
        auto changedFamily = family;
        if (frame == 4) changedFamily.RenderSize = changedFamily.OutputSize = {96, 48};
        histories.Resolve(a, changedFamily);
        histories.Resolve(b, family);
        string reason;
        auto first = histories.AcquireHistoryTexture(a, changedFamily, request, reason);
        auto other = histories.AcquireHistoryTexture(b, family, request, reason);
        ASSERT_TRUE(first.Current) << reason;
        ASSERT_TRUE(other.Current) << reason;
        if (otherGeneration)
            EXPECT_EQ(other.CommitToken.Generation, otherGeneration);
        else
            otherGeneration = other.CommitToken.Generation;
        if (frame == 4) {
            EXPECT_FALSE(first.PreviousValid);
            EXPECT_EQ(histories.GetStats().RetiredGenerations, 1u);
            EXPECT_EQ(histories.GetStats().GenerationsDestroyed, 0u);
        }
        commands[frame - 1]->Begin();
        {
            RenderGraph graph{device, *resources[flight], *Registry, "history generations in flight"};
            const auto color = graph.ImportTexture(*first.Current, "resizing view", RenderGraphExternalAccess::ObservableOutput);
            const auto second = graph.ImportTexture(*other.Current, "independent view", RenderGraphExternalAccess::ObservableOutput);
            for (const auto target : {color, second}) graph.AddRasterPass<test::EmptyGraphPass>("clear history", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, target, {.Clear = {float(frame), 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
            const auto host = graph.ImportBuffer(imports[frame - 1], "readback", RenderGraphExternalAccess::ObservableOutput);
            graph.AddCopyTextureToBufferPass("read history marker", color, host);
            HostRead(graph, host);
            ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *commands[frame - 1]).Success) << graph.GetReport().ToText();
        }
        EXPECT_TRUE(histories.CommitHistory(first.CommitToken));
        EXPECT_TRUE(histories.CommitHistory(other.CommitToken));
        EXPECT_TRUE(histories.CommitView(a.StateId));
        EXPECT_TRUE(histories.CommitView(b.StateId));
        writes[flight].Flush(device);
        commands[frame - 1]->End();
        auto* command = commands[frame - 1].get();
        auto* signal = complete.Get();
        auto* wait = gate.Fence.get();
        uint64_t waitValue = frame;
        Context.Queue->Submit({.CmdBuffers = std::span{&command, 1}, .SignalFences = std::span{&signal, 1}, .SignalValues = std::span{&frame, 1}, .WaitFences = frame <= 3 ? std::span{&wait, 1} : std::span<render::Fence*>{}, .WaitValues = frame <= 3 ? std::span{&waitValue, 1} : std::span<uint64_t>{}});
        if (frame == 3) EXPECT_EQ(complete->GetCompletedValue(), 0u);
    }
    ASSERT_TRUE(gate.Release(2));
    complete->Wait(2);
    histories.BeginFlight(1, 5);
    EXPECT_EQ(histories.GetStats().GenerationsDestroyed, 0u);
    ASSERT_TRUE(gate.Release(3));
    complete->Wait(3);
    histories.BeginFlight(2, 6);
    EXPECT_EQ(histories.GetStats().GenerationsDestroyed, 0u);
    complete->Wait(4);
    const auto framebufferCount = Registry->GetFramebufferCount();
    histories.BeginFlight(0, 7);
    EXPECT_EQ(histories.GetStats().GenerationsDestroyed, 1u);
    EXPECT_EQ(histories.GetStats().RetiredGenerations, 0u);
    EXPECT_LT(Registry->GetFramebufferCount(), framebufferCount);
    for (uint32_t i = 0; i < 4; ++i) {
        const auto bytes = Read(*readbacks[i]);
        float value;
        std::memcpy(&value, bytes.data(), sizeof(value));
        EXPECT_FLOAT_EQ(value, float(i + 1));
    }
}

TEST_P(FlightLifetimeTest, L08UnregisterDetachesTheIdWhileCallerKeepsInFlightOutputAlive) {
    auto& device = *Context.Device;
    RenderOutputRegistry outputs;
    auto target = render::test::MakeRenderTarget(&device, render::TextureFormat::RGBA8_UNORM, 16, 16,
                                                 render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource);
    ASSERT_TRUE(target);
    const auto id = outputs.RegisterExternal({"borrowed", target->Tex.get(), target->View.get()});
    ASSERT_TRUE(id.IsValid());
    const auto surface = outputs.ResolveExternal(id);
    ASSERT_TRUE(surface);
    array<render::TextureStates, 1> states{surface->CurrentState};
    array<uint8_t, 1> valid{0};
    RenderExternalTexture external{target->Tex.get(), target->Tex->GetDesc(), states, valid, target->View.get()};
    const auto pitch = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
    array<unique_ptr<render::Buffer>, 2> readbacks;
    array<RenderExternalBuffer, 2> imports{};
    for (uint32_t i = 0; i < 2; ++i) {
        readbacks[i] = device.CreateBuffer({pitch * 16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}}).Release();
        ASSERT_TRUE(readbacks[i]);
        imports[i] = {readbacks[i].get(), readbacks[i]->GetDesc(), render::BufferState::CopyDestination};
    }
    auto command = device.CreateCommandBuffer(Context.Queue);
    auto complete = device.CreateFence();
    ASSERT_TRUE(command);
    ASSERT_TRUE(complete);
    GpuGate gate{device, *Context.Queue};
    ASSERT_TRUE(gate.Fence);
    command->Begin();
    {
        auto graph = MakeGraph("borrowed output lifetime");
        const auto color = graph.ImportTexture(external, "caller output", RenderGraphExternalAccess::ObservableOutput);
        graph.AddRasterPass<test::EmptyGraphPass>("output producer", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, color, {.Clear = {.25f, .5f, .75f, 1}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
        for (uint32_t i = 0; i < 2; ++i) {
            const auto host = graph.ImportBuffer(imports[i], fmt::format("view {} readback", i), RenderGraphExternalAccess::ObservableOutput);
            graph.AddCopyTextureToBufferPass(fmt::format("view {} consumer", i), color, host);
            HostRead(graph, host);
        }
        ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success) << graph.GetReport().ToText();
    }
    command->End();
    auto* raw = command.Get();
    auto* wait = gate.Fence.get();
    auto* signal = complete.Get();
    uint64_t value = 1;
    Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}, .SignalFences = std::span{&signal, 1}, .SignalValues = std::span{&value, 1}, .WaitFences = std::span{&wait, 1}, .WaitValues = std::span{&value, 1}});
    EXPECT_TRUE(outputs.Unregister(id));
    EXPECT_FALSE(outputs.ResolveExternal(id));
    EXPECT_FALSE(outputs.Find(id));
    auto infos = outputs.GetGameThreadInfos();
    RenderFramePlan plan;
    RenderWorkloadBuilder builder{plan, infos};
    EXPECT_FALSE(builder.AddViewFamily({"stale", id}));
    EXPECT_TRUE(target->Tex->IsValid());
    EXPECT_TRUE(target->View->IsValid());
    EXPECT_EQ(gate.Released, 0u);
    ASSERT_TRUE(gate.Release(1));
    complete->Wait(1);
    for (const auto& readback : readbacks) {
        const auto bytes = Read(*readback);
        EXPECT_NEAR(std::to_integer<int>(bytes[0]), 64, 1);
        EXPECT_NEAR(std::to_integer<int>(bytes[1]), 128, 1);
        EXPECT_NEAR(std::to_integer<int>(bytes[2]), 191, 1);
    }
    Registry->RemoveFramebuffersUsing(target->View.get());
    target.reset();
    auto replacement = render::test::MakeRenderTarget(&device, render::TextureFormat::RGBA8_UNORM, 16, 16, render::TextureUse::RenderTarget | render::TextureUse::Resource);
    ASSERT_TRUE(replacement);
    const auto next = outputs.RegisterExternal({"replacement", replacement->Tex.get(), replacement->View.get()});
    EXPECT_GT(next.Value, id.Value);
    EXPECT_TRUE(outputs.ResolveExternal(next));
    EXPECT_FALSE(outputs.ResolveExternal(id));
    EXPECT_TRUE(outputs.Unregister(next));
}

INSTANTIATE_TEST_SUITE_P(Backends, FlightLifetimeTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
