#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "stage_b_test_support.h"

#include <algorithm>
#include <cstring>
#include <tuple>

#include <gtest/gtest.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

constexpr uint32_t kRequestedPublicationFrames = 18;

class ConsumerPrimitive final : public PrimitiveSceneProxy {
public:
    ConsumerPrimitive(const GpuMesh::DrawData* geometry, Material* material) : Geometry(geometry), DrawMaterial(material) {}
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {Geometry, 0, 3, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
    const GpuMesh::DrawData* Geometry;
    Material* DrawMaterial;
};

bool CompileConsumerPolicy(const StaticPassCompileInput& input, StaticPassCompileResult& result) {
    result.NormalState = result.MirroredState = input.Pass.PipelineState;
    result.Bindings = input.Bindings ? *input.Bindings : StaticBindingRecipe{};
    result.Bindings.Valid = true;
    return true;
}

float NumericColor(std::span<const byte> bytes) {
    if (bytes.size() < sizeof(float)) return -1;
    float result;
    std::memcpy(&result, bytes.data(), sizeof(result));
    return result;
}

struct ConsumerObservations {
    uint32_t Frames{0}, Recorded{0}, SharedPublications{0}, ColdFrames{0}, NumericFrames{0}, StableFrames{0};
    array<bool, 3> Flights{};
    array<bool, kRequestedPublicationFrames> RecordedRequestedFrames{};
    bool Initialized{false};
};

// Each child is a real RenderPipeline participant, dispatched through the application's
// RenderSystem cutoff. Rendering clears from its frozen value; mesh pixel quality is tested
// separately. This fixture checks nonempty CPU recipes and exact publication sharing.
class SceneConsumerPipeline final : public RenderPipeline {
public:
    SceneConsumerPipeline(Scene& scene, PassPolicy policy, string name) : Source(scene), Policy(std::move(policy)), Name(std::move(name)) {}
    void CollectScenePolicies(RenderPrepareContext& prepare) override {
        ASSERT_TRUE(prepare.RegisterScenePolicy(Source, Policy));
        ASSERT_TRUE(prepare.RegisterScenePolicy(Source, Policy));
    }
    void PrepareFrame(RenderPrepareContext& prepare) override {
        ASSERT_TRUE(prepare.ScenesFrozen);
        auto snapshot = prepare.PrepareScene(Source);
        ASSERT_TRUE(snapshot);
        const auto publicationRevision = snapshot->PublicationRevision;
        const auto repeated = prepare.PrepareScene(Source);
        ASSERT_TRUE(repeated);
        EXPECT_EQ(snapshot.Get(), repeated.Get());
        EXPECT_EQ(repeated->PublicationRevision, publicationRevision);
        auto& frame = Frames[prepare.App.FlightIndex];
        frame = snapshot.Release();
        ASSERT_TRUE(frame->Valid);
        EXPECT_EQ(frame->SceneEpoch, prepare.PrepareSerial);
        EXPECT_EQ(frame->Stats.SceneCommits, 1u);
        EXPECT_EQ(frame->Stats.SnapshotPublications, 1u);
        ASSERT_EQ(frame->Materials.size(), 1u);
        ASSERT_EQ(frame->Materials[0].Passes.size(), 1u);
        uint32_t matching = 0;
        for (const auto& draw : frame->DrawRecords) {
            if (draw.Policy != Policy.Id) continue;
            ++matching;
            EXPECT_EQ(draw.Status, DrawRecordStatus::Ready);
            EXPECT_EQ(draw.Description.IndexCount, 3u);
            EXPECT_TRUE(draw.Description.LayoutId.IsValid());
        }
        EXPECT_EQ(matching, 1u);
    }
    void BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        const auto& snapshot = Frames[context.FlightIndex()];
        ASSERT_TRUE(snapshot);
        const float value = NumericColor(snapshot->Materials[0].Passes[0].NumericBytes);
        struct Clear {};
        for (auto& output : outputs) {
            output.Texture = graph.NextVersion(output.Texture);
            graph.AddRasterPass<Clear>(Name, [=, target = output.Texture](Clear&, RenderGraphRasterBuilder& pass) { pass.SetColorAttachment(0, target, {.Clear = {{value, 0, 0, 1}}}); }, +[](const Clear&, RenderGraphRasterContext&) {});
        }
    }
    Scene& Source;
    PassPolicy Policy;
    string Name;
    array<shared_ptr<const RenderSceneSnapshot>, 3> Frames;
};

