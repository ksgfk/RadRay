#include "gpu_test_fixture.h"

#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::render {
namespace {

using vulkan::SwapChainRetirementVulkan;

TEST(VulkanSwapChainRetirement, RetiredGenerationWaitsForSuccessorPresentAndOwnAcquires) {
    SwapChainRetirementVulkan tracker;
    const auto old = tracker.BeginGeneration();
    tracker.Acquire(old);
    tracker.QueuePresent(old);
    tracker.Retire(old);
    const auto next = tracker.BeginGeneration();
    tracker.Acquire(next);
    EXPECT_TRUE(tracker.Collect().empty());

    tracker.CompleteAcquire(next, true);
    EXPECT_TRUE(tracker.Collect().empty());

    tracker.CompleteAcquire(old, false);
    EXPECT_EQ(tracker.Collect(), vector<uint64_t>{old});
    EXPECT_TRUE(tracker.Collect().empty());
}

TEST(VulkanSwapChainRetirement, OwnPresentDoesNotReleaseRetiredGeneration) {
    SwapChainRetirementVulkan tracker;
    const auto generation = tracker.BeginGeneration();
    tracker.QueuePresent(generation);
    tracker.Acquire(generation);
    tracker.CompleteAcquire(generation, true);
    tracker.Retire(generation);

    EXPECT_TRUE(tracker.Collect().empty());
}

TEST(VulkanSwapChainRetirement, SuccessorPresentReleasesAllPresentedAncestors) {
    SwapChainRetirementVulkan tracker;
    const auto first = tracker.BeginGeneration();
    tracker.QueuePresent(first);
    tracker.Retire(first);
    const auto second = tracker.BeginGeneration();
    tracker.QueuePresent(second);
    tracker.Retire(second);
    const auto current = tracker.BeginGeneration();
    tracker.Acquire(current);
    tracker.CompleteAcquire(current, false);
    EXPECT_TRUE(tracker.Collect().empty());

    tracker.QueuePresent(current);
    tracker.Acquire(current);
    tracker.CompleteAcquire(current, true);

    EXPECT_EQ(tracker.Collect(), (vector<uint64_t>{first, second}));
}

TEST(VulkanSwapChainRetirement, UnpresentedGenerationOnlyWaitsForItsAcquires) {
    SwapChainRetirementVulkan tracker;
    const auto generation = tracker.BeginGeneration();
    tracker.Acquire(generation);
    tracker.Retire(generation);
    EXPECT_TRUE(tracker.Collect().empty());

    tracker.CompleteAcquire(generation, false);

    EXPECT_EQ(tracker.Collect(), vector<uint64_t>{generation});
}

TEST(VulkanSwapChainRetirement, TrackersDoNotShareProof) {
    SwapChainRetirementVulkan left, right;
    const auto retired = left.BeginGeneration();
    left.QueuePresent(retired);
    left.Retire(retired);
    right.BeginGeneration();
    const auto presented = right.BeginGeneration();
    right.Acquire(presented);
    right.CompleteAcquire(presented, true);

    EXPECT_TRUE(left.Collect().empty());
}

#if defined(RADRAY_PLATFORM_WINDOWS)

class VulkanSwapChainWindowTest : public test::GpuTest {
protected:
    void SetUp() override {
        _device = OpenDevice(RenderBackend::Vulkan).Get();
        if (!_device) return;
        _window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"RadRay swapchain test", WS_OVERLAPPEDWINDOW,
                                    0, 0, 160, 120, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(_window, nullptr);
        ::ShowWindow(_window, SW_SHOWNOACTIVATE);
        PumpMessages();
    }

    void TearDown() override {
        _swapchain.reset();
        if (_window) ::DestroyWindow(_window);
        GpuTest::TearDown();
    }

    static void PumpMessages() {
        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    RECT ClientRect() const {
        RECT client{};
        ::GetClientRect(_window, &client);
        return client;
    }

    // Acquires, transitions the backbuffer to Present and presents it.
    SwapChainStatus PresentFrame() {
        PumpMessages();
        auto acquired = _swapchain->AcquireNext(1000);
        if (!acquired.Frame) return acquired.Status;
        auto frame = std::move(*acquired.Frame);
        auto recording = test::BeginRecording(*_device);
        test::Barrier(*recording.Commands, {BarrierTextureDescriptor{.Target = frame.GetBackBuffer(), .Before = TextureState::Undefined, .After = TextureState::Present}});
        recording.Commands->End();
        CommandBuffer* commands[]{recording.Commands.get()};
        SwapChainSyncObject* waits[]{frame.GetWaitToDraw()};
        SwapChainSyncObject* signals[]{frame.GetReadyToPresent()};
        auto& queue = test::DirectQueue(*_device);
        queue.Submit({.CmdBuffers = commands, .WaitToExecute = waits, .ReadyToPresent = signals});
        const auto presented = _swapchain->Present(std::move(frame));
        queue.Wait();
        return presented.Status;
    }

    Device* _device{nullptr};
    HWND _window{nullptr};
    unique_ptr<SwapChain> _swapchain;
};

TEST_F(VulkanSwapChainWindowTest, PresentsBeforeAndAfterResize) {
    const auto client = ClientRect();
    _swapchain = _device->CreateSwapChain({.PresentQueue = &test::DirectQueue(*_device), .NativeHandler = _window,
                                           .Width = uint32_t(client.right), .Height = uint32_t(client.bottom), .BackBufferCount = 3,
                                           .Format = TextureFormat::BGRA8_UNORM, .PresentMode = PresentMode::FIFO})
                     .Unwrap();
    ASSERT_EQ(PresentFrame(), SwapChainStatus::Success);

    ASSERT_TRUE(::SetWindowPos(_window, nullptr, 0, 0, 240, 180, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    PumpMessages();
    const auto resized = ClientRect();
    ASSERT_TRUE(_swapchain->Recreate(uint32_t(resized.right), uint32_t(resized.bottom), TextureFormat::BGRA8_UNORM, PresentMode::FIFO));

    EXPECT_EQ(_swapchain->GetDesc().Width, uint32_t(resized.right));
    EXPECT_EQ(_swapchain->GetDesc().Height, uint32_t(resized.bottom));
    for (int frame = 0; frame < 3; ++frame) EXPECT_EQ(PresentFrame(), SwapChainStatus::Success) << "frame " << frame;
}

#endif

}  // namespace
}  // namespace radray::render
#endif
