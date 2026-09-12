#include "foundation_graph_fixture.h"
#include <algorithm>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/scene.h>

// With the static runtime linked into this executable, replacements measure its calling-thread
// C++ new as well as the fixture, including aligned/array forms.
// They do not measure other threads, driver/DLL-private allocators, malloc, or resident memory.
namespace ready_allocations {
thread_local bool Enabled{false};
thread_local uint64_t Count{0}, Bytes{0};
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
struct Probe {
    Probe() {
        Count = Bytes = 0;
        Enabled = true;
    }
    ~Probe() { Enabled = false; }
};
}  // namespace ready_allocations
void* operator new(size_t size) { return ready_allocations::Allocate(size); }
void* operator new[](size_t size) { return ready_allocations::Allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
void* operator new(size_t size, std::align_val_t alignment) { return ready_allocations::AllocateAligned(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return ready_allocations::AllocateAligned(size, size_t(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { ready_allocations::FreeAligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { ready_allocations::FreeAligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { ready_allocations::FreeAligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { ready_allocations::FreeAligned(value); }

namespace radray {
namespace {
constexpr uint32_t kAllocationPrimitives = 128, kAllocationWarmup = 24, kAllocationFrames = 1000;
class AllocationPrimitive final : public PrimitiveSceneProxy {
public:
    AllocationPrimitive(const GpuMesh::DrawData& geometry, Material& material) : Geometry(geometry), DrawMaterial(material) {}
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {{-1, -1, 0}, {3, 3, 0}}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {&Geometry, 0, 3, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return &DrawMaterial; }
    const GpuMesh::DrawData& Geometry;
    Material& DrawMaterial;
};
bool CompileAllocationPolicy(const StaticPassCompileInput& input, StaticPassCompileResult& result) {
    result.NormalState = result.MirroredState = input.Pass.PipelineState;
    result.Bindings = input.Bindings ? *input.Bindings : StaticBindingRecipe{};
    result.Bindings.Valid = true;
    return true;
}
struct AllocationFlight {
    AllocationFlight(render::Device& device, render::RenderPassRegistry& registry) : GraphResources(device, registry), Groups(&device) {}
    HostWriteBatch Writes;
    RenderGraphFrameResources GraphResources;
    FrameDrawResources Groups;
    RenderSceneSnapshot Snapshot;
    vector<StreamingAssetRefAny> Owners;
    array<RendererList, 3> Lists;
    array<std::optional<PreparedRendererList>, 3> Ready;
    uint32_t Frame{0};
    uint64_t Samples{0}, Allocations{0}, AllocatedBytes{0}, ColdAllocations{0};
    void Reset() {
        for (auto& ready : Ready) ready.reset();
        for (auto& list : Lists) list.ResetForReuse();
        Owners.clear();
        Writes.Reset();
    }
    ~AllocationFlight() {
        Reset();
        Groups.ClearSets();
        GraphResources.Clear();
    }
};
class ReadyAllocationsTest : public test::FoundationGraphGpuTest {};

TEST_P(ReadyAllocationsTest, PublishedStaticListsHaveNoCallingThreadNewDuring1000WarmPreparations) {
    // Verify both replacement paths independently of optimizer-elidable new expressions.
    {
        ready_allocations::Probe probe;
        void* ordinary = ::operator new(37);
        void* aligned = ::operator new(65, std::align_val_t{64});
        ::operator delete(ordinary);
        ::operator delete(aligned, std::align_val_t{64});
    }
    ASSERT_EQ(ready_allocations::Count, 2u);
    ASSERT_EQ(ready_allocations::Bytes, 102u);
    auto& device = *Context.Device;
    render::ShaderProgramLayoutRecipe recipe;
    const render::ShaderLayoutSelector selector{.DeclarationName = "Values", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
    recipe.D3D12.BufferPlacements.push_back({selector, render::D3D12BufferPlacement::RootDescriptor});
    recipe.Vulkan.BufferDescriptors.push_back({selector, render::VulkanBufferDescriptorPlacement::Dynamic});
    auto program = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
struct Data { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Data> Values : register(b0);
[shader("vertex")] float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1); }
[shader("pixel")] float PSMain() : SV_Target0 { return Values.Value.x; }
)hlsl",
                                                   recipe);
    ASSERT_TRUE(program);
    array<unique_ptr<MaterialTechnique>, 2> techniques;
    for (size_t i = 0; i < techniques.size(); ++i) {
        MaterialPipelineState state;
        state.Primitive.Cull = render::CullMode::None;
        state.Primitive.FaceClockwise = i ? render::FrontFace::CW : render::FrontFace::CCW;
        state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
        auto technique = MaterialTechnique::Create({{"AllocationLit", program.Get(), "Values", state}}, "AllocationLit");
        ASSERT_TRUE(technique);
        techniques[i] = technique.Release();
    }
    array<unique_ptr<Material>, 8> materials;
    for (size_t i = 0; i < materials.size(); ++i) {
        auto material = Material::Create(techniques[i % techniques.size()].get());
        ASSERT_TRUE(material);
        materials[i] = material.Release();
    }
    const array<float, 9> vertices{-1, -1, 0, 3, -1, 0, -1, 3, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    auto vertex = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{vertices}), render::BufferUse::Vertex);
    auto index = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertex && index);
    GpuMesh::DrawData geometry;
    geometry.VertexBuffers = {{0, {vertex.Get(), 0, sizeof(vertices)}}};
    geometry.Ibv = {index.Get(), 0, 4};
    geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    Scene scene;
    for (uint32_t i = 0; i < kAllocationPrimitives; ++i)
        ASSERT_TRUE(scene.AddPrimitive(make_unique<AllocationPrimitive>(geometry, *materials[i % materials.size()])));
    const PassPolicy policy{{3191}, 1, "AllocationLit", CompileAllocationPolicy};
    array<unique_ptr<AllocationFlight>, 3> flights;
    for (auto& flight : flights) flight = make_unique<AllocationFlight>(device, *Registry);
    static const uint8_t wire = 0;
    for (uint32_t frame = 0; frame < kAllocationWarmup + kAllocationFrames; ++frame) {
        auto& flight = *flights[frame % flights.size()];
        flight.Reset();
        flight.Frame = frame;
        flight.GraphResources.BeginFlight(frame + 1, flight.Writes);
        ASSERT_TRUE(flight.Groups.BeginFrame(flight.Writes));
        for (size_t i = 0; i < materials.size(); ++i)
            ASSERT_TRUE(materials[i]->SetFloat4("Value", {float(i + 1) / 16 + (frame % 2 ? .125f : 0.f), 0, 0, 1}));
        ASSERT_TRUE(scene.GetDrawStore().SetActivePolicies(frame + 1, std::span{&policy, 1}));
        ASSERT_TRUE(scene.GetRenderState().Publish(scene, flight.Snapshot, flight.Owners, RenderValidationMode::Off, frame + 1));
        ASSERT_TRUE(flight.Snapshot.Valid);
        ASSERT_EQ(flight.Snapshot.DrawRecords.size(), kAllocationPrimitives);
        ASSERT_EQ(flight.Snapshot.Materials.size(), materials.size());
        array<FrameDrawBindingId, 8> bindings;
        for (size_t i = 0; i < flight.Snapshot.Materials.size(); ++i) {
            const auto& material = flight.Snapshot.Materials[i];
            ASSERT_EQ(material.Passes.size(), 1u);
            ASSERT_GE(material.Passes[0].NumericBytes.size(), sizeof(float));
            const auto group = flight.Groups.PrepareGroupId(*program, 0,
                                                            {.Source = &scene, .WireType = &wire, .Generation = material.Generation, .Revision = material.ValuesRevision}, static_cast<uint32_t>(i), material.Passes[0].NumericBytes);
            ASSERT_TRUE(group.IsValid());
            bindings[i] = flight.Groups.InternBinding(std::span{&group, 1});
            ASSERT_TRUE(bindings[i].IsValid());
        }
        const array<uint32_t, 3> counts = frame % 2 ? array<uint32_t, 3>{96, 8, 120} : array<uint32_t, 3>{16, 96, 48};
        for (size_t list = 0; list < flight.Lists.size(); ++list) {
            for (uint32_t row = 0; row < counts[list]; ++row) {
                const auto& record = flight.Snapshot.DrawRecords[row];
                ASSERT_EQ(record.Status, DrawRecordStatus::Ready);
                ASSERT_TRUE(record.Description.LayoutId.IsValid());
                ASSERT_NE(record.NormalStateId, 0u);
                ASSERT_LT(record.Material, bindings.size());
                ASSERT_TRUE(flight.Lists[list].AppendStatic(flight.Snapshot, row, flight.Groups, bindings[record.Material]));
            }
            ASSERT_TRUE(flight.Lists[list].Commands.empty());
            ASSERT_EQ(flight.Lists[list].GetDrawCount(), counts[list]);
        }
        RenderGraph graph{device, flight.GraphResources, *Registry, "static Ready allocation probe", kPerformanceRenderGraphRuntimeOptions};
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "ready result");
        struct Payload {
            AllocationFlight* Flight;
            render::RenderBackend Backend;
        };
        graph.AddRasterPass<Payload>("static Ready", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data.Flight = &flight; data.Backend = GetParam(); builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
            auto& current = *data.Flight;
            for (size_t i = 0; i < current.Lists.size(); ++i) {
                {
                    ready_allocations::Probe probe;
                    current.Ready[i] = PrepareRendererList(current.Lists[i], context);
                }
                const auto allocations = ready_allocations::Count, bytes = ready_allocations::Bytes;
                if (current.Frame >= kAllocationWarmup) {
                    ++current.Samples;
                    current.Allocations += allocations;
                    current.AllocatedBytes += bytes;
                    EXPECT_EQ(allocations, 0u) << "frame=" << current.Frame << " list=" << i;
                    EXPECT_EQ(bytes, 0u);
                } else current.ColdAllocations += allocations;
                if (!current.Ready[i]) return false;
            }
            return true; }, +[](const Payload& data, RenderGraphRasterContext& context) {
            context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 4, 4)); context.Encoder().SetScissor({0, 0, 4, 4});
            for (size_t i = 0; i < data.Flight->Ready.size(); ++i) {
                DrawExecutionStats stats;
                RecordRendererList(*data.Flight->Ready[i], context, stats);
                EXPECT_TRUE(stats.Succeeded());
                EXPECT_EQ(stats.Draws, data.Flight->Lists[i].GetDrawCount());
                EXPECT_GT(stats.Draws, 0u);
            } });
        const bool checkPixels = frame < 6 || frame >= kAllocationWarmup + kAllocationFrames - 6;
        const auto readback = checkPixels ? graph.ReadbackTexture("oracle pixel", color) : RgReadbackTicket{};
        auto command = device.CreateCommandBuffer(Context.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        const auto result = RenderGraphTestDriver::Execute(graph, *command);
        flight.Writes.Flush(device);
        command->End();
        if (result.CommandsRecorded) {
            auto* raw = command.Get();
            Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
            RenderGraphTestDriver::Submitted(raw);
            Context.Queue->Wait();
            RenderGraphTestDriver::Completed(raw);
        }
        ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
        EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, counts[0] + counts[1] + counts[2]);
        EXPECT_EQ(graph.GetReport().ValidateReadyFrameCalls, 0u);
        if (frame >= kAllocationWarmup) EXPECT_EQ(graph.GetReport().GraphicsPipelineCreations, 0u);
        if (checkPixels) {
            vector<byte> pixels;
            ASSERT_TRUE(readback.Read(pixels));
            ASSERT_GE(pixels.size(), sizeof(float));
            float actual, expected;
            std::memcpy(&actual, pixels.data(), sizeof(float));
            const auto material = flight.Snapshot.DrawRecords[counts[2] - 1].Material;
            std::memcpy(&expected, flight.Snapshot.Materials[material].Passes[0].NumericBytes.data(), sizeof(float));
            EXPECT_FLOAT_EQ(actual, expected);
        }
    }
    uint64_t samples = 0, cold = 0;
    for (const auto& flight : flights) {
        samples += flight->Samples;
        cold += flight->ColdAllocations;
        EXPECT_GT(flight->Samples, 0u);
        EXPECT_EQ(flight->Allocations, 0u);
        EXPECT_EQ(flight->AllocatedBytes, 0u);
    }
    EXPECT_EQ(samples, uint64_t{kAllocationFrames} * 3);
    EXPECT_GT(cold, 0u);
    EXPECT_EQ(program->GetGraphicsPipelineStateCount(), 2u);
}
INSTANTIATE_TEST_SUITE_P(Backends, ReadyAllocationsTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
