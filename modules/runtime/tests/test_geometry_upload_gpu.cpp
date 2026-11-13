#include "../../render/tests/gpu_test_fixture.h"
#include "test_fakes.h"

#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/gpu_resource.h>

namespace radray {
namespace {

using namespace render;

class GeometryUploadGpuTest : public render::test::BackendTest {
protected:
    void DrawPooledGeometry(bool separateUploadFrame, bool dynamic = false) {
        auto& device = GetDevice();
        auto& queue = render::test::DirectQueue(device);
        DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
        const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
        const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
        auto vertices = VertexBufferPool::Create(device, {.VertexStride = 12, .PageVertexCapacity = 4, .AdditionalUsage = BufferUse::Resource | BufferUse::CopySource, .Layouts = layouts, .Attributes = attributes});
        auto indices = IndexBufferPool::Create(device, {.PageIndexCapacity = 4, .AdditionalUsage = BufferUse::CopySource});
        ASSERT_TRUE(vertices && indices);
        auto unusedVertex = vertices->Allocate(1);
        auto unusedIndex = indices->Allocate(1);
        auto vertex = vertices->Allocate(3);
        auto index = indices->Allocate(3);
        ASSERT_TRUE(unusedVertex && unusedIndex && vertex && index);
        const array positions{-1.f, -1.f, 0.5f, 3.f, -1.f, 0.5f, -1.f, 3.f, 0.5f};
        const array<uint32_t, 3> order{0, 1, 2};

        auto vs = render::test::LoadShader(device, "geometry", "VSMain", ShaderStage::Vertex).Unwrap();
        auto ps = render::test::LoadShader(device, "graphics", "SolidPS", ShaderStage::Pixel).Unwrap();
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
        UploadArena uploads{device, dynamic ? 64u : 16u};
        Nullable<CommandBuffer*> recorded{nullptr};
        Nullable<CompiledRDG*> compiled{nullptr};
        auto pipeline = make_unique<radray::test::CallbackPipeline>();
        bool drawFrame = !separateUploadFrame;
        bool uploadFrame = true;
        pipeline->OnExtract = [&](FrameSlot& slot) {
            RDGBuilder graph{slot};
            if (uploadFrame && !dynamic) {
                const auto vb = graph.ImportPersistentOwned(vertex->GetGpuBuffer());
                const auto ib = graph.ImportPersistentOwned(index->GetGpuBuffer());
                graph.UploadBuffer(vb, vertex->GetByteOffset(), RDGUploadSource::BorrowedBytes(render::test::AsBytes(positions)));
                graph.UploadBuffer(ib, index->GetByteOffset(), RDGUploadSource::BorrowedBytes(render::test::AsBytes(order)));
                graph.MarkOutput(vb);
                graph.MarkOutput(ib);
            }
            if (drawFrame) {
                const auto color = graph.ImportTexture(target.get(), TextureState::Undefined, TextureState::Common);
                graph.MarkOutput(color);
                struct PassData {
                    GraphicsPipelineState* Pipeline;
                    render::VertexBufferBinding Vertex;
                    render::IndexBufferView Index;
                    uint32_t Count, First;
                    int32_t Base;
                    DrawData Constants;
                    RDGBufferHandle DynamicVertex{}, DynamicIndex{};
                };
                RDGBufferHandle dynamicVertex, dynamicIndex;
                if (dynamic) {
                    const array<byte, 8> padding{};
                    const auto prefix = graph.CreateBuffer(RDGDynamicBufferDescriptor{8, BufferUse::CopySource}, RDGUploadSource::SnapshotBytes(padding));
                    graph.MarkOutput(prefix);
                    dynamicVertex = graph.CreateBuffer(RDGDynamicBufferDescriptor{sizeof(positions), BufferUse::Vertex, 4}, RDGUploadSource::BorrowedBytes(render::test::AsBytes(positions)));
                    const auto spacer = graph.CreateBuffer(RDGDynamicBufferDescriptor{32, BufferUse::CopySource}, RDGUploadSource::Generated(32, uint32_t{0}, +[](const uint32_t&, RDGUploadWriteContext& write) noexcept {
                        std::fill(write.Bytes.begin(), write.Bytes.end(), byte{0});
                        return true; }));
                    graph.MarkOutput(spacer);
                    dynamicIndex = graph.CreateBuffer(RDGDynamicBufferDescriptor{sizeof(order), BufferUse::Index, 32}, RDGUploadSource::BorrowedBytes(render::test::AsBytes(order)));
                }
                auto pass = graph.AddRasterPass("draw pooled triangle", PassData{pso.get(), {0, {vertex->GetBuffer(), 0, vertex->GetBuffer()->GetDesc().Size}}, index->GetIndexBufferView(), index->GetIndexCount(), index->GetFirstIndex(), static_cast<int32_t>(vertex->GetFirstVertex()), draw, dynamicVertex, dynamicIndex}, +[](const PassData& data, RDGRasterPassContext& ctx) noexcept {
                        auto& encoder = ctx.GetEncoder();
                        encoder.BindGraphicsPipelineState(data.Pipeline);
                        encoder.SetViewport({0, 0, 4, 4, 0, 1});
                        encoder.SetScissor({0, 0, 4, 4});
                        auto vertex = data.Vertex;
                        auto index = data.Index;
                        if (data.DynamicVertex.IsValid()) {
                            const auto v = ctx.ResolveBuffer(data.DynamicVertex), i = ctx.ResolveBuffer(data.DynamicIndex);
                            EXPECT_EQ(v.Offset, 8u);
                            EXPECT_EQ(i.Offset, 32u);
                            EXPECT_NE(v.Buffer, i.Buffer);
                            vertex = {0, {v.Buffer.Get(), v.Offset, v.Size}};
                            index = {i.Buffer.Get(), static_cast<uint32_t>(i.Offset), 4};
                        }
                        encoder.BindVertexBuffers(std::span{&vertex, 1});
                        encoder.BindIndexBuffer(index);
                        if (!encoder.SetPushConstants(0, 0, render::test::AsBytes(data.Constants))) return false;
                        encoder.DrawIndexed(data.Count, 1, data.DynamicVertex.IsValid() ? 0 : data.First, data.DynamicVertex.IsValid() ? 0 : data.Base, 0);
                        return true; });
                pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
                if (dynamic) {
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
            uploads.Reset();
            ASSERT_TRUE(compiled->Prepare(slot, transient, uploads));
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
            queue.Submit({.CmdBuffers = std::span{&raw, 1}});
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
        EXPECT_EQ(render::test::FromBytes<uint32_t>(render::test::ReadTexture(device, *target, TextureState::Common)), vector<uint32_t>(16, 0xff0000ff));
        if (dynamic) return;
        const auto actual = render::test::ReadBuffer(device, *vertex->GetBuffer(), BufferState::Common, vertex->GetBuffer()->GetDesc().Size);
        ASSERT_GE(actual.size(), vertex->GetByteOffset() + sizeof(positions));
        EXPECT_EQ(std::memcmp(actual.data() + vertex->GetByteOffset(), positions.data(), sizeof(positions)), 0);
    }
};

TEST_P(GeometryUploadGpuTest, UploadOnlyFrameThenDrawUsesCommonBoundary) { DrawPooledGeometry(true); }
TEST_P(GeometryUploadGpuTest, UploadsBothPoolsAndDrawsInOneFrame) { DrawPooledGeometry(false); }

TEST_P(GeometryUploadGpuTest, DynamicVertexAndIndexUseSliceOffsetsAcrossPages) { DrawPooledGeometry(false, true); }

TEST_P(GeometryUploadGpuTest, BeginFrameResetsReferencesBeforeUploadBufferRelease) {
    auto& device = GetDevice();
    auto& queue = render::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    auto upload = device.CreateBuffer({.Size = 16, .Memory = MemoryType::Upload, .Usage = BufferUse::CopySource | BufferUse::MapWrite, .Hints = ResourceHint::PersistentMap}).Unwrap();
    const array<byte, 16> expected{byte{42}, byte{7}};
    auto* data = upload->Map(0, expected.size());
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
        render::test::Barrier(*command, {BarrierBufferDescriptor{upload.get(), BufferState::HostWrite, BufferState::CopySource},
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
    upload->Unmap();
    upload.reset();
    const auto actual = render::test::ReadBuffer(device, *destination, BufferState::CopyDestination, expected.size());
    EXPECT_EQ(actual, (vector<byte>{expected.begin(), expected.end()}));
}

INSTANTIATE_TEST_SUITE_P(Backends, GeometryUploadGpuTest, testing::ValuesIn(render::test::AllBackends), render::test::BackendParamName);

}  // namespace
}  // namespace radray
