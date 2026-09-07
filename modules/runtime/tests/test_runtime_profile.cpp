#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include "stage_b_test_support.h"
#include "forward_pipeline/forward_capture.h"
#include "forward_pipeline/forward_lit_mesh_pass_processor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <gtest/gtest.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/render_framework/viewport.h>

// Only this benchmark executable replaces new. Counts are calling-thread C++ allocations,
// excluding driver/DLL allocations, malloc and process-wide residency.
namespace profile_allocations {
thread_local bool Enabled = false;
thread_local uint64_t Count = 0, Bytes = 0;
void CountAllocation(size_t size) noexcept {
    if (Enabled) {
        ++Count;
        Bytes += size;
    }
}
void* Allocate(size_t size) {
    CountAllocation(size);
    if (auto* result = std::malloc(std::max(size, size_t{1}))) return result;
    std::abort();
}
void* AllocateAligned(size_t size, size_t alignment) {
    CountAllocation(size);
#ifdef _WIN32
    auto* result = _aligned_malloc(std::max(size, size_t{1}), alignment);
#else
    auto* result = std::aligned_alloc(alignment, ((std::max(size, size_t{1}) + alignment - 1) / alignment) * alignment);
#endif
    if (!result) std::abort();
    return result;
}
void FreeAligned(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}
}  // namespace profile_allocations
void* operator new(size_t size) { return profile_allocations::Allocate(size); }
void* operator new[](size_t size) { return profile_allocations::Allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
void* operator new(size_t size, std::align_val_t alignment) { return profile_allocations::AllocateAligned(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return profile_allocations::AllocateAligned(size, size_t(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { profile_allocations::FreeAligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { profile_allocations::FreeAligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { profile_allocations::FreeAligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { profile_allocations::FreeAligned(value); }

namespace radray {
namespace {
using Clock = std::chrono::steady_clock;
struct Sample {
    uint64_t Ns{0}, Allocations{0}, Bytes{0};
};
template <typename F>
Sample Measure(F&& callback) {
    profile_allocations::Count = profile_allocations::Bytes = 0;
    profile_allocations::Enabled = true;
    const auto start = Clock::now();
    callback();
    const auto end = Clock::now();
    profile_allocations::Enabled = false;
    return {uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()), profile_allocations::Count, profile_allocations::Bytes};
}
void PrintSamples(std::string_view backend, uint32_t primitives, bool moving, bool diagnostics, bool prepared, std::string_view stage, vector<Sample> samples, bool allocationsMeasured = true) {
#ifdef RADRAY_IS_DEBUG
    constexpr bool isDebug = true;
#else
    constexpr bool isDebug = false;
#endif
    vector<uint64_t> times, counts, bytes;
    for (const auto& sample : samples) {
        times.push_back(sample.Ns);
        counts.push_back(sample.Allocations);
        bytes.push_back(sample.Bytes);
    }
    std::sort(times.begin(), times.end());
    std::sort(counts.begin(), counts.end());
    std::sort(bytes.begin(), bytes.end());
    const auto quantile = [](const auto& values, uint32_t percentage) { return values[std::min(values.size() - 1, (values.size() * percentage + 99) / 100 - 1)]; };
    fmt::print("PROFILE {{\"backend\":\"{}\",\"debug\":{},\"primitives\":{},\"moving\":{},\"diagnostics\":{},\"prepared\":{},\"samples\":{},\"stage\":\"{}\",\"p50Ms\":{:.6f},\"p95Ms\":{:.6f},\"p99Ms\":{:.6f},\"p50Allocations\":{},\"p50AllocatedBytes\":{}}}\n",
               backend, isDebug, primitives, moving, diagnostics, prepared, samples.size(), stage,
               double(quantile(times, 50)) / 1e6, double(quantile(times, 95)) / 1e6, double(quantile(times, 99)) / 1e6,
               allocationsMeasured ? fmt::format("{}", quantile(counts, 50)) : "null", allocationsMeasured ? fmt::format("{}", quantile(bytes, 50)) : "null");
}
class ImmediateWait final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};
class RuntimeProfile : public testing::TestWithParam<render::RenderBackend> {};

// Reference of the old no-graph-groups record path for isolated PSO preparation comparison.
// This is not a public compatibility API and does not emulate the former snapshot/runner implementation.
void RecordReference(const RendererList& list, RenderGraphRasterContext& ctx, DrawExecutionStats& stats) {
    auto& commands = ctx.Encoder();
    for (const auto& draw : list.Commands) {
        if (!ValidateMeshDrawCommand(draw)) {
            ctx.Fail("Invalid reference geometry");
            return;
        }
        const auto pso = draw.Program->GetOrCreateGraphicsPipelineState(draw.PipelineState, draw.Geometry->VertexLayout, draw.Geometry->Topology, ctx.PassState());
        if (!pso) {
            ctx.Fail("Reference PSO failed");
            return;
        }
        commands.BindGraphicsPipelineState(pso.Get());
        for (const auto& group : draw.Groups) commands.BindPersistentShaderParameterSet(group.Group, group.Set.Get(), group.DynamicOffsets);
        commands.BindVertexBuffers(draw.Geometry->VertexBuffers);
        commands.BindIndexBuffer(draw.Geometry->Ibv);
        commands.DrawIndexed(draw.IndexCount, 1, draw.FirstIndex, draw.VertexOffset, 0);
        ++stats.Draws;
    }
}

TEST_P(RuntimeProfile, StageCostsAndWarmResourceCounts) {
    const bool extended = std::getenv("RADRAY_RUNTIME_PROFILE") != nullptr;
    const uint32_t sampleCount = extended ? 32 : 3;
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(GetParam(), context, false)) GTEST_SKIP() << context.Reason;
    auto& device = *context.Device;
    auto program = test::CompileStageBProgram(device, R"hlsl(
#include <pipelines/forward/bindings.hlsli>
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return mul(ForwardView.ViewProj, mul(ForwardObject.LocalToWorld, float4(p, 1))); }
[shader("pixel")] float4 PSMain() : SV_Target0 { return ForwardMaterial.BaseColor; }
)hlsl",
                                              ForwardPipeline::GetLayoutRecipe());
    ASSERT_TRUE(program);
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    auto technique = MaterialTechnique::Create({{"ForwardLit", program.Get(), "ForwardMaterial", state}}, "ForwardLit");
    ASSERT_TRUE(technique);
    auto material = Material::Create(technique.Get());
    ASSERT_TRUE(material);
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    const array<float, 9> positions{-.5f, -.5f, .5f, .5f, -.5f, .5f, 0, .5f, .5f};
    const array<uint32_t, 3> indices{0, 1, 2};
    auto vertices = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto index = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertices);
    ASSERT_TRUE(index);
    GpuMesh geometry;
    auto& draw = geometry.Draws.emplace_back();
    draw.VertexBuffers = {{0, {vertices.Get(), 0, sizeof(positions)}}};
    draw.Ibv = {index.Get(), 0, 4};
    draw.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    draw.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    geometry.Buffers.push_back(vertices.Release());
    geometry.Buffers.push_back(index.Release());
    ImmediateWait wait;
    AssetManager assets;
    assets.SetWaitFrameProcessor(&wait);
    auto mesh = assets.AddReady<StaticMesh>(AssetId{0x90211, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, make_unique<StaticMesh>(MeshResource{},
                                                                                                                     vector<StaticMeshSection>{{0, 0, 3, 0, 2}, {0, 0, 3, 0, 2}}, Eigen::Vector3f{-.5f, -.5f, .5f}, Eigen::Vector3f{.5f, .5f, .5f}, std::move(geometry)));
    ASSERT_TRUE(mesh.IsReady());
    const array<uint32_t, 3> scales{1000, 10000, 100000};
    for (const uint32_t count : scales) {
        if (!extended && count != 1000) continue;
        Scene scene;
        for (uint32_t i = 0; i < count; ++i) ASSERT_TRUE(scene.AddPrimitive(make_unique<StaticMeshSceneProxy>(mesh, vector<Nullable<Material*>>{material.Get(), material.Get()}, Eigen::Matrix4f::Identity())));
        RenderSceneSnapshotBuilder builder;
        RenderSceneSnapshot snapshot;
        vector<StreamingAssetRefAny> owners;
        ResolvedRenderView view;
        view.View = view.Projection = view.ViewProjection = Eigen::Matrix4f::Identity();
        view.ViewRect = view.ScissorRect = {0, 0, 16, 16};
        view.StateId = AllocateViewStateId();
        CullingResults culling;
        RendererList list;
        HostWriteBatch writes;
        FrameDrawResources draws{&device, {.BasicSize = 32 * 1024 * 1024, .Alignment = 256, .MaxResetSize = 32 * 1024 * 1024}};
        forward_detail::ForwardBindingCache bindings;
        render::RenderPassRegistry passes(&device);
        RenderGraphFrameResources graphResources(device, passes);
        auto command = device.CreateCommandBuffer(context.Queue);
        ASSERT_TRUE(command);
        bool warned = false;
        uint64_t serial = 0;
        for (const bool moving : {false, true})
            for (uint32_t mode = 0; mode < (extended ? 4u : 2u); ++mode) {
                const bool prepared = mode < 2;
                const bool diagnostics = mode % 2 != 0;
                array<vector<Sample>, 10> samples;
                array<vector<Sample>, 4> graphStages;
                for (uint32_t frame = 0; frame < 3 + sampleCount; ++frame) {
                    ++serial;
                    owners.clear();
                    list.ResetForReuse();
                    writes.Reset();
                    graphResources.BeginFlight(serial, writes);
                    ASSERT_TRUE(draws.BeginFrame(writes));
                    array<Sample, 10> values;
                    values[0] = Measure([&] {
                        if (moving)
                            for (const auto& proxy : scene.Primitives()) {
                                Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
                                transform(0, 3) = float(serial % 10) * .001f;
                                proxy->SetLocalToWorld(transform);
                            }
                    });
                    const auto beforeGc = assets.GetCollectionStats().CandidatesVisited;
                    values[1] = Measure([&] { assets.Pump(); });
                    EXPECT_EQ(assets.GetCollectionStats().CandidatesVisited, beforeGc);
                    bool valid = false;
                    values[2] = Measure([&] { valid = builder.Build(scene, snapshot, owners); });
                    ASSERT_TRUE(valid);
                    if (frame) EXPECT_EQ(snapshot.Stats.ScratchEntriesCreated, 0u);
                    values[3] = Measure([&] { valid = Cull({&snapshot, &view}, culling); });
                    ASSERT_TRUE(valid);
                    values[4] = Measure([&] {
                        forward_detail::ForwardLitMeshPassProcessor processor{draws, bindings, warned};
                        valid = BuildRendererList({"profile", "ForwardLit", &culling, &view, RenderQueueRange::Opaque()}, processor, list);
                    });
                    ASSERT_TRUE(valid);
                    ASSERT_EQ(list.Commands.size(), count * 2u);
                    EXPECT_EQ(draws.GetStats().GroupPreparations, count + 2u);
                    EXPECT_EQ(draws.GetStats().RecipeBuilds, 3u);
                    DrawExecutionStats stats;
                    unique_ptr<RenderGraph> graph;
                    values[5] = Measure([&] {
                        graph = make_unique<RenderGraph>(device, graphResources, passes, "runtime profile");
                        graph->SetResourceView(view.StateId.Value);
                        const auto color = graph->CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
                        const auto depth = graph->CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::D32_FLOAT, render::MemoryType::Device, render::TextureUse::DepthStencilWrite, {}}, "depth");
                        struct Data {
                            std::optional<PreparedRendererList> List;
                            const RendererList* Source;
                            DrawExecutionStats* Stats;
                            render::RenderBackend Backend;
                        };
                        graph->AddRasterPass<Data>("draw", [&](Data& data, RenderGraphRasterBuilder& pass) {
                        pass.SetColorAttachment(0, color); pass.SetDepthAttachment(depth); pass.SetSideEffect();
                        data = {prepared ? PrepareRendererList(list, pass) : std::nullopt, &list, &stats, GetParam()}; }, +[](const Data& data, RenderGraphRasterContext& ctx) {
                        ctx.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16)); ctx.Encoder().SetScissor({0, 0, 16, 16});
                        if (data.List) SubmitRendererList(*data.List, ctx, *data.Stats);
                        else RecordReference(*data.Source, ctx, *data.Stats); });
                    });
                    command->Begin();
                    RenderGraphExecutionResult execution;
                    values[6] = Measure([&] { execution = RenderGraphTestDriver::Execute(*graph, *command); });
                    ASSERT_TRUE(execution.Success) << graph->GetReport().ToText();
                    EXPECT_EQ(stats.Draws, count * 2u);
                    EXPECT_EQ(graph->GetReport().GraphicsPipelinePreparations, prepared ? 1u : 0u);
                    if (serial > 1) EXPECT_EQ(graph->GetReport().GraphicsPipelineCreations, 0u);
                    forward_detail::ForwardCapture capture;
                    if (diagnostics) {
                        capture.Name = "profile";
                        capture.Directory = ".";
                    }
                    values[7] = Measure([&] { capture.CaptureReport(graph->GetReport()); });
                    EXPECT_EQ(capture.Report.empty(), !diagnostics);
                    EXPECT_EQ(capture.Dot.empty(), !diagnostics);
                    if (!diagnostics) EXPECT_EQ(values[7].Allocations, 0u);
                    values[8] = Measure([&] {
                        writes.Flush(device);
                        command->End();
                        auto* raw = command.Get();
                        context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
                        RenderGraphTestDriver::Submitted(raw);
                    });
                    values[9] = Measure([&] { context.Queue->Wait(); });
                    RenderGraphTestDriver::Completed(command.Get());
                    if (frame >= 3) {
                        for (size_t i = 0; i < values.size(); ++i) samples[i].push_back(values[i]);
                        const auto& cpu = graph->GetReport().Cpu;
                        const uint64_t times[]{cpu.CompileNanoseconds, cpu.RealizeNanoseconds, cpu.PrepareNanoseconds, cpu.RecordNanoseconds};
                        for (size_t i = 0; i < graphStages.size(); ++i) graphStages[i].push_back({times[i], 0, 0});
                    }
                }
                const std::string_view names[]{"proxyTransform", "assetPump", "snapshot", "cull", "listAndParameters", "graphSetup", "graphExecute", "diagnosticSerialization", "flushAndSubmit", "gpuWait"};
                for (size_t i = 0; i < samples.size(); ++i) PrintSamples(EnumName(GetParam()), count, moving, diagnostics, prepared, names[i], std::move(samples[i]));
                const std::string_view stages[]{"graphCompile", "graphRealize", "graphPrepare", "graphRecord"};
                for (size_t i = 0; i < graphStages.size(); ++i) PrintSamples(EnumName(GetParam()), count, moving, diagnostics, prepared, stages[i], std::move(graphStages[i]), false);
                const auto& resourceStats = draws.GetStats();
                fmt::print("PROFILE_COUNTS {{\"backend\":\"{}\",\"primitives\":{},\"moving\":{},\"diagnostics\":{},\"objectAndMaterialPreparations\":{},\"recipeBuilds\":{},\"setCreations\":{},\"setCacheHits\":{},\"constantBytes\":{},\"snapshotMaterialBytes\":{},\"poolBytes\":{},\"poolPeakBytes\":{}}}\n",
                           EnumName(GetParam()), count, moving, diagnostics, resourceStats.GroupPreparations, resourceStats.RecipeBuilds, resourceStats.SetCreations,
                           resourceStats.SetCacheHits, resourceStats.BufferBytesCopied, snapshot.Stats.MaterialBytesCopied, graphResources.GetPoolStats().EstimatedBytes, graphResources.GetPoolStats().PeakEstimatedBytes);
            }
    }
}
INSTANTIATE_TEST_SUITE_P(Backends, RuntimeProfile, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