class SharedScenePipelines final : public RenderPipeline {
public:
    SharedScenePipelines(Scene& shared, Scene& separate, Material& edited, bool different, ConsumerObservations& result)
        : First(shared, {{6101}, 1, "ForwardLit", CompileConsumerPolicy}, "First scene consumer"),
          Second(shared, {{different ? 6102u : 6101u}, 1, "ForwardLit", CompileConsumerPolicy}, "Second scene consumer"),
          Independent(separate, {{6101}, 1, "ForwardLit", CompileConsumerPolicy}, "Independent scene consumer"),
          Edited(edited), Different(different), Result(result) {}
    void CollectScenePolicies(RenderPrepareContext& prepare) override {
        First.CollectScenePolicies(prepare);
        Second.CollectScenePolicies(prepare);
        Independent.CollectScenePolicies(prepare);
        EXPECT_EQ(prepare.RegisteredScenes.size(), 2u);
        EXPECT_EQ(prepare.ScenePolicies.size(), Different ? 3u : 2u);
        ExpectedColor = NumericColor(std::as_const(Edited).NumericBytes());
    }
    void PrepareFrame(RenderPrepareContext& prepare) override {
        prepare.Workloads.AddPresentationOutputs();
        First.PrepareFrame(prepare);
        const auto flight = prepare.App.FlightIndex;
        ASSERT_TRUE(First.Frames[flight]);
        const auto firstPublicationRevision = First.Frames[flight]->PublicationRevision;
        // A mutation after the global cutoff must be invisible to the next consumer and
        // repeated freeze requests. The next epoch must observe it without another setter.
        if (Result.Frames == 5) ASSERT_TRUE(Edited.SetFloat4("BaseColor", {4, 0, 0, 1}));
        ASSERT_TRUE(prepare.FreezeRegisteredScenes());
        Second.PrepareFrame(prepare);
        Independent.PrepareFrame(prepare);
        const auto& first = First.Frames[flight];
        const auto& second = Second.Frames[flight];
        const auto& independent = Independent.Frames[flight];
        ASSERT_TRUE(first && second && independent);
        EXPECT_EQ(first, second);
        EXPECT_EQ(second->PublicationRevision, firstPublicationRevision);
        EXPECT_NE(first, independent);
        EXPECT_FLOAT_EQ(NumericColor(first->Materials[0].Passes[0].NumericBytes), ExpectedColor);
        EXPECT_FLOAT_EQ(NumericColor(second->Materials[0].Passes[0].NumericBytes), ExpectedColor);
        EXPECT_FLOAT_EQ(NumericColor(independent->Materials[0].Passes[0].NumericBytes), .25f);
        EXPECT_EQ(first->DrawRecords.size(), Different ? 2u : 1u);
        EXPECT_EQ(independent->DrawRecords.size(), 1u);
        if (Result.Frames == 0) {
            EXPECT_EQ(first->Stats.StaticRecipeCompiles, Different ? 2u : 1u);
            EXPECT_EQ(independent->Stats.StaticRecipeCompiles, 1u);
            ++Result.ColdFrames;
        } else {
            EXPECT_EQ(first->Stats.StaticRecipeCompiles, 0u);
            EXPECT_EQ(independent->Stats.StaticRecipeCompiles, 0u);
            EXPECT_EQ(independent->Stats.MaterialsRebuilt, 0u);
            if (Result.Frames >= 3) EXPECT_EQ(independent->Stats.PublishedPages, 0u);
            if (Result.Frames >= 11) EXPECT_EQ(first->Stats.PublishedPages, 0u);
            const bool numeric = Result.Frames == 3 || Result.Frames == 6 || Result.Frames == 8;
            EXPECT_EQ(first->Stats.MaterialsRebuilt, numeric ? 1u : 0u);
            if (numeric)
                ++Result.NumericFrames;
            else
                ++Result.StableFrames;
        }
        for (uint32_t previous = 0; previous < First.Frames.size(); ++previous) {
            if (previous == flight || !First.Frames[previous]) continue;
            EXPECT_FLOAT_EQ(NumericColor(First.Frames[previous]->Materials[0].Passes[0].NumericBytes), FlightColors[previous]);
        }
        FlightColors[flight] = ExpectedColor;
        Result.Flights[flight] = true;
        PreparedOrdinals[flight] = Result.Frames;
        ++Result.SharedPublications;
        ++Result.Frames;
    }
    void BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        First.BuildGraph(context, graph, outputs);
        Second.BuildGraph(context, graph, outputs);
        Independent.BuildGraph(context, graph, outputs);
    }
    void GraphRecorded(RenderPipelineContext& context, const RenderGraph&, RenderGraphExecutionResult result) override {
        EXPECT_TRUE(result.Success);
        const auto ordinal = PreparedOrdinals[context.FlightIndex()];
        // Closing the application may prepare an additional frame without recording it.
        // Require every requested publication to record exactly once before that drain.
        if (ordinal >= kRequestedPublicationFrames) return;
        EXPECT_TRUE(result.CommandsRecorded);
        EXPECT_FALSE(Result.RecordedRequestedFrames[ordinal]);
        Result.RecordedRequestedFrames[ordinal] = true;
        ++Result.Recorded;
    }
    SceneConsumerPipeline First, Second, Independent;
    Material& Edited;
    bool Different;
    ConsumerObservations& Result;
    float ExpectedColor{0};
    array<float, 3> FlightColors{};
    array<uint32_t, 3> PreparedOrdinals{};
};

