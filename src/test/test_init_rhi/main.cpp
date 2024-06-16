#include <exception>

#include <radray/window/glfw_window.h>
#include <radray/rhi/device_interface.h>

using namespace radray;

int main() {
    window::GlobalInitGlfw();
    try {
        window::GlfwWindow glfw{"Test RHI", 1280, 720};
        auto device = rhi::CreateDeviceD3D12({std::nullopt, true});
        while (!glfw.ShouldClose()) {
            window::GlobalPollEventsGlfw();
        }
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("exception {}", e.what());
    }
    window::GlobalTerminateGlfw();
    return 0;
}
