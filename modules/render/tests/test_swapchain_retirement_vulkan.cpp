#include <gtest/gtest.h>

#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>

namespace radray::render::vulkan {
namespace {

TEST(VulkanSwapChainRetirementTest, SuccessorProofAndOwnAcquiresAreBothRequired) {
    SwapChainRetirementVulkan tracker;
    const auto a = tracker.BeginGeneration();
    tracker.Acquire(a);
    tracker.QueuePresent(a);
    tracker.Retire(a);
    const auto b = tracker.BeginGeneration();
    tracker.Acquire(b);
    EXPECT_TRUE(tracker.Collect().empty());
    tracker.CompleteAcquire(b, true);
    EXPECT_TRUE(tracker.Collect().empty());
    tracker.CompleteAcquire(a, false);
    EXPECT_EQ(tracker.Collect(), (vector<uint64_t>{a}));
    EXPECT_TRUE(tracker.Collect().empty());
}

TEST(VulkanSwapChainRetirementTest, OwnPresentationDoesNotReleaseRetiredGeneration) {
    SwapChainRetirementVulkan tracker;
    const auto a = tracker.BeginGeneration();
    tracker.QueuePresent(a);
    tracker.Acquire(a);
    tracker.CompleteAcquire(a, true);
    tracker.Retire(a);
    EXPECT_TRUE(tracker.Collect().empty());
}

TEST(VulkanSwapChainRetirementTest, LaterGenerationReleasesMultiplePresentedAncestors) {
    SwapChainRetirementVulkan tracker;
    const auto a = tracker.BeginGeneration();
    tracker.QueuePresent(a);
    tracker.Retire(a);
    const auto b = tracker.BeginGeneration();
    tracker.QueuePresent(b);
    tracker.Retire(b);
    const auto c = tracker.BeginGeneration();
    tracker.Acquire(c);
    tracker.CompleteAcquire(c, false);
    EXPECT_TRUE(tracker.Collect().empty());
    tracker.QueuePresent(c);
    tracker.Acquire(c);
    tracker.CompleteAcquire(c, true);
    EXPECT_EQ(tracker.Collect(), (vector<uint64_t>{a, b}));
}

TEST(VulkanSwapChainRetirementTest, UnpresentedGenerationStillWaitsForAcquire) {
    SwapChainRetirementVulkan tracker;
    const auto a = tracker.BeginGeneration();
    tracker.Acquire(a);
    tracker.Retire(a);
    EXPECT_TRUE(tracker.Collect().empty());
    tracker.CompleteAcquire(a, false);
    EXPECT_EQ(tracker.Collect(), (vector<uint64_t>{a}));
}

TEST(VulkanSwapChainRetirementTest, DifferentSwapchainsNeverShareProof) {
    SwapChainRetirementVulkan left, right;
    const auto a = left.BeginGeneration();
    left.QueuePresent(a);
    left.Retire(a);
    right.BeginGeneration();
    const auto b = right.BeginGeneration();
    right.Acquire(b);
    right.CompleteAcquire(b, true);
    EXPECT_TRUE(left.Collect().empty());
    left.Clear();
    EXPECT_TRUE(left.Collect().empty());
    EXPECT_GT(left.BeginGeneration(), a);
}

struct SwapchainDriverProbe {
    struct Acquisition {
        VkSemaphore Semaphore;
        VkFence Fence;
    };
    uintptr_t NextHandle{1};
    unordered_map<VkFence, bool> Fences;
    unordered_set<VkSemaphore> Semaphores;
    vector<Acquisition> Acquisitions;
    vector<VkSwapchainKHR> DestroyedSwapchains;
    uint32_t ImageIndex{0};
    VkResult AcquireResult{VK_SUCCESS};
    VkResult PresentResult{VK_SUCCESS};
    VkResult PollResult{VK_SUCCESS};
    VkResult IdleResult{VK_SUCCESS};
    uint32_t IdleCalls{0}, WaitCalls{0}, SubmitCalls{0}, PresentCalls{0};

