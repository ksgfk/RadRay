#include <exception>

#include <radray/window/glfw_window.h>

using namespace radray;

int main() {
    window::GlobalInit();
    try {
        window::GlfwWindow glfw{"Test RHI", 1280, 720};
        while (!glfw.ShouldClose()) {
            window::GlobalPollEvents();
        }
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("exception {}", e.what());
    }
    window::GlobalTerminate();
    return 0;
}
