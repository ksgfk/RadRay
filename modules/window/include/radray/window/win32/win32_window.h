#pragma once

#ifdef RADRAY_PLATFORM_WINDOWS

#include <atomic>

#include <radray/window/native_window.h>
#include <radray/platform/win32_headers.h>

namespace radray {

class Win32EventPump final : public NativeEventPump {
public:
    ~Win32EventPump() noexcept override = default;

    void DispatchEvents() noexcept override;
    bool Register(NativeWindow* window) noexcept override;
    void Unregister(NativeWindow* window) noexcept override;
    sigslot::signal<NativeWindow*>& EventModalLoopTick() noexcept override;

private:
    vector<NativeWindow*> _windows;
    sigslot::signal<NativeWindow*> _eventModalLoopTick;
};

class WndClassRAII {
public:
    WndClassRAII(ATOM clazz, HINSTANCE hInstance, std::wstring_view className) noexcept;
    WndClassRAII(const WndClassRAII&) = delete;
    WndClassRAII& operator=(const WndClassRAII&) = delete;
    WndClassRAII(WndClassRAII&&) noexcept = delete;
    WndClassRAII& operator=(WndClassRAII&&) noexcept = delete;
    ~WndClassRAII() noexcept;

    ATOM GetClass() const noexcept;
    std::wstring_view GetName() const noexcept;
    HINSTANCE GetHInstance() const noexcept;

private:
    ATOM _clazz;
    HINSTANCE _hInstance;
    wstring _name;
};

class Win32Window : public NativeWindow {
public:
    Win32Window() noexcept;
    ~Win32Window() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    NativeWindowType GetType() const noexcept override { return NativeWindowType::Win32HWND; }

    bool ShouldClose() const noexcept override;
    void* GetNativeHandler() const noexcept override;
    Eigen::Vector2i GetSize() const noexcept override;
    Eigen::Vector2i GetPosition() const noexcept override;
    bool IsMinimized() const noexcept override;
    bool IsFocused() const noexcept override;

    void SetSize(int width, int height) noexcept override;
    void SetPosition(int x, int y) noexcept override;
    void SetTitle(std::string_view title) noexcept override;
    void Show() noexcept override;
    void Show(NativeWindowShowMode mode) noexcept override;
    void Focus() noexcept override;
    void SetAlpha(float alpha) noexcept override;
    void SetOwner(Nullable<NativeWindow*> owner) noexcept override;
    void SetDecorated(bool value) noexcept override;
    void SetShowInTaskbar(bool value) noexcept override;
    void SetTopMost(bool value) noexcept override;
    void SetFocusOnClick(bool value) noexcept override;
    void SetInputPassthrough(bool value) noexcept override;
    float GetDpiScale() const noexcept override;
    Eigen::Vector2i ClientToScreen(Eigen::Vector2i pos) const noexcept override;
    Eigen::Vector2i ScreenToClient(Eigen::Vector2i pos) const noexcept override;

    sigslot::signal<int, int>& EventResized() noexcept override;
    sigslot::signal<int, int, MouseButton, Action>& EventTouch() noexcept override;
    sigslot::signal<KeyCode, Action>& EventKeyboard() noexcept override;
    sigslot::signal<int>& EventMouseWheel() noexcept override;
    sigslot::signal<std::string_view>& EventTextInput() noexcept override;
    sigslot::signal<bool>& EventFocused() noexcept override;
    sigslot::signal<>& EventCloseRequested() noexcept override;
    sigslot::signal<int, int>& EventMoved() noexcept override;
    sigslot::signal<>& EventMouseLeave() noexcept override;

    bool EnterFullscreen();
    bool ExitFullscreen();

    static void GlobalInit() noexcept;
    static void GlobalShutdown() noexcept;
    static Nullable<unique_ptr<Win32Window>> Create(const Win32WindowCreateDescriptor& desc) noexcept;

public:
    void DestroyImpl() noexcept;

    HWND _hwnd{nullptr};
    Win32EventPump* _eventPump{nullptr};
    HMONITOR _monitor{nullptr};
    std::atomic<RECT> _windowedRect{};
    POINT _lastClientPos{};
    int _lastClientWidth{0};
    int _lastClientHeight{0};
    DWORD _windowedStyle{0};
    DWORD _windowedExStyle{0};
    uint64_t _nextExtraWndProcId{1};
    HWND _ownerHwnd{nullptr};
    wchar_t _highSurrogate{0};
    int _inModalLoop{0};
    bool _isFullscreen{false};
    bool _sizeMoveResized{false};
    bool _hasLastClientState{false};
    bool _resizable{false};
    bool _decorated{true};
    bool _showInTaskbar{true};
    bool _topMost{false};
    bool _activateOnShow{true};
    bool _focusOnClick{true};
    bool _inputPassthrough{false};
    bool _trackingMouseLeave{false};
    std::atomic_bool _closeRequested{false};

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

KeyCode MapWin32VKToKeyCode(WPARAM vk, LPARAM lp) noexcept;
MouseButton MapWin32MSGToMouseButton(UINT msg, WPARAM wParam) noexcept;

}  // namespace radray

#endif
