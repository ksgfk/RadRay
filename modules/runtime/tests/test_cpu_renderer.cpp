#include <array>
#include <span>
#include <gtest/gtest.h>

#include <radray/runtime/render_framework/cpu_draw_record.h>
#include <radray/runtime/render_framework/culling.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>
#include <radray/runtime/render_framework/render_graph_compiler.h>
#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {
namespace {

class VersionedProxy final : public PrimitiveSceneProxy {
public:
    uint64_t RenderRevision{1};
    uint32_t GetSectionCount() const noexcept override { return 0; }
    uint64_t GetRenderDataRevision() const noexcept override { return RenderRevision; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
};

class RecordingProcessor final : public MeshPassProcessor {
public:
    uint32_t Calls{0};
    void AddMeshBatch(const RendererListDesc&, const RenderSceneSnapshot& scene, const MeshBatch& batch, MeshPassDrawListContext& out) override {
        ++Calls;
        MeshDrawCommand command;
        command.Program = scene.Materials[batch.Material].FindPass("ForwardLit")->Program;
        command.IndexCount = batch.IndexCount;
        out.AddCommand(std::move(command));
    }
};

TEST(CpuRenderer, SceneSlotGenerationRejectsStaleEvents) {
    Scene scene;
    auto first = make_unique<PrimitiveSceneProxy>();
    auto* proxy = scene.AddPrimitive(std::move(first)).Get();
    const auto id = scene.GetPrimitiveId(proxy);
    ASSERT_TRUE(id.IsValid());
    EXPECT_EQ(scene.FindPrimitive(id).Get(), proxy);
    scene.RemovePrimitive(proxy);
    EXPECT_FALSE(scene.FindPrimitive(id));
    auto* replacement = scene.AddPrimitive(make_unique<PrimitiveSceneProxy>()).Get();
    const auto next = scene.GetPrimitiveId(replacement);
    EXPECT_EQ(next.Slot, id.Slot);
    EXPECT_NE(next.Generation, id.Generation);
    EXPECT_FALSE(scene.FindPrimitive(id));
    EXPECT_EQ(scene.FindPrimitive(next).Get(), replacement);
}

TEST(CpuRenderer, SameFrameTransformWritesApplyOnceAtPublish) {
    Scene scene;
    auto* proxy = static_cast<VersionedProxy*>(scene.AddPrimitive(make_unique<VersionedProxy>()).Get());
    RenderSceneSnapshot snapshot;
    vector<StreamingAssetRefAny> retained;
    RenderSceneSnapshotBuilder builder;
    ASSERT_TRUE(builder.Build(scene, snapshot, retained));
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    for (int i = 0; i < 20; ++i) {
        matrix(0, 3) = static_cast<float>(i + 1);
        proxy->SetLocalToWorld(matrix);
    }
    ASSERT_TRUE(builder.Build(scene, snapshot, retained));
    EXPECT_EQ(snapshot.Stats.AppliedTransforms, 1u);
    EXPECT_FLOAT_EQ(snapshot.Primitives.front().LocalToWorld(0, 3), 20.0f);
}

TEST(CpuRenderer, EqualTransformDoesNotApply) {
    Scene scene;
    auto* proxy = static_cast<VersionedProxy*>(scene.AddPrimitive(make_unique<VersionedProxy>()).Get());
    RenderSceneSnapshot snapshot;
    vector<StreamingAssetRefAny> retained;
    RenderSceneSnapshotBuilder builder;
    ASSERT_TRUE(builder.Build(scene, snapshot, retained));
    const auto matrix = proxy->GetLocalToWorld();
    for (int i = 0; i < 8; ++i) proxy->SetLocalToWorld(matrix);
    ASSERT_TRUE(builder.Build(scene, snapshot, retained));
    EXPECT_EQ(snapshot.Stats.AppliedTransforms, 0u);
    EXPECT_EQ(snapshot.Stats.PrimitiveBoundsReused, 1u);
}

TEST(CpuRenderer, CameraOnlyDoesNotRebuildDrawRecords) {
    RenderSceneSnapshot scene;
    array<byte, 4> token{};
    MaterialRenderData material;
    MaterialPassRenderData pass;
    pass.PassName = "ForwardLit";
    pass.Program = reinterpret_cast<ShaderProgram*>(token.data());
    pass.Valid = true;
    material.Passes.push_back(std::move(pass));
    scene.Materials.push_back(std::move(material));
    scene.Primitives.push_back({.Generation = 7, .RenderDataRevision = 1, .TransformRevision = 1});
    scene.Primitives.back().MeshBatchCount = 1;
    scene.MeshBatches.push_back({0, 0, nullptr, 0, 3, 0, 0});
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().DrawRecordBuilds, 1u);
    EXPECT_EQ(scene.DrawRecords.size(), 1u);
    EXPECT_EQ(scene.DrawRecords.front().Status, DrawRecordStatus::InvalidGeometry);
    store.ResetCounters();
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().DrawRecordBuilds, 0u);
    EXPECT_EQ(store.GetStats().DrawRecordsReused, 1u);
}