    template <class T>
    T Handle() { return reinterpret_cast<T>(NextHandle++); }
    static SwapchainDriverProbe& From(VkDevice device) { return *reinterpret_cast<SwapchainDriverProbe*>(device); }
    static VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice device, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
        auto& probe = From(device);
        *fence = probe.Handle<VkFence>();
        probe.Fences.emplace(*fence, false);
        return VK_SUCCESS;
    }
    static VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks*) {
        EXPECT_EQ(From(device).Fences.erase(fence), 1u);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL FenceStatus(VkDevice device, VkFence fence) {
        auto& probe = From(device);
        if (probe.PollResult != VK_SUCCESS) return probe.PollResult;
        EXPECT_TRUE(probe.Fences.contains(fence));
        return probe.Fences.at(fence) ? VK_SUCCESS : VK_NOT_READY;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice device, uint32_t count, const VkFence* fences) {
        auto& probe = From(device);
        for (uint32_t i = 0; i < count; ++i) {
            EXPECT_TRUE(probe.Fences.at(fences[i]));
            probe.Fences.at(fences[i]) = false;
        }
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL WaitFences(VkDevice device, uint32_t count, const VkFence* fences, VkBool32, uint64_t) {
        auto& probe = From(device);
        ++probe.WaitCalls;
        for (uint32_t i = 0; i < count; ++i) probe.Fences.at(fences[i]) = true;
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* semaphore) {
        auto& probe = From(device);
        *semaphore = probe.Handle<VkSemaphore>();
        probe.Semaphores.emplace(*semaphore);
        return VK_SUCCESS;
    }
    static VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks*) {
        EXPECT_EQ(From(device).Semaphores.erase(semaphore), 1u);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice device, VkSwapchainKHR, uint64_t, VkSemaphore semaphore, VkFence fence, uint32_t* index) {
        auto& probe = From(device);
        EXPECT_NE(fence, VK_NULL_HANDLE);
        EXPECT_FALSE(probe.Fences.at(fence));
        probe.Acquisitions.push_back({semaphore, fence});
        if (probe.AcquireResult == VK_SUCCESS || probe.AcquireResult == VK_SUBOPTIMAL_KHR) *index = probe.ImageIndex;
        return probe.AcquireResult;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue, const VkPresentInfoKHR* info) {
        auto& probe = *reinterpret_cast<SwapchainDriverProbe*>(queue);
        ++probe.PresentCalls;
        EXPECT_EQ(info->pNext, nullptr);
        EXPECT_EQ(info->waitSemaphoreCount, 1u);
        return probe.PresentResult;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
        auto& probe = *reinterpret_cast<SwapchainDriverProbe*>(queue);
        ++probe.SubmitCalls;
        EXPECT_EQ(count, 1u);
        EXPECT_EQ(fence, VK_NULL_HANDLE);
        EXPECT_EQ(submits->waitSemaphoreCount, 1u);
        EXPECT_EQ(submits->pWaitDstStageMask[0], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Idle(VkDevice device) {
        auto& probe = From(device);
        ++probe.IdleCalls;
        return probe.IdleResult;
    }
    static VKAPI_ATTR void VKAPI_CALL DestroySwapchain(VkDevice device, VkSwapchainKHR chain, const VkAllocationCallbacks*) {
        auto& probe = From(device);
        EXPECT_EQ(std::ranges::count(probe.DestroyedSwapchains, chain), 0);
        probe.DestroyedSwapchains.push_back(chain);
    }
    static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice, const VkAllocationCallbacks*) {}
};

class VulkanSwapChainNativeTest : public testing::Test {
protected:
    SwapchainDriverProbe Probe;
    InstanceVulkanImpl Instance{VK_NULL_HANDLE, std::nullopt, {}, {}};
    DeviceVulkan Device{&Instance, VK_NULL_HANDLE, reinterpret_cast<VkDevice>(&Probe)};
    QueueVulkan Queue{&Device, reinterpret_cast<VkQueue>(&Probe), {}, QueueType::Direct, 0};
    unique_ptr<SwapChainVulkan> Chain;

    void SetUp() override {
        auto& table = Device._ftb;
        table = {};
        table.vkCreateFence = Probe.CreateFence;
        table.vkDestroyFence = Probe.DestroyFence;
        table.vkGetFenceStatus = Probe.FenceStatus;
        table.vkResetFences = Probe.ResetFences;
        table.vkWaitForFences = Probe.WaitFences;
        table.vkCreateSemaphore = Probe.CreateSemaphore;
        table.vkDestroySemaphore = Probe.DestroySemaphore;
        table.vkAcquireNextImageKHR = Probe.Acquire;
        table.vkQueuePresentKHR = Probe.Present;
        table.vkQueueSubmit = Probe.Submit;
        table.vkDeviceWaitIdle = Probe.Idle;
        table.vkDestroySwapchainKHR = Probe.DestroySwapchain;
        table.vkDestroyDevice = Probe.DestroyDevice;
        Chain = make_unique<SwapChainVulkan>(&Device, &Queue, nullptr, Probe.Handle<VkSwapchainKHR>(), SwapChainDescriptor{});
        AddImages();
    }
    void AddImages() {
        for (uint32_t i = 0; i < 2; ++i) {
            auto& frame = Chain->_frames.emplace_back();
            frame.image = make_unique<ImageVulkan>(&Device, Probe.Handle<VkImage>(), VK_NULL_HANDLE, VmaAllocationInfo{});
            frame.image->_hints = ResourceHint::External;
            frame.readyToPresent = Chain->AcquireSyncObjectFromPool().Release();
        }
    }
    void TearDown() override {
        Chain.reset();
        EXPECT_TRUE(Probe.Fences.empty());
        EXPECT_TRUE(Probe.Semaphores.empty());
    }
    void SubmitAndPresent(SwapChainFrame frame) {
        SwapChainSyncObject* waits[]{frame.GetWaitToDraw()};
        SwapChainSyncObject* signals[]{frame.GetReadyToPresent()};
        Queue.Submit({.WaitToExecute = waits, .ReadyToPresent = signals});
        Chain->Present(std::move(frame));
    }
    void ReplaceGeneration() {
        Chain->_retirement->Retire(Chain->_generation);
        Chain->_retiredGenerations.push_back({Chain->_generation, Chain->_swapchain, std::move(Chain->_frames)});
        Chain->_frames.clear();
        Chain->_swapchain = Probe.Handle<VkSwapchainKHR>();
        Chain->_generation = Chain->_retirement->BeginGeneration();
        Chain->_requiresRecreate = false;
        AddImages();
    }
};

TEST_F(VulkanSwapChainNativeTest, ReacquireReturnDoesNotRecycleSemaphoreUntilFenceSignals) {
    auto first = Chain->AcquireNext(0);
    ASSERT_TRUE(first.Frame);
    const auto firstAcquire = Probe.Acquisitions.back();
    SubmitAndPresent(std::move(*first.Frame));
    auto second = Chain->AcquireNext(0);
    ASSERT_TRUE(second.Frame);
    const auto secondAcquire = Probe.Acquisitions.back();
    EXPECT_NE(firstAcquire.Semaphore, secondAcquire.Semaphore);
    EXPECT_TRUE(Chain->_recycledSyncObjects.empty());
    SubmitAndPresent(std::move(*second.Frame));
    Probe.Fences.at(secondAcquire.Fence) = true;
    Chain->CollectRetired();
    auto third = Chain->AcquireNext(0);
    ASSERT_TRUE(third.Frame);
    EXPECT_EQ(Probe.Acquisitions.back().Semaphore, firstAcquire.Semaphore);
    EXPECT_EQ(Probe.SubmitCalls, 2u);
    EXPECT_EQ(Probe.IdleCalls, 0u);
    EXPECT_EQ(Probe.WaitCalls, 0u);
    SubmitAndPresent(std::move(*third.Frame));
}

TEST_F(VulkanSwapChainNativeTest, OtherImageAndFirstAcquireOfNewGenerationDoNotProveRetirement) {
    auto frame = Chain->AcquireNext(0);
    SubmitAndPresent(std::move(*frame.Frame));
    Probe.Fences.at(Probe.Acquisitions.back().Fence) = true;
    Chain->CollectRetired();
    const auto old = Chain->_swapchain;
    ReplaceGeneration();
    frame = Chain->AcquireNext(0);
    Probe.Fences.at(Probe.Acquisitions.back().Fence) = true;
    SubmitAndPresent(std::move(*frame.Frame));
    EXPECT_TRUE(Probe.DestroyedSwapchains.empty());
    Probe.ImageIndex = 1;
    frame = Chain->AcquireNext(0);
    Probe.Fences.at(Probe.Acquisitions.back().Fence) = true;
    SubmitAndPresent(std::move(*frame.Frame));
    EXPECT_TRUE(Probe.DestroyedSwapchains.empty());
    frame = Chain->AcquireNext(0);
    Probe.Fences.at(Probe.Acquisitions.back().Fence) = true;
    SubmitAndPresent(std::move(*frame.Frame));
    EXPECT_EQ(Probe.DestroyedSwapchains, (vector<VkSwapchainKHR>{old}));
    EXPECT_TRUE(Chain->_retiredGenerations.empty());
    EXPECT_EQ(Probe.IdleCalls, 0u);
}

TEST_F(VulkanSwapChainNativeTest, FailedAcquireDoesNotCreatePendingFenceOrProof) {
    for (const auto status : {VK_TIMEOUT, VK_NOT_READY, VK_ERROR_OUT_OF_DATE_KHR}) {
        Probe.AcquireResult = status;
        const auto result = Chain->AcquireNext(0);
        EXPECT_FALSE(result.Frame);
        EXPECT_TRUE(Chain->_pendingAcquires.empty());
        EXPECT_EQ(result.Status, status == VK_ERROR_OUT_OF_DATE_KHR ? SwapChainStatus::RequireRecreate : SwapChainStatus::RetryLater);
    }
    EXPECT_EQ(Probe.WaitCalls, 0u);
    EXPECT_EQ(Probe.Fences.size(), 1u);
}

TEST_F(VulkanSwapChainNativeTest, SuboptimalAcquireStillProducesConsumableFrame) {
    Probe.AcquireResult = VK_SUBOPTIMAL_KHR;
    auto acquired = Chain->AcquireNext(0);
    ASSERT_TRUE(acquired.Frame);
    EXPECT_EQ(acquired.Status, SwapChainStatus::Success);
    Probe.PresentResult = VK_SUBOPTIMAL_KHR;
    auto result = Chain->Present(std::move(*acquired.Frame));
    EXPECT_EQ(result.Status, SwapChainStatus::RequireRecreate);
    EXPECT_TRUE(Chain->_frames[0].successfulPresent);
    EXPECT_FALSE(Chain->_outstandingAcquire.IsValid());
}

TEST_F(VulkanSwapChainNativeTest, PresentOomConsumesTokenButDoesNotRecycleItsSemaphore) {
    auto acquired = Chain->AcquireNext(0);
    ASSERT_TRUE(acquired.Frame);
    Probe.PresentResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto result = Chain->Present(std::move(*acquired.Frame));
    EXPECT_EQ(result.Status, SwapChainStatus::Error);
    EXPECT_FALSE(Chain->_outstandingAcquire.IsValid());
    EXPECT_FALSE(Chain->_frames[0].successfulPresent);
    EXPECT_TRUE(Chain->_recycledSyncObjects.empty());
    EXPECT_EQ(Chain->AcquireNext(0).Status, SwapChainStatus::RequireRecreate);
    EXPECT_EQ(Probe.Acquisitions.size(), 1u);
}

TEST_F(VulkanSwapChainNativeTest, OutOfDatePresentStaysAliveUntilSuccessorProof) {
    auto acquired = Chain->AcquireNext(0);
    Probe.PresentResult = VK_ERROR_OUT_OF_DATE_KHR;
    auto result = Chain->Present(std::move(*acquired.Frame));
    EXPECT_EQ(result.Status, SwapChainStatus::RequireRecreate);
    Probe.Fences.at(Probe.Acquisitions.back().Fence) = true;
    Chain->CollectRetired();
    ReplaceGeneration();
    Chain->CollectRetired();
    EXPECT_TRUE(Probe.DestroyedSwapchains.empty());
}

TEST_F(VulkanSwapChainNativeTest, ShutdownWaitsAcquiresAndIsIdempotent) {
    auto acquired = Chain->AcquireNext(0);
    SubmitAndPresent(std::move(*acquired.Frame));
    ReplaceGeneration();
    Chain->Destroy();
    EXPECT_EQ(Probe.IdleCalls, 1u);
    EXPECT_EQ(Probe.WaitCalls, 1u);
    EXPECT_EQ(Probe.DestroyedSwapchains.size(), 2u);
    Chain->Destroy();
    EXPECT_EQ(Probe.IdleCalls, 1u);
}

TEST_F(VulkanSwapChainNativeTest, DeviceLostSkipsUnsignalableFenceWaits) {
    auto acquired = Chain->AcquireNext(0);
    SubmitAndPresent(std::move(*acquired.Frame));
    Probe.PollResult = VK_ERROR_DEVICE_LOST;
    EXPECT_EQ(Chain->AcquireNext(0).NativeStatusCode, VK_ERROR_DEVICE_LOST);
    Chain->Destroy();
    EXPECT_EQ(Probe.IdleCalls, 0u);
    EXPECT_EQ(Probe.WaitCalls, 0u);
}

TEST_F(VulkanSwapChainNativeTest, DeviceLostDuringIdleSkipsFenceWaits) {
    auto acquired = Chain->AcquireNext(0);
    SubmitAndPresent(std::move(*acquired.Frame));
    Probe.IdleResult = VK_ERROR_DEVICE_LOST;
    Chain->Destroy();
    EXPECT_EQ(Probe.IdleCalls, 1u);
    EXPECT_EQ(Probe.WaitCalls, 0u);
}

TEST_F(VulkanSwapChainNativeTest, SurfaceLostRequiresNewObject) {
    auto acquired = Chain->AcquireNext(0);
    Probe.PresentResult = VK_ERROR_SURFACE_LOST_KHR;
    EXPECT_EQ(Chain->Present(std::move(*acquired.Frame)).Status, SwapChainStatus::Error);
    EXPECT_EQ(Chain->AcquireNext(0).NativeStatusCode, VK_ERROR_SURFACE_LOST_KHR);
}

}  // namespace
}  // namespace radray::render::vulkan
#endif
