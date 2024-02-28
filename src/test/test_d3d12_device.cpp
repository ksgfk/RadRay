#include <radray/d3d12/device.h>
#include <radray/window/native_window.h>

using namespace radray;

int main() {
    window::GlobalInit();
    window::NativeWindow window{"test d3d12", 1280, 720};
    d3d12::Device d{};
    while (!window.ShouldClose()) {
        window::GlobalPollEvents();
    }
    window.Destroy();
    window::GlobalTerminate();
    return 0;
}
