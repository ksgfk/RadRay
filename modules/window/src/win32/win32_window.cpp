#include "win32_window.h"

#include <radray/platform.h>
#include <radray/utility.h>

namespace radray {

static const wchar_t* RADRAY_WIN32_WINDOW_PROP = L"RADRAY_WIN32_WINDOW_PTR";
static const wchar_t* RADRAY_WIN32_WNDCLASS_NAME = L"RADRAY_WIN32_WNDCLASS";

static unique_ptr<WndClassRAII> g_wndClass;

static LRESULT CALLBACK _RadrayWin32WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            auto cs = std::bit_cast<CREATESTRUCT*>(lParam);
            auto window = std::bit_cast<Win32Window*>(cs->lpCreateParams);
            SetProp(hWnd, RADRAY_WIN32_WINDOW_PROP, window);
            return 0;
        }
        case WM_DESTROY: {
            RemoveProp(hWnd, RADRAY_WIN32_WINDOW_PROP);
            return 0;
        }
        case WM_CLOSE: {
            auto window = std::bit_cast<Win32Window*>(GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_closeRequested = true;
            }
            return 0;
        }
        default: {
            return DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
    }
}

WndClassRAII::WndClassRAII(ATOM clazz, HINSTANCE hInstance, std::wstring_view className) noexcept
    : _clazz(clazz), _hInstance(hInstance), _name(className) {}

WndClassRAII::~WndClassRAII() noexcept {
    if (_clazz) {
        UnregisterClass(_name.c_str(), _hInstance);
        _clazz = 0;
    }
}

ATOM WndClassRAII::GetClass() const noexcept { return _clazz; }

std::wstring_view WndClassRAII::GetName() const noexcept { return _name; }

HINSTANCE WndClassRAII::GetHInstance() const noexcept { return _hInstance; }

Nullable<unique_ptr<Win32Window>> CreateWin32Window(const Win32WindowCreateDescriptor& desc) noexcept {
    HMODULE hInstance;
    {
        LPCWSTR moduleAddr = std::bit_cast<LPCWSTR>(&_RadrayWin32WindowProc);
        if (GetModuleHandleEx(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                moduleAddr,
                &hInstance) == 0) {
            auto fmtErr = FormatLastErrorMessageWin32();
            RADRAY_ERR_LOG("win32 GetModuleHandleEx failed, reason: {} (code={})", fmtErr, GetLastError());
            return nullptr;
        }
    }
    if (!g_wndClass) {
        WNDCLASSEX wce{};
        wce.cbSize = sizeof(wce);
        wce.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wce.lpfnWndProc = _RadrayWin32WindowProc;
        wce.cbClsExtra = 0;
        wce.cbWndExtra = 0;
        wce.hInstance = hInstance;
        wce.hIcon = nullptr;
        wce.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wce.hbrBackground = nullptr;
        wce.lpszMenuName = nullptr;
        wce.lpszClassName = RADRAY_WIN32_WNDCLASS_NAME;
        ATOM clazz = RegisterClassEx(&wce);
        if (!clazz) {
            auto fmtErr = FormatLastErrorMessageWin32();
            RADRAY_ERR_LOG("win32 RegisterClassEx failed, reason: {} (code={})", fmtErr, GetLastError());
            return nullptr;
        }
        g_wndClass = std::make_unique<WndClassRAII>(clazz, wce.hInstance, RADRAY_WIN32_WNDCLASS_NAME);
    }

    auto win = std::make_unique<Win32Window>();
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!desc.Resizable) {
        style &= ~WS_THICKFRAME;
        style &= ~WS_MAXIMIZEBOX;
    }
    if (desc.StartMaximized) {
        style |= WS_MAXIMIZE;
    }

    DWORD exStyle = WS_EX_APPWINDOW;

    int x = (desc.X < 0) ? CW_USEDEFAULT : desc.X;
    int y = (desc.Y < 0) ? CW_USEDEFAULT : desc.Y;
    int w = desc.Width;
    int h = desc.Height;

    if (style & WS_OVERLAPPEDWINDOW) {
        RECT rc{0, 0, w, h};
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }

    wstring title = ToWideChar(desc.Title).value();
    HWND hwnd = CreateWindowEx(
        exStyle,
        g_wndClass->GetName().data(),
        title.c_str(),
        style,
        x, y,
        w, h,
        nullptr,
        nullptr,
        g_wndClass->GetHInstance(),
        win.get());
    if (!hwnd) {
        auto fmtErr = FormatLastErrorMessageWin32();
        RADRAY_ERR_LOG("win32 CreateWindowEx failed: {} (code={})", fmtErr, GetLastError());
        return nullptr;
    }
    win->_hwnd = hwnd;
    GetWindowRect(hwnd, &win->_windowedRect);
    win->_windowedStyle = style;
    win->_windowedExStyle = exStyle;

    ShowWindow(hwnd, desc.StartMaximized ? SW_MAXIMIZE : SW_SHOW);
    UpdateWindow(hwnd);

    if (desc.Fullscreen) {
        win->EnterFullscreen();
    }

    return win;
}

Win32Window::~Win32Window() noexcept {
    this->DestroyImpl();
}

bool Win32Window::IsValid() const noexcept {
    return _hwnd != nullptr;
}

void Win32Window::Destroy() noexcept {
    this->DestroyImpl();
}

void Win32Window::DestroyImpl() noexcept {
    if (_hwnd) {
        DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
}

void Win32Window::DispatchEvents() noexcept {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

bool Win32Window::ShouldClose() const noexcept {
    return _closeRequested;
}

WindowNativeHandler Win32Window::GetNativeHandler() const noexcept {
    return WindowNativeHandler{WindowHandlerTag::HWND, _hwnd};
}

bool Win32Window::EnterFullscreen() {
    if (!_hwnd || _isFullscreen) return false;
    _monitor = MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(_monitor, &mi)) return false;

    GetWindowRect(_hwnd, &_windowedRect);
    _windowedStyle = GetWindowLong(_hwnd, GWL_STYLE);
    _windowedExStyle = GetWindowLong(_hwnd, GWL_EXSTYLE);
    SetWindowLong(_hwnd, GWL_STYLE, _windowedStyle & ~(WS_OVERLAPPEDWINDOW));
    SetWindowPos(
        _hwnd, HWND_TOP,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    _isFullscreen = true;
    return true;
}

bool Win32Window::ExitFullscreen() {
    if (!_hwnd || !_isFullscreen) return false;
    SetWindowLong(_hwnd, GWL_STYLE, _windowedStyle);
    SetWindowLong(_hwnd, GWL_EXSTYLE, _windowedExStyle);
    SetWindowPos(
        _hwnd, nullptr,
        _windowedRect.left, _windowedRect.top,
        _windowedRect.right - _windowedRect.left,
        _windowedRect.bottom - _windowedRect.top,
        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    _isFullscreen = false;
    return true;
}

}  // namespace radray
