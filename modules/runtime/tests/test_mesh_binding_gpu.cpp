#include "foundation_graph_fixture.h"
#include <radray/runtime/render_framework/mesh_binding_contract.h>
#include <radray/scope_guard.h>
#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>

namespace radray {
namespace {
class MeshBindingGpuTest : public test::FoundationGraphGpuTest {};
constexpr std::string_view kProgram = R"hlsl(
#include <core/platform.hlsli>
struct ScalarData { float4 Value; };
VK_BINDING(2, 5) ConstantBuffer<ScalarData> LensGain : register(b2, space5);
VK_BINDING(5, 5) ConstantBuffer<ScalarData> LensBias : register(b5, space5);
VK_BINDING(4, 3) Texture2D<float> Samples[2] : register(t4, space3);
VK_BINDING(6, 3) SamplerState SampleFilter : register(s6, space3);
VK_BINDING(7, 3) StructuredBuffer<float> Coefficients : register(t7, space3);
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
[shader("pixel")] float PSMain() : SV_Target0 {
    return (Samples[0].SampleLevel(SampleFilter, float2(.5, .5), 0) +
            Samples[1].SampleLevel(SampleFilter, float2(.5, .5), 0)) * LensGain.Value.x +
           LensBias.Value.x + Coefficients[0];
}
)hlsl";
bool ResolveLens(const ShaderProgram& program, MeshBindingPlan& plan) {
    plan.Slots = {{70, MeshParameterScope::View, MeshParameterKind::CBuffer, 991, 16},
                  {71, MeshParameterScope::View, MeshParameterKind::CBuffer, 992, 16},
                  {80, MeshParameterScope::Material, MeshParameterKind::GraphTexture},
                  {90, MeshParameterScope::Pass, MeshParameterKind::Sampler},
                  {91, MeshParameterScope::Pass, MeshParameterKind::GraphBuffer}};
    const auto buffers = program.GetParameterLayout().Buffers();
    const auto& artifact = program.GetArtifact().Generic();
    for (const auto& binding : artifact.Bindings()) {
        const auto name = artifact.GetName(binding.Name);
        if (!name) return false;
        uint32_t slot = *name == "LensGain" ? 0 : *name == "LensBias"   ? 1
                                              : *name == "Samples"      ? 2
                                              : *name == "SampleFilter" ? 3
                                              : *name == "Coefficients" ? 4
                                                                        : UINT32_MAX;
        if (slot == UINT32_MAX) return false;
        uint32_t bufferIndex = UINT32_MAX;
        for (uint32_t index = 0; index < buffers.size(); ++index)
            if (buffers[index].Name == *name) bufferIndex = index;
        for (uint32_t element = 0; element < binding.Count; ++element)
            plan.Bindings.push_back({slot, binding.Group, binding.Binding, element, bufferIndex, element});
    }
    return true;
}

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
    pass.PassName = "CustomSurface";
    pass.Program = program;
    pass.ProgramGeneration = program->GetGeneration();
    pass.Valid = true;
    material.Passes.push_back(std::move(pass));
    scene.Materials.push_back(std::move(material));
    for (uint32_t index = 0; index < count; ++index) {
        scene.Primitives.push_back({.FirstMeshBatch = index, .MeshBatchCount = 1, .Generation = index + 1ull, .RenderDataRevision = 1, .TransformRevision = 1});
        scene.MeshBatches.push_back({index, 0, &geometry, 0, 3, 0, 0});
    }
}

uint32_t gContractResolutions{0};
bool ResolveSharedContract(const ShaderProgram& program, MeshBindingPlan& plan) {
    ++gContractResolutions;
    return ResolveLens(program, plan);
}

TEST_P(MeshBindingGpuTest, ProgramContractsShareAcrossGeometryLayoutsAndStatePolicies) {
    RenderSceneSnapshot scene;
    array<GpuMesh::DrawData, 2> geometry;
    geometry[1].VertexLayout.Buffers = {{0, 24, render::VertexStepMode::Vertex}};
    auto program = test::CompileFoundationGraphics(*Context.Device, kProgram);
    ASSERT_TRUE(program);
    FillPolicyScene(scene, geometry[0], program.Get(), 2);
    scene.Materials[0].Passes[0].PassName = "CustomSurface";
    scene.MeshBatches[1].Geometry = &geometry[1];
    const MeshBindingContract contract{93, 1, 0, ResolveSharedContract};
    const array<PassPolicy, 2> policies{{{{31}, 1, "CustomSurface", CompilePolicyWrite, contract},
                                         {{32}, 1, "CustomSurface", CompilePolicyRead, contract}}};
    CpuDrawStore store;
    gContractResolutions = 0;
    ASSERT_TRUE(store.SetActivePolicies(1, policies));
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(scene.DrawRecords.size(), 4u);
    EXPECT_EQ(gContractResolutions, 1u);
    EXPECT_EQ(store.GetStats().BindingRecipeCompiles, 1u);
    const auto first = scene.DrawRecords[0].BindingRecipe;
    for (const auto& record : scene.DrawRecords) EXPECT_EQ(record.BindingRecipe, first);
    ASSERT_TRUE(scene.BindingRecipes[first].Parameters.Valid);
    EXPECT_EQ(scene.BindingRecipes[first].Parameters.Groups, (vector<uint32_t>{3, 5}));
    EXPECT_NE(scene.DrawRecords[0].NormalStateId, scene.DrawRecords[1].NormalStateId);
    const auto frozen = scene;
    ++scene.Materials[0].ValuesRevision;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 0u);
    EXPECT_EQ(gContractResolutions, 1u);
    EXPECT_EQ(frozen.BindingRecipes[first].Parameters.Groups, scene.BindingRecipes[first].Parameters.Groups);
}

