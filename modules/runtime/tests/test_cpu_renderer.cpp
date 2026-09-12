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

bool CompilePolicyWrite(const StaticPassCompileInput& input, StaticPassCompileResult& result) {
    result.Bindings = input.Bindings ? *input.Bindings : StaticBindingRecipe{};
    result.Bindings.Valid = true;
    result.Bindings.Groups[0] = 7;
    result.NormalState = input.Pass.PipelineState;
    result.NormalState.DepthStencil.DepthWriteEnable = true;
    result.MirroredState = result.NormalState;
    result.MirroredState.Primitive.FaceClockwise = OppositeFrontFace(result.NormalState.Primitive.FaceClockwise);
    return true;
}

bool CompilePolicyRead(const StaticPassCompileInput& input, StaticPassCompileResult& result) {
    CompilePolicyWrite(input, result);
    result.NormalState.DepthStencil.DepthWriteEnable = result.MirroredState.DepthStencil.DepthWriteEnable = false;
    return true;
}

void FillPolicyScene(RenderSceneSnapshot& scene, const GpuMesh::DrawData& geometry, ShaderProgram* program, uint32_t count) {
    MaterialRenderData material;
    material.Generation = material.Revision = material.StructureRevision = material.ValuesRevision = 1;
    MaterialPassRenderData pass;
    pass.PassName = "ForwardLit";
    pass.Program = program;
    pass.ProgramGeneration = 9;
    pass.Valid = true;
    material.Passes.push_back(std::move(pass));
    scene.Materials.push_back(std::move(material));
    for (uint32_t index = 0; index < count; ++index) {
        scene.Primitives.push_back({.FirstMeshBatch = index, .MeshBatchCount = 1, .Generation = index + 1ull, .RenderDataRevision = 1, .TransformRevision = 1});
        scene.MeshBatches.push_back({index, 0, &geometry, 0, 3, 0, 0});
    }
}

TEST(CpuRenderer, SameSizeRendererListAssignmentAndMovesInvalidatePreparedRevisions) {
    RendererList first, other;
    MeshDrawCommand a, b;
    a.IndexCount = 3;
    b.IndexCount = 9;
    ASSERT_TRUE(first.AppendDynamic(std::move(a)));
    ASSERT_TRUE(other.AppendDynamic(std::move(b)));
    first.Items.push_back({{}, 0});
    other.Items.push_back({{}, 0});
    const auto* commands = first.Commands.data();
    const auto* items = first.Items.data();
    const auto revision = first.GetBuildRevision();
    ASSERT_EQ(revision, other.GetBuildRevision());
    first = other;
    EXPECT_EQ(first.Commands.data(), commands);
    EXPECT_EQ(first.Items.data(), items);
    EXPECT_EQ(first.GetDrawCount(), 1u);
    EXPECT_EQ(first.GetDescription(0).IndexCount, 9u);
    EXPECT_GT(first.GetBuildRevision(), revision);
    const auto sourceRevision = other.GetBuildRevision();
    RendererList moved{std::move(other)};
    EXPECT_EQ(moved.GetDrawCount(), 1u);
    EXPECT_EQ(other.GetDrawCount(), 0u);
    EXPECT_GT(other.GetBuildRevision(), sourceRevision);
    const auto destinationRevision = first.GetBuildRevision();
    const auto movedRevision = moved.GetBuildRevision();
    first = std::move(moved);
    EXPECT_GT(first.GetBuildRevision(), destinationRevision);
    EXPECT_GT(moved.GetBuildRevision(), movedRevision);
    EXPECT_EQ(moved.GetDrawCount(), 0u);
    const auto appendedRevision = first.GetBuildRevision();
    ASSERT_TRUE(first.AppendDynamic(MeshDrawCommand{}));
    EXPECT_GT(first.GetBuildRevision(), appendedRevision);
}

