#include "win32_window.h"

#include <radray/errors.h>
#include <radray/platform.h>
#include <radray/utility.h>

// make windows happy :)
#ifdef DELETE
#undef DELETE
#endif

namespace radray {

static const wchar_t* RADRAY_WIN32_WINDOW_PROP = L"RADRAY_WIN32_WINDOW_PTR";
static const wchar_t* RADRAY_WIN32_WNDCLASS_NAME = L"RADRAY_WIN32_WNDCLASS";

static unique_ptr<WndClassRAII> g_wndClass;

static LRESULT CALLBACK _RadrayWin32WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    {
        auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
        if (window) {
            for (auto& proc : window->_extraWndProcs) {
                if (proc.expired()) {
                    continue;
                }
                auto procPtr = proc.lock();
                LRESULT r = (*procPtr)(hWnd, uMsg, wParam, lParam);
                if (r) {
                    return r;
                }
            }
        }
    }
    switch (uMsg) {
        case WM_CREATE: {
            auto cs = std::bit_cast<CREATESTRUCT*>(lParam);
            auto window = std::bit_cast<Win32Window*>(cs->lpCreateParams);
            ::SetProp(hWnd, RADRAY_WIN32_WINDOW_PROP, window);
            return 0;
        }
        case WM_ENTERSIZEMOVE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_inSizeMove = true;
                RECT rc{};
                ::GetClientRect(hWnd, &rc);
                window->_windowedRect = rc;
                int width = rc.right - rc.left;
                int height = rc.bottom - rc.top;
                window->_eventResizing(width, height);
            }
            return 0;
        }
        case WM_SIZING: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                RECT rc{};
                ::GetClientRect(hWnd, &rc);
                window->_windowedRect = rc;
                int width = rc.right - rc.left;
                int height = rc.bottom - rc.top;
                window->_eventResizing(width, height);
            }
            return 0;
        }
        case WM_SIZE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                int width = LOWORD(lParam);
                int height = HIWORD(lParam);
                if (!window->_inSizeMove) {  // 最大最小化
                    window->_eventResized(width, height);
                }
            }
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_inSizeMove = false;
                RECT rc{};
                ::GetClientRect(hWnd, &rc);
                window->_windowedRect = rc;
                int width = rc.right - rc.left;
                int height = rc.bottom - rc.top;
                window->_eventResized(width, height);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                window->_eventMouseWheel(delta);
            }
            return 0;
        }
        case WM_DESTROY: {
            ::RemoveProp(hWnd, RADRAY_WIN32_WINDOW_PROP);
            return 0;
        }
        case WM_CLOSE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_closeRequested = true;
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                KeyCode code = MapWin32VKToKeyCode(wParam, lParam);
                Action action = ((lParam & (1 << 30)) == 0) ? Action::PRESSED : Action::REPEATED;
                window->_eventKeyboard(code, action);
            }
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                KeyCode code = MapWin32VKToKeyCode(wParam, lParam);
                window->_eventKeyboard(code, Action::RELEASED);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            auto extractMousePos = [](LPARAM lp, int& x, int& y) noexcept -> void {
                x = static_cast<short>(LOWORD(lp));
                y = static_cast<short>(HIWORD(lp));
            };
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                int x = 0, y = 0;
                extractMousePos(lParam, x, y);
                MouseButton btn = MapWin32MSGToMouseButton(uMsg, wParam);
                Action action = (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN || uMsg == WM_XBUTTONDOWN)
                                    ? Action::PRESSED
                                    : Action::RELEASED;
                if (action == Action::PRESSED) {
                    ::SetCapture(hWnd);
                } else {  // RELEASED
                    if ((wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) == 0) {
                        ::ReleaseCapture();
                    }
                }
                window->_eventTouch(x, y, btn, action);
            }
            if (uMsg == WM_XBUTTONDOWN || uMsg == WM_XBUTTONUP) return TRUE;
            return 0;
        }
        case WM_MOUSEMOVE: {
            auto mapMKBUTTON = [](WPARAM wParam) -> MouseButton {
                if (wParam & MK_LBUTTON) return MouseButton::BUTTON_LEFT;
                if (wParam & MK_RBUTTON) return MouseButton::BUTTON_RIGHT;
                if (wParam & MK_MBUTTON) return MouseButton::BUTTON_MIDDLE;
                if (wParam & MK_XBUTTON1) return MouseButton::BUTTON_4;
                if (wParam & MK_XBUTTON2) return MouseButton::BUTTON_5;
                return MouseButton::UNKNOWN;
            };
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                int x = static_cast<short>(LOWORD(lParam));
                int y = static_cast<short>(HIWORD(lParam));
                MouseButton btn = mapMKBUTTON(wParam);
                window->_eventTouch(x, y, btn, Action::REPEATED);
            };
            return 0;
        }
        default: {
            return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
    }
}

