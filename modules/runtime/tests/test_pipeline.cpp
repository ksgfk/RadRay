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
TEST(PipelineMaterial, InstanceUpdatesCoalesceAndCancelOnRemoveWithoutDirtyingMatrices) {
    SceneManager manager{1};
    auto scene = manager.CreateSceneGT();
    auto writer = manager.GetSceneWriterGT(scene);
    auto id = writer->CreateShape();
    writer->SetStaticMesh(id, {}, Eigen::Matrix4f::Identity());
    InstanceParameterLayout layout{"Color", 16, {{"RGBA", 0, 16, 0, 1, 1, 4}}};
    array<byte, 16> values{};
    ASSERT_TRUE(writer->SetInstanceParameters(id, layout, values));
    values[0] = byte{7};
    ASSERT_TRUE(writer->SetInstanceParameters(id, layout, values));
    EXPECT_FALSE(writer->SetInstanceParameters(id, layout, std::span{values}.first(15)));
    manager.SealFrameGT(0);
    EXPECT_EQ(SceneBatch(manager, scene, 0).Parameters.size(), 1u);
    ConsumeFrame(manager, 0);
    CompleteFrame(manager, 0);
    values[0] = byte{9};
    ASSERT_TRUE(writer->SetInstanceParameters(id, layout, values));
    manager.SealFrameGT(0);
    EXPECT_TRUE(SceneBatch(manager, scene, 0).Transforms.empty());
    EXPECT_TRUE(SceneBatch(manager, scene, 0).MeshStates.empty());
    ConsumeFrame(manager, 0);
    CompleteFrame(manager, 0);
    ASSERT_TRUE(writer->SetInstanceParameters(id, layout, values));
    writer->RemoveShape(id);
    auto replacement = writer->CreateShape();
    EXPECT_EQ(id.Index, replacement.Index);
    EXPECT_NE(id.Generation, replacement.Generation);
    writer->SetStaticMesh(replacement, {}, Eigen::Matrix4f::Identity());
    manager.SealFrameGT(0);
    EXPECT_TRUE(SceneBatch(manager, scene, 0).Parameters.empty());
    ConsumeFrame(manager, 0);
    CompleteFrame(manager, 0);
    manager.OnShutdown();
}
TEST(PipelineMaterial, BulkBindingsAndPackedParametersKeepOnlyFinalValuesAndFreezeAtSeal) {
    AssetManager assets;
    SceneManager manager{2};
    const auto scene = manager.CreateSceneGT();
    auto writer = manager.GetSceneWriterGT(scene);
    vector<ShapeId> ids(257);
    ASSERT_TRUE(writer->CreateShapes(ids));
    vector<Eigen::Matrix4f> matrices(ids.size(), Eigen::Matrix4f::Identity());
    writer->SetStaticMeshes(ids, {}, matrices);
    auto material = assets.AddReady<Material>({600, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<Material>(vector{TestMaterialPass()}, vector<vector<byte>>{vector<byte>(16)}));
    writer->SetMaterials(ids, std::span{&material, 1});
    InstanceParameterLayout layout{"Color", 16, {{"RGBA", 0, 16, 0, 1, 1, 4}}};
    vector<byte> bytes(ids.size() * 16, byte{1});
    ASSERT_TRUE(writer->SetInstanceParameters(ids, layout, bytes));
    array<byte, 16> final{};
    for (uint32_t i = 0; i < 10000; ++i) {
        final[0] = byte(i % 256);
        ASSERT_TRUE(writer->SetInstanceParameters(ids[128], layout, final));
        writer->SetMaterials(ids[128], std::span{&material, 1});
    }
    manager.SealFrameGT(0);
    const auto& sealed = SceneBatch(manager, scene, 0);
    ASSERT_EQ(sealed.Materials.size(), ids.size());
    ASSERT_EQ(sealed.Parameters.size(), ids.size());
    EXPECT_EQ(sealed.Materials[0].Materials.data(), sealed.Materials[128].Materials.data());
    EXPECT_EQ(sealed.Parameters[0].Layout.get(), sealed.Parameters[128].Layout.get());
    EXPECT_EQ(sealed.Parameters[0].Storage.get(), sealed.Parameters[128].Storage.get());
    EXPECT_EQ(sealed.Parameters[128].GetValues()[0], byte{15});
    final[0] = byte{7};
    ASSERT_TRUE(writer->SetInstanceParameters(ids[128], layout, final));
    EXPECT_EQ(sealed.Parameters[128].GetValues()[0], byte{15});
    ConsumeFrame(manager, 0);
    CompleteFrame(manager, 0);
    writer->RemoveShapes(std::span{ids}.subspan(10, 10));
    manager.SealFrameGT(1);
    ASSERT_EQ(SceneBatch(manager, scene, 1).Parameters.size(), 1u);
    ConsumeFrame(manager, 1);
    EXPECT_EQ(manager.GetSceneRT(scene)->GetParameterStreams().at("Color").GetValues(ids[128].Index)[0], byte{7});
    CompleteFrame(manager, 1);
    manager.OnShutdown();
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
TEST(PipelineMaterial, NativeWorldComponentUsesTheSameInstanceUpdateWithoutTransformOrMaterialCopies) {
    Application app;
    SceneManager manager{1};
    ScopedWorld world;
    const auto scene = ConnectWorld(world, manager);
    auto* mesh = world.SpawnActor()->AddComponent<StaticMeshComponent>();
    SceneUpdateBatch initial;
    CollectScene(world, manager, initial);
    InstanceParameterLayout layout{"Tint", 16, {{"Color", 0, 16, 0, 1, 1, 4}}};
    array<byte, 16> values{};
    ASSERT_TRUE(mesh->SetInstanceParameters(layout, values));
    PrepareScene(world, manager, 0);
    const auto& batch = SceneBatch(manager, scene, 0);
    EXPECT_TRUE(batch.Transforms.empty());
    EXPECT_TRUE(batch.LocalTransforms.empty());
    EXPECT_TRUE(batch.Materials.empty());
    ASSERT_EQ(batch.Parameters.size(), 1u);
    ConsumeFrame(manager, 0);
    CompleteFrame(manager, 0);
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
    }
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

class PipelineTestAllocator final : public IGpuCmdAllocator {
public:
    void Discard(std::span<render::CommandBuffer*> commands) override { Return(commands); }
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
    GpuFrameResources resources{&device};
    resources.BeginEpoch();
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
    resources.SealEpoch();
    resources.CompleteEpoch();
}
TEST(FrameResources, ConstantSlicesStayDistinctAndAlignedAcrossPageGrowth) {
    UploadTestDevice device;
    GpuFrameResources resources{&device};
    resources.BeginEpoch();
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

TEST(PipelineMaterial, PublishedMaterialsShareOnePassDefinitionAndKeepIndependentDefaults) {
    auto definition = make_shared<const MaterialPass>(TestMaterialPass());
    Material a{{MaterialPass{definition}}, {{byte{1}}}};
    Material b{{MaterialPass{definition}}, {{byte{2}}}};
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());
    EXPECT_EQ(&a.GetRenderData().Passes[0].GetDefinition(), definition.get());
    EXPECT_EQ(&b.GetRenderData().Passes[0].GetDefinition(), definition.get());
    EXPECT_EQ(a.GetRenderData().Constants[0][0], byte{1});
    EXPECT_EQ(b.GetRenderData().Constants[0][0], byte{2});
}
TEST(PipelineMaterial, SceneTemplatesShareGeometryAndOnlyChangeWithFinalBindings) {
    StaticMeshRenderData mesh;
    mesh.Mesh.Draws.resize(1);
    mesh.Sections.emplace_back(0, 0, 3, 0, 2);
    Material first{vector{TestMaterialPass()}, vector<vector<byte>>{vector<byte>(16)}};
    Material second{vector{TestMaterialPass()}, vector<vector<byte>>{vector<byte>(16)}};
    const MaterialBindings firstBindings{vector<MaterialBinding>{{{}, &first.GetRenderData(), &first.GetGpuState()}}};
    const MaterialBindings secondBindings{vector<MaterialBinding>{{{}, &second.GetRenderData(), &second.GetGpuState()}}};
    RenderScene scene;
    SceneUpdateBatch batch;
    const ShapeId a{0, 0}, b{1, 0};
    batch.CreateShapes = {a, b};
    batch.MeshStates = {{.Id = a}, {.Id = b}};
    scene.Apply(batch);
    EXPECT_TRUE(scene.GetDrawPrimitives(a).empty());
    EXPECT_TRUE(scene.GetDrawPrimitives(b).empty());
    batch.Clear();
    batch.MeshStates = {{.Id = a, .Mesh = {{}, &mesh}}, {.Id = b, .Mesh = {{}, &mesh}}};
    batch.Materials = {{a, firstBindings}, {b, firstBindings}};
    scene.Apply(batch);
    const auto original = scene.GetDrawPrimitives(a);
    ASSERT_EQ(original.size(), 1u);
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives(b).data());
    batch.Clear();
    Eigen::Matrix4f moved = Eigen::Matrix4f::Identity();
    moved(0, 3) = 1;
    batch.Transforms = {{a, moved}};
    scene.Apply(batch);
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives(a).data());
    batch.Clear();
    batch.Materials = {{a, secondBindings}, {a, firstBindings}};
    scene.Apply(batch);
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives(a).data());
    batch.Materials = {{a, secondBindings}};
    scene.Apply(batch);
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives(b).data());
    EXPECT_NE(original.data(), scene.GetDrawPrimitives(a).data());
    EXPECT_EQ(scene.GetDrawPrimitives(a)[0].Binding.Data.Get(), &second.GetRenderData());
    batch.Clear();
    batch.MeshStates = {{.Id = a}};
    scene.Apply(batch);
    EXPECT_TRUE(scene.GetDrawPrimitives(a).empty());
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives(b).data());
    batch.Clear();
    batch.RemoveShapes = {a};
    batch.CreateShapes = {{a.Index, 1}};
    batch.MeshStates = {{.Id = {a.Index, 1}, .Mesh = {{}, &mesh}}};
    batch.Materials = {{{a.Index, 1}, firstBindings}};
    scene.Apply(batch);
    EXPECT_TRUE(scene.GetDrawPrimitives(a).empty());
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives({a.Index, 1}).data());
    batch.Clear();
    for (uint32_t index = 2; index < 300; ++index) {
        const ShapeId extra{index, 0};
        batch.CreateShapes.push_back(extra);
        batch.MeshStates.push_back({.Id = extra, .Mesh = {{}, &mesh}});
        batch.Materials.push_back({extra, MaterialBindings{vector<MaterialBinding>{{{}, &first.GetRenderData(), &first.GetGpuState()}}}});
    }
    scene.Apply(batch);
    // Unrelated shared-node insertion/rehash must not invalidate a borrowed primitive template.
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives(b).data());
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives({a.Index, 1}).data());
    batch.Clear();
    for (uint32_t index = 2; index < 300; ++index) batch.RemoveShapes.push_back({index, 0});
    scene.Apply(batch);
    EXPECT_EQ(original.data(), scene.GetDrawPrimitives(b).data());
}
TEST(FrameResources, TextureBudgetEpochReuseAndLogicalValidity) {
    UploadTestDevice device;
    GpuFrameResources resources{&device};
    resources.SetTextureBudget({2, 1, 64 * 64 * 4 * 2});
    const render::TextureDescriptor desc{render::TextureDimension::Dim2D, 64, 64, 1, 1, 1,
                                         render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget};
    resources.BeginEpoch();
    auto a = resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget);
    auto b = resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget);
    ASSERT_TRUE(a && b);
    EXPECT_NE(a->Target, b->Target);
    EXPECT_FALSE(a->Initialized);
    EXPECT_TRUE(resources.IsCurrent(*a));
    EXPECT_FALSE(resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget));
    resources.SetTextureFinalState(*a, render::TextureState::CopySource);
    auto* physical = a->Target;
    resources.SealEpoch();
    EXPECT_FALSE(resources.IsCurrent(*a));
    EXPECT_FALSE(resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget));
    resources.CompleteEpoch();
    resources.BeginEpoch();
    auto next = resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->Target, physical);
    EXPECT_EQ(next->InitialState, render::TextureState::CopySource);
    EXPECT_FALSE(next->Initialized);
    EXPECT_FALSE(resources.IsCurrent(*a));
    auto incompatible = desc;
    incompatible.Width = 128;
    EXPECT_FALSE(resources.BorrowTexture(incompatible, render::TextureViewUsage::RenderTarget));
    incompatible = desc;
    incompatible.Hints = render::ResourceHint::External;
    EXPECT_FALSE(resources.BorrowTexture(incompatible, render::TextureViewUsage::RenderTarget));
    resources.SealEpoch();
    resources.CompleteEpoch();
    resources.SetTextureBudget({0, 0, 0});
    EXPECT_EQ(device.LiveTextures, 0);
    EXPECT_EQ(device.LiveTextureViews, 0);
}
TEST(FrameTimeline, IdentityWaitersAndBoundedCompletionAcrossFlightCounts) {
    for (uint32_t count : {1u, 2u, 3u, 4u}) {
        FrameTimeline timeline{count};
        vector<uint64_t> identities;
        for (uint32_t flight = 0; flight < count; ++flight) identities.push_back(timeline.OpenFrameDomain(flight));
        for (uint32_t flight = 0; flight < count; ++flight) {
            EXPECT_EQ(timeline.OpenFrameDomain(flight), identities[flight]);
            std::thread producer{[&timeline, flight, &identities] { timeline.PublishCompletion({flight, false, identities[flight]}); }};
            producer.join();
        }
        for (uint32_t flight = 0; flight < count; ++flight) {
            FlightCompletion event;
            ASSERT_TRUE(timeline.TryReadCompletion(event));
            EXPECT_EQ(event.FlightIndex, flight);
            EXPECT_EQ(event.FrameSerial, identities[flight]);
            EXPECT_TRUE(timeline.IsCompleted(flight, identities[flight]));
            EXPECT_FALSE(timeline.IsCompleted(flight, identities[flight] + 1));
        }
        FlightCompletion empty;
        EXPECT_FALSE(timeline.TryReadCompletion(empty));
        uint32_t resumed = 0;
        TaskScope scope;
        auto waitTwice = [&]() -> task<void> {
            co_await timeline.Wait();
            ++resumed;
            co_await timeline.Wait();
            ++resumed;
        };
        scope.Spawn(waitTwice());
        timeline.PumpWaitFrame(0);
        EXPECT_EQ(resumed, 0u);
        const auto first = timeline.GetFrameSerial(0);
        EXPECT_GT(first, identities.back());
        timeline.PublishCompletion({0, false, first});
        ASSERT_TRUE(timeline.TryReadCompletion(empty));
        timeline.PumpWaitFrame(0);
        EXPECT_EQ(resumed, 1u);
        timeline.PumpWaitFrame(0);
        EXPECT_EQ(resumed, 1u);
        const auto second = timeline.GetFrameSerial(0);
        EXPECT_GT(second, first);
        timeline.PublishCompletion({0, false, second});
        ASSERT_TRUE(timeline.TryReadCompletion(empty));
        timeline.CleanupCompletedFlights();
        EXPECT_EQ(resumed, 2u);
    }
}
TEST(FrameTimelineDeathTest, RejectsDuplicateForeignAndUnconsumedSlotCompletion) {
    FrameTimeline timeline{1};
    const auto first = timeline.OpenFrameDomain(0);
    EXPECT_DEATH(timeline.PublishCompletion({0, false, first + 1}), "");
    timeline.PublishCompletion({0, false, first});
    EXPECT_DEATH(timeline.PublishCompletion({0, false, first}), "");
    const auto next = timeline.OpenFrameDomain(0);
    EXPECT_DEATH(timeline.PublishCompletion({0, false, next}), "");
    FlightCompletion event;
    ASSERT_TRUE(timeline.TryReadCompletion(event));
    timeline.PublishCompletion({0, false, next});
    ASSERT_TRUE(timeline.TryReadCompletion(event));
    EXPECT_EQ(event.FrameSerial, next);
}
TEST(FrameTimeline, CancellationAndTerminalStopAreNotCompletionEvidence) {
    FrameTimeline timeline{1};
    TaskScope scope;
    bool resumed = false;
    auto waiting = [&]() -> task<void> { co_await timeline.Wait(); resumed = true; };
    scope.Spawn(waiting());
    scope.RequestStop();
    timeline.CleanupCompletedFlights();
    EXPECT_FALSE(resumed);
    timeline.BeginStopping();
    scope.Spawn(waiting());
    EXPECT_FALSE(resumed);
}
}  // namespace
}  // namespace radray::test
