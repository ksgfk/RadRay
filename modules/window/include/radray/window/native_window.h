#pragma once

#include <variant>
#include <string_view>
#include <functional>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/utility.h>
#include <radray/platform.h>
#include <radray/window/input.h>

namespace radray {

enum class WindowHandlerTag {
    UNKNOWN,
    HWND
};

using Win32WNDPROC = int64_t(HWND hwnd, uint32_t uMsg, uint64_t wParam, int64_t lParam);

struct WindowNativeHandler {
    WindowHandlerTag Type;
    void* Handle;
};

struct Win32WindowCreateDescriptor {
    std::string_view Title;
    int Width;
    int Height;
    int X;
    int Y;
    bool Resizable;
    bool StartMaximized;
    bool Fullscreen;
    std::span<std::weak_ptr<std::function<Win32WNDPROC>>> ExtraWndProcs;
};

using NativeWindowCreateDescriptor = std::variant<Win32WindowCreateDescriptor>;

struct WindowVec2i {
    int X;
    int Y;
};

class NativeWindow {
public:
    virtual ~NativeWindow() noexcept = default;

    virtual bool IsValid() const noexcept = 0;
    virtual void Destroy() noexcept = 0;

    virtual void DispatchEvents() noexcept = 0;

    virtual bool ShouldClose() const noexcept = 0;
    virtual WindowNativeHandler GetNativeHandler() const noexcept = 0;
    virtual WindowVec2i GetSize() const noexcept = 0;
    virtual bool IsMinimized() const noexcept = 0;

    virtual void SetSize(int width, int height) noexcept = 0;

    virtual sigslot::signal<int, int>& EventResized() noexcept = 0;
    virtual sigslot::signal<int, int>& EventResizing() noexcept = 0;
    virtual sigslot::signal<int, int, MouseButton, Action>& EventTouch() noexcept = 0;
    virtual sigslot::signal<KeyCode, Action>& EventKeyboard() noexcept = 0;
    virtual sigslot::signal<int>& EventMouseWheel() noexcept = 0;
};

Nullable<unique_ptr<NativeWindow>> CreateNativeWindow(const NativeWindowCreateDescriptor& desc) noexcept;

}  // namespace radray
