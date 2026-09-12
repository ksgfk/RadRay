#include "gpu_test_fixture.h"
#include "upload_test_support.h"
#if defined(RADRAY_ENABLE_SHADER_JIT)
#include "stage_b_test_support.h"
#endif

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstring>
#include <new>

#include <gtest/gtest.h>
#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/wait_frame.h>

namespace radray {
namespace {

class CacheProxy : public PrimitiveSceneProxy {
public:
    uint64_t Revision{1};
    bool Versioned{true};
    bool Ready{true};
    uint32_t SectionCount{1};
    AxisAlignedBounds Bounds{Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()};
    GpuMesh::DrawData Geometry;
    Nullable<Material*> DrawMaterial{nullptr};
    mutable uint32_t BoundsReads{0}, DrawReads{0}, TransformReads{0};

    uint64_t GetRenderDataRevision() const noexcept override { return Versioned ? Revision : 0; }
    uint64_t GetTransformRevision() const noexcept override { return Versioned ? GetLocalToWorldRevision() : 0; }
    AxisAlignedBounds GetLocalBounds() const noexcept override {
        ++BoundsReads;
        return Bounds;
    }
    Eigen::Matrix4f GetLocalToWorld() const noexcept override {
        ++TransformReads;
        return PrimitiveSceneProxy::GetLocalToWorld();
    }
    uint32_t GetSectionCount() const noexcept override { return SectionCount; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override {
        ++DrawReads;
        return {Ready ? &Geometry : nullptr, 0, 3, 0};
    }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
};

class ReusedAddressProxy final : public CacheProxy {
public:
    static void* operator new(size_t, void* storage) noexcept { return storage; }
    static void operator delete(void*) noexcept {}
    static void operator delete(void*, void*) noexcept {}
};

class ImmediateWait final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};

class ManualGate {
public:
    ~ManualGate() { Resume(); }
    void Resume() {
        const auto handle = _handle;
        _handle = {};
        if (handle) handle.resume();
    }
    struct Awaiter {
        ManualGate* Gate;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) const noexcept { Gate->_handle = handle; }
        void await_resume() const noexcept {}
    };
    Awaiter Wait() noexcept { return {this}; }

private:
    std::coroutine_handle<> _handle{};
};

TEST(SceneSnapshotCache, StructureTransformAndMotionRevisionsAreIndependentAcrossFlights) {
    Scene scene;
    auto proxy = make_unique<CacheProxy>();
    auto* raw = proxy.get();
    scene.AddPrimitive(std::move(proxy));
    RenderSceneSnapshotBuilder builder;
    array<RenderSceneSnapshot, 2> flights;
    vector<StreamingAssetRefAny> retained;
    ASSERT_TRUE(builder.Build(scene, flights[0], retained));
    ASSERT_EQ(flights[0].Stats.PrimitiveStructuresRebuilt, 1u);
    ASSERT_EQ(flights[0].Stats.PrimitiveBoundsRebuilt, 1u);
    ASSERT_TRUE(builder.Build(scene, flights[1], retained));
    EXPECT_EQ(flights[1].Stats.PrimitiveStructuresReused, 1u);
    EXPECT_EQ(flights[1].Stats.PrimitiveBoundsRebuilt, 0u);
    EXPECT_GT(flights[1].Stats.PublishedBytes, 0u);
    EXPECT_EQ(flights[1].ChangedPrimitiveRanges.size(), 1u);
    EXPECT_EQ(raw->DrawReads, 1u);
    EXPECT_EQ(raw->BoundsReads, 1u);
    EXPECT_EQ(raw->TransformReads, 1u);

    const auto oldGeneration = flights[0].Primitives[0].Generation;
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 3) = 5;
    raw->SetLocalToWorld(transform);
    ASSERT_TRUE(builder.Build(scene, flights[1], retained));
    EXPECT_EQ(flights[1].Stats.PrimitiveStructuresReused, 1u);
    EXPECT_EQ(flights[1].Stats.PrimitiveBoundsRebuilt, 1u);
    EXPECT_EQ(flights[1].Primitives[0].Generation, oldGeneration);
    EXPECT_TRUE(flights[1].Primitives[0].WorldBounds.Min.isApprox(Eigen::Vector3f{5, 0, 0}));
    EXPECT_TRUE(flights[0].Primitives[0].LocalToWorld.isIdentity());
    EXPECT_TRUE(flights[0].Primitives[0].WorldBounds.Min.isZero());
    const auto transformRevision = flights[1].Primitives[0].TransformRevision;
    raw->SetLocalToWorld(transform);
    raw->ResetMotion();
    ASSERT_TRUE(builder.Build(scene, flights[1], retained));
    EXPECT_EQ(flights[1].Stats.PrimitiveBoundsReused, 1u);
    EXPECT_EQ(flights[1].Primitives[0].TransformRevision, transformRevision);
    EXPECT_NE(flights[1].Primitives[0].MotionRevision, flights[0].Primitives[0].MotionRevision);

