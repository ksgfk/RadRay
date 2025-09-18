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
    return 0;
}
