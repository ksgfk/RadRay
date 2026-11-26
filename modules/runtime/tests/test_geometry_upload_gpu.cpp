#include "../../gpu_impl/tests/gpu_test_fixture.h"
#include "test_fakes.h"

#include <thread>

#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/dynamic_buffer_arena.h>
#include <radray/runtime/staging_buffer_pool.h>

namespace radray {
namespace {

using namespace gpu;

class GeometryUploadGpuTest : public gpu::test::BackendTest {
protected:
    enum class Source { Pool, MappedPool, Arena, Persistent, PartiallyFlushed };
    void DrawPooledGeometry(bool separateUploadFrame, Source source = Source::Pool) {
        const bool dynamic = source == Source::Arena;
        auto& device = GetDevice();
        auto& queue = gpu::test::DirectQueue(device);
        DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
        const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
        const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3}};
        const auto memory = source == Source::MappedPool ? GpuBufferMemory::DeviceUploadWithFallback : GpuBufferMemory::DeviceLocal;
        auto vertices = VertexBufferPool::Create(device, {.VertexStride = 12, .PageVertexCapacity = 4, .AdditionalUsage = BufferUse::Resource | BufferUse::CopySource, .Layouts = layouts, .Attributes = attributes, .Memory = memory});
        auto indices = IndexBufferPool::Create(device, {.PageIndexCapacity = 4, .AdditionalUsage = BufferUse::CopySource, .Memory = memory});
        ASSERT_TRUE(vertices && indices);
        auto unusedVertex = vertices->Allocate(1);
        auto unusedIndex = indices->Allocate(1);
        auto vertex = vertices->Allocate(3);
        auto index = indices->Allocate(3);
        ASSERT_TRUE(unusedVertex && unusedIndex && vertex && index);
        const array positions{-1.f, -1.f, 0.5f, 3.f, -1.f, 0.5f, -1.f, 3.f, 0.5f};
        const array<uint32_t, 3> order{0, 1, 2};

        IntrusivePtr<GpuBuffer> uploadedVertices, uploadedIndices;
        if (source == Source::Persistent) {
            std::jthread worker{[&] {
                uploadedVertices = GpuBuffer::Create(device, {sizeof(positions), BufferUse::Vertex, GpuBufferMemory::DeviceUploadWithFallback}).Unwrap();
                uploadedIndices = GpuBuffer::Create(device, {sizeof(order), BufferUse::Index, GpuBufferMemory::DeviceUploadWithFallback}).Unwrap();
                std::memcpy(uploadedVertices->GetMappedData().Unwrap(), positions.data(), sizeof(positions));
                std::memcpy(uploadedIndices->GetMappedData().Unwrap(), order.data(), sizeof(order));
            }};
            worker.join();
            ASSERT_TRUE(uploadedIndices->FlushMappedRange({0, sizeof(order)}));
            ASSERT_TRUE(uploadedVertices->FlushMappedRange({0, sizeof(positions)}));
        }

        unique_ptr<Buffer> partialVertices, partialIndices;
        if (source == Source::PartiallyFlushed) {
            const auto createPartial = [&](BufferUse usage, std::span<const byte> data) {
                auto buffer = device.CreateBuffer({.Size = 8192, .Memory = MemoryType::HostUpload,
                                                   .Usage = BufferUse::MapWrite | usage}).Unwrap();
                auto* mapped = buffer->GetMappedData().Get();
                if (!mapped) return unique_ptr<Buffer>{};
                std::memcpy(mapped, data.data(), data.size());
                buffer->FlushMappedRange({0, 4096});

                return buffer;
            };
            partialVertices = createPartial(BufferUse::Vertex, gpu::test::AsBytes(positions));
            partialIndices = createPartial(BufferUse::Index, gpu::test::AsBytes(order));
            ASSERT_TRUE(partialVertices && partialIndices);
        }