    raw->Bounds.Max = Eigen::Vector3f::Constant(2);
    raw->Ready = false;
    ++raw->Revision;
    ASSERT_TRUE(builder.Build(scene, flights[1], retained));
    EXPECT_EQ(flights[1].Stats.PrimitiveStructuresRebuilt, 1u);
    EXPECT_EQ(flights[1].Stats.PrimitiveBoundsRebuilt, 1u);
    EXPECT_EQ(flights[1].Stats.MissingGeometry, 1u);
    EXPECT_TRUE(flights[1].Primitives[0].WorldBounds.Max.isApprox(Eigen::Vector3f{7, 2, 2}));
    EXPECT_TRUE(flights[0].Primitives[0].WorldBounds.Max.isApprox(Eigen::Vector3f::Ones()));
}

TEST(SceneSnapshotCache, DenseSlotsPreserveUnchangedStructuresAfterRemovalAndInsertion) {
    Scene scene;
    auto addProxy = [&](uint32_t sections, float translation) {
        auto proxy = make_unique<CacheProxy>();
        auto* raw = proxy.get();
        raw->SectionCount = sections;
        Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
        transform(0, 3) = translation;
        raw->SetLocalToWorld(transform);
        scene.AddPrimitive(std::move(proxy));
        return raw;
    };
    auto* removed = addProxy(1, 10);
    auto* first = addProxy(2, 20);
    auto* second = addProxy(3, 30);
    const uint64_t removedGeneration = removed->GetGeneration();
    RenderSceneSnapshotBuilder builder;
    array<RenderSceneSnapshot, 2> flights;
    vector<StreamingAssetRefAny> retained;
    ASSERT_TRUE(builder.Build(scene, flights[0], retained));
    EXPECT_EQ(flights[0].Stats.InputSections, 6u);
    ASSERT_TRUE(builder.Build(scene, flights[1], retained));
    EXPECT_EQ(flights[1].Stats.PrimitiveStructuresReused, 3u);

    scene.RemovePrimitive(removed);
    auto* appended = addProxy(0, 40);
    ASSERT_TRUE(builder.Build(scene, flights[0], retained));
    ASSERT_EQ(flights[0].Primitives.size(), 3u);
    EXPECT_EQ(flights[0].Stats.PrimitiveStructuresReused, 2u);
    EXPECT_EQ(flights[0].Stats.PrimitiveStructuresRebuilt, 1u);
    EXPECT_EQ(flights[0].Stats.PrimitiveBoundsRebuilt, 1u);
    EXPECT_GT(flights[0].Stats.PublishedBytes, 0u);
    EXPECT_EQ(flights[0].Stats.InputSections, 5u);
    EXPECT_EQ(first->DrawReads, 2u);
    EXPECT_EQ(second->DrawReads, 3u);
    EXPECT_EQ(flights[0].Primitives[0].Generation, first->GetGeneration());
    EXPECT_EQ(flights[0].Primitives[1].Generation, second->GetGeneration());
    EXPECT_EQ(flights[0].Primitives[2].Generation, appended->GetGeneration());
    EXPECT_TRUE(flights[0].Primitives[0].WorldBounds.Min.isApprox(Eigen::Vector3f{20, 0, 0}));
    EXPECT_TRUE(flights[0].Primitives[1].WorldBounds.Min.isApprox(Eigen::Vector3f{30, 0, 0}));
    EXPECT_EQ(flights[1].Primitives[0].Generation, removedGeneration);
    EXPECT_TRUE(flights[1].Primitives[0].WorldBounds.Min.isApprox(Eigen::Vector3f{10, 0, 0}));

    ASSERT_TRUE(builder.Build(scene, flights[0], retained));
    EXPECT_EQ(flights[0].Stats.PrimitiveStructuresReused, 3u);
    EXPECT_EQ(flights[0].Stats.PrimitiveBoundsReused, 3u);
    EXPECT_EQ(flights[0].Stats.ScratchEntriesCreated, 0u);
    ASSERT_TRUE(builder.Build(scene, flights[1], retained));
    EXPECT_EQ(flights[1].Stats.PrimitiveStructuresReused, 3u);
    EXPECT_EQ(flights[1].Stats.PrimitiveBoundsRebuilt, 0u);
    EXPECT_GT(flights[1].Stats.PublishedBytes, 0u);

    second->SectionCount = 1;
    second->Bounds.Max = Eigen::Vector3f::Constant(4);
    ++second->Revision;
    ASSERT_TRUE(builder.Build(scene, flights[0], retained));
    EXPECT_EQ(flights[0].Stats.InputSections, 3u);
    EXPECT_EQ(flights[0].Stats.PrimitiveStructuresRebuilt, 1u);
    EXPECT_EQ(second->DrawReads, 4u);
    EXPECT_TRUE(flights[1].Primitives[1].WorldBounds.Max.isApprox(Eigen::Vector3f{31, 1, 1}));
    ASSERT_TRUE(builder.Build(scene, flights[1], retained));
    EXPECT_EQ(flights[1].Stats.PrimitiveStructuresReused, 3u);
    EXPECT_EQ(flights[1].Stats.PrimitiveBoundsRebuilt, 0u);
    EXPECT_GT(flights[1].Stats.PublishedBytes, 0u);
    EXPECT_TRUE(flights[1].Primitives[1].WorldBounds.Max.isApprox(Eigen::Vector3f{34, 4, 4}));

    scene.RemovePrimitive(second);
    ASSERT_TRUE(builder.Build(scene, flights[0], retained));
    ASSERT_EQ(flights[0].Primitives.size(), 2u);
    EXPECT_EQ(flights[0].Stats.PrimitiveStructuresReused, 2u);
    EXPECT_EQ(flights[0].Primitives[1].Generation, appended->GetGeneration());
    flights[0].ResetForReuse();
    ASSERT_TRUE(builder.Build(scene, flights[0], retained));
    EXPECT_EQ(flights[0].Stats.PrimitiveStructuresReused, 2u);
    EXPECT_EQ(flights[0].Stats.PrimitiveBoundsRebuilt, 0u);
    EXPECT_GT(flights[0].Stats.PublishedBytes, 0u);
}

