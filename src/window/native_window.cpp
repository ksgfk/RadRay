#include <radray/window/native_window.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#define GLFW_EXPOSE_NATIVE_WIN32
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#endif

#define GLFW_NATIVE_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <radray/logger.h>

namespace radray::window {

void GlobalInit() noexcept {
    glfwSetErrorCallback([](int error_code, const char* description) {
        RADRAY_LOG_ERROR("glfw error: {} (code = {})", description, error_code);
    });
    glfwInit();
}

void GlobalPollEvents() noexcept {
    glfwPollEvents();
}

void GlobalTerminate() noexcept {
    glfwTerminate();
}

class NativeWindowImpl : public NativeWindow::Impl {
public:
    NativeWindowImpl(const std::string& name, uint32 width, uint32 height, bool resizable, bool fullScreen) noexcept {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, resizable);
        window = glfwCreateWindow(width, height, name.c_str(), fullScreen ? glfwGetPrimaryMonitor() : nullptr, nullptr);
        if (window == nullptr) {
            RADRAY_ABORT("create glfw window fail");
        }
#if defined(RADRAY_PLATFORM_WINDOWS)
        nativeHandle = reinterpret_cast<size_t>(glfwGetWin32Window(window));
#endif
        glfwSetWindowUserPointer(window, this);
    }
    ~NativeWindowImpl() noexcept override {
        if (window != nullptr) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
    }

    GLFWwindow* window{nullptr};
    size_t nativeHandle{0};
};

NativeWindow::NativeWindow(std::string name, uint32 width, uint32 height, bool resizable, bool fullScreen) noexcept {
    _impl = std::make_unique<NativeWindowImpl>(name, width, height, resizable, fullScreen);
}

NativeWindow::~NativeWindow() noexcept = default;

bool NativeWindow::IsValid() const noexcept { return _impl != nullptr; }

bool NativeWindow::ShouldClose() const noexcept {
    return glfwWindowShouldClose(static_cast<NativeWindowImpl*>(_impl.get())->window);
}

void NativeWindow::Destroy() noexcept {
    _impl = nullptr;
}

}  // namespace radray::window