TEST_P(MeshBindingGpuTest, DrawRejectionCannotInvalidateSharedProgramContract) {
    RenderSceneSnapshot scene;
    GpuMesh::DrawData geometry;
    auto program = test::CompileFoundationGraphics(*Context.Device, kProgram);
    ASSERT_TRUE(program);
    FillPolicyScene(scene, geometry, program.Get(), 2);
    const MeshBindingContract contract{94, 1, 0, ResolveSharedContract};
    const auto reject = +[](const StaticPassCompileInput&, StaticPassCompileResult&) { return false; };
    const array<PassPolicy, 2> policies{{{{31}, 1, "CustomSurface", reject, contract},
                                         {{32}, 1, "CustomSurface", CompilePolicyWrite, contract}}};
    CpuDrawStore store;
    gContractResolutions = 0;
    ASSERT_TRUE(store.SetActivePolicies(1, policies));
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(scene.DrawRecords.size(), 4u);
    EXPECT_EQ(gContractResolutions, 1u);
    for (uint32_t index = 0; index < 4; ++index) {
        EXPECT_EQ(scene.DrawRecords[index].Status, index % 2 == 0 ? DrawRecordStatus::InvalidBindings : DrawRecordStatus::Ready);
        EXPECT_TRUE(scene.BindingRecipes[scene.DrawRecords[index].BindingRecipe].Valid);
    }
}

TEST_P(MeshBindingGpuTest, DeclaredValuesAndEpochPoliciesInvalidateOnlyTheirUsers) {
    RenderSceneSnapshot scene;
    GpuMesh::DrawData geometry;
    auto program = test::CompileFoundationGraphics(*Context.Device, kProgram);
    ASSERT_TRUE(program);
    FillPolicyScene(scene, geometry, program.Get(), 2);
    const MeshBindingContract contract{95, 1, 0, ResolveSharedContract};
    array<PassPolicy, 3> policies{{{{31}, 1, "CustomSurface", CompilePolicyWrite, contract},
                                   {{32}, 1, "CustomSurface", CompilePolicyRead, contract},
                                   {{33}, 1, "CustomSurface", CompilePolicyWrite, contract}}};
    policies[1].Dependencies = MeshPassDependency::MaterialValues;
    policies[2].CacheMode = MeshPassCacheMode::PerEpoch;
    CpuDrawStore store;
    gContractResolutions = 0;
    ASSERT_TRUE(store.SetActivePolicies(1, policies));
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 6u);
    ASSERT_TRUE(store.SyncChanged(scene, {}, false));
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 2u);
    ++scene.Materials[0].ValuesRevision;
    const array<uint32_t, 2> changed{0, 1};
    ASSERT_TRUE(store.SyncChanged(scene, changed, false));
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 4u);
    EXPECT_EQ(gContractResolutions, 1u);
}

TEST_P(MeshBindingGpuTest, ConfigurationIsExplicitAndDeclaredNumericValuesChangeTheCompiledState) {
    auto program = test::CompileFoundationGraphics(*Context.Device, kProgram);
    ASSERT_TRUE(program);
    MeshBindingContract contract{97, 1, 1, nullptr};
    contract.ResolveConfigured = +[](const ShaderProgram& program, uint64_t configuration, MeshBindingPlan& plan) {
        if (!ResolveLens(program, plan)) return false;
        plan.Slots[0].WireLayout += configuration;
        return true;
    };
    const auto compile = +[](const StaticPassCompileInput& input, StaticPassCompileResult& result) {
        result.Bindings.Valid = true;
        result.NormalState = input.Pass.PipelineState;
        result.NormalState.DepthStencil.DepthWriteEnable = input.Configuration == 1 &&
                                                           !input.Pass.NumericBytes.empty() && input.Pass.NumericBytes[0] == byte{1};
        result.MirroredState = result.NormalState;
        return true;
    };
    array<PassPolicy, 3> policies{{{{1}, 1, "CustomSurface", compile, contract},
                                   {{2}, 1, "CustomSurface", CompilePolicyRead, contract},
                                   {{3}, 1, "CustomSurface", CompilePolicyWrite, contract}}};
    policies[0].Configuration = 1;
    policies[0].Dependencies = MeshPassDependency::MaterialValues;
    policies[2].BindingContract.Configuration = 2;
    RenderSceneSnapshot scene;
    GpuMesh::DrawData geometry;
    FillPolicyScene(scene, geometry, program.Get(), 2);
    scene.Materials[0].Passes[0].NumericBytes = {byte{0}};
    CpuDrawStore store;
    ASSERT_TRUE(store.SetActivePolicies(1, policies));
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(scene.DrawRecords.size(), 6u);
    EXPECT_EQ(store.GetStats().BindingRecipeCompiles, 2u);
    EXPECT_EQ(scene.DrawRecords[0].BindingRecipe, scene.DrawRecords[1].BindingRecipe);
    EXPECT_NE(scene.DrawRecords[0].BindingRecipe, scene.DrawRecords[2].BindingRecipe);
    EXPECT_FALSE(scene.ResolveDraw(scene.DrawRecords[0]).Description.PipelineState.DepthStencil.DepthWriteEnable);
    const auto frozen = scene;
    scene.Materials[0].Passes[0].NumericBytes[0] = byte{1};
    ++scene.Materials[0].ValuesRevision;
    const array<uint32_t, 2> changed{0, 1};
    ASSERT_TRUE(store.SyncChanged(scene, changed, false));
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 2u);
    EXPECT_EQ(store.GetStats().BindingRecipeCompiles, 0u);
    EXPECT_TRUE(scene.ResolveDraw(scene.DrawRecords[0]).Description.PipelineState.DepthStencil.DepthWriteEnable);
    EXPECT_FALSE(frozen.ResolveDraw(frozen.DrawRecords[0]).Description.PipelineState.DepthStencil.DepthWriteEnable);
}

