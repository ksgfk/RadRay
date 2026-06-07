#pragma once

#ifdef RADRAY_PLATFORM_MACOS

#include <radray/window/native_window.h>

namespace radray {

class CocoaEventPump final : public NativeEventPump {
public:
    CocoaEventPump() noexcept;
    ~CocoaEventPump() noexcept override;

    void DispatchEvents() noexcept override;
    bool Register(NativeWindow* window) noexcept override;
    void Unregister(NativeWindow* window) noexcept override;
    sigslot::signal<NativeWindow*>& EventModalLoopTick() noexcept override;

private:
    void* _pump{nullptr};
    vector<NativeWindow*> _windows;
    sigslot::signal<NativeWindow*> _eventModalLoopTick;
};

class CocoaWindow final : public NativeWindow {
public:
    CocoaWindow() noexcept;
    ~CocoaWindow() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    NativeWindowType GetType() const noexcept override { return NativeWindowType::CocoaNSWindow; }

    void Show() noexcept override;
    void Show(NativeWindowShowMode mode) noexcept override;
    void Focus() noexcept override;

    bool ShouldClose() const noexcept override;
    void* GetNativeHandler() const noexcept override;
    void* GetContentView() const noexcept;
    Eigen::Vector2i GetSize() const noexcept override;
    Eigen::Vector2i GetPosition() const noexcept override;
    float GetDpiScale() const noexcept override;
    bool IsMinimized() const noexcept override;
    bool IsFocused() const noexcept override;

    void SetSize(int width, int height) noexcept override;
    void SetPosition(int x, int y) noexcept override;
    void SetTitle(std::string_view title) noexcept override;
    void SetAlpha(float alpha) noexcept override;
    void SetOwner(Nullable<NativeWindow*> owner) noexcept override;
    void SetDecorated(bool value) noexcept override;
    void SetShowInTaskbar(bool value) noexcept override;
    void SetTopMost(bool value) noexcept override;
    void SetFocusOnClick(bool value) noexcept override;
    void SetInputPassthrough(bool value) noexcept override;
    Eigen::Vector2i ClientToScreen(Eigen::Vector2i pos) const noexcept override;
    Eigen::Vector2i ScreenToClient(Eigen::Vector2i pos) const noexcept override;
    void* GetObjCWindow() const noexcept;

    sigslot::signal<int, int>& EventResized() noexcept override;
    sigslot::signal<int, int, MouseButton, Action>& EventTouch() noexcept override;
    sigslot::signal<KeyCode, Action>& EventKeyboard() noexcept override;
    sigslot::signal<int>& EventMouseWheel() noexcept override;
    sigslot::signal<std::string_view>& EventTextInput() noexcept override;
    sigslot::signal<bool>& EventFocused() noexcept override;
    sigslot::signal<>& EventCloseRequested() noexcept override;
    sigslot::signal<int, int>& EventMoved() noexcept override;
    sigslot::signal<>& EventMouseLeave() noexcept override;

    static void GlobalInit() noexcept;
    static void GlobalShutdown() noexcept;
    static Nullable<unique_ptr<CocoaWindow>> Create(const CocoaWindowCreateDescriptor& desc) noexcept;

public:
    void* _objcWindow{nullptr};
    void* _objcDelegate{nullptr};
    sigslot::signal<int, int> _eventResized;
    sigslot::signal<int, int, MouseButton, Action> _eventTouch;
    sigslot::signal<KeyCode, Action> _eventKeyboard;
    sigslot::signal<int> _eventMouseWheel;
    sigslot::signal<std::string_view> _eventTextInput;
    sigslot::signal<bool> _eventFocused;
    sigslot::signal<> _eventCloseRequested;
    sigslot::signal<int, int> _eventMoved;
    sigslot::signal<> _eventMouseLeave;
};

KeyCode MapCocoaKeyCodeToKeyCode(unsigned short keyCode) noexcept;

}  // namespace radray

#endif
