#include <exception>

#include <radray/window/glfw_window.h>
#include <radray/rhi/device.h>

using namespace radray;

int main() {
    window::GlobalInit();
    try {
        window::GlfwWindow glfw{"Test RHI", 1280, 720};
        std::shared_ptr<rhi::IDevice> device = rhi::CreateDeviceD3D12({.IsEnableDebugLayer = true});
        if (device == nullptr) {
            throw std::runtime_error("cannot create graphics device");
        }
        std::shared_ptr<rhi::ICommandQueue> queue = device->CreateCommandQueue({rhi::CommandListType::Graphics});
        while (!glfw.ShouldClose()) {
            window::GlobalPollEvents();
        }
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("exception {}", e.what());
    }
    window::GlobalTerminate();
    return 0;
}
