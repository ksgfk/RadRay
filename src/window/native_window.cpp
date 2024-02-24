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
        glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mods) {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            auto x = 0.0;
            auto y = 0.0;
            glfwGetCursorPos(self->window, &x, &y);
            Eigen::Vector2f xy{static_cast<float>(x), static_cast<float>(y)};
            self->_mouseButtonCb(xy, static_cast<MouseButton>(button), static_cast<Action>(action), static_cast<KeyModifiers>(mods));
        });
        glfwSetCursorPosCallback(window, [](GLFWwindow* window, double x, double y) noexcept {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            self->_cursorPositionCb(Eigen::Vector2f{static_cast<float>(x), static_cast<float>(y)});
        });
        glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            self->_keyCb(static_cast<Key>(key), static_cast<Action>(action), static_cast<KeyModifiers>(mods));
        });
        glfwSetScrollCallback(window, [](GLFWwindow* window, double dx, double dy) {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            self->_scrollCb(Eigen::Vector2f{static_cast<float>(dx), static_cast<float>(dy)});
        });
        glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) noexcept {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            self->_windowResizeCb(Eigen::Vector2i{width, height});
        });
    }
    ~NativeWindowImpl() noexcept override {
        if (window != nullptr) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
    }

    GLFWwindow* window{nullptr};
    size_t nativeHandle{0};
    MultiDelegate<MouseButtonCallback> _mouseButtonCb;
    MultiDelegate<CursorPositionCallback> _cursorPositionCb;
    MultiDelegate<KeyCallback> _keyCb;
    MultiDelegate<ScrollCallback> _scrollCb;
    MultiDelegate<WindowResizeCallback> _windowResizeCb;
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

void NativeWindow::AddMouseButtonCallback(std::weak_ptr<Delegate<MouseButtonCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_mouseButtonCb.Add(callback);
}
void NativeWindow::AddCursorPositionCallback(std::weak_ptr<Delegate<CursorPositionCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_cursorPositionCb.Add(callback);
}
void NativeWindow::AddKeyCallback(std::weak_ptr<Delegate<KeyCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_keyCb.Add(callback);
}
void NativeWindow::AddScrollCallback(std::weak_ptr<Delegate<ScrollCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_scrollCb.Add(callback);
}
void NativeWindow::AddWindowResizeCallback(std::weak_ptr<Delegate<WindowResizeCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_windowResizeCb.Add(callback);
}
void NativeWindow::RemoveMouseButtonCallback(std::weak_ptr<Delegate<MouseButtonCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_mouseButtonCb.Remove(callback);
}
void NativeWindow::RemoveCursorPositionCallback(std::weak_ptr<Delegate<CursorPositionCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_cursorPositionCb.Remove(callback);
}
void NativeWindow::RemoveKeyCallback(std::weak_ptr<Delegate<KeyCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_keyCb.Remove(callback);
}
void NativeWindow::RemoveScrollCallback(std::weak_ptr<Delegate<ScrollCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_scrollCb.Remove(callback);
}
void NativeWindow::RemoveWindowResizeCallback(std::weak_ptr<Delegate<WindowResizeCallback>> callback) noexcept {
    static_cast<NativeWindowImpl*>(_impl.get())->_windowResizeCb.Remove(callback);
}

}  // namespace radray::window
