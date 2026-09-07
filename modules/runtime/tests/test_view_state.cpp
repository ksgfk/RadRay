#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include <gtest/gtest.h>
#include <radray/runtime/render_framework/view_state.h>
#include <radray/utility.h>

namespace radray {
namespace {
class ViewStateTest : public testing::TestWithParam<render::RenderBackend> {};

TEST_P(ViewStateTest, PreviousMatrixOnlyAdvancesOnCommitAndCutInvalidates) {
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(GetParam(), device, true)) GTEST_SKIP() << device.Reason;
    render::RenderPassRegistry passes(device.Device.get());
    ViewStateRegistry registry(*device.Device, passes, 2);
    ResolvedRenderViewFamily family{};
    family.OutputAvailable = true;
    family.RenderSize = {16, 16};
    family.OutputFormat = render::TextureFormat::RGBA8_UNORM;
    family.SampleCount = 1;
    ResolvedRenderView view{};
    view.StateId = AllocateViewStateId();
    view.ViewProjection = Eigen::Matrix4f::Identity();
    registry.BeginFlight(0, 1);
    registry.Resolve(view, family);
    EXPECT_FALSE(view.PreviousViewValid);
    EXPECT_EQ(registry.GetStats().TexturesCreated, 0u);
    EXPECT_TRUE(registry.CommitView(view.StateId));
    EXPECT_FALSE(registry.CommitView(view.StateId));
    registry.BeginFlight(1, 2);
    view.ViewProjection(0, 3) = 2;
    registry.Resolve(view, family);
    EXPECT_TRUE(view.PreviousViewValid);
    EXPECT_TRUE(view.PreviousViewProjection.isIdentity());
    registry.BeginFlight(0, 3);
    view.ViewProjection(0, 3) = 3;
    registry.Resolve(view, family);
    EXPECT_TRUE(view.PreviousViewProjection.isIdentity());
    EXPECT_TRUE(registry.CommitView(view.StateId));
    registry.BeginFlight(1, 4);
    view.CameraCut = true;
    registry.Resolve(view, family);
    EXPECT_FALSE(view.PreviousViewValid);
    EXPECT_EQ(registry.GetInvalidationReason(view.StateId), ViewHistoryInvalidationReason::CameraCut);
    registry.BeginFlight(0, 5);
    view.CameraCut = false;
    family.OutputAvailable = false;
    registry.Resolve(view, family);
    EXPECT_FALSE(registry.CommitView(view.StateId));
    EXPECT_EQ(device.ValidationErrors.load(), 0u);
}

TEST_P(ViewStateTest, HistoryGpuRoundTripRotationResizeAndRetirement) {
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(GetParam(), device, true)) GTEST_SKIP() << device.Reason;
    render::RenderPassRegistry passes(device.Device.get());
    RenderResourcePool pools[]{RenderResourcePool{*device.Device, passes}, RenderResourcePool{*device.Device, passes}};
    ViewStateRegistry registry(*device.Device, passes, 2);
    ResolvedRenderViewFamily family{};
    family.OutputAvailable = true;
    family.RenderSize = family.OutputSize = {16, 16};
    family.OutputFormat = render::TextureFormat::RGBA8_UNORM;
    family.SampleCount = 1;
    ResolvedRenderView view{};
    view.StateId = AllocateViewStateId();
    view.ViewProjection.setIdentity();
    HistoryTextureRequest request;
    request.Key = "color";
    request.DebugName = "History";
    request.Desc.Extent.Mode = RenderExtentMode::RelativeToFamilyRenderExtent;
    request.Desc.Format = render::TextureFormat::RGBA8_UNORM;
    request.Desc.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource;
    string reason;
    pools[0].BeginFlight(1);
    registry.BeginFlight(0, 1);
    registry.Resolve(view, family);
    auto first = registry.AcquireHistoryTexture(view, family, request, reason);
    ASSERT_TRUE(first.Current) << reason;
    EXPECT_FALSE(first.PreviousValid);
    auto* original = first.Current->Texture;
    EXPECT_FALSE(registry.AcquireHistoryTexture(view, family, request, reason).Current);
    EXPECT_FALSE(registry.CommitHistory(first.CommitToken));
    RenderGraph write(*device.Device, pools[0], passes, "history write");
    auto current = write.ImportTexture(*first.Current, "history current", RenderGraphExternalAccess::ObservableOutput);
    struct Data {};
    write.AddRasterPass<Data>("history clear", [=](Data&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, current, {.Clear = {{.2f, .4f, .6f, 1}}}); }, +[](const Data&, RenderGraphRasterContext&) {});
    auto command = device.Device->CreateCommandBuffer(device.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(write, *command).Success) << write.GetReport().ToText();
    command->End();
    auto* raw = command.Get();
    device.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
    device.Queue->Wait();
    EXPECT_TRUE(registry.CommitHistory(first.CommitToken));
    EXPECT_FALSE(registry.CommitHistory(first.CommitToken));
    EXPECT_TRUE(registry.CommitView(view.StateId));
    pools[1].BeginFlight(2);
    registry.BeginFlight(1, 2);
    registry.Resolve(view, family);
    auto next = registry.AcquireHistoryTexture(view, family, request, reason);
    ASSERT_TRUE(next.PreviousValid);
    EXPECT_EQ(next.Previous->Texture, original);
    EXPECT_NE(next.Current->Texture, original);
    auto wrongIndex = next.CommitToken;
    wrongIndex.Index = first.CommitToken.Index;
    EXPECT_FALSE(registry.CommitHistory(wrongIndex));
    const uint64_t row = Align(uint64_t{16 * 4}, device.Device->GetDetail().TextureDataPitchAlignment);
    auto readback = device.Device->CreateBuffer({row * 16, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    RenderGraph read(*device.Device, pools[1], passes, "history read");
    const auto previous = read.ImportTexture(*next.Previous, "previous", RenderGraphExternalAccess::ReadOnly);
    const auto destination = read.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput);
    read.AddCopyTextureToBufferPass("copy previous", previous, destination);
    read.AddComputePass<Data>("host", [=](Data&, RenderGraphComputeBuilder& builder) { builder.ReadBuffer(destination, RgBufferAccess::HostRead); builder.SetSideEffect(); }, +[](const Data&, RenderGraphComputeContext&) {});
    auto readCommand = device.Device->CreateCommandBuffer(device.Queue);
    ASSERT_TRUE(readCommand);
    readCommand->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(read, *readCommand).Success) << read.GetReport().ToText();
    readCommand->End();
    raw = readCommand.Get();
    device.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
    device.Queue->Wait();
    auto* mapped = static_cast<const uint8_t*>(readback->Map(0, row * 16));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, row * 16});
    EXPECT_NEAR(mapped[0], 51, 1);
    EXPECT_NEAR(mapped[1], 102, 1);
    EXPECT_NEAR(mapped[2], 153, 1);
    readback->Unmap();
    EXPECT_FALSE(registry.CommitHistory(next.CommitToken));
    pools[0].BeginFlight(3);
    registry.BeginFlight(0, 3);
    family.RenderSize = {32, 16};
    registry.Resolve(view, family);
    auto resized = registry.AcquireHistoryTexture(view, family, request, reason);
    ASSERT_TRUE(resized.Current);
    EXPECT_FALSE(resized.PreviousValid);
    EXPECT_NE(resized.CommitToken.Generation, next.CommitToken.Generation);
    EXPECT_EQ(registry.GetStats().RetiredGenerations, 1u);
    EXPECT_EQ(registry.GetStats().GenerationsDestroyed, 0u);
    pools[1].BeginFlight(4);
    registry.BeginFlight(1, 4);
    EXPECT_EQ(registry.GetStats().RetiredGenerations, 1u);
    pools[0].BeginFlight(5);
    registry.BeginFlight(0, 5);
    EXPECT_EQ(registry.GetStats().RetiredGenerations, 0u);
    EXPECT_EQ(registry.GetStats().GenerationsDestroyed, 1u);
    EXPECT_EQ(device.ValidationErrors.load(), 0u);
}

