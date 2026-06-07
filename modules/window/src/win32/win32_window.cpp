#include <radray/window/win32/win32_window.h>

#include <radray/platform/win32_headers.h>
#include <radray/text_encoding.h>
#include <radray/logger.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>

// make windows happy :)
#ifdef DELETE
#undef DELETE
#endif

namespace radray {

#ifndef WM_UNICHAR
#define WM_UNICHAR 0x0109
#endif

#ifndef UNICODE_NOCHAR
#define UNICODE_NOCHAR 0xFFFF
#endif

static const wchar_t* RADRAY_WIN32_WINDOW_PROP = L"RADRAY_WIN32_WINDOW_PTR";
static const wchar_t* RADRAY_WIN32_WNDCLASS_NAME = L"RADRAY_WIN32_WNDCLASS";

static unique_ptr<WndClassRAII> g_wndClass;

static UINT_PTR GetModalLoopTimerId() noexcept {
    return std::bit_cast<UINT_PTR>(&GetModalLoopTimerId);
}

static auto _FmtWin32ErrMessage(DWORD errCode) {
    void* buffer = nullptr;
    ::FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&buffer,
        0, nullptr);
    auto msg = fmt::format("{} (code = 0x{:x}).", static_cast<char*>(buffer), errCode);
    ::LocalFree(buffer);
    return msg;
}

static auto _FmtWin32LastErrMessage() {
    return _FmtWin32ErrMessage(::GetLastError());
}

static bool IsHighSurrogate(WPARAM ch) noexcept {
    return ch >= 0xD800 && ch <= 0xDBFF;
}

static bool IsLowSurrogate(WPARAM ch) noexcept {
    return ch >= 0xDC00 && ch <= 0xDFFF;
}

static std::optional<string> ToUtf8(std::wstring_view text) noexcept {
    if (text.empty() || text.size() >= std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::nullopt;
    }

    string utf8(size, '\0');
    int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    if (written != size) {
        return std::nullopt;
    }

    return utf8;
}

static std::optional<wstring> ToUtf16(uint32_t codepoint) noexcept {
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return std::nullopt;
    }

    wstring utf16;
    if (codepoint <= 0xFFFF) {
        utf16.push_back(static_cast<wchar_t>(codepoint));
        return utf16;
    }

    codepoint -= 0x10000;
    utf16.push_back(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
    utf16.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
    return utf16;
}

static void EmitTextInputUtf16(Win32Window* window, std::wstring_view text) noexcept {
    auto utf8 = ToUtf8(text);
    if (!utf8.has_value()) {
        RADRAY_WARN_LOG(L"Win32Window cannot convert wstring to utf8 {}", text);
        return;
    }

    window->_eventTextInput(std::string_view{utf8->c_str(), utf8->size()});
}

static void EmitTextInputCodepoint(Win32Window* window, uint32_t codepoint) noexcept {
    auto utf16 = ToUtf16(codepoint);
    if (!utf16.has_value()) {
        return;
    }

    EmitTextInputUtf16(window, *utf16);
}

static DWORD BuildWindowStyle(bool decorated, bool resizable) noexcept {
    if (!decorated) {
        return WS_POPUP;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!resizable) {
        style &= ~WS_THICKFRAME;
        style &= ~WS_MAXIMIZEBOX;
    }
    return style;
}

static DWORD BuildWindowExStyle(bool showInTaskbar, bool topMost) noexcept {
    DWORD exStyle = showInTaskbar ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW;
    if (topMost) {
        exStyle |= WS_EX_TOPMOST;
    }
    return exStyle;
}

static HWND ExtractOwnerHwnd(Nullable<NativeWindow*> owner) noexcept {
    if (!owner) {
        return nullptr;
    }

    NativeWindow* ownerWindow = owner.Get();
    if (ownerWindow == nullptr) {
        return nullptr;
    }
    if (ownerWindow->GetType() != NativeWindowType::Win32HWND) {
        RADRAY_ERR_LOG("Win32Window owner must be a Win32HWND window");
        return nullptr;
    }
    return static_cast<HWND>(ownerWindow->GetNativeHandler());
}

