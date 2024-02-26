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
        _mouseButtonCb = std::make_shared<MultiDelegate<MouseButtonCallback>>();
        _cursorPositionCb = std::make_shared<MultiDelegate<CursorPositionCallback>>();
        _keyCb = std::make_shared<MultiDelegate<KeyCallback>>();
        _scrollCb = std::make_shared<MultiDelegate<ScrollCallback>>();
        _windowResizeCb = std::make_shared<MultiDelegate<WindowResizeCallback>>();
        glfwSetWindowUserPointer(window, this);
        glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mods) {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            auto x = 0.0;
            auto y = 0.0;
            glfwGetCursorPos(self->window, &x, &y);
            Eigen::Vector2f xy{static_cast<float>(x), static_cast<float>(y)};
            self->_mouseButtonCb->Invoke(xy, static_cast<MouseButton>(button), static_cast<Action>(action), static_cast<KeyModifiers>(mods));
        });
        glfwSetCursorPosCallback(window, [](GLFWwindow* window, double x, double y) noexcept {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            self->_cursorPositionCb->Invoke(Eigen::Vector2f{static_cast<float>(x), static_cast<float>(y)});
        });
        glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            self->_keyCb->Invoke(static_cast<Key>(key), static_cast<Action>(action), static_cast<KeyModifiers>(mods));
        });
        glfwSetScrollCallback(window, [](GLFWwindow* window, double dx, double dy) {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            self->_scrollCb->Invoke(Eigen::Vector2f{static_cast<float>(dx), static_cast<float>(dy)});
        });
        glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) noexcept {
            auto self = static_cast<NativeWindowImpl*>(glfwGetWindowUserPointer(window));
            self->_windowResizeCb->Invoke(Eigen::Vector2i{width, height});
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
    std::shared_ptr<MultiDelegate<MouseButtonCallback>> _mouseButtonCb;
    std::shared_ptr<MultiDelegate<CursorPositionCallback>> _cursorPositionCb;
    std::shared_ptr<MultiDelegate<KeyCallback>> _keyCb;
    std::shared_ptr<MultiDelegate<ScrollCallback>> _scrollCb;
    std::shared_ptr<MultiDelegate<WindowResizeCallback>> _windowResizeCb;
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

std::shared_ptr<MultiDelegate<MouseButtonCallback>> NativeWindow::EventMouseButtonCall() const noexcept {
    return static_cast<NativeWindowImpl*>(_impl.get())->_mouseButtonCb;
}

std::shared_ptr<MultiDelegate<CursorPositionCallback>> NativeWindow::EventCursorPosition() const noexcept {
    return static_cast<NativeWindowImpl*>(_impl.get())->_cursorPositionCb;
}

std::shared_ptr<MultiDelegate<KeyCallback>> NativeWindow::EventKey() const noexcept {
    return static_cast<NativeWindowImpl*>(_impl.get())->_keyCb;
}

std::shared_ptr<MultiDelegate<ScrollCallback>> NativeWindow::EventScroll() const noexcept {
    return static_cast<NativeWindowImpl*>(_impl.get())->_scrollCb;
}

std::shared_ptr<MultiDelegate<WindowResizeCallback>> NativeWindow::EventWindwResize() const noexcept {
    return static_cast<NativeWindowImpl*>(_impl.get())->_windowResizeCb;
}

}  // namespace radray::window
