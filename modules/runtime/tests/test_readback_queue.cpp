#include "../../gpu_impl/tests/gpu_test_fixture.h"

#include <thread>
#include "test_fakes.h"

#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/readback_queue.h>
#include <radray/runtime/render_graph_pipeline.h>

namespace radray {
namespace {

class ReadbackPipeline final : public RenderGraphFramePipeline {
public:
    ReadbackPipeline(DeviceContext& ctx, AsyncReadbackQueue& reads, gpu::Fence& fence)
        : RenderGraphFramePipeline(ctx, 1), Reads(reads), Fence(fence) {}
    void Wait(FrameSlot&) noexcept override { Fence.Wait(Submitted); }
    void ExtractFrameData(FrameSlot&) noexcept override {}
    void SetupGraph(RDGBuilder& graph, FrameSlot&) noexcept override {
        const auto target = graph.CreateTexture({.Dim = gpu::TextureDimension::Dim2D, .Width = 7, .Height = 3, .DepthOrArraySize = 1,
            .MipLevels = 1, .SampleCount = 1, .Format = gpu::TextureFormat::RGBA8_UNORM, .Usage = gpu::TextureUse::RenderTarget | gpu::TextureUse::CopySource});
        graph.AddClearColor(target, {{0, 1, 0, 1}});
        EXPECT_TRUE(Reads.ReserveTexture(2, {6, 3, gpu::TextureFormat::RGBA8_UNORM}));
        EXPECT_FALSE(Reads.EnqueueReserved(graph, 2, target));
        EXPECT_TRUE(Reads.ReserveTexture(3, {7, 3, gpu::TextureFormat::RGBA8_UNORM}));
        EXPECT_TRUE(Reads.EnqueueReserved(graph, 3, target));
        ReadbackResultInfo info{};
        EXPECT_EQ(Reads.Poll(3, UINT64_MAX, info), ReadbackStatus::Pending);
        EXPECT_FALSE(info.Ready);
    }
    bool SubmitFrame(FrameSlot& slot, gpu::CommandBuffer& command) noexcept override {
        auto* raw = &command;
        gpu::Fence* signal = &Fence;
        auto next = Submitted + 1;
        if (!slot.GetQueue().Submit({.CmdBuffers = {&raw, 1}, .SignalFences = {&signal, 1}, .SignalValues = {&next, 1}})) return false;
        Submitted = next;
        Reads.MarkSubmitted(Submitted);
        return true;
    }
    AsyncReadbackQueue& Reads;
    gpu::Fence& Fence;
    uint64_t Submitted{};
};

TEST(AsyncReadbackQueue, MapsAtReservationAndInvalidatesAtConsumption) {
    class CopyCommand final : public radray::test::FakeCommandBuffer {
    public:
        bool CopyTextureToBufferRegion(const gpu::TextureToBufferCopyDescriptor&) noexcept override { return true; }
    } command;
    radray::test::FakeGpu gpu;
    gpu.Device->EnableHostBuffers = true;
    gpu.Device->EnableTextures = true;
    AsyncReadbackQueue reads{*gpu.Device, 2};
    size_t maps = 0, invalidations = 0;
    gpu.Device->OnMap = [&] { ++maps; };
    gpu.Device->OnInvalidate = [&](gpu::BufferRange) { ++invalidations; };
    const ReadbackTexturePlan plan{2, 2, gpu::TextureFormat::RGBA8_UNORM};
    gpu.Device->FailMaps = true;
    EXPECT_FALSE(reads.ReserveBuffer(1, 16));
    EXPECT_FALSE(reads.ReserveTexture(2, plan));
    ASSERT_EQ(reads.GetActiveRequestCount(), 0u);
    gpu.Device->FailMaps = false;
    ASSERT_TRUE(reads.ReserveBuffer(1, 16));
    ASSERT_TRUE(reads.ReserveTexture(2, plan));
    ASSERT_EQ(maps, 4u);
    gpu.Device->FailMaps = true;

    auto buffer = gpu.Device->CreateBuffer({.Size = 16, .Usage = gpu::BufferUse::CopySource}).Unwrap();
    auto texture = gpu.Device->CreateTexture({.Dim = gpu::TextureDimension::Dim2D, .Width = 2, .Height = 2,
        .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = plan.Format, .Usage = gpu::TextureUse::CopySource}).Unwrap();
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<radray::test::CallbackPipeline>(), 1).Unwrap();
    auto& slot = scheduler->BeginFrame();
    RDGTransientResourcePool pool{*gpu.Device};
    UploadArena uploads{*gpu.Device, 64};
    RDGBuilder graph{slot};
    const auto srcBuffer = graph.ImportBuffer(buffer.get(), gpu::BufferState::CopySource);
    const auto srcTexture = graph.ImportTexture(texture.get(), gpu::TextureState::CopySource);
    ASSERT_TRUE(reads.EnqueueReservedBuffer(graph, 1, srcBuffer));
    ASSERT_TRUE(reads.EnqueueReserved(graph, 2, srcTexture));
    auto compiled = graph.Compile();
    ASSERT_TRUE(compiled);
    pool.BeginFrame();
    ASSERT_TRUE(compiled->Prepare(slot, pool, uploads));
    command.Begin();
    ASSERT_TRUE(compiled->Record(command));
    command.End();
    gpu::CommandBuffer* commands[]{&command};
    ASSERT_TRUE(gpu.Device->Queue.Submit({.CmdBuffers = commands}));
    reads.MarkSubmitted(1);
    array<byte, 16> bytes{};
    ReadbackResultInfo info{};
    for (uint64_t id : {1u, 2u}) {
        ASSERT_EQ(reads.Poll(id, 0, info), ReadbackStatus::Pending);
        EXPECT_FALSE(info.Ready);
        ASSERT_EQ(reads.Poll(id, 1, info, bytes.data(), bytes.size()), ReadbackStatus::Ready);
        EXPECT_TRUE(info.Ready);
    }
    EXPECT_EQ(invalidations, 2u);
    EXPECT_TRUE(reads.ReserveBuffer(3, 8));
    EXPECT_EQ(maps, 4u);
}

TEST(AsyncReadbackQueue, CancelledGraphCannotPrepareAReusedRequestId) {
    radray::test::FakeGpu gpu;
    gpu.Device->EnableHostBuffers = true;
    AsyncReadbackQueue reads{*gpu.Device, 1};
    auto source = gpu.Device->CreateBuffer({.Size = 16, .Usage = gpu::BufferUse::CopySource}).Unwrap();
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<radray::test::CallbackPipeline>(), 1).Unwrap();
    auto& slot = scheduler->BeginFrame();
    RDGTransientResourcePool pool{*gpu.Device};
    UploadArena uploads{*gpu.Device, 64};
    const auto build = [&](RDGBuilder& graph, uint64_t size) {
        auto handle = graph.ImportBuffer(source.get(), gpu::BufferState::CopySource);
        EXPECT_TRUE(reads.EnqueueBuffer(graph, 7, handle, 0, size));
        return graph.Compile();
    };
    RDGBuilder abandoned{slot};
    auto oldGraph = build(abandoned, 4);
    ASSERT_TRUE(oldGraph);
    reads.CancelUnsubmitted();
    RDGBuilder current{slot};
    auto newGraph = build(current, 16);
    ASSERT_TRUE(newGraph);
    pool.BeginFrame();
    EXPECT_FALSE(oldGraph->Prepare(slot, pool, uploads));
    ASSERT_TRUE(newGraph->Prepare(slot, pool, uploads));
    ReadbackResultInfo info{};
    EXPECT_EQ(reads.Poll(7, 0, info), ReadbackStatus::Pending);
    gpu.Device->Lost = true;
    EXPECT_EQ(reads.Poll(7, 0, info), ReadbackStatus::DeviceLost);
}