static bool EnableProcessDpiAwarenessContext(HANDLE context) noexcept {
#if (WINVER >= 0x0605)
    return ::SetProcessDpiAwarenessContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(context)) != 0;
#else
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);

    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return false;

    auto setProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setProcessDpiAwarenessContext == nullptr) return false;

    return setProcessDpiAwarenessContext(context) != 0;
#endif
}

static bool EnableProcessDpiAwarenessShcore(int awareness) noexcept {
    using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);

    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (shcore == nullptr) return false;

    auto setProcessDpiAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
        ::GetProcAddress(shcore, "SetProcessDpiAwareness"));
    bool ok = false;
    if (setProcessDpiAwareness != nullptr) {
        const HRESULT hr = setProcessDpiAwareness(awareness);
        ok = SUCCEEDED(hr) || hr == E_ACCESSDENIED;
    }
    ::FreeLibrary(shcore);
    return ok;
}

static void EnableProcessDpiAwareness() noexcept {
    if (EnableProcessDpiAwarenessContext(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))) ||
        EnableProcessDpiAwarenessContext(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-3))) ||
        EnableProcessDpiAwarenessShcore(2) ||
        ::SetProcessDPIAware() != 0) {
        return;
    }

    const DWORD err = ::GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        RADRAY_WARN_LOG("Set process DPI awareness failed: {}", _FmtWin32ErrMessage(err));
    }
}

