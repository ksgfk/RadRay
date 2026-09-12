#include "gpu_test_fixture.h"
#include "stage_b_test_support.h"

#include <algorithm>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

#include <gtest/gtest.h>
#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/render_framework/scene.h>

namespace scene_memory_allocations {
thread_local bool Enabled = false;
thread_local uint64_t Count = 0, Bytes = 0;
void CountAllocation(size_t size) noexcept {
    if (Enabled) {
        ++Count;
        Bytes += size;
    }
}
void* Allocate(size_t size) {
    CountAllocation(size);
    if (auto* result = std::malloc(std::max(size, size_t{1}))) return result;
    std::abort();
}
void* AllocateAligned(size_t size, size_t alignment) {
    CountAllocation(size);
#ifdef _WIN32
    auto* result = _aligned_malloc(std::max(size, size_t{1}), alignment);
#else
    auto* result = std::aligned_alloc(alignment, ((std::max(size, size_t{1}) + alignment - 1) / alignment) * alignment);
#endif
    if (!result) std::abort();
    return result;
}
void FreeAligned(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}
}  // namespace scene_memory_allocations

void* operator new(size_t size) { return scene_memory_allocations::Allocate(size); }
void* operator new[](size_t size) { return scene_memory_allocations::Allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
void* operator new(size_t size, std::align_val_t alignment) { return scene_memory_allocations::AllocateAligned(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return scene_memory_allocations::AllocateAligned(size, size_t(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { scene_memory_allocations::FreeAligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { scene_memory_allocations::FreeAligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { scene_memory_allocations::FreeAligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { scene_memory_allocations::FreeAligned(value); }

namespace radray {
namespace {

class AllocationProbe {
public:
    AllocationProbe() {
        scene_memory_allocations::Count = scene_memory_allocations::Bytes = 0;
        scene_memory_allocations::Enabled = true;
    }
    ~AllocationProbe() { scene_memory_allocations::Enabled = false; }
};

class MemoryGeometry final : public Asset {
public:
    explicit MemoryGeometry(uint32_t& destroyed) : Destroyed(destroyed) {}
    ~MemoryGeometry() noexcept override { ++Destroyed; }
    void OnUnload(AssetManager&) override {}
    unique_ptr<render::Buffer> Vertices, Indices;
    array<GpuMesh::DrawData, 2> Draws;
    uint32_t& Destroyed;
};

class MemoryPrimitive final : public PrimitiveSceneProxy {
public:
    MemoryPrimitive(StreamingAssetRef<MemoryGeometry> geometry, Material* material, uint32_t variant)
        : Geometry(std::move(geometry)), DrawMaterial(material), Variant(variant) {}
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return Revision; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {&Geometry->Draws[Variant], 0, 3, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
    void CollectAssetReferences(vector<StreamingAssetRefAny>& out) const override { out.push_back(Geometry.AsAny()); }
    void SetMaterial(Material* material) {
        if (DrawMaterial == material) return;
        DrawMaterial = material;
        MarkRenderDirty(PrimitiveDirtyKind::MaterialAssignment);
    }
    void SetVariant(uint32_t variant) {
        if (Variant == variant) return;
        Variant = variant;
        ++Revision;
        MarkRenderDirty(PrimitiveDirtyKind::Structure);
    }
    StreamingAssetRef<MemoryGeometry> Geometry;
    Material* DrawMaterial;
    uint32_t Variant;
    uint64_t Revision{1};
};

bool CompileMemoryPolicy(const StaticPassCompileInput& input, StaticPassCompileResult& output) {
    output.NormalState = input.Pass.PipelineState;
    output.MirroredState = output.NormalState;
    output.MirroredState.Primitive.FaceClockwise = OppositeFrontFace(output.NormalState.Primitive.FaceClockwise);
    output.Bindings = input.Bindings ? *input.Bindings : StaticBindingRecipe{};
    output.Bindings.Valid = true;
    output.Bindings.Groups[1] = 1;
    output.Bindings.Buffers[1] = 0;
    output.Bindings.GroupOrder[0] = 1;
    output.Bindings.GroupCount = 1;
    return true;
}

uint64_t SnapshotFingerprint(const RenderSceneSnapshot& snapshot) {
    HashCode hash;
    hash.Add(snapshot.SceneEpoch);
    hash.Add(snapshot.PublicationId);
    hash.Add(snapshot.PublicationRevision);
    for (const auto& primitive : snapshot.Primitives) {
        hash.Add(primitive.Id.Slot);
        hash.Add(primitive.Id.Generation);
        hash.Add(HashData64(primitive.LocalToWorld.data(), sizeof(float) * 16));
    }
    for (const auto& draw : snapshot.DrawRecords) {
        hash.Add(draw.Id);
        hash.Add(draw.RecipeRevision);
        hash.Add(draw.Description.LayoutId.Value);
        hash.Add(reinterpret_cast<uintptr_t>(draw.Description.Geometry.Get()));
        hash.Add(draw.Material);
        hash.Add(draw.NormalStateId);
        hash.Add(draw.BindingRecipe);
    }
    for (const auto& material : snapshot.Materials) {
        hash.Add(material.Generation);
        hash.Add(material.Revision);
        for (const auto& pass : material.Passes) {
            hash.Add(pass.ProgramGeneration);
            hash.Add(HashData64(pass.NumericBytes.data(), pass.NumericBytes.size()));
        }
    }
    return hash.ToHashCode();
}

void MergeHighWatermark(RenderMemoryStats& target, const RenderMemoryStats& value) {
    target.ObjectBytes = std::max(target.ObjectBytes, value.ObjectBytes);
    target.VectorCapacityBytes = std::max(target.VectorCapacityBytes, value.VectorCapacityBytes);
    target.StringCapacityBytes = std::max(target.StringCapacityBytes, value.StringCapacityBytes);
    target.MapValueBytes = std::max(target.MapValueBytes, value.MapValueBytes);
    target.MapNodes = std::max(target.MapNodes, value.MapNodes);
    target.MapBuckets = std::max(target.MapBuckets, value.MapBuckets);
    target.MapContainers = std::max(target.MapContainers, value.MapContainers);
    target.VariablePayloadBytes = std::max(target.VariablePayloadBytes, value.VariablePayloadBytes);
    target.VariablePayloadCapacityBytes = std::max(target.VariablePayloadCapacityBytes, value.VariablePayloadCapacityBytes);
    target.LiveEntries = std::max(target.LiveEntries, value.LiveEntries);
    target.DirtyEntries = std::max(target.DirtyEntries, value.DirtyEntries);
    target.DependencyEdges = std::max(target.DependencyEdges, value.DependencyEdges);
    target.OwnerReferences = std::max(target.OwnerReferences, value.OwnerReferences);
}

class SceneMemoryTest : public testing::TestWithParam<render::RenderBackend> {};

TEST_P(SceneMemoryTest, TenThousandFiniteWorkingSetFramesBoundEveryCatalogAndFlightContainer) {
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(GetParam(), device, true)) {
        if (render::test::SetupMustFail(device.Status, render::test::RequiredBackend(GetParam()))) FAIL() << device.Reason;
        GTEST_SKIP() << device.Reason;
    }
    const string passName = "MemoryTrackedForwardPassWithAnOwnedLongName";
    array<unique_ptr<ShaderProgram>, 2> programs;
    array<unique_ptr<MaterialTechnique>, 2> techniques;
    array<unique_ptr<Material>, 4> materials;
    for (uint32_t index = 0; index < programs.size(); ++index) {
        auto program = test::CompileStageBProgram(*device.Device, test::StageBMaterialSource(index == 0 ? "float4 BaseColor;" : "float4 BaseColor; float4 Padding[31];"));
        ASSERT_TRUE(program);
        programs[index] = program.Release();
        auto technique = MaterialTechnique::Create({{passName, programs[index].get(), "MaterialValues", {}}}, passName);
        ASSERT_TRUE(technique);
        techniques[index] = technique.Release();
    }
    for (uint32_t index = 0; index < materials.size(); ++index) {
        auto material = Material::Create(techniques[index % 2].get());
        ASSERT_TRUE(material);
        materials[index] = material.Release();
    }
    EXPECT_LT(std::as_const(*materials[0]).NumericBytes().size(), std::as_const(*materials[1]).NumericBytes().size());
    uint32_t destroyed = 0;
    AssetManager assets;
    auto geometry = make_unique<MemoryGeometry>(destroyed);
    const array<float, 16> vertices{};
    const array<uint32_t, 3> indices{0, 1, 2};
    auto vertex = render::test::MakeUploadBuffer(*device.Device, std::as_bytes(std::span{vertices}), render::BufferUse::Vertex);
    auto index = render::test::MakeUploadBuffer(*device.Device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertex);
    ASSERT_TRUE(index);
    geometry->Vertices = vertex.Release();
    geometry->Indices = index.Release();
    for (uint32_t variant = 0; variant < geometry->Draws.size(); ++variant) {
        auto& draw = geometry->Draws[variant];
        draw.VertexBuffers = {{0, {geometry->Vertices.get(), 0, sizeof(vertices)}}};
        draw.Ibv = {geometry->Indices.get(), 0, 4};
        draw.VertexLayout.Buffers = {{0, 12 + variant * 4, render::VertexStepMode::Vertex}};
        draw.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
        draw.Topology = PrimitiveTopology::TriangleList;
        ASSERT_TRUE(ValidateMeshGeometry(draw, 0, 3));
    }
    const AssetId id{0x94129ead, 0xe771, 0x4eab, 0xa1, 0x33, 0x11, 0x90, 0x1a, 0xab, 0xde, 0x21};
    auto geometryRef = assets.AddReady<MemoryGeometry>(id, std::move(geometry));
    ASSERT_TRUE(geometryRef.IsReady());
    Scene scene;
    array<Nullable<MemoryPrimitive*>, 64> primitives;
    const auto add = [&](uint32_t slot) {
        auto proxy = make_unique<MemoryPrimitive>(geometryRef, materials[slot % 4].get(), 0);
        auto* raw = proxy.get();
        scene.AddPrimitive(std::move(proxy));
        primitives[slot] = raw;
    };
    for (uint32_t slot = 0; slot < primitives.size(); ++slot) add(slot);
    array<vector<StreamingAssetRefAny>, 3> owners;
    array<shared_ptr<const RenderSceneSnapshot>, 3> snapshots;
    const array<PassPolicy, 2> policies{{{{901}, 1, passName, CompileMemoryPolicy}, {{902}, 1, passName, CompileMemoryPolicy}}};
    array<RenderMemoryStats, 5> high;
    uint64_t copiedPayload = 0, movedPayload = 0, payloadMoves = 0;
    uint64_t steadySamples = 0, changedAllocations = 0, changedAllocationBytes = 0;
    uint32_t removedMaterial = 0;
    uint64_t transferredMaterial = 0;
    constexpr uint32_t warmup = 192, frames = 10000;
    for (uint32_t frame = 0; frame < warmup + frames; ++frame) {
        const auto phase = frame % 12;
        const auto flight = frame % 3;
        if (phase == 0) {
            for (uint32_t slot = 60; slot < 64; ++slot) {
                scene.RemovePrimitive(primitives[slot].Get());
                primitives[slot] = nullptr;
            }
        } else if (phase == 1) {
            for (uint32_t slot = 60; slot < 64; ++slot) add(slot);
        } else if (phase == 2 || phase == 3) {
            if (phase == 2) {
                // Reinserted materials append to the catalog. Select its current first entry
                // each cycle so measured cycles keep exercising an owning swap-removal.
                const auto& latest = snapshots[(frame - 1) % snapshots.size()];
                ASSERT_TRUE(latest);
                ASSERT_EQ(latest->Materials.size(), materials.size());
                const auto generation = latest->Materials.front().Generation;
                const auto found = std::find_if(materials.begin(), materials.end(), [&](const auto& material) { return material->GetGeneration() == generation; });
                ASSERT_NE(found, materials.end());
                removedMaterial = static_cast<uint32_t>(found - materials.begin());
                transferredMaterial = latest->Materials.back().Generation;
                ASSERT_NE(generation, transferredMaterial);
            }
            for (uint32_t slot = removedMaterial; slot < primitives.size(); slot += materials.size())
                primitives[slot]->SetMaterial(materials[phase == 2 ? (removedMaterial + 1) % materials.size() : removedMaterial].get());
        } else if (phase == 4 || phase == 6) {
            for (auto proxy : primitives) proxy->SetVariant(phase == 4 ? 1 : 0);
        } else if (phase == 8) {
            ASSERT_TRUE(materials[0]->SetFloat4("BaseColor", Eigen::Vector4f::Constant(float(frame))));
        }
        const size_t policyCount = phase == 5 || phase == 6 ? 1 : 2;
        ASSERT_TRUE(scene.GetDrawStore().SetActivePolicies(frame + 1, std::span{policies}.first(policyCount)));
        array<uint64_t, 3> previous{};
        for (uint32_t other = 0; other < snapshots.size(); ++other)
            if (other != flight && snapshots[other]) previous[other] = SnapshotFingerprint(*snapshots[other]);
        owners[flight].clear();
        Nullable<shared_ptr<const RenderSceneSnapshot>> published;
        {
            AllocationProbe probe;
            published = scene.GetRenderState().PrepareShared(scene, frame + 1, flight, owners[flight], RenderValidationMode::Off);
        }
        const auto allocations = scene_memory_allocations::Count;
        const auto allocationBytes = scene_memory_allocations::Bytes;
        ASSERT_TRUE(published);
        snapshots[flight] = published.Release();
        if (phase == 2) {
            ASSERT_EQ(snapshots[flight]->Materials.size(), materials.size() - 1);
            EXPECT_EQ(snapshots[flight]->Materials.front().Generation, transferredMaterial);
            EXPECT_EQ(snapshots[flight]->Stats.MaterialPayloadMoves, 1u);
            EXPECT_GT(snapshots[flight]->Stats.MovedMaterialPayloadBytes, 0u);
        }
        ASSERT_EQ(snapshots[flight]->DrawRecords.size(), scene.Primitives().size() * policyCount);
        for (const auto& draw : snapshots[flight]->DrawRecords) ASSERT_EQ(draw.Status, DrawRecordStatus::Ready);
        for (uint32_t other = 0; other < snapshots.size(); ++other)
            if (other != flight && snapshots[other]) ASSERT_EQ(SnapshotFingerprint(*snapshots[other]), previous[other]);
        EXPECT_EQ(owners[flight].size(), scene.Primitives().size());
        EXPECT_EQ(destroyed, 0u);
        EXPECT_EQ(assets.GetAssetCount(), 1u);
        SceneRenderStateMemoryStats state;
        array<RenderMemoryStats, 5> census;
        {
            AllocationProbe probe;
            census[0] = scene.GetMemoryStats();
            census[1] = scene.GetDrawStore().GetMemoryStats();
            state = scene.GetRenderState().GetMemoryStats();
            census[2] = state.Catalog;
            census[2].Add(state.Canonical);
            census[3] = state.Publications;
            census[4] = state.SharedFlights;
            for (const auto& retained : owners) {
                census[4].VectorCapacityBytes += retained.capacity() * sizeof(StreamingAssetRefAny);
                census[4].OwnerReferences += retained.size();
            }
        }
        ASSERT_EQ(scene_memory_allocations::Count, 0u);
        EXPECT_LE(state.LivePublications, 3u);
        EXPECT_LE(state.SharedFlightCount, 3u);
        EXPECT_LE(state.MaterialEntries, 4u);
        EXPECT_LE(state.ProgramEntries, 2u);
        for (size_t layer = 0; layer < census.size(); ++layer) {
            if (frame < warmup) {
                MergeHighWatermark(high[layer], census[layer]);
            } else {
                auto observed = high[layer];
                MergeHighWatermark(observed, census[layer]);
                ASSERT_EQ(observed, high[layer]) << "frame=" << frame << " phase=" << phase << " layer=" << layer;
            }
        }
        if (frame >= warmup) {
            const auto& stats = snapshots[flight]->Stats;
            copiedPayload += stats.PublishedVariablePayloadBytes;
            movedPayload += stats.MovedMaterialPayloadBytes;
            payloadMoves += stats.MaterialPayloadMoves;
            if (phase >= 9) {
                EXPECT_EQ(allocations, 0u) << "frame=" << frame << " phase=" << phase;
                ++steadySamples;
            } else {
                changedAllocations += allocations;
                changedAllocationBytes += allocationBytes;
            }
        }
    }
    ASSERT_GT(steadySamples, 0u);
    ASSERT_GT(copiedPayload, 0u);
    ASSERT_GT(payloadMoves, 0u);
    ASSERT_GT(movedPayload, 0u);
    for (size_t layer = 0; layer < high.size(); ++layer) {
        const auto& memory = high[layer];
        fmt::print("SCENE_MEMORY {{\"layer\":{},\"frames\":{},\"flights\":3,\"knownBytes\":{},\"vectorCapacityBytes\":{},\"stringCapacityBytes\":{},\"mapNodes\":{},\"mapBuckets\":{},\"mapValueBytes\":{},\"payloadCapacityBytes\":{},\"dependencyEdges\":{},\"ownerReferences\":{}}}\n",
                   layer, frames, memory.KnownBytes(), memory.VectorCapacityBytes, memory.StringCapacityBytes, memory.MapNodes, memory.MapBuckets, memory.MapValueBytes, memory.VariablePayloadCapacityBytes, memory.DependencyEdges, memory.OwnerReferences);
    }
    fmt::print("SCENE_MEMORY_WORK {{\"steadyPublishSamples\":{},\"changedAllocations\":{},\"changedAllocationBytes\":{},\"publishedVariablePayloadBytes\":{},\"materialOwnershipMoves\":{},\"ownershipTransferredPayloadBytes\":{},\"allocationScope\":\"calling-thread C++ new during CPU Commit/Publish; excludes authoring, driver/DLL and malloc\",\"storageScope\":\"known container storage, not allocator overhead or RSS\"}}\n",
               steadySamples, changedAllocations, changedAllocationBytes, copiedPayload, payloadMoves, movedPayload);
    for (auto proxy : primitives)
        if (proxy) scene.RemovePrimitive(proxy.Get());
    geometryRef.Reset();
    assets.Pump();
    EXPECT_EQ(destroyed, 0u);
    for (auto& retained : owners) retained.clear();
    assets.Pump();
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(assets.GetAssetCount(), 0u);
    EXPECT_EQ(device.ValidationErrors.load(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, SceneMemoryTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