class ReadbackQueueTest : public gpu::test::BackendTest {};
TEST_P(ReadbackQueueTest, OnlySubmittedGraphCanBeConsumedAndOnlyAfterCopy) {
    auto& device = GetDevice();
    DeviceContext context{shared_ptr<gpu::Device>{&device, [](gpu::Device*) {}}, &gpu::test::DirectQueue(device)};
    AsyncReadbackQueue reads{device, 2};
    auto fence = device.CreateFence().Unwrap();
    auto scheduler = FrameScheduler::Create(context, make_unique<ReadbackPipeline>(context, reads, *fence), 1).Unwrap();
    {
        RDGBuilder abandoned{scheduler->BeginFrame()};
        const auto source = abandoned.CreateTexture({.Dim = gpu::TextureDimension::Dim2D, .Width = 7, .Height = 3, .DepthOrArraySize = 1,
            .MipLevels = 1, .SampleCount = 1, .Format = gpu::TextureFormat::RGBA8_UNORM, .Usage = gpu::TextureUse::CopySource});
        EXPECT_TRUE(reads.ReserveTexture(1, {7, 3, gpu::TextureFormat::RGBA8_UNORM}));
        EXPECT_FALSE(reads.ReserveTexture(1, {7, 3, gpu::TextureFormat::RGBA8_UNORM}));
        EXPECT_TRUE(reads.EnqueueReserved(abandoned, 1, source));
    }
    reads.CancelUnsubmitted();
    scheduler->RunFrame();
    ReadbackResultInfo info{};
    EXPECT_FALSE(reads.HasRequest(1));
    EXPECT_EQ(reads.GetActiveRequestCount(), 2u);
    EXPECT_FALSE(reads.ReserveTexture(4, {7, 3, gpu::TextureFormat::RGBA8_UNORM}));
    ASSERT_EQ(reads.Poll(3, 0, info), ReadbackStatus::Pending);
    EXPECT_FALSE(info.Ready);
    reads.CancelUnsubmitted();
    EXPECT_FALSE(reads.HasRequest(2));
    scheduler->WaitIdle();
    const auto completed = fence->GetCompletedValue();
    ASSERT_EQ(reads.Poll(3, completed, info), ReadbackStatus::Ready);
    EXPECT_TRUE(info.Ready);
    ASSERT_EQ(info.PackedByteSize, 7u * 3 * 4);
    EXPECT_EQ(reads.Poll(3, completed, info), ReadbackStatus::Ready);
    vector<byte> bytes(info.PackedByteSize);
    EXPECT_EQ(reads.Poll(3, completed, info, bytes.data(), 0), ReadbackStatus::DestinationTooSmall);
    EXPECT_EQ(reads.Poll(3, completed, info, bytes.data(), bytes.size() - 1), ReadbackStatus::DestinationTooSmall);
    ASSERT_EQ(reads.Poll(3, completed, info, bytes.data(), bytes.size()), ReadbackStatus::Ready);
    for (size_t i = 0; i < bytes.size(); i += 4) {
        EXPECT_EQ(bytes[i], byte{0}); EXPECT_EQ(bytes[i + 1], byte{255});
        EXPECT_EQ(bytes[i + 2], byte{0}); EXPECT_EQ(bytes[i + 3], byte{255});
    }
    EXPECT_EQ(reads.Poll(3, completed, info), ReadbackStatus::InvalidRequest);
}
TEST_P(ReadbackQueueTest, PersistentUploadOwnedByGraphSuppliesBufferReadback) {
    EXPECT_GE(GetDevice().GetCapabilities().Limits.HostCoherentAtomSize, 1u);
    class Pipeline final : public RenderGraphFramePipeline {
    public:
        Pipeline(DeviceContext& ctx, AsyncReadbackQueue& reads, gpu::Fence& fence)
            : RenderGraphFramePipeline(ctx, 1), Reads(reads), Fence(fence) {}
        void Wait(FrameSlot&) noexcept override { Fence.Wait(1); }
        void ExtractFrameData(FrameSlot&) noexcept override {}
        void SetupGraph(RDGBuilder& graph, FrameSlot&) noexcept override {
            const auto src = graph.ImportPersistentOwned(Upload);
            Upload.Reset();
            EXPECT_TRUE(Reads.EnqueueBuffer(graph, 7, src, 4, 4));
            const array<byte, 4> data{byte{1}, byte{2}, byte{3}, byte{4}};
            const auto shortSource = graph.CreateBuffer(RDGDynamicBufferDescriptor{.MaxSize = 16, .Usage = gpu::BufferUse::CopySource},
                                                        RDGUploadSource::SnapshotBytes(data));
            EXPECT_FALSE(graph.IsDynamicBuffer(src));
            EXPECT_FALSE(graph.IsDynamicBuffer({}));
            EXPECT_TRUE(graph.IsDynamicBuffer(shortSource));
            EXPECT_TRUE(Reads.EnqueueBuffer(graph, 8, shortSource, 0, 8));
            EXPECT_TRUE(Reads.EnqueueBuffer(graph, 9, shortSource, 0, 4));
        }
        bool SubmitFrame(FrameSlot& slot, gpu::CommandBuffer& cmd) noexcept override {
            gpu::CommandBuffer* commands[]{&cmd};
            gpu::Fence* signals[]{&Fence};
            uint64_t values[]{1};
            if (!slot.GetQueue().Submit({.CmdBuffers = commands, .SignalFences = signals, .SignalValues = values})) return false;
            Reads.MarkSubmitted(1);
            return true;
        }
        AsyncReadbackQueue& Reads;
        gpu::Fence& Fence;
        IntrusivePtr<GpuUploadBuffer> Upload;
    };
    auto& device = GetDevice();
    DeviceContext ctx{shared_ptr<gpu::Device>{&device, [](gpu::Device*) {}}, &gpu::test::DirectQueue(device)};
    auto fence = device.CreateFence().Unwrap();
    AsyncReadbackQueue reads{device};
    auto pipeline = make_unique<Pipeline>(ctx, reads, *fence);
    const array<byte, 4> payload{byte{5}, byte{7}, byte{11}, byte{13}};
    std::jthread producer{[&] {
        pipeline->Upload = GpuUploadBuffer::Create(device, {.Size = 8, .Usage = gpu::BufferUse::CopySource}).Release();
        if (pipeline->Upload) EXPECT_TRUE(pipeline->Upload->WriteAndFlush(4, payload));
    }};
    producer.join();
    ASSERT_TRUE(pipeline->Upload);
    auto scheduler = FrameScheduler::Create(ctx, std::move(pipeline), 1).Unwrap();
    ASSERT_TRUE(scheduler->RunFrame());
    ReadbackResultInfo info{};
    EXPECT_EQ(reads.Poll(8, 0, info), ReadbackStatus::Pending);
    EXPECT_TRUE(reads.HasRequest(8));
    scheduler->WaitIdle();
    array<byte, 4> data{};
    ASSERT_EQ(reads.Poll(7, fence->GetCompletedValue(), info, data.data(), data.size()), ReadbackStatus::Ready);
    EXPECT_TRUE(info.Ready);
    EXPECT_EQ(info.PackedByteSize, 4u);
    EXPECT_EQ(data, payload);
    EXPECT_EQ(reads.Poll(8, fence->GetCompletedValue(), info), ReadbackStatus::SourceOutOfRange);
    EXPECT_FALSE(reads.HasRequest(8));
    ASSERT_EQ(reads.Poll(9, fence->GetCompletedValue(), info, data.data(), data.size()), ReadbackStatus::Ready);
    EXPECT_EQ(data[3], byte{4});
}

INSTANTIATE_TEST_SUITE_P(Backends, ReadbackQueueTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);
}  // namespace
}  // namespace radray
