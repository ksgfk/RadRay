#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include "stage_b_test_support.h"
#include "forward_pipeline/forward_lit_mesh_pass_processor.h"
#include "forward_pipeline/depth_only_mesh_pass_processor.h"

#include <gtest/gtest.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/render_framework/viewport.h>

namespace radray {
namespace {

class ImmediateWait final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};
class StreamComponent final : public PrimitiveComponent {
public:
    StreamComponent(StreamingAssetRef<StaticMesh> mesh, Material* material) : Mesh(std::move(mesh)), DrawMaterial(material) {}
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override {
        return make_unique<StaticMeshSceneProxy>(Mesh, vector<Nullable<Material*>>{DrawMaterial}, Eigen::Matrix4f::Identity());
    }
    StreamingAssetRef<StaticMesh> Mesh;
    Material* DrawMaterial;
};

class StageBDraw : public testing::TestWithParam<render::RenderBackend> {
protected:
    void SetUp() override {
        if (!render::test::TryCreateDevice(GetParam(), Device, true)) GTEST_SKIP() << "Backend unavailable";
    }
    void TearDown() override {
        if (Device.Queue) Device.Queue->Wait();
        EXPECT_EQ(Device.ValidationErrors.load(), 0u);
    }
    render::test::DeviceContext Device;
};

TEST_P(StageBDraw, MultipleVertexStreamsThroughSnapshotListsAndGraph) {
    auto& device = *Device.Device;
    constexpr std::string_view source = R"hlsl(
#include <pipelines/forward/bindings.hlsli>
struct Input { float3 Position : POSITION; float2 UV : TEXCOORD0; };
struct Output { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
[shader("vertex")] Output VSMain(Input input) {
    Output output;
    output.Position = mul(ForwardView.ViewProj, mul(ForwardObject.LocalToWorld, float4(input.Position, 1)));
    output.UV = input.UV;
    return output;
}
[shader("pixel")] float4 PSMain(Output input) : SV_Target0 { return float4(input.UV, .25f, 1) * ForwardMaterial.BaseColor; }
)hlsl";
    auto forward = test::CompileStageBProgram(device, source, ForwardPipeline::GetLayoutRecipe());
    const auto depthSource = ReadTextFile(std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib/pipelines/forward/depth_only.hlsl");
    ASSERT_TRUE(depthSource);
    auto depth = test::CompileStageBProgram(device, *depthSource, ForwardPipeline::GetDepthOnlyLayoutRecipe());
    ASSERT_TRUE(forward);
    ASSERT_TRUE(depth);
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    auto technique = MaterialTechnique::Create({{"ForwardLit", forward.Get(), "ForwardMaterial", state}, {"DepthOnly", depth.Get(), "", state}}, "ForwardLit");
    ASSERT_TRUE(technique);
    auto material = Material::Create(technique.Get());
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    constexpr array<float, 12> positions{-1, -1, .5f, 1, -1, .5f, 1, 1, .5f, -1, 1, .5f};
    constexpr array<float, 8> uvs{0, 1, 1, 1, 1, 0, 0, 0};
    constexpr array<uint32_t, 6> indices{0, 2, 1, 0, 3, 2};
    auto positionBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto uvBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{uvs}), render::BufferUse::Vertex);
    auto indexBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(positionBuffer);
    ASSERT_TRUE(uvBuffer);
    ASSERT_TRUE(indexBuffer);
    GpuMesh geometry;
    GpuMesh::DrawData draw;
    draw.VertexBuffers = {{3, {positionBuffer.Get(), 0, sizeof(positions)}}, {7, {uvBuffer.Get(), 0, sizeof(uvs)}}};
    draw.Ibv = {indexBuffer.Get(), 0, 4};
    draw.VertexLayout.Buffers = {{3, 12, render::VertexStepMode::Vertex}, {7, 8, render::VertexStepMode::Vertex}};
    draw.VertexLayout.Attributes = {{"POSITION", 0, 3, 0, render::VertexFormat::FLOAT32X3}, {"TEXCOORD", 0, 7, 0, render::VertexFormat::FLOAT32X2}};
    geometry.Draws.push_back(std::move(draw));
    geometry.Buffers.push_back(positionBuffer.Release());
    geometry.Buffers.push_back(uvBuffer.Release());
    geometry.Buffers.push_back(indexBuffer.Release());
    ImmediateWait wait;
    AssetManager assets;
    assets.SetWaitFrameProcessor(&wait);
    auto mesh = assets.AddReady<StaticMesh>(AssetId{0x10011, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, make_unique<StaticMesh>(
                                                                                                 MeshResource{}, vector<StaticMeshSection>{{0, 0, 6, 0, 3}}, Eigen::Vector3f{-1, -1, .5f}, Eigen::Vector3f{1, 1, .5f}, std::move(geometry)));
    ASSERT_TRUE(mesh.IsReady());
    StreamComponent component{mesh, material.Get()};
    Scene scene;
    auto* proxy = scene.AddPrimitive(&component);
    ASSERT_NE(proxy, nullptr);
    RenderSceneSnapshot snapshot;
    vector<StreamingAssetRefAny> retained;
    ASSERT_TRUE(BuildRenderSceneSnapshot(scene, snapshot, retained));
    ASSERT_EQ(snapshot.MeshBatches.size(), 1u);
    ASSERT_EQ(snapshot.MeshBatches[0].Geometry->VertexBuffers.size(), 2u);
    scene.RemovePrimitive(proxy);
    component.Mesh.Reset();
    mesh.Reset();
    material = nullptr;
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 1u);

