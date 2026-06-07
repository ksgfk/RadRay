#include <radray/window/native_window.h>

#include <radray/logger.h>
#include <radray/utility.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/window/win32/win32_window.h>
#endif
#if defined(RADRAY_PLATFORM_MACOS)
#include <radray/window/cocoa/cocoa_window.h>
#endif

namespace radray {

void NativeWindow::GlobalInit() noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    Win32Window::GlobalInit();
#elif defined(RADRAY_PLATFORM_MACOS)
    CocoaWindow::GlobalInit();
#endif
}

void NativeWindow::GlobalShutdown() noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    Win32Window::GlobalShutdown();
#elif defined(RADRAY_PLATFORM_MACOS)
    CocoaWindow::GlobalShutdown();
#endif
}

Nullable<unique_ptr<NativeWindow>> NativeWindow::Create(const NativeWindowCreateDescriptor& desc) noexcept {
    return std::visit(
        [](const auto& specificDesc) -> Nullable<unique_ptr<NativeWindow>> {
            using T = std::decay_t<decltype(specificDesc)>;
            if constexpr (std::is_same_v<T, Win32WindowCreateDescriptor>) {
#if defined(RADRAY_PLATFORM_WINDOWS)
                return Win32Window::Create(specificDesc);
#else
                RADRAY_ERR_LOG("Win32Window disable");
                return nullptr;
#endif
            } else if constexpr (std::is_same_v<T, CocoaWindowCreateDescriptor>) {
#if defined(RADRAY_PLATFORM_MACOS)
                return CocoaWindow::Create(specificDesc);
#else
                RADRAY_ERR_LOG("CocoaWindow disable");
                return nullptr;
#endif
            }
        },
        desc);
}

Nullable<unique_ptr<NativeEventPump>> NativeEventPump::Create(NativeWindowType type) noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    if (type == NativeWindowType::Win32HWND) {
        return make_unique<Win32EventPump>();
    } else {
        RADRAY_ERR_LOG("Unsupported NativeWindowType {}", type);
        return nullptr;
    }
#elif defined(RADRAY_PLATFORM_MACOS)
    if (type == NativeWindowType::CocoaNSWindow) {
        return make_unique<CocoaEventPump>();
    } else {
        RADRAY_ERR_LOG("Unsupported NativeWindowType {}", type);
        return nullptr;
    }
#else
    RADRAY_ERR_LOG("NativeEventPump disable");
    return nullptr;
#endif
}

std::string_view format_as(NativeWindowType v) noexcept {
    switch (v) {
        case NativeWindowType::UNKNOWN: return "UNKNOWN";
        case NativeWindowType::Win32HWND: return "Win32HWND";
        case NativeWindowType::CocoaNSWindow: return "CocoaNSWindow";
    }
    Unreachable();
}

}  // namespace radray
