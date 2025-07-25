#include <thread>

#include <radray/logger.h>

#include <radray/render/dxc.h>
#include <radray/render/common.h>

#include <radray/window/glfw_window.h>

#include "../src/render/vk/vulkan_impl.h"

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int RT_COUNT = 2;

unique_ptr<GlfwWindow> glfw;
shared_ptr<Dxc> dxc;
shared_ptr<Device> device;
shared_ptr<CommandBuffer> cmdBuffer;
shared_ptr<SwapChain> swapchain;
shared_ptr<Fence> fences[RT_COUNT];
shared_ptr<Semaphore> presentSemaphore[2];
int nowPresentIndex = 0;
shared_ptr<Semaphore> renderSemaphore;

void InitGraphics() {
    VulkanBackendInitDescriptor vkInitDesc{};
    vkInitDesc.IsEnableDebugLayer = true;
    vkInitDesc.IsEnableGpuBasedValid = false;
    BackendInitDescriptor initDescs[] = {vkInitDesc};
    GlobalInitGraphics(initDescs);
}

void InitDevice() {
    VulkanDeviceDescriptor vkDesc{};
    VulkanCommandQueueDescriptor queueDesc[] = {
        {QueueType::Direct, 1},
        {QueueType::Compute, 1},
        {QueueType::Copy, 1}};
    vkDesc.Queues = queueDesc;
    device = CreateDevice(vkDesc).Unwrap();
    auto cmdQueue = device->GetCommandQueue(QueueType::Direct).Unwrap();
    cmdBuffer = device->CreateCommandBuffer(cmdQueue).Unwrap();
    SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = cmdQueue;
    swapchainDesc.NativeHandler = glfw->GetNativeHandle();
    swapchainDesc.Width = WIN_WIDTH;
    swapchainDesc.Height = WIN_HEIGHT;
    swapchainDesc.BackBufferCount = RT_COUNT;
    swapchainDesc.Format = TextureFormat::RGBA8_UNORM;
    swapchainDesc.EnableSync = false;
    swapchain = device->CreateSwapChain(swapchainDesc).Unwrap();
    for (auto& i : fences) {
        i = device->CreateFence().Unwrap();
    }
    for (auto& i : presentSemaphore) {
        i = device->CreateGpuSemaphore().Unwrap();
    }
    renderSemaphore = device->CreateGpuSemaphore().Unwrap();
}

void Init() {
    GlobalInitGlfw();
    glfw = make_unique<GlfwWindow>(RADRAY_APPNAME, WIN_WIDTH, WIN_HEIGHT, false, false);

    dxc = CreateDxc().Unwrap();

    InitGraphics();
    InitDevice();
}

void End() {
    auto cmdQueue = device->GetCommandQueue(QueueType::Direct).Unwrap();
    cmdQueue->WaitIdle();

    renderSemaphore.reset();
    for (auto& i : presentSemaphore) {
        i.reset();
    }
    for (auto& i : fences) {
        i.reset();
    }
    swapchain = nullptr;
    cmdBuffer = nullptr;
    device = nullptr;

    GlobalTerminateGraphics();

    dxc = nullptr;

    glfw = nullptr;
    GlobalTerminateGlfw();
}

bool Update() {
    GlobalPollEventsGlfw();
    bool isClose = glfw->ShouldClose();

    auto deviceVulkan = static_cast<vulkan::DeviceVulkan*>(device.get());
    auto swapchainVulkan = static_cast<vulkan::SwapChainVulkan*>(swapchain.get());

    SwapChainAcquireNextDescriptor acquireDesc{};
    acquireDesc.SignalSemaphore = presentSemaphore[nowPresentIndex].get();
    Texture* rt = swapchain->AcquireNextTexture(acquireDesc).Value();
    uint32_t backBufferIndex = swapchain->GetCurrentBackBufferIndex();
    cmdBuffer->Begin();
    auto cmdBufferVulkan = static_cast<vulkan::CommandBufferVulkan*>(cmdBuffer.get());
    {
        VkImageMemoryBarrier imgBarrier{};
        imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imgBarrier.pNext = nullptr;
        imgBarrier.srcAccessMask = 0;
        imgBarrier.dstAccessMask = 0;
        imgBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarrier.image = static_cast<vulkan::ImageVulkan*>(rt)->_image;
        imgBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgBarrier.subresourceRange.baseMipLevel = 0;
        imgBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        imgBarrier.subresourceRange.baseArrayLayer = 0;
        imgBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        deviceVulkan->_ftb.vkCmdPipelineBarrier(
            cmdBufferVulkan->_cmdBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &imgBarrier);
    }
    cmdBuffer->End();

    auto cmdQueue = device->GetCommandQueue(QueueType::Direct).Unwrap();
    {
        auto queueVulkan = static_cast<vulkan::QueueVulkan*>(cmdQueue);
        VkPipelineStageFlags wait_stage{VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = nullptr;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &static_cast<vulkan::SemaphoreVulkan*>(presentSemaphore[nowPresentIndex].get())->_semaphore;
        submitInfo.pWaitDstStageMask = &wait_stage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBufferVulkan->_cmdBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &static_cast<vulkan::SemaphoreVulkan*>(renderSemaphore.get())->_semaphore;
        auto signalFence = static_cast<vulkan::FenceVulkan*>(fences[backBufferIndex].get());
        deviceVulkan->_ftb.vkQueueSubmit(queueVulkan->_queue, 1, &submitInfo, signalFence->_fence);
        signalFence->_isSubmitted = true;
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.pNext = nullptr;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &static_cast<vulkan::SemaphoreVulkan*>(renderSemaphore.get())->_semaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchainVulkan->_swapchain;
        presentInfo.pImageIndices = &swapchainVulkan->_currentFrameIndex;
        presentInfo.pResults = nullptr;
        deviceVulkan->_ftb.vkQueuePresentKHR(queueVulkan->_queue, &presentInfo);

        nowPresentIndex = (nowPresentIndex + 1) % 2;
    }
    {
        Fence* f[] = {fences[backBufferIndex].get()};
        device->WaitFences(f);
        device->ResetFences(f);
    }

    return !isClose;
}

int main() {
    Init();
    while (Update()) {
        std::this_thread::yield();
    }
    End();
    return 0;
}