TEST(CpuRenderer, EffectiveStatesAndGeometryPlansShareAndReleaseWithoutInvalidatingOldSnapshots) {
    RenderSceneSnapshot scene;
    array<GpuMesh::DrawData, 2> geometry;
    geometry[0].VertexBuffers = {{2, {}}, {3, {}}, {7, {}}};
    geometry[1].VertexBuffers = {{1, {}}, {5, {}}, {6, {}}};
    byte token{};
    FillPolicyScene(scene, geometry[0], reinterpret_cast<ShaderProgram*>(&token), 16);
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(scene.GeometryBindingPlans.size(), 1u);
    EXPECT_EQ(store.GetActiveGeometryPlanCount(), 1u);
    EXPECT_EQ(store.GetActiveStateCount(), 2u);
    const auto original = scene.DrawRecords[0];
    const auto frozen = scene;
    const auto& runs = frozen.GeometryBindingPlans[original.GeometryBindingPlan].Runs;
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].First, 0u);
    EXPECT_EQ(runs[0].Count, 2u);
    EXPECT_EQ(runs[1].First, 2u);
    EXPECT_EQ(runs[1].Count, 1u);
    for (const auto& record : scene.DrawRecords) {
        EXPECT_EQ(record.NormalStateId, original.NormalStateId);
        EXPECT_EQ(record.MirroredStateId, original.MirroredStateId);
        EXPECT_EQ(record.GeometryBindingPlan, original.GeometryBindingPlan);
    }
    for (uint32_t change = 1; change <= 64; ++change) {
        scene.Materials[0].Passes[0].PipelineState.DepthStencil.DepthBias.Constant = static_cast<int32_t>(change);
        ++scene.Materials[0].StructureRevision;
        for (uint32_t index = 0; index < scene.Primitives.size(); ++index) {
            ++scene.Primitives[index].RenderDataRevision;
            scene.MeshBatches[index].Geometry = &geometry[change % 2];
        }
        ASSERT_TRUE(store.Sync(scene));
        EXPECT_EQ(store.GetActiveStateCount(), 2u);
        EXPECT_EQ(store.GetActiveGeometryPlanCount(), 1u);
        EXPECT_LE(scene.GeometryBindingPlans.size(), 2u);
        EXPECT_NE(scene.DrawRecords[0].NormalStateId, original.NormalStateId);
    }
    EXPECT_EQ(frozen.DrawRecords[0].NormalStateId, original.NormalStateId);
    EXPECT_EQ(runs[0].Count, 2u);
    EXPECT_EQ(runs[1].First, 2u);
    scene.Primitives.clear();
    scene.MeshBatches.clear();
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetActiveStateCount(), 0u);
    EXPECT_EQ(store.GetActiveGeometryPlanCount(), 0u);
}

TEST(CpuRenderer, TenThousandLayoutReplacementsRetainOnlyActiveRecipesAndPreserveOldIds) {
    RenderSceneSnapshot scene;
    GpuMesh::DrawData originalGeometry;
    originalGeometry.VertexBuffers = {{0, {}}};
    originalGeometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    originalGeometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    byte token{};
    FillPolicyScene(scene, originalGeometry, reinterpret_cast<ShaderProgram*>(&token), 16);
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(store.GetActiveLayoutCount(), 1u);
    const auto frozen = scene;
    const auto originalId = frozen.DrawRecords[0].Description.LayoutId;
    auto previous = originalId;
    array<GpuMesh::DrawData, 2> changingGeometry;
    for (uint32_t index = 0; index < 10000; ++index) {
        auto& geometry = changingGeometry[index % changingGeometry.size()];
        geometry = originalGeometry;
        geometry.VertexLayout.Buffers[0].ArrayStride = 16 + index;
        for (uint32_t primitive = 0; primitive < scene.Primitives.size(); ++primitive) {
            scene.MeshBatches[primitive].Geometry = &geometry;
            ++scene.Primitives[primitive].RenderDataRevision;
        }
        ASSERT_TRUE(store.Sync(scene));
        ASSERT_EQ(store.GetActiveLayoutCount(), 1u);
        const auto current = scene.DrawRecords[0].Description.LayoutId;
        ASSERT_GT(current.Value, previous.Value);
        for (const auto& record : scene.DrawRecords) ASSERT_EQ(record.Description.LayoutId, current);
        previous = current;
    }
    EXPECT_EQ(frozen.DrawRecords[0].Description.LayoutId, originalId);
    EXPECT_EQ(frozen.DrawRecords[0].Description.Geometry.Get(), &originalGeometry);
    EXPECT_EQ(originalGeometry.VertexLayout.Buffers[0].ArrayStride, 12u);
    scene.Primitives.clear();
    scene.MeshBatches.clear();
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetActiveLayoutCount(), 0u);
}

