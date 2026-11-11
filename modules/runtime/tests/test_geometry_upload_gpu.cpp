#include "../../render/tests/gpu_test_fixture.h"
#include "test_fakes.h"

#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/upload_buffer.h>

namespace radray {
namespace {

using namespace render;

class GeometryUploadGpuTest : public render::test::BackendTest {
protected:
    void DrawPooledGeometry(bool separateUploadFrame) {
        auto& device = GetDevice();
        auto& queue = render::test::DirectQueue(device);
        DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
        const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
        const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
        auto vertices = VertexBufferPool::Create(device, {.VertexStride = 12, .PageVertexCapacity = 4, .AdditionalUsage = BufferUse::Resource | BufferUse::CopySource, .Layouts = layouts, .Attributes = attributes});
        auto indices = IndexBufferPool::Create(device, {.PageIndexCapacity = 4, .AdditionalUsage = BufferUse::CopySource});
        auto upload = BatchedBufferUpload::Create(device, 16);
        ASSERT_TRUE(vertices && indices && upload);
        auto unusedVertex = vertices->Allocate(1);
        auto unusedIndex = indices->Allocate(1);
        auto vertex = vertices->Allocate(3);
        auto index = indices->Allocate(3);
        ASSERT_TRUE(unusedVertex && unusedIndex && vertex && index);
        const array positions{-1.f, -1.f, 0.5f, 3.f, -1.f, 0.5f, -1.f, 3.f, 0.5f};
        const array<uint32_t, 3> order{0, 1, 2};
        auto first = upload->Acquire(index->GetBuffer(), index->GetByteOffset(), sizeof(order));
        auto second = upload->Acquire(vertex->GetBuffer(), vertex->GetByteOffset(), sizeof(positions));
        ASSERT_TRUE(first && second);
        EXPECT_EQ(upload->GetPageCount(), 2u);
        std::memcpy(first.Get(), order.data(), sizeof(order));
        std::memcpy(second.Get(), positions.data(), sizeof(positions));

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
        Nullable<CompiledRDG*> compiled{nullptr};
        auto pipeline = make_unique<radray::test::CallbackPipeline>();
        bool drawFrame = !separateUploadFrame;
        bool uploadFrame = true;
        pipeline->OnExtract = [&](FrameSlot& slot) {
            RDGBuilder graph{slot.GetArena()};
            if (uploadFrame) EXPECT_TRUE(upload->CommitToGraph(graph));
            if (drawFrame) {
                const auto color = graph.ImportTexture(target.get(), TextureState::Undefined, TextureState::Common);
                graph.MarkOutput(color);
                graph.AddRasterPass("draw pooled triangle", [&](RDGRasterPassBuilder& pass) {
                    pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
                    vertices->AddPassReadAccess(graph, pass, BufferState::Vertex | BufferState::ShaderRead);
                    indices->AddPassReadAccess(graph, pass); }, [&](RDGRasterPassContext& ctx) {
                    auto& encoder = ctx.GetEncoder();
                    encoder.BindGraphicsPipelineState(pso.get());
                    encoder.SetViewport({0, 0, 4, 4, 0, 1});
                    encoder.SetScissor({0, 0, 4, 4});
                    const VertexBufferBinding vb{0, {vertex->GetBuffer(), 0, vertex->GetBuffer()->GetDesc().Size}};
                    encoder.BindVertexBuffers(std::span{&vb, 1});
                    encoder.BindIndexBuffer(index->GetIndexBufferView());
                    if (!encoder.SetPushConstants(0, 0, render::test::AsBytes(draw))) return false;
                    encoder.DrawIndexed(index->GetIndexCount(), 1, index->GetFirstIndex(), static_cast<int32_t>(vertex->GetFirstVertex()), 0);
                    return true; });
            }
            compiled = graph.Compile();
            ASSERT_TRUE(compiled);
        };
        pipeline->OnRecord = [&](FrameSlot& slot) {
            ASSERT_TRUE(compiled);
            transient.BeginFrame();
            auto command = slot.AcquireCommandBuffer();
            ASSERT_TRUE(command);
            command->Begin();
            ASSERT_TRUE(compiled->Execute(slot, transient, *command));
            command->End();
            auto* raw = command.Get();
            queue.Submit({.CmdBuffers = std::span{&raw, 1}});
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
        const auto actual = render::test::ReadBuffer(device, *vertex->GetBuffer(), BufferState::Common, vertex->GetBuffer()->GetDesc().Size);
        ASSERT_GE(actual.size(), vertex->GetByteOffset() + sizeof(positions));
        EXPECT_EQ(std::memcmp(actual.data() + vertex->GetByteOffset(), positions.data(), sizeof(positions)), 0);
    }
};

TEST_P(GeometryUploadGpuTest, UploadOnlyFrameThenDrawUsesCommonBoundary) { DrawPooledGeometry(true); }
TEST_P(GeometryUploadGpuTest, UploadsBothPoolsAndDrawsInOneFrame) { DrawPooledGeometry(false); }

TEST_P(GeometryUploadGpuTest, BeginFrameResetsReferencesBeforeMappedBufferGrowth) {
    auto& device = GetDevice();
    auto& queue = render::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    auto mapped = MappedUploadBuffer::Create(device, 16).Unwrap();
    auto destination = device.CreateBuffer({.Size = 16, .Usage = BufferUse::CopyDestination}).Unwrap();
    auto pipeline = make_unique<radray::test::CallbackPipeline>();
    pipeline->OnRecord = [&](FrameSlot& slot) {
        auto command = slot.AcquireCommandBuffer().Unwrap();
        command->Begin();
        render::test::Barrier(*command, {BarrierBufferDescriptor{mapped->Get(), BufferState::HostWrite, BufferState::CopySource},
                                         BarrierBufferDescriptor{destination.get(), BufferState::Common, BufferState::CopyDestination}});
        command->CopyBufferToBuffer(destination.get(), 0, mapped->Get(), 0, 16);
        command->End();
        queue.Submit({.CmdBuffers = std::span{&command, 1}});
    };
    pipeline->OnWait = [&](FrameSlot&) { queue.Wait(); };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    scheduler->BeginFrame();
    ASSERT_TRUE(mapped->Reserve(device, 64));
    EXPECT_EQ(mapped->GetCapacity(), 64u);
}

INSTANTIATE_TEST_SUITE_P(Backends, GeometryUploadGpuTest, testing::ValuesIn(render::test::AllBackends), render::test::BackendParamName);

}  // 匿名命名空间
}  // namespace radray
