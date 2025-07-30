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
        f.cmdBuffer = std::static_pointer_cast<vulkan::CommandBufferVulkan>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    }
}

void End() {
    cmdQueue->Wait();
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

    swapchain->AcquireNext();
    swapchain->Present();

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
