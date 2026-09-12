#include "gpu_test_fixture.h"
#include "stage_b_test_support.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <new>
#include <tuple>
#ifdef _WIN32
#include <malloc.h>
#endif

#include <gtest/gtest.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/render_framework/scene.h>

// Replacements affect only this executable. Injection is confined to the calling thread
// during Publish; driver/private DLL allocations and other threads are excluded.
namespace publication_allocations {
thread_local bool Enabled{false}, Fired{false};
thread_local size_t Remaining{0};
void BeforeAllocation() {
    if (!Enabled) return;
    if (Remaining != 0) {
        --Remaining;
        return;
    }
    Enabled = false;
    Fired = true;
    throw std::bad_alloc{};
}
void* Allocate(size_t size) {
    BeforeAllocation();
    if (auto* result = std::malloc(std::max(size, size_t{1}))) return result;
    throw std::bad_alloc{};
}
void* AllocateAligned(size_t size, size_t alignment) {
    BeforeAllocation();
#ifdef _WIN32
    auto* result = _aligned_malloc(std::max(size, size_t{1}), alignment);
#else
    auto* result = std::aligned_alloc(alignment, ((std::max(size, size_t{1}) + alignment - 1) / alignment) * alignment);
#endif
    if (!result) throw std::bad_alloc{};
    return result;
}
void FreeAligned(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}
struct FailureScope {
    explicit FailureScope(size_t skip) {
        Remaining = skip;
        Fired = false;
        Enabled = true;
    }
    ~FailureScope() { Enabled = false; }
};
}  // namespace publication_allocations
void* operator new(size_t size) { return publication_allocations::Allocate(size); }
void* operator new[](size_t size) { return publication_allocations::Allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
void* operator new(size_t size, std::align_val_t alignment) { return publication_allocations::AllocateAligned(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return publication_allocations::AllocateAligned(size, size_t(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { publication_allocations::FreeAligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { publication_allocations::FreeAligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { publication_allocations::FreeAligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { publication_allocations::FreeAligned(value); }

namespace radray {
namespace {

constexpr uint32_t kFailurePrimitives = 96, kFailureMaterials = 8;

class PublicationGeometry final : public Asset {
public:
    explicit PublicationGeometry(uint32_t& destroyed) : Destroyed(destroyed) {}
    ~PublicationGeometry() noexcept override { ++Destroyed; }
    void OnUnload(AssetManager&) override {}
    unique_ptr<render::Buffer> Vertices, Indices;
    array<GpuMesh::DrawData, 2> Draws;
    uint32_t& Destroyed;
};

class PublicationPrimitive final : public PrimitiveSceneProxy {
public:
    PublicationPrimitive(StreamingAssetRef<PublicationGeometry> geometry, Material* material)
        : Geometry(std::move(geometry)), DrawMaterial(material) {}
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return Revision; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {&Geometry->Draws[Variant], 0, Variant ? 6u : 3u, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
    void CollectAssetReferences(vector<StreamingAssetRefAny>& owners) const override { owners.push_back(Geometry); }
    void SetVariant(uint32_t variant) {
        if (Variant == variant) return;
        Variant = variant;
        ++Revision;
        MarkRenderDirty(PrimitiveDirtyKind::Structure);
    }
    StreamingAssetRef<PublicationGeometry> Geometry;
    Material* DrawMaterial;
    uint32_t Variant{0};
    uint64_t Revision{1};
};

bool CompilePublicationPolicy(const StaticPassCompileInput& input, StaticPassCompileResult& result) {
    result.NormalState = result.MirroredState = input.Pass.PipelineState;
    result.Bindings = input.Bindings ? *input.Bindings : StaticBindingRecipe{};
    result.Bindings.Valid = true;
    return true;
}

enum class AllocationPublicationResult { Published,
                                         Rejected,
                                         AllocationFailed };

// Keep the injected exception boundary separate from assertions and caller-owned flights.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
AllocationPublicationResult PublishWithAllocationFailure(Scene& scene, RenderSceneSnapshot& target,
                                                         vector<StreamingAssetRefAny>& owners, RenderValidationMode validation,
                                                         uint64_t epoch, size_t failAt) {
    try {
        publication_allocations::FailureScope allocation(failAt);
        return scene.GetRenderState().Publish(scene, target, owners, validation, epoch)
                   ? AllocationPublicationResult::Published
                   : AllocationPublicationResult::Rejected;
    } catch (const std::bad_alloc&) {
        return AllocationPublicationResult::AllocationFailed;
    }
}

void CheckPublicationValues(const RenderSceneSnapshot& snapshot, uint32_t value,
                            const array<unique_ptr<Material>, kFailureMaterials>& materials,
                            const array<string, 2>& names) {
    ASSERT_TRUE(snapshot.Valid);
    ASSERT_EQ(snapshot.Primitives.size(), kFailurePrimitives);
    ASSERT_EQ(snapshot.MeshBatches.size(), kFailurePrimitives);
    ASSERT_EQ(snapshot.DrawRecords.size(), kFailurePrimitives * 2);
    ASSERT_EQ(snapshot.Materials.size(), materials.size());
    ASSERT_EQ(snapshot.PrimitiveDrawBegin.size(), kFailurePrimitives + 1);
    for (uint32_t primitive = 0; primitive < kFailurePrimitives; ++primitive) {
        const auto& data = snapshot.Primitives[primitive];
        EXPECT_FLOAT_EQ(data.LocalToWorld(0, 3), float(value) + float(primitive) * .25f);
        EXPECT_FLOAT_EQ(data.WorldBounds.Min.x(), float(value) + float(primitive) * .25f);
        EXPECT_FLOAT_EQ(data.WorldBounds.Max.x(), float(value) + float(primitive) * .25f + 1);
        ASSERT_LT(data.FirstMeshBatch, snapshot.MeshBatches.size());
        const auto& batch = snapshot.MeshBatches[data.FirstMeshBatch];
        ASSERT_LT(batch.Material, snapshot.Materials.size());
        EXPECT_EQ(snapshot.Materials[batch.Material].Generation, materials[primitive % materials.size()]->GetGeneration());
        EXPECT_EQ(batch.IndexCount, value % 2 ? 6u : 3u);
        const auto begin = snapshot.PrimitiveDrawBegin[primitive], end = snapshot.PrimitiveDrawBegin[primitive + 1];
        ASSERT_EQ(end - begin, 2u);
        ASSERT_LE(end, snapshot.DrawRecords.size());
        for (uint32_t row = begin; row < end; ++row) {
            const auto& draw = snapshot.DrawRecords[row];
            EXPECT_EQ(draw.PrimitiveId, data.Id);
            EXPECT_EQ(draw.Material, batch.Material);
            EXPECT_EQ(draw.Status, DrawRecordStatus::Ready);
            EXPECT_EQ(draw.Description.IndexCount, value % 2 ? 6u : 3u);
            EXPECT_EQ(draw.PolicyRevision, value + 1);
            EXPECT_TRUE(draw.Description.LayoutId.IsValid());
        }
    }
    for (const auto& material : snapshot.Materials) {
        const auto source = std::find_if(materials.begin(), materials.end(), [&](const auto& candidate) { return candidate->GetGeneration() == material.Generation; });
        ASSERT_NE(source, materials.end());
        const auto index = static_cast<uint32_t>(source - materials.begin());
        ASSERT_EQ(material.Passes.size(), names.size());
        for (uint32_t passIndex = 0; passIndex < names.size(); ++passIndex) {
            const auto& pass = material.Passes[passIndex];
            EXPECT_EQ(pass.PassName, names[passIndex]);
            EXPECT_TRUE(pass.Valid);
            ASSERT_GE(pass.NumericBytes.size(), sizeof(float) * 4);
            array<float, 4> color;
            std::memcpy(color.data(), pass.NumericBytes.data(), sizeof(color));
            EXPECT_EQ(color, (array<float, 4>{float(value), float(index), float(index % 2), 1}));
            EXPECT_EQ(pass.NumericBytes.size(), index % 2 ? 512u : 16u);
            for (size_t byteIndex = sizeof(color); byteIndex < pass.NumericBytes.size(); ++byteIndex)
                EXPECT_EQ(pass.NumericBytes[byteIndex], byte{0});
        }
    }
}

class ScenePublicationFailureTest : public testing::TestWithParam<std::tuple<render::RenderBackend, RenderValidationMode>> {};
TEST_P(ScenePublicationFailureTest, RetainAndCopyFailuresRetryAllPendingPagesAndOwnedPayloadsAcrossFlights) {
    const auto [backend, validation] = GetParam();
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(backend, device, true)) {
        if (render::test::SetupMustFail(device.Status, render::test::RequiredBackend(backend))) FAIL() << device.Reason;
        GTEST_SKIP() << device.Reason;
    }
    const array<string, 2> names{"PublicationFirstPassWithOwnedVariableLengthName", "PublicationSecondPassWithAnotherOwnedVariableLengthName"};
    array<unique_ptr<ShaderProgram>, 2> programs;
    array<unique_ptr<MaterialTechnique>, 2> techniques;
    array<unique_ptr<Material>, kFailureMaterials> materials;
    for (uint32_t schema = 0; schema < programs.size(); ++schema) {
        auto program = test::CompileStageBProgram(*device.Device, test::StageBMaterialSource(schema ? "float4 BaseColor; float4 Padding[31];" : "float4 BaseColor;"));
        ASSERT_TRUE(program);
        programs[schema] = program.Release();
        auto technique = MaterialTechnique::Create({{names[0], programs[schema].get(), "MaterialValues", {}}, {names[1], programs[schema].get(), "MaterialValues", {}}}, names[0]);
        ASSERT_TRUE(technique);
        techniques[schema] = technique.Release();
    }
    for (uint32_t index = 0; index < materials.size(); ++index) {
        auto material = Material::Create(techniques[index % 2].get());
        ASSERT_TRUE(material);
        materials[index] = material.Release();
    }
    uint32_t destroyed = 0;
    AssetManager assets;
    auto geometry = make_unique<PublicationGeometry>(destroyed);
    const array<float, 24> vertices{-1, -1, 0, 1, -1, 0, 0, 1, 0};
    const array<uint32_t, 6> indices{0, 1, 2, 2, 1, 0};
    auto vertex = render::test::MakeUploadBuffer(*device.Device, std::as_bytes(std::span{vertices}), render::BufferUse::Vertex);
    auto index = render::test::MakeUploadBuffer(*device.Device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertex && index);
    geometry->Vertices = vertex.Release();
    geometry->Indices = index.Release();
    for (uint32_t variant = 0; variant < geometry->Draws.size(); ++variant) {
        auto& draw = geometry->Draws[variant];
        draw.VertexBuffers = {{0, {geometry->Vertices.get(), 0, sizeof(vertices)}}};
        draw.Ibv = {geometry->Indices.get(), 0, 4};
        draw.VertexLayout.Buffers = {{0, variant ? 16u : 12u, render::VertexStepMode::Vertex}};
        draw.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    }
    const AssetId id{0xfa119e49, 0x1248, 0x4534, 0x81, 0xaa, 0x28, 0x39, 0x4a, 0x5b, 0x6c, 0x7d};
    auto asset = assets.AddReady<PublicationGeometry>(id, std::move(geometry));
    ASSERT_TRUE(asset.IsReady());
    Scene scene;
    array<PublicationPrimitive*, kFailurePrimitives> proxies;
    for (uint32_t primitive = 0; primitive < proxies.size(); ++primitive) {
        auto proxy = make_unique<PublicationPrimitive>(asset, materials[primitive % materials.size()].get());
        proxies[primitive] = proxy.get();
        ASSERT_TRUE(scene.AddPrimitive(std::move(proxy)));
    }
    array<PassPolicy, 2> policies{{{{4901}, 1, names[0], CompilePublicationPolicy}, {{4902}, 1, names[1], CompilePublicationPolicy}}};
    const auto edit = [&](uint32_t value) {
        for (uint32_t primitive = 0; primitive < proxies.size(); ++primitive) {
            Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
            transform(0, 3) = float(value) + float(primitive) * .25f;
            proxies[primitive]->SetLocalToWorld(transform);
            proxies[primitive]->SetVariant(value % 2);
        }
        for (uint32_t material = 0; material < materials.size(); ++material)
            ASSERT_TRUE(materials[material]->SetFloat4("BaseColor", {float(value), float(material), float(material % 2), 1}));
    };
    array<RenderSceneSnapshot, 3> snapshots;
    array<vector<StreamingAssetRefAny>, 3> owners;
    array<uint32_t, 3> flightValues{};
    uint64_t epoch = 0;
    edit(0);
    for (uint32_t flight = 0; flight < snapshots.size(); ++flight) {
        ASSERT_TRUE(scene.GetDrawStore().SetActivePolicies(++epoch, policies));
        ASSERT_TRUE(scene.GetRenderState().Publish(scene, snapshots[flight], owners[flight], validation, epoch));
        CheckPublicationValues(snapshots[flight], 0, materials, names);
    }
    vector<SnapshotPublicationFailure> failures{{.AfterRetainedOwners = 0}, {.AfterRetainedOwners = 17}, {.AfterRetainedOwners = kFailurePrimitives}};
    for (uint32_t tables = 0; tables <= 8; ++tables) failures.push_back({.AfterCopiedTables = tables});
    // Every element boundary through primitive, batch, material and draw pages, including
    // material rows that own two long names and differently sized numeric payloads.
    for (uint32_t entries = 0; entries <= kFailurePrimitives * 4 + kFailureMaterials; ++entries)
        failures.push_back({.AfterCopiedEntries = entries});
    for (uint32_t attempt = 0; attempt < failures.size(); ++attempt) {
        SCOPED_TRACE(fmt::format("failure={} epoch={}", attempt, epoch + 1));
        const auto flight = attempt % snapshots.size();
        auto& target = snapshots[flight];
        const auto priorRevision = target.PublicationRevision, priorEpoch = target.SceneEpoch;
        edit(attempt + 1);
        for (auto& policy : policies) policy.Revision = attempt + 2;
        ASSERT_TRUE(scene.GetDrawStore().SetActivePolicies(++epoch, policies));
        owners[flight].clear();
        owners[flight].push_back(asset);
        scene.GetRenderState().FailNextPublicationForTesting(failures[attempt]);
        EXPECT_FALSE(scene.GetRenderState().Publish(scene, target, owners[flight], validation, epoch));
        EXPECT_FALSE(target.Valid);
        EXPECT_EQ(target.PublicationRevision, priorRevision);
        EXPECT_EQ(target.SceneEpoch, priorEpoch);
        EXPECT_EQ(owners[flight].size(), 1u);
        const auto pendingAfterFailure = scene.GetRenderState().GetMemoryStats().PendingPages;
        EXPECT_GT(pendingAfterFailure, 3u);
        for (uint32_t other = 0; other < snapshots.size(); ++other)
            if (other != flight) CheckPublicationValues(snapshots[other], flightValues[other], materials, names);
        ASSERT_TRUE(scene.GetRenderState().Publish(scene, target, owners[flight], validation, epoch));
        EXPECT_EQ(target.PublicationRevision, priorRevision + 1);
        EXPECT_EQ(target.ChangedFromPublicationRevision, priorRevision);
        EXPECT_EQ(target.SceneEpoch, epoch);
        EXPECT_EQ(target.Stats.SceneCommits, 0u);
        EXPECT_EQ(target.Stats.StaticRecipeCompiles, 0u);
        EXPECT_GE(target.Stats.PublishedPages, 11u);
        EXPECT_GT(target.Stats.PublishedVariablePayloadBytes, 512u);
        EXPECT_LT(scene.GetRenderState().GetMemoryStats().PendingPages, pendingAfterFailure);
        EXPECT_EQ(owners[flight].size(), kFailurePrimitives + 1);
        flightValues[flight] = attempt + 1;
        CheckPublicationValues(target, flightValues[flight], materials, names);
        ASSERT_FALSE(HasFatalFailure());
        // Repeated requests neither publish twice nor append another set of frame owners.
        ASSERT_TRUE(scene.GetRenderState().Publish(scene, target, owners[flight], validation, epoch));
        EXPECT_EQ(target.PublicationRevision, priorRevision + 1);
        EXPECT_EQ(owners[flight].size(), kFailurePrimitives + 1);
        EXPECT_EQ(destroyed, 0u);
    }
    for (uint32_t trial = 0; trial < snapshots.size() * 2; ++trial) {
        const uint32_t flight = trial % snapshots.size();
        const bool reusePassStorage = trial >= snapshots.size();
        SCOPED_TRACE(fmt::format("allocation failure flight={} reusePassStorage={}", flight, reusePassStorage));
        auto& target = snapshots[flight];
        const auto priorRevision = target.PublicationRevision, priorEpoch = target.SceneEpoch;
        const uint32_t value = static_cast<uint32_t>(failures.size()) + trial + 1;
        edit(value);
        for (auto& policy : policies) policy.Revision = value + 1;
        ASSERT_TRUE(scene.GetDrawStore().SetActivePolicies(++epoch, policies));
        scene.GetRenderState().FailNextPublicationForTesting({.AfterRetainedOwners = 0});
        ASSERT_FALSE(scene.GetRenderState().Publish(scene, target, owners[flight], validation, epoch));
        const auto pending = scene.GetRenderState().GetMemoryStats().PendingPages;
        bool succeeded = false, sawPartialMaterial = false;
        size_t allocationFailures = 0;
        for (size_t failAt = 0; failAt < 256; ++failAt) {
            SCOPED_TRACE(fmt::format("allocation={}", failAt));
            // The target is writable and invalid. Release only tables dirtied in full above
            // so every retry exercises real storage growth, including nested material fields.
            const auto release = [](auto& values) { std::remove_reference_t<decltype(values)>{}.swap(values); };
            release(target.Primitives);
            release(target.Materials);
            release(target.DrawRecords);
            release(target.ChangedPrimitiveRanges);
            if (reusePassStorage) {
                target.Materials.resize(kFailureMaterials);
                for (auto& material : target.Materials) material.Passes.resize(names.size());
            }
            vector<StreamingAssetRefAny> initialOwners{asset};
            owners[flight].swap(initialOwners);
            const auto result = PublishWithAllocationFailure(scene, target, owners[flight], validation, epoch, failAt);
            if (result != AllocationPublicationResult::AllocationFailed) {
                ASSERT_FALSE(publication_allocations::Fired);
                ASSERT_EQ(result, AllocationPublicationResult::Published);
                succeeded = true;
                break;
            }
            ++allocationFailures;
            EXPECT_TRUE(publication_allocations::Fired);
            EXPECT_FALSE(target.Valid);
            EXPECT_EQ(target.PublicationRevision, priorRevision);
            EXPECT_EQ(target.SceneEpoch, priorEpoch);
            EXPECT_EQ(owners[flight].size(), 1u);
            EXPECT_EQ(scene.GetRenderState().GetMemoryStats().PendingPages, pending);
            for (const auto& material : target.Materials)
                for (const auto& pass : material.Passes)
                    sawPartialMaterial |= pass.PassName == names[0] && pass.NumericBytes.empty();
            for (uint32_t other = 0; other < snapshots.size(); ++other)
                if (other != flight) CheckPublicationValues(snapshots[other], flightValues[other], materials, names);
        }
        ASSERT_TRUE(succeeded);
        EXPECT_GT(allocationFailures, 32u);
        if (reusePassStorage) EXPECT_TRUE(sawPartialMaterial);
        EXPECT_EQ(target.PublicationRevision, priorRevision + 1);
        EXPECT_EQ(target.SceneEpoch, epoch);
        EXPECT_LT(scene.GetRenderState().GetMemoryStats().PendingPages, pending);
        EXPECT_EQ(owners[flight].size(), kFailurePrimitives + 1);
        EXPECT_EQ(target.Stats.SceneCommits, 0u);
        EXPECT_EQ(target.Stats.StaticRecipeCompiles, 0u);
        flightValues[flight] = value;
        CheckPublicationValues(target, value, materials, names);
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_TRUE(scene.GetRenderState().Publish(scene, target, owners[flight], validation, epoch));
        EXPECT_EQ(target.PublicationRevision, priorRevision + 1);
        EXPECT_EQ(owners[flight].size(), kFailurePrimitives + 1);
        fmt::print("PUBLICATION_ALLOCATION_FAILURES backend={} validation={} flight={} reusePassStorage={} failures={} partialMaterial={} recovered=true\n", backend, validation == RenderValidationMode::Full ? "Full" : "Off", flight, reusePassStorage, allocationFailures, sawPartialMaterial);
    }
    for (auto* proxy : proxies) scene.RemovePrimitive(proxy);
    asset.Reset();
    assets.Pump();
    EXPECT_EQ(destroyed, 0u);
    for (auto& retained : owners) retained.clear();
    assets.Pump();
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(assets.GetAssetCount(), 0u);
    EXPECT_EQ(device.ValidationErrors.load(), 0u);
}
INSTANTIATE_TEST_SUITE_P(Backends, ScenePublicationFailureTest,
                         testing::Combine(testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan),
                                          testing::Values(RenderValidationMode::Off, RenderValidationMode::Full)));

}  // namespace
}  // namespace radray
