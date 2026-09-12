#include "runtime_profile_integrated.h"
#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include "stage_b_test_support.h"
#include "runtime_profile_support.h"
#include "forward_pipeline/forward_capture.h"
#include "forward_pipeline/forward_frame.h"
#include "forward_pipeline/forward_lit_mesh_pass_processor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <new>
#include <source_location>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <gtest/gtest.h>
#include <radray/profiler.h>
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
    std::source_location Source{};
};
template <typename F>
Sample Measure(F&& callback, std::source_location source = std::source_location::current()) {
    profile_allocations::Count = profile_allocations::Bytes = 0;
    profile_allocations::Enabled = true;
    const auto start = Clock::now();
    callback();
    const auto end = Clock::now();
    profile_allocations::Enabled = false;
    return {uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()), profile_allocations::Count, profile_allocations::Bytes, source};
}
void PrintSamples(std::string_view backend, uint32_t primitives, bool moving, bool diagnostics, uint32_t round, std::string_view stage, vector<Sample> samples) {
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
    fmt::print("PROFILE {{\"backend\":\"{}\",\"debug\":{},\"primitives\":{},\"moving\":{},\"diagnostics\":{},\"round\":{},\"samples\":{},\"stage\":\"{}\",\"p50Ms\":{:.6f},\"p95Ms\":{:.6f},\"p99Ms\":{:.6f},\"p50Allocations\":{},\"p50AllocatedBytes\":{}}}\n",
               backend, isDebug, primitives, moving, diagnostics, round, samples.size(), stage,
               double(quantile(times, 50)) / 1e6, double(quantile(times, 95)) / 1e6, double(quantile(times, 99)) / 1e6,
               quantile(counts, 50), quantile(bytes, 50));
}
class ImmediateWait final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};
class RuntimeProfile : public testing::TestWithParam<render::RenderBackend> {};