TEST(CpuRenderer, PoliciesShareBindingsAndPolicyOnlyChangesPublishAffectedRecipes) {
    RenderSceneSnapshot scene;
    GpuMesh::DrawData geometry;
    byte token{};
    constexpr uint32_t count = 1000;
    FillPolicyScene(scene, geometry, reinterpret_cast<ShaderProgram*>(&token), count);
    array<PassPolicy, 2> policies{{{{100}, 1, "ForwardLit", CompilePolicyWrite}, {{101}, 1, "ForwardLit", CompilePolicyRead}}};
    CpuDrawStore store;
    ASSERT_TRUE(store.SetActivePolicies(0, policies));
    ASSERT_TRUE(store.SetActivePolicies(0, policies));
    ASSERT_TRUE(store.SyncChanged(scene, {}, false));
    ASSERT_EQ(scene.DrawRecords.size(), count * 2);
    EXPECT_EQ(scene.Stats.StaticRecipeCompiles, count * 2);
    EXPECT_EQ(scene.Stats.BindingRecipeCompiles, 2u);
    EXPECT_EQ(scene.BindingRecipes.size(), 2u);
    const auto before = scene.DrawRecords[0];
    const auto independent = scene.DrawRecords[1];
    EXPECT_NE(before.BindingRecipe, independent.BindingRecipe);
    EXPECT_TRUE(before.Description.PipelineState.DepthStencil.DepthWriteEnable);
    EXPECT_FALSE(independent.Description.PipelineState.DepthStencil.DepthWriteEnable);
    for (uint32_t index = 0; index < count; ++index) {
        EXPECT_EQ(scene.DrawRecords[index * 2].BindingRecipe, before.BindingRecipe);
        EXPECT_EQ(scene.DrawRecords[index * 2 + 1].BindingRecipe, independent.BindingRecipe);
    }
    for (uint64_t epoch = 1; epoch < 1001; ++epoch) {
        ASSERT_TRUE(store.SetActivePolicies(epoch, policies));
        ASSERT_TRUE(store.SyncChanged(scene, {}, false));
        EXPECT_EQ(scene.Stats.StaticRecipeCompiles, 0u);
        EXPECT_EQ(scene.Stats.BindingRecipeCompiles, 0u);
        EXPECT_EQ(scene.Stats.DrawRecordPrimitivesVisited, 0u);
        EXPECT_TRUE(store.ChangedDrawRanges().empty());
        EXPECT_TRUE(store.ChangedBindingRanges().empty());
    }
    scene.Primitives[0].LocalToWorld(0, 0) = -1;
    ++scene.Primitives[0].TransformRevision;
    const uint32_t changedPrimitive = 0;
    ASSERT_TRUE(store.SyncChanged(scene, std::span{&changedPrimitive, 1}, false));
    EXPECT_EQ(scene.Stats.StaticRecipeCompiles, 0u);
    EXPECT_EQ(scene.Stats.BindingRecipeCompiles, 0u);
    EXPECT_EQ(scene.Stats.DrawRecordStateSelects, 2u);
    EXPECT_TRUE(scene.DrawRecords[0].Mirrored);
    EXPECT_EQ(scene.DrawRecords[0].MirroredState.Primitive.FaceClockwise,
              OppositeFrontFace(scene.DrawRecords[0].Description.PipelineState.Primitive.FaceClockwise));
    ++scene.Materials[0].Revision;
    ++scene.Materials[0].ValuesRevision;
    scene.Materials[0].Passes[0].NumericBytes.push_back(byte{8});
    ASSERT_TRUE(store.SyncChanged(scene, {}, false));
    EXPECT_EQ(scene.Stats.StaticRecipeCompiles, 0u);
    policies[0].Revision = 2;
    policies[0].CompileStatic = CompilePolicyRead;
    EXPECT_FALSE(store.SetActivePolicies(1000, policies));
    ASSERT_TRUE(store.SetActivePolicies(1001, policies));
    ASSERT_TRUE(store.SyncChanged(scene, {}, false));
    EXPECT_EQ(scene.Stats.StaticRecipeCompiles, count);
    EXPECT_EQ(scene.Stats.BindingRecipeCompiles, 1u);
    EXPECT_EQ(scene.Stats.DrawRecordFullSyncs, 0u);
    EXPECT_EQ(store.ChangedDrawRanges().size(), count);
    EXPECT_FALSE(store.ChangedBindingRanges().empty());
    EXPECT_EQ(scene.DrawRecords[0].Id, before.Id);
    EXPECT_GT(scene.DrawRecords[0].RecipeRevision, before.RecipeRevision);
    EXPECT_FALSE(scene.DrawRecords[0].Description.PipelineState.DepthStencil.DepthWriteEnable);
    EXPECT_EQ(scene.DrawRecords[1].Id, independent.Id);
    EXPECT_EQ(scene.DrawRecords[1].RecipeRevision, independent.RecipeRevision);
    EXPECT_EQ(scene.DrawRecords[1].BindingRecipe, independent.BindingRecipe);
    ASSERT_TRUE(store.SetActivePolicies(1002, std::span{&policies[1], 1}));
    ASSERT_TRUE(store.SyncChanged(scene, {}, false));
    EXPECT_EQ(scene.DrawRecords.size(), count);
    EXPECT_EQ(scene.DrawRecords[0].Id, independent.Id);
    EXPECT_EQ(scene.Stats.StaticRecipeCompiles, 0u);
    for (uint64_t epoch = 1003; epoch < 1035; ++epoch) {
        const PassPolicy replacement{{epoch}, 1, "ForwardLit", CompilePolicyWrite};
        ASSERT_TRUE(store.SetActivePolicies(epoch, std::span{&replacement, 1}));
        ASSERT_TRUE(store.SyncChanged(scene, {}, false));
        EXPECT_LE(scene.BindingRecipes.size(), 3u);
        EXPECT_EQ(scene.Stats.BindingRecipeCompiles, 1u);
    }
}

