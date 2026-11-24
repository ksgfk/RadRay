#include "../../gpu_impl/tests/gpu_test_fixture.h"

#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/runtime/readback_queue.h>
#include <radray/runtime/texture_asset.h>

namespace radray {
namespace {

using namespace gpu;

class UploadPipeline final : public RenderGraphFramePipeline {
public:
    UploadPipeline(DeviceContext& device, uint32_t slots, uint64_t pageSize = 1024 * 1024) : RenderGraphFramePipeline(device, slots, pageSize) {}
    std::function<void(RDGBuilder&, FrameSlot&)> Build;
    void Wait(FrameSlot& slot) noexcept override { slot.GetQueue().Wait(); }

private:
    void ExtractFrameData(FrameSlot&) noexcept override {}
    void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept override { Build(graph, slot); }
    bool SubmitFrame(FrameSlot& slot, CommandBuffer& command) noexcept override {
        auto* raw = &command;
        return slot.GetQueue().Submit({.CmdBuffers = std::span{&raw, 1}});
    }
};

struct BufferCopyData {
    RDGBufferHandle Source, Destination;
    uint64_t Size;
};

class GpuTimingTest : public gpu::test::BackendTest {};
TEST_P(GpuTimingTest, PublishesOnlyRequestedScopesWithSubmittedUserSequence) {
    auto& device = GetDevice();
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &gpu::test::DirectQueue(device)};
    auto pipeline = make_unique<UploadPipeline>(context, 1);
    auto* runner = pipeline.get();
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    runner->Build = [](RDGBuilder& graph, FrameSlot&) {
        const TextureDescriptor desc{.Dim = TextureDimension::Dim2D, .Width = 16, .Height = 16, .DepthOrArraySize = 1,
            .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget};
        graph.AddClearColor(graph.CreateTexture(desc), {});
        const auto live = graph.CreateTexture(desc);
        graph.AddClearColor(live, {});
        struct Load {};
        graph.AddRasterPass("load existing color", Load{}, +[](const Load&, RDGRasterPassContext&) noexcept { return true; })
            .WriteColor(0, live, gpu::LoadAction::Load, gpu::StoreAction::Store);
        graph.MarkOutput(live);
    };
    uint64_t id = 19;
    for (auto mode : {RDGGpuProfilingMode::FrameAndPasses, RDGGpuProfilingMode::FrameOnly, RDGGpuProfilingMode::Off}) {
        auto& slot = scheduler->BeginFrame();
        runner->SetGpuProfiling(slot, mode, ++id);
        const auto frameId = slot.GetFrameId();
        scheduler->RunFrame();
        scheduler->WaitIdle();
        const auto result = runner->GetCompletedGpuTiming(slot);
        if (mode == RDGGpuProfilingMode::Off || !device.GetCapabilities().Features.TimestampQueries || !slot.GetQueue().GetTimestampValidBits()) {
            EXPECT_FALSE(result);
        } else {
            ASSERT_TRUE(result);
            EXPECT_EQ(result->UserSequence, id);
            EXPECT_EQ(result->FrameId, frameId);
            ASSERT_EQ(result->Passes.size(), mode == RDGGpuProfilingMode::FrameAndPasses ? 2u : 0u);
            if (!result->Passes.empty()) {
                EXPECT_EQ(result->Passes[0].Name, "ClearColor");
                EXPECT_EQ(result->Passes[1].Name, "load existing color");
            }
        }
    }
}
TEST_P(GpuTimingTest, ExternalQueryPoolSurvivesOverwriteAndIndependentCopyScheduling) {
    auto& device = GetDevice();
    auto& queue = gpu::test::DirectQueue(device);
    if (!device.GetCapabilities().Features.TimestampQueries || !queue.GetTimestampValidBits()) GTEST_SKIP();
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    auto queries = device.CreateQueryPool({QueryType::Timestamp, 2, "external RDG timestamps"}).Unwrap();
    auto result = gpu::test::CreateReadbackBuffer(device, 2 * sizeof(uint64_t)).Unwrap();
    auto pipeline = make_unique<UploadPipeline>(context, 1);
    pipeline->Build = [&](RDGBuilder& graph, FrameSlot&) {
        const auto reset = graph.AddCopyPass("reset", queries.get(), +[](QueryPool* const& pool, RDGCopyPassContext& ctx) noexcept {
            ctx.GetCommandBuffer().ResetQueryPool(pool, 0, 2);
            return true;
        }).NeverCull().GetHandle();
        graph.AddOrderBarrier(reset);
        const auto color = graph.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1,
            .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget});
        const auto stamp = graph.AddRasterPass("timestamps", queries.get(), +[](QueryPool* const& pool, RDGRasterPassContext& ctx) noexcept {
            auto* commands = ctx.GetEncoder().GetCommandBuffer();
            commands->WriteTimestamp({pool, QueryPipelineStage::Top, 0});
            commands->WriteTimestamp({pool, QueryPipelineStage::Bottom, 1});
            return true;
        }).WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull().GetHandle();
        graph.AddOrderBarrier(stamp);
        graph.AddClearColor(color, {});
        graph.MarkOutput(color);
        struct Resolve { QueryPool* Pool; RDGBufferHandle Output; };
        const auto output = graph.ImportBuffer(result.get(), BufferState::Undefined, BufferState::HostRead);
        const auto resolve = graph.AddCopyPass("resolve", Resolve{queries.get(), output}, +[](const Resolve& data, RDGCopyPassContext& ctx) noexcept {
            const auto slice = ctx.ResolveBuffer(data.Output);
            ctx.GetCommandBuffer().ResolveQueryData({data.Pool, 0, 2, slice.Buffer.Get(), slice.Offset});
            return true;
        }).WriteBuffer(output).NeverCull().GetHandle();
        graph.AddOrderBarrier(resolve);
    };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    scheduler->WaitIdle();
    const auto ticks = gpu::test::FromBytes<uint64_t>(gpu::test::MapAndRead(*result));
    ASSERT_EQ(ticks.size(), 2u);
    EXPECT_GT(ticks[0], 0u);
    EXPECT_GE(ticks[1], ticks[0]);
}
INSTANTIATE_TEST_SUITE_P(Backends, GpuTimingTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

class VolumeClearTest : public gpu::test::BackendTest {};
TEST_P(VolumeClearTest, ClearsEverySliceOfSingleMipColorTarget) {
    auto& device = GetDevice();
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &gpu::test::DirectQueue(device)};
    AsyncReadbackQueue readbacks{device};
    auto pipeline = make_unique<UploadPipeline>(context, 1);
    pipeline->Build = [&](RDGBuilder& graph, FrameSlot&) {
        const auto target = graph.CreateTexture({.Dim = TextureDimension::Dim3D, .Width = 7, .Height = 3, .DepthOrArraySize = 3,
            .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource});
        graph.AddClearColor(target, {{0, 0, 1, 1}});
        EXPECT_TRUE(readbacks.EnqueueTexture(graph, 1, target));
    };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    readbacks.MarkSubmitted(1);
    scheduler->WaitIdle();
    vector<byte> pixels(7 * 3 * 3 * 4);
    ReadbackResultInfo result{};
    ASSERT_TRUE(readbacks.Poll(1, 1, result, pixels.data(), pixels.size()));
    ASSERT_TRUE(result.Ready);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        EXPECT_EQ(pixels[i], byte{0});
        EXPECT_EQ(pixels[i + 1], byte{0});
        EXPECT_EQ(pixels[i + 2], byte{255});
        EXPECT_EQ(pixels[i + 3], byte{255});
    }
}
TEST_P(VolumeClearTest, PartialClearPreservesPixelsAcrossMipsAndArrayLayers) {
    auto& device = GetDevice();
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &gpu::test::DirectQueue(device)};
    AsyncReadbackQueue readbacks{device};
    auto pipeline = make_unique<UploadPipeline>(context, 1);
    pipeline->Build = [&](RDGBuilder& graph, FrameSlot&) {
        const auto target = graph.CreateTexture({.Dim = TextureDimension::Dim2DArray, .Width = 8, .Height = 4, .DepthOrArraySize = 2,
            .MipLevels = 2, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource});
        graph.AddClearColor(target, {{1, 0, 0, 1}}, {0, 2, 0, 1});
        graph.AddClearColor(target, {{0, 1, 0, 1}}, {0, 2, 1, 1});
        graph.AddClearColor(target, {{0, 0, 1, 1}}, {1, 1, 1, 1}, Rect{1, 0, 2, 1});
        for (uint32_t mip = 0; mip < 2; ++mip)
            for (uint32_t layer = 0; layer < 2; ++layer)
                EXPECT_TRUE(readbacks.EnqueueTexture(graph, 1 + 2 * mip + layer, target, mip, layer));
        EXPECT_TRUE(readbacks.ReserveTexture(5, {3, 1, TextureFormat::RGBA8_UNORM}));
        EXPECT_FALSE(readbacks.EnqueueReserved(graph, 5, target, 1, 1));
        EXPECT_FALSE(readbacks.EnqueueReserved(graph, 5, target, TextureRegion2D{1, 1, 2, 0, 3, 1}));
        EXPECT_TRUE(readbacks.EnqueueReserved(graph, 5, target, TextureRegion2D{1, 1, 1, 0, 3, 1}));
    };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    readbacks.MarkSubmitted(1);
    scheduler->WaitIdle();
    array<byte, 12> crop{};
    ReadbackResultInfo cropped{};
    ASSERT_TRUE(readbacks.Poll(5, 1, cropped, crop.data(), crop.size()));
    EXPECT_EQ(cropped.PackedRowPitch, 12u);
    EXPECT_EQ(gpu::test::FromBytes<uint32_t>(crop), (vector<uint32_t>{0xffff0000u, 0xffff0000u, 0xff00ff00u}));
    for (uint32_t mip = 0; mip < 2; ++mip) {
        for (uint32_t layer = 0; layer < 2; ++layer) {
            const uint32_t width = 8u >> mip, height = 4u >> mip;
            vector<byte> pixels(width * height * 4);
            ReadbackResultInfo info{};
            ASSERT_TRUE(readbacks.Poll(1 + 2 * mip + layer, 1, info, pixels.data(), pixels.size()));
            ASSERT_TRUE(info.Ready);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const uint32_t channel = mip == 0 ? 0 : (layer == 1 && y == 0 && x >= 1 && x < 3 ? 2 : 1);
                    for (uint32_t c = 0; c < 4; ++c)
                        EXPECT_EQ(pixels[(y * width + x) * 4 + c], byte(c == channel || c == 3 ? 255 : 0))
                            << "mip " << mip << ", layer " << layer << ", x " << x << ", y " << y;
                }
            }
        }
    }
}
INSTANTIATE_TEST_SUITE_P(Backends, VolumeClearTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

bool RecordBufferCopy(const BufferCopyData& data, RDGCopyPassContext& context) noexcept {
    const auto source = context.ResolveBuffer(data.Source), destination = context.ResolveBuffer(data.Destination);
    context.GetCommandBuffer().CopyBufferToBuffer(destination.Buffer.Get(), destination.Offset, source.Buffer.Get(), source.Offset, data.Size);
    return true;
}

class BufferUploadGpuTest : public gpu::test::BackendTest {
protected:
    void VerifyUploads(uint32_t slots) {
        auto& device = GetDevice();
        auto& queue = gpu::test::DirectQueue(device);
        DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
        auto buffer = GpuBuffer::Create(device, {.Size = 32, .Usage = BufferUse::CopySource}).Unwrap();
        auto middle = gpu::test::CreateReadbackBuffer(device, 32).Unwrap();
        auto output = gpu::test::CreateReadbackBuffer(device, 32).Unwrap();
        auto pipeline = make_unique<UploadPipeline>(context, slots);
        auto* runner = pipeline.get();
        auto scheduler = FrameScheduler::Create(context, std::move(pipeline), slots).Unwrap();
        vector<byte> original(32), expected(32);
        for (size_t i = 0; i < original.size(); ++i) original[i] = static_cast<byte>(i);
        const array patch{byte{91}, byte{92}, byte{93}, byte{94}};
        expected = original;
        std::copy(patch.begin(), patch.end(), expected.begin() + 4);
        runner->Build = [&](RDGBuilder& graph, FrameSlot&) {
            const auto dst = graph.ImportPersistentOwned(buffer);
            graph.UploadBuffer(dst, 0, RDGUploadSource::BorrowedBytes(std::span{original}.first(16)));
            graph.UploadBuffer(dst, 16, RDGUploadSource::BorrowedBytes(std::span{original}.subspan(16)));
            const auto before = graph.ImportBuffer(middle.get(), BufferState::HostRead, BufferState::HostRead);
            graph.AddCopyPass("middle consumer", BufferCopyData{dst, before, 32}, &RecordBufferCopy).ReadBuffer(dst).WriteBuffer(before).NeverCull();
            graph.UploadBuffer(dst, 4, RDGUploadSource::BorrowedBytes(patch));
            const auto result = graph.ImportBuffer(output.get(), BufferState::HostRead, BufferState::HostRead);
            graph.AddCopyPass("after patch", BufferCopyData{dst, result, 32}, &RecordBufferCopy).ReadBuffer(dst).WriteBuffer(result);
            graph.MarkOutput(result);
        };
        scheduler->RunFrame();
        scheduler->WaitIdle();
        EXPECT_EQ(gpu::test::MapAndRead(*middle), original);
        EXPECT_EQ(gpu::test::MapAndRead(*output), expected);
        const array tail{byte{101}, byte{102}, byte{103}};
        for (uint32_t frame = 0; frame <= slots; ++frame) {
            runner->Build = [&](RDGBuilder& graph, FrameSlot&) {
                const auto dst = graph.ImportPersistentOwned(buffer);
                EXPECT_EQ(dst, graph.ImportPersistentOwned(buffer));
                graph.UploadBuffer(dst, 28, RDGUploadSource::BorrowedBytes(tail));
                const auto result = graph.ImportBuffer(output.get(), BufferState::HostRead, BufferState::HostRead);
                graph.AddCopyPass("tail read", BufferCopyData{dst, result, 32}, &RecordBufferCopy).ReadBuffer(dst).WriteBuffer(result);
                graph.MarkOutput(result);
                if (frame == slots) buffer.Reset();
            };
            scheduler->RunFrame();
        }
        scheduler->WaitIdle();
        std::copy(tail.begin(), tail.end(), expected.begin() + 28);
        EXPECT_EQ(gpu::test::MapAndRead(*output), expected);
    }
};

TEST_P(BufferUploadGpuTest, OverlapIntermediateConsumerTailAndOwnershipSingleSlot) { VerifyUploads(1); }
TEST_P(BufferUploadGpuTest, OverlapIntermediateConsumerTailAndOwnershipThreeSlots) { VerifyUploads(3); }

TEST_P(BufferUploadGpuTest, ManySmallMixedUploadsAndOverlappingTilesReadBack) {
    auto& device = GetDevice();
    auto& queue = gpu::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    auto buffer = GpuBuffer::Create(device, {.Size = 2048, .Usage = BufferUse::CopySource}).Unwrap();
    auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 32, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::CopyDestination | TextureUse::CopySource | TextureUse::Resource}).Unwrap();
    auto middle = gpu::test::CreateReadbackBuffer(device, 2048).Unwrap();
    auto output = gpu::test::CreateReadbackBuffer(device, 2048).Unwrap();
    vector<uint32_t> original(512), expected(512);
    for (uint32_t i = 0; i < 512; ++i) original[i] = i | 0xff000000;
    expected = original;
    expected[1] = 0xffabcdef;
    auto pipeline = make_unique<UploadPipeline>(context, 1);
    pipeline->Build = [&](RDGBuilder& graph, FrameSlot&) {
        const auto dst = graph.ImportPersistentOwned(buffer);
        const auto image = graph.ImportTexture(texture.get(), TextureState::Undefined, TextureState::ShaderRead);
        for (uint32_t i = 0; i < 512; ++i) {
            const auto bytes = std::as_bytes(std::span{original}.subspan(i, 1));
            graph.UploadBuffer(dst, i * 4, RDGUploadSource::BorrowedBytes(bytes));
            graph.UploadTexture(image, {.X = i % 32, .Y = i / 32, .Width = 1, .Height = 1}, RDGTextureUploadSource::BorrowedBytes(bytes, 4));
        }
        const auto before = graph.ImportBuffer(middle.get(), BufferState::HostRead, BufferState::HostRead);
        graph.AddCopyPass("before patch", BufferCopyData{dst, before, 2048}, &RecordBufferCopy).ReadBuffer(dst).WriteBuffer(before).NeverCull();
        const auto patch = std::as_bytes(std::span{expected}.subspan(1, 1));
        graph.UploadBuffer(dst, 4, RDGUploadSource::BorrowedBytes(patch));
        graph.UploadBuffer(dst, 0, RDGUploadSource::BorrowedBytes({}));
        graph.UploadTexture(image, {.X = 1, .Width = 1, .Height = 1}, RDGTextureUploadSource::BorrowedBytes(std::as_bytes(std::span{original}.subspan(1, 1)), 4));
        graph.UploadTexture(image, {.X = 1, .Width = 1, .Height = 1}, RDGTextureUploadSource::BorrowedBytes(patch, 4));
        const auto result = graph.ImportBuffer(output.get(), BufferState::HostRead, BufferState::HostRead);
        graph.AddCopyPass("after patch", BufferCopyData{dst, result, 2048}, &RecordBufferCopy).ReadBuffer(dst).WriteBuffer(result);
        graph.MarkOutput(result);
        graph.MarkOutput(image);
    };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    scheduler->WaitIdle();
    EXPECT_EQ(gpu::test::FromBytes<uint32_t>(gpu::test::MapAndRead(*middle)), original);
    EXPECT_EQ(gpu::test::FromBytes<uint32_t>(gpu::test::MapAndRead(*output)), expected);
    EXPECT_EQ(buffer->GetSubmittedState(), BufferStates{BufferState::Common});
    EXPECT_EQ(gpu::test::FromBytes<uint32_t>(gpu::test::ReadTexture(device, *texture, TextureState::ShaderRead)), expected);
}