TEST_P(RuntimeProfile, StageCostsAndWarmResourceCounts) {
    const profile::Options options;
    ASSERT_TRUE(options.Valid) << "invalid RADRAY_PROFILE_* option";
    profile::PrintOptions(options, "shared-mesh-micro", EnumName(GetParam()), 1, 1, 16, 16, false);
    if (std::getenv("RADRAY_PROFILE_IDENTITY_ONLY")) return;
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(GetParam(), context, options.DriverValidation)) GTEST_SKIP() << context.Reason;
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
    const array<uint32_t, 1> scales{options.Primitives};
    for (const uint32_t count : scales) {
        Scene scene;
        for (uint32_t i = 0; i < count; ++i) ASSERT_TRUE(scene.AddPrimitive(make_unique<StaticMeshSceneProxy>(mesh, vector<Nullable<Material*>>{material.Get(), material.Get()}, Eigen::Matrix4f::Identity())));
        RenderSceneSnapshotBuilder builder;
        RenderSceneSnapshot snapshot;
        PackedCBufferTable objects;
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
        for (const bool moving : {false, true}) {
            array<vector<double>, 10> roundMedians;
            for (uint32_t round = 0; round < options.Rounds; ++round) {
                const bool diagnostics = options.SerializeReport;
                array<vector<Sample>, 10> samples;
                struct FrameRow {
                    uint64_t Serial, BeginNs, SubmitNs, RetireNs;
                    uint64_t Draws, GroupPreparations, RecipeBuilds, SetCreations, ConstantBytes, SnapshotMaterialBytes;
                    array<Sample, 10> Phases;
                };
                vector<FrameRow> rows;
                rows.reserve(options.Samples + options.Warmup);
                for (auto& phaseSamples : samples) phaseSamples.reserve(options.Samples);
                for (uint32_t frame = 0; frame < options.Warmup + options.Samples; ++frame) {
                    const auto frameBegin = profile::TimestampNs();
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
                    values[2] = Measure([&] {
                        valid = builder.Build(scene, snapshot, owners, options.Runtime.Validation);
                        if (valid) forward_detail::FreezeObjectData(snapshot, objects);
                    });
                    ASSERT_TRUE(valid);
                    if (frame) {
                        EXPECT_EQ(snapshot.Stats.ScratchEntriesCreated, 0u);
                        EXPECT_EQ(snapshot.Stats.PrimitiveStructuresRebuilt, 0u);
                        EXPECT_EQ(snapshot.Stats.PrimitiveStructuresReused, count);
                        EXPECT_EQ(snapshot.Stats.MaterialsRebuilt, 0u);
                        EXPECT_EQ(snapshot.Stats.MaterialBytesCopied, 0u);
                        if (!moving) {
                            EXPECT_EQ(snapshot.Stats.PrimitiveBoundsRebuilt, 0u);
                            EXPECT_EQ(snapshot.Stats.PrimitiveBoundsReused, count);
                        }
                    }
                    values[3] = Measure([&] { valid = Cull({&snapshot, &view}, culling); });
                    ASSERT_TRUE(valid);
                    const auto recipesBefore = program->GetParameterGroupRecipeCount();
                    values[4] = Measure([&] {
                        forward_detail::ForwardLitMeshPassProcessor processor{draws, bindings, warned, objects};
                        valid = BuildRendererList({.Name = "profile", .MaterialPassName = "ForwardLit", .Culling = &culling, .View = &view,
                                                   .QueueRange = RenderQueueRange::Opaque(), .Validation = options.Runtime.Validation}, processor, list);
                    });
                    ASSERT_TRUE(valid);
                    ASSERT_EQ(list.Commands.size(), count * 2u);
                    EXPECT_EQ(draws.GetStats().GroupPreparations, count + 2u);
                    EXPECT_EQ(draws.GetStats().RecipeBuilds, program->GetParameterGroupRecipeCount() - recipesBefore);
                    if (frame) EXPECT_EQ(draws.GetStats().RecipeBuilds, 0u);
                    DrawExecutionStats stats;
                    unique_ptr<RenderGraph> graph;
                    values[5] = Measure([&] {
                        graph = make_unique<RenderGraph>(device, graphResources, passes, "runtime profile", options.Runtime);
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
                        data = {std::nullopt, &list, &stats, GetParam()}; }, +[](Data& data, RenderGraphPrepareContext& ctx) {
                        data.List = PrepareRendererList(*data.Source, ctx);
                        return data.List.has_value(); }, +[](const Data& data, RenderGraphRasterContext& ctx) {
                        ctx.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16)); ctx.Encoder().SetScissor({0, 0, 16, 16});
                        RecordRendererList(*data.List, ctx, *data.Stats); });
                    });
                    command->Begin();
                    RenderGraphExecutionResult execution;
                    values[6] = Measure([&] { execution = RenderGraphTestDriver::Execute(*graph, *command); });
                    ASSERT_TRUE(execution.Success) << graph->GetReport().ToText();
                    EXPECT_EQ(stats.Draws, count * 2u);
                    if (options.Runtime.Report != RenderGraphReportMode::Minimal) {
                        EXPECT_EQ(graph->GetReport().GraphicsPipelinePreparations, 1u);
                        if (serial > 1) EXPECT_EQ(graph->GetReport().GraphicsPipelineCreations, 0u);
                    }
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
                    const auto submitNs = profile::TimestampNs();
                    values[9] = Measure([&] { context.Queue->Wait(); });
                    RenderGraphTestDriver::Completed(command.Get());
                    const auto& counters = draws.GetStats();
                    rows.push_back({serial, frameBegin, submitNs, profile::TimestampNs(), stats.Draws, counters.GroupPreparations, counters.RecipeBuilds, counters.SetCreations, counters.BufferBytesCopied, snapshot.Stats.MaterialBytesCopied, values});
                    RADRAY_PROFILE_FRAME();
                    if (frame >= options.Warmup) {
                        for (size_t i = 0; i < values.size(); ++i) samples[i].push_back(values[i]);
                    }
                }
                const std::string_view names[]{"proxyTransform", "assetPump", "snapshot", "cull", "listAndParameters", "graphSetup", "graphExecute", "diagnosticSerialization", "flushAndSubmit", "gpuWait"};
                for (size_t i = 0; i < samples.size(); ++i) {
                    vector<uint64_t> times;
                    for (const auto& sample : samples[i]) times.push_back(sample.Ns);
                    roundMedians[i].push_back(profile::QuantileMs(std::move(times), 50));
                    PrintSamples(EnumName(GetParam()), count, moving, diagnostics, round, names[i], std::move(samples[i]));
                }
                for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
                    const auto& row = rows[rowIndex];
                    fmt::print("PROFILE_FRAME {{\"fixture\":\"shared-mesh-micro\",\"backend\":{:?},\"primitives\":{},\"moving\":{},\"round\":{},\"sampleIndex\":{},\"warmup\":{},\"frameSerial\":{},\"flightIndex\":0,\"beginNs\":{},\"submitNs\":{},\"retireNs\":{},\"actualDraws\":{},\"uniqueGeometry\":1,\"uniqueMaterials\":1,\"uniqueLayouts\":1,\"uniquePsoRecipes\":1,\"groupPreparations\":{},\"recipeBuilds\":{},\"setCreations\":{},\"constantBytes\":{},\"snapshotMaterialBytes\":{},\"phases\":[",
                               EnumName(GetParam()), count, moving, round, rowIndex, rowIndex < options.Warmup, row.Serial, row.BeginNs, row.SubmitNs, row.RetireNs, row.Draws, row.GroupPreparations, row.RecipeBuilds, row.SetCreations, row.ConstantBytes, row.SnapshotMaterialBytes);
                    for (size_t i = 0; i < row.Phases.size(); ++i) {
                        const auto& phase = row.Phases[i];
                        fmt::print("{}{{\"name\":{:?},\"src_file\":{:?},\"src_line\":{},\"scopeMode\":\"inclusive\",\"ns\":{},\"allocations\":{},\"allocatedBytes\":{}}}", i ? "," : "", names[i], std::string_view{phase.Source.file_name()}, phase.Source.line(), phase.Ns, phase.Allocations, phase.Bytes);
                    }
                    fmt::print("]}}\n");
                }
                const auto& resourceStats = draws.GetStats();
                fmt::print("PROFILE_COUNTS {{\"backend\":\"{}\",\"primitives\":{},\"moving\":{},\"diagnostics\":{},\"groupPreparations\":{},\"recipeBuilds\":{},\"setCreations\":{},\"setCacheHits\":{},\"constantBytes\":{},\"snapshotMaterialBytes\":{},\"poolBytes\":{},\"poolPeakBytes\":{}}}\n",
                           EnumName(GetParam()), count, moving, diagnostics, resourceStats.GroupPreparations, resourceStats.RecipeBuilds, resourceStats.SetCreations,
                           resourceStats.SetCacheHits, resourceStats.BufferBytesCopied, snapshot.Stats.MaterialBytesCopied, graphResources.GetPoolStats().EstimatedBytes, graphResources.GetPoolStats().PeakEstimatedBytes);
                const auto& snapshotStats = snapshot.Stats;
                fmt::print("PROFILE_REUSE {{\"backend\":\"{}\",\"primitives\":{},\"moving\":{},\"diagnostics\":{},\"structuresRebuilt\":{},\"structuresReused\":{},\"boundsRebuilt\":{},\"boundsReused\":{},\"materialsRebuilt\":{},\"materialsReused\":{}}}\n",
                           EnumName(GetParam()), count, moving, diagnostics,
                           snapshotStats.PrimitiveStructuresRebuilt, snapshotStats.PrimitiveStructuresReused,
                           snapshotStats.PrimitiveBoundsRebuilt, snapshotStats.PrimitiveBoundsReused,
                           snapshotStats.MaterialsRebuilt, snapshotStats.MaterialsReused);
            }
            const std::string_view names[]{"proxyTransform", "assetPump", "snapshot", "cull", "listAndParameters", "graphSetup", "graphExecute", "diagnosticSerialization", "flushAndSubmit", "gpuWait"};
            for (size_t i = 0; i < roundMedians.size(); ++i) {
                const auto cv = profile::CoefficientOfVariation(roundMedians[i]);
                fmt::print("PROFILE_STABILITY {{\"backend\":{:?},\"primitives\":{},\"moving\":{},\"stage\":{:?},\"rounds\":{},\"p50Cv\":{},\"stable\":{}}}\n", EnumName(GetParam()), count, moving, names[i], options.Rounds, cv, options.Rounds >= 5 && cv <= .05);
            }
        }
    }
}
TEST_P(RuntimeProfile, ThreeViewForwardSteadyState) {
    const profile::Options options;
    ASSERT_TRUE(options.Valid);
    profile::PrintOptions(options, "three-view-forward", EnumName(GetParam()), 3, 2, 960, 540, true, profile::IntegratedSnapshotMode<ForwardPipeline>());
    if (std::getenv("RADRAY_PROFILE_IDENTITY_ONLY")) return;
    ASSERT_TRUE(profile::AwaitCapturePermit()) << "Capture startup permit is absent, stale, or profiling is disabled";
    {
        render::test::DeviceContext context;
        if (!render::test::TryCreateDevice(GetParam(), context, options.DriverValidation)) GTEST_SKIP() << context.Reason;
    }
    test::RuntimeLogCapture logs;
    profile::IntegratedApp app{options};
    const std::filesystem::path root{RADRAY_PROJECT_DIR};
    ASSERT_EQ(app.Run({.Backend = GetParam(), .EnableValidation = options.DriverValidation, .Multithreaded = true,
                       .AppName = "runtime profile", .ShaderSourceRoot = root, .ShaderIncludePaths = {root / "shaderlib"},
                       .WindowTitle = "RadRay runtime profile", .WindowWidth = 960, .WindowHeight = 540, .BackBufferCount = 3, .FlightDataCount = 2,
                       .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::Immediate}), 0);
    EXPECT_FALSE(app.Failed) << app.Failure;
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    EXPECT_GE(app.Completed, options.Rounds * (options.Warmup + options.Samples));
    for (const auto& frame : app.Frames) {
        if (!frame.Complete || frame.Index >= options.Rounds * (options.Warmup + options.Samples)) continue;
        EXPECT_EQ(frame.ResolvedViews, 3u);
        EXPECT_EQ(frame.AvailableViews, 3u);
        EXPECT_EQ(frame.FullViews, 2u);
        EXPECT_EQ(frame.AuxiliaryViews, 1u);
        EXPECT_EQ(frame.AvailableOutputs, 2u);
        EXPECT_EQ(frame.WrittenOutputs, 2u);
        EXPECT_TRUE(frame.StageCommandsKnown);
        EXPECT_GT(frame.MeshDraws, 0u);
        EXPECT_GT(frame.DepthCommands, 0u);
        EXPECT_GT(frame.OpaqueCommands, 0u);
        EXPECT_GT(frame.TransparentCommands, 0u);
        if (frame.Submission.Available) {
            EXPECT_EQ(frame.Submission.FrameSerial, frame.FrameSerial);
            EXPECT_EQ(frame.Submission.FlightIndex, frame.FlightIndex);
            EXPECT_GE(frame.Submission.SubmitCallbackNs, frame.RecordedNs);
            EXPECT_GE(frame.Submission.CompletionCallbackNs, frame.Submission.SubmitCallbackNs);
            EXPECT_LE(frame.Submission.CompletionCallbackNs, frame.RetireObservedNs);
            EXPECT_TRUE(frame.Submission.CompletionSucceeded);
        }
    }
    profile::PrintIntegratedFrames(options, EnumName(GetParam()), app.Frames);
}

