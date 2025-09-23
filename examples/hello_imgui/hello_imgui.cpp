#include <stdexcept>

#include <radray/types.h>
#include <radray/logger.h>
#include <radray/window/native_window.h>
#include <radray/imgui/dear_imgui.h>

const char* RADRAY_APP_NAME = "Hello Dear ImGui";

using namespace radray;

class HelloImguiException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    template <typename... Args>
    explicit HelloImguiException(fmt::format_string<Args...> fmt, Args&&... args)
        : _msg(radray::format(fmt, std::forward<Args>(args)...)) {}
    ~HelloImguiException() noexcept override = default;

    const char* what() const noexcept override { return _msg.empty() ? "Unknown error" : _msg.c_str(); }

private:
    string _msg;
};

class HelloImguiApp {
public:
    unique_ptr<NativeWindow> _window;
};

void CreateApp() {
    unique_ptr<NativeWindow> window;
#ifdef RADRAY_PLATFORM_WINDOWS
    Win32WindowCreateDescriptor desc{};
    desc.Title = RADRAY_APP_NAME;
    desc.Width = 1280;
    desc.Height = 720;
    desc.X = -1;
    desc.Y = -1;
    desc.Resizable = false;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    window = CreateNativeWindow(desc).Unwrap();
#endif
    if (window) {
        throw HelloImguiException("Failed to create native window");
    }
}

int main() {
    GlobalInitDearImGui();
    CreateApp();
    GlobalTerminateDearImGui();
    FlushLog();
    return 0;
}
