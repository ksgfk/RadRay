#include <radray/logger.h>
#include <radray/window/native_window.h>

int main() {
    radray::unique_ptr<radray::NativeWindow> window;
#ifdef RADRAY_PLATFORM_WINDOWS
    radray::Win32WindowCreateDescriptor desc{
        "Test Native Window Win32",
        1280,
        720,
        -1,
        -1,
        true,
        false,
        false};
    window = radray::CreateNativeWindow(desc).Unwrap();
    sigslot::scoped_connection conn = window->EventResized().connect([](int w, int h) {
        RADRAY_INFO_LOG("Window resized: {}x{}", w, h);
    });
#endif
    if (!window) {
        RADRAY_ERR_LOG("Failed to create native window");
        return -1;
    }
    while (true) {
        window->DispatchEvents();
        if (window->ShouldClose()) {
            break;
        }
    }
    conn.disconnect();
    return 0;
}
