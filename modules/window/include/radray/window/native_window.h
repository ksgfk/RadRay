#pragma once

#include <variant>
#include <string_view>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/multi_delegate.h>

namespace radray {

enum class WindowHandlerTag {
    UNKNOWN,
    HWND
};

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
};

using NativeWindowCreateDescriptor = std::variant<Win32WindowCreateDescriptor>;

using NativeWindowBeginResizeDelegate = void(int width, int height);
using NativeWindowResizingDelegate = void(int width, int height);
using NativeWindowEndResizeDelegate = void(int width, int height);

class NativeWindow {
public:
    virtual ~NativeWindow() noexcept = default;

    virtual bool IsValid() const noexcept = 0;
    virtual void Destroy() noexcept = 0;

    virtual void DispatchEvents() noexcept = 0;

    virtual bool ShouldClose() const noexcept = 0;
    virtual WindowNativeHandler GetNativeHandler() const noexcept = 0;

    virtual shared_ptr<MultiDelegate<NativeWindowBeginResizeDelegate>> EventBeginResize() const noexcept = 0;
    virtual shared_ptr<MultiDelegate<NativeWindowResizingDelegate>> EventResizing() const noexcept = 0;
    virtual shared_ptr<MultiDelegate<NativeWindowEndResizeDelegate>> EventEndResize() const noexcept = 0;
};

Nullable<unique_ptr<NativeWindow>> CreateNativeWindow(const NativeWindowCreateDescriptor& desc) noexcept;

}  // namespace radray