        auto vs = gpu::test::LoadShader(device, "geometry", "VSMain", ShaderStage::Vertex).Unwrap();
        auto ps = gpu::test::LoadShader(device, "graphics", "SolidPS", ShaderStage::Pixel).Unwrap();
        struct DrawData {
            array<float, 4> Color;
            float Depth;
        };
        const DrawData draw{{1, 0, 0, 1}, 0.5f};
        const PushConstantDescriptor push{.PushId = 0, .Size = sizeof(draw), .Stages = ShaderStage::Graphics, .D3D12 = PushConstantDescriptor::D3D12Location{0, 0}, .Vulkan = PushConstantDescriptor::VulkanLocation{0}};
        auto pipelineLayout = device.CreatePipelineLayout({{}, std::span{&push, 1}}).Unwrap();
        const VertexAttribute attribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0};
        auto primitive = PrimitiveState::Default();
        primitive.Cull = CullMode::None;
        const auto colorState = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
        auto pso = device.CreateGraphicsPipelineState({.PipelineLayout = pipelineLayout.get(),
                                                       .VS = ShaderEntry{vs.get(), "VSMain"},
                                                       .PS = ShaderEntry{ps.get(), "SolidPS"},
                                                       .VertexInput = {layouts, std::span{&attribute, 1}},
                                                       .Primitive = primitive,
                                                       .MultiSample = MultiSampleState::Default(),
                                                       .ColorTargets = std::span{&colorState, 1}})
                       .Unwrap();
        auto target = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
        RDGTransientResourcePool transient{device};
        StagingBufferPool staging{device, dynamic ? 64u : 16u};
        DynamicBufferArena uploads{device, dynamic ? 64u : 16u};
        Nullable<CommandBuffer*> recorded{nullptr};
        Nullable<CompiledRDG*> compiled{nullptr};
        auto pipeline = make_unique<radray::test::CallbackPipeline>();
        bool drawFrame = !separateUploadFrame;
        bool uploadFrame = true;
        pipeline->OnExtract = [&](FrameSlot& slot) {
            RDGBuilder graph{slot};
            if (uploadFrame && (source == Source::Pool || source == Source::MappedPool)) {
                const auto vb = graph.ImportPersistentOwned(vertex->GetGpuBuffer());
                const auto ib = graph.ImportPersistentOwned(index->GetGpuBuffer());
                graph.UploadBuffer(vb, vertex->GetByteOffset(), RDGUploadSource::BorrowedBytes(gpu::test::AsBytes(positions)));
                graph.UploadBuffer(ib, index->GetByteOffset(), RDGUploadSource::BorrowedBytes(gpu::test::AsBytes(order)));
                graph.MarkOutput(vb);
                graph.MarkOutput(ib);
            }
            if (drawFrame) {
                const auto color = graph.ImportTexture(target.get(), TextureState::Undefined, TextureState::Common);
                graph.MarkOutput(color);
                struct PassData {
                    GraphicsPipelineState* Pipeline;
                    gpu::VertexBufferBinding Vertex;
                    gpu::IndexBufferView Index;
                    uint32_t Count, First;
                    int32_t Base;
                    DrawData Constants;
                    RDGBufferHandle DynamicVertex{}, DynamicIndex{};
                    bool ArenaOffsets;
                };
                RDGBufferHandle dynamicVertex, dynamicIndex;
                if (dynamic) {
                    const array<byte, 8> padding{};
                    const auto prefix = graph.CreateDynamicBuffer(RDGDynamicBufferDescriptor{8, BufferUse::Vertex}, RDGUploadSource::SnapshotBytes(padding));
                    graph.MarkOutput(prefix);
                    dynamicVertex = graph.CreateDynamicBuffer(RDGDynamicBufferDescriptor{sizeof(positions), BufferUse::Vertex, 4}, RDGUploadSource::BorrowedBytes(gpu::test::AsBytes(positions)));
                    const auto spacer = graph.CreateDynamicBuffer(RDGDynamicBufferDescriptor{32, BufferUse::Vertex}, RDGUploadSource::Generated(32, uint32_t{0}, +[](const uint32_t&, RDGUploadWriteContext& write) noexcept {
                        std::fill(write.Bytes.begin(), write.Bytes.end(), byte{0});
                        return true; }));
                    graph.MarkOutput(spacer);
                    dynamicIndex = graph.CreateDynamicBuffer(RDGDynamicBufferDescriptor{sizeof(order), BufferUse::Index, 32}, RDGUploadSource::BorrowedBytes(gpu::test::AsBytes(order)));
                }
                if (source == Source::Persistent) {
                    dynamicVertex = graph.ImportPersistentOwned(uploadedVertices);
                    dynamicIndex = graph.ImportPersistentOwned(uploadedIndices);
                    uploadedVertices.Reset();
                    uploadedIndices.Reset();
                }
                if (source == Source::PartiallyFlushed) {
                    dynamicVertex = graph.ImportBuffer(partialVertices.get(), BufferState::HostWrite, BufferState::HostWrite);
                    dynamicIndex = graph.ImportBuffer(partialIndices.get(), BufferState::HostWrite, BufferState::HostWrite);
                }
                auto pass = graph.AddRasterPass("draw pooled triangle", PassData{pso.get(), {0, {vertex->GetBuffer(), 0, vertex->GetBuffer()->GetDesc().Size}}, index->GetIndexBufferView(), index->GetIndexCount(), index->GetFirstIndex(), static_cast<int32_t>(vertex->GetFirstVertex()), draw, dynamicVertex, dynamicIndex, dynamic}, +[](const PassData& data, RDGRasterPassContext& ctx) noexcept {
                        auto& encoder = ctx.GetEncoder();
                        encoder.BindGraphicsPipelineState(data.Pipeline);
                        encoder.SetViewport({0, 0, 4, 4, 0, 1});
                        encoder.SetScissor({0, 0, 4, 4});
                        auto vertex = data.Vertex;
                        auto index = data.Index;
                        if (data.DynamicVertex.IsValid()) {
                            const auto v = ctx.ResolveBuffer(data.DynamicVertex), i = ctx.ResolveBuffer(data.DynamicIndex);
                            EXPECT_EQ(v.Offset, data.ArenaOffsets ? 8u : 0u);
                            EXPECT_EQ(i.Offset, data.ArenaOffsets ? 32u : 0u);
                            EXPECT_NE(v.Buffer, i.Buffer);
                            vertex = {0, {v.Buffer.Get(), v.Offset, v.Size}};
                            index = {i.Buffer.Get(), static_cast<uint32_t>(i.Offset), 4};
                        }
                        encoder.BindVertexBuffers(std::span{&vertex, 1});
                        encoder.BindIndexBuffer(index);
                        if (!encoder.SetPushConstants(0, 0, gpu::test::AsBytes(data.Constants))) return false;
                        encoder.DrawIndexed(data.Count, 1, data.DynamicVertex.IsValid() ? 0 : data.First, data.DynamicVertex.IsValid() ? 0 : data.Base, 0);
                        return true; });
                pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
                if (dynamicVertex.IsValid()) {
                    pass.ReadBuffer(dynamicVertex, BufferState::Vertex).ReadBuffer(dynamicIndex, BufferState::Index);
                } else {
                    vertices->AddPassReadAccess(graph, pass, BufferState::Vertex | BufferState::ShaderRead);
                    indices->AddPassReadAccess(graph, pass);
                }
            }
            compiled = graph.Compile();
            ASSERT_TRUE(compiled);
        };
        pipeline->OnPrepare = [&](FrameSlot& slot) {
            transient.BeginFrame();
            staging.BeginFrame();
            uploads.BeginFrame();
            ASSERT_TRUE(compiled->Prepare(slot, transient, staging, uploads));
            recorded = slot.AcquireCommandBuffer();
            ASSERT_TRUE(recorded);
        };
        pipeline->OnRecord = [&](FrameSlot&) {
            recorded->Begin();
            ASSERT_TRUE(compiled->Record(*recorded));
            recorded->End();
        };
        pipeline->OnSubmit = [&](FrameSlot&) {
            auto* raw = recorded.Get();
            ASSERT_TRUE(queue.Submit({.CmdBuffers = std::span{&raw, 1}}));
            compiled->CommitSubmittedStates();
        };
        pipeline->OnWait = [&](FrameSlot&) { queue.Wait(); };
        auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
        scheduler->RunFrame();
        if (separateUploadFrame) {
            drawFrame = true;
            uploadFrame = false;
            scheduler->RunFrame();
        }
        scheduler->WaitIdle();
        EXPECT_EQ(gpu::test::FromBytes<uint32_t>(gpu::test::ReadTexture(device, *target, TextureState::Common)), vector<uint32_t>(16, 0xff0000ff));
        if (source != Source::Pool) return;
        const auto actual = gpu::test::ReadBuffer(device, *vertex->GetBuffer(), BufferState::Common, vertex->GetBuffer()->GetDesc().Size);
        ASSERT_GE(actual.size(), vertex->GetByteOffset() + sizeof(positions));
        EXPECT_EQ(std::memcmp(actual.data() + vertex->GetByteOffset(), positions.data(), sizeof(positions)), 0);
    }
};

