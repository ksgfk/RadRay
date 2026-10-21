#include <gtest/gtest.h>

#if defined(RADRAY_ENABLE_VULKAN) && defined(RADRAY_PLATFORM_WINDOWS)
#include <windows.h>
#include <radray/scope_guard.h>
#include "gpu_test_fixture.h"
#include "rhi_validation.h"

namespace radray::render {
namespace {

class VulkanSwapChainWsiTest : public testing::Test {
protected:
    test::DeviceContext Context;
    vector<HWND> Windows;
    vector<unique_ptr<SwapChain>> Chains;

    void SetUp() override {
        if (!test::TryCreateDevice(RenderBackend::Vulkan, Context, true, false)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(RenderBackend::Vulkan))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
        ASSERT_TRUE(AddWindowAndChain());
    }
    void TearDown() override {
        if (Context.Queue) Context.Queue->Wait();
        Chains.clear();
        for (HWND window : Windows) ::DestroyWindow(window);
        Windows.clear();
        Context.Reset();
        EXPECT_EQ(Context.ValidationErrors.load(), 0u);
        EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
    }
    void Pump() {
        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
    bool AddWindowAndChain() {
        HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"RadRay Vulkan WSI test",
                                        WS_OVERLAPPEDWINDOW, 0, 0, 192, 144, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!window) return false;
        Windows.push_back(window);
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
        Pump();
        RECT client{};
        if (!::GetClientRect(window, &client)) return false;
        SwapChainDescriptor desc;
        desc.PresentQueue = Context.Queue;
        desc.NativeHandler = window;
        desc.Width = client.right;
        desc.Height = client.bottom;
        desc.BackBufferCount = 3;
        desc.Format = TextureFormat::BGRA8_UNORM;
        auto chain = Context.Device->CreateSwapChain(desc);
        if (!chain) return false;
        Chains.push_back(chain.Release());
        return true;
    }
    bool Recreate(SwapChain& chain, PresentMode mode = PresentMode::FIFO) {
        Context.Queue->Wait();
        const auto desc = chain.GetDesc();
        RECT client{};
        if (!::GetClientRect(static_cast<HWND>(const_cast<void*>(desc.NativeHandler)), &client)) return false;
        return chain.Recreate(client.right, client.bottom, desc.Format, mode);
    }
    bool Draw(SwapChain& chain) {
        Pump();
        auto allocator = Context.Device->CreateCommandAllocator(Context.Queue);
        if (!allocator) return false;
        auto command = Context.Device->CreateCommandBuffer(allocator.Get());
        if (!command) return false;
        auto acquired = chain.AcquireNext(1000);
        if (!acquired.Frame) {
            ADD_FAILURE() << "Acquire failed: " << static_cast<int>(acquired.Status) << ", native " << acquired.NativeStatusCode;
            return false;
        }
        auto frame = std::move(*acquired.Frame);
        command->Begin();
        const ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{
            .Target = frame.GetBackBuffer(), .Before = TextureState::Undefined, .After = TextureState::Present};
        command->ResourceBarrier(std::span{&barrier, 1});
        command->End();
        CommandBuffer* commands[]{command.Get()};
        SwapChainSyncObject* waits[]{frame.GetWaitToDraw()};
        SwapChainSyncObject* signals[]{frame.GetReadyToPresent()};
        Context.Queue->Submit({.CmdBuffers = commands, .WaitToExecute = waits, .ReadyToPresent = signals});
        const auto presented = chain.Present(std::move(frame));
        Context.Queue->Wait();
        allocator->Reset();
        command.Release().reset();
        allocator.Release().reset();
        if (presented.Status == SwapChainStatus::RequireRecreate) return Recreate(chain, chain.GetDesc().PresentMode);
        EXPECT_EQ(presented.Status, SwapChainStatus::Success);
        return presented.Status == SwapChainStatus::Success;
    }
    vulkan::DeviceVulkan& NativeDevice() { return *static_cast<vulkan::DeviceVulkan*>(Context.Device.get()); }
    vulkan::SwapChainVulkan& NativeChain(size_t index = 0) { return *static_cast<vulkan::SwapChainVulkan*>(Chains[index].get()); }
    static VKAPI_ATTR VkResult VKAPI_CALL FailCreate(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL FailSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL UnexpectedDeviceIdle(VkDevice) {
        ADD_FAILURE() << "Normal swapchain operation called vkDeviceWaitIdle";
        return VK_SUCCESS;
    }
};

TEST_F(VulkanSwapChainWsiTest, ContinuousPresentationResizeAndModeChanges) {
    auto& chain = NativeChain();
    vector<VkPresentModeKHR> modes;
    ASSERT_EQ(vulkan::EnumerateVectorFromVkFunc(modes, vkGetPhysicalDeviceSurfacePresentModesKHR,
                                                NativeDevice()._physicalDevice, chain._surface->_surface),
              VK_SUCCESS);
    const auto alternate = std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end() ? PresentMode::Mailbox : PresentMode::FIFO;
    const auto originalIdle = NativeDevice()._ftb.vkDeviceWaitIdle;
    auto restore = MakeScopeGuard([&] { NativeDevice()._ftb.vkDeviceWaitIdle = originalIdle; });
    NativeDevice()._ftb.vkDeviceWaitIdle = UnexpectedDeviceIdle;
    for (uint32_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(Draw(chain));
        ASSERT_TRUE(::SetWindowPos(Windows[0], nullptr, 0, 0, 200 + i * 8, 160 + i * 5, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        Pump();
        ASSERT_TRUE(Recreate(chain, i % 2 ? alternate : PresentMode::FIFO));
    }
    for (uint32_t i = 0; i < 16 && !chain._retiredGenerations.empty(); ++i) ASSERT_TRUE(Draw(chain));
    chain.CollectRetired();
    EXPECT_TRUE(chain._retiredGenerations.empty());
}

TEST_F(VulkanSwapChainWsiTest, TwoSwapchainsHaveIndependentRetirementAndShutdown) {
    ASSERT_TRUE(AddWindowAndChain());
    for (uint32_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(Draw(*Chains[0]));
        ASSERT_TRUE(Draw(*Chains[1]));
    }
    ASSERT_TRUE(Recreate(*Chains[0]));
    ASSERT_TRUE(Recreate(*Chains[1]));
    for (uint32_t i = 0; i < 8; ++i) ASSERT_TRUE(Draw(*Chains[0]));
    NativeChain(0).CollectRetired();
    EXPECT_TRUE(NativeChain(0)._retiredGenerations.empty());
    EXPECT_FALSE(NativeChain(1)._retiredGenerations.empty());
    Chains[1]->Destroy();
    Chains[1]->Destroy();
    ASSERT_TRUE(Draw(*Chains[0]));
}

TEST_F(VulkanSwapChainWsiTest, NativeCreateFailureClearsOldGenerationsAndRetriesFromEmpty) {
    auto& chain = NativeChain();
    ASSERT_TRUE(Draw(chain));
    ASSERT_TRUE(Recreate(chain));
    const auto original = NativeDevice()._ftb.vkCreateSwapchainKHR;
    {
        auto restore = MakeScopeGuard([&] { NativeDevice()._ftb.vkCreateSwapchainKHR = original; });
        NativeDevice()._ftb.vkCreateSwapchainKHR = FailCreate;
        EXPECT_FALSE(Recreate(chain));
    }
    EXPECT_EQ(chain.GetBackBufferCount(), 0u);
    EXPECT_FALSE(chain.IsValid());
    EXPECT_TRUE(chain._retiredGenerations.empty());
    EXPECT_TRUE(chain._pendingAcquires.empty());
    ASSERT_TRUE(Recreate(chain));
    ASSERT_TRUE(Draw(chain));
}

TEST_F(VulkanSwapChainWsiTest, PartialImageInitializationFailureClearsAndRetries) {
    auto& chain = NativeChain();
    ASSERT_TRUE(Draw(chain));
    Context.Queue->Wait();
    chain.CollectRetired();
    auto heldPool = std::move(chain._recycledSyncObjects);
    chain._recycledSyncObjects.clear();
    // The first image gets this semaphore; allocation for the next image fails.
    auto semaphore = NativeDevice().CreateLegacySemaphore(0);
    ASSERT_TRUE(semaphore);
    chain._recycledSyncObjects.push_back(make_unique<vulkan::SwapChainSyncObjectVulkan>(semaphore.Release()));
    const auto original = NativeDevice()._ftb.vkCreateSemaphore;
    {
        auto restore = MakeScopeGuard([&] { NativeDevice()._ftb.vkCreateSemaphore = original; });
        NativeDevice()._ftb.vkCreateSemaphore = FailSemaphore;
        EXPECT_FALSE(Recreate(chain));
    }
    EXPECT_EQ(chain.GetBackBufferCount(), 0u);
    EXPECT_TRUE(chain._retiredGenerations.empty());
    EXPECT_TRUE(chain._pendingAcquires.empty());
    ASSERT_TRUE(Recreate(chain));
    ASSERT_TRUE(Draw(chain));
}

TEST_F(VulkanSwapChainWsiTest, RhiValidationWrappersFollowRecreation) {
    Chains.clear();
    auto checked = detail::CreateValidationDevice(Context.Device, {.IsEnabled = true, .LogCallback = test::CaptureRhiValidationMessage, .LogUserData = &Context}).Unwrap();
    auto originalDevice = Context.Device;
    auto* originalQueue = Context.Queue;
    auto restore = MakeScopeGuard([&] {
        Chains.clear();
        Context.Queue = originalQueue;
        Context.Device = originalDevice;
    });
    Context.Device = checked;
    Context.Queue = checked->GetCommandQueue(QueueType::Direct, 0).Unwrap();
    ASSERT_TRUE(AddWindowAndChain());
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t frame = 0; frame < 4; ++frame) ASSERT_TRUE(Draw(*Chains[0]));
        ASSERT_TRUE(Recreate(*Chains[0]));
    }
    ASSERT_TRUE(Draw(*Chains[0]));
}

}  // namespace
}  // namespace radray::render
#endif
