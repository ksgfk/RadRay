#include <stdexcept>

#include <radray/window/glfw_window.h>
#include <radray/rhi/device_interface.h>

using namespace radray;

int main() {
    window::GlobalInitGlfw();
    try {
        window::GlfwWindow glfw{"Test RHI", 1280, 720};
#ifdef RADRAY_ENABLE_D3D12
        auto device = rhi::CreateDeviceD3D12({std::nullopt, true});
#endif
#ifdef RADRAY_ENABLE_METAL
        auto device = rhi::CreateDeviceMetal({});
#endif
        if (device == nullptr) {
            throw std::runtime_error{"cannot create device"};
        }
        while (!glfw.ShouldClose()) {
            window::GlobalPollEventsGlfw();
        }
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("exception {}", e.what());
    }
    window::GlobalTerminateGlfw();
    return 0;
}