TEST_P(GeometryUploadGpuTest, UploadOnlyFrameThenDrawUsesCommonBoundary) { DrawPooledGeometry(true); }
TEST_P(GeometryUploadGpuTest, UploadsBothPoolsAndDrawsInOneFrame) { DrawPooledGeometry(false); }

TEST_P(GeometryUploadGpuTest, GraphWritesMappedGeometryPagesBeforeDrawing) { DrawPooledGeometry(true, Source::MappedPool); }

TEST_P(GeometryUploadGpuTest, DynamicVertexAndIndexUseSliceOffsetsAcrossPages) { DrawPooledGeometry(false, Source::Arena); }

TEST_P(GeometryUploadGpuTest, WorkerPublishedPersistentGeometryDrawsThroughOwnedImports) { DrawPooledGeometry(false, Source::Persistent); }

TEST_P(GeometryUploadGpuTest, IndexedDrawReadsOnlyTheFlushedVertexAndIndexPrefix) { DrawPooledGeometry(false, Source::PartiallyFlushed); }

TEST_P(GeometryUploadGpuTest, BeginFrameResetsReferencesBeforeUploadBufferRelease) {
    auto& device = GetDevice();
    auto& queue = gpu::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    auto upload = device.CreateBuffer({.Size = 16, .Memory = MemoryType::HostUpload, .Usage = BufferUse::CopySource | BufferUse::MapWrite}).Unwrap();
    const array<byte, 16> expected{byte{42}, byte{7}};
    auto* data = upload->GetMappedData().Get();
    ASSERT_NE(data, nullptr);
    std::memcpy(data, expected.data(), expected.size());
    upload->FlushMappedRange({0, expected.size()});
    auto destination = device.CreateBuffer({.Size = 16, .Usage = BufferUse::CopyDestination | BufferUse::CopySource}).Unwrap();
    auto pipeline = make_unique<radray::test::CallbackPipeline>();
    Nullable<CommandBuffer*> recorded{nullptr};
    pipeline->OnPrepare = [&](FrameSlot& slot) { recorded = slot.AcquireCommandBuffer(); };
    pipeline->OnRecord = [&](FrameSlot&) {
        auto* command = recorded.Get();
        command->Begin();
        gpu::test::Barrier(*command, {BarrierBufferDescriptor{upload.get(), BufferState::HostWrite, BufferState::CopySource},
                                         BarrierBufferDescriptor{destination.get(), BufferState::Common, BufferState::CopyDestination}});
        command->CopyBufferToBuffer(destination.get(), 0, upload.get(), 0, expected.size());
        command->End();
    };
    pipeline->OnSubmit = [&](FrameSlot&) {
        auto* command = recorded.Get();
        queue.Submit({.CmdBuffers = std::span{&command, 1}});
    };
    pipeline->OnWait = [&](FrameSlot&) { queue.Wait(); };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    scheduler->BeginFrame();

    upload.reset();
    const auto actual = gpu::test::ReadBuffer(device, *destination, BufferState::CopyDestination, expected.size());
    EXPECT_EQ(actual, (vector<byte>{expected.begin(), expected.end()}));
}