TEST(SceneSnapshotCache, UnknownProxyRevisionsRemainConservative) {
    Scene scene;
    auto proxy = make_unique<CacheProxy>();
    auto* raw = proxy.get();
    raw->Versioned = false;
    scene.AddPrimitive(std::move(proxy));
    RenderSceneSnapshotBuilder builder;
    RenderSceneSnapshot snapshot;
    vector<StreamingAssetRefAny> retained;
    ASSERT_TRUE(builder.Build(scene, snapshot, retained));
    raw->Ready = false;
    raw->Bounds.Max = Eigen::Vector3f::Constant(3);
    ASSERT_TRUE(builder.Build(scene, snapshot, retained));
    EXPECT_EQ(snapshot.Stats.PrimitiveStructuresRebuilt, 1u);
    EXPECT_EQ(snapshot.Stats.PrimitiveBoundsRebuilt, 1u);
    EXPECT_EQ(snapshot.Stats.MissingGeometry, 1u);
    EXPECT_EQ(raw->DrawReads, 2u);
    EXPECT_TRUE(snapshot.Primitives[0].WorldBounds.Max.isApprox(Eigen::Vector3f::Constant(3)));
}

TEST(SceneSnapshotCache, ReplacingAProxyAtTheSameAddressDoesNotReuseItsOldGeneration) {
    alignas(ReusedAddressProxy) array<byte, sizeof(ReusedAddressProxy)> storage;
    Scene scene;
    auto* first = new (storage.data()) ReusedAddressProxy;
    scene.AddPrimitive(unique_ptr<PrimitiveSceneProxy>{first});
    RenderSceneSnapshotBuilder builder;
    RenderSceneSnapshot oldFlight, newFlight;
    vector<StreamingAssetRefAny> retained;
    ASSERT_TRUE(builder.Build(scene, oldFlight, retained));
    const uint64_t generation = first->GetGeneration();
    scene.RemovePrimitive(first);
    auto* replacement = new (storage.data()) ReusedAddressProxy;
    replacement->Bounds.Max = Eigen::Vector3f::Constant(4);
    replacement->Ready = false;
    scene.AddPrimitive(unique_ptr<PrimitiveSceneProxy>{replacement});
    ASSERT_EQ(static_cast<void*>(replacement), static_cast<void*>(first));
    ASSERT_NE(replacement->GetGeneration(), generation);
    ASSERT_TRUE(builder.Build(scene, newFlight, retained));
    EXPECT_EQ(newFlight.Stats.PrimitiveStructuresRebuilt, 1u);
    EXPECT_EQ(newFlight.Stats.MissingGeometry, 1u);
    EXPECT_EQ(replacement->DrawReads, 1u);
    EXPECT_EQ(oldFlight.Primitives[0].Generation, generation);
    EXPECT_TRUE(oldFlight.Primitives[0].WorldBounds.Max.isApprox(Eigen::Vector3f::Ones()));
    scene.RemovePrimitive(replacement);
    ASSERT_TRUE(builder.Build(scene, newFlight, retained));
    EXPECT_TRUE(newFlight.Primitives.empty());
    EXPECT_TRUE(newFlight.MeshBatches.empty());
}