TEST_P(MeshBindingGpuTest, StaticCompilerSelectsFrozenProgramsRangesAndFiltersWithoutPoisoningBindings) {
    auto plain = test::CompileFoundationGraphics(*Context.Device, kProgram);
    string vertexSource{kProgram};
    const auto begin = vertexSource.find("[shader(\"vertex\")]");
    const auto end = vertexSource.find("[shader(\"pixel\")]");
    vertexSource.replace(begin, end - begin, R"hlsl(
[shader("vertex")] float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1); }
)hlsl");
    auto position = test::CompileFoundationGraphics(*Context.Device, vertexSource);
    ASSERT_TRUE(plain);
    ASSERT_TRUE(position);
    RenderSceneSnapshot scene;
    array<GpuMesh::DrawData, 2> geometry;
    geometry[1].VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    geometry[1].VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    FillPolicyScene(scene, geometry[0], plain.Get(), 4);
    scene.MeshBatches[1].Geometry = scene.MeshBatches[3].Geometry = &geometry[1];
    auto variant = scene.Materials[0].Passes[0];
    variant.PassName = "PositionVariant";
    variant.Program = position.Get();
    variant.ProgramGeneration = position->GetGeneration();
    variant.ProgramFrameId = 2;
    scene.Materials[0].Passes.push_back(std::move(variant));
    scene.Materials[0].Passes[0].NumericBytes = {byte{0}};
    const MeshBindingContract contract{104, 1, 0, ResolveLens};
    PassPolicy policy{{1004}, 1, "CustomSurface", nullptr, contract};
    policy.Dependencies = MeshPassDependency::MaterialValues | MeshPassDependency::PrimitiveValues;
    policy.CompileMesh = +[](const MeshStaticDrawCompileInput& input, MeshStaticDrawCompileResult& result) {
        result.ProgramPassIndex = input.Pass.NumericBytes[0] == byte{1} ? 1 : 0;
        result.FirstIndex = input.Batch.FirstIndex + 1;
        result.IndexCount = 2;
        result.VertexOffset = -1;
        return input.Primitive.LocalToWorld(0, 3) < 0 ? MeshStaticCompileStatus::Filtered : MeshStaticCompileStatus::Ready;
    };
    CpuDrawStore store;
    ASSERT_TRUE(store.SetActivePolicies(1, std::span{&policy, 1}));
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(scene.DrawRecords.size(), 4u);
    EXPECT_EQ(store.GetStats().BindingRecipeCompiles, 1u);
    EXPECT_EQ(store.GetStats().VertexInputCompiles, 2u);
    for (const auto& record : scene.DrawRecords) {
        const auto draw = scene.ResolveDraw(record).Description;
        EXPECT_EQ(record.Status, DrawRecordStatus::Ready);
        EXPECT_EQ(draw.Program.Get(), plain.Get());
        EXPECT_EQ(draw.FirstIndex, 1u);
        EXPECT_EQ(draw.IndexCount, 2u);
        EXPECT_EQ(draw.VertexOffset, -1);
    }
    const auto frozen = scene;
    scene.Materials[0].Passes[0].NumericBytes[0] = byte{1};
    ++scene.Materials[0].ValuesRevision;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().BindingRecipeCompiles, 1u);
    EXPECT_EQ(store.GetStats().VertexInputCompiles, 2u);
    for (uint32_t index = 0; index < 4; ++index) {
        const auto& record = scene.DrawRecords[index];
        EXPECT_EQ(record.Status, index & 1 ? DrawRecordStatus::Ready : DrawRecordStatus::InvalidGeometry);
        EXPECT_EQ(record.PassIndex, 1u);
        EXPECT_EQ(record.ProgramFrameId, 2u);
        EXPECT_EQ(scene.ResolveDraw(record).Description.Program.Get(), position.Get());
        EXPECT_TRUE(scene.BindingRecipes[record.BindingRecipe].Valid);
        EXPECT_EQ(frozen.ResolveDraw(frozen.DrawRecords[index]).Description.Program.Get(), plain.Get());
    }
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 0u);
    EXPECT_EQ(store.GetStats().VertexInputCompiles, 0u);
    scene.Materials[0].Passes[1].Valid = false;
    ASSERT_TRUE(store.Sync(scene));
    for (const auto& record : scene.DrawRecords) EXPECT_EQ(record.Status, DrawRecordStatus::InvalidBindings);
    scene.Materials[0].Passes[1].Valid = true;
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(scene.DrawRecords[1].Status, DrawRecordStatus::Ready);
    scene.Primitives[1].LocalToWorld(0, 3) = -1;
    ++scene.Primitives[1].TransformRevision;
    const uint32_t changed = 1;
    ASSERT_TRUE(store.SyncChanged(scene, std::span{&changed, 1}, false));
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 1u);
    EXPECT_EQ(scene.DrawRecords[1].Status, DrawRecordStatus::Filtered);
    ResolvedRenderView view;
    CullingResults culling;
    culling.Scene = &scene;
    culling.View = &view;
    culling.Stats.Valid = true;
    culling.Primitives.push_back({1, 0});
    class FilterProbe final : public MeshPassProcessor {
    public:
        uint32_t Calls{0};
        void AddMeshBatch(const RendererListDesc&, const RenderSceneSnapshot&, const MeshBatch&, MeshPassDrawListContext&) override { ++Calls; }
    } processor;
    RendererList list;
    RendererListDesc desc{"filtered", "CustomSurface", &culling, &view};
    desc.Policy = policy.Id;
    desc.RequireMaterialPass = true;
    ASSERT_TRUE(BuildRendererList(desc, processor, list));
    EXPECT_TRUE(list.Stats.ContentSucceeded());
    EXPECT_EQ(list.Stats.FilteredDraws, 1u);
    EXPECT_EQ(processor.Calls, 0u);
    EXPECT_EQ(list.GetDrawCount(), 0u);
    // The old immutable vertex input survives removal of all active cache consumers.
    auto oldInput = frozen.GeometryBindingPlans[frozen.DrawRecords[0].GeometryBindingPlan].VertexInput;
    ASSERT_TRUE(oldInput && oldInput->Input);
    scene.Primitives.clear();
    scene.MeshBatches.clear();
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(store.GetActiveVertexInputCount(), 0u);
    EXPECT_TRUE(oldInput->Input.has_value());
}

