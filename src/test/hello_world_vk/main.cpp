#include <thread>

#include <radray/logger.h>

#include <radray/render/dxc.h>
#include <radray/render/device.h>
#include <radray/render/fence.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_buffer.h>
#include <radray/render/shader.h>

#include <radray/window/glfw_window.h>

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;

unique_ptr<GlfwWindow> glfw;
shared_ptr<Dxc> dxc;
shared_ptr<Device> device;
shared_ptr<CommandBuffer> cmdBuffer;
shared_ptr<SwapChain> swapchain;

void InitGraphics() {
    VulkanBackendInitDdescriptor vkInitDesc{};
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
    cmdBuffer = cmdQueue->CreateCommandBuffer().Unwrap();
    swapchain = device->CreateSwapChain(cmdQueue, glfw->GetNativeHandle(), WIN_WIDTH, WIN_HEIGHT, 2, TextureFormat::RGBA8_UNORM, true).Unwrap();
}

void Init() {
    GlobalInitGlfw();
    glfw = make_unique<GlfwWindow>(RADRAY_APPNAME, WIN_WIDTH, WIN_HEIGHT, false, false);

    dxc = CreateDxc().Unwrap();

    InitGraphics();
    InitDevice();
}

void End() {
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

    // swapchain->AcquireNextRenderTarget();
    // cmdBuffer->Begin();
    // ColorAttachment colorAttachment[] = {
    //     DefaultColorAttachment(nullptr)};
    // RenderPassDesc renderPassDesc{
    //     "Clear",
    //     colorAttachment,
    //     std::nullopt};
    // auto cmdEncoder = cmdBuffer->BeginRenderPass(renderPassDesc).Unwrap();
    // cmdBuffer->EndRenderPass(std::move(cmdEncoder));
    // cmdBuffer->End();
    // auto cmdQueue = device->GetCommandQueue(QueueType::Direct).Unwrap();
    // CommandBuffer* submits[] = {cmdBuffer.get()};
    // cmdQueue->Submit(submits, nullptr);
    // swapchain->Present();
    // cmdQueue->Wait();

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
