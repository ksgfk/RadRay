#include <radray/window/glfw_window.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <windows.h>
#endif

#define GLFW_NATIVE_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <radray/logger.h>
#include <radray/utility.h>

#if defined(RADRAY_PLATFORM_MACOS)
#include "cocoa_handle.h"
#endif

namespace radray::window {

void GlobalInitGlfw() noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    GLFWallocator alloc{
        .allocate = [](size_t size, void* user) { 
            RADRAY_UNUSED(user);
            return mi_malloc(size); },
        .reallocate = [](void* block, size_t size, void* user) {
            RADRAY_UNUSED(user);
            return mi_realloc(block, size); },
        .deallocate = [](void* block, void* user) { 
            RADRAY_UNUSED(user);
            mi_free(block); },
        .user = nullptr};
    glfwInitAllocator(&alloc);
#endif
    glfwSetErrorCallback([](int error_code, const char* description) {
        RADRAY_ERR_LOG("glfw error: {} (code = {})", description, error_code);
    });
    glfwInit();
}

void GlobalPollEventsGlfw() noexcept {
    glfwPollEvents();
}

void GlobalTerminateGlfw() noexcept {
    glfwTerminate();
}

class GlfwWindowImpl : public GlfwWindow::Impl {
public:
    GlfwWindowImpl(
        const radray::string& name,
        uint32_t width,
        uint32_t height,
        bool resizable,
        bool fullScreen) noexcept
        : mouseButtonCb(radray::make_shared<MultiDelegate<MouseButtonCallback>>()),
          cursorPositionCb(radray::make_shared<MultiDelegate<CursorPositionCallback>>()),
          keyCb(radray::make_shared<MultiDelegate<KeyCallback>>()),
          scrollCb(radray::make_shared<MultiDelegate<ScrollCallback>>()),
          windowResizeCb(radray::make_shared<MultiDelegate<WindowResizeCallback>>()),
          frameResizeCb(radray::make_shared<MultiDelegate<FrameResizeCallback>>()),
          windowRefreshCb(radray::make_shared<MultiDelegate<WindowRefreshCallback>>()) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, resizable);
        window = glfwCreateWindow(width, height, name.c_str(), fullScreen ? glfwGetPrimaryMonitor() : nullptr, nullptr);
        if (window == nullptr) {
            RADRAY_ABORT("create glfw window fail");
        }
#if defined(RADRAY_PLATFORM_WINDOWS)
        nativeHandle = reinterpret_cast<size_t>(glfwGetWin32Window(window));
#endif
#if defined(RADRAY_PLATFORM_MACOS)
        nativeHandle = RadrayGetCocoaHandlerFromGlfw(window);