TEST(RuntimeProfileSubmission, PreservesCallbacksAndRejectsStaleRowIdentity) {
    struct Result { shared_ptr<FrameSubmission> Submission; };
    uint32_t submitted = 0, completed = 0;
    bool succeeded = false;
    Result result{make_shared<FrameSubmission>(7)};
    result.Submission->OnSubmitted = [&] { ++submitted; };
    result.Submission->OnCompleted = [&](bool success) { ++completed; succeeded = success; };
    profile::SubmissionObservation observation;
    profile::ObserveSubmission(result, 7, 1, observation);
    ASSERT_TRUE(observation.Available);
    ASSERT_TRUE(result.Submission->Record());
    ASSERT_TRUE(result.Submission->Submit(7));
    ASSERT_TRUE(result.Submission->Complete(7, true));
    EXPECT_EQ(submitted, 1u);
    EXPECT_EQ(completed, 1u);
    EXPECT_TRUE(succeeded);
    EXPECT_GT(observation.SubmitCallbackNs, 0u);
    EXPECT_GE(observation.CompletionCallbackNs, observation.SubmitCallbackNs);
    EXPECT_TRUE(observation.CompletionSucceeded);
    result.Submission->Cancel();
    EXPECT_EQ(completed, 1u);

    Result stale{make_shared<FrameSubmission>(8)};
    stale.Submission->OnCompleted = [&](bool success) { ++completed; succeeded = success; };
    profile::SubmissionObservation reused;
    profile::ObserveSubmission(stale, 8, 0, reused);
    reused.FrameSerial = 10;
    stale.Submission->Cancel();
    EXPECT_EQ(completed, 2u);
    EXPECT_FALSE(succeeded);
    EXPECT_EQ(reused.CompletionCallbackNs, 0u);

    struct OldResult {} old;
    profile::SubmissionObservation unavailable;
    profile::ObserveSubmission(old, 9, 0, unavailable);
    EXPECT_FALSE(unavailable.Available);
}

INSTANTIATE_TEST_SUITE_P(Backends, RuntimeProfile, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