TEST_P(ViewStateTest, IndependentViewsAndInactiveRecordsRetireAtOwningFlight) {
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(GetParam(), device, true)) GTEST_SKIP() << device.Reason;
    render::RenderPassRegistry passes(device.Device.get());
    ViewStateRegistry registry(*device.Device, passes, 2, 1);
    ResolvedRenderViewFamily family{};
    family.OutputAvailable = true;
    family.RenderSize = family.OutputSize = {8, 8};
    family.OutputFormat = render::TextureFormat::RGBA8_UNORM;
    ResolvedRenderView a{}, b{};
    a.StateId = AllocateViewStateId();
    b.StateId = AllocateViewStateId();
    a.ViewProjection.setIdentity();
    b.ViewProjection.setIdentity();
    HistoryTextureRequest request;
    request.Key = "independent";
    request.Desc.Format = family.OutputFormat;
    request.Desc.Usage = render::TextureUse::RenderTarget;
    request.BufferCount = 4;
    string reason;
    registry.BeginFlight(0, 1);
    registry.Resolve(a, family);
    registry.Resolve(b, family);
    auto first = registry.AcquireHistoryTexture(a, family, request, reason), second = registry.AcquireHistoryTexture(b, family, request, reason);
    ASSERT_TRUE(first.Current);
    ASSERT_TRUE(second.Current);
    EXPECT_NE(first.Current->Texture, second.Current->Texture);
    EXPECT_EQ(registry.GetStats().TexturesCreated, 8u);
    registry.BeginFlight(1, 2);
    registry.BeginFlight(0, 3);
    EXPECT_EQ(registry.GetStats().ActiveViews, 0u);
    EXPECT_EQ(registry.GetStats().RetiredGenerations, 2u);
    registry.BeginFlight(1, 4);
    EXPECT_EQ(registry.GetStats().GenerationsDestroyed, 0u);
    registry.BeginFlight(0, 5);
    EXPECT_EQ(registry.GetStats().GenerationsDestroyed, 2u);
    EXPECT_EQ(device.ValidationErrors.load(), 0u);
}
TEST_P(ViewStateTest, ThreeFlightsReportPerViewMemoryAndConvergeAfterRepeatedResizeAndDisable) {
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(GetParam(), device, true)) GTEST_SKIP() << device.Reason;
    render::RenderPassRegistry passes(device.Device.get());
    RenderResourcePool pools[]{RenderResourcePool{*device.Device, passes, 2}, RenderResourcePool{*device.Device, passes, 2}, RenderResourcePool{*device.Device, passes, 2}};
    ViewStateRegistry histories(*device.Device, passes, 3, 2);
    ResolvedRenderView views[2];
    for (auto& view : views) view.StateId = AllocateViewStateId();
    ResolvedRenderViewFamily family{};
    family.OutputAvailable = true;
    family.OutputFormat = render::TextureFormat::RGBA8_UNORM;
    family.SampleCount = 1;
    HistoryTextureRequest request;
    request.Key = "resizing";
    request.Desc.Extent.Mode = RenderExtentMode::RelativeToFamilyRenderExtent;
    request.Desc.Format = family.OutputFormat;
    request.Desc.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource;
    request.BufferCount = 3;
    uint64_t historyPeak = 0;
    for (uint64_t frame = 1; frame <= 72; ++frame) {
        const auto flight = uint32_t((frame - 1) % 3);
        auto& pool = pools[flight];
        pool.BeginFlight(frame);
        histories.BeginFlight(flight, frame);
        for (uint32_t index = 0; index < 2; ++index) {
            if (frame > (index == 0 ? 60u : 30u)) continue;
            family.RenderSize = family.OutputSize = {uint32_t(32 + ((frame / 2 + index) % 3) * 16), 32};
            histories.Resolve(views[index], family);
            string reason;
            ASSERT_TRUE(histories.AcquireHistoryTexture(views[index], family, request, reason).Current) << reason;
            auto desc = *ResolveRuntimeTextureDesc(request.Desc, family, *device.Device, reason);
            const auto id = views[index].StateId.Value;
            ASSERT_TRUE(pool.AcquireTexture(desc, "color", id));
            desc.Format = render::TextureFormat::D32_FLOAT;
            desc.Usage = render::TextureUse::DepthStencilWrite;
            ASSERT_TRUE(pool.AcquireTexture(desc, "depth", id));
            desc.Format = render::TextureFormat::RGBA8_UNORM;
            desc.Usage = render::TextureUse::UnorderedAccess | render::TextureUse::Resource;
            ASSERT_TRUE(pool.AcquireTexture(desc, "storage", id));
            ASSERT_TRUE(pool.AcquireBuffer({1024, render::MemoryType::Device, render::BufferUse::UnorderedAccess, {}}, "buffer", id));
        }
        pool.EndGraph();
        const auto& memory = pool.GetStats();
        uint64_t sum = 0;
        for (const auto& entry : memory.MemoryByView) {
            sum += entry.TotalBytes();
            EXPECT_LE(entry.InactiveBytes, entry.TotalBytes());
            EXPECT_TRUE(entry.ViewId == views[0].StateId.Value || entry.ViewId == views[1].StateId.Value);
        }
        EXPECT_EQ(sum, memory.EstimatedBytes);
        EXPECT_GE(memory.PeakEstimatedBytes, memory.EstimatedBytes);
        EXPECT_LE(memory.EstimatedBytes, 2u * 3u * (64u * 32u * 4u * 3u + 1024u));
        const auto history = histories.GetStats();
        uint64_t active = 0, retired = 0, flights = 0;
        for (const auto& entry : history.MemoryByView) {
            active += entry.ActiveBytes;
            retired += entry.RetiredBytes;
        }
        for (const auto bytes : history.RetiredBytesByFlight) flights += bytes;
        EXPECT_EQ(active + retired, history.EstimatedBytes);
        EXPECT_EQ(retired, history.RetiredBytes);
        EXPECT_EQ(flights, retired);
        historyPeak = std::max(historyPeak, history.EstimatedBytes);
        EXPECT_EQ(history.PeakEstimatedBytes, historyPeak);
        if (frame == 72) {
            EXPECT_EQ(history.EstimatedBytes, 0u);
            EXPECT_EQ(history.ActiveViews, 0u);
            EXPECT_EQ(history.RetiredGenerations, 0u);
        }
    }
    for (const auto& pool : pools) {
        EXPECT_EQ(pool.GetStats().EstimatedBytes, 0u);
        EXPECT_GT(pool.GetStats().PeakEstimatedBytes, 0u);
        EXPECT_GT(pool.GetStats().Trimmed, 0u);
    }
    EXPECT_EQ(device.ValidationErrors.load(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, ViewStateTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
