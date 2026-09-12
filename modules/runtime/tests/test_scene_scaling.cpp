#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include "stage_b_test_support.h"
#include "forward_pipeline/forward_bindings.h"
#include "forward_pipeline/forward_frame.h"
#include "forward_pipeline/forward_lit_mesh_pass_processor.h"

#include <chrono>
#include <cstring>
#include <tuple>
#include <gtest/gtest.h>
#include <fmt/format.h>

#include <radray/runtime/application.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {
namespace {

using ScalingClock = std::chrono::steady_clock;
uint64_t ElapsedNs(ScalingClock::time_point begin) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ScalingClock::now() - begin).count());
}

enum class ScalingChange { ParentTransform,
                           IndexRange,
                           SharedMaterialValues };
enum class ScalingDistribution { Repeated,
                                 UniqueGeometry,
                                 UniqueMaterial,
                                 EscapedMaterial };
std::string_view DistributionName(ScalingDistribution distribution) {
    switch (distribution) {
        case ScalingDistribution::Repeated: return "repeated";
        case ScalingDistribution::UniqueGeometry: return "unique-geometry-descriptions";
        case ScalingDistribution::UniqueMaterial: return "unique-material";
        case ScalingDistribution::EscapedMaterial: return "escaped-material";
    }
    return "unknown";
}
std::string_view ChangeName(ScalingChange change) {
    switch (change) {
        case ScalingChange::ParentTransform: return "parent-transform";
        case ScalingChange::IndexRange: return "index-range";
        case ScalingChange::SharedMaterialValues: return "shared-material-values";
    }
    return "unknown";
}

class ScalingPrimitive final : public PrimitiveSceneProxy {
public:
    ScalingPrimitive(const GpuMesh::DrawData* geometry, Material* material) : Geometry(geometry), DrawMaterial(material) {}
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return Revision; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Constant(-.02f), Eigen::Vector3f::Constant(.02f)}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {Geometry, 0, Expanded ? 6u : 3u, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
    void SetExpanded(bool value) {
        if (Expanded == value) return;
        Expanded = value;
        ++Revision;
        MarkRenderDirty(PrimitiveDirtyKind::Structure);
    }
    void SetMaterial(Material* value) {
        if (DrawMaterial == value) return;
        DrawMaterial = value;
        MarkRenderDirty(PrimitiveDirtyKind::MaterialAssignment);
    }

private:
    const GpuMesh::DrawData* Geometry;
    Material* DrawMaterial;
    uint64_t Revision{1};
    bool Expanded{false};
};

// Production SceneComponent attachment/ancestor notification drives the proxy setter. This
// bridge isolates CPU scaling from Application/World registration; it does not replace Cull,
// static policy compilation, Forward preparation, indexed list building, or Ready recording.
class ScalingTransformNode final : public SceneComponent {
public:
    ScalingTransformNode(ScalingPrimitive* proxy, uint64_t& notifications) : Proxy(proxy), Notifications(notifications) {}

protected:
    void OnTransformChanged() override {
        ++Notifications;
        Proxy->SetLocalToWorld(GetWorldMatrix());
    }

private:
    ScalingPrimitive* Proxy;
    uint64_t& Notifications;
};

// Native work is completed by the real Forward processor before this replay. Reusing its
// successful IDs separates BuildRendererList's filtering/index emission/sort from native
// group preparation. The batch fallback deliberately fails, so an empty/legacy path cannot pass.
class ScalingPreparedIds final : public MeshPassProcessor {
public:
    ScalingPreparedIds(FrameDrawResources& resources, std::span<const FrameDrawBindingId> bindings)
        : Resources(resources), Bindings(bindings) {}
    void AddMeshBatch(const RendererListDesc&, const RenderSceneSnapshot&, const MeshBatch&, MeshPassDrawListContext& out) override {
        ++BatchCalls;
        out.Reject(MeshPassRejectReason::ProcessorRejected);
    }
    void PrepareRecord(const RendererListDesc&, const RenderSceneSnapshot&, const DrawRecord& record, MeshPassDrawListContext& out) override {
        ++RecordCalls;
        if (record.Status != DrawRecordStatus::Ready || record.Primitive >= Bindings.size()) {
            out.Reject(MeshPassRejectReason::InvalidGeometry);
            return;
        }
        out.AddRecord(Resources, Bindings[record.Primitive]);
    }
    uint64_t RecordCalls{0}, BatchCalls{0};

private:
    FrameDrawResources& Resources;
    std::span<const FrameDrawBindingId> Bindings;
};

