#include "gpu_test_fixture.h"

#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/gpu_impl/backend/vulkan_impl.h>
#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::gpu {
namespace {

using vulkan::SwapChainRetirementVulkan;

TEST(VulkanDeviceLoss, FrozenTimelineAndSingleIdleBeforeNativeDestruction) {
    struct Probe {
        bool LoseCounter{};
        int QueueIdles{}, Idles{}, Signals{}, Semaphores{}, Swapchains{}, Devices{};
        int CounterReads{}, Waits{};
    };
    enum class LossAt { Submit, Drain, Counter };
    for (const LossAt lossAt : {LossAt::Submit, LossAt::Drain, LossAt::Counter}) {
        SCOPED_TRACE(static_cast<int>(lossAt));
        Probe probe{};
        vulkan::InstanceVulkanImpl instance{VK_NULL_HANDLE, std::nullopt, {}, {}};
        {
            vulkan::DeviceVulkan device{&instance, VK_NULL_HANDLE, reinterpret_cast<VkDevice>(&probe)};
            device._ftb.vkQueueWaitIdle = +[](VkQueue q) -> VkResult {
                ++reinterpret_cast<Probe*>(q)->QueueIdles;
                return VK_ERROR_DEVICE_LOST;
            };
            device._ftb.vkDeviceWaitIdle = +[](VkDevice d) -> VkResult {
                ++reinterpret_cast<Probe*>(d)->Idles;
                return VK_ERROR_DEVICE_LOST;
            };
            device._ftb.vkGetSemaphoreCounterValue = +[](VkDevice d, VkSemaphore, uint64_t* value) -> VkResult {
                auto& trace = *reinterpret_cast<Probe*>(d);
                ++trace.CounterReads;
                if (trace.LoseCounter) return VK_ERROR_DEVICE_LOST;
                *value = 7;
                return VK_SUCCESS;
            };
            device._ftb.vkWaitSemaphores = +[](VkDevice d, const VkSemaphoreWaitInfo*, uint64_t) -> VkResult {
                ++reinterpret_cast<Probe*>(d)->Waits;
                return VK_TIMEOUT;
            };
            device._ftb.vkQueueSubmit = +[](VkQueue q, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult {
                ++reinterpret_cast<Probe*>(q)->Signals;
                return VK_ERROR_DEVICE_LOST;
            };
            device._ftb.vkDestroySemaphore = +[](VkDevice d, VkSemaphore, const VkAllocationCallbacks*) {
                auto& trace = *reinterpret_cast<Probe*>(d);
                EXPECT_EQ(trace.Idles, 1);
                ++trace.Semaphores;
            };
            device._ftb.vkDestroySwapchainKHR = +[](VkDevice d, VkSwapchainKHR, const VkAllocationCallbacks*) {
                auto& trace = *reinterpret_cast<Probe*>(d);
                EXPECT_EQ(trace.Idles, 1);
                ++trace.Swapchains;
            };
            device._ftb.vkDestroyDevice = +[](VkDevice d, const VkAllocationCallbacks*) {
                auto& trace = *reinterpret_cast<Probe*>(d);
                EXPECT_EQ(trace.Idles, 1);
                ++trace.Devices;
            };
            vulkan::QueueVulkan queue{&device, reinterpret_cast<VkQueue>(&probe), {}, QueueType::Direct, VK_QUEUE_GRAPHICS_BIT, {1, 1, 1}, 0};
            vulkan::FenceVulkan fence{&device, make_unique<vulkan::TimelineSemaphoreVulkan>(&device, reinterpret_cast<VkSemaphore>(&probe))};
            vulkan::SwapChainVulkan chain{&device, &queue, nullptr, reinterpret_cast<VkSwapchainKHR>(&probe), {}};
            EXPECT_EQ(fence.WaitForStatus(7, 0), FenceStatus::Completed);
            EXPECT_EQ(probe.CounterReads, 1);
            EXPECT_EQ(fence.WaitForStatus(8, 0), FenceStatus::NotReady);
            EXPECT_EQ(probe.Waits, 0);
            if (lossAt == LossAt::Counter) {
                probe.LoseCounter = true;
                EXPECT_FALSE(device.IsDeviceLost());
                EXPECT_EQ(fence.WaitForStatus(7, 0), FenceStatus::DeviceLost);
            } else if (lossAt == LossAt::Drain) {
                chain.Destroy();
            } else {
                Fence* signal = &fence;
                uint64_t value = 8;
                EXPECT_FALSE(queue.Submit({.SignalFences = {&signal, 1}, .SignalValues = {&value, 1}}));
            }
            EXPECT_TRUE(device.IsDeviceLost());
            EXPECT_EQ(probe.QueueIdles, lossAt == LossAt::Drain ? 1 : 0);
            EXPECT_EQ(probe.Idles, lossAt == LossAt::Drain ? 1 : 0);
            EXPECT_EQ(fence.WaitForStatus(7, 0), FenceStatus::DeviceLost);
            probe.LoseCounter = true;
            EXPECT_EQ(fence.GetCompletedValue(), 7u);
            EXPECT_EQ(fence.WaitForStatus(8, 30'000'000'000), FenceStatus::DeviceLost);
            fence.Wait(8);
            EXPECT_FALSE(queue.Submit({}));
            queue.Wait();
            EXPECT_EQ(probe.Waits, 0);
            fence.Destroy();
            EXPECT_EQ(probe.Idles, 1);
        }
        EXPECT_EQ(probe.Signals, lossAt == LossAt::Submit ? 1 : 0);
        EXPECT_EQ(probe.Semaphores, 1);
        EXPECT_EQ(probe.Swapchains, 1);
        EXPECT_EQ(probe.Devices, 1);
    }
}

TEST(VulkanSwapChainRetirement, DestructionWaitsOnlyForPresentationQueueAndPendingAcquires) {
    struct Probe { int QueueIdles{}, FenceWaits{}; };
    for (const size_t queueCount : {1u, 2u}) {
        Probe probes[2]{};
        vulkan::InstanceVulkanImpl instance{VK_NULL_HANDLE, std::nullopt, {}, {}};
        vulkan::DeviceVulkan device{&instance, VK_NULL_HANDLE, reinterpret_cast<VkDevice>(probes)};
        device._ftb.vkDeviceWaitIdle = +[](VkDevice) { ADD_FAILURE() << "unexpected device idle"; return VK_SUCCESS; };
        device._ftb.vkQueueWaitIdle = +[](VkQueue q) {
            ++reinterpret_cast<Probe*>(q)->QueueIdles;
            return VK_SUCCESS;
        };
        device._ftb.vkWaitForFences = +[](VkDevice d, uint32_t n, const VkFence*, VkBool32, uint64_t) {
            auto& probe = *reinterpret_cast<Probe*>(d);
            EXPECT_EQ(probe.QueueIdles, 1);
            probe.FenceWaits += n;
            return VK_SUCCESS;
        };
        device._ftb.vkDestroyFence = +[](VkDevice d, VkFence, const VkAllocationCallbacks*) {
            EXPECT_EQ(reinterpret_cast<Probe*>(d)->FenceWaits, 1);
        };
        device._ftb.vkDestroySwapchainKHR = +[](VkDevice d, VkSwapchainKHR, const VkAllocationCallbacks*) {
            EXPECT_EQ(reinterpret_cast<Probe*>(d)->FenceWaits, 1);
        };
        device._ftb.vkDestroyDevice = +[](VkDevice, const VkAllocationCallbacks*) {};
        auto& queues = device._queues[static_cast<size_t>(QueueType::Direct)];
        for (size_t index = 0; index < queueCount; ++index)
            queues.push_back(make_unique<vulkan::QueueVulkan>(&device, reinterpret_cast<VkQueue>(&probes[index]), vulkan::QueueIndexInFamily{},
                QueueType::Direct, VK_QUEUE_GRAPHICS_BIT, VkExtent3D{1, 1, 1}, 0));
        vulkan::SwapChainVulkan chain{&device, queues.front().get(), nullptr, reinterpret_cast<VkSwapchainKHR>(probes), {}};
        chain._pendingAcquires.push_back({1, make_unique<vulkan::LegacyFenceVulkan>(&device, reinterpret_cast<VkFence>(probes)), nullptr, false});
        chain.Destroy();
        EXPECT_EQ(probes[0].QueueIdles, 1);
        EXPECT_EQ(probes[1].QueueIdles, 0);
        EXPECT_EQ(probes[0].FenceWaits, 1);
    }
}

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
        _device = OpenDevice(GpuBackend::Vulkan, DirectQueueCount()).Get();
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

    virtual uint32_t DirectQueueCount() const noexcept { return 1; }

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
    SwapChainStatus PresentFrame(SwapChainStatus* acquiredStatus = nullptr) {
        PumpMessages();
        auto acquired = _swapchain->AcquireNext(1000);
        if (acquiredStatus) *acquiredStatus = acquired.Status;
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

TEST_F(VulkanSwapChainWindowTest, FailedCreationLatchesOnlyDeviceLoss) {
    const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
    auto device = vulkan::CreateDeviceVulkan({.Queues = queues});
    ASSERT_TRUE(device);
    const auto client = ClientRect();
    const SwapChainDescriptor desc{.PresentQueue = &test::DirectQueue(*device.Get()), .NativeHandler = _window,
                                   .Width = uint32_t(client.right), .Height = uint32_t(client.bottom), .BackBufferCount = 3,
                                   .Format = TextureFormat::BGRA8_UNORM, .PresentMode = PresentMode::FIFO};

    device->_ftb.vkCreateSwapchainKHR = +[](VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    };
    EXPECT_FALSE(device->CreateSwapChain(desc));
    ASSERT_FALSE(device->IsDeviceLost());

    device->_ftb.vkCreateSwapchainKHR = +[](VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*) {
        return VK_ERROR_DEVICE_LOST;
    };
    EXPECT_FALSE(device->CreateSwapChain(desc));
    EXPECT_TRUE(device->IsDeviceLost());
    EXPECT_TRUE(device->_lostDrained.load());
    EXPECT_FALSE(device->CreateBuffer({.Size = 16, .Memory = MemoryType::HostReadBack, .Usage = BufferUse::MapRead}));
}

class VulkanSwapChainCleanupWindowTest : public VulkanSwapChainWindowTest, public testing::WithParamInterface<uint32_t> {
protected:
    uint32_t DirectQueueCount() const noexcept override { return GetParam(); }
};

TEST_P(VulkanSwapChainCleanupWindowTest, SurfaceLossRecoversOnTheSameSwapChain) {
    const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, GetParam()}};
    auto device = vulkan::CreateDeviceVulkan({.Queues = queues});
    ASSERT_TRUE(device);
    const auto client = ClientRect();
    const SwapChainDescriptor desc{.PresentQueue = &test::DirectQueue(*device.Get()), .NativeHandler = _window,
                                   .Width = uint32_t(client.right), .Height = uint32_t(client.bottom), .BackBufferCount = 3,
                                   .Format = TextureFormat::BGRA8_UNORM, .PresentMode = PresentMode::FIFO};
    auto chain = device->CreateSwapChain(desc).Release();
    ASSERT_TRUE(chain);
    const auto acquire = device->_ftb.vkAcquireNextImageKHR;
    device->_ftb.vkAcquireNextImageKHR = +[](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*) {
        return VK_ERROR_SURFACE_LOST_KHR;
    };
    const auto lost = chain->AcquireNext(0);
    EXPECT_FALSE(lost.Frame);
    EXPECT_EQ(lost.Status, SwapChainStatus::SurfaceLost);
    EXPECT_EQ(chain->AcquireNext(0).Status, SwapChainStatus::SurfaceLost);
    device->_ftb.vkAcquireNextImageKHR = acquire;
    ASSERT_TRUE(chain->Recreate(desc.Width, desc.Height, desc.Format, desc.PresentMode));
    _device = device.Get();
    _swapchain = std::move(chain);
    EXPECT_EQ(PresentFrame(), SwapChainStatus::Success);
    _swapchain.reset();
}

TEST_P(VulkanSwapChainCleanupWindowTest, FailedRecreateDrainsAndCanRetryFromAnEmptyChain) {
    const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, GetParam()}};
    auto device = vulkan::CreateDeviceVulkan({.Queues = queues});
    ASSERT_TRUE(device);
    const auto client = ClientRect();
    const SwapChainDescriptor desc{.PresentQueue = &test::DirectQueue(*device.Get()), .NativeHandler = _window,
                                   .Width = uint32_t(client.right), .Height = uint32_t(client.bottom), .BackBufferCount = 3,
                                   .Format = TextureFormat::BGRA8_UNORM, .PresentMode = PresentMode::FIFO};
    auto chain = device->CreateSwapChain(desc).Unwrap();
    const auto create = device->_ftb.vkCreateSwapchainKHR;
    const auto idle = device->_ftb.vkDeviceWaitIdle;
    device->_ftb.vkCreateSwapchainKHR = +[](VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    };
    device->_ftb.vkDeviceWaitIdle = +[](VkDevice) { ADD_FAILURE() << "healthy cleanup waited for device"; return VK_SUCCESS; };
    EXPECT_FALSE(chain->Recreate(desc.Width, desc.Height, desc.Format, desc.PresentMode));
    EXPECT_FALSE(device->IsDeviceLost());
    EXPECT_EQ(chain->GetBackBufferCount(), 0u);
    device->_ftb.vkCreateSwapchainKHR = create;
    EXPECT_TRUE(chain->Recreate(desc.Width, desc.Height, desc.Format, desc.PresentMode));
    chain.reset();
    device->_ftb.vkDeviceWaitIdle = idle;
}

INSTANTIATE_TEST_SUITE_P(DirectQueueCounts, VulkanSwapChainCleanupWindowTest, testing::Values(1u, 2u));

TEST_F(VulkanSwapChainWindowTest, SuboptimalAcquireDeliversAPresentableFrame) {
    const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
    auto device = vulkan::CreateDeviceVulkan({.Queues = queues});
    ASSERT_TRUE(device);
    const auto client = ClientRect();
    const SwapChainDescriptor desc{.PresentQueue = &test::DirectQueue(*device.Get()), .NativeHandler = _window,
                                   .Width = uint32_t(client.right), .Height = uint32_t(client.bottom), .BackBufferCount = 3,
                                   .Format = TextureFormat::BGRA8_UNORM, .PresentMode = PresentMode::FIFO};
    _device = device.Get();
    _swapchain = device->CreateSwapChain(desc).Unwrap();
    const auto acquire = device->_ftb.vkAcquireNextImageKHR;
    device->_ftb.vkAcquireNextImageKHR = +[](VkDevice d, VkSwapchainKHR chain, uint64_t timeout, VkSemaphore signal, VkFence fence, uint32_t* index) {
        auto native = reinterpret_cast<PFN_vkAcquireNextImageKHR>(vkGetDeviceProcAddr(d, "vkAcquireNextImageKHR"));
        const auto result = native(d, chain, timeout, signal, fence, index);
        return result == VK_SUCCESS ? VK_SUBOPTIMAL_KHR : result;
    };
    SwapChainStatus status{};
    EXPECT_EQ(PresentFrame(&status), SwapChainStatus::Success);
    EXPECT_EQ(status, SwapChainStatus::Suboptimal);
    device->_ftb.vkAcquireNextImageKHR = acquire;
    const auto queueIdle = device->_ftb.vkQueueWaitIdle;
    const auto deviceIdle = device->_ftb.vkDeviceWaitIdle;
    device->_ftb.vkQueueWaitIdle = +[](VkQueue) { ADD_FAILURE() << "healthy recreate waited for queue"; return VK_SUCCESS; };
    device->_ftb.vkDeviceWaitIdle = +[](VkDevice) { ADD_FAILURE() << "healthy recreate waited for device"; return VK_SUCCESS; };
    EXPECT_TRUE(_swapchain->Recreate(desc.Width, desc.Height, desc.Format, desc.PresentMode));
    device->_ftb.vkQueueWaitIdle = queueIdle;
    device->_ftb.vkDeviceWaitIdle = deviceIdle;
    EXPECT_EQ(PresentFrame(), SwapChainStatus::Success);
    _swapchain.reset();
}

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
}  // namespace radray::gpu
#endif