TEST(SceneSnapshotCache, StaticMeshReadinessInvalidatesCachedEmptyGeometryAndRetainsPayload) {
    ImmediateWait wait;
    AssetManager assets;
    assets.SetWaitFrameProcessor(&wait);
    ManualGate gate;
    auto mesh = assets.Load<StaticMesh>({test::kUploadTestId,
                                         [](ManualGate* pending) -> task<AssetLoadResult> {
                                             co_await pending->Wait();
                                             GpuMesh gpu;
                                             gpu.Draws.emplace_back();
                                             co_return AssetLoadResult::Success(make_unique<StaticMesh>(test::MakeUploadTestMesh(),
                                                                                                        vector<StaticMeshSection>{{0, 0, 3, 0, 2}}, Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), std::move(gpu)));
                                         }(&gate),
                                         "snapshot pending mesh"});
    Scene scene;
    auto* proxy = scene.AddPrimitive(make_unique<StaticMeshSceneProxy>(mesh,
                                                                       vector<Nullable<Material*>>{}, Eigen::Matrix4f::Identity()))
                      .Get();
    RenderSceneSnapshotBuilder builder;
    RenderSceneSnapshot before, after;
    vector<StreamingAssetRefAny> retained;
    ASSERT_TRUE(builder.Build(scene, before, retained));
    EXPECT_EQ(before.Stats.InputSections, 0u);
    retained.clear();
    gate.Resume();
    assets.Pump();
    ASSERT_TRUE(mesh.IsReady());
    ASSERT_TRUE(builder.Build(scene, after, retained));
    EXPECT_EQ(after.Stats.PrimitiveStructuresRebuilt, 1u);
    EXPECT_EQ(after.Stats.InputSections, 1u);
    EXPECT_NE(after.Primitives[0].RenderDataRevision, before.Primitives[0].RenderDataRevision);
    EXPECT_TRUE(after.Primitives[0].WorldBounds.IsFiniteValid());
    EXPECT_FALSE(before.Primitives[0].WorldBounds.IsFiniteValid());
    scene.RemovePrimitive(proxy);
    mesh.Reset();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 1u);
    retained.clear();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 0u);
    // The stale cache owns no asset and is discarded without dereferencing its former geometry.
    ASSERT_TRUE(builder.Build(scene, after, retained));
    EXPECT_TRUE(after.Primitives.empty());
}

class MaterialSnapshotCache : public testing::TestWithParam<render::RenderBackend> {
protected:
    void SetUp() override {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
        GTEST_SKIP() << "Shader JIT disabled";
#else
        if (!render::test::TryCreateDevice(GetParam(), Device, true)) GTEST_SKIP() << "Backend unavailable";
        auto program = test::CompileStageBProgram(*Device.Device, test::StageBMaterialSource());
        ASSERT_TRUE(program);
        Program = program.Release();
        auto technique = MaterialTechnique::Create({{"ForwardLit", Program.get(), "MaterialValues", {}}}, "ForwardLit");
        ASSERT_TRUE(technique);
        Technique = technique.Release();
#endif
    }
    void TearDown() override { EXPECT_EQ(Device.ValidationErrors.load(), 0u); }
    render::test::DeviceContext Device;
    unique_ptr<ShaderProgram> Program;
    unique_ptr<MaterialTechnique> Technique;
};