static void ApplyWindowStyles(Win32Window* window) noexcept {
    if (window == nullptr || window->_hwnd == nullptr) {
        return;
    }

    DWORD style = BuildWindowStyle(window->_decorated, window->_resizable);
    DWORD exStyle = BuildWindowExStyle(window->_showInTaskbar, window->_topMost);

    const auto oldStyle = static_cast<DWORD>(::GetWindowLongPtrW(window->_hwnd, GWL_STYLE));
    const auto oldExStyle = static_cast<DWORD>(::GetWindowLongPtrW(window->_hwnd, GWL_EXSTYLE));
    style |= oldStyle & (WS_VISIBLE | WS_DISABLED | WS_MINIMIZE | WS_MAXIMIZE);
    exStyle |= oldExStyle & WS_EX_LAYERED;

    if (!window->_isFullscreen) {
        ::SetWindowLongPtrW(window->_hwnd, GWL_STYLE, static_cast<LONG_PTR>(style));
        ::SetWindowLongPtrW(window->_hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle));
        window->_windowedStyle = style;
        window->_windowedExStyle = exStyle;
        ::SetWindowPos(
            window->_hwnd,
            window->_topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        return;
    }

    ::SetWindowPos(
        window->_hwnd,
        window->_topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static bool GetClientState(HWND hwnd, POINT& pos, int& width, int& height) noexcept {
    RECT rc{};
    if (::GetClientRect(hwnd, &rc) == 0) {
        return false;
    }

    pos = POINT{0, 0};
    if (::ClientToScreen(hwnd, &pos) == 0) {
        return false;
    }

    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    return true;
}

static void RememberClientState(Win32Window* window, POINT pos, int width, int height) noexcept {
    window->_lastClientPos = pos;
    window->_lastClientWidth = width;
    window->_lastClientHeight = height;
    window->_hasLastClientState = true;
}

static void RefreshClientState(Win32Window* window) noexcept {
    if (window == nullptr || window->_hwnd == nullptr) {
        return;
    }

    POINT pos{};
    int width = 0;
    int height = 0;
    if (GetClientState(window->_hwnd, pos, width, height)) {
        RememberClientState(window, pos, width, height);
    }
}

static void EmitWindowPosChanged(Win32Window* window, bool forceResized) noexcept {
    if (window == nullptr || window->_hwnd == nullptr) {
        return;
    }

    POINT pos{};
    int width = 0;
    int height = 0;
    if (!GetClientState(window->_hwnd, pos, width, height)) {
        return;
    }

    const bool hadState = window->_hasLastClientState;
    const bool moved = !hadState || pos.x != window->_lastClientPos.x || pos.y != window->_lastClientPos.y;
    const bool resized = !hadState || width != window->_lastClientWidth || height != window->_lastClientHeight;
    RememberClientState(window, pos, width, height);

    if (moved) {
        window->_eventMoved(pos.x, pos.y);
    }
    if (!resized && !forceResized) {
        return;
    }

    if (window->_inModalLoop > 0) {
        window->_sizeMoveResized = true;
    } else {
        window->_eventResized(width, height);
    }
}

void Win32EventPump::DispatchEvents() noexcept {
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }
}

bool Win32EventPump::Register(NativeWindow* window) noexcept {
    if (window == nullptr || window->GetType() != NativeWindowType::Win32HWND) {
        return false;
    }
    if (std::ranges::find(_windows, window) != _windows.end()) {
        return true;
    }
    _windows.push_back(window);
    static_cast<Win32Window*>(window)->_eventPump = this;
    return true;
}

void Win32EventPump::Unregister(NativeWindow* window) noexcept {
    auto iter = std::ranges::find(_windows, window);
    if (iter != _windows.end()) {
        auto win32Window = static_cast<Win32Window*>(window);
        if (win32Window->_hwnd != nullptr) {
            ::KillTimer(win32Window->_hwnd, GetModalLoopTimerId());
            win32Window->_inModalLoop = 0;
            win32Window->_sizeMoveResized = false;
        }
        win32Window->_eventPump = nullptr;
        _windows.erase(iter);
    }
}

sigslot::signal<NativeWindow*>& Win32EventPump::EventModalLoopTick() noexcept {
    return _eventModalLoopTick;
}

static LRESULT CALLBACK _RadrayWin32WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            auto cs = std::bit_cast<CREATESTRUCT*>(lParam);
            auto window = std::bit_cast<Win32Window*>(cs->lpCreateParams);
            ::SetProp(hWnd, RADRAY_WIN32_WINDOW_PROP, window);
            return 0;
        }
        case WM_NCHITTEST: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window && window->_inputPassthrough) {
                return HTTRANSPARENT;
            }
            return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
        case WM_MOUSEACTIVATE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window && !window->_focusOnClick) {
                return MA_NOACTIVATE;
            }
            return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
        case WM_ENTERSIZEMOVE:
        case WM_ENTERMENULOOP: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                ++window->_inModalLoop;
                if (window->_inModalLoop == 1) {
                    window->_sizeMoveResized = false;
                    RefreshClientState(window);
                    ::SetTimer(hWnd, GetModalLoopTimerId(), USER_TIMER_MINIMUM, nullptr);
                }
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == GetModalLoopTimerId()) {
                auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
                if (window != nullptr && window->_eventPump != nullptr) {
                    window->_eventPump->EventModalLoopTick()(window);
                }
                return 0;
            }
            return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
        case WM_SIZING: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                EmitWindowPosChanged(window, false);
            }
            return 0;
        }
        case WM_WINDOWPOSCHANGED: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                EmitWindowPosChanged(window, false);
            }
            return 0;
        }
        case WM_EXITSIZEMOVE:
        case WM_EXITMENULOOP: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                if (window->_inModalLoop > 0) {
                    --window->_inModalLoop;
                }
                if (window->_inModalLoop != 0) {
                    return 0;
                }

                ::KillTimer(hWnd, GetModalLoopTimerId());

                const bool sizeMoveResized = window->_sizeMoveResized;
                window->_sizeMoveResized = false;

                POINT pos{};
                int width = 0;
                int height = 0;
                if (GetClientState(hWnd, pos, width, height)) {
                    const bool hadState = window->_hasLastClientState;
                    const bool moved = !hadState || pos.x != window->_lastClientPos.x || pos.y != window->_lastClientPos.y;
                    const bool resized = !hadState || width != window->_lastClientWidth || height != window->_lastClientHeight;
                    RememberClientState(window, pos, width, height);

                    if (moved) {
                        window->_eventMoved(pos.x, pos.y);
                    }
                    if (resized || sizeMoveResized) {
                        window->_eventResized(width, height);
                    }
                }
            }
            return 0;
        }
        case WM_SIZE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                if (static_cast<UINT>(wParam) == SIZE_MINIMIZED) {
                    window->_inModalLoop = 0;
                    window->_sizeMoveResized = false;
                    ::KillTimer(hWnd, GetModalLoopTimerId());
                }
                EmitWindowPosChanged(window, false);
            }
            return 0;
        }
        case WM_MOVE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                EmitWindowPosChanged(window, false);
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
        case WM_CHAR:
        case WM_SYSCHAR: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window && wParam > 0) {
                if (IsHighSurrogate(wParam)) {
                    window->_highSurrogate = static_cast<wchar_t>(wParam);
                } else {
                    wstring utf16;
                    if (IsLowSurrogate(wParam)) {
                        if (window->_highSurrogate != 0) {
                            utf16.push_back(window->_highSurrogate);
                            utf16.push_back(static_cast<wchar_t>(wParam));
                        }
                    } else {
                        utf16.push_back(static_cast<wchar_t>(wParam));
                    }

                    window->_highSurrogate = 0;
                    EmitTextInputUtf16(window, utf16);
                }
            }
            return 0;
        }
        case WM_UNICHAR: {
            if (wParam == UNICODE_NOCHAR) {
                return TRUE;
            }
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_highSurrogate = 0;
                EmitTextInputCodepoint(window, static_cast<uint32_t>(wParam));
            }
            return 0;
        }
        case WM_SETFOCUS: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_eventFocused(true);
            }
            return 0;
        }
        case WM_KILLFOCUS: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_highSurrogate = 0;
                window->_eventFocused(false);
            }
            return 0;
        }
        case WM_DESTROY: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_hwnd = nullptr;
            }
            ::RemoveProp(hWnd, RADRAY_WIN32_WINDOW_PROP);
            return 0;
        }
        case WM_CLOSE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_closeRequested = true;
                window->_eventCloseRequested();
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
                if (!window->_trackingMouseLeave) {
                    TRACKMOUSEEVENT trackMouseEvent{};
                    trackMouseEvent.cbSize = sizeof(trackMouseEvent);
                    trackMouseEvent.dwFlags = TME_LEAVE;
                    trackMouseEvent.hwndTrack = hWnd;
                    if (::TrackMouseEvent(&trackMouseEvent) != 0) {
                        window->_trackingMouseLeave = true;
                    }
                }
                int x = static_cast<short>(LOWORD(lParam));
                int y = static_cast<short>(HIWORD(lParam));
                MouseButton btn = mapMKBUTTON(wParam);
                window->_eventTouch(x, y, btn, Action::REPEATED);
            };
            return 0;
        }
        case WM_MOUSELEAVE: {
            auto window = std::bit_cast<Win32Window*>(::GetProp(hWnd, RADRAY_WIN32_WINDOW_PROP));
            if (window) {
                window->_trackingMouseLeave = false;
                window->_eventMouseLeave();
            }
            return 0;
        }
        default: {
            return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
    }
}