TEST(CpuRenderer, RendererRecordsSelectFullPassNamesAndDistinctPolicies) {
    RenderSceneSnapshot scene;
    GpuMesh::DrawData geometry;
    byte token{};
    FillPolicyScene(scene, geometry, reinterpret_cast<ShaderProgram*>(&token), 1);
    auto collision = scene.Materials[0].Passes[0];
    collision.PassName = "OtherPass";
    scene.Materials[0].Passes.push_back(std::move(collision));
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(scene.DrawRecords.size(), 2u);
    scene.DrawRecords[1].PassNameHash = scene.DrawRecords[0].PassNameHash;
    ResolvedRenderView view;
    CullingResults culling;
    culling.Scene = &scene;
    culling.View = &view;
    culling.Stats.Valid = true;
    culling.Primitives.push_back({0, 0});
    RecordingProcessor processor;
    RendererList output;
    RendererListDesc desc{"test", "ForwardLit", &culling, &view};
    ASSERT_TRUE(BuildRendererList(desc, processor, output));
    EXPECT_EQ(output.Commands.size(), 1u);
    desc.Policy = {100};
    ASSERT_TRUE(BuildRendererList(desc, processor, output));
    EXPECT_EQ(output.Commands.size(), 1u);
    const array<PassPolicy, 2> policies{{{{100}, 1, "ForwardLit", CompilePolicyWrite}, {{101}, 1, "ForwardLit", CompilePolicyRead}}};
    ASSERT_TRUE(store.SetActivePolicies(0, policies));
    ASSERT_TRUE(store.SyncChanged(scene, {}, false));
    desc.Policy = {100};
    ASSERT_TRUE(BuildRendererList(desc, processor, output));
    EXPECT_EQ(output.Commands.size(), 1u);
    desc.Policy = {101};
    ASSERT_TRUE(BuildRendererList(desc, processor, output));
    EXPECT_EQ(output.Commands.size(), 1u);
    desc.Policy = {102};
    ASSERT_TRUE(BuildRendererList(desc, processor, output));
    EXPECT_TRUE(output.Commands.empty());
    EXPECT_EQ(output.Stats.MissingPass, 1u);
}

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

