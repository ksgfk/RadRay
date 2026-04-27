#pragma once

#include <atomic>

#include <radray/window/native_window.h>
#include <radray/platform/win32_headers.h>

namespace radray {

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

    void DispatchEvents() noexcept override;

    bool ShouldClose() const noexcept override;
    WindowNativeHandler GetNativeHandler() const noexcept override;
    WindowVec2i GetSize() const noexcept override;
    WindowVec2i GetPosition() const noexcept override;
    bool IsMinimized() const noexcept override;
    bool IsFocused() const noexcept override;

    void SetSize(int width, int height) noexcept override;
    void SetPosition(int x, int y) noexcept override;
    void SetTitle(std::string_view title) noexcept override;
    void Show() noexcept override;
    void Focus() noexcept override;
    void SetAlpha(float alpha) noexcept override;
    float GetDpiScale() const noexcept override;
    Win32MsgProcHandle AddWin32MsgProc(std::function<Win32MsgProc> proc) noexcept override;
    void RemoveWin32MsgProc(Win32MsgProcHandle handle) noexcept override;

    sigslot::signal<int, int>& EventResized() noexcept override;
    sigslot::signal<int, int>& EventResizing() noexcept override;
    sigslot::signal<int, int, MouseButton, Action>& EventTouch() noexcept override;
    sigslot::signal<KeyCode, Action>& EventKeyboard() noexcept override;
    sigslot::signal<int>& EventMouseWheel() noexcept override;
    sigslot::signal<uint32_t>& EventTextInput() noexcept override;

    bool EnterFullscreen();
    bool ExitFullscreen();

public:
    struct Win32MsgProcEntry {
        Win32MsgProcHandle Handle{};
        std::function<Win32MsgProc> Proc;
    };

    void DestroyImpl() noexcept;

    HWND _hwnd{nullptr};
    HMONITOR _monitor{nullptr};
    std::atomic<RECT> _windowedRect{};
    DWORD _windowedStyle{0};
    DWORD _windowedExStyle{0};
    vector<Win32MsgProcEntry> _extraWndProcs;
    uint64_t _nextExtraWndProcId{1};
    bool _isFullscreen{false};
    bool _inSizeMove{false};
    std::atomic_bool _closeRequested{false};

    sigslot::signal<int, int> _eventResized;
    sigslot::signal<int, int> _eventResizing;
    sigslot::signal<int, int, MouseButton, Action> _eventTouch;
    sigslot::signal<KeyCode, Action> _eventKeyboard;
    sigslot::signal<int> _eventMouseWheel;
    sigslot::signal<uint32_t> _eventTextInput;
};

Nullable<unique_ptr<Win32Window>> CreateWin32Window(const Win32WindowCreateDescriptor& desc) noexcept;

KeyCode MapWin32VKToKeyCode(WPARAM vk, LPARAM lp) noexcept;
MouseButton MapWin32MSGToMouseButton(UINT msg, WPARAM wParam) noexcept;

}  // namespace radray