WndClassRAII::WndClassRAII(ATOM clazz, HINSTANCE hInstance, std::wstring_view className) noexcept
    : _clazz(clazz), _hInstance(hInstance), _name(className) {}

WndClassRAII::~WndClassRAII() noexcept {
    if (_clazz) {
        ::UnregisterClass(_name.c_str(), _hInstance);
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
        if (::GetModuleHandleEx(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                moduleAddr,
                &hInstance) == 0) {
            auto fmtErr = FormatLastErrorMessageWin32();
            RADRAY_ERR_LOG("{} {} {} {}", "WIN32", "GetModuleHandleEx", fmtErr, ::GetLastError());
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
        wce.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        wce.hbrBackground = nullptr;
        wce.lpszMenuName = nullptr;
        wce.lpszClassName = RADRAY_WIN32_WNDCLASS_NAME;
        ATOM clazz = ::RegisterClassEx(&wce);
        if (!clazz) {
            auto fmtErr = FormatLastErrorMessageWin32();
            RADRAY_ERR_LOG("{} {} {} {}", "WIN32", "RegisterClassEx", fmtErr, ::GetLastError());
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
        ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }

    wstring title = ToWideChar(desc.Title).value();
    HWND hwnd = ::CreateWindowEx(
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
        RADRAY_ERR_LOG("{} {} {} {}", "WIN32", "CreateWindowEx", fmtErr, ::GetLastError());
        return nullptr;
    }
    win->_hwnd = hwnd;
    {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        win->_windowedRect = rc;
    }
    win->_windowedStyle = style;
    win->_windowedExStyle = exStyle;
    win->_extraWndProcs = {desc.ExtraWndProcs.begin(), desc.ExtraWndProcs.end()};

    ::ShowWindow(hwnd, desc.StartMaximized ? SW_MAXIMIZE : SW_SHOW);
    ::UpdateWindow(hwnd);

    if (desc.Fullscreen) {
        win->EnterFullscreen();
    }

    return win;
}

Win32Window::Win32Window() noexcept {}

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
        ::DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
}

void Win32Window::DispatchEvents() noexcept {
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }
}

bool Win32Window::ShouldClose() const noexcept {
    return _closeRequested;
}

WindowNativeHandler Win32Window::GetNativeHandler() const noexcept {
    return WindowNativeHandler{WindowHandlerTag::HWND, _hwnd};
}

WindowVec2i Win32Window::GetSize() const noexcept {
    RECT rc;
    ::GetClientRect(_hwnd, &rc);
    return WindowVec2i{rc.right - rc.left, rc.bottom - rc.top};
}

bool Win32Window::IsMinimized() const noexcept {
    return ::IsIconic(_hwnd) != 0;
}

