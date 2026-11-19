#include <radray/window/native_window.h>

#include <radray/enum_flags.h>
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

void NativeEventPump::PumpNativeEvents(uint32_t) noexcept { RADRAY_ABORT("Split event delivery is unsupported on this platform"); }
void NativeEventPump::DispatchPendingEvents(const std::function<bool()>&) noexcept { RADRAY_ABORT("Deferred events are unsupported on this platform"); }
void NativeEventPump::StopEventDelivery() noexcept { RADRAY_ABORT("Deferred events are unsupported on this platform"); }
void NativeEventPump::DispatchCloseRequest(NativeWindow*) noexcept { RADRAY_ABORT("Deferred events are unsupported on this platform"); }
void NativeEventPump::WaitEvents(uint32_t, std::span<void* const>) noexcept { RADRAY_ABORT("Event waiting is unsupported on this platform"); }
void NativeEventPump::Wake() noexcept { RADRAY_ABORT("Event waking is unsupported on this platform"); }
bool NativeEventPump::QuitRequested() const noexcept { return false; }

Nullable<unique_ptr<NativeEventPump>> NativeEventPump::Create(NativeWindowType type, NativeEventDelivery delivery) noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    if (type == NativeWindowType::Win32HWND) {
        return make_unique<Win32EventPump>(delivery);
    } else {
        RADRAY_ERR_LOG("Unsupported NativeWindowType {}", type);
        return nullptr;
    }
#elif defined(RADRAY_PLATFORM_MACOS)
    if (delivery != NativeEventDelivery::Immediate) {
        RADRAY_ERR_LOG("Cocoa deferred event delivery is not implemented");
        return nullptr;
    }
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
    return EnumNameOr(v);
}

bool NativeWindow::IsVisible() const noexcept {
    return true;
}

NativeDesktopCapabilities NativeWindow::GetDesktopCapabilities() const noexcept {
    return {};
}

bool NativeWindow::SetCursor(NativeCursor) noexcept {
    return false;
}

std::optional<Eigen::Vector2i> NativeWindow::GetDesktopMousePosition() const noexcept {
    return {};
}

bool NativeWindow::SetDesktopMousePosition(Eigen::Vector2i) noexcept {
    return false;
}

Nullable<NativeWindow*> NativeWindow::GetHoveredWindow() const noexcept {
    return nullptr;
}

vector<NativeMonitor> NativeWindow::GetMonitors() const {
    return {};
}

std::optional<string> NativeWindow::GetClipboardText() const {
    return {};
}

bool NativeWindow::SetClipboardText(std::string_view) {
    return false;
}

bool NativeWindow::SetImePosition(Eigen::Vector2i, int, bool) noexcept {
    return false;
}

sigslot::signal<float, float>& NativeWindow::EventScroll() noexcept {
    return _eventScroll;
}

sigslot::signal<>& NativeWindow::EventCaptureLost() noexcept {
    return _eventCaptureLost;
}

sigslot::signal<>& NativeWindow::EventDisplayChanged() noexcept {
    return _eventDisplayChanged;
}

sigslot::signal<>& NativeWindow::EventBeforeSurfaceChange() noexcept {
    return _eventBeforeSurfaceChange;
}

}  // namespace radray