TEST_P(MeshBindingGpuTest, PolicyConfigurationsCompileAndSelectIndependentlyWhileSharingProgramPlans) {
    auto program = test::CompileFoundationGraphics(*Context.Device, kProgram);
    ASSERT_TRUE(program);
    RenderSceneSnapshot scene;
    GpuMesh::DrawData geometry;
    FillPolicyScene(scene, geometry, program.Get(), 2);
    const MeshBindingContract contract{8101, 1, 0, ResolveLens};
    array<PassPolicy, 2> policies{{{{8102}, 1, "CustomSurface", CompilePolicyWrite, contract, 0},
                                   {{8102}, 1, "CustomSurface", CompilePolicyRead, contract, 1}}};
    CpuDrawStore store;
    ASSERT_TRUE(store.SetActivePolicies(1, policies));
    ASSERT_TRUE(store.Sync(scene));
    ASSERT_EQ(scene.DrawRecords.size(), 4u);
    ASSERT_EQ(store.GetStats().BindingRecipeCompiles, 1u);
    const auto frozen = scene;
    ResolvedRenderView view;
    CullingResults culling;
    culling.Scene = &scene;
    culling.View = &view;
    culling.Stats.Valid = true;
    culling.Primitives = {{0, 0}, {1, 0}};
    class Probe final : public MeshPassProcessor {
    public:
        uint64_t Configuration{UINT64_MAX};
        uint32_t RecordCalls{0};
        bool RejectBatch{false};
        bool PrepareBatch(std::span<const MeshPassListPreparation> batches) override {
            EXPECT_EQ(batches.size(), 1u);
            const auto& batch = batches.front();
            if (!batch.Candidates.empty()) {
                EXPECT_EQ(batch.BindingPlans.size(), 1u);
                EXPECT_EQ(batch.DrawPlans.size(), 1u);
            }
            return !RejectBatch;
        }
        void AddMeshBatch(const RendererListDesc&, const RenderSceneSnapshot&, const MeshBatch&, MeshPassDrawListContext&) override { ADD_FAILURE(); }
        void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene, const DrawRecord& record, MeshPassDrawListContext& out) override {
            EXPECT_EQ(record.PolicyConfiguration, desc.PolicyConfiguration);
            ++RecordCalls;
            Configuration = record.PolicyConfiguration;
            MeshDrawCommand command;
            static_cast<MeshDrawDescription&>(command) = scene.ResolveDraw(record).Description;
            out.AddCommand(std::move(command));
        }
    } probe;
    for (uint64_t config = 0; config < 3; ++config) {
        RendererList list;
        RendererListDesc desc{"configured", "CustomSurface", &culling, &view};
        desc.Policy = policies[0].Id;
        desc.PolicyConfiguration = config;
        desc.RequireMaterialPass = true;
        ASSERT_TRUE(BuildRendererList(desc, probe, list));
        EXPECT_EQ(list.GetDrawCount(), config < 2 ? 2u : 0u);
        if (config < 2) {
            EXPECT_TRUE(list.Stats.ContentSucceeded());
            EXPECT_EQ(probe.Configuration, config);
            for (size_t draw = 0; draw < list.GetDrawCount(); ++draw)
                EXPECT_EQ(list.GetPipelineState(draw).DepthStencil.DepthWriteEnable, config == 0);
        } else
            EXPECT_EQ(list.Stats.MissingRequiredPass, 2u);
    }
    RendererList list;
    RendererListDesc rejected{"rejected batch", "CustomSurface", &culling, &view};
    rejected.Policy = policies[0].Id;
    const auto previousCalls = probe.RecordCalls;
    probe.RejectBatch = true;
    EXPECT_FALSE(BuildRendererList(rejected, probe, list));
    EXPECT_FALSE(list.Stats.Valid);
    EXPECT_EQ(list.GetDrawCount(), 0u);
    EXPECT_EQ(probe.RecordCalls, previousCalls);
    probe.RejectBatch = false;
    ASSERT_TRUE(BuildRendererList(rejected, probe, list));
    EXPECT_EQ(list.GetDrawCount(), 2u);
    auto conflict = policies;
    conflict[0].CompileStatic = CompilePolicyRead;
    EXPECT_FALSE(store.SetActivePolicies(2, conflict));
    policies[1].Revision = 2;
    policies[1].CompileStatic = CompilePolicyWrite;
    ASSERT_TRUE(store.SetActivePolicies(2, policies));
    ASSERT_TRUE(store.SyncChanged(scene, {}, false));
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 2u);
    EXPECT_EQ(store.GetStats().BindingRecipeCompiles, 0u);
    EXPECT_EQ(store.GetActiveDrawPlanCount(), 1u);
    for (uint32_t index = 0; index < scene.DrawRecords.size(); ++index) {
        EXPECT_EQ(scene.DrawRecords[index].Id, frozen.DrawRecords[index].Id);
        EXPECT_TRUE(scene.ResolveDraw(scene.DrawRecords[index]).Description.PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_EQ(frozen.ResolveDraw(frozen.DrawRecords[index]).Description.PipelineState.DepthStencil.DepthWriteEnable,
                  frozen.DrawRecords[index].PolicyConfiguration == 0);
    }
}

