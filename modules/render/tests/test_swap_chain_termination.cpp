#include "gpu_test_fixture.h"

namespace radray::render::test {

#if defined(RADRAY_ENABLE_D3D12)
namespace {
struct FixtureWindow {
    CommandQueue& Queue;
    HWND Handle;
    explicit FixtureWindow(CommandQueue& queue, int x = 0) : Queue(queue),
        Handle(CreateWindowExW(0, L"STATIC", L"RadRay token fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            x, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)) {}
    ~FixtureWindow() { Queue.Wait(); if (Handle) DestroyWindow(Handle); }
};
Nullable<unique_ptr<SwapChain>> CreateFixtureChain(DeviceContext& context, HWND window) {
    return context.Device->CreateSwapChain({.PresentQueue = context.Queue, .NativeHandler = window,
        .Width = 128, .Height = 128, .BackBufferCount = 2, .Format = TextureFormat::RGBA8_UNORM,
        .PresentMode = PresentMode::FIFO});
}
}  // namespace
#endif

TEST(RadRaySwapChainTermination, DiscardPreservesForeignTokenAndRequiresRecreate) {
#if defined(RADRAY_ENABLE_D3D12)
    DeviceContext context;
    if (!TryCreateDevice(RenderBackend::D3D12, context, true)) GTEST_SKIP() << context.Reason;
    FixtureWindow first(*context.Queue), second(*context.Queue, 192);
    ASSERT_NE(first.Handle, nullptr); ASSERT_NE(second.Handle, nullptr);
    auto a = CreateFixtureChain(context, first.Handle), b = CreateFixtureChain(context, second.Handle);
    ASSERT_TRUE(a); ASSERT_TRUE(b);
    auto acquired = a.Get()->AcquireNext(1000);
    ASSERT_EQ(acquired.Status, SwapChainStatus::Success); ASSERT_TRUE(acquired.Frame);
    auto& frame = *acquired.Frame;
    EXPECT_EQ(a.Get()->SwapChain::DiscardAcquiredFrame(std::move(frame)).Status, SwapChainStatus::Error);
    EXPECT_TRUE(frame.IsValid());
    EXPECT_EQ(b.Get()->DiscardAcquiredFrame(std::move(frame)).Status, SwapChainStatus::Error);
    EXPECT_TRUE(frame.IsValid());
    EXPECT_EQ(b.Get()->Present(std::move(frame)).Status, SwapChainStatus::Error);
    EXPECT_TRUE(frame.IsValid());
    context.Queue->Wait();
    EXPECT_EQ(a.Get()->DiscardAcquiredFrame(std::move(frame)).Status, SwapChainStatus::Success);
    EXPECT_FALSE(frame.IsValid());
    EXPECT_EQ(a.Get()->DiscardAcquiredFrame(std::move(frame)).Status, SwapChainStatus::Error);
    EXPECT_EQ(a.Get()->AcquireNext(0).Status, SwapChainStatus::RequireRecreate);
    ASSERT_TRUE(a.Get()->Recreate(128, 128, TextureFormat::RGBA8_UNORM, PresentMode::FIFO));
    auto next = a.Get()->AcquireNext(1000);
    ASSERT_EQ(next.Status, SwapChainStatus::Success); ASSERT_TRUE(next.Frame);
    const auto presented = a.Get()->Present(std::move(*next.Frame));
    EXPECT_TRUE(presented.Status == SwapChainStatus::Success || presented.Status == SwapChainStatus::RetryLater);
    if (presented.Status == SwapChainStatus::RetryLater)
        EXPECT_EQ(presented.NativeStatusCode, static_cast<int64_t>(DXGI_STATUS_OCCLUDED));
    EXPECT_FALSE(next.Frame->IsValid());
    context.Queue->Wait();
#else
    GTEST_SKIP() << "D3D12 backend not built";
#endif
}

TEST(RadRaySwapChainTermination, HiddenPresentConsumesTokenAndReportsRetry) {
#if defined(RADRAY_ENABLE_D3D12)
    DeviceContext context;
    if (!TryCreateDevice(RenderBackend::D3D12, context, true)) GTEST_SKIP() << context.Reason;
    FixtureWindow window(*context.Queue);
    ASSERT_NE(window.Handle, nullptr);
    auto chain = CreateFixtureChain(context, window.Handle); ASSERT_TRUE(chain);
    auto acquired = chain.Get()->AcquireNext(1000);
    ASSERT_EQ(acquired.Status, SwapChainStatus::Success); ASSERT_TRUE(acquired.Frame);
    ShowWindow(window.Handle, SW_HIDE);
    const auto result = chain.Get()->Present(std::move(*acquired.Frame));
    EXPECT_EQ(result.Status, SwapChainStatus::RetryLater);
    EXPECT_EQ(result.NativeStatusCode, static_cast<int64_t>(DXGI_STATUS_OCCLUDED));
    EXPECT_FALSE(acquired.Frame->IsValid());
    EXPECT_EQ(chain.Get()->AcquireNext(0).Status, SwapChainStatus::RetryLater);
    context.Queue->Wait();
#else
    GTEST_SKIP() << "D3D12 backend not built";
#endif
}

}  // namespace radray::render::test