TEST(CpuRenderer, T37SharedNumericChangeAndT43FrameRenumberingKeepStaticRecipes) {
    RenderSceneSnapshot scene;
    array<byte, 4> token{};
    GpuMesh::DrawData geometry;
    MaterialRenderData material;
    material.Generation = 17;
    material.Revision = material.StructureRevision = material.ValuesRevision = 1;
    MaterialPassRenderData pass;
    pass.PassName = "ForwardLit";
    pass.Program = reinterpret_cast<ShaderProgram*>(token.data());
    pass.ProgramGeneration = 13;
    pass.Valid = true;
    material.Passes.push_back(std::move(pass));
    scene.Materials.push_back(std::move(material));
    constexpr uint32_t count = 10000;
    for (uint32_t index = 0; index < count; ++index) {
        scene.Primitives.push_back({.FirstMeshBatch = index, .MeshBatchCount = 1, .Generation = index + 1ull, .RenderDataRevision = 1, .TransformRevision = 1});
        scene.MeshBatches.push_back({index, 0, &geometry, 0, 3, 0, 0});
    }
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(scene.DrawRecords.size(), count);
    const auto identity = scene.DrawRecords.back();
    ++scene.Materials[0].Revision;
    ++scene.Materials[0].ValuesRevision;
    scene.Materials[0].Passes[0].NumericBytes.push_back(byte{42});
    scene.Materials[0].Passes[0].ProgramFrameId = 9;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().DrawRecordBuilds, 0u);
    EXPECT_EQ(scene.DrawRecords.back().Id, identity.Id);
    EXPECT_EQ(scene.DrawRecords.back().Generation, identity.Generation);
    EXPECT_EQ(scene.DrawRecords.back().RecipeRevision, identity.RecipeRevision);
    EXPECT_EQ(scene.DrawRecords.back().ProgramFrameId, 9u);
    EXPECT_EQ(scene.DrawRecords.front().Description.LayoutId, scene.DrawRecords.back().Description.LayoutId);
}

TEST(CpuRenderer, PassOrderUsesFullNamesAndProgramReplacementChangesOnlyRecipeVersion) {
    RenderSceneSnapshot scene;
    array<byte, 4> token{};
    MaterialRenderData material;
    for (const auto* name : {"ForwardLit", "DepthOnly"}) {
        MaterialPassRenderData pass;
        pass.PassName = name;
        pass.Program = reinterpret_cast<ShaderProgram*>(token.data());
        pass.ProgramGeneration = 1;
        pass.Valid = true;
        material.Passes.push_back(std::move(pass));
    }
    scene.Materials.push_back(std::move(material));
    scene.Primitives.push_back({.MeshBatchCount = 1, .Generation = 1, .RenderDataRevision = 1});
    scene.MeshBatches.push_back({0, 0, nullptr, 0, 3, 0, 0});
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    const auto lit = scene.DrawRecords[0], depth = scene.DrawRecords[1];
    std::swap(scene.Materials[0].Passes[0], scene.Materials[0].Passes[1]);
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().DrawRecordBuilds, 0u);
    EXPECT_EQ(scene.DrawRecords[0].Id, depth.Id);
    EXPECT_EQ(scene.DrawRecords[1].Id, lit.Id);
    ++scene.Materials[0].Passes[1].ProgramGeneration;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().DrawRecordBuilds, 1u);
    EXPECT_EQ(scene.DrawRecords[1].Id, lit.Id);
    EXPECT_EQ(scene.DrawRecords[1].Generation, lit.Generation);
    EXPECT_GT(scene.DrawRecords[1].RecipeRevision, lit.RecipeRevision);
    EXPECT_EQ(scene.DrawRecords[0].RecipeRevision, depth.RecipeRevision);
}

