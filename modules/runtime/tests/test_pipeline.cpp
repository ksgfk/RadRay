#include "scene_test_support.h"
#include "upload_test_support.h"
#include <gtest/gtest.h>
#include <radray/runtime/render_framework/material.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/graphics_pipeline_cache.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>

namespace radray::test {
namespace {
MaterialPass TestMaterialPass() {
    MaterialPass pass;
    pass.Name = "Unlit";
    pass.Program.SourceName = "material.hlsl";
    pass.Inputs = {{"Tint", MaterialInputSource::MaterialConstants}};
    pass.ObjectIndexPushConstant = "DrawData";
    return pass;
}
TEST(PipelineMaterial, TypedUpdatesCoalesceAndDoNotDirtyObjectMatrices) {
    Application app;
    AssetManager assets;
    SceneManager renderer{2};
    auto scene = renderer.CreateSceneGT();
    auto* writer = renderer.GetSceneWriterGT(scene).Get();
    auto shape = writer->CreateShape();
    writer->SetStaticMesh(shape, {}, Eigen::Matrix4f::Identity());
    const AssetId firstId{510, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const AssetId secondId{511, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    auto first = assets.AddReady<Material>(firstId, make_unique<Material>(vector{TestMaterialPass()}, vector<vector<byte>>{vector<byte>(16)}));
    auto second = assets.AddReady<Material>(secondId, make_unique<Material>(vector{TestMaterialPass()}, vector<vector<byte>>{vector<byte>(32)}));
    ASSERT_TRUE(first.Get()->IsValid());
    writer->SetMaterials(shape, std::span{&first, 1});
    writer->SetMaterials(shape, std::span{&second, 1});
    renderer.SealFrameGT(0);
    ASSERT_EQ(SceneBatch(renderer, scene, 0).Materials.size(), 1u);
    EXPECT_EQ(SceneBatch(renderer, scene, 0).Materials[0].Materials[0].Id, secondId);
    ConsumeFrame(renderer, 0);
    CompleteFrame(renderer, 0);
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 3) = 1;
    writer->SetTransform(shape, transform);
    renderer.SealFrameGT(1);
    EXPECT_TRUE(SceneBatch(renderer, scene, 1).Materials.empty());
    ConsumeFrame(renderer, 1);
    CompleteFrame(renderer, 1);
    writer->SetMaterials(shape, std::span{&first, 1});
    renderer.SealFrameGT(0);
    EXPECT_TRUE(SceneBatch(renderer, scene, 0).MeshStates.empty());
    EXPECT_TRUE(SceneBatch(renderer, scene, 0).Transforms.empty());
    ConsumeFrame(renderer, 0);
    EXPECT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(shape)->Materials[0].Id, firstId);
    first.Reset();
    second.Reset();
    assets.Pump();
    EXPECT_TRUE(assets.Find<Material>(secondId));
    CompleteFrame(renderer, 0);
    assets.Pump();
    EXPECT_FALSE(assets.Find<Material>(secondId));
    EXPECT_TRUE(assets.Find<Material>(firstId));
    writer->RemoveShape(shape);
    renderer.SealFrameGT(1);
    ConsumeFrame(renderer, 1);
    assets.Pump();
    EXPECT_TRUE(assets.Find<Material>(firstId));
    CompleteFrame(renderer, 1);
    assets.Pump();
    EXPECT_FALSE(assets.Find<Material>(firstId));
    renderer.DestroySceneGT(scene);
    renderer.SealFrameGT(0);
    ConsumeFrame(renderer, 0);
    CompleteFrame(renderer, 0);
}
TEST(PipelineMaterial, ComponentMaterialChangeUsesItsOwnTypedUpdate) {
    Application app;
    AssetManager assets;
    SceneManager renderer{1};
    ScopedWorld world;
    const auto scene = ConnectWorld(world, renderer);
    auto* mesh = world.SpawnActor()->AddComponent<StaticMeshComponent>();
    SceneUpdateBatch initial;
    CollectScene(world, renderer, initial);
    auto material = assets.AddReady<Material>({512, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<Material>(vector{TestMaterialPass()}, vector<vector<byte>>{vector<byte>(16)}));
    mesh->SetMaterials({material});
    PrepareScene(world, renderer, 0);
    const auto& batch = SceneBatch(renderer, scene, 0);
    EXPECT_TRUE(batch.MeshStates.empty());
    EXPECT_TRUE(batch.Transforms.empty());
    EXPECT_TRUE(batch.LocalTransforms.empty());
    ASSERT_EQ(batch.Materials.size(), 1u);
    ConsumeFrame(renderer, 0);
    CompleteFrame(renderer, 0);
}
TEST(PipelineVertexLayout, OwnsSemanticsAndRejectsAmbiguousOrOutOfBoundsInputs) {
    GeometryVertexLayout layout{{{0, 24, render::VertexStepMode::Vertex}, {1, 8, render::VertexStepMode::Vertex}},
                                {{"POSITION", 0, render::VertexFormat::FLOAT32X3, 0, 0}, {"NORMAL", 0, render::VertexFormat::FLOAT32X3, 0, 12}, {"TEXCOORD", 0, render::VertexFormat::FLOAT32X2, 1, 0}}};
    EXPECT_TRUE(ValidateGeometryVertexLayout(layout));
    const auto copy = layout;
    layout.Attributes[0].Semantic = "OTHER";
    EXPECT_EQ(copy.Attributes[0].Semantic, "POSITION");
    layout.Attributes.push_back(layout.Attributes[0]);
    EXPECT_FALSE(ValidateGeometryVertexLayout(layout));
    layout = copy;
    layout.Attributes[1].Offset = 20;
    EXPECT_FALSE(ValidateGeometryVertexLayout(layout));
    layout = copy;
    layout.Streams[1].Binding = 0;
    EXPECT_FALSE(ValidateGeometryVertexLayout(layout));
}
TEST(PipelineCacheKey, ContentEqualitySurvivesStorageChangesAndHashCollisions) {
    GraphicsPipelineKey key{};
    key.VertexInput = {{{0, 12, render::VertexStepMode::Vertex}}, {{"POSITION", 0, render::VertexFormat::FLOAT32X3}}};
    key.Primitive = render::PrimitiveState::Default();
    key.Samples = render::MultiSampleState::Default();
    key.Colors.push_back(render::ColorTargetState::Default(render::TextureFormat::RGBA8_UNORM));
    const auto copy = key;
    EXPECT_EQ(key, copy);
    EXPECT_EQ(GraphicsPipelineKeyHash{}(key), GraphicsPipelineKeyHash{}(copy));
    struct ConstantHash {
        size_t operator()(const GraphicsPipelineKey&) const noexcept { return 1; }
    };
    unordered_map<GraphicsPipelineKey, int, ConstantHash> entries;
    entries.emplace(key, 1);
    auto changed = key;
    changed.Primitive.FaceClockwise = render::FrontFace::CCW;
    entries.emplace(changed, 2);
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries.at(copy), 1);
    changed = key;
    changed.VertexInput.Attributes[0].Location = 2;
    EXPECT_NE(changed, key);
    changed = key;
    changed.Samples.Mask = 7;
    EXPECT_NE(changed, key);
    changed = key;
    changed.Colors[0].WriteMask = render::ColorWrite::Red;
    EXPECT_NE(changed, key);
    changed = key;
    changed.Depth = render::DepthStencilState::Default();
    EXPECT_NE(changed, key);
}

TEST(PipelineMaterial, SharedMaterialRetainsTextureUntilBothScenesRetire) {
    Application app;
    UploadTestDevice device;
    {
        AssetManager assets;
        SceneManager renderer{2};
        auto texture = device.CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 1, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Usage = render::TextureUse::Resource}).Unwrap();
        auto view = device.CreateTextureView({texture.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap();
        auto image = assets.AddReady<TextureAsset>({520, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<TextureAsset>(&device, "retained", std::move(texture), std::move(view)));
        auto pass = TestMaterialPass();
        pass.Inputs.push_back({"Albedo", MaterialInputSource::Texture});
        auto material = assets.AddReady<Material>({521, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<Material>(vector{pass}, vector<vector<byte>>{vector<byte>(16)}, vector{image}));
        const auto first = renderer.CreateSceneGT(), second = renderer.CreateSceneGT();
        for (const auto scene : {first, second}) {
            auto writer = renderer.GetSceneWriterGT(scene);
            const auto shape = writer->CreateShape();
            writer->SetStaticMesh(shape, {}, Eigen::Matrix4f::Identity());
            writer->SetMaterials(shape, std::span{&material, 1});
        }
        renderer.SealFrameGT(0);
        ConsumeFrame(renderer, 0);
        CompleteFrame(renderer, 0);
        material.Reset();
        image.Reset();
        assets.Pump();
        EXPECT_EQ(device.LiveTextures, 1);
        renderer.DestroySceneGT(first);
        renderer.SealFrameGT(0);
        ConsumeFrame(renderer, 0);
        CompleteFrame(renderer, 0);
        assets.Pump();
        EXPECT_EQ(device.LiveTextures, 1);
        renderer.DestroySceneGT(second);
        renderer.SealFrameGT(1);
        ConsumeFrame(renderer, 1);
        assets.Pump();
        EXPECT_EQ(device.LiveTextures, 1);
        CompleteFrame(renderer, 1);
        assets.Pump();
        assets.Pump();
        EXPECT_EQ(assets.GetAssetCount(), 0u);
    }  // TextureAsset uses the existing deferred-destruction service.
    EXPECT_EQ(device.LiveTextures, 0);
    EXPECT_EQ(device.LiveTextureViews, 0);
}
TEST(PipelineMaterialDeathTest, StaleMaterialGenerationCannotOverwriteReusedShape) {
    RenderScene scene;
    SceneUpdateBatch initial;
    initial.CreateShapes.push_back({0, 0});
    initial.MeshStates.push_back({{0, 0}, {}, Eigen::Matrix4f::Identity()});
    scene.Apply(initial);
    SceneUpdateBatch replace;
    replace.RemoveShapes.push_back({0, 0});
    replace.CreateShapes.push_back({0, 1});
    replace.MeshStates.push_back({{0, 1}, {}, Eigen::Matrix4f::Identity()});
    const MaterialBinding current{{530, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, nullptr};
    replace.Materials.push_back({{0, 1}, {current}});
    scene.Apply(replace);
    SceneUpdateBatch stale;
    stale.Materials.push_back({{0, 0}, {}});
    EXPECT_DEATH(scene.Apply(stale), "");
    auto object = scene.GetStaticMesh({0, 1});
    ASSERT_TRUE(object);
    ASSERT_EQ(object->Materials.size(), 1u);
    EXPECT_EQ(object->Materials[0].Id, current.Id);
}

class PipelineTestAllocator final : public ICmdAllocator {
public:
    vector<unique_ptr<UploadTestCommand>> Commands;
    vector<render::CommandBuffer*> Returned;
    render::CommandBuffer* Allocate() override {
        Commands.push_back(make_unique<UploadTestCommand>());
        Commands.back()->Begin();
        return Commands.back().get();
    }
    void Return(std::span<render::CommandBuffer*> commands) override {
        for (auto* command : commands) {
            command->End();
            Returned.push_back(command);
        }
    }
};
class PipelineTestRecorder final : public RenderPipeline {
public:
    bool Fail{false}, Produce{false};
    PipelineRecordResult Record(PipelineContext& context, const RenderPipelineRequest& request) override {
        PipelineRecordResult result;
        result.Status = Fail ? PipelineRecordStatus::RecoverableFailure : PipelineRecordStatus::NoWork;
        if (Produce) {
            auto* first = context.Commands.Allocate();
            auto* second = context.Commands.Allocate();
            if (request.Output) {
                result.OutputState = InitialRenderOutputState(*request.Output);
                TransitionRenderOutput(first, *request.Output, *result.OutputState, false);
            }
            context.Commands.Return(std::span{&second, 1});
            context.Commands.Return(std::span{&first, 1});
            result.Commands = {first, second};
        }
        return result;
    }
};
TEST(PipelineOutput, EmptyFailureAndNoScenePreserveRecordingOrderAndFinishAttachments) {
    Application app;
    UploadTestDevice device;
    RenderSystem renderer{&device};
    HostWriteBatch writes;
    ResourceUploader uploader{&device, 1};
    uploader.BeginFlight(0, writes);
    GpuFrameResources resources{&device, uploader, writes};
    PipelineTestAllocator allocator;
    PipelineContext context{renderer, {0, 1, {}}, allocator, resources};
    PipelineTestRecorder pipeline;
    auto texture = device.CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 64, .Height = 32, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Usage = render::TextureUse::RenderTarget}).Unwrap();
    auto view = device.CreateTextureView({.Target = texture.get(), .Dim = render::TextureDimension::Dim2D, .Format = render::TextureFormat::RGBA8_UNORM, .Range = {0, 1, 0, 1}, .Usage = render::TextureViewUsage::RenderTarget}).Unwrap();
    RenderOutput output{{{view.get(), render::TextureState::Undefined, render::TextureState::ShaderRead}}, {}};
    auto empty = RecordRenderPipeline(pipeline, context, {{}, {}});
    EXPECT_TRUE(empty.Commands.empty());
    auto clear = RecordRenderPipeline(pipeline, context, {{}, output});
    ASSERT_EQ(clear.Commands.size(), 1u);
    EXPECT_TRUE(IsRenderOutputFinished(output, *clear.OutputState));
    pipeline.Fail = true;
    pipeline.Produce = true;
    auto failed = RecordRenderPipeline(pipeline, context, {{}, output});
    ASSERT_EQ(failed.Commands.size(), 3u);
    EXPECT_EQ(failed.Status, PipelineRecordStatus::RecoverableFailure);
    EXPECT_EQ(failed.Commands[0], allocator.Commands[1].get());
    EXPECT_EQ(failed.Commands[1], allocator.Commands[2].get());
    EXPECT_EQ(allocator.Returned[1], failed.Commands[1]);
    EXPECT_EQ(allocator.Returned[2], failed.Commands[0]);
    EXPECT_TRUE(IsRenderOutputFinished(output, *failed.OutputState));
    auto alias = device.CreateTextureView(view->GetDesc()).Unwrap();
    output.Colors.push_back({alias.get(), render::TextureState::Undefined, render::TextureState::ShaderRead});
    EXPECT_FALSE(ValidateRenderOutput(output, device));
    uploader.EndFlight(0);
}
TEST(FrameResources, ConstantSlicesStayDistinctAndAlignedAcrossPageGrowth) {
    UploadTestDevice device;
    HostWriteBatch writes;
    ResourceUploader uploader{&device, 1};
    GpuFrameResources resources{&device, uploader, writes};
    vector<MappedUploadPage::Allocation> slices;
    for (uint32_t i = 0; i < 300; ++i) {
        auto reservation = resources.AllocateConstants(4096);
        ASSERT_TRUE(reservation.IsValid());
        std::memset(reservation.Data(), int(i % 251), 4096);
        auto slice = reservation.Commit(4096);
        EXPECT_EQ(slice.Offset % 256, 0u);
        for (const auto& previous : slices)
            if (previous.Target == slice.Target) EXPECT_GE(slice.Offset, previous.Offset + previous.Size);
        slices.push_back(slice);
    }
    EXPECT_GT(device.UploadAllocations, 1);
    for (uint32_t i = 0; i < slices.size(); ++i) {
        const auto& slice = slices[i];
        const auto* data = static_cast<const uint8_t*>(slice.Target->Map(slice.Offset, slice.Size));
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(data[0], i % 251);
        EXPECT_EQ(data[4095], i % 251);
    }
}
}  // namespace
}  // namespace radray::test
