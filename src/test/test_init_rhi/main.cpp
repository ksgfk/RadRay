#include <radray/window/glfw_window.h>
#include <radray/rhi/device.h>

using namespace radray;

int main() {
    window::GlobalInit();
    {
        window::GlfwWindow glfw{"Test RHI", 1280, 720};
        std::shared_ptr<rhi::IDevice> device = rhi::CreateDeviceD3D12({.IsEnableDebugLayer = true});
        while (!glfw.ShouldClose()) {
            window::GlobalPollEvents();
        }
    }
    window::GlobalTerminate();
    return 0;
}
