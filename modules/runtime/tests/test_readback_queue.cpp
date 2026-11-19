#include "../../gpu_impl/tests/gpu_test_fixture.h"

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
        EXPECT_TRUE(Reads.Poll(3, UINT64_MAX, info));
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
    ASSERT_TRUE(reads.Poll(3, 0, info));
    EXPECT_FALSE(info.Ready);
    reads.CancelUnsubmitted();
    EXPECT_FALSE(reads.HasRequest(2));
    scheduler->WaitIdle();
    const auto completed = fence->GetCompletedValue();
    ASSERT_TRUE(reads.Poll(3, completed, info));
    EXPECT_TRUE(info.Ready);
    ASSERT_EQ(info.PackedByteSize, 7u * 3 * 4);
    EXPECT_TRUE(reads.Poll(3, completed, info));
    vector<byte> bytes(info.PackedByteSize);
    EXPECT_FALSE(reads.Poll(3, completed, info, bytes.data(), 0));
    EXPECT_FALSE(reads.Poll(3, completed, info, bytes.data(), bytes.size() - 1));
    ASSERT_TRUE(reads.Poll(3, completed, info, bytes.data(), bytes.size()));
    for (size_t i = 0; i < bytes.size(); i += 4) {
        EXPECT_EQ(bytes[i], byte{0}); EXPECT_EQ(bytes[i + 1], byte{255});
        EXPECT_EQ(bytes[i + 2], byte{0}); EXPECT_EQ(bytes[i + 3], byte{255});
    }
    EXPECT_FALSE(reads.Poll(3, completed, info));
}
INSTANTIATE_TEST_SUITE_P(Backends, ReadbackQueueTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);
}  // namespace
}  // namespace radray