class ScenePipelineHost final : public Application {
public:
    ScenePipelineHost(bool different, RenderValidationMode validation, ConsumerObservations& result)
        : Different(different), Validation(validation), Result(result) {}

protected:
    void OnInit() override {
        SetRenderGraphRuntimeOptions({Validation, RenderGraphReportMode::Counters, false});
        auto program = test::CompileStageBProgram(*GetDevice(), test::StageBMaterialSource());
        ASSERT_TRUE(program);
        Program = program.Release();
        auto technique = MaterialTechnique::Create({{"ForwardLit", Program.get(), "MaterialValues", {}}}, "ForwardLit");
        ASSERT_TRUE(technique);
        Technique = technique.Release();
        for (uint32_t i = 0; i < Materials.size(); ++i) {
            auto material = Material::Create(Technique.get());
            ASSERT_TRUE(material);
            Materials[i] = material.Release();
            ASSERT_TRUE(Materials[i]->SetFloat4("BaseColor", {i ? .25f : 1.f, 0, 0, 1}));
        }
        const float vertices[]{-1, -1, 0, 1, -1, 0, 0, 1, 0};
        const uint32_t indices[]{0, 1, 2};
        auto vertex = render::test::MakeUploadBuffer(*GetDevice(), std::as_bytes(std::span{vertices}), render::BufferUse::Vertex);
        auto index = render::test::MakeUploadBuffer(*GetDevice(), std::as_bytes(std::span{indices}), render::BufferUse::Index);
        ASSERT_TRUE(vertex && index);
        Vertices = vertex.Release();
        Indices = index.Release();
        Geometry.VertexBuffers = {{0, {Vertices.get(), 0, sizeof(vertices)}}};
        Geometry.Ibv = {Indices.get(), 0, 4};
        Geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
        Geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
        for (uint32_t i = 0; i < Scenes.size(); ++i) {
            Scenes[i] = make_unique<Scene>();
            Scenes[i]->AddPrimitive(make_unique<ConsumerPrimitive>(&Geometry, Materials[i].get()));
        }
        ASSERT_TRUE(GetRenderSystem()->SetPipeline(make_unique<SharedScenePipelines>(*Scenes[0], *Scenes[1], *Materials[0], Different, Result)));
        Result.Initialized = true;
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (!Result.Initialized || Result.Frames >= kRequestedPublicationFrames || ++Ticks > 25) {
            test::CloseMainWindow(*this);
            return;
        }
        if (Result.Frames == 3 || Result.Frames == 8)
            EXPECT_TRUE(Materials[0]->SetFloat4("BaseColor", {Result.Frames == 3 ? 2.f : 3.f, 0, 0, 1}));
    }
    void OnShutdown() override {
        if (GetRenderSystem()) GetRenderSystem()->SetPipeline(nullptr);
        for (auto& scene : Scenes) scene.reset();
        for (auto& material : Materials) material.reset();
        Technique.reset();
        Program.reset();
        Indices.reset();
        Vertices.reset();
    }

private:
    bool Different;
    RenderValidationMode Validation;
    ConsumerObservations& Result;
    uint32_t Ticks{0};
    unique_ptr<ShaderProgram> Program;
    unique_ptr<MaterialTechnique> Technique;
    array<unique_ptr<Material>, 2> Materials;
    unique_ptr<render::Buffer> Vertices, Indices;
    GpuMesh::DrawData Geometry;
    array<unique_ptr<Scene>, 2> Scenes;
};

class ScenePipelinePublicationTest : public testing::TestWithParam<std::tuple<render::RenderBackend, bool, RenderValidationMode>> {};
TEST_P(ScenePipelinePublicationTest, RenderSystemFreezesSharedScenesOnceBeforeEveryPipelineConsumer) {
    const auto [backend, different, validation] = GetParam();
    {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(backend, probe, true)) {
            if (render::test::SetupMustFail(probe.Status, render::test::RequiredBackend(backend))) FAIL() << probe.Reason;
            GTEST_SKIP() << probe.Reason;
        }
    }
    ConsumerObservations result;
    test::RuntimeLogCapture logs;
    ScenePipelineHost app{different, validation, result};
    ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = false, .WindowTitle = "Shared Scene publication", .WindowWidth = 64, .WindowHeight = 48, .BackBufferCount = 3, .FlightDataCount = 3, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::Immediate}), 0);
    EXPECT_TRUE(result.Initialized);
    EXPECT_GE(result.Frames, kRequestedPublicationFrames);
    EXPECT_EQ(result.Recorded, kRequestedPublicationFrames);
    EXPECT_TRUE(std::all_of(result.RecordedRequestedFrames.begin(), result.RecordedRequestedFrames.end(), [](bool recorded) { return recorded; }));
    EXPECT_EQ(result.SharedPublications, result.Frames);
    EXPECT_EQ(result.ColdFrames, 1u);
    EXPECT_EQ(result.NumericFrames, 3u);
    EXPECT_GE(result.StableFrames, 14u);
    for (const bool seen : result.Flights) EXPECT_TRUE(seen);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
INSTANTIATE_TEST_SUITE_P(BackendsAndPolicies, ScenePipelinePublicationTest,
                         testing::Combine(testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan), testing::Bool(),
                                          testing::Values(RenderValidationMode::Off, RenderValidationMode::Full)));

}  // namespace
}  // namespace radray