// Executes the production sealed Ready recorder against valid native PSOs/sets/VB/IB, but
// counts draws instead of submitting geometry. This fixture is CPU/count evidence, not an
// image-quality oracle or a measurement of driver/GPU draw cost.
class ScalingCountingEncoder final : public render::GraphicsCommandEncoder {
public:
    ScalingCountingEncoder(render::CommandBuffer& commands, const GpuMesh::DrawData& geometry, uint64_t requiredGroups)
        : Commands(commands), Geometry(geometry), RequiredGroups(requiredGroups) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::CommandBuffer* GetCommandBuffer() const noexcept override { return &Commands; }
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set,
                                std::span<const render::ShaderParameterDynamicOffset> offsets) noexcept override {
        if (group >= 64 || !set->IsValid() || offsets.size() != 1 || !offsets[0].Binding.IsValid()) {
            Invalid = true;
            return;
        }
        BoundGroups |= uint64_t{1} << group;
        ++SetBinds;
    }
    bool SetPushConstants(render::BindingHandle, std::span<const byte>) noexcept override {
        Invalid = true;
        return false;
    }
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> buffers) noexcept override {
        VertexBound = buffers.size() == 1 && buffers[0].Binding == Geometry.VertexBuffers[0].Binding &&
                      buffers[0].View.Target == Geometry.VertexBuffers[0].View.Target &&
                      buffers[0].View.Offset == 0 && buffers[0].View.Size == Geometry.VertexBuffers[0].View.Size;
        Invalid |= !VertexBound;
    }
    void BindIndexBuffer(render::IndexBufferView buffer) noexcept override {
        IndexBound = buffer.Target == Geometry.Ibv.Target && buffer.Offset == Geometry.Ibv.Offset && buffer.Stride == 4;
        Invalid |= !IndexBound;
    }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* pipeline) noexcept override {
        PipelineBound = pipeline->IsValid();
        BoundGroups = 0;
        VertexBound = IndexBound = false;
        Invalid |= !PipelineBound;
        ++PipelineBinds;
    }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override { Invalid = true; }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance) noexcept override {
        Invalid |= !PipelineBound || !VertexBound || !IndexBound || BoundGroups != RequiredGroups ||
                   (count != 3 && count != 6) || instances != 1 || first != 0 || offset != 0 || firstInstance != 0;
        ++Draws;
        IndexElements += count;
    }
    void DrawIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { Invalid = true; }
    void DrawIndexedIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { Invalid = true; }
    uint64_t Draws{0}, IndexElements{0}, SetBinds{0}, PipelineBinds{0};
    bool Invalid{false};

private:
    render::CommandBuffer& Commands;
    const GpuMesh::DrawData& Geometry;
    uint64_t RequiredGroups, BoundGroups{0};
    bool PipelineBound{false}, VertexBound{false}, IndexBound{false};
};

struct ScalingConsumerStats {
    uint64_t CullNs{0}, ForwardNativeAndStagingNs{0}, IndexedBuildAndSortNs{0}, ReadyNs{0}, CountRecordNs{0};
    uint64_t Visible{0}, NativeRecordCalls{0}, IndexedRecordCalls{0}, ReadyDraws{0}, RecordedDraws{0};
    uint64_t RecordedIndices{0}, SetBinds{0}, PipelineBinds{0};
    FrameDrawResourceStats Native;
};