    ResolvedRenderView view;
    view.View = view.Projection = view.ViewProjection = Eigen::Matrix4f::Identity();
    view.WorldPosition.setZero();
    view.ViewRect = view.ScissorRect = {0, 0, 64, 64};
    CullingResults culling;
    ASSERT_TRUE(Cull({&snapshot, &view}, culling));
    HostWriteBatch writes;
    FrameDrawResources resources{&device};
    ASSERT_TRUE(resources.BeginFrame(writes));
    forward_detail::ForwardBindingCache bindings;
    forward_detail::DepthOnlyBindingCache depthBindings;
    bool warned = false;
    forward_detail::ForwardLitMeshPassProcessor lit{resources, bindings, warned};
    forward_detail::DepthOnlyMeshPassProcessor z{resources, depthBindings};
    RendererList opaque, depthList;
    ASSERT_TRUE(BuildRendererList({"opaque", "ForwardLit", &culling, &view, RenderQueueRange::Opaque()}, lit, opaque));
    ASSERT_TRUE(BuildRendererList({"depth", "DepthOnly", &culling, &view, RenderQueueRange::Opaque()}, z, depthList));
    ASSERT_EQ(opaque.Commands.size(), 1u);
    ASSERT_EQ(depthList.Commands.size(), 1u);
    EXPECT_EQ(opaque.Commands[0].Geometry->VertexBuffers[0].Binding, 3u);
    EXPECT_EQ(opaque.Commands[0].Geometry->VertexBuffers[1].Binding, 7u);
    MeshDrawCommand duplicate = opaque.Commands[0];
    duplicate.Groups.push_back(duplicate.Groups.front());
    EXPECT_FALSE(FinalizeMeshDrawCommand(duplicate));
    opaque.Commands.push_back(duplicate);
    GpuMesh::DrawData incompatible = *snapshot.MeshBatches[0].Geometry.Get();
    incompatible.VertexLayout.Attributes[0].Semantic = "MISSING_POSITION";
    MeshDrawCommand psoFailure = opaque.Commands[0];
    psoFailure.Geometry = &incompatible;
    opaque.Commands.push_back(std::move(psoFailure));