TEST_P(MaterialSnapshotCache, UnchangedAndFailedWritesReuseBytesWhileFlightsKeepIndependentValues) {
    auto material = Material::Create(Technique.get());
    const Eigen::Vector4f first{1, 2, 3, 4}, changed{5, 6, 7, 8};
    ASSERT_TRUE(material->SetFloat4("BaseColor", first));
    MaterialRenderData a, b;
    vector<StreamingAssetRefAny> retained;
    uint64_t bytes = 0;
    ASSERT_TRUE(material->BuildRenderData(a, retained, &bytes));
    EXPECT_GT(bytes, 0u);
    ASSERT_TRUE(material->BuildRenderData(b, retained, &bytes));
    EXPECT_GT(bytes, 0u);
    const uint64_t revision = a.Revision;
    ASSERT_TRUE(material->SetFloat4("BaseColor", first));
    EXPECT_FALSE(material->SetFloat("BaseColor", 2));
    EXPECT_FALSE(material->SetFloat4("BaseColor", first, 1));
    EXPECT_FALSE(material->SetFloat4("Unknown", changed));
    ASSERT_TRUE(material->BuildRenderData(a, retained, &bytes));
    EXPECT_EQ(bytes, 0u);
    EXPECT_EQ(a.Revision, revision);
    a.Passes[0].NumericBytes.clear();
    a.Invalidate();
    ASSERT_TRUE(material->BuildRenderData(a, retained, &bytes));
    EXPECT_GT(bytes, 0u);
    const auto& old = a.Passes[0].NumericBytes;
    const vector<byte> expected(old.begin(), old.end());
    ASSERT_TRUE(material->SetFloat4("BaseColor", changed));
    ASSERT_TRUE(material->BuildRenderData(b, retained, &bytes));
    EXPECT_GT(bytes, 0u);
    EXPECT_GT(b.Revision, a.Revision);
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), old.begin()));
    const auto& updated = b.Passes[0].NumericBytes;
    EXPECT_NE(std::memcmp(updated.data(), old.data(), old.size()), 0);

    auto& exposedState = material->GetPipelineState();
    ASSERT_TRUE(material->BuildRenderData(b, retained, &bytes));
    exposedState.Primitive.Cull = render::CullMode::None;
    material->SetRenderQueue(RenderQueue::Transparent);
    ASSERT_TRUE(material->BuildRenderData(b, retained, &bytes));
    EXPECT_EQ(b.Passes[0].PipelineState.Primitive.Cull, render::CullMode::None);
    EXPECT_EQ(b.Queue, RenderQueue::Transparent);
    EXPECT_NE(a.Queue, b.Queue);
    EXPECT_NE(a.Passes[0].PipelineState.Primitive.Cull, b.Passes[0].PipelineState.Primitive.Cull);
}

TEST_P(MaterialSnapshotCache, SceneMaterialsReusePerFlightAndReplacementsHaveFreshIdentities) {
    auto material = Material::Create(Technique.get());
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    Scene scene;
    auto proxy = make_unique<CacheProxy>();
    auto* raw = proxy.get();
    raw->DrawMaterial = material.Get();
    scene.AddPrimitive(std::move(proxy));
    RenderSceneSnapshotBuilder builder;
    array<RenderSceneSnapshot, 2> flights;
    vector<StreamingAssetRefAny> retained;
    for (size_t index = 0; index < flights.size(); ++index) {
        auto& flight = flights[index];
        ASSERT_TRUE(builder.Build(scene, flight, retained));
        EXPECT_EQ(flight.Stats.MaterialsRebuilt, index == 0 ? 1u : 0u);
        EXPECT_EQ(flight.Stats.MaterialsReused, index == 0 ? 0u : 1u);
        if (index == 0)
            EXPECT_GT(flight.Stats.MaterialBytesCopied, 0u);
        else
            EXPECT_EQ(flight.Stats.MaterialBytesCopied, 0u);
        EXPECT_GT(flight.Stats.PublishedMaterialBytes, 0u);
    }
    for (auto& flight : flights) {
        ASSERT_TRUE(builder.Build(scene, flight, retained));
        EXPECT_EQ(flight.Stats.MaterialsReused, 1u);
        EXPECT_EQ(flight.Stats.MaterialBytesCopied, 0u);
        EXPECT_EQ(flight.Stats.PublishedMaterialBytes, 0u);
        EXPECT_EQ(flight.Stats.PrimitiveBoundsReused, 1u);
    }
    const uint64_t generation = material->GetGeneration();
    material = nullptr;
    material = Material::Create(Technique.get());
    ASSERT_NE(material->GetGeneration(), generation);
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Constant(2)));
    raw->DrawMaterial = material.Get();
    ASSERT_TRUE(builder.Build(scene, flights[1], retained));
    EXPECT_EQ(flights[1].Stats.MaterialsRebuilt, 1u);
    EXPECT_GT(flights[1].Stats.MaterialBytesCopied, 0u);
    EXPECT_EQ(flights[0].Materials[0].Generation, generation);
    EXPECT_EQ(flights[1].Materials[0].Generation, material->GetGeneration());
}

