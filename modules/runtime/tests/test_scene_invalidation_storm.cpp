#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "stage_b_test_support.h"
#include "upload_test_support.h"

#include <algorithm>
#include <coroutine>
#include <cstring>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/wait_frame.h>

namespace radray {
namespace {

constexpr uint32_t kStormPrimitives = 256, kStormMaterials = 64, kStormPending = 16;
constexpr uint32_t kStormRepeats = 64, kStormPauseBegin = 6, kStormResume = 18, kStormFrames = 36;
// Maximum touched pages in all three publications for the eight existing table page sizes.
constexpr uint32_t kStormMaxPendingPages = 3 * ((kStormPrimitives + kStormPending + 31) / 32 + (kStormPrimitives + 63) / 64 +
                                                kStormMaterials + (kStormPrimitives * 2 + 15) / 16 + (kStormPrimitives + kStormPending + 1 + 255) / 256 + 1 + 1);

class StormGate {
public:
    ~StormGate() { Resume(); }
    void Resume() {
        const auto handle = Handle;
        Handle = {};
        if (handle) handle.resume();
    }
    struct Awaiter {
        StormGate* Gate;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) const noexcept { Gate->Handle = handle; }
        void await_resume() const noexcept {}
    };
    Awaiter Wait() noexcept { return {this}; }

private:
    std::coroutine_handle<> Handle{};
};
class StormImmediateWait final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};