TEST_P(MeshBindingGpuTest, SparseParameterRowsRemainBoundedAcrossHighIndicesEpochsAndFailures) {
    render::ShaderProgramLayoutRecipe recipe;
    const render::ShaderLayoutSelector selector{.DeclarationName = "SparseData", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
    recipe.D3D12.BufferPlacements.push_back({selector, render::D3D12BufferPlacement::RootDescriptor});
    recipe.Vulkan.BufferDescriptors.push_back({selector, render::VulkanBufferDescriptorPlacement::Dynamic});
    auto program = test::CompileFoundationGraphics(*Context.Device, R"hlsl(
#include <core/platform.hlsli>
struct Values { float4 Value; };
VK_BINDING(8, 7) ConstantBuffer<Values> SparseData : register(b8, space7);
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }
[shader("pixel")] float PSMain() : SV_Target0 { return SparseData.Value.x; }
)hlsl",
                                                   recipe);
    ASSERT_TRUE(program);
    FrameDrawResources resources{Context.Device.get()};
    static constexpr byte source{}, wire{};
    array<float, 4> values{1, 2, 3, 4};
    FrameParameterDomain previous;
    size_t capacity = 0;
    for (uint32_t epoch = 0; epoch < 10010; ++epoch) {
        ASSERT_TRUE(resources.BeginFrame(Writes));
        EXPECT_FALSE(resources.IsValid(previous));
        EXPECT_EQ(resources.GetParameterRowCount(), 0u);
        array<FrameParameterDomain, 4> domains;
        array<FrameParameterGroup, 4> groups;
        for (uint32_t layout = 0; layout < domains.size(); ++layout) {
            domains[layout] = resources.RegisterParameterDomain({&source, &wire, layout});
            groups[layout] = resources.RegisterParameterGroup(*program, 7, domains[layout]);
            ASSERT_TRUE(groups[layout].IsValid());
        }
        const auto boundaryLookups = resources.GetStats().NativeGroupLookups;
        for (uint32_t layout = 0; layout < domains.size(); ++layout) {
            const array<uint32_t, 2> rows{(layout + 1u) * 10000000u + epoch * 257u, UINT32_MAX - 1};
            for (uint32_t row : rows) {
                if (epoch == 0 && layout == 0) resources.FailNextGroupForTesting();
                auto id = resources.PrepareIndexedGroup(groups[layout], row, std::as_bytes(std::span{values}));
                if (epoch == 0 && layout == 0) {
                    EXPECT_FALSE(id.IsValid());
                    EXPECT_TRUE(resources.HasCBufferValues(domains[layout], row));
                    id = resources.PrepareIndexedGroup(groups[layout], row, {});
                }
                ASSERT_TRUE(id.IsValid());
                EXPECT_EQ(resources.PrepareIndexedGroup(groups[layout], row, {}), id);
                const auto tuple = resources.PrepareIndexedBinding(domains[layout], row, array{id});
                ASSERT_TRUE(tuple.IsValid());
                EXPECT_EQ(resources.PrepareIndexedBinding(domains[layout], row, array{id}), tuple);
            }
        }
        EXPECT_EQ(resources.GetStats().NativeGroupLookups, boundaryLookups);
        EXPECT_EQ(resources.GetStats().SharedBufferUploads, 8u);
        EXPECT_EQ(resources.GetParameterRowCount(), 24u);
        EXPECT_FALSE(resources.PrepareIndexedGroup(groups[0], UINT32_MAX, {}).IsValid());
        EXPECT_FALSE(resources.HasCBufferValues(domains[0], 0));
        // Capacity tracks the eight used combinations, not the largest row or its cross product with layouts.
        EXPECT_LT(resources.GetCacheCapacityBytes(), 256u * 1024u);
        if (epoch == 10) capacity = resources.GetCacheCapacityBytes();
        if (epoch > 10) EXPECT_EQ(resources.GetCacheCapacityBytes(), capacity);
        previous = domains[0];
        Writes.Flush(*Context.Device);
    }
}