TEST_P(MaterialSnapshotCache, MovedFromAndInvalidatedSnapshotsCanBeMaterializedAgain) {
    auto material = Material::Create(Technique.get());
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    MaterialRenderData original;
    vector<StreamingAssetRefAny> retained;
    uint64_t bytes = 0;
    ASSERT_TRUE(material->BuildRenderData(original, retained, &bytes));
    MaterialRenderData moved = std::move(original);
    ASSERT_TRUE(material->BuildRenderData(original, retained, &bytes));
    EXPECT_GT(bytes, 0u);
    ASSERT_EQ(original.Passes.size(), moved.Passes.size());
    EXPECT_EQ(original.Generation, moved.Generation);
    const vector<byte> expected = moved.Passes[0].NumericBytes;
    const vector<byte> restored = original.Passes[0].NumericBytes;
    EXPECT_EQ(expected, restored);

    // An invalidated externally edited snapshot must refill its bytes from the authoring material.
    moved.Invalidate();
    moved.Passes[0].NumericBytes.clear();
    const uint64_t generation = material->GetGeneration();
    material = nullptr;
    material = Material::Create(Technique.get());
    ASSERT_NE(material->GetGeneration(), generation);
    ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f::Constant(3)));
    ASSERT_TRUE(material->BuildRenderData(moved, retained, &bytes));
    EXPECT_GT(bytes, 0u);
    EXPECT_EQ(moved.Generation, material->GetGeneration());
    EXPECT_EQ(moved.Passes[0].NumericBytes.size(), expected.size());
    EXPECT_NE(std::memcmp(moved.Passes[0].NumericBytes.data(), restored.data(), restored.size()), 0);
}

TEST_P(MaterialSnapshotCache, TextureReadinessReplacementAndSamplerChangesInvalidateMaterialValues) {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
    GTEST_SKIP() << "Shader JIT disabled";
#else
    auto program = test::CompileStageBProgram(*Device.Device, test::StageBMaterialSource("float4 BaseColor;", 1, true));
    ASSERT_TRUE(program);
    auto technique = MaterialTechnique::Create({{"ForwardLit", program.Get(), "MaterialValues", {}}}, "ForwardLit");
    ASSERT_TRUE(technique);
    test::UploadTestDevice textureDevice;
    ImmediateWait wait;
    AssetManager assets;
    assets.SetWaitFrameProcessor(&wait);
    ManualGate gate;
    const auto makeTexture = [&]() {
        auto texture = textureDevice.CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::Resource, {}});
        auto view = textureDevice.CreateTextureView({texture.Get(), render::TextureDimension::Dim2D,
                                                     render::TextureFormat::RGBA8_UNORM, render::SubresourceRange::AllSub(), render::TextureViewUsage::Resource});
        return make_unique<TextureAsset>(&textureDevice, "snapshot cache texture", texture.Release(), view.Release());
    };
    auto texture = assets.Load<TextureAsset>({test::kUploadTestId,
                                              [](ManualGate* pending, unique_ptr<TextureAsset> value) -> task<AssetLoadResult> {
                                                  co_await pending->Wait();
                                                  co_return AssetLoadResult::Success(std::move(value));
                                              }(&gate, makeTexture()),
                                              "snapshot loading texture"});
    auto material = Material::Create(technique.Get());
    ASSERT_TRUE(material->SetTexture("AlbedoTexture", texture));
    const auto sampler = render::SamplerDescriptor{};
    ASSERT_TRUE(material->SetSampler("LinearSampler", sampler));
    MaterialRenderData before, after;
    vector<StreamingAssetRefAny> retained;
    uint64_t bytes = 0;
    EXPECT_FALSE(material->BuildRenderData(before, retained, &bytes));
    const uint64_t notReadyRevision = before.Revision;
    EXPECT_FALSE(material->BuildRenderData(before, retained, &bytes));
    EXPECT_EQ(bytes, 0u);
    after = before;
    gate.Resume();
    assets.Pump();
    ASSERT_TRUE(texture.IsReady());
    ASSERT_TRUE(material->BuildRenderData(after, retained, &bytes));
    EXPECT_GT(after.Revision, notReadyRevision);
    EXPECT_FALSE(before.Passes[0].Valid);
    EXPECT_TRUE(before.Passes[0].Textures.empty());
    auto* firstTexture = texture.Get().Get();
    ASSERT_EQ(after.Passes[0].Textures[0].Texture, firstTexture);
    const uint64_t readyRevision = after.Revision;
    ASSERT_TRUE(material->SetTexture("AlbedoTexture", texture));
    ASSERT_TRUE(material->SetSampler("LinearSampler", sampler));
    retained.clear();
    ASSERT_TRUE(material->BuildRenderData(after, retained, &bytes));
    EXPECT_EQ(bytes, 0u);
    EXPECT_EQ(after.Revision, readyRevision);
    EXPECT_EQ(retained.size(), 1u);
    texture.Reset();

    const AssetId nextId{0xaabbccde, 0x1122, 0x3344, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};
    auto nextTexture = assets.AddReady<TextureAsset>(nextId, makeTexture());
    ASSERT_TRUE(material->SetTexture("AlbedoTexture", nextTexture));
    auto changedSampler = sampler;
    changedSampler.MinFilter = render::FilterMode::Linear;
    ASSERT_TRUE(material->SetSampler("LinearSampler", changedSampler));
    vector<StreamingAssetRefAny> nextRetained;
    ASSERT_TRUE(material->BuildRenderData(before, nextRetained));
    EXPECT_EQ(after.Passes[0].Textures[0].Texture, firstTexture);
    EXPECT_EQ(before.Passes[0].Textures[0].Texture, nextTexture.Get().Get());
    EXPECT_EQ(before.Passes[0].Samplers[0].Sampler, changedSampler);
    material = nullptr;
    nextTexture.Reset();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 2u);
    retained.clear();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 1u);
    nextRetained.clear();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 0u);
    EXPECT_EQ(textureDevice.LiveTextures, 0);
    EXPECT_EQ(textureDevice.LiveTextureViews, 0);