TEST_P(BufferUploadGpuTest, ExternalSubmissionPublishesCopyDestinationForRdgReadback) {
    auto& device = GetDevice();
    auto& queue = gpu::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    const array<uint32_t, 4> values{17, 29, 41, 53};
    const auto bytes = std::as_bytes(std::span{values});
    auto buffer = GpuBuffer::Create(device, {.Size = bytes.size(), .Usage = BufferUse::CopySource}).Unwrap();
    auto source = gpu::test::CreateUploadBuffer(device, bytes, BufferUse::CopySource).Unwrap();
    auto readback = gpu::test::CreateReadbackBuffer(device, bytes.size()).Unwrap();
    auto external = gpu::test::BeginRecording(device);
    gpu::test::Barrier(*external.Commands, {
        BarrierBufferDescriptor{source.get(), BufferState::HostWrite, BufferState::CopySource},
        BarrierBufferDescriptor{buffer->GetBuffer(), BufferState::Common, BufferState::CopyDestination}});
    external.Commands->CopyBufferToBuffer(buffer->GetBuffer(), 0, source.get(), 0, bytes.size());
    external.Commands->End();
    auto* command = external.Commands.get();
    queue.Submit({.CmdBuffers = std::span{&command, 1}});
    buffer->CommitSubmittedState(BufferState::CopyDestination);

    auto pipeline = make_unique<UploadPipeline>(context, 1);
    pipeline->Build = [&](RDGBuilder& graph, FrameSlot&) {
        const auto input = graph.ImportPersistentOwned(buffer);
        const auto output = graph.ImportBuffer(readback.get(), BufferState::HostRead, BufferState::HostRead);
        graph.AddCopyPass("read external upload", BufferCopyData{input, output, bytes.size()}, &RecordBufferCopy).ReadBuffer(input).WriteBuffer(output);
        graph.MarkOutput(output);
    };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    EXPECT_EQ(buffer->GetSubmittedState(), BufferStates{BufferState::Common});
    scheduler->WaitIdle();
    EXPECT_EQ(gpu::test::FromBytes<uint32_t>(gpu::test::MapAndRead(*readback)), (vector<uint32_t>{values.begin(), values.end()}));
}