uint32_t gViewCompiles{0};
TEST_P(MeshBindingGpuTest, ViewCompilationUsesOnlyVisibleCandidatesAndDoesNotModifyPublishedPlans) {
    auto first = test::CompileFoundationGraphics(*Context.Device, kProgram);
    auto second = test::CompileFoundationGraphics(*Context.Device, kProgram);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    RenderSceneSnapshot scene;
    array<GpuMesh::DrawData, 2> geometries;
    FillPolicyScene(scene, geometries[0], first.Get(), 3);
    auto alternate = scene.Materials[0].Passes[0];
    alternate.PassName = "NearVariant";
    alternate.Program = second.Get();
    alternate.ProgramGeneration = second->GetGeneration();
    alternate.ProgramFrameId = 8;
    scene.Materials[0].Passes[0].ProgramFrameId = 4;
    scene.Materials[0].Passes.push_back(std::move(alternate));
    scene.MeshBatches[1].Geometry = &geometries[1];
    PassPolicy policy{{1901}, 1, "CustomSurface", nullptr, {1902, 1, 0, ResolveLens}};
    policy.CacheMode = MeshPassCacheMode::PerView;
    policy.CompileView = +[](const MeshStaticDrawCompileInput& input, const ResolvedRenderView& view, MeshStaticDrawCompileResult& result) {
        ++gViewCompiles;
        if (view.LodBias == 0) return MeshStaticCompileStatus::Filtered;
        result.ProgramPassIndex = view.LodBias < 1 ? 1 : 0;
        result.FirstIndex = view.LodBias < 1 ? 2 : 0;
        result.IndexCount = view.LodBias < 1 ? 1 : 3;
        result.VertexOffset = view.PreviousViewValid ? -1 : 0;
        // Invalid geometry affects only the candidate in the view that selected it.
        if (view.LodBias < 1 && input.Batch.Primitive == 0) result.Geometry = nullptr;
        result.NormalState.DepthStencil.DepthWriteEnable = view.LodBias >= 1;
        result.MirroredState = result.NormalState;
        return MeshStaticCompileStatus::Ready;
    };
    CpuDrawStore store;
    auto invalid = policy;
    invalid.CacheMode = MeshPassCacheMode::OnChange;
    EXPECT_FALSE(store.SetActivePolicies(1, std::span{&invalid, 1}));
    gViewCompiles = 0;
    ASSERT_TRUE(store.SetActivePolicies(1, std::span{&policy, 1}));
    ASSERT_TRUE(store.Sync(scene));
    EXPECT_EQ(gViewCompiles, 0u);
    EXPECT_EQ(store.GetStats().StaticRecipeCompiles, 0u);
    EXPECT_EQ(store.GetStats().BindingRecipeCompiles, 0u);
    const auto frozen = scene;
    class Processor final : public MeshPassProcessor {
    public:
        uint32_t Preparations{0}, Batches{0};
        bool PrepareBatch(std::span<const MeshPassListPreparation> batches) override {
            ++Batches;
            EXPECT_EQ(batches.size(), 2u);
            for (const auto& batch : batches) {
                EXPECT_EQ(batch.Candidates.size(), batch.ViewDraws.size());
                EXPECT_TRUE(batch.BindingPlans.empty());
                EXPECT_TRUE(batch.DrawPlans.empty());
            }
            return true;
        }
        void AddMeshBatch(const RendererListDesc&, const RenderSceneSnapshot&, const MeshBatch&, MeshPassDrawListContext& out) override {
            out.Reject(MeshPassRejectReason::ProcessorRejected);
        }
        void PrepareViewRecord(const RendererListDesc&, const RenderSceneSnapshot& scene, const DrawRecord& record,
                               const MeshStaticDrawCompileResult& compiled, MeshPassDrawListContext& out) override {
            ++Preparations;
            MeshDrawCommand command;
            command.Program = scene.Materials[record.Material].Passes[compiled.ProgramPassIndex].Program;
            command.Geometry = compiled.Geometry;
            command.FirstIndex = compiled.FirstIndex;
            command.IndexCount = compiled.IndexCount;
            command.VertexOffset = compiled.VertexOffset;
            command.PipelineState = record.Mirrored ? compiled.MirroredState : compiled.NormalState;
            out.AddCommand(std::move(command));
        }
    } processor;
    ResolvedRenderView view{};
    CullingResults culling;
    culling.Scene = &scene;
    culling.View = &view;
    culling.Stats.Valid = true;
    culling.Primitives = {{0, 1}, {1, 2}};
    array<RendererListDesc, 2> descs;
    for (auto& desc : descs) {
        desc = {"view", "CustomSurface", &culling, &view};
        desc.Policy = policy.Id;
    }
    array<RendererList, 2> lists;
    array<RendererList*, 2> outputs{&lists[0], &lists[1]};
    ASSERT_TRUE(BuildRendererLists(descs, processor, outputs));
    EXPECT_EQ(processor.Batches, 1u);
    EXPECT_EQ(gViewCompiles, 2u);  // Same view/policy in two lists compiles each candidate only once.
    EXPECT_EQ(processor.Preparations, 4u);
    EXPECT_EQ(lists[0].GetDrawCount(), 2u);
    EXPECT_EQ(lists[0].GetDescription(0).Program.Get(), first.Get());
    EXPECT_EQ(lists[0].Items[0].SortData.ProgramFrameId, 4u);
    RendererList distant = lists[0];
    view.LodBias = .5f;
    view.PreviousViewValid = true;
    ASSERT_TRUE(BuildRendererLists(descs, processor, outputs));
    EXPECT_EQ(gViewCompiles, 4u);
    ASSERT_EQ(lists[0].GetDrawCount(), 1u);
    EXPECT_EQ(lists[0].Stats.InvalidGeometry, 1u);
    const auto nearDraw = lists[0].GetDescription(0);
    EXPECT_EQ(nearDraw.Program.Get(), second.Get());
    EXPECT_EQ(nearDraw.Geometry.Get(), &geometries[1]);
    EXPECT_EQ(nearDraw.FirstIndex, 2u);
    EXPECT_EQ(nearDraw.IndexCount, 1u);
    EXPECT_EQ(nearDraw.VertexOffset, -1);
    EXPECT_FALSE(nearDraw.PipelineState.DepthStencil.DepthWriteEnable);
    EXPECT_EQ(lists[0].Items[0].SortData.ProgramFrameId, 8u);
    EXPECT_EQ(distant.GetDescription(0).Program.Get(), first.Get());
    EXPECT_EQ(distant.GetDescription(0).IndexCount, 3u);
    scene.Materials[0].Passes[1].Valid = false;
    ASSERT_TRUE(BuildRendererLists(descs, processor, outputs));
    EXPECT_EQ(lists[0].Stats.InvalidBindings, 2u);
    EXPECT_EQ(lists[0].GetDrawCount(), 0u);
    scene.Materials[0].Passes[1].Valid = true;
    ASSERT_TRUE(BuildRendererLists(descs, processor, outputs));
    EXPECT_EQ(lists[0].GetDrawCount(), 1u);
    view.LodBias = 0;
    ASSERT_TRUE(BuildRendererLists(descs, processor, outputs));
    EXPECT_EQ(lists[0].Stats.FilteredDraws, 2u);
    EXPECT_TRUE(lists[0].Stats.ContentSucceeded());
    for (size_t index = 0; index < scene.DrawRecords.size(); ++index) {
        EXPECT_EQ(scene.DrawRecords[index].Id, frozen.DrawRecords[index].Id);
        EXPECT_EQ(scene.DrawRecords[index].RecipeRevision, frozen.DrawRecords[index].RecipeRevision);
        EXPECT_EQ(scene.DrawRecords[index].Plan, frozen.DrawRecords[index].Plan);
        EXPECT_EQ(scene.ResolveDraw(scene.DrawRecords[index]).Description.Program.Get(), first.Get());
    }
}

