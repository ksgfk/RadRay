#include "gpu_test_fixture.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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
    for (int i = 0; i < 4; ++i) {
        context.Queue->Wait();
        ASSERT_EQ(a.Get()->DiscardAcquiredFrame(std::move(*next.Frame)).Status, SwapChainStatus::Success);
        EXPECT_FALSE(next.Frame->IsValid());
        ASSERT_TRUE(a.Get()->Recreate(128, 128, TextureFormat::RGBA8_UNORM, PresentMode::FIFO));
        next = a.Get()->AcquireNext(1000);
        ASSERT_EQ(next.Status, SwapChainStatus::Success); ASSERT_TRUE(next.Frame);
    }
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
    for (int i = 0; i < 3; ++i) {
        ShowWindow(window.Handle, SW_SHOW);
        auto resumed = chain.Get()->AcquireNext(1000);
        ASSERT_EQ(resumed.Status, SwapChainStatus::Success); ASSERT_TRUE(resumed.Frame);
        ShowWindow(window.Handle, SW_HIDE);
        EXPECT_EQ(chain.Get()->Present(std::move(*resumed.Frame)).Status, SwapChainStatus::RetryLater);
        EXPECT_FALSE(resumed.Frame->IsValid());
        context.Queue->Wait();
    }
#else
    GTEST_SKIP() << "D3D12 backend not built";
#endif
}

TEST(RadRayFenceWait, TimeoutAndEarlierEventDoNotReportCompletion) {
#if defined(RADRAY_ENABLE_D3D12)
    DeviceContext context;
    if (!TryCreateDevice(RenderBackend::D3D12, context, true)) GTEST_SKIP() << context.Reason;
    auto created = context.Device->CreateFence(); ASSERT_TRUE(created);
    auto fence = created.Release();
    auto* native = d3d12::CastD3D12Object(fence.get());
    EXPECT_FALSE(fence->WaitFor(1, 0));
    EXPECT_FALSE(fence->WaitFor(1, 1));
    EXPECT_EQ(fence->GetCompletedValue(), 0);
    ASSERT_EQ(native->_fence->Signal(1), S_OK);
    EXPECT_FALSE(fence->WaitFor(2, 2));
    EXPECT_EQ(fence->GetCompletedValue(), 1);
    ASSERT_EQ(native->_fence->Signal(2), S_OK);
    EXPECT_TRUE(fence->WaitFor(2, 0));
    EXPECT_TRUE(fence->WaitFor(2, UINT64_MAX));
    Fence* signals[]{fence.get()}; uint64_t values[]{3};
    context.Queue->Submit({.SignalFences = signals, .SignalValues = values});
    EXPECT_TRUE(fence->WaitFor(3, 5000));
    EXPECT_EQ(fence->GetCompletedValue(), 3);
    EXPECT_EQ(context.ValidationErrors.load(), 0);
#else
    GTEST_SKIP() << "D3D12 backend not built";
#endif
}

TEST(RadRaySwapChainTermination, PresentReturnsWhileQueueIsBlocked) {
#if defined(RADRAY_ENABLE_D3D12)
    DeviceContext context;
    if (!TryCreateDevice(RenderBackend::D3D12, context, true)) GTEST_SKIP() << context.Reason;
    FixtureWindow window(*context.Queue); ASSERT_NE(window.Handle, nullptr);
    auto chain = CreateFixtureChain(context, window.Handle); ASSERT_TRUE(chain);
    for (bool hidden : {false, true}) {
        ShowWindow(window.Handle, SW_SHOW);
        auto acquired = chain.Get()->AcquireNext(1000);
        ASSERT_EQ(acquired.Status, SwapChainStatus::Success); ASSERT_TRUE(acquired.Frame);
        auto created = context.Device->CreateFence(); ASSERT_TRUE(created);
        auto blocker = created.Release();
        auto* nativeFence = d3d12::CastD3D12Object(blocker.get());
        auto* nativeQueue = d3d12::CastD3D12Object(context.Queue);
        ASSERT_EQ(nativeQueue->_queue->Wait(nativeFence->_fence.Get(), 1), S_OK);
        std::mutex mutex;
        std::condition_variable condition;
        bool returned = false, neededWatchdog = false;
        std::jthread watchdog([&] {
            std::unique_lock lock(mutex);
            neededWatchdog = !condition.wait_for(lock, std::chrono::seconds(2), [&] { return returned; });
            EXPECT_EQ(nativeFence->_fence->Signal(1), S_OK);
        });
        if (hidden) ShowWindow(window.Handle, SW_HIDE);
        const auto result = chain.Get()->Present(std::move(*acquired.Frame));
        {
            std::lock_guard lock(mutex);
            returned = true;
        }
        condition.notify_one(); watchdog.join();
        EXPECT_FALSE(neededWatchdog) << "Present drained or blocked on the queue";
        EXPECT_FALSE(acquired.Frame->IsValid());
        EXPECT_TRUE(result.Status == SwapChainStatus::Success || result.Status == SwapChainStatus::RetryLater);
        if (hidden) EXPECT_EQ(result.NativeStatusCode, static_cast<int64_t>(DXGI_STATUS_OCCLUDED));
        context.Queue->Wait();
    }
    EXPECT_EQ(context.ValidationErrors.load(), 0);
#else
    GTEST_SKIP() << "D3D12 backend not built";
#endif
}

}  // namespace radray::render::test