    render::RenderPassRegistry registry{&device};
    RenderResourcePool pool{device, registry};
    pool.BeginFlight(1);
    RenderGraph graph{device, pool, registry, "StageB streams"};
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 64, 64, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "color");
    const auto depthTarget = graph.CreateTexture({render::TextureDimension::Dim2D, 64, 64, 1, 1, 1, render::TextureFormat::D32_FLOAT, render::MemoryType::Device, render::TextureUse::DepthStencilWrite, {}}, "depth");
    DrawExecutionStats execution;
    struct Payload {
        const RendererList* List;
        DrawExecutionStats* Stats;
        render::RenderBackend Backend;
    };
    const auto execute = +[](const Payload& payload, RenderGraphRasterContext& ctx) {
        ctx.Encoder().SetViewport(MakeViewport(payload.Backend, 0, 0, 64, 64));
        ctx.Encoder().SetScissor({0, 0, 64, 64});
        SubmitRendererList(*payload.List, ctx, ctx.PassState(), *payload.Stats);
    };
    graph.AddRasterPass<Payload>("depth", [&](Payload& data, RenderGraphRasterBuilder& builder) {
        data = {&depthList, &execution, GetParam()}; builder.SetDepthAttachment(depthTarget); }, execute);
    graph.AddRasterPass<Payload>("opaque", [&](Payload& data, RenderGraphRasterBuilder& builder) {
        data = {&opaque, &execution, GetParam()}; builder.SetColorAttachment(0, color); builder.SetDepthAttachment(depthTarget, {.Load = render::LoadAction::Load}); }, execute);
    const auto row = Align(uint64_t{64 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({row * 64, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer destination{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    const auto output = graph.ImportBuffer(destination, "readback", RenderGraphExternalAccess::ObservableOutput);
    graph.AddCopyTextureToBufferPass("readback", color, output);
    graph.AddComputePass<uint32_t>("host visibility", [&](uint32_t&, RenderGraphComputeBuilder& builder) {
        builder.ReadBuffer(output, RgBufferAccess::HostRead); builder.SetSideEffect(); }, +[](const uint32_t&, RenderGraphComputeContext&) {});
    writes.Flush(device);
    const auto setsBefore = resources.GetSetCount();
    const auto commitsBefore = writes.GetStats().CommitCount;
    auto command = device.CreateCommandBuffer(Device.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success) << graph.GetReport().ToText();
    EXPECT_EQ(resources.GetSetCount(), setsBefore);
    EXPECT_EQ(writes.GetStats().CommitCount, commitsBefore);
    EXPECT_EQ(execution.Draws, 2u);
    EXPECT_EQ(execution.BindingFailure, 1u);
    EXPECT_EQ(execution.PsoFailure, 1u);
    command->End();
    auto* raw = command.Get();
    Device.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
    Device.Queue->Wait();
    const auto* mapped = static_cast<const uint8_t*>(readback->Map(0, row * 64));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, row * 64});
    const auto* pixel = mapped + row * 32 + 32 * 4;
    EXPECT_NEAR(pixel[0], 128, 4);
    EXPECT_NEAR(pixel[1], 128, 4);
    EXPECT_NEAR(pixel[2], 64, 1);
    EXPECT_EQ(pixel[3], 255);
    readback->Unmap();
    opaque.ResetForReuse();
    depthList.ResetForReuse();
    retained.clear();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 0u);
}

TEST_P(StageBDraw, ReadOnlyDepthPassPreservesEverySupportedDepthFormat) {
    auto& device = *Device.Device;
    render::RenderPassRegistry registry{&device};
    RenderResourcePool pool{device, registry};
    pool.BeginFlight(1);
    RenderGraph graph{device, pool, registry, "Read-only depth formats"};
    uint32_t formats = 0;
    for (const auto format : {render::TextureFormat::D32_FLOAT, render::TextureFormat::D24_UNORM_S8_UINT, render::TextureFormat::D16_UNORM}) {
        const auto usage = render::TextureUse::DepthStencilRead | render::TextureUse::DepthStencilWrite;
        if (!SelectFirstSupportedFormat(device, std::span{&format, 1}, render::TextureDimension::Dim2D, usage, 1)) continue;
        ++formats;
        const auto depth = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, format, render::MemoryType::Device, usage}, "depth");
        graph.AddRasterPass<uint32_t>("clear", [&](uint32_t&, RenderGraphRasterBuilder& builder) { builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Clear}); }, +[](const uint32_t&, RenderGraphRasterContext&) {});
        graph.AddRasterPass<uint32_t>("read-only", [&](uint32_t&, RenderGraphRasterBuilder& builder) {
            builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Load, .ReadOnly = true});
            builder.SetSideEffect(); }, +[](const uint32_t&, RenderGraphRasterContext&) {});
    }
    ASSERT_GT(formats, 0u);
    auto command = device.CreateCommandBuffer(Device.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success) << graph.GetReport().ToText();
    command->End();
    auto* raw = command.Get();
    Device.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
    Device.Queue->Wait();
}

INSTANTIATE_TEST_SUITE_P(Backends, StageBDraw, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