TEST_P(BufferUploadGpuTest, DynamicCopyUsesNonzeroSliceOffsetWithoutDeviceUploadCopy) {
    auto& device = GetDevice();
    auto& queue = gpu::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    auto output = gpu::test::CreateReadbackBuffer(device, 16).Unwrap();
    auto pipeline = make_unique<UploadPipeline>(context, 1);
    pipeline->Build = [&](RDGBuilder& graph, FrameSlot&) {
        const array<byte, 16> first{byte{1}}, second{byte{42}};
        const auto a = graph.CreateBuffer(RDGDynamicBufferDescriptor{16, BufferUse::CopySource}, RDGUploadSource::SnapshotBytes(first));
        const auto b = graph.CreateBuffer(RDGDynamicBufferDescriptor{16, BufferUse::CopySource}, RDGUploadSource::SnapshotBytes(second));
        graph.MarkOutput(a);
        const auto out = graph.ImportBuffer(output.get(), BufferState::HostRead, BufferState::HostRead);
        graph.AddCopyPass("slice", BufferCopyData{b, out, 16}, +[](const BufferCopyData& data, RDGCopyPassContext& context) noexcept {
            const auto slice = context.ResolveBuffer(data.Source);
            EXPECT_GT(slice.Offset, 0u);
            EXPECT_EQ(slice.Buffer->GetDesc().Usage, BufferUse::MapWrite | BufferUse::CopySource);
            return RecordBufferCopy(data, context); }).ReadBuffer(b).WriteBuffer(out);
        graph.MarkOutput(out);
    };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    scheduler->WaitIdle();
    EXPECT_EQ(gpu::test::MapAndRead(*output)[0], byte{42});
}

