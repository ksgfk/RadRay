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
    Win32Window() noexcept = default;
    ~Win32Window() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void DispatchEvents() noexcept override;

    bool ShouldClose() const noexcept override;

    WindowNativeHandler GetNativeHandler() const noexcept override;

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
    std::atomic_bool _closeRequested{false};
};

Nullable<unique_ptr<Win32Window>> CreateWin32Window(const Win32WindowCreateDescriptor& desc) noexcept;

}  // namespace radray
