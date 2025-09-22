#pragma once

#include <variant>
#include <string_view>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/utility.h>

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

class NativeWindow {
public:
    virtual ~NativeWindow() noexcept = default;

    virtual bool IsValid() const noexcept = 0;
    virtual void Destroy() noexcept = 0;

    virtual void DispatchEvents() noexcept = 0;

    virtual bool ShouldClose() const noexcept = 0;
    virtual WindowNativeHandler GetNativeHandler() const noexcept = 0;

    virtual sigslot::signal<int, int>& EventResized() noexcept = 0;
    virtual sigslot::signal<int, int>& EventResizing() noexcept = 0;
};

Nullable<unique_ptr<NativeWindow>> CreateNativeWindow(const NativeWindowCreateDescriptor& desc) noexcept;

}  // namespace radray