#endif
        glfwSetWindowUserPointer(window, this);
        glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mods) {
            auto self = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(window));
            auto x = 0.0;
            auto y = 0.0;
            glfwGetCursorPos(self->window, &x, &y);
            Eigen::Vector2f xy{static_cast<float>(x), static_cast<float>(y)};
            self->mouseButtonCb->Invoke(xy, static_cast<MouseButton>(button), static_cast<Action>(action), static_cast<KeyModifiers>(mods));
        });
        glfwSetCursorPosCallback(window, [](GLFWwindow* window, double x, double y) noexcept {
            auto self = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(window));
            self->cursorPositionCb->Invoke(Eigen::Vector2f{static_cast<float>(x), static_cast<float>(y)});
        });
        glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            auto self = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(window));
            self->keyCb->Invoke(static_cast<Key>(key), static_cast<Action>(action), static_cast<KeyModifiers>(mods));
            RADRAY_UNUSED(scancode);
        });
        glfwSetScrollCallback(window, [](GLFWwindow* window, double dx, double dy) {
            auto self = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(window));
            self->scrollCb->Invoke(Eigen::Vector2f{static_cast<float>(dx), static_cast<float>(dy)});
        });
        glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) noexcept {
            auto self = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(window));
            self->windowResizeCb->Invoke(Eigen::Vector2i{width, height});
        });
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow* window, int width, int height) {
            auto self = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(window));
            self->frameResizeCb->Invoke(Eigen::Vector2i{width, height});
        });
        glfwSetWindowRefreshCallback(window, [](GLFWwindow* window) {
            auto self = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(window));
            self->windowRefreshCb->Invoke();
        });
    }
    ~GlfwWindowImpl() noexcept override {
#if defined(RADRAY_PLATFORM_MACOS)
        if (nativeHandle != nullptr) {
            RadrayReleaseCocoaHandlerFromGlfw(window, nativeHandle);
            nativeHandle = nullptr;
        }
#endif
        if (window != nullptr) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
    }

    GLFWwindow* window{nullptr};
    const void* nativeHandle{nullptr};
    radray::shared_ptr<MultiDelegate<MouseButtonCallback>> mouseButtonCb;
    radray::shared_ptr<MultiDelegate<CursorPositionCallback>> cursorPositionCb;
    radray::shared_ptr<MultiDelegate<KeyCallback>> keyCb;
    radray::shared_ptr<MultiDelegate<ScrollCallback>> scrollCb;
    radray::shared_ptr<MultiDelegate<WindowResizeCallback>> windowResizeCb;
    radray::shared_ptr<MultiDelegate<FrameResizeCallback>> frameResizeCb;
    radray::shared_ptr<MultiDelegate<WindowRefreshCallback>> windowRefreshCb;
};

GlfwWindow::GlfwWindow(radray::string name, uint32_t width, uint32_t height, bool resizable, bool fullScreen) noexcept
    : _impl(radray::make_unique<GlfwWindowImpl>(name, width, height, resizable, fullScreen)) {}

GlfwWindow::~GlfwWindow() noexcept {
    Destroy();
}

bool GlfwWindow::IsValid() const noexcept { return _impl != nullptr; }

bool GlfwWindow::ShouldClose() const noexcept {
    return glfwWindowShouldClose(static_cast<GlfwWindowImpl*>(_impl.get())->window);
}

const void* GlfwWindow::GetNativeHandle() const noexcept {
    return static_cast<GlfwWindowImpl*>(_impl.get())->nativeHandle;
}

Eigen::Vector2i GlfwWindow::GetSize() const noexcept {
    int w, h;
    glfwGetWindowSize(static_cast<GlfwWindowImpl*>(_impl.get())->window, &w, &h);
    return Eigen::Vector2i{w, h};
}

void GlfwWindow::Destroy() noexcept { _impl.reset(); }

radray::shared_ptr<MultiDelegate<MouseButtonCallback>> GlfwWindow::EventMouseButtonCall() const noexcept { return static_cast<GlfwWindowImpl*>(_impl.get())->mouseButtonCb; }

radray::shared_ptr<MultiDelegate<CursorPositionCallback>> GlfwWindow::EventCursorPosition() const noexcept { return static_cast<GlfwWindowImpl*>(_impl.get())->cursorPositionCb; }

radray::shared_ptr<MultiDelegate<KeyCallback>> GlfwWindow::EventKey() const noexcept { return static_cast<GlfwWindowImpl*>(_impl.get())->keyCb; }

radray::shared_ptr<MultiDelegate<ScrollCallback>> GlfwWindow::EventScroll() const noexcept { return static_cast<GlfwWindowImpl*>(_impl.get())->scrollCb; }

radray::shared_ptr<MultiDelegate<WindowResizeCallback>> GlfwWindow::EventWindwResize() const noexcept { return static_cast<GlfwWindowImpl*>(_impl.get())->windowResizeCb; }

radray::shared_ptr<MultiDelegate<FrameResizeCallback>> GlfwWindow::EventFrameResize() const noexcept { return static_cast<GlfwWindowImpl*>(_impl.get())->frameResizeCb; }

radray::shared_ptr<MultiDelegate<WindowRefreshCallback>> GlfwWindow::EventWindowRefresh() const noexcept { return static_cast<GlfwWindowImpl*>(_impl.get())->windowRefreshCb; }

}  // namespace radray::window
