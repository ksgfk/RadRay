#include <thread>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/window/glfw_window.h>

#include "../src/render/vk/vulkan_impl.h"

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int RT_COUNT = 2;

struct FrameData {
    shared_ptr<vulkan::FenceVulkan> fence;
    shared_ptr<vulkan::SemaphoreVulkan> imageAvailableSemaphore;
    shared_ptr<vulkan::SemaphoreVulkan> renderFinishedSemaphore;
    shared_ptr<vulkan::CommandBufferVulkan> cmdBuffer;
};

unique_ptr<GlfwWindow> glfw;
shared_ptr<vulkan::DeviceVulkan> device;
vulkan::QueueVulkan* cmdQueue = nullptr;
shared_ptr<vulkan::SwapChainVulkan> swapchain;
vector<FrameData> frames;
uint32_t currentFrameIndex = 0;
uint32_t currentTextureIndex = 0;
FrameData* submitSync = nullptr;

void Init() {
    GlobalInitGlfw();
    glfw = make_unique<GlfwWindow>(RADRAY_APPNAME, WIN_WIDTH, WIN_HEIGHT, false, false);
    VulkanBackendInitDescriptor backendDesc{};
    backendDesc.IsEnableDebugLayer = true;
    vulkan::GlobalInitVulkan(backendDesc);
    VulkanDeviceDescriptor deviceDesc{};
    VulkanCommandQueueDescriptor queueDesc[] = {
        {QueueType::Direct, 1}};
    deviceDesc.Queues = queueDesc;
    device = vulkan::CreateDeviceVulkan(deviceDesc).Unwrap();
    cmdQueue = static_cast<vulkan::QueueVulkan*>(device->GetCommandQueue(QueueType::Direct, 0).Unwrap());
    SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = cmdQueue;
    swapchainDesc.NativeHandler = glfw->GetNativeHandle();
    swapchainDesc.Width = WIN_WIDTH;
    swapchainDesc.Height = WIN_HEIGHT;
    swapchainDesc.BackBufferCount = RT_COUNT;
    swapchainDesc.Format = TextureFormat::RGBA8_UNORM;
    swapchainDesc.EnableSync = false;
    swapchain = std::static_pointer_cast<vulkan::SwapChainVulkan>(device->CreateSwapChain(swapchainDesc).Unwrap());
    frames.reserve(swapchain->_frames.size());
    for (size_t i = 0; i < swapchain->_frames.size(); ++i) {
        auto& f = frames.emplace_back();
        f.fence = device->CreateFence(VK_FENCE_CREATE_SIGNALED_BIT).Unwrap();
        f.imageAvailableSemaphore = device->CreateGpuSemaphore(0).Unwrap();
        f.renderFinishedSemaphore = device->CreateGpuSemaphore(0).Unwrap();
        f.cmdBuffer = std::static_pointer_cast<vulkan::CommandBufferVulkan>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    }
}

void End() {
    cmdQueue->WaitIdle();
    frames.clear();
    swapchain = nullptr;
    cmdQueue = nullptr;
    device = nullptr;
    vulkan::GlobalTerminateVulkan();
    glfw = nullptr;
    GlobalTerminateGlfw();
}

bool Update() {
    GlobalPollEventsGlfw();
    bool isClose = glfw->ShouldClose();

    FrameData& frameData = frames[currentFrameIndex];
    device->_ftb.vkWaitForFences(device->_device, 1, &frameData.fence->_fence, VK_TRUE, UINT64_MAX);
    device->_ftb.vkResetFences(device->_device, 1, &frameData.fence->_fence);
    currentTextureIndex = std::numeric_limits<uint32_t>::max();
    if (auto vr = device->_ftb.vkAcquireNextImageKHR(
            device->_device,
            swapchain->_swapchain,
            UINT64_MAX,
            frameData.imageAvailableSemaphore->_semaphore,
            VK_NULL_HANDLE,
            &currentTextureIndex);
        vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        RADRAY_ABORT("vk call vkAcquireNextImageKHR failed: {}", vr);
    }
    submitSync = &frameData;
    frameData.cmdBuffer->Begin();
    VkImageMemoryBarrier imgBarrier{};
    imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imgBarrier.pNext = nullptr;
    imgBarrier.srcAccessMask = 0;
    imgBarrier.dstAccessMask = 0;
    imgBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgBarrier.image = swapchain->_frames[currentTextureIndex]->_image;
    imgBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imgBarrier.subresourceRange.baseMipLevel = 0;
    imgBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    imgBarrier.subresourceRange.baseArrayLayer = 0;
    imgBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    device->_ftb.vkCmdPipelineBarrier(
        frameData.cmdBuffer->_cmdBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &imgBarrier);
    frameData.cmdBuffer->End();
    VkPipelineStageFlags wait_stage{VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = nullptr;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &submitSync->imageAvailableSemaphore->_semaphore;
    submitInfo.pWaitDstStageMask = &wait_stage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frameData.cmdBuffer->_cmdBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &submitSync->renderFinishedSemaphore->_semaphore;
    device->_ftb.vkQueueSubmit(cmdQueue->_queue, 1, &submitInfo, submitSync->fence->_fence);
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &submitSync->renderFinishedSemaphore->_semaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain->_swapchain;
    presentInfo.pImageIndices = &currentTextureIndex;
    presentInfo.pResults = nullptr;
    device->_ftb.vkQueuePresentKHR(cmdQueue->_queue, &presentInfo);
    currentFrameIndex = (currentFrameIndex + 1) % frames.size();

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