TEST_P(GeometryUploadGpuTest, MultiPageAllocFreeAndReallocTracksExactFreeBytes) {
    auto& device = GetDevice();
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3}};
    auto vertices = VertexBufferPool::Create(device, {.VertexStride = 12, .PageVertexCapacity = 8, .Layouts = layouts, .Attributes = attributes});
    auto indices = IndexBufferPool::Create(device, {.IndexStride = 4, .PageIndexCapacity = 12});
    ASSERT_TRUE(vertices && indices);

    EXPECT_EQ(vertices->GetFreeBytes(), 0u);
    EXPECT_EQ(indices->GetFreeBytes(), 0u);

    auto v0 = vertices->Allocate(6);   // page 0 (cap 8): free 2
    auto v1 = vertices->Allocate(5);   // page 1 (cap 8): free 3
    auto v2 = vertices->Allocate(10);  // page 2 (cap 10): free 0
    ASSERT_TRUE(v0 && v1 && v2);
    EXPECT_EQ(vertices->GetPageCount(), 3u);
    EXPECT_EQ(vertices->GetFreeBytes(), (2u + 3u + 0u) * 12u);

    v0.reset();
    EXPECT_EQ(vertices->GetFreeBytes(), (8u + 3u + 0u) * 12u);
    auto v3 = vertices->Allocate(3);
    ASSERT_TRUE(v3);
    EXPECT_EQ(vertices->GetPageCount(), 3u);
    EXPECT_EQ(vertices->GetFreeBytes(), (8u + 3u - 3u) * 12u);
    v1.reset();
    v2.reset();
    v3.reset();
    EXPECT_EQ(vertices->GetFreeBytes(), (8u + 8u + 10u) * 12u);

    auto i0 = indices->Allocate(9);   // page 0 (cap 12): free 3
    auto i1 = indices->Allocate(10);  // page 1 (cap 12): free 2
    auto i2 = indices->Allocate(16);  // page 2 (cap 16): free 0
    ASSERT_TRUE(i0 && i1 && i2);
    EXPECT_EQ(indices->GetPageCount(), 3u);
    EXPECT_EQ(indices->GetFreeBytes(), (3u + 2u + 0u) * 4u);
    i1.reset();
    auto i3 = indices->Allocate(4);
    ASSERT_TRUE(i3);
    EXPECT_EQ(indices->GetFreeBytes(), (3u + 12u - 4u) * 4u);
    i0.reset();
    i2.reset();
    i3.reset();
    EXPECT_EQ(indices->GetFreeBytes(), (12u + 12u + 16u) * 4u);
}

INSTANTIATE_TEST_SUITE_P(Backends, GeometryUploadGpuTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

}  // namespace
}  // namespace radray
