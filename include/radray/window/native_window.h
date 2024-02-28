#pragma once

#include <memory>
#include <string>
#include <radray/types.h>
#include <radray/basic_math.h>
#include <radray/multi_delegate.h>
#include <radray/window/input.h>

namespace radray::window {

void GlobalInit() noexcept;
void GlobalPollEvents() noexcept;
void GlobalTerminate() noexcept;

using MouseButtonCallback = void(const Eigen::Vector2f& xy, MouseButton button, Action action, KeyModifiers modifiers);
using CursorPositionCallback = void(const Eigen::Vector2f& xy);
using KeyCallback = void(Key key, Action action, KeyModifiers modifiers);
using ScrollCallback = void(const Eigen::Vector2f& dxdy);
using WindowResizeCallback = void(const Eigen::Vector2i& size);

class NativeWindow {
public:
    class Impl {
    public:
        virtual ~Impl() noexcept = default;
    };

    NativeWindow(std::string name, uint32 width, uint32 height, bool resizable = false, bool fullScreen = false) noexcept;
    ~NativeWindow() noexcept;
    NativeWindow(const NativeWindow&) = delete;
    NativeWindow(NativeWindow&&) = default;
    NativeWindow& operator=(NativeWindow&&) noexcept = default;
    NativeWindow& operator=(const NativeWindow&) noexcept = delete;

    bool IsValid() const noexcept;
    bool ShouldClose() const noexcept;
    size_t GetNativeHandle() const noexcept;

    void Destroy() noexcept;

    std::shared_ptr<MultiDelegate<MouseButtonCallback>> EventMouseButtonCall() const noexcept;
    std::shared_ptr<MultiDelegate<CursorPositionCallback>> EventCursorPosition() const noexcept;
    std::shared_ptr<MultiDelegate<KeyCallback>> EventKey() const noexcept;
    std::shared_ptr<MultiDelegate<ScrollCallback>> EventScroll() const noexcept;
    std::shared_ptr<MultiDelegate<WindowResizeCallback>> EventWindwResize() const noexcept;

private:
    std::unique_ptr<Impl> _impl;
};

}  // namespace radray::window