void ConsumeScalingSnapshot(render::test::DeviceContext& deviceContext, render::RenderPassRegistry& registry,
                            RenderResourcePool& pool, render::CommandBuffer& commands, uint64_t serial,
                            const RenderSceneSnapshot& snapshot, std::span<const GpuMesh::DrawData> geometries,
                            const PackedCBufferTable& objects, FrameDrawResources& resources, HostWriteBatch& writes,
                            uint32_t viewCount, uint64_t expectedIndicesPerView, ScalingConsumerStats& stats) {
    const auto n = static_cast<uint32_t>(snapshot.Primitives.size());
    ASSERT_TRUE(geometries.size() == 1 || geometries.size() == n);
    const auto& geometry = geometries.front();
    vector<uint32_t> records(n, UINT32_MAX);
    uint64_t invalidRecords = 0;
    for (uint32_t index = 0; index < snapshot.DrawRecords.size(); ++index) {
        const auto& record = snapshot.DrawRecords[index];
        invalidRecords += record.Status != DrawRecordStatus::Ready || record.Primitive >= n ||
                          record.Description.Geometry.Get() != &geometries[record.Primitive % geometries.size()] || !record.NormalStateId ||
                          record.GeometryBindingPlan >= snapshot.GeometryBindingPlans.size();
        if (record.Policy == forward_detail::kForwardLitPolicy && record.Primitive < n) {
            invalidRecords += records[record.Primitive] != UINT32_MAX;
            records[record.Primitive] = index;
        }
    }
    ASSERT_EQ(invalidRecords, 0u);
    ASSERT_EQ(std::count(records.begin(), records.end(), UINT32_MAX), 0);
    ASSERT_EQ(snapshot.DrawRecords.size(), uint64_t{n} * 2);
    writes.Reset();
    ASSERT_TRUE(resources.BeginFrame(writes));
    forward_detail::ForwardBindingCache bindings;
    bool overflow = false;
    forward_detail::ForwardLitMeshPassProcessor forward{resources, bindings, overflow, objects};
    vector<ResolvedRenderView> views(viewCount);
    vector<CullingResults> culling(viewCount);
    vector<RendererList> lists(viewCount);
    vector<FrameDrawBindingId> preparedIds(n);
    RendererList staging;
    for (uint32_t viewIndex = 0; viewIndex < viewCount; ++viewIndex) {
        auto& view = views[viewIndex];
        view.Name = "scaling-view";
        view.StateId = {viewIndex + 1};
        view.View = view.Projection = view.ViewProjection = view.PreviousViewProjection = Eigen::Matrix4f::Identity();
        view.ViewProjection(0, 3) = static_cast<float>(viewIndex) * .01f;
        view.WorldPosition = Eigen::Vector3f::Zero();
        view.ViewRect = view.ScissorRect = {0, 0, 4, 4};
        auto begin = ScalingClock::now();
        const bool culled = Cull({&snapshot, &view}, culling[viewIndex]);
        stats.CullNs += ElapsedNs(begin);
        ASSERT_TRUE(culled);
        ASSERT_EQ(culling[viewIndex].Primitives.size(), n);
        EXPECT_FALSE(std::is_sorted(culling[viewIndex].Primitives.begin(), culling[viewIndex].Primitives.end(),
                                    [](const auto& left, const auto& right) { return left.ViewDepth < right.ViewDepth; }));
        EXPECT_EQ(culling[viewIndex].Stats.InvalidBoundsVisible, 0u);
        EXPECT_EQ(culling[viewIndex].Stats.FrustumRejected, 0u);
        stats.Visible += culling[viewIndex].Primitives.size();
        RendererListDesc desc{"scaling-forward", "ForwardLit", &culling[viewIndex], &view};
        desc.Policy = forward_detail::kForwardLitPolicy;
        desc.QueueRange = RenderQueueRange::Opaque();
        desc.RequireMaterialPass = true;
        forward.ResetView();
        staging.ResetForReuse();
        bool nativeSucceeded = true;
        begin = ScalingClock::now();
        for (const auto visible : culling[viewIndex].Primitives) {
            const auto recordIndex = records[visible.Primitive];
            MeshPassDrawListContext out;
            forward.PrepareRecord(desc, snapshot, snapshot.DrawRecords[recordIndex], out);
            ++stats.NativeRecordCalls;
            if (!out.HasDraw() || !out.AppendTo(staging, snapshot, recordIndex)) {
                nativeSucceeded = false;
                break;
            }
            preparedIds[visible.Primitive] = staging.GetBindingId(staging.GetDrawCount() - 1);
        }
        stats.ForwardNativeAndStagingNs += ElapsedNs(begin);
        ASSERT_TRUE(nativeSucceeded);
        ASSERT_EQ(staging.GetDrawCount(), n);
        ASSERT_TRUE(staging.Commands.empty());
        const auto nativeBeforeReplay = resources.GetStats();
        ScalingPreparedIds replay{resources, preparedIds};
        begin = ScalingClock::now();
        const bool built = BuildRendererList(desc, replay, lists[viewIndex]);
        stats.IndexedBuildAndSortNs += ElapsedNs(begin);
        ASSERT_TRUE(built);
        stats.IndexedRecordCalls += replay.RecordCalls;
        ASSERT_EQ(replay.RecordCalls, n);
        ASSERT_EQ(replay.BatchCalls, 0u);
        ASSERT_TRUE(lists[viewIndex].Stats.ContentSucceeded());
        ASSERT_EQ(lists[viewIndex].GetDrawCount(), n);
        ASSERT_EQ(lists[viewIndex].GetItems().size(), n);
        const auto items = lists[viewIndex].GetItems();
        const auto sortKey = [](const RendererListItem& item) {
            const auto& value = item.SortData;
            return std::tuple{static_cast<int32_t>(value.Queue), value.ProgramFrameId, value.Material, value.ViewDepth, value.Primitive, value.Batch};
        };
        EXPECT_TRUE(std::is_sorted(items.begin(), items.end(), [&](const auto& left, const auto& right) { return sortKey(left) < sortKey(right); }));
        uint64_t wrongExecutionMappings = 0;
        for (size_t index = 0; index < items.size(); ++index) {
            const auto primitive = items[index].SortData.Primitive;
            wrongExecutionMappings += primitive >= n ||
                                      &lists[viewIndex].GetDescription(index) != &snapshot.DrawRecords[records[primitive]].Description;
        }
        EXPECT_EQ(wrongExecutionMappings, 0u);
        EXPECT_TRUE(lists[viewIndex].Commands.empty());
        EXPECT_EQ(lists[viewIndex].GetPrograms().size(), 1u);
        EXPECT_EQ(resources.GetStats().GroupPreparations, nativeBeforeReplay.GroupPreparations);
        EXPECT_EQ(resources.GetStats().BufferBytesCopied, nativeBeforeReplay.BufferBytesCopied);
        EXPECT_EQ(resources.GetStats().SharedGroupHits, nativeBeforeReplay.SharedGroupHits);
    }
    EXPECT_EQ(bindings.LayoutParses(), 0u);
    EXPECT_FALSE(overflow);
    stats.Native = resources.GetStats();
    const uint64_t uniqueGroups = uint64_t{n} + viewCount + snapshot.Materials.size();
    EXPECT_EQ(stats.Native.GroupPreparations, uniqueGroups);
    EXPECT_EQ(stats.Native.SharedBufferUploads, uniqueGroups);
    EXPECT_EQ(resources.GetGroupCount(), uniqueGroups);
    EXPECT_EQ(resources.GetBindingCount(), uint64_t{n} * viewCount);
    uint64_t materialBytes = 0;
    for (const auto& material : snapshot.Materials) materialBytes += material.Passes[0].NumericBytes.size();
    EXPECT_EQ(stats.Native.BufferBytesCopied, n * sizeof(Forward_ObjectData) + viewCount * sizeof(Forward_ViewData) + materialBytes);
    writes.Flush(*deviceContext.Device);

    uint64_t requiredGroups = 0;
    const auto& schema = snapshot.BindingRecipes[snapshot.DrawRecords[records[0]].BindingRecipe];
    for (size_t index = 0; index < 3; ++index) {
        ASSERT_LT(schema.Groups[index], 64u);
        requiredGroups |= uint64_t{1} << schema.Groups[index];
    }
    pool.BeginFlight(serial);
    const RenderGraphRuntimeOptions options{RenderValidationMode::Full, RenderGraphReportMode::Counters, false};
    RenderGraph graph{*deviceContext.Device, pool, registry, "T55 Ready consumer", options};
    struct Payload {
        const RendererList* List;
        const GpuMesh::DrawData* Geometry;
        ScalingConsumerStats* Stats;
        uint64_t RequiredGroups, ExpectedIndices;
        std::optional<PreparedRendererList> Ready;
    };
    for (uint32_t viewIndex = 0; viewIndex < viewCount; ++viewIndex) {
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "scaling color");
        const auto depth = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::D32_FLOAT, render::MemoryType::Device, render::TextureUse::DepthStencilRead | render::TextureUse::DepthStencilWrite, {}}, "scaling depth");
        graph.AddRasterPass<Payload>("scaling Ready", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data = {&lists[viewIndex], &geometry, &stats, requiredGroups, expectedIndicesPerView, std::nullopt};
            builder.SetColorAttachment(0, color);
            builder.SetDepthAttachment(depth);
            builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
            const auto begin = ScalingClock::now();
            data.Ready = PrepareRendererList(*data.List, context);
            data.Stats->ReadyNs += ElapsedNs(begin);
            if (!data.Ready) return false;
            data.Stats->ReadyDraws += RenderGraphTestDriver::DrawCount(*data.Ready);
            return true; }, +[](const Payload& data, RenderGraphRasterContext& context) {
            auto& native = RenderGraphTestDriver::NativeEncoder(context);
            ScalingCountingEncoder encoder{*native.GetCommandBuffer(), *data.Geometry, data.RequiredGroups};
            DrawExecutionStats execution;
            const auto begin = ScalingClock::now();
            RenderGraphTestDriver::WithEncoder(context, encoder, [&](RenderGraphRasterContext& counted) {
                RecordRendererList(*data.Ready, counted, execution);
            });
            data.Stats->CountRecordNs += ElapsedNs(begin);
            EXPECT_TRUE(execution.Succeeded());
            EXPECT_EQ(execution.Draws, data.List->GetDrawCount());
            EXPECT_EQ(encoder.Draws, execution.Draws);
            EXPECT_EQ(encoder.IndexElements, data.ExpectedIndices);
            EXPECT_FALSE(encoder.Invalid);
            data.Stats->RecordedDraws += encoder.Draws;
            data.Stats->RecordedIndices += encoder.IndexElements;
            data.Stats->SetBinds += encoder.SetBinds;
            data.Stats->PipelineBinds += encoder.PipelineBinds; });
    }
    commands.Begin();
    const auto result = RenderGraphTestDriver::Execute(graph, commands);
    commands.End();
    ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
    auto* raw = &commands;
    deviceContext.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
    RenderGraphTestDriver::Submitted(raw);
    deviceContext.Queue->Wait();
    RenderGraphTestDriver::Completed(raw);
    EXPECT_EQ(stats.ReadyDraws, uint64_t{n} * viewCount);
    EXPECT_EQ(stats.RecordedDraws, stats.ReadyDraws);
    EXPECT_EQ(stats.PipelineBinds, viewCount);
    EXPECT_EQ(stats.SetBinds, uint64_t{viewCount} * (n + 2 + snapshot.Materials.size() - 1));
    EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, stats.RecordedDraws);
}

