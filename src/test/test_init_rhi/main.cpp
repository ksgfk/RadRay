#include <stdexcept>
#include <thread>
#include <chrono>

#include <radray/window/glfw_window.h>
#include <radray/basic_math.h>
#include <radray/rhi/device_interface.h>

constexpr int FRAME_COUNT = 3;

radray::shared_ptr<radray::window::GlfwWindow> glfw;
radray::rhi::DeviceInterface* device;
RadrayCommandQueue cmdQueue;
RadraySwapChain swapchain;

void start() {
    radray::window::GlobalInitGlfw();
    glfw = radray::make_shared<radray::window::GlfwWindow>("init rhi", 1280, 720);
#ifdef RADRAY_ENABLE_D3D12
    RadrayDeviceDescriptorD3D12 desc{
        .AdapterIndex = RADRAY_RHI_AUTO_SELECT_DEVICE,
        .IsEnableDebugLayer = true};
    device = reinterpret_cast<radray::rhi::DeviceInterface*>(RadrayCreateDeviceD3D12(&desc));
#endif
#ifdef RADRAY_ENABLE_METAL
    RadrayDeviceDescriptorMetal desc{
        .DeviceIndex = RADRAY_RHI_AUTO_SELECT_DEVICE};
    device = reinterpret_cast<radray::rhi::DeviceInterface*>(RadrayCreateDeviceMetal(&desc));
#endif
    if (device == nullptr) {
        throw std::runtime_error{"cannot create device"};
    }
    cmdQueue = device->CreateCommandQueue(RADRAY_QUEUE_TYPE_DIRECT);
    RadraySwapChainDescriptor chainDesc{
        cmdQueue,
        glfw->GetNativeHandle(),
        static_cast<uint32_t>(glfw->GetSize().x()),
        static_cast<uint32_t>(glfw->GetSize().y()),
        FRAME_COUNT,
        RADRAY_FORMAT_RGBA8_UNORM,
        true};
    swapchain = device->CreateSwapChain(chainDesc);
}

void update() {
    while (!glfw->ShouldClose()) {
        radray::window::GlobalPollEventsGlfw();
        std::this_thread::yield();
    }
}

void destroy() {
    device->DestroySwapChian(swapchain);
    device->DestroyCommandQueue(cmdQueue);
    RadrayReleaseDevice(device);
    radray::window::GlobalTerminateGlfw();
}

int main() {
    try {
        start();
        update();
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("{}", e.what());
    } catch (...) {
        RADRAY_ERR_LOG("crital error");
    }
    destroy();
    return 0;
}