static UINT _GetDpiForWindow(HWND hwnd) noexcept {
#if (WINVER >= 0x0605)
    return ::GetDpiForWindow(hwnd);
#else
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return 0;

    auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(::GetProcAddress(user32, "GetDpiForWindow"));
    if (getDpiForWindow == nullptr) return 0;

    return getDpiForWindow(hwnd);
#endif
}

static BOOL AdjustWindowRectForWindowDpi(HWND hwnd, LPRECT rect, DWORD style, DWORD exStyle) noexcept {
    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const UINT dpi = hwnd != nullptr ? _GetDpiForWindow(hwnd) : 0;
    if (user32 != nullptr && dpi > 0) {
        auto adjustWindowRectExForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
            ::GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjustWindowRectExForDpi != nullptr) {
            return adjustWindowRectExForDpi(rect, style, FALSE, exStyle, dpi);
        }
    }

    return ::AdjustWindowRectEx(rect, style, FALSE, exStyle);
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

void Win32Window::GlobalInit() noexcept {
    if (g_wndClass) {
        RADRAY_ERR_LOG("Win32Window class already registered");
        return;
    }
    EnableProcessDpiAwareness();

    HMODULE hInstance;
    {
        LPCWSTR moduleAddr = std::bit_cast<LPCWSTR>(&_RadrayWin32WindowProc);
        if (::GetModuleHandleEx(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                moduleAddr,
                &hInstance) == 0) {
            auto fmtErr = _FmtWin32LastErrMessage();
            RADRAY_ABORT("GetModuleHandleEx failed: {}", fmtErr);
            return;
        }
    }
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
        auto fmtErr = _FmtWin32LastErrMessage();
        RADRAY_ABORT("RegisterClassEx failed: {}", fmtErr);
        return;
    }
    g_wndClass = std::make_unique<WndClassRAII>(clazz, wce.hInstance, RADRAY_WIN32_WNDCLASS_NAME);
}