uint64_t PageCount(uint64_t changedRows, uint64_t rowsPerPage) { return (changedRows + rowsPerPage - 1) / rowsPerPage; }
uint64_t PublishedRows(uint64_t changedRows, uint64_t rowsPerPage, uint64_t totalRows) {
    return std::min(totalRows, PageCount(changedRows, rowsPerPage) * rowsPerPage);
}

class SceneScaling : public testing::TestWithParam<std::tuple<uint32_t, ScalingDistribution>> {};

TEST_P(SceneScaling, T55PublicationAndReadyConsumerMatrix) {
    render::test::DeviceContext deviceContext;
    if (!render::test::TryCreateAnyDevice(deviceContext)) GTEST_SKIP() << "No backend available";
    auto& device = *deviceContext.Device;
    const auto [n, distribution] = GetParam();
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
#include <pipelines/forward/cbuffers.hlsli>
VK_BINDING(0, 0) ConstantBuffer<Forward_ViewData> ForwardView : register(b0, space0);
VK_BINDING(0, 1) ConstantBuffer<Forward_MaterialData> ForwardMaterial : register(b0, space1);
VK_BINDING(0, 2) ConstantBuffer<Forward_ObjectData> ForwardObject : register(b0, space2);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position {
    return mul(ForwardView.ViewProj, mul(ForwardObject.LocalToWorld, float4(p, 1)));
}
[shader("pixel")] float4 PSMain() : SV_Target0 { return ForwardMaterial.BaseColor; }
)hlsl";
    auto program = test::CompileStageBProgram(device, source, ForwardPipeline::GetLayoutRecipe());
    ASSERT_TRUE(program);
    auto technique = MaterialTechnique::Create({{"ForwardLit", program.Get(), "ForwardMaterial", {}}}, "ForwardLit");
    ASSERT_TRUE(technique);
    auto baseMaterial = Material::Create(technique.Get());
    auto sharedMaterial = Material::Create(technique.Get());
    ASSERT_TRUE(baseMaterial);
    ASSERT_TRUE(sharedMaterial);
    constexpr array<float, 12> positions{-.01f, -.01f, 0, .01f, -.01f, 0, .01f, .01f, 0, -.01f, .01f, 0};
    constexpr array<uint32_t, 6> indices{0, 2, 1, 0, 3, 2};
    auto vertex = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto indexBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertex);
    ASSERT_TRUE(indexBuffer);
    vector<GpuMesh::DrawData> geometries(distribution == ScalingDistribution::UniqueGeometry ? n : 1);
    auto& geometry = geometries.front();
    geometry.VertexBuffers = {{0, {vertex.Get(), 0, sizeof(positions)}}};
    geometry.Ibv = {indexBuffer.Get(), 0, 4};
    geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    geometry.Topology = PrimitiveTopology::TriangleList;
    ASSERT_TRUE(ValidateMeshGeometry(geometry, 0, 6));
    for (size_t index = 1; index < geometries.size(); ++index) geometries[index] = geometry;
    vector<unique_ptr<Material>> uniqueMaterials;
    if (distribution == ScalingDistribution::UniqueMaterial) {
        uniqueMaterials.reserve(n);
        for (uint32_t index = 0; index < n; ++index) {
            auto material = Material::Create(technique.Get());
            ASSERT_TRUE(material);
            ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
            uniqueMaterials.push_back(material.Release());
        }
    }
    std::span<byte> escapedSharedBytes;
    if (distribution == ScalingDistribution::EscapedMaterial) {
        (void)baseMaterial->NumericBytes();
        escapedSharedBytes = sharedMaterial->NumericBytes();
    }
    const auto baselineMaterial = [&](uint32_t index) -> Material* {
        return uniqueMaterials.empty() ? baseMaterial.Get() : uniqueMaterials[index].get();
    };
    Scene scene;
    array<SceneComponent, 100> parents;
    uint64_t childNotifications = 0;
    vector<unique_ptr<ScalingTransformNode>> nodes;
    vector<ScalingPrimitive*> primitives;
    nodes.reserve(n);
    primitives.reserve(n);
    for (uint32_t index = 0; index < n; ++index) {
        auto primitive = make_unique<ScalingPrimitive>(&geometries[index % geometries.size()], baselineMaterial(index));
        auto* raw = primitive.get();
        ASSERT_TRUE(scene.AddPrimitive(std::move(primitive)));
        primitives.push_back(raw);
        auto node = make_unique<ScalingTransformNode>(raw, childNotifications);
        node->SetRelativeLocation({0, 0, .2f + .5f * static_cast<float>((index * 37u) % n) / static_cast<float>(n)});
        node->AttachTo(&parents[index / (n / 100)]);
        nodes.push_back(std::move(node));
    }
    array<vector<StreamingAssetRefAny>, 2> owners;
    array<shared_ptr<const RenderSceneSnapshot>, 2> flights;
    array<forward_detail::ForwardObjectDataCache, 2> objects;
    array<HostWriteBatch, 2> hostWrites;
    array<unique_ptr<FrameDrawResources>, 2> frameResources;
    for (auto& resources : frameResources)
        resources = make_unique<FrameDrawResources>(&device, DynamicCBufferArena::Descriptor{.BasicSize = 1u << 20, .Alignment = 256, .MaxResetSize = 64u << 20});
    render::RenderPassRegistry registry{&device};
    RenderResourcePool pool{device, registry};
    auto commands = device.CreateCommandBuffer(deviceContext.Queue);
    ASSERT_TRUE(commands);
    uint64_t serial = 0;
    const auto publish = [&](uint32_t flight) -> bool {
        owners[flight].clear();
        AppUpdateContext app{.FlightIndex = flight};
        RenderFramePlan plan;
        RenderWorkloadBuilder workloads{plan, {}};
        const RenderGraphRuntimeOptions runtime{RenderValidationMode::Full, RenderGraphReportMode::Counters, false};
        RenderPrepareContext prepare{app, {}, workloads, owners[flight], runtime, serial++};
        if (!forward_detail::RegisterForwardPassPolicies(prepare, scene) || !prepare.FreezeRegisteredScenes()) return false;
        auto snapshot = prepare.PrepareScene(scene);
        if (!snapshot) return false;
        flights[flight] = snapshot.Release();
        return true;
    };
    fmt::print("SCALING_OPTIONS {{\"n\":{},\"backend\":{:?},\"driverValidation\":{},\"rgValidation\":\"full\",\"flights\":2,\"assets\":1,\"materialsMax\":{},\"parentGroups\":100,\"parentBridge\":\"SceneComponent-to-proxy\",\"staticPoliciesPerPrimitive\":2,\"native\":\"real Forward groups and Ready PSOs\",\"record\":\"CPU counting encoder\",\"sort\":\"included in indexed build, excluded from native stage\",\"timing\":\"single observation; no performance threshold\"}}\n",
               n, render::test::BackendName(device.GetBackend()), deviceContext.ValidationEnabled, uniqueMaterials.size() + 2);
    fmt::print("SCALING_UNITS {{\"time\":\"nanoseconds per observed phase\",\"publishedBytes\":\"table rows including MaterialRenderData headers\",\"publishedMaterialBytes\":\"complete material payload including headers; overlaps publishedBytes; not additive\",\"phaseAccounting\":\"Forward staging emits extra test indices before indexed build; phase timings are not a production PreparationTotal\",\"drawsPerN\":{},\"baseline\":\"two flight publications excluded before every change\"}}\n", 240ull * n);
    fmt::print("SCALING_DISTRIBUTION {{\"name\":{:?},\"geometryDescriptions\":{},\"physicalGeometryBuffers\":2,\"authoringMaterials\":{},\"escaped\":{}}}\n",
               DistributionName(distribution), geometries.size(), uniqueMaterials.size() + 2, distribution == ScalingDistribution::EscapedMaterial);
    uint32_t rows = 0;
    for (const auto change : {ScalingChange::ParentTransform, ScalingChange::IndexRange, ScalingChange::SharedMaterialValues}) {
        for (const uint32_t percent : {0u, 1u, 10u, 100u}) {
            const uint32_t k = n / 100 * percent;
            for (const uint32_t viewCount : {1u, 3u, 6u}) {
                SCOPED_TRACE(fmt::format("N={} K={} views={} kind={} distribution={}", n, k, viewCount, ChangeName(change), DistributionName(distribution)));
                for (auto& parent : parents) parent.SetRelativeLocation(Eigen::Vector3f::Zero());
                ASSERT_TRUE(baseMaterial->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
                ASSERT_TRUE(sharedMaterial->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
                for (uint32_t index = 0; index < n; ++index) {
                    primitives[index]->SetExpanded(false);
                    primitives[index]->SetMaterial(change == ScalingChange::SharedMaterialValues && index < k ? sharedMaterial.Get() : baselineMaterial(index));
                }
                // Both actual publication targets and their object caches receive the baseline.
                // Subsequent timings/counts exclude this reset and initial full materialization.
                for (uint32_t flight = 0; flight < 2; ++flight) {
                    ASSERT_TRUE(publish(flight));
                    objects[flight].Update(*flights[flight]);
                    ASSERT_EQ(flights[flight]->DrawRecords.size(), uint64_t{n} * 2);
                }
                childNotifications = 0;
                uint64_t authorCalls = 0;
                const auto authorBegin = ScalingClock::now();
                if (change == ScalingChange::ParentTransform) {
                    for (uint32_t parent = 0; parent < percent; ++parent) {
                        parents[parent].SetRelativeLocation({.02f, 0, 0});
                        ++authorCalls;
                    }
                } else if (change == ScalingChange::IndexRange) {
                    for (uint32_t index = 0; index < k; ++index) {
                        primitives[index]->SetExpanded(true);
                        ++authorCalls;
                    }
                } else if (k) {
                    if (distribution == ScalingDistribution::EscapedMaterial) {
                        ASSERT_GE(escapedSharedBytes.size(), sizeof(Forward_MaterialData));
                        auto values = *AsCBuffer<Forward_MaterialData>(escapedSharedBytes);
                        values.BaseColor = Eigen::Vector4f{2, 2, 2, 2};
                        std::memcpy(escapedSharedBytes.data(), &values, sizeof(values));
                    } else {
                        ASSERT_TRUE(sharedMaterial->SetFloat4("BaseColor", Eigen::Vector4f::Constant(2)));
                    }
                    ++authorCalls;
                }
                const auto authorNs = ElapsedNs(authorBegin);
                const bool primitiveChange = change != ScalingChange::SharedMaterialValues;
                EXPECT_EQ(scene.GetPendingRenderChangeCount(), primitiveChange ? k : 0u);
                EXPECT_EQ(childNotifications, change == ScalingChange::ParentTransform ? k : 0u);
                for (uint32_t flight = 0; flight < 2; ++flight) {
                    const auto publishBegin = ScalingClock::now();
                    ASSERT_TRUE(publish(flight));
                    const auto publishNs = ElapsedNs(publishBegin);
                    const auto& snapshot = *flights[flight];
                    const auto& counters = snapshot.Stats;
                    const auto objectBegin = ScalingClock::now();
                    const auto derivedRows = objects[flight].Update(snapshot);
                    const auto objectNs = ElapsedNs(objectBegin);
                    const uint64_t changedNow = flight == 0 && primitiveChange ? k : 0;
                    const uint64_t rebuiltNow = change == ScalingChange::IndexRange ? 2 * changedNow : 0;
                    EXPECT_EQ(scene.GetCommitStats().DirtySlots, changedNow);
                    EXPECT_EQ(scene.GetCommitStats().LegacyProxiesObserved, 0u);
                    EXPECT_EQ(counters.DrawRecordPrimitivesVisited, changedNow);
                    EXPECT_EQ(counters.DrawRecordCopies, 2 * changedNow);
                    EXPECT_EQ(counters.DrawRecordBuilds, rebuiltNow);
                    EXPECT_EQ(counters.StaticRecipeCompiles, rebuiltNow);
                    EXPECT_EQ(counters.BindingRecipeCompiles, 0u);
                    EXPECT_EQ(counters.DrawRecordFullSyncs, 0u);
                    EXPECT_EQ(counters.PrimitiveStructuresRebuilt, change == ScalingChange::IndexRange ? changedNow : 0u);
                    EXPECT_EQ(counters.PrimitiveBoundsRebuilt, changedNow);
                    EXPECT_EQ(derivedRows, change == ScalingChange::ParentTransform ? k : 0u);
                    EXPECT_EQ(counters.MaterialsRebuilt, change == ScalingChange::SharedMaterialValues && k && flight == 0 ? 1u : 0u);
                    EXPECT_EQ(counters.MaterialBytesCopied, change == ScalingChange::SharedMaterialValues && k && flight == 0 ? sizeof(Forward_MaterialData) : 0u);
                    EXPECT_EQ(counters.LegacyMaterialsObserved, distribution == ScalingDistribution::EscapedMaterial ? snapshot.Materials.size() : 0u);
                    EXPECT_EQ(counters.PendingResourcesObserved, 0u);
                    const uint64_t primitivePages = primitiveChange ? PageCount(k, 32) : 0;
                    const uint64_t drawPages = primitiveChange ? PageCount(2ull * k, 16) : 0;
                    const uint64_t batchPages = change == ScalingChange::IndexRange ? PageCount(k, 64) : 0;
                    const uint64_t materialPages = change == ScalingChange::SharedMaterialValues && k ? 1 : 0;
                    EXPECT_EQ(counters.PublishedPages, primitivePages + drawPages + batchPages + materialPages);
                    const uint64_t expectedBytes =
                        (primitiveChange ? PublishedRows(k, 32, n) * sizeof(RenderPrimitiveData) + PublishedRows(2ull * k, 16, 2ull * n) * sizeof(DrawRecord) : 0) +
                        (change == ScalingChange::IndexRange ? PublishedRows(k, 64, n) * sizeof(MeshBatch) : 0) + materialPages * sizeof(MaterialRenderData);
                    EXPECT_EQ(counters.PublishedBytes, expectedBytes);
                    uint64_t materialPayloadBytes = 0;
                    for (const auto& material : snapshot.Materials) {
                        ASSERT_EQ(material.Passes.size(), 1u);
                        const auto& pass = material.Passes[0];
                        ASSERT_EQ(pass.NumericBytes.size(), sizeof(Forward_MaterialData));
                        const auto* values = AsCBuffer<Forward_MaterialData>(pass.NumericBytes);
                        const bool edited = change == ScalingChange::SharedMaterialValues && k && material.Generation == sharedMaterial->GetGeneration();
                        const auto color = static_cast<Eigen::Vector4f>(values->BaseColor);
                        EXPECT_TRUE(color.isApprox(Eigen::Vector4f::Constant(edited ? 2.0f : 1.0f)));
                        if (edited) materialPayloadBytes = sizeof(material) + sizeof(pass) + pass.PassName.size() + pass.NumericBytes.size();
                    }
                    EXPECT_EQ(counters.PublishedMaterialBytes, materialPayloadBytes);
                    EXPECT_EQ(snapshot.ChangedPrimitiveRanges.size(), primitivePages);
                    EXPECT_EQ(counters.MissingGeometry + counters.EmptyDraw + counters.InvalidDrawRange + counters.MaterialUnavailable + counters.InvalidBounds, 0u);
                    // Verify exact primitive -> material/range output, independently of work counters.
                    uint64_t wrongRows = 0;
                    for (const auto& record : snapshot.DrawRecords) {
                        const bool shared = change == ScalingChange::SharedMaterialValues && record.Primitive < k;
                        const uint64_t generation = shared ? sharedMaterial->GetGeneration() : baselineMaterial(record.Primitive)->GetGeneration();
                        wrongRows += record.Material >= snapshot.Materials.size() || snapshot.Materials[record.Material].Generation != generation ||
                                     record.Description.IndexCount != (change == ScalingChange::IndexRange && record.Primitive < k ? 6u : 3u);
                    }
                    ASSERT_EQ(wrongRows, 0u);
                    if (flight == 0 && k) {
                        EXPECT_EQ(flights[1]->DrawRecords[0].Description.IndexCount, 3u);
                        EXPECT_FLOAT_EQ(flights[1]->Primitives[0].LocalToWorld(0, 3), 0);
                        for (const auto& material : flights[1]->Materials) {
                            const auto* values = AsCBuffer<Forward_MaterialData>(material.Passes[0].NumericBytes);
                            EXPECT_TRUE(static_cast<Eigen::Vector4f>(values->BaseColor).isApprox(Eigen::Vector4f::Ones()));
                        }
                    }
                    const uint64_t indicesPerView = 3ull * n + (change == ScalingChange::IndexRange ? 3ull * k : 0);
                    ScalingConsumerStats consumer;
                    ConsumeScalingSnapshot(deviceContext, registry, pool, *commands, serial, snapshot, geometries,
                                           objects[flight].Rows(), *frameResources[flight], hostWrites[flight], viewCount, indicesPerView, consumer);
                    ASSERT_FALSE(HasFatalFailure());
                    EXPECT_EQ(consumer.Visible, uint64_t{n} * viewCount);
                    EXPECT_EQ(consumer.NativeRecordCalls, consumer.Visible);
                    EXPECT_EQ(consumer.IndexedRecordCalls, consumer.Visible);
                    EXPECT_EQ(consumer.RecordedDraws, consumer.Visible);
                    fmt::print("SCALING {{\"n\":{},\"k\":{},\"percent\":{},\"views\":{},\"kind\":{:?},\"flight\":{},\"authorCalls\":{},\"childNotifications\":{},\"dirtySlots\":{},\"recipesBuilt\":{},\"recordVisits\":{},\"recordCopies\":{},\"boundsBuilt\":{},\"derivedRows\":{},\"materialBuilds\":{},\"materialBuildBytes\":{},\"publishedPages\":{},\"publishedBytes\":{},\"publishedMaterialBytes\":{},\"nativeGroups\":{},\"nativeUploadRows\":{},\"nativeUploadBytes\":{},\"nativeSetsCreated\":{},\"visible\":{},\"readyDraws\":{},\"recordedDraws\":{},\"authorNs\":{},\"publishNs\":{},\"objectNs\":{},\"cullNs\":{},\"forwardNativeAndStagingNs\":{},\"indexedBuildAndSortNs\":{},\"readyNs\":{},\"countRecordNs\":{},\"nativeRecordCalls\":{},\"indexedRecordCalls\":{},\"materialSubscribers\":{}}}\n",
                               n, k, percent, viewCount, ChangeName(change), flight, flight == 0 ? authorCalls : 0, flight == 0 ? childNotifications : 0,
                               scene.GetCommitStats().DirtySlots, counters.StaticRecipeCompiles, counters.DrawRecordPrimitivesVisited, counters.DrawRecordCopies,
                               counters.PrimitiveBoundsRebuilt, derivedRows, counters.MaterialsRebuilt, counters.MaterialBytesCopied,
                               counters.PublishedPages, counters.PublishedBytes, counters.PublishedMaterialBytes, consumer.Native.GroupPreparations,
                               consumer.Native.SharedBufferUploads, consumer.Native.BufferBytesCopied, consumer.Native.SetCreations,
                               consumer.Visible, consumer.ReadyDraws, consumer.RecordedDraws, flight == 0 ? authorNs : 0, publishNs, objectNs,
                               consumer.CullNs, consumer.ForwardNativeAndStagingNs, consumer.IndexedBuildAndSortNs, consumer.ReadyNs, consumer.CountRecordNs,
                               consumer.NativeRecordCalls, consumer.IndexedRecordCalls, change == ScalingChange::SharedMaterialValues ? k : 0u);
                    ++rows;
                }
            }
        }
    }
    EXPECT_EQ(rows, 72u);
    EXPECT_EQ(deviceContext.ValidationErrors.load(), 0u);
    RecordProperty("matrix_rows", rows);
    RecordProperty("primitive_count", n);
    RecordProperty("image_validation", "not performed; CPU counting encoder");
}

INSTANTIATE_TEST_SUITE_P(PrimitiveCounts, SceneScaling,
                         testing::Combine(testing::Values(1000u, 10000u, 100000u),
                                          testing::Values(ScalingDistribution::Repeated, ScalingDistribution::UniqueGeometry,
                                                          ScalingDistribution::UniqueMaterial, ScalingDistribution::EscapedMaterial)),
                         [](const testing::TestParamInfo<SceneScaling::ParamType>& value) {
                             return fmt::format("N{}_D{}", std::get<0>(value.param), static_cast<uint32_t>(std::get<1>(value.param)));
                         });

}  // namespace
}  // namespace radray