class StormPrimitive final : public PrimitiveSceneProxy {
public:
    StormPrimitive(const GpuMesh::DrawData* geometry, Material* material) : Geometry(geometry), DrawMaterial(material) {}
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {Geometry, 0, 3, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
    void SetMaterial(Material* material) {
        if (DrawMaterial == material) return;
        DrawMaterial = material;
        MarkRenderDirty(PrimitiveDirtyKind::MaterialAssignment);
    }

private:
    const GpuMesh::DrawData* Geometry;
    Material* DrawMaterial;
};

bool CompileStormPolicy(const StaticPassCompileInput& input, StaticPassCompileResult& output) {
    output.NormalState = input.Pass.PipelineState;
    output.MirroredState = output.NormalState;
    output.MirroredState.Primitive.FaceClockwise = OppositeFrontFace(output.NormalState.Primitive.FaceClockwise);
    output.Bindings = input.Bindings ? *input.Bindings : StaticBindingRecipe{};
    output.Bindings.Valid = true;
    output.Bindings.Buffers[1] = 0;
    output.Bindings.Groups[1] = 1;
    output.Bindings.GroupCount = 1;
    output.Bindings.GroupOrder[0] = 1;
    return true;
}

float StormNumeric(std::span<const byte> bytes) {
    if (bytes.size() < sizeof(float)) return -1;
    float value;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

struct StormResult {
    uint32_t Prepared{0}, Paused{0}, Resumed{0}, DetachedChecks{0}, PolicyEmptyChecks{0};
    uint64_t AuthoringEdits{0}, ResumeRecipeCompiles{0}, ResumeMaterialBytes{0}, ResumePublishedPages{0};
    uint64_t MaxDirtySlots{0}, MaxPendingMaterials{0}, PausedKnownBytes{0};
    array<bool, kStormFrames> Recorded{};
    array<bool, 3> Flights{};
    bool Initialized{false};
};

class StormWorld {
public:
    explicit StormWorld(render::Device& device) : Device(device) { Assets.SetWaitFrameProcessor(&Wait); }
    ~StormWorld() {
        Source.reset();
        for (auto& bank : Materials)
            for (auto& material : bank) material.reset();
        for (auto& material : PendingMaterials) material.reset();
        PendingTexture.Reset();
        Gate.Resume();
        Assets.Pump();
    }
    void Initialize() {
        for (uint32_t programIndex = 0; programIndex < Programs.size(); ++programIndex) {
            string source = test::StageBMaterialSource("float4 BaseColor;", 1, programIndex == 4);
            // Finite immutable program generations; bank changes switch real shader programs.
            const string expression = "MaterialValues.BaseColor.xxxx";
            const auto at = source.find(expression);
            ASSERT_NE(at, string::npos);
            source.insert(at + expression.size(), fmt::format(" * {}", programIndex + 1));
            auto program = test::CompileStageBProgram(Device, source);
            ASSERT_TRUE(program);
            Programs[programIndex] = program.Release();
            auto technique = MaterialTechnique::Create({{"Storm", Programs[programIndex].get(), "MaterialValues", {}}}, "Storm");
            ASSERT_TRUE(technique);
            Techniques[programIndex] = technique.Release();
        }
        for (uint32_t bank = 0; bank < Materials.size(); ++bank)
            for (uint32_t index = 0; index < kStormMaterials; ++index) {
                auto material = Material::Create(Techniques[bank * 2 + index % 2].get());
                ASSERT_TRUE(material);
                Materials[bank][index] = material.Release();
                Values[bank][index] = .1f + index * .001f;
                ASSERT_TRUE(Materials[bank][index]->SetFloat4("BaseColor", {Values[bank][index], 0, 0, 1}));
            }
        auto texture = TextureDevice.CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::Resource, {}});
        ASSERT_TRUE(texture);
        auto view = TextureDevice.CreateTextureView({texture.Get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, render::SubresourceRange::AllSub(), render::TextureViewUsage::Resource});
        ASSERT_TRUE(view);
        auto asset = make_unique<TextureAsset>(&TextureDevice, "storm pending texture", texture.Release(), view.Release());
        const AssetId id{0x60c0ffee, 0x2211, 0x3344, 0x55, 0x66, 0x77, 0x88, 0x91, 0x92, 0x93, 0x94};
        PendingTexture = Assets.Load<TextureAsset>({id, [](StormGate* gate, unique_ptr<TextureAsset> pending) -> task<AssetLoadResult> {
                                                        co_await gate->Wait();
                                                        co_return AssetLoadResult::Success(std::move(pending));
                                                    }(&Gate, std::move(asset)),
                                                    "storm Loading texture"});
        ASSERT_FALSE(PendingTexture.IsReady());
        for (auto& value : PendingMaterials) {
            auto material = Material::Create(Techniques[4].get());
            ASSERT_TRUE(material);
            value = material.Release();
            ASSERT_TRUE(value->SetFloat4("BaseColor", {1, 0, 0, 1}));
            ASSERT_TRUE(value->SetTexture("AlbedoTexture", PendingTexture));
            ASSERT_TRUE(value->SetSampler("LinearSampler", {}));
            ASSERT_TRUE(value->HasPendingResources());
        }
        const array<float, 9> positions{-1, -1, 0, 1, -1, 0, 0, 1, 0};
        const array<uint32_t, 3> indices{0, 1, 2};
        auto vertex = render::test::MakeUploadBuffer(Device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
        auto index = render::test::MakeUploadBuffer(Device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
        ASSERT_TRUE(vertex && index);
        Vertices = vertex.Release();
        Indices = index.Release();
        Geometry.VertexBuffers = {{0, {Vertices.get(), 0, sizeof(positions)}}};
        Geometry.Ibv = {Indices.get(), 0, 4};
        Geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
        Geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
        Source = make_unique<Scene>();
        for (uint32_t slot = 0; slot < kStormPrimitives; ++slot) {
            auto proxy = make_unique<StormPrimitive>(&Geometry, Materials[0][slot % kStormMaterials].get());
            auto transform = Eigen::Matrix4f::Identity().eval();
            transform(0, 3) = float(slot);
            proxy->SetLocalToWorld(transform);
            Proxies[slot] = proxy.get();
            ASSERT_TRUE(Source->AddPrimitive(std::move(proxy)));
            Ids[slot] = Source->GetPrimitiveId(Proxies[slot].Get());
            Translations[slot] = float(slot);
        }
        for (uint32_t slot = 0; slot < kStormPending; ++slot) {
            auto proxy = make_unique<StormPrimitive>(&Geometry, PendingMaterials[slot].get());
            PendingProxies[slot] = proxy.get();
            ASSERT_TRUE(Source->AddPrimitive(std::move(proxy)));
            PendingIds[slot] = Source->GetPrimitiveId(PendingProxies[slot].Get());
        }
        Initialized = true;
    }

    void EditPaused(uint32_t step, StormResult& result) {
        if (step == kStormPauseBegin) {
            FrozenDraws = Source->GetDrawStore().GetMemoryStats();
            for (uint32_t flight = 0; flight < Snapshots.size(); ++flight) {
                ASSERT_TRUE(Snapshots[flight]);
                FrozenEpochs[flight] = Snapshots[flight]->SceneEpoch;
            }
        }
        for (uint32_t repeat = 0; repeat < kStormRepeats; ++repeat) {
            for (uint32_t bank = 0; bank < Materials.size(); ++bank)
                for (uint32_t index = 0; index < kStormMaterials; ++index) {
                    const float value = .1f + step * .01f + index * .0001f + repeat * .000001f;
                    Values[bank][index] = value;
                    ASSERT_TRUE(Materials[bank][index]->SetFloat4("BaseColor", {value, 0, 0, 1}));
                    MaterialPipelineState state;
                    state.Primitive.FaceClockwise = repeat % 2 ? render::FrontFace::CW : render::FrontFace::CCW;
                    state.Primitive.Cull = repeat % 2 ? render::CullMode::None : render::CullMode::Back;
                    state.WriteMask = bank ? render::ColorWrite::Red : render::ColorWrite::All;
                    States[bank] = state;
                    ASSERT_TRUE(Materials[bank][index]->SetPassPipelineState("Storm", state));
                    result.AuthoringEdits += 2;
                }
            ActiveBank = (step + repeat + 1) % 2;
            for (uint32_t slot = 0; slot < kStormPrimitives; ++slot) {
                Proxies[slot]->SetMaterial(Materials[ActiveBank][slot % kStormMaterials].get());
                Translations[slot] = float(slot) + step * .01f + repeat * .0001f;
                auto transform = Eigen::Matrix4f::Identity().eval();
                transform(0, 3) = Translations[slot];
                Proxies[slot]->SetLocalToWorld(transform);
                result.AuthoringEdits += 2;
            }
            for (auto& material : PendingMaterials)
                if (material) {
                    ASSERT_TRUE(material->SetFloat4("BaseColor", {float(step + repeat + 1), 0, 0, 1}));
                    ++result.AuthoringEdits;
                }
            EXPECT_LE(Source->GetPendingRenderChangeCount(), kStormPrimitives + kStormPending);
            result.MaxDirtySlots = std::max<uint64_t>(result.MaxDirtySlots, Source->GetPendingRenderChangeCount());
            const auto memory = Source->GetRenderState().GetMemoryStats();
            EXPECT_LE(memory.PendingMaterials, kStormMaterials + kStormPending);
            result.MaxPendingMaterials = std::max(result.MaxPendingMaterials, memory.PendingMaterials);
            EXPECT_EQ(memory.WorkSlots, 0u);
            EXPECT_EQ(memory.PendingPages, 0u);
        }
        if (step == 8) {
            for (uint32_t slot = 0; slot < kStormPending; ++slot) {
                Source->RemovePrimitive(PendingProxies[slot].Get());
                PendingProxies[slot] = nullptr;
                EXPECT_FALSE(Source->FindPrimitive(PendingIds[slot]));
                if (slot % 2 == 0) PendingMaterials[slot].reset();
            }
        }
        if (step == 12) {
            Gate.Resume();
            Assets.Pump();
            ASSERT_TRUE(PendingTexture.IsReady());
        }
        EXPECT_EQ(Source->GetDrawStore().GetMemoryStats(), FrozenDraws);
        const auto memory = Source->GetRenderState().GetMemoryStats();
        EXPECT_EQ(memory.ObservedMaterials, kStormPending);
        EXPECT_EQ(memory.MaterialEntries, kStormMaterials + kStormPending);
        EXPECT_EQ(memory.Catalog.DependencyEdges, kStormPrimitives + kStormPending + kStormMaterials);
        EXPECT_EQ(memory.ProgramEntries, 2u);
        if (step == 8) {
            PlateauScene = Source->GetMemoryStats();
            PlateauCatalog = memory.Total();
            result.PausedKnownBytes = PlateauScene.KnownBytes() + PlateauCatalog.KnownBytes() + FrozenDraws.KnownBytes();
        }
        if (step > 8) {
            EXPECT_EQ(Source->GetMemoryStats(), PlateauScene);
            EXPECT_EQ(memory.Total(), PlateauCatalog);
        }
        for (uint32_t flight = 0; flight < Snapshots.size(); ++flight) {
            const auto& snapshot = *Snapshots[flight];
            EXPECT_EQ(snapshot.SceneEpoch, FrozenEpochs[flight]);
            ASSERT_EQ(snapshot.Primitives.size(), kStormPrimitives + kStormPending);
            for (uint32_t slot = 0; slot < kStormPrimitives; ++slot) {
                const auto primitive = std::find_if(snapshot.Primitives.begin(), snapshot.Primitives.end(), [&](const auto& value) { return value.Id == Ids[slot]; });
                ASSERT_NE(primitive, snapshot.Primitives.end());
                EXPECT_FLOAT_EQ(primitive->LocalToWorld(0, 3), float(slot));
                ASSERT_LT(primitive->FirstMeshBatch, snapshot.MeshBatches.size());
                const auto material = snapshot.MeshBatches[primitive->FirstMeshBatch].Material;
                ASSERT_LT(material, snapshot.Materials.size());
                EXPECT_FLOAT_EQ(StormNumeric(snapshot.Materials[material].Passes[0].NumericBytes), .1f + (slot % kStormMaterials) * .001f);
            }
        }
    }

    void CheckDetached(uint64_t serial, StormResult& result) {
        const auto before = Source->GetRenderState().GetMemoryStats();
        EXPECT_EQ(before.ObservedMaterials, 0u);
        EXPECT_EQ(before.PendingMaterials, 0u);
        for (auto& material : Materials[1 - ActiveBank]) {
            ASSERT_TRUE(material->SetFloat4("BaseColor", {99, 0, 0, 1}));
            material->GetRevisions(serial);
        }
        for (auto& material : PendingMaterials)
            if (material) {
                ASSERT_TRUE(material->SetFloat4("BaseColor", {99, 0, 0, 1}));
                material->GetRevisions(serial);
                EXPECT_FALSE(material->HasPendingResources());
            }
        EXPECT_EQ(Source->GetRenderState().GetMemoryStats().Total(), before.Total());
        EXPECT_EQ(Source->GetPendingRenderChangeCount(), 0u);
        for (auto& material : PendingMaterials) material.reset();
        PendingTexture.Reset();
        Assets.Pump();
        EXPECT_EQ(Assets.GetAssetCount(), 0u);
        EXPECT_EQ(TextureDevice.LiveTextures, 0);
        EXPECT_EQ(TextureDevice.LiveTextureViews, 0);
        ++result.DetachedChecks;
    }

    void CheckPublished(const RenderSceneSnapshot& snapshot, uint32_t step, uint32_t policies) const {
        ASSERT_TRUE(snapshot.Valid);
        ASSERT_EQ(snapshot.Materials.size(), kStormMaterials);
        EXPECT_EQ(snapshot.MeshBatches.size(), kStormPrimitives);
        EXPECT_EQ(snapshot.Primitives.size(), kStormPrimitives + (step < kStormResume ? kStormPending : 0));
        EXPECT_EQ(snapshot.DrawRecords.size(), kStormPrimitives * policies);
        for (uint32_t slot = 0; slot < kStormPrimitives; ++slot) {
            const auto primitive = std::find_if(snapshot.Primitives.begin(), snapshot.Primitives.end(), [&](const auto& value) { return value.Id == Ids[slot]; });
            ASSERT_NE(primitive, snapshot.Primitives.end());
            EXPECT_FLOAT_EQ(primitive->LocalToWorld(0, 3), Translations[slot]);
            ASSERT_LT(primitive->FirstMeshBatch, snapshot.MeshBatches.size());
            const auto materialIndex = snapshot.MeshBatches[primitive->FirstMeshBatch].Material;
            ASSERT_LT(materialIndex, snapshot.Materials.size());
            const auto& material = snapshot.Materials[materialIndex];
            ASSERT_EQ(material.Passes.size(), 1u);
            const auto logical = slot % kStormMaterials;
            EXPECT_EQ(material.Generation, Materials[ActiveBank][logical]->GetGeneration());
            EXPECT_EQ(material.Passes[0].Program.Get(), Programs[ActiveBank * 2 + logical % 2].get());
            EXPECT_FLOAT_EQ(StormNumeric(material.Passes[0].NumericBytes), Values[ActiveBank][logical]);
            EXPECT_EQ(material.Passes[0].PipelineState, States[ActiveBank]);
        }
        for (const auto& draw : snapshot.DrawRecords) {
            EXPECT_EQ(draw.Status, DrawRecordStatus::Ready);
            EXPECT_TRUE(draw.Description.LayoutId.IsValid());
            EXPECT_EQ(draw.Description.Geometry.Get(), &Geometry);
            EXPECT_EQ(draw.Description.IndexCount, 3u);
            EXPECT_GE(draw.Policy.Value, 6001u);
            EXPECT_LT(draw.Policy.Value, 6001u + policies);
        }
        const auto memory = Source->GetRenderState().GetMemoryStats();
        EXPECT_EQ(memory.PendingMaterials, 0u);
        EXPECT_EQ(memory.WorkSlots, 0u);
        EXPECT_LE(memory.PendingPages, kStormMaxPendingPages);
        EXPECT_LE(memory.MaterialEntries, kStormMaterials + (step < kStormResume ? kStormPending : 0));
        EXPECT_LE(memory.ProgramEntries, 2u);
        EXPECT_LE(memory.SharedFlightCount, 3u);
        EXPECT_LE(memory.LivePublications, 3u);
        EXPECT_EQ(memory.Catalog.DependencyEdges, kStormPrimitives + kStormMaterials + (step < kStormResume ? kStormPending : 0));
        const auto draws = Source->GetDrawStore().GetMemoryStats();
        EXPECT_LE(draws.DirtyEntries, kStormPrimitives * 4u + 8u);
        EXPECT_EQ(draws.DependencyEdges, kStormPrimitives * policies * 3u);
    }

    render::Device& Device;
    test::UploadTestDevice TextureDevice;
    StormImmediateWait Wait;
    AssetManager Assets;
    StormGate Gate;
    StreamingAssetRef<TextureAsset> PendingTexture;
    array<unique_ptr<ShaderProgram>, 5> Programs;
    array<unique_ptr<MaterialTechnique>, 5> Techniques;
    array<array<unique_ptr<Material>, kStormMaterials>, 2> Materials;
    array<unique_ptr<Material>, kStormPending> PendingMaterials;
    unique_ptr<render::Buffer> Vertices, Indices;
    GpuMesh::DrawData Geometry;
    unique_ptr<Scene> Source;
    array<Nullable<StormPrimitive*>, kStormPrimitives> Proxies;
    array<Nullable<StormPrimitive*>, kStormPending> PendingProxies;
    array<SceneObjectId, kStormPrimitives> Ids;
    array<SceneObjectId, kStormPending> PendingIds;
    array<array<float, kStormMaterials>, 2> Values{};
    array<MaterialPipelineState, 2> States;
    array<float, kStormPrimitives> Translations{};
    array<shared_ptr<const RenderSceneSnapshot>, 3> Snapshots;
    array<uint64_t, 3> FrozenEpochs{};
    RenderMemoryStats FrozenDraws, PlateauScene, PlateauCatalog;
    uint32_t ActiveBank{0};
    bool Initialized{false};
};

class StormPipeline final : public RenderPipeline {
public:
    StormPipeline(StormWorld& world, StormResult& result) : World(world), Result(result) {}
    void CollectScenePolicies(RenderPrepareContext& context) override {
        Step = Result.Prepared;
        if (Step >= kStormFrames) return;
        if (Step >= kStormPauseBegin && Step < kStormResume) {
            World.EditPaused(Step, Result);
            return;
        }
        if (Step == kStormResume + 1) World.CheckDetached(context.PrepareSerial, Result);
        ASSERT_TRUE(context.RegisterScene(*World.Source));
        PolicyCount = Step >= 24 && Step < 27 ? 0 : Step >= kStormResume && Step < 21 ? 1
                                                                                      : 2;
        for (uint32_t policy = 0; policy < PolicyCount; ++policy) {
            const PassPolicy value{{6001u + policy}, 1, "Storm", CompileStormPolicy};
            ASSERT_TRUE(context.RegisterScenePolicy(*World.Source, value));
            ASSERT_TRUE(context.RegisterScenePolicy(*World.Source, value));
        }
        EXPECT_EQ(context.ScenePolicies.size(), PolicyCount);
    }
    void PrepareFrame(RenderPrepareContext& context) override {
        ASSERT_TRUE(context.ScenesFrozen);
        const auto flight = context.App.FlightIndex;
        Ordinals[flight] = Step;
        Result.Flights[flight] = true;
        ++Result.Prepared;
        if (Step >= kStormFrames) return;
        if (Step >= kStormPauseBegin && Step < kStormResume) {
            EXPECT_TRUE(context.RegisteredScenes.empty());
            EXPECT_FALSE(context.PrepareScene(*World.Source));
            ++Result.Paused;
            return;
        }
        auto snapshot = context.PrepareScene(*World.Source);
        ASSERT_TRUE(snapshot);
        World.Snapshots[flight] = snapshot.Release();
        const auto& value = *World.Snapshots[flight];
        World.CheckPublished(value, Step, PolicyCount);
        EXPECT_EQ(value.Stats.SceneCommits, 1u);
        EXPECT_EQ(value.Stats.SnapshotPublications, 1u);
        const auto publication = value.PublicationRevision;
        const auto memory = World.Source->GetRenderState().GetMemoryStats();
        const auto repeated = context.PrepareScene(*World.Source);
        ASSERT_TRUE(repeated);
        EXPECT_EQ(repeated.Get(), &value);
        EXPECT_EQ(repeated->PublicationRevision, publication);
        EXPECT_EQ(World.Source->GetRenderState().GetMemoryStats().Total(), memory.Total());
        if (Step == kStormResume) {
            EXPECT_EQ(World.ActiveBank, 1u);
            EXPECT_EQ(value.Stats.MaterialsRebuilt, kStormMaterials);
            EXPECT_EQ(value.Stats.PrimitiveBoundsRebuilt, kStormPrimitives);
            EXPECT_EQ(value.Stats.StaticRecipeCompiles, kStormPrimitives);
            EXPECT_EQ(memory.ObservedMaterials, 0u);
            EXPECT_EQ(memory.MaterialEntries, kStormMaterials);
            EXPECT_GT(value.Stats.MaterialBytesCopied, 0u);
            Result.ResumeRecipeCompiles = value.Stats.StaticRecipeCompiles;
            Result.ResumeMaterialBytes = value.Stats.MaterialBytesCopied;
            Result.ResumePublishedPages = value.Stats.PublishedPages;
            ++Result.Resumed;
        } else if (Step > kStormResume) {
            EXPECT_EQ(value.Stats.MaterialsRebuilt, 0u);
            EXPECT_EQ(value.Stats.PrimitiveBoundsRebuilt, 0u);
            EXPECT_EQ(value.Stats.PendingResourcesObserved, 0u);
            if (Step >= 30) {
                EXPECT_EQ(value.Stats.PublishedPages, 0u);
                EXPECT_EQ(value.Stats.StaticRecipeCompiles, 0u);
            }
        }
        if (PolicyCount == 0) {
            EXPECT_EQ(World.Source->GetDrawStore().GetMemoryStats().DependencyEdges, 0u);
            ++Result.PolicyEmptyChecks;
        }
        context.Workloads.AddPresentationOutputs();
    }
    void BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        const auto ordinal = Ordinals[context.FlightIndex()];
        if (ordinal >= kStormFrames || (ordinal >= kStormPauseBegin && ordinal < kStormResume)) {
            EXPECT_TRUE(outputs.empty());
            return;
        }
        ASSERT_FALSE(outputs.empty());
        const auto& snapshot = World.Snapshots[context.FlightIndex()];
        ASSERT_TRUE(snapshot);
        ASSERT_FALSE(snapshot->Materials.empty());
        const float color = StormNumeric(snapshot->Materials.front().Passes[0].NumericBytes);
        struct Clear {};
        for (auto& output : outputs) {
            output.Texture = graph.NextVersion(output.Texture);
            graph.AddRasterPass<Clear>("storm frozen output", [=, target = output.Texture](Clear&, RenderGraphRasterBuilder& pass) { pass.SetColorAttachment(0, target, {.Clear = {{color, 0, 0, 1}}}); }, +[](const Clear&, RenderGraphRasterContext&) {});
        }
    }
    void GraphRecorded(RenderPipelineContext& context, const RenderGraph&, RenderGraphExecutionResult result) override {
        EXPECT_TRUE(result.Success);
        const auto ordinal = Ordinals[context.FlightIndex()];
        if (ordinal >= kStormFrames || (ordinal >= kStormPauseBegin && ordinal < kStormResume)) return;
        EXPECT_TRUE(result.CommandsRecorded);
        EXPECT_FALSE(Result.Recorded[ordinal]);
        Result.Recorded[ordinal] = true;
    }

private:
    StormWorld& World;
    StormResult& Result;
    uint32_t Step{0}, PolicyCount{2};
    array<uint32_t, 3> Ordinals{};
};

class StormApplication final : public Application {
public:
    explicit StormApplication(StormResult& result) : Result(result) {}

protected:
    void OnInit() override {
        SetRenderGraphRuntimeOptions({RenderValidationMode::Full, RenderGraphReportMode::Counters, false});
        World = make_unique<StormWorld>(*GetDevice());
        World->Initialize();
        ASSERT_TRUE(World->Initialized);
        ASSERT_TRUE(GetRenderSystem()->SetPipeline(make_unique<StormPipeline>(*World, Result)));
        Result.Initialized = true;
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (!Result.Initialized || Result.Recorded.back() || ++Ticks > kStormFrames + 12) test::CloseMainWindow(*this);
    }
    void OnShutdown() override {
        if (GetRenderSystem()) GetRenderSystem()->SetPipeline(nullptr);
        World.reset();
    }

private:
    StormResult& Result;
    unique_ptr<StormWorld> World;
    uint32_t Ticks{0};
};

class SceneInvalidationStormTest : public testing::TestWithParam<render::RenderBackend> {};
TEST_P(SceneInvalidationStormTest, PausedOutputsCoalesceFiniteEditsRetirePendingDependenciesAndResumeLatestValues) {
    {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(GetParam(), probe, true)) {
            if (render::test::SetupMustFail(probe.Status, render::test::RequiredBackend(GetParam()))) FAIL() << probe.Reason;
            GTEST_SKIP() << probe.Reason;
        }
    }
    StormResult result;
    test::RuntimeLogCapture logs;
    StormApplication app{result};
    ASSERT_EQ(app.Run({.Backend = GetParam(), .EnableValidation = true, .Multithreaded = false, .WindowTitle = "Scene invalidation storm", .WindowWidth = 64, .WindowHeight = 48, .BackBufferCount = 3, .FlightDataCount = 3, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::Immediate}), 0);
    EXPECT_TRUE(result.Initialized);
    EXPECT_EQ(result.Paused, kStormResume - kStormPauseBegin);
    EXPECT_EQ(result.Resumed, 1u);
    EXPECT_EQ(result.DetachedChecks, 1u);
    EXPECT_EQ(result.PolicyEmptyChecks, 3u);
    EXPECT_GT(result.AuthoringEdits, 500000u);
    EXPECT_EQ(result.MaxPendingMaterials, kStormMaterials + kStormPending);
    EXPECT_GE(result.MaxDirtySlots, kStormPrimitives);
    for (const bool flight : result.Flights) EXPECT_TRUE(flight);
    for (uint32_t ordinal = 0; ordinal < kStormFrames; ++ordinal)
        if (ordinal < kStormPauseBegin || ordinal >= kStormResume) EXPECT_TRUE(result.Recorded[ordinal]) << ordinal;
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    fmt::print("SCENE_STORM edits={} paused_frames={} max_dirty_slots={} max_pending_materials={} paused_known_container_bytes={} resumed_recipe_compiles={} resumed_material_bytes={} resumed_pages={} native_output=clear program_changes=finite_generation_assignment source_hot_reload=not_covered mesh_pixels=not_covered async_race_sanitizer=external_run_required\n",
               result.AuthoringEdits, result.Paused, result.MaxDirtySlots, result.MaxPendingMaterials, result.PausedKnownBytes, result.ResumeRecipeCompiles, result.ResumeMaterialBytes, result.ResumePublishedPages);
}
INSTANTIATE_TEST_SUITE_P(Backends, SceneInvalidationStormTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