void Win32Window::GlobalShutdown() noexcept {
    g_wndClass.reset();
}

Nullable<unique_ptr<Win32Window>> Win32Window::Create(const Win32WindowCreateDescriptor& desc) noexcept {
    if (!g_wndClass) {
        RADRAY_ERR_LOG("Win32Window class not registered");
        return nullptr;
    }

    auto win = std::make_unique<Win32Window>();
    win->_resizable = desc.Resizable;
    win->_decorated = desc.Decorated;
    win->_showInTaskbar = desc.ShowInTaskbar;
    win->_topMost = desc.TopMost;
    win->_activateOnShow = desc.ActivateOnShow;
    win->_focusOnClick = desc.FocusOnClick;
    win->_inputPassthrough = desc.InputPassthrough;
    win->_ownerHwnd = ExtractOwnerHwnd(desc.OwnerWindow);

    DWORD style = BuildWindowStyle(win->_decorated, win->_resizable);
    if (desc.StartMaximized) {
        style |= WS_MAXIMIZE;
    }

    DWORD exStyle = BuildWindowExStyle(win->_showInTaskbar, win->_topMost);

    int x = (desc.X < 0) ? CW_USEDEFAULT : desc.X;
    int y = (desc.Y < 0) ? CW_USEDEFAULT : desc.Y;
    int w = desc.Width;
    int h = desc.Height;

    if (win->_decorated) {
        RECT rc{0, 0, w, h};
        AdjustWindowRectForWindowDpi(nullptr, &rc, style, exStyle);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }

    auto title = ToWideChar(desc.Title);
    if (!title.has_value()) {
        RADRAY_ERR_LOG("failed to convert window title to UTF-16");
        return nullptr;
    }
    HWND hwnd = ::CreateWindowEx(
        exStyle,
        g_wndClass->GetName().data(),
        title->c_str(),
        style,
        x, y,
        w, h,
        win->_ownerHwnd,
        nullptr,
        g_wndClass->GetHInstance(),
        win.get());
    const DWORD createWindowErr = hwnd == nullptr ? ::GetLastError() : ERROR_SUCCESS;
    if (!hwnd) {
        auto fmtErr = _FmtWin32ErrMessage(createWindowErr);
        RADRAY_ERR_LOG("CreateWindowEx failed: {}", fmtErr);
        return nullptr;
    }
    win->_hwnd = hwnd;
    {
        RECT rc{};
        ::GetWindowRect(hwnd, &rc);
        win->_windowedRect = rc;
    }
    RefreshClientState(win.get());
    win->_windowedStyle = style;
    win->_windowedExStyle = exStyle;

    if (desc.StartVisible) {
        int showCommand = desc.StartMaximized ? SW_MAXIMIZE : (desc.ActivateOnShow ? SW_SHOW : SW_SHOWNA);
        ::ShowWindow(hwnd, showCommand);
        ::UpdateWindow(hwnd);
    }

    if (!desc.StartMaximized && !desc.Fullscreen) {
        win->SetSize(desc.Width, desc.Height);
    }

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

bool Win32Window::ShouldClose() const noexcept {
    return _closeRequested;
}

void* Win32Window::GetNativeHandler() const noexcept {
    return _hwnd;
}

Eigen::Vector2i Win32Window::GetSize() const noexcept {
    if (_hwnd == nullptr) {
        return Eigen::Vector2i{0, 0};
    }
    RECT rc;
    ::GetClientRect(_hwnd, &rc);
    return Eigen::Vector2i{rc.right - rc.left, rc.bottom - rc.top};
}

Eigen::Vector2i Win32Window::GetPosition() const noexcept {
    if (_hwnd == nullptr) {
        return Eigen::Vector2i{0, 0};
    }
    POINT pos{0, 0};
    ::ClientToScreen(_hwnd, &pos);
    return Eigen::Vector2i{pos.x, pos.y};
}

bool Win32Window::IsMinimized() const noexcept {
    if (_hwnd == nullptr) {
        return false;
    }
    return ::IsIconic(_hwnd) != 0;
}

bool Win32Window::IsFocused() const noexcept {
    if (_hwnd == nullptr) {
        return false;
    }
    return ::GetForegroundWindow() == _hwnd;
}

void Win32Window::SetSize(int width, int height) noexcept {
    if (_hwnd == nullptr) {
        return;
    }
    if (width <= 0 || height <= 0) {
        RADRAY_ERR_LOG("invalid window size: {}x{}", width, height);
        return;
    }
    if (_isFullscreen) {
        RADRAY_ERR_LOG("cannot set size when in fullscreen mode");
        return;
    }
    RECT rc{0, 0, width, height};
    DWORD style = ::GetWindowLong(_hwnd, GWL_STYLE);
    DWORD exStyle = ::GetWindowLong(_hwnd, GWL_EXSTYLE);
    AdjustWindowRectForWindowDpi(_hwnd, &rc, style, exStyle);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    ::SetWindowPos(_hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

void Win32Window::SetPosition(int x, int y) noexcept {
    if (_hwnd == nullptr) {
        return;
    }
    if (_isFullscreen) {
        RADRAY_ERR_LOG("cannot set position when in fullscreen mode");
        return;
    }
    RECT rc{x, y, x, y};
    DWORD style = ::GetWindowLong(_hwnd, GWL_STYLE);
    DWORD exStyle = ::GetWindowLong(_hwnd, GWL_EXSTYLE);
    AdjustWindowRectForWindowDpi(_hwnd, &rc, style, exStyle);
    ::SetWindowPos(_hwnd, nullptr, rc.left, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

void Win32Window::SetTitle(std::string_view title) noexcept {
    if (_hwnd == nullptr) {
        return;
    }
    auto wideTitle = ToWideChar(title);
    if (!wideTitle.has_value()) {
        RADRAY_ERR_LOG("failed to convert window title to UTF-16");
        return;
    }
    ::DefWindowProcW(_hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(wideTitle->c_str()));
}

void Win32Window::Show() noexcept {
    this->Show(_activateOnShow ? NativeWindowShowMode::Default : NativeWindowShowMode::NoActivate);
}

void Win32Window::Show(NativeWindowShowMode mode) noexcept {
    if (_hwnd == nullptr) {
        return;
    }
    ::ShowWindow(_hwnd, mode == NativeWindowShowMode::NoActivate ? SW_SHOWNA : SW_SHOW);
    ::UpdateWindow(_hwnd);
}

void Win32Window::Focus() noexcept {
    if (_hwnd == nullptr) {
        return;
    }
    ::BringWindowToTop(_hwnd);
    ::SetForegroundWindow(_hwnd);
    ::SetFocus(_hwnd);
}

void Win32Window::SetAlpha(float alpha) noexcept {
    if (_hwnd == nullptr) {
        return;
    }
    alpha = Clamp(alpha, 0.0f, 1.0f);
    if (alpha < 1.0f) {
        const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(_hwnd, GWL_EXSTYLE)) | WS_EX_LAYERED;
        ::SetWindowLongPtrW(_hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle));
        ::SetLayeredWindowAttributes(_hwnd, 0, static_cast<BYTE>(255.0f * alpha), LWA_ALPHA);
    } else {
        const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(_hwnd, GWL_EXSTYLE)) & ~WS_EX_LAYERED;
        ::SetWindowLongPtrW(_hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle));
    }
}

void Win32Window::SetOwner(Nullable<NativeWindow*> owner) noexcept {
    if (_hwnd == nullptr) {
        return;
    }

    HWND ownerHwnd = ExtractOwnerHwnd(owner);
    if (ownerHwnd == _hwnd) {
        RADRAY_ERR_LOG("Win32Window cannot own itself");
        return;
    }

    _ownerHwnd = ownerHwnd;
    ::SetWindowLongPtrW(_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(_ownerHwnd));
}

void Win32Window::SetDecorated(bool value) noexcept {
    if (_decorated == value) {
        return;
    }
    _decorated = value;
    ApplyWindowStyles(this);
}

void Win32Window::SetShowInTaskbar(bool value) noexcept {
    if (_showInTaskbar == value) {
        return;
    }
    _showInTaskbar = value;
    ApplyWindowStyles(this);
}

void Win32Window::SetTopMost(bool value) noexcept {
    if (_topMost == value) {
        return;
    }
    _topMost = value;
    ApplyWindowStyles(this);
}

void Win32Window::SetFocusOnClick(bool value) noexcept {
    _focusOnClick = value;
}

void Win32Window::SetInputPassthrough(bool value) noexcept {
    _inputPassthrough = value;
}

float Win32Window::GetDpiScale() const noexcept {
    if (_hwnd == nullptr) {
        return 1.0f;
    }
    const UINT windowDpi = _GetDpiForWindow(_hwnd);
    if (windowDpi > 0) {
        return static_cast<float>(windowDpi) / 96.0f;
    }

    HDC dc = ::GetDC(_hwnd);
    if (dc == nullptr) return 1.0f;
    const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    ::ReleaseDC(_hwnd, dc);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

Eigen::Vector2i Win32Window::ClientToScreen(Eigen::Vector2i pos) const noexcept {
    if (_hwnd == nullptr) {
        return pos;
    }
    POINT point{pos.x(), pos.y()};
    ::ClientToScreen(_hwnd, &point);
    return Eigen::Vector2i{point.x, point.y};
}

Eigen::Vector2i Win32Window::ScreenToClient(Eigen::Vector2i pos) const noexcept {
    if (_hwnd == nullptr) {
        return pos;
    }
    POINT point{pos.x(), pos.y()};
    ::ScreenToClient(_hwnd, &point);
    return Eigen::Vector2i{point.x, point.y};
}

sigslot::signal<int, int>& Win32Window::EventResized() noexcept {
    return _eventResized;
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

sigslot::signal<std::string_view>& Win32Window::EventTextInput() noexcept {
    return _eventTextInput;
}

sigslot::signal<bool>& Win32Window::EventFocused() noexcept {
    return _eventFocused;
}

sigslot::signal<>& Win32Window::EventCloseRequested() noexcept {
    return _eventCloseRequested;
}

sigslot::signal<int, int>& Win32Window::EventMoved() noexcept {
    return _eventMoved;
}

sigslot::signal<>& Win32Window::EventMouseLeave() noexcept {
    return _eventMouseLeave;
}

bool Win32Window::EnterFullscreen() {
    if (_isFullscreen) return false;
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
    if (!_isFullscreen) return false;
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