TEST(CpuRenderer, DrawRecordsAreNotCopiedPerView) {
    RenderSceneSnapshot scene;
    array<byte, 4> token{};
    for (uint32_t i = 0; i < 3; ++i) {
        MaterialRenderData material;
        for (const char* name : {"ForwardLit", "DepthOnly"}) {
            MaterialPassRenderData pass;
            pass.PassName = name;
            pass.Program = reinterpret_cast<ShaderProgram*>(token.data());
            pass.Valid = true;
            material.Passes.push_back(std::move(pass));
        }
        scene.Materials.push_back(std::move(material));
        scene.Primitives.push_back({.Generation = i + 1, .RenderDataRevision = 1, .TransformRevision = 1});
        scene.Primitives.back().FirstMeshBatch = i;
        scene.Primitives.back().MeshBatchCount = 1;
        scene.MeshBatches.push_back({i, i, nullptr, 0, 3, 0, 0});
    }
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    const auto records = scene.DrawRecords.size();
    EXPECT_EQ(records, 6u);
    ResolvedRenderView view;
    CullingResults culling;
    culling.Scene = &scene;
    culling.View = &view;
    culling.Stats.Valid = true;
    for (uint32_t i = 0; i < 3; ++i) culling.Primitives.push_back({i, float(i)});
    RecordingProcessor processor;
    RendererList opaque, depth;
    ASSERT_TRUE(BuildRendererList({"opaque", "ForwardLit", &culling, &view, RenderQueueRange::Opaque()}, processor, opaque));
    ASSERT_TRUE(BuildRendererList({"depth", "DepthOnly", &culling, &view, RenderQueueRange::Opaque()}, processor, depth));
    EXPECT_EQ(scene.DrawRecords.size(), records);
    EXPECT_EQ(opaque.Stats.InvalidGeometry, 3u);
    EXPECT_EQ(depth.Stats.InvalidGeometry, 3u);
    EXPECT_TRUE(opaque.Commands.empty());
    EXPECT_EQ(processor.Calls, 0u);
}

TEST(CpuRenderer, UniqueSceneTablesDoNotScaleWithImaginaryViews) {
    RenderSceneSnapshot scene;
    scene.Primitives.resize(4);
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    const auto bytes = scene.Stats.CpuSceneBytes;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(scene.Stats.CpuSceneBytes, bytes);
    EXPECT_EQ(scene.PrimitiveDrawBegin.size(), scene.Primitives.size() + 1);
}

TEST(CpuRenderer, MirroredAffineFlipsFrontFace) {
    Eigen::Matrix4f identity = Eigen::Matrix4f::Identity();
    EXPECT_FALSE(IsMirroredAffine(identity));
    identity(0, 0) = -1;
    EXPECT_TRUE(IsMirroredAffine(identity));
    EXPECT_EQ(OppositeFrontFace(render::FrontFace::CCW), render::FrontFace::CW);
}

TEST(RenderGraphCompilerTest, CompileInputHashMatchesEqualStructureAndSeparatesDifferentGraphs) {
    const uint32_t none = RgInvalidIndex;
    const vector<RgResourceVersionNode> versions{{0, 0, 0, none, none, false}, {0, 0, 1, 0, 0, true}};
    const vector<RgExecutionNode> passes{{{}, {1}, true}};
    const uint32_t root = 1;
    const RenderGraphCompileOptions options{};
    const auto hash = HashRenderGraphCompileInput(1, versions, passes, std::span{&root, 1}, options);
    EXPECT_EQ(hash, HashRenderGraphCompileInput(1, versions, passes, std::span{&root, 1}, options));
    EXPECT_TRUE(EqualRenderGraphCompileInput(1, versions, passes, std::span{&root, 1}, options,
                                             1, versions, passes, std::span{&root, 1}, options));
    const vector<RgExecutionNode> other{{{1}, {}, true}};
    EXPECT_NE(hash, HashRenderGraphCompileInput(1, versions, other, std::span{&root, 1}, options));
    EXPECT_FALSE(EqualRenderGraphCompileInput(1, versions, passes, std::span{&root, 1}, options,
                                              1, versions, other, std::span{&root, 1}, options));
    const auto collided = HashRenderGraphCompileInput(1, versions, other, std::span{&root, 1}, options);
    EXPECT_FALSE(EqualRenderGraphCompileInput(1, versions, passes, std::span{&root, 1}, options,
                                               1, versions, other, std::span{&root, 1}, options));
    EXPECT_NE(collided, 0u);
}

}  // namespace
}  // namespace radray