#endif
}

TEST_P(MaterialSnapshotCache, UnavailableMaterialsAndOrderChangesPreserveOtherFlightMaterialCaches) {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
    GTEST_SKIP() << "Shader JIT disabled";
#else
    auto program = test::CompileStageBProgram(*Device.Device, test::StageBMaterialSource("float4 BaseColor;", 1, true));
    ASSERT_TRUE(program);
    auto technique = MaterialTechnique::Create({{"ForwardLit", program.Get(), "MaterialValues", {}}}, "ForwardLit");
    ASSERT_TRUE(technique);
    test::UploadTestDevice textureDevice;
    ImmediateWait wait;
    AssetManager assets;
    assets.SetWaitFrameProcessor(&wait);
    ManualGate gate;
    const auto makeTexture = [&]() {
        auto texture = textureDevice.CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::Resource, {}});
        auto view = textureDevice.CreateTextureView({texture.Get(), render::TextureDimension::Dim2D,
                                                     render::TextureFormat::RGBA8_UNORM, render::SubresourceRange::AllSub(), render::TextureViewUsage::Resource});
        return make_unique<TextureAsset>(&textureDevice, "snapshot material ordering", texture.Release(), view.Release());
    };
    auto pendingTexture = assets.Load<TextureAsset>({test::kUploadTestId,
                                                     [](ManualGate* pending, unique_ptr<TextureAsset> value) -> task<AssetLoadResult> {
                                                         co_await pending->Wait();
                                                         co_return AssetLoadResult::Success(std::move(value));
                                                     }(&gate, makeTexture()),
                                                     "pending ordering texture"});
    const AssetId readyId{0xaabbccef, 0x1122, 0x3344, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};
    auto readyTexture = assets.AddReady<TextureAsset>(readyId, makeTexture());
    auto a = Material::Create(technique.Get());
    auto b = Material::Create(technique.Get());
    ASSERT_TRUE(a->SetTexture("AlbedoTexture", pendingTexture));
    ASSERT_TRUE(b->SetTexture("AlbedoTexture", readyTexture));
    ASSERT_TRUE(a->SetSampler("LinearSampler", {}));
    ASSERT_TRUE(b->SetSampler("LinearSampler", {}));
    ASSERT_TRUE(a->SetFloat4("BaseColor", Eigen::Vector4f::Ones()));
    ASSERT_TRUE(b->SetFloat4("BaseColor", Eigen::Vector4f::Constant(2)));
    Scene scene;
    auto first = make_unique<CacheProxy>(), second = make_unique<CacheProxy>();
    auto* firstProxy = first.get();
    auto* secondProxy = second.get();
    firstProxy->DrawMaterial = a.Get();
    secondProxy->DrawMaterial = b.Get();
    scene.AddPrimitive(std::move(first));
    scene.AddPrimitive(std::move(second));
    RenderSceneSnapshotBuilder builder;
    array<RenderSceneSnapshot, 2> flights;
    array<vector<StreamingAssetRefAny>, 2> retained;
    const auto expectBatchMaterials = [&](const RenderSceneSnapshot& snapshot, array<uint64_t, 2> expected) {
        ASSERT_EQ(snapshot.MeshBatches.size(), 2u);
        for (const auto& batch : snapshot.MeshBatches) {
            ASSERT_LT(batch.Primitive, expected.size());
            ASSERT_LT(batch.Material, snapshot.Materials.size());
            EXPECT_EQ(snapshot.Materials[batch.Material].Generation, expected[batch.Primitive]);
            EXPECT_EQ(snapshot.Primitives[batch.Primitive].Id,
                      scene.GetPrimitiveId(batch.Primitive == 0 ? firstProxy : secondProxy));
        }
    };
    for (uint32_t flight = 0; flight < flights.size(); ++flight)
        ASSERT_TRUE(builder.Build(scene, flights[flight], retained[flight]));
    for (uint32_t frame = 0; frame < 6; ++frame) {
        const uint32_t flight = frame % 2;
        retained[flight].clear();
        ASSERT_TRUE(builder.Build(scene, flights[flight], retained[flight]));
        EXPECT_EQ(flights[flight].Stats.MaterialBytesCopied, 0u);
        EXPECT_EQ(flights[flight].Stats.MaterialsReused, 2u);
        EXPECT_EQ(flights[flight].Stats.MaterialUnavailable, 1u);
        ASSERT_EQ(flights[flight].Materials.size(), 1u);
        EXPECT_EQ(flights[flight].Materials[0].Generation, b->GetGeneration());
        ASSERT_EQ(flights[flight].MeshBatches.size(), 1u);
        EXPECT_EQ(flights[flight].MeshBatches[0].Primitive, 1u);
        EXPECT_EQ(flights[flight].MeshBatches[0].Material, 0u);
        EXPECT_EQ(retained[flight].size(), 1u);
    }

    // Change material encounter order while the unavailable candidate remains unpublished.
    firstProxy->DrawMaterial = b.Get();
    secondProxy->DrawMaterial = a.Get();
    retained[0].clear();
    ASSERT_TRUE(builder.Build(scene, flights[0], retained[0]));
    EXPECT_EQ(flights[0].Stats.MaterialBytesCopied, 0u);
    EXPECT_EQ(flights[0].Stats.MaterialsReused, 2u);
    EXPECT_EQ(flights[0].Stats.PublishedMaterialBytes, 0u);
    ASSERT_EQ(flights[0].MeshBatches.size(), 1u);
    EXPECT_EQ(flights[0].MeshBatches[0].Primitive, 0u);
    EXPECT_EQ(flights[0].MeshBatches[0].Material, 0u);
    gate.Resume();
    assets.Pump();
    ASSERT_TRUE(pendingTexture.IsReady());
    for (uint32_t flight = 0; flight < flights.size(); ++flight) {
        retained[flight].clear();
        ASSERT_TRUE(builder.Build(scene, flights[flight], retained[flight]));
        ASSERT_EQ(flights[flight].Materials.size(), 2u);
        EXPECT_EQ(flights[flight].Materials[0].Generation, b->GetGeneration());
        EXPECT_EQ(flights[flight].Materials[1].Generation, a->GetGeneration());
        EXPECT_EQ(flights[flight].Stats.MaterialsRebuilt, flight == 0 ? 1u : 0u);
        EXPECT_EQ(flights[flight].Stats.MaterialsReused, flight == 0 ? 1u : 2u);
        EXPECT_GT(flights[flight].Stats.PublishedMaterialBytes, 0u);
        expectBatchMaterials(flights[flight], {b->GetGeneration(), a->GetGeneration()});
        EXPECT_EQ(retained[flight].size(), 2u);
    }
    EXPECT_NE(flights[0].Materials[1].Passes[0].NumericBytes.data(),
              flights[1].Materials[1].Passes[0].NumericBytes.data());

    // Assignment exchanges preserve canonical material payloads and update each batch's material index.
    firstProxy->DrawMaterial = a.Get();
    secondProxy->DrawMaterial = b.Get();
    for (uint32_t flight = 0; flight < flights.size(); ++flight) {
        retained[flight].clear();
        ASSERT_TRUE(builder.Build(scene, flights[flight], retained[flight]));
        EXPECT_EQ(flights[flight].Stats.MaterialBytesCopied, 0u);
        EXPECT_EQ(flights[flight].Stats.MaterialsReused, 2u);
        EXPECT_EQ(flights[flight].Stats.PublishedMaterialBytes, 0u);
        expectBatchMaterials(flights[flight], {a->GetGeneration(), b->GetGeneration()});
        EXPECT_EQ(retained[flight].size(), 2u);
    }
    auto replacement = Material::Create(technique.Get());
    ASSERT_TRUE(replacement->SetTexture("AlbedoTexture", readyTexture));
    ASSERT_TRUE(replacement->SetSampler("LinearSampler", {}));
    firstProxy->DrawMaterial = replacement.Get();
    retained[1].clear();
    ASSERT_TRUE(builder.Build(scene, flights[1], retained[1]));
    EXPECT_EQ(flights[1].Stats.MaterialsRebuilt, 1u);
    EXPECT_EQ(flights[1].Stats.MaterialsReused, 1u);
    expectBatchMaterials(flights[1], {replacement->GetGeneration(), b->GetGeneration()});
    expectBatchMaterials(flights[0], {a->GetGeneration(), b->GetGeneration()});
    EXPECT_GT(flights[1].Stats.PublishedMaterialBytes, 0u);
    EXPECT_EQ(retained[1].size(), 2u);
#endif
}

INSTANTIATE_TEST_SUITE_P(Backends, MaterialSnapshotCache,
                         testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
