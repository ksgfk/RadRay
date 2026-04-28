#pragma once

#include <variant>
#include <string_view>
#include <functional>
#include <limits>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/utility.h>
#include <radray/window/input.h>

namespace radray {

enum class WindowHandlerTag {
    UNKNOWN,
    HWND,
    NS_VIEW
};

struct WindowNativeHandler {
    WindowHandlerTag Type{WindowHandlerTag::UNKNOWN};
    void* Handle{nullptr};
};

using Win32MsgProc = int64_t(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam);

struct Win32MsgProcHandle {
    uint64_t Id{std::numeric_limits<uint64_t>::max()};

    constexpr bool IsValid() const noexcept { return Id != std::numeric_limits<uint64_t>::max(); }
    constexpr static Win32MsgProcHandle Invalid() noexcept { return {}; }
};

struct Win32WindowCreateDescriptor {
    std::string_view Title{};
    int32_t Width{0};
    int32_t Height{0};
    int32_t X{0};
    int32_t Y{0};
    bool Resizable{false};
    bool StartMaximized{false};
    bool Fullscreen{false};
    bool StartVisible{true};
    std::span<std::function<Win32MsgProc>> ExtraWndProcs{};
};

struct CocoaWindowCreateDescriptor {
    std::string_view Title{};
    int32_t Width{0};
    int32_t Height{0};
    int32_t X{0};
    int32_t Y{0};
    bool Resizable{false};
    bool StartMaximized{false};
    bool Fullscreen{false};
    bool StartVisible{true};
};

using NativeWindowCreateDescriptor = std::variant<Win32WindowCreateDescriptor, CocoaWindowCreateDescriptor>;

struct WindowVec2i {
    int32_t X;
    int32_t Y;
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
    virtual WindowVec2i GetPosition() const noexcept = 0;
    virtual bool IsMinimized() const noexcept = 0;
    virtual bool IsFocused() const noexcept = 0;

    virtual void SetSize(int width, int height) noexcept = 0;
    virtual void SetPosition(int x, int y) noexcept = 0;
    virtual void SetTitle(std::string_view title) noexcept = 0;
    virtual void Show() noexcept = 0;
    virtual void Focus() noexcept = 0;
    virtual void SetAlpha(float alpha) noexcept = 0;
    virtual float GetDpiScale() const noexcept = 0;
    virtual Win32MsgProcHandle AddWin32MsgProc(std::function<Win32MsgProc> proc) noexcept = 0;
    virtual void RemoveWin32MsgProc(Win32MsgProcHandle handle) noexcept = 0;

    virtual sigslot::signal<int, int>& EventResized() noexcept = 0;
    virtual sigslot::signal<int, int>& EventResizing() noexcept = 0;
    virtual sigslot::signal<int, int, MouseButton, Action>& EventTouch() noexcept = 0;
    virtual sigslot::signal<KeyCode, Action>& EventKeyboard() noexcept = 0;
    virtual sigslot::signal<int>& EventMouseWheel() noexcept = 0;
    virtual sigslot::signal<uint32_t>& EventTextInput() noexcept = 0;
};

Nullable<unique_ptr<NativeWindow>> CreateNativeWindow(const NativeWindowCreateDescriptor& desc) noexcept;

std::string_view format_as(WindowHandlerTag v) noexcept;

}  // namespace radray