TEST_P(MeshBindingGpuTest, MultipleViewSlotsMixedGroupsAndGraphResourcesMatchNamedReference) {
    auto& device = *Context.Device;
    auto program = test::CompileFoundationGraphics(device, kProgram);
    auto initializer = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<float> Values : register(u0);
[shader("compute")][numthreads(1, 1, 1)] void CSMain() { Values[0] = 2; }
)hlsl");
    ASSERT_TRUE(program);
    ASSERT_TRUE(initializer);
    auto cleanup = MakeScopeGuard([this]() noexcept { Resources->Clear(); });
    MeshBindingPlan plan;
    ASSERT_TRUE(ResolveLens(*program, plan));
    ASSERT_TRUE(plan.Finalize(*program));
    EXPECT_EQ(plan.Groups, (vector<uint32_t>{3, 5}));
    EXPECT_EQ(plan.GroupBegin, (vector<uint32_t>{0, 4, 6}));
    const auto validPlan = plan;
    plan.Bindings.pop_back();
    EXPECT_FALSE(plan.Finalize(*program));
    EXPECT_FALSE(plan.Valid);
    plan = validPlan;
    std::erase_if(plan.Bindings, [](const MeshParameterBinding& binding) { return binding.Group == 5; });
    EXPECT_FALSE(plan.Finalize(*program));
    EXPECT_FALSE(plan.Valid);
    plan = validPlan;
    const array<uint32_t, 3> indices{0, 1, 2};
    const array<float, 3> unusedVertices{};
    auto indexBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    auto vertexBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{unusedVertices}), render::BufferUse::Vertex);
    ASSERT_TRUE(indexBuffer);
    ASSERT_TRUE(vertexBuffer);
    GpuMesh::DrawData geometry;
    geometry.Ibv = {indexBuffer.Get(), 0, sizeof(uint32_t)};
    geometry.VertexBuffers = {{0, {vertexBuffer.Get(), 0, sizeof(unusedVertices)}}};
    geometry.VertexLayout.Buffers = {{0, sizeof(float), render::VertexStepMode::Vertex}};
    RenderSceneSnapshot scene;
    FillPolicyScene(scene, geometry, program.Get(), 2);
    scene.Materials[0].Passes[0].PipelineState.Primitive.Cull = render::CullMode::None;
    scene.Materials[0].Passes[0].PipelineState.DepthStencil.DepthTestEnable = false;
    scene.Materials[0].Passes[0].PipelineState.DepthStencil.DepthWriteEnable = false;
    PassPolicy policy{{2001}, 1, "CustomSurface", nullptr, {2002, 1, 0, ResolveLens}};
    policy.CompileMesh = +[](const MeshStaticDrawCompileInput&, MeshStaticDrawCompileResult&) { return MeshStaticCompileStatus::Ready; };
    CpuDrawStore store;
    ASSERT_TRUE(store.SetActivePolicies(1, std::span{&policy, 1}));
    ASSERT_TRUE(store.Sync(scene));
    class LensProcessor final : public MeshPassProcessor {
    public:
        Nullable<RenderGraphPrepareContext*> Context{nullptr};
        Nullable<FrameDrawResources*> Resources{nullptr};
        std::span<const RgMeshParameterSlot> Slots;
        FrameDrawBindingId Binding;
        bool PrepareBatch(std::span<const MeshPassListPreparation> batches) override {
            EXPECT_EQ(batches.size(), 1u);
            const auto& batch = batches.front();
            EXPECT_EQ(batch.Candidates.size(), 2u);
            EXPECT_TRUE(batch.ViewDraws.empty());
            EXPECT_EQ(batch.BindingPlans.size(), 1u);
            EXPECT_EQ(batch.DrawPlans.size(), 1u);
            if (batch.BindingPlans.size() != 1 || batch.Candidates.empty()) return false;
            const auto& scene = *batch.Scene;
            const auto& record = scene.DrawRecords[batch.Candidates.front().Record];
            const auto& plan = scene.BindingRecipes[batch.BindingPlans.front()].Parameters;
            auto& program = *scene.Materials[record.Material].Passes[record.PassIndex].Program;
            Binding = Context->CreateMeshParameterBinding(*Resources, program, plan, Slots);
            if (!Binding.IsValid()) return false;
            const auto repeated = Context->CreateMeshParameterSet(program, plan, 1, Slots);
            EXPECT_TRUE(repeated.IsValid());
            EXPECT_EQ(Resources->GetStats().SharedBufferUploads, 2u);
            EXPECT_EQ(Resources->GetStats().BufferBytesCopied, 32u);
            return true;
        }
        void AddMeshBatch(const RendererListDesc&, const RenderSceneSnapshot&, const MeshBatch&, MeshPassDrawListContext& out) override {
            out.Reject(MeshPassRejectReason::ProcessorRejected);
        }
        void PrepareRecord(const RendererListDesc&, const RenderSceneSnapshot&, const DrawRecord&, MeshPassDrawListContext& out) override {
            out.AddRecord(*Resources, Binding);
        }
    };
    // Both a reflected, named reference and the indexed contract run real RHI work on every backend.
    FrameDrawResources drawResources{&device};
    static const byte scalarWire{};
    for (uint32_t frame = 0; frame < 8; ++frame) {
        ASSERT_TRUE(drawResources.BeginFrame(Writes));
        auto domain = drawResources.RegisterParameterDomain({&scene, &scalarWire});
        if (frame >= 6) --domain.Epoch;
        Resources->BeginFlight(frame + 1, Writes);
        RenderGraph graph{device, *Resources, *Registry, "independent lens pass", {frame & 1 ? RenderValidationMode::Off : RenderValidationMode::Full, RenderGraphReportMode::Counters, false}};
        array<RgTextureValue, 2> inputs, outputs;
        for (uint32_t index = 0; index < 2; ++index) {
            inputs[index] = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource, {}}, "input");
            outputs[index] = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource | render::TextureUse::Resource, {}}, "output");
            graph.AddRasterPass<test::EmptyGraphPass>("clear input", [&](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, inputs[index], {.Clear = {.25f * (index + 1), 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
        }
        auto coefficients = graph.CreateBuffer({4, render::MemoryType::Device, render::BufferUse::Resource | render::BufferUse::UnorderedAccess, {}}, "coefficients");
        struct Init {
            ShaderProgram* Program;
            RgBufferValue Buffer;
            PreparedShaderGroup Set;
            Nullable<render::ComputePipelineState*> Pipeline{nullptr};
        };
        graph.AddComputePass<Init>("initialize coefficients", [&](Init& data, RenderGraphComputeBuilder& builder) {
            data.Program = initializer.Get(); data.Buffer = coefficients = builder.WriteBuffer(coefficients); }, +[](Init& data, RenderGraphPrepareContext& context) {
            const RgParameterBinding binding{"Values", 0, RgBufferParameterBinding{data.Buffer, {0, 4}, 4}};
            data.Set = context.CreateParameterSet(*data.Program, 0, std::span{&binding, 1});
            data.Pipeline = context.ResolveComputePipeline(*data.Program);
            return data.Set.IsValid() && bool(data.Pipeline); }, +[](const Init& data, RenderGraphComputeContext& context) {
            context.Encoder().BindComputePipelineState(data.Pipeline.Get());
            context.Encoder().BindShaderParameterSet(data.Set);
            context.Encoder().Dispatch(1, 1, 1); });
        struct Draw {
            ShaderProgram* Program;
            const MeshBindingPlan* Plan;
            const RenderSceneSnapshot* Scene;
            PassPolicyId Policy;
            FrameDrawResources* Resources;
            FrameParameterDomain Domain;
            RendererList List;
            std::optional<PreparedRendererList> Ready;
            bool Indexed;
            render::RenderBackend Backend;
            array<RgTextureViewHandle, 2> Inputs;
            RgBufferValue Coefficients;
            array<float, 4> Gain, Bias;
            array<PreparedShaderGroup, 2> Sets;
            Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
        };
        Nullable<RendererList*> borrowedList{nullptr};
        for (uint32_t index = 0; index < 2; ++index) {
            graph.AddRasterPass<Draw>(index ? "indexed lens" : "named reference", [&](Draw& data, RenderGraphRasterBuilder& builder) {
                data.Program = program.Get(); data.Plan = &scene.BindingRecipes[scene.DrawRecords[0].BindingRecipe].Parameters; data.Scene = &scene; data.Policy = policy.Id; data.Indexed = index != 0; data.Backend = GetParam();
                data.Resources = &drawResources; data.Domain = domain;
                if (index) borrowedList = &data.List;
                data.Gain = {.4f + .1f * frame, 0, 0, 0}; data.Bias = {.2f, 0, 0, 0};
                for (uint32_t input = 0; input < 2; ++input) data.Inputs[input] = builder.ReadTexture(inputs[input]);
                data.Coefficients = builder.ReadBuffer(coefficients, RgBufferAccess::ShaderRead);
                builder.SetColorAttachment(0, outputs[index]); }, +[](Draw& data, RenderGraphPrepareContext& context) {
                const array<RgParameterBindingValue, 1> gain{{RgCBufferParameterBinding{std::as_bytes(std::span{data.Gain}), data.Indexed ? data.Domain : FrameParameterDomain{}, 0}}};
                const array<RgParameterBindingValue, 1> bias{{RgCBufferParameterBinding{std::as_bytes(std::span{data.Bias}), data.Indexed ? data.Domain : FrameParameterDomain{}, 1}}};
                const array<RgParameterBindingValue, 2> images{{RgTextureParameterBinding{data.Inputs[0]}, RgTextureParameterBinding{data.Inputs[1]}}};
                const array<RgParameterBindingValue, 1> sampler{{RgSamplerParameterBinding{}}};
                const array<RgParameterBindingValue, 1> coefficients{{RgBufferParameterBinding{data.Coefficients, {0, 4}, 4}}};
                const array<RgMeshParameterSlot, 5> slots{{{gain}, {bias}, {images}, {sampler}, {coefficients}}};
                if (!data.Indexed) {
                    const array<RgParameterBinding, 4> resources{{{"Samples", 0, images[0]}, {"Samples", 1, images[1]}, {"SampleFilter", 0, sampler[0]}, {"Coefficients", 0, coefficients[0]}}};
                    const array<RgParameterBinding, 2> values{{{"LensGain", 0, gain[0]}, {"LensBias", 0, bias[0]}}};
                    data.Sets[0] = context.CreateParameterSet(*data.Program, 3, resources);
                    data.Sets[1] = context.CreateParameterSet(*data.Program, 5, values);
                }
                MaterialPipelineState state;
                state.Primitive.Cull = render::CullMode::None;
                state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
                if (data.Indexed) {
                    ResolvedRenderView view{};
                    CullingResults culling;
                    culling.Scene = data.Scene; culling.View = &view; culling.Stats.Valid = true;
                    culling.Primitives = {{0, 1}, {1, 2}};
                    RendererListDesc desc{"independent mesh", "CustomSurface", &culling, &view};
                    desc.Policy = data.Policy;
                    LensProcessor processor;
                    processor.Context = &context;
                    processor.Resources = data.Resources;
                    processor.Slots = slots;
                    if (!BuildRendererList(desc, processor, data.List) || !data.List.Stats.ContentSucceeded()) return false;
                    EXPECT_TRUE(data.List.Commands.empty());
                    data.Ready = PrepareRendererList(data.List, context);
                    return data.Ready.has_value();
                }
                data.Pipeline = context.ResolveGraphicsPipeline(*data.Program, state);
                return data.Sets[0].IsValid() && data.Sets[1].IsValid() && bool(data.Pipeline); }, +[](const Draw& data, RenderGraphRasterContext& context) {
                if (data.Indexed) {
                    context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 4, 4));
                    context.Encoder().SetScissor({0, 0, 4, 4});
                    DrawExecutionStats stats;
                    RecordRendererList(*data.Ready, context, stats);
                    EXPECT_TRUE(stats.Succeeded());
                    EXPECT_EQ(stats.Draws, 2u);
                    return;
                }
                context.Encoder().BindGraphicsPipelineState(data.Pipeline.Get());
                for (const auto& set : data.Sets) context.Encoder().BindShaderParameterSet(set);
                context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 4, 4));
                context.Encoder().SetScissor({0, 0, 4, 4});
                context.Encoder().Draw(3, 1, 0, 0); });
        }
        if (frame == 4 || frame == 5) {
            struct Borrowed {
                RendererList* List;
            };
            ASSERT_TRUE(borrowedList);
            const auto target = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "invalid consumer target");
            graph.AddRasterPass<Borrowed>("invalid graph binding consumer", [&](Borrowed& data, RenderGraphRasterBuilder& builder) {
                data.List = borrowedList.Get();
                builder.ReadTexture(outputs[1]);
                builder.SetColorAttachment(0, target);
                builder.SetSideEffect(); }, +[](Borrowed& data, RenderGraphPrepareContext& context) { return PrepareRendererList(*data.List, context).has_value(); }, +[](const Borrowed&, RenderGraphRasterContext&) { ADD_FAILURE() << "Cross-pass binding recorded"; });
        }
        const auto reference = graph.ReadbackTexture("reference", outputs[0]);
        const auto indexed = graph.ReadbackTexture("indexed", outputs[1]);
        if (frame >= 4) {
            EXPECT_FALSE(Run(graph));
            EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 0u);
            EXPECT_EQ(graph.GetFirstErrorCode(), frame < 6 ? "RendererListBindingPass" : "ParameterUpload");
            if (frame >= 6) EXPECT_EQ(drawResources.GetBindingCount(), 0u);
            continue;
        }
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        vector<byte> referenceBytes, indexedBytes;
        ASSERT_TRUE(reference.Read(referenceBytes));
        ASSERT_TRUE(indexed.Read(indexedBytes));
        ASSERT_EQ(referenceBytes, indexedBytes);
        ASSERT_GE(indexedBytes.size(), sizeof(float));
        float value;
        std::memcpy(&value, indexedBytes.data(), sizeof(value));
        EXPECT_NEAR(value, .75f * (.4f + .1f * frame) + .2f + 2.f, .00001f);
    }
}
INSTANTIATE_TEST_SUITE_P(Backends, MeshBindingGpuTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