TEST(CpuRenderer, GeometryRevisionRefreshesLayoutWhileTransformSelectsMirrorOnce) {
    RenderSceneSnapshot scene;
    array<byte, 4> token{};
    GpuMesh::DrawData geometry;
    MaterialRenderData material;
    MaterialPassRenderData pass;
    pass.PassName = "ForwardLit";
    pass.Program = reinterpret_cast<ShaderProgram*>(token.data());
    pass.Valid = true;
    material.Passes.push_back(std::move(pass));
    scene.Materials.push_back(std::move(material));
    scene.Primitives.push_back({.MeshBatchCount = 1, .Generation = 1, .RenderDataRevision = 1, .TransformRevision = 1});
    scene.MeshBatches.push_back({0, 0, &geometry, 0, 3, 0, 0});
    CpuDrawStore store;
    ASSERT_TRUE(store.Sync(scene));
    const auto layout = scene.DrawRecords[0].Description.LayoutId;
    scene.Primitives[0].LocalToWorld(0, 0) = -1;
    ++scene.Primitives[0].TransformRevision;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().DrawRecordBuilds, 0u);
    EXPECT_EQ(store.GetStats().DrawRecordStateSelects, 1u);
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().DrawRecordStateSelects, 0u);
    geometry.VertexLayout.Attributes.push_back({"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3});
    ++scene.Primitives[0].RenderDataRevision;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().DrawRecordBuilds, 1u);
    EXPECT_NE(scene.DrawRecords[0].Description.LayoutId, layout);
}

TEST(CpuRenderer, IncrementalCatalogTouchesOnlyChangedPrimitiveAndReflowsCountChanges) {
    RenderSceneSnapshot scene;
    array<byte, 4> token{};
    MaterialRenderData material;
    MaterialPassRenderData pass;
    pass.PassName = "ForwardLit";
    pass.Program = reinterpret_cast<ShaderProgram*>(token.data());
    pass.Valid = true;
    material.Passes.push_back(std::move(pass));
    scene.Materials.push_back(std::move(material));
    for (uint32_t index = 0; index < 1000; ++index) {
        scene.Primitives.push_back({.FirstMeshBatch = index, .MeshBatchCount = 1, .Generation = index + 1ull, .RenderDataRevision = 1, .TransformRevision = 1});
        scene.MeshBatches.push_back({index, 0, nullptr, 0, 3, 0, 0});
    }
    CpuDrawStore store;
    ASSERT_TRUE(store.SyncChanged(scene, {}, true));
    const auto old = scene.DrawRecords;
    for (uint32_t frame = 0; frame < 1000; ++frame) {
        ASSERT_TRUE(store.SyncChanged(scene, {}, false));
        EXPECT_EQ(store.GetStats().DrawRecordFullSyncs, 0u);
        EXPECT_EQ(store.GetStats().DrawRecordPrimitivesVisited, 0u);
        EXPECT_EQ(store.GetStats().DrawRecordCopies, 0u);
    }
    constexpr uint32_t changed = 421;
    scene.Primitives[changed].LocalToWorld(0, 0) = -1;
    ++scene.Primitives[changed].TransformRevision;
    ASSERT_TRUE(store.SyncChanged(scene, std::span{&changed, 1}, false));
    EXPECT_EQ(store.GetStats().DrawRecordPrimitivesVisited, 1u);
    EXPECT_EQ(store.GetStats().DrawRecordCopies, 1u);
    EXPECT_EQ(store.GetStats().DrawRecordBuilds, 0u);
    EXPECT_TRUE(scene.DrawRecords[changed].Mirrored);
    EXPECT_FALSE(old[changed].Mirrored);
    EXPECT_EQ(scene.DrawRecords[changed + 1].Id, old[changed + 1].Id);

    // A pass count change changes the dense ranges for every subsequent primitive.
    scene.Materials[0].Passes.push_back(scene.Materials[0].Passes[0]);
    scene.Materials[0].Passes[1].PassName = "DepthOnly";
    ASSERT_TRUE(store.SyncChanged(scene, std::span{&changed, 1}, false));
    EXPECT_EQ(store.GetStats().DrawRecordFullSyncs, 1u);
    ASSERT_EQ(scene.DrawRecords.size(), 2000u);
    for (uint32_t index = 0; index < 1000; ++index) {
        EXPECT_EQ(scene.PrimitiveDrawBegin[index], index * 2);
        EXPECT_EQ(scene.DrawRecords[index * 2].Id, old[index].Id);
    }
    EXPECT_EQ(scene.PrimitiveDrawBegin.back(), 2000u);
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
