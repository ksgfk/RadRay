#pragma once

#include <memory>
#include <string>

#include <radray/types.h>
#include <radray/basic_math.h>
#include <radray/multi_delegate.h>
#include <radray/window/input.h>

namespace radray::window {

void GlobalInitGlfw() noexcept;
void GlobalPollEventsGlfw() noexcept;
void GlobalTerminateGlfw() noexcept;

using MouseButtonCallback = void(const Eigen::Vector2f& xy, MouseButton button, Action action, KeyModifiers modifiers);
using CursorPositionCallback = void(const Eigen::Vector2f& xy);
using KeyCallback = void(Key key, Action action, KeyModifiers modifiers);
using ScrollCallback = void(const Eigen::Vector2f& dxdy);
using WindowResizeCallback = void(const Eigen::Vector2i& size);
using FrameResizeCallback = void(const Eigen::Vector2i& size);
using WindowRefreshCallback = void();

class GlfwWindow {
public:
    class Impl {
    public:
        virtual ~Impl() noexcept = default;
    };

    GlfwWindow(std::string name, uint32_t width, uint32_t height, bool resizable = false, bool fullScreen = false) noexcept;
    ~GlfwWindow() noexcept;
    GlfwWindow(const GlfwWindow&) = delete;
    GlfwWindow(GlfwWindow&&) = default;
    GlfwWindow& operator=(GlfwWindow&&) noexcept = default;
    GlfwWindow& operator=(const GlfwWindow&) noexcept = delete;

    bool IsValid() const noexcept;
    bool ShouldClose() const noexcept;
    size_t GetNativeHandle() const noexcept;
    Eigen::Vector2i GetSize() const noexcept;

    void Destroy() noexcept;

    std::shared_ptr<MultiDelegate<MouseButtonCallback>> EventMouseButtonCall() const noexcept;
    std::shared_ptr<MultiDelegate<CursorPositionCallback>> EventCursorPosition() const noexcept;
    std::shared_ptr<MultiDelegate<KeyCallback>> EventKey() const noexcept;
    std::shared_ptr<MultiDelegate<ScrollCallback>> EventScroll() const noexcept;
    std::shared_ptr<MultiDelegate<WindowResizeCallback>> EventWindwResize() const noexcept;
    std::shared_ptr<MultiDelegate<FrameResizeCallback>> EventFrameResize() const noexcept;
    std::shared_ptr<MultiDelegate<WindowRefreshCallback>> EventWindowRefresh() const noexcept;

private:
    std::unique_ptr<Impl> _impl;
};

}  // namespace radray::window