void Win32Window::SetSize(int width, int height) noexcept {
    if (!_hwnd) return;
    if (width <= 0 || height <= 0) return;
    if (_isFullscreen) {
        RADRAY_ERR_LOG("{} {} ", "WIN32", "cannot set size when in fullscreen mode");
        return;
    }
    RECT rc{0, 0, width, height};
    DWORD style = ::GetWindowLong(_hwnd, GWL_STYLE);
    DWORD exStyle = ::GetWindowLong(_hwnd, GWL_EXSTYLE);
    ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    ::SetWindowPos(_hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

sigslot::signal<int, int>& Win32Window::EventResized() noexcept {
    return _eventResized;
}

sigslot::signal<int, int>& Win32Window::EventResizing() noexcept {
    return _eventResizing;
}

sigslot::signal<int, int, MouseButton, Action>& Win32Window::EventTouch() noexcept {
    return _eventTouch;
}

sigslot::signal<KeyCode, Action>& Win32Window::EventKeyboard() noexcept {
    return _eventKeyboard;
}

sigslot::signal<int>& Win32Window::EventMouseWheel() noexcept {
    return _eventMouseWheel;
}

bool Win32Window::EnterFullscreen() {
    if (!_hwnd || _isFullscreen) return false;
    _monitor = ::MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfo(_monitor, &mi)) return false;

    RECT rc{};
    ::GetWindowRect(_hwnd, &rc);
    _windowedRect = rc;
    _windowedStyle = ::GetWindowLong(_hwnd, GWL_STYLE);
    _windowedExStyle = ::GetWindowLong(_hwnd, GWL_EXSTYLE);
    ::SetWindowLong(_hwnd, GWL_STYLE, _windowedStyle & ~(WS_OVERLAPPEDWINDOW));
    ::SetWindowPos(
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
    ::SetWindowLong(_hwnd, GWL_STYLE, _windowedStyle);
    ::SetWindowLong(_hwnd, GWL_EXSTYLE, _windowedExStyle);
    RECT rc = _windowedRect.load();
    ::SetWindowPos(
        _hwnd, nullptr,
        rc.left, rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    _isFullscreen = false;
    return true;
}

KeyCode MapWin32VKToKeyCode(WPARAM vk, LPARAM lp) noexcept {
    const bool isExtended = (lp & 0x01000000) != 0;
    switch (vk) {
        case VK_SPACE: return KeyCode::SPACE;
        case VK_OEM_7: return KeyCode::APOSTROPHE;
        case VK_OEM_COMMA: return KeyCode::COMMA;
        case VK_OEM_MINUS: return KeyCode::MINUS;
        case VK_OEM_PERIOD: return KeyCode::PERIOD;
        case VK_OEM_2: return KeyCode::SLASH;
        case '0': return KeyCode::NUM0;
        case '1': return KeyCode::NUM1;
        case '2': return KeyCode::NUM2;
        case '3': return KeyCode::NUM3;
        case '4': return KeyCode::NUM4;
        case '5': return KeyCode::NUM5;
        case '6': return KeyCode::NUM6;
        case '7': return KeyCode::NUM7;
        case '8': return KeyCode::NUM8;
        case '9': return KeyCode::NUM9;
        case VK_OEM_1: return KeyCode::SEMICOLON;
        case VK_OEM_PLUS: return KeyCode::EQUAL;
        case 'A': return KeyCode::A;
        case 'B': return KeyCode::B;
        case 'C': return KeyCode::C;
        case 'D': return KeyCode::D;
        case 'E': return KeyCode::E;
        case 'F': return KeyCode::F;
        case 'G': return KeyCode::G;
        case 'H': return KeyCode::H;
        case 'I': return KeyCode::I;
        case 'J': return KeyCode::J;
        case 'K': return KeyCode::K;
        case 'L': return KeyCode::L;
        case 'M': return KeyCode::M;
        case 'N': return KeyCode::N;
        case 'O': return KeyCode::O;
        case 'P': return KeyCode::P;
        case 'Q': return KeyCode::Q;
        case 'R': return KeyCode::R;
        case 'S': return KeyCode::S;
        case 'T': return KeyCode::T;
        case 'U': return KeyCode::U;
        case 'V': return KeyCode::V;
        case 'W': return KeyCode::W;
        case 'X': return KeyCode::X;
        case 'Y': return KeyCode::Y;
        case 'Z': return KeyCode::Z;
        case VK_OEM_4: return KeyCode::LEFT_BRACKET;
        case VK_OEM_5: return KeyCode::BACKSLASH;
        case VK_OEM_6: return KeyCode::RIGHT_BRACKET;
        case VK_OEM_3: return KeyCode::GRAVE_ACCENT;
        case VK_ESCAPE: return KeyCode::ESCAPE;
        case VK_RETURN: return isExtended ? KeyCode::KP_ENTER : KeyCode::ENTER;
        case VK_TAB: return KeyCode::TAB;
        case VK_BACK: return KeyCode::BACKSPACE;
        case VK_INSERT: return KeyCode::INSERT;
        case VK_DELETE: return KeyCode::DELETE;
        case VK_RIGHT: return KeyCode::RIGHT;
        case VK_LEFT: return KeyCode::LEFT;
        case VK_DOWN: return KeyCode::DOWN;
        case VK_UP: return KeyCode::UP;
        case VK_PRIOR: return KeyCode::PAGE_UP;
        case VK_NEXT: return KeyCode::PAGE_DOWN;
        case VK_HOME: return KeyCode::HOME;
        case VK_END: return KeyCode::END;
        case VK_CAPITAL: return KeyCode::CAPS_LOCK;
        case VK_SCROLL: return KeyCode::SCROLL_LOCK;
        case VK_NUMLOCK: return KeyCode::NUM_LOCK;
        case VK_SNAPSHOT: return KeyCode::PRINT_SCREEN;
        case VK_PAUSE: return KeyCode::PAUSE;
        case VK_F1: return KeyCode::F1;
        case VK_F2: return KeyCode::F2;
        case VK_F3: return KeyCode::F3;
        case VK_F4: return KeyCode::F4;
        case VK_F5: return KeyCode::F5;
        case VK_F6: return KeyCode::F6;
        case VK_F7: return KeyCode::F7;
        case VK_F8: return KeyCode::F8;
        case VK_F9: return KeyCode::F9;
        case VK_F10: return KeyCode::F10;
        case VK_F11: return KeyCode::F11;
        case VK_F12: return KeyCode::F12;
        case VK_F13: return KeyCode::F13;
        case VK_F14: return KeyCode::F14;
        case VK_F15: return KeyCode::F15;
        case VK_F16: return KeyCode::F16;
        case VK_F17: return KeyCode::F17;
        case VK_F18: return KeyCode::F18;
        case VK_F19: return KeyCode::F19;
        case VK_F20: return KeyCode::F20;
        case VK_F21: return KeyCode::F21;
        case VK_F22: return KeyCode::F22;
        case VK_F23: return KeyCode::F23;
        case VK_F24: return KeyCode::F24;
        case VK_NUMPAD0: return KeyCode::KP_0;
        case VK_NUMPAD1: return KeyCode::KP_1;
        case VK_NUMPAD2: return KeyCode::KP_2;
        case VK_NUMPAD3: return KeyCode::KP_3;
        case VK_NUMPAD4: return KeyCode::KP_4;
        case VK_NUMPAD5: return KeyCode::KP_5;
        case VK_NUMPAD6: return KeyCode::KP_6;
        case VK_NUMPAD7: return KeyCode::KP_7;
        case VK_NUMPAD8: return KeyCode::KP_8;
        case VK_NUMPAD9: return KeyCode::KP_9;
        case VK_DECIMAL: return KeyCode::KP_DECIMAL;
        case VK_DIVIDE: return KeyCode::KP_DIVIDE;
        case VK_MULTIPLY: return KeyCode::KP_MULTIPLY;
        case VK_SUBTRACT: return KeyCode::KP_SUBTRACT;
        case VK_ADD: return KeyCode::KP_ADD;
        case VK_SHIFT: return KeyCode::LEFT_SHIFT;
        case VK_CONTROL: return KeyCode::LEFT_CONTROL;
        case VK_MENU: return KeyCode::LEFT_ALT;
        case VK_LSHIFT: return KeyCode::LEFT_SHIFT;
        case VK_RSHIFT: return KeyCode::RIGHT_SHIFT;
        case VK_LCONTROL: return KeyCode::LEFT_CONTROL;
        case VK_RCONTROL: return KeyCode::RIGHT_CONTROL;
        case VK_LMENU: return KeyCode::LEFT_ALT;
        case VK_RMENU: return KeyCode::RIGHT_ALT;
        case VK_LWIN: return KeyCode::LEFT_SUPER;
        case VK_RWIN: return KeyCode::RIGHT_SUPER;
        case VK_APPS: return KeyCode::MENU;
        default:
            if (vk >= 'a' && vk <= 'z') {
                return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (int(vk) - int('a')));
            }
            return KeyCode::UNKNOWN;
    }
}

MouseButton MapWin32MSGToMouseButton(UINT msg, WPARAM wParam) noexcept {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return MouseButton::BUTTON_LEFT;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return MouseButton::BUTTON_RIGHT;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return MouseButton::BUTTON_MIDDLE;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            WORD xbtn = HIWORD(wParam);
            if (xbtn == XBUTTON1) return MouseButton::BUTTON_4;
            if (xbtn == XBUTTON2) return MouseButton::BUTTON_5;
            return MouseButton::UNKNOWN;
        }
        default:
            return MouseButton::UNKNOWN;
    }
}

}  // namespace radray
