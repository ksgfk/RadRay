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
    void Destroy() noexcept;

    void AddMouseButtonCallback(std::weak_ptr<Delegate<MouseButtonCallback>> callback) noexcept;
    void AddCursorPositionCallback(std::weak_ptr<Delegate<CursorPositionCallback>> callback) noexcept;
    void AddKeyCallback(std::weak_ptr<Delegate<KeyCallback>> callback) noexcept;
    void AddScrollCallback(std::weak_ptr<Delegate<ScrollCallback>> callback) noexcept;
    void AddWindowResizeCallback(std::weak_ptr<Delegate<WindowResizeCallback>> callback) noexcept;
    void RemoveMouseButtonCallback(std::weak_ptr<Delegate<MouseButtonCallback>> callback) noexcept;
    void RemoveCursorPositionCallback(std::weak_ptr<Delegate<CursorPositionCallback>> callback) noexcept;
    void RemoveKeyCallback(std::weak_ptr<Delegate<KeyCallback>> callback) noexcept;
    void RemoveScrollCallback(std::weak_ptr<Delegate<ScrollCallback>> callback) noexcept;
    void RemoveWindowResizeCallback(std::weak_ptr<Delegate<WindowResizeCallback>> callback) noexcept;

private:
    std::unique_ptr<Impl> _impl;
};

}  // namespace radray::window
