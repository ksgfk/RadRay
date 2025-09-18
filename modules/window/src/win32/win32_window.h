#pragma once

#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>

#include <radray/window/native_window.h>

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

    shared_ptr<MultiDelegate<NativeWindowBeginResizeDelegate>> EventBeginResize() const noexcept override;
    shared_ptr<MultiDelegate<NativeWindowResizingDelegate>> EventResizing() const noexcept override;
    shared_ptr<MultiDelegate<NativeWindowEndResizeDelegate>> EventEndResize() const noexcept override;

    bool EnterFullscreen();
    bool ExitFullscreen();

public:
    void DestroyImpl() noexcept;

    HWND _hwnd{nullptr};
    HMONITOR _monitor{nullptr};
    RECT _windowedRect{0, 0, 0, 0};
    DWORD _windowedStyle{0};
    DWORD _windowedExStyle{0};
    bool _isFullscreen{false};
    bool _inSizeMove{false};
    std::atomic_bool _closeRequested{false};
    shared_ptr<MultiDelegate<NativeWindowBeginResizeDelegate>> _eventBeginResize{};
    shared_ptr<MultiDelegate<NativeWindowResizingDelegate>> _eventResizing{};
    shared_ptr<MultiDelegate<NativeWindowEndResizeDelegate>> _eventEndResize{};
};

Nullable<unique_ptr<Win32Window>> CreateWin32Window(const Win32WindowCreateDescriptor& desc) noexcept;

}  // namespace radray