TEST_P(BufferUploadGpuTest, ConstantsUseNonzeroOffsetsAcrossPagesAndReuseParameterSets) {
    auto& device = GetDevice();
    auto& queue = gpu::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    auto result = device.CreateBuffer({.Size = 8, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    auto readback = gpu::test::CreateReadbackBuffer(device, 8).Unwrap();
    const array bindings{
        BindingDescriptor{.BindingId = 0, .Kind = BindingKind::RWStructuredBuffer, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{GetBindingRegisterClass(BindingKind::RWStructuredBuffer), 0, 0, D3D12BufferPlacement::Table}, .Vulkan = VulkanBindingDescriptor{0, 0}},
        BindingDescriptor{.BindingId = 2, .Kind = BindingKind::CBuffer, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{GetBindingRegisterClass(BindingKind::CBuffer), 0, 0, D3D12BufferPlacement::Table}, .Vulkan = VulkanBindingDescriptor{0, 2}}};
    auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
    const PipelineLayoutGroupDescriptor layoutGroup{0, group.get()};
    auto layout = device.CreatePipelineLayout({.Groups = std::span{&layoutGroup, 1}, .PushConstants = {}}).Unwrap();
    auto shader = gpu::test::LoadShader(device, "compute", "ReadConstants", ShaderStage::Compute).Unwrap();
    auto kernel = device.CreateComputePipelineState({.PipelineLayout = layout.get(), .CS = ShaderEntry{shader.get(), "ReadConstants"}}).Unwrap();
    array<unique_ptr<ShaderParameterSet>, 2> sets{device.CreateShaderParameterSet({group.get()}).Unwrap(), device.CreateShaderParameterSet({group.get()}).Unwrap()};
    auto pipeline = make_unique<UploadPipeline>(context, 1, 512);
    auto* runner = pipeline.get();
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    for (uint32_t frame = 0; frame < 2; ++frame) {
        runner->Build = [&](RDGBuilder& graph, FrameSlot&) {
            const array<byte, 256> padding{};
            const auto prefix = graph.CreateBuffer(RDGDynamicBufferDescriptor{256, BufferUse::CBuffer}, RDGUploadSource::SnapshotBytes(padding));
            graph.MarkOutput(prefix);
            const auto output = graph.ImportBuffer(result.get(), frame ? BufferState::Common : BufferState::Undefined, BufferState::Common);
            for (uint32_t i = 0; i < 2; ++i) {
                const array<uint32_t, 2> value{42u + frame + i * 49, i};
                const auto constants = graph.CreateBuffer(RDGDynamicBufferDescriptor{80, BufferUse::CBuffer, 256},
                                                          RDGUploadSource::Generated(80, value, +[](const array<uint32_t, 2>& data, RDGUploadWriteContext& ctx) noexcept {
                        std::fill(ctx.Bytes.begin(), ctx.Bytes.end(), byte{0});
                        std::memcpy(ctx.Bytes.data(), data.data(), 8);
                        return true; }));
                struct Data {
                    RDGBufferHandle Constants, Output;
                    ShaderParameterSet* Set;
                    ComputePipelineState* Pipeline;
                    uint32_t Index;
                };
                graph.AddComputePass("read sliced constants", Data{constants, output, sets[i].get(), kernel.get(), i}, +[](const Data& data, RDGComputePassContext& ctx) noexcept {
                        ctx.GetEncoder().BindComputePipelineState(data.Pipeline);
                        ctx.GetEncoder().BindShaderParameterSet(0, data.Set);
                        ctx.GetEncoder().Dispatch(1, 1, 1);
                        return true; }, +[](Data& data, RDGPrepareContext& ctx) noexcept {
                        const auto constants = ctx.ResolveBuffer(data.Constants), output = ctx.ResolveBuffer(data.Output);
                        data.Set->ResetBindingsForReuse();
                        if (data.Index == 0) EXPECT_EQ(constants.Offset, 256u);
                        if (data.Index == 1) EXPECT_EQ(constants.Offset, 0u);
                        return data.Set->Set(2, 0, ShaderBufferBinding{constants.Buffer.Get(), {constants.Offset, constants.Size}, 0}) &&
                               data.Set->Set(0, 0, ShaderBufferBinding{output.Buffer.Get(), {output.Offset, output.Size}, 4}) && data.Set->FlushWrites(); }).ReadBuffer(constants, BufferState::CBuffer).WriteBuffer(output);
            }
            const auto out = graph.ImportBuffer(readback.get(), BufferState::HostRead, BufferState::HostRead);
            graph.AddCopyPass("read constants", BufferCopyData{output, out, 8}, &RecordBufferCopy).ReadBuffer(output).WriteBuffer(out);
            graph.MarkOutput(out);
        };
        scheduler->RunFrame();
        scheduler->WaitIdle();
        EXPECT_EQ(gpu::test::FromBytes<uint32_t>(gpu::test::MapAndRead(*readback)), (vector<uint32_t>{42 + frame, 91 + frame}));
    }
    for (auto& set : sets) set->ResetBindingsForReuse();
}

INSTANTIATE_TEST_SUITE_P(Backends, BufferUploadGpuTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

class TextureUploadGpuTest : public gpu::test::BackendTest {
protected:
    enum class Initialization { Upload, CopyPass, ExternalCopy };
    void VerifyTexture(Initialization initialization) {
        auto& device = GetDevice();
        auto& queue = gpu::test::DirectQueue(device);
        DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
        auto texture = GpuTexture::Create(device, {.Dim = TextureDimension::Dim2DArray, .Width = 4, .Height = 4, .ArrayLayers = 2, .MipLevels = 3, .Format = TextureFormat::R32_UINT}).Unwrap();
        vector<uint32_t> expected;
        array<array<uint32_t, 3>, 2> offsets{};
        for (uint32_t layer = 0; layer < 2; ++layer) {
            for (uint32_t mip = 0; mip < 3; ++mip) {
                offsets[layer][mip] = static_cast<uint32_t>(expected.size());
                for (uint32_t i = 0; i < (4u >> mip) * (4u >> mip); ++i) expected.push_back(1000 * layer + 100 * mip + i + 1);
            }
        }
        const auto byteSize = expected.size() * 4;
        auto output = device.CreateBuffer({.Size = byteSize, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
        auto readback = gpu::test::CreateReadbackBuffer(device, byteSize).Unwrap();
        const array bindings{
            BindingDescriptor{.BindingId = 0, .Kind = BindingKind::Texture, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{GetBindingRegisterClass(BindingKind::Texture), 0, 0, D3D12BufferPlacement::Table}, .Vulkan = VulkanBindingDescriptor{0, 0}},
            BindingDescriptor{.BindingId = 1, .Kind = BindingKind::RWStructuredBuffer, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{GetBindingRegisterClass(BindingKind::RWStructuredBuffer), 0, 0, D3D12BufferPlacement::Table}, .Vulkan = VulkanBindingDescriptor{0, 1}}};
        auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
        const PipelineLayoutGroupDescriptor layoutGroup{0, group.get()};
        const PushConstantDescriptor push{.PushId = 0, .Size = 16, .Stages = ShaderStage::Compute, .D3D12 = PushConstantDescriptor::D3D12Location{0, 0}, .Vulkan = PushConstantDescriptor::VulkanLocation{0}};
        auto layout = device.CreatePipelineLayout({std::span{&layoutGroup, 1}, std::span{&push, 1}}).Unwrap();
        auto shader = gpu::test::LoadShader(device, "texture_upload", "LoadUploadedTexture", ShaderStage::Compute).Unwrap();
        auto kernel = device.CreateComputePipelineState({.PipelineLayout = layout.get(), .CS = ShaderEntry{shader.get(), "LoadUploadedTexture"}}).Unwrap();
        auto parameters = device.CreateShaderParameterSet({group.get()}).Unwrap();
        ASSERT_TRUE(parameters->Set(0, 0, texture->GetDefaultView().Unwrap()));
        ASSERT_TRUE(parameters->Set(1, 0, ShaderBufferBinding{output.get(), {0, byteSize}, 4}));
        ASSERT_TRUE(parameters->FlushWrites());
        vector<unique_ptr<Buffer>> externalSources;
        std::optional<gpu::test::Recording> external;
        auto pipeline = make_unique<UploadPipeline>(context, 2);
        auto* runner = pipeline.get();
        auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 2).Unwrap();
        runner->Build = [&](RDGBuilder& graph, FrameSlot&) {
            const auto image = graph.ImportPersistentOwned(texture);
            for (uint32_t layer = 0; layer < 2; ++layer) {
                for (uint32_t mip = 0; mip < 3; ++mip) {
                    const auto width = 4u >> mip;
                    const TextureRegion2D region{mip, layer, 0, 0, width, width};
                    if (initialization == Initialization::CopyPass) {
                        const auto pitchAlignment = std::max(1u, device.GetDetail().TextureDataPitchAlignment);
                        const auto pitch = ((width * 4 + pitchAlignment - 1) / pitchAlignment) * pitchAlignment;
                        vector<byte> bytes(pitch * (width - 1) + width * 4);
                        for (uint32_t row = 0; row < width; ++row)
                            std::memcpy(bytes.data() + row * pitch, expected.data() + offsets[layer][mip] + row * width, width * 4);
                        const auto sourceSize = bytes.size();
                        const auto source = graph.CreateBuffer(RDGDynamicBufferDescriptor{sourceSize, BufferUse::CopySource, device.GetDetail().TextureDataPlacementAlignment}, RDGUploadSource::OwnedBytes(std::move(bytes)));
                        struct Data {
                            RDGBufferHandle Source;
                            RDGTextureHandle Target;
                            TextureRegion2D Region;
                            uint32_t Pitch;
                        };
                        graph.AddCopyPass("ordinary texture initialization", Data{source, image, region, pitch}, +[](const Data& data, RDGCopyPassContext& ctx) noexcept {
                            const auto source = ctx.ResolveBuffer(data.Source);
                            return ctx.GetCommandBuffer().CopyBufferToTextureRegion({source.Buffer.Get(), &ctx.Resolve(data.Target),
                                {source.Offset, data.Pitch, data.Region}}); }).ReadBuffer(source).WriteTexture(image);
                    } else if (layer == 1) {
                        struct Pixels {
                            std::span<const uint32_t> Values;
                            uint32_t Width;
                        };
                        graph.UploadTexture(image, region, RDGTextureUploadSource::Generated(uint64_t{width} * width * 4, Pixels{std::span{expected}.subspan(offsets[layer][mip], width * width), width}, +[](const Pixels& data, RDGUploadWriteContext& write) noexcept {
                                for (uint32_t row = 0; row < data.Width; ++row)
                                    std::memcpy(write.Bytes.data() + row * write.RowPitch, data.Values.data() + row * data.Width, write.RowBytes);
                                return true; }));
                    } else {
                        // 源行 padding 刻意不服从设备 staging 对齐。
                        vector<byte> bytes((width + 1) * 4 * width, byte{0});
                        for (uint32_t row = 0; row < width; ++row)
                            std::memcpy(bytes.data() + row * (width + 1) * 4, expected.data() + offsets[layer][mip] + row * width, width * 4);
                        graph.UploadTexture(image, region, RDGTextureUploadSource::OwnedBytes(std::move(bytes), (width + 1) * 4));
                    }
                }
            }
            graph.MarkOutput(image);
        };
        if (initialization == Initialization::ExternalCopy) {
            external.emplace(gpu::test::BeginRecording(device));
            gpu::test::Barrier(*external->Commands, {BarrierTextureDescriptor{texture->GetTexture(), TextureState::Undefined, TextureState::CopyDestination}});
            for (uint32_t layer = 0; layer < 2; ++layer) {
                for (uint32_t mip = 0; mip < 3; ++mip) {
                    const auto width = 4u >> mip;
                    const auto pitch = gpu::test::CopyRowPitch(device, width, 4);
                    vector<byte> bytes(pitch * (width - 1) + width * 4);
                    for (uint32_t row = 0; row < width; ++row)
                        std::memcpy(bytes.data() + row * pitch, expected.data() + offsets[layer][mip] + row * width, width * 4);
                    auto source = gpu::test::CreateUploadBuffer(device, bytes, BufferUse::CopySource).Unwrap();
                    gpu::test::Barrier(*external->Commands, {BarrierBufferDescriptor{source.get(), BufferState::HostWrite, BufferState::CopySource}});
                    ASSERT_TRUE(external->Commands->CopyBufferToTextureRegion({source.get(), texture->GetTexture(), {0, pitch, {mip, layer, 0, 0, width, width}}}));
                    externalSources.push_back(std::move(source));
                }
            }
            external->Commands->End();
            auto* command = external->Commands.get();
            queue.Submit({.CmdBuffers = std::span{&command, 1}});
            texture->CommitSubmittedState(TextureState::CopyDestination);
        } else {
            scheduler->RunFrame();
            EXPECT_EQ(texture->GetSubmittedState(), TextureState::ShaderRead);
        }
        const array<uint32_t, 4> tile{9001, 9002, 9003, 9004};
        runner->Build = [&](RDGBuilder& graph, FrameSlot&) {
            const auto image = graph.ImportPersistentOwned(texture);
            graph.UploadTexture(image, {.X = 1, .Y = 1, .Width = 2, .Height = 2}, RDGTextureUploadSource::BorrowedBytes(std::as_bytes(std::span{tile}), 8));
            const auto values = graph.ImportBuffer(output.get(), BufferState::Undefined, BufferState::Common);
            const auto result = graph.ImportBuffer(readback.get(), BufferState::HostRead, BufferState::HostRead);
            struct Data {
                ComputePipelineState* Kernel;
                ShaderParameterSet* Parameters;
                array<array<uint32_t, 3>, 2> Offsets;
            };
            graph.AddComputePass("sample all subresources", Data{kernel.get(), parameters.get(), offsets}, +[](const Data& data, RDGComputePassContext& ctx) noexcept {
                auto& encoder = ctx.GetEncoder();
                encoder.BindComputePipelineState(data.Kernel);
                encoder.BindShaderParameterSet(0, data.Parameters);
                for (uint32_t layer = 0; layer < 2; ++layer) {
                    for (uint32_t mip = 0; mip < 3; ++mip) {
                        const array<uint32_t, 4> selection{mip, layer, 4u >> mip, data.Offsets[layer][mip]};
                        if (!encoder.SetPushConstants(0, 0, std::as_bytes(std::span{selection}))) return false;
                        encoder.Dispatch(4u >> mip, 4u >> mip, 1);
                    }
                }
                return true; }).ReadTexture(image).WriteBuffer(values);
            graph.AddCopyPass("read texture values", BufferCopyData{values, result, byteSize}, &RecordBufferCopy).ReadBuffer(values).WriteBuffer(result);
            graph.MarkOutput(result);
        };
        scheduler->RunFrame();
        scheduler->WaitIdle();
        expected[5] = 9001;
        expected[6] = 9002;
        expected[9] = 9003;
        expected[10] = 9004;
        EXPECT_EQ(gpu::test::FromBytes<uint32_t>(gpu::test::MapAndRead(*readback)), expected);
        EXPECT_EQ(texture->GetSubmittedState(), TextureState::ShaderRead);
    }
};

TEST_P(TextureUploadGpuTest, MipsLayersSourcePaddingAndNextFrameTile) { VerifyTexture(Initialization::Upload); }

TEST_P(TextureUploadGpuTest, GraphPacksPaddedVolumeSlicesAndCopiesCubeTailBlock) {
    auto& device = GetDevice();
    auto& queue = gpu::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    const auto usage = TextureUse::CopySource | TextureUse::CopyDestination;
    auto cube = GpuTexture::Create(device, {.Dim = TextureDimension::Cube, .Width = 4, .Height = 4, .ArrayLayers = 6,
                                            .MipLevels = 3, .Format = TextureFormat::BC1_UNORM, .Usage = usage}).Unwrap();
    auto volume = GpuTexture::Create(device, {.Dim = TextureDimension::Dim3D, .Width = 2, .Height = 2, .ArrayLayers = 3,
                                              .Format = TextureFormat::R8_UNORM, .Usage = usage}).Unwrap();
    const uint32_t rowPitch = gpu::test::CopyRowPitch(device, 2, 1);
    auto cubeOutput = gpu::test::CreateReadbackBuffer(device, gpu::test::CopyRowPitch(device, 1, 8)).Unwrap();
    auto volumeOutput = gpu::test::CreateReadbackBuffer(device, 6 * rowPitch).Unwrap();
    const array<byte, 8> block{byte{1}, byte{2}, byte{3}, byte{4}, byte{5}, byte{6}, byte{7}, byte{8}};
    vector<byte> source(22, byte{0xec});
    for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 2; ++y)
            std::fill_n(source.begin() + z * 16 + y * 4, 2, byte(8 * z + y));
    auto pipeline = make_unique<UploadPipeline>(context, 1);
    pipeline->Build = [&](RDGBuilder& graph, FrameSlot&) {
        auto cubeHandle = graph.ImportPersistentOwned(cube);
        auto volumeHandle = graph.ImportPersistentOwned(volume);
        graph.UploadTexture(cubeHandle, {2, 5, 0, 0, 1, 1}, RDGTextureUploadSource::BorrowedBytes(block));
        graph.UploadTexture(volumeHandle, {0, 0, 0, 0, 2, 2, 1, 2}, RDGTextureUploadSource::BorrowedBytes(source, 4, 4));
        auto read = [&](RDGTextureHandle texture, Buffer& output, SubresourceRange range) {
            auto result = graph.ImportBuffer(&output, BufferState::Undefined, BufferState::HostRead);
            struct Data { RDGTextureHandle Texture; RDGBufferHandle Output; SubresourceRange Range; };
            graph.AddCopyPass("read uploaded mip", Data{texture, result, range}, +[](const Data& data, RDGCopyPassContext& ctx) noexcept {
                const auto slice = ctx.ResolveBuffer(data.Output);
                ctx.GetCommandBuffer().CopyTextureToBuffer(slice.Buffer.Get(), slice.Offset, &ctx.Resolve(data.Texture), data.Range);
                return true;
            }).ReadTexture(texture).WriteBuffer(result);
            graph.MarkOutput(result);
        };
        read(cubeHandle, *cubeOutput, {5, 1, 2, 1});
        read(volumeHandle, *volumeOutput, {0, 1, 0, 1});
    };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    scheduler->RunFrame();
    scheduler->WaitIdle();
    auto bytes = gpu::test::MapAndRead(*cubeOutput);
    ASSERT_GE(bytes.size(), block.size());
    EXPECT_TRUE(std::equal(block.begin(), block.end(), bytes.begin()));
    bytes = gpu::test::MapAndRead(*volumeOutput);
    ASSERT_EQ(bytes.size(), 6 * rowPitch);
    for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 2; ++y)
            for (uint32_t x = 0; x < 2; ++x)
                EXPECT_EQ(bytes[(2 + z * 2 + y) * rowPitch + x], byte(8 * z + y));
}

TEST_P(TextureUploadGpuTest, AttachmentOnlyImportKeepsSubmittedStateAcrossGraphs) {
    auto& device = GetDevice();
    auto& queue = gpu::test::DirectQueue(device);
    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &queue};
    auto texture = GpuTexture::Create(device, {.Width = 4, .Height = 4, .Format = TextureFormat::RGBA8_UNORM,
                                               .Usage = TextureUse::RenderTarget}).Unwrap();
    EXPECT_FALSE(texture->GetDefaultView());
    EXPECT_FALSE(TextureAsset::Create(texture));
    auto pipeline = make_unique<UploadPipeline>(context, 1);
    pipeline->Build = [&](RDGBuilder& graph, FrameSlot&) {
        auto handle = graph.ImportPersistentOwned(texture);
        struct Data {};
        graph.AddRasterPass("clear", Data{}, +[](const Data&, RDGRasterPassContext&) noexcept { return true; })
            .WriteColor(0, handle, LoadAction::Clear, StoreAction::Store, {{0, 0, 0, 1}});
        graph.MarkOutput(handle);
    };
    auto scheduler = FrameScheduler::Create(context, std::move(pipeline), 1).Unwrap();
    for (uint32_t frame = 0; frame < 2; ++frame) {
        scheduler->RunFrame();
        EXPECT_EQ(texture->GetSubmittedState(), TextureState::RenderTarget);
    }
    scheduler->WaitIdle();
}

TEST_P(TextureUploadGpuTest, TextureAssetsClassifyReplacementAndReuseSamplingViews) {
    auto& device = GetDevice();
    const GpuTextureDescriptor desc{.Width = 8, .Height = 8, .MipLevels = 3, .Format = TextureFormat::BC3_UNORM};
    auto texture = GpuTexture::Create(device, desc).Unwrap();
    auto first = TextureAsset::Create(texture);
    auto same = TextureAsset::Create(texture);
    auto replacement = TextureAsset::Create(GpuTexture::Create(device, desc).Unwrap());
    ASSERT_TRUE(first && same && replacement);
    EXPECT_EQ(first->GetDefaultView(), texture->GetDefaultView().Get());
    EXPECT_EQ(first->ClassifyReload(*same).Kind, AssetReloadKind::Compatible);
    EXPECT_EQ(first->ClassifyReload(*replacement).Kind, AssetReloadKind::Breaking);
    GpuTextureViewKey key{.Dim = desc.Dim, .Format = desc.Format, .Range = SubresourceRange::AllSub(), .Usage = TextureViewUsage::Resource};
    EXPECT_EQ(texture->GetOrCreateView(device, key).Get(), first->GetDefaultView());
    key.Range = {0, 1, 1, 1};
    auto view = texture->GetOrCreateView(device, key);
    ASSERT_TRUE(view);
    EXPECT_NE(view.Get(), first->GetDefaultView());
    EXPECT_EQ(texture->GetOrCreateView(device, key).Get(), view.Get());
}
TEST_P(TextureUploadGpuTest, OrdinaryCopyInitializationPublishesStateForNextFrameTileAndSampling) { VerifyTexture(Initialization::CopyPass); }
TEST_P(TextureUploadGpuTest, ExternalSubmissionPublishesStateForTileUpdateAndSampling) { VerifyTexture(Initialization::ExternalCopy); }

INSTANTIATE_TEST_SUITE_P(Backends, TextureUploadGpuTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

}  // namespace
}  // namespace radray
