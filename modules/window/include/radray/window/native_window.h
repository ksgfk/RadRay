#pragma once

#include <variant>
#include <string_view>
#include <functional>
#include <limits>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/utility.h>
#include <radray/basic_math.h>
#include <radray/window/input.h>

namespace radray {

enum class NativeWindowType {
    UNKNOWN,
    Win32HWND,
    CocoaNSWindow
};

enum class NativeWindowShowMode {
    Default,
    NoActivate
};

class NativeWindow;

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
    Nullable<NativeWindow*> OwnerWindow{nullptr};
    bool Decorated{true};
    bool ShowInTaskbar{true};
    bool TopMost{false};
    bool ActivateOnShow{true};
    bool FocusOnClick{true};
    bool InputPassthrough{false};
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
    Nullable<NativeWindow*> OwnerWindow{nullptr};
    bool Decorated{true};
    bool ShowInTaskbar{true};
    bool TopMost{false};
    bool ActivateOnShow{true};
    bool FocusOnClick{true};
    bool InputPassthrough{false};
};

using NativeWindowCreateDescriptor = std::variant<Win32WindowCreateDescriptor, CocoaWindowCreateDescriptor>;

class NativeWindow {
public:
    virtual ~NativeWindow() noexcept = default;

    virtual bool IsValid() const noexcept = 0;
    virtual void Destroy() noexcept = 0;
    virtual NativeWindowType GetType() const noexcept = 0;

    virtual void Show() noexcept = 0;
    virtual void Show(NativeWindowShowMode mode) noexcept = 0;
    virtual void Focus() noexcept = 0;

    virtual bool ShouldClose() const noexcept = 0;
    virtual void* GetNativeHandler() const noexcept = 0;
    virtual Eigen::Vector2i GetSize() const noexcept = 0;
    virtual Eigen::Vector2i GetPosition() const noexcept = 0;
    virtual float GetDpiScale() const noexcept = 0;
    virtual bool IsMinimized() const noexcept = 0;
    virtual bool IsFocused() const noexcept = 0;

    virtual void SetSize(int width, int height) noexcept = 0;
    virtual void SetPosition(int x, int y) noexcept = 0;
    virtual void SetTitle(std::string_view title) noexcept = 0;
    virtual void SetAlpha(float alpha) noexcept = 0;
    virtual void SetOwner(Nullable<NativeWindow*> owner) noexcept = 0;
    virtual void SetDecorated(bool value) noexcept = 0;
    virtual void SetShowInTaskbar(bool value) noexcept = 0;
    virtual void SetTopMost(bool value) noexcept = 0;
    virtual void SetFocusOnClick(bool value) noexcept = 0;
    virtual void SetInputPassthrough(bool value) noexcept = 0;
    virtual Eigen::Vector2i ClientToScreen(Eigen::Vector2i pos) const noexcept = 0;
    virtual Eigen::Vector2i ScreenToClient(Eigen::Vector2i pos) const noexcept = 0;

    virtual sigslot::signal<int, int>& EventResized() noexcept = 0;
    virtual sigslot::signal<int, int, MouseButton, Action>& EventTouch() noexcept = 0;
    virtual sigslot::signal<KeyCode, Action>& EventKeyboard() noexcept = 0;
    virtual sigslot::signal<int>& EventMouseWheel() noexcept = 0;
    // UTF-8 text input. The string_view is valid and null-terminated only during signal emission.
    virtual sigslot::signal<std::string_view>& EventTextInput() noexcept = 0;
    virtual sigslot::signal<bool>& EventFocused() noexcept = 0;
    virtual sigslot::signal<>& EventCloseRequested() noexcept = 0;
    virtual sigslot::signal<int, int>& EventMoved() noexcept = 0;
    virtual sigslot::signal<>& EventMouseLeave() noexcept = 0;

    static void GlobalInit() noexcept;
    static void GlobalShutdown() noexcept;
    static Nullable<unique_ptr<NativeWindow>> Create(const NativeWindowCreateDescriptor& desc) noexcept;
};

class NativeEventPump {
public:
    virtual ~NativeEventPump() noexcept = default;

    virtual void DispatchEvents() noexcept = 0;
    virtual bool Register(NativeWindow* window) noexcept = 0;
    virtual void Unregister(NativeWindow* window) noexcept = 0;
    virtual sigslot::signal<NativeWindow*>& EventModalLoopTick() noexcept = 0;

    static Nullable<unique_ptr<NativeEventPump>> Create(NativeWindowType type) noexcept;
};

std::string_view format_as(NativeWindowType v) noexcept;

}  // namespace radray
