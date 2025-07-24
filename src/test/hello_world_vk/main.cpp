#include <thread>

#include <radray/logger.h>

#include <radray/render/dxc.h>
#include <radray/render/common.h>

#include <radray/window/glfw_window.h>

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
shared_ptr<Semaphore> presentSemaphore;

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
    presentSemaphore = device->CreateGpuSemaphore().Unwrap();
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

    presentSemaphore = nullptr;
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

    SwapChainAcquireNextDescriptor acquireDesc{};
    acquireDesc.SignalSemaphore = presentSemaphore.get();
    swapchain->AcquireNextTexture(acquireDesc);
    {
        Fence* f[] = {fences[swapchain->GetCurrentBackBufferIndex()].get()};
        device->WaitFences(f);
    }
    cmdBuffer->Begin();
    Texture* rt = swapchain->GetCurrentBackBuffer().Value();
    {
        BarrierTextureDescriptor texBarriers[] = {
            {rt,
             TextureUse::Uninitialized,
             TextureUse::Present,
             nullptr,
             false,
             false,
             0, 0, 0, 0}};
        cmdBuffer->ResourceBarrier({}, texBarriers);
    }
    cmdBuffer->End();
    auto cmdQueue = device->GetCommandQueue(QueueType::Direct).Unwrap();

    // 创建一个渲染完成信号量用于同步
    static shared_ptr<Semaphore> renderCompleteSemaphore = device->CreateGpuSemaphore().Unwrap();

    {
        CommandBuffer* submits[] = {cmdBuffer.get()};
        Semaphore* waitSemaphores[] = {presentSemaphore.get()};           // 等待acquire完成
        Semaphore* signalSemaphores[] = {renderCompleteSemaphore.get()};  // 信号渲染完成
        CommandQueueSubmitDescriptor submitDesc{};
        submitDesc.CmdBuffers = submits;
        submitDesc.WaitSemaphores = waitSemaphores;      // 添加等待信号量
        submitDesc.SignalSemaphores = signalSemaphores;  // 添加信号信号量
        submitDesc.SignalFence = fences[swapchain->GetCurrentBackBufferIndex()].get();
        cmdQueue->Submit(submitDesc);
    }
    {
        Semaphore* waitSemaphores[] = {renderCompleteSemaphore.get()};  // 等待渲染完成
        CommandQueuePresentDescriptor presentDesc{};
        presentDesc.Target = swapchain.get();
        presentDesc.WaitSemaphores = waitSemaphores;
        cmdQueue->Present(presentDesc);
    }
    return !glfw->ShouldClose();
}

int main() {
    Init();
    while (Update()) {
        std::this_thread::yield();
    }
    End();
    return 0;
}
