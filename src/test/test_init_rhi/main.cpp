#include <stdexcept>
#include <thread>
#include <chrono>

#include <radray/window/glfw_window.h>
#include <radray/basic_math.h>
#include <radray/rhi/device_interface.h>

using namespace radray;

constexpr int FRAME_COUNT = 3;

std::shared_ptr<window::GlfwWindow> glfw;
std::shared_ptr<rhi::DeviceInterface> device;
RadrayCommandQueue queue;

void start() {
    window::GlobalInitGlfw();
    glfw = std::make_shared<window::GlfwWindow>("init rhi", 1280, 720);
#ifdef RADRAY_ENABLE_D3D12
    RadrayDeviceDescriptorD3D12 desc{
        .IsEnableDebugLayer = true};
    device = rhi::CreateDeviceD3D12(&desc);
#endif
    if (device == nullptr) {
        throw std::runtime_error{"cannot create device"};
    }
    queue = device->CreateCommandQueue(RADRAY_QUEUE_TYPE_DIRECT);
}

void update() {
    while (!glfw->ShouldClose()) {
        window::GlobalPollEventsGlfw();
        std::this_thread::yield();
    }
}

void destroy() {
    device->DestroyCommandQueue(queue);
    device = nullptr;
    window::GlobalTerminateGlfw();
}

int main() {
    try {
        start();
        update();
        destroy();
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("{}", e.what());
    } catch (...) {
        RADRAY_ERR_LOG("crital error");
    }
    return 0;
}
