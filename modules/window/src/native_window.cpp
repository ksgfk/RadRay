#include <radray/window/native_window.h>

#include <radray/logger.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include "win32/win32_window.h"
#endif

#if defined(RADRAY_PLATFORM_MACOS)
#include "cocoa/cocoa_window_cpp.h"
#endif

namespace radray {

Nullable<unique_ptr<NativeWindow>> CreateNativeWindow(const NativeWindowCreateDescriptor& desc) noexcept {
    return std::visit(
        [](const auto& specificDesc) -> Nullable<unique_ptr<NativeWindow>> {
            using T = std::decay_t<decltype(specificDesc)>;
            if constexpr (std::is_same_v<T, Win32WindowCreateDescriptor>) {
#if defined(RADRAY_PLATFORM_WINDOWS)
                return CreateWin32Window(specificDesc);
#else
                RADRAY_ERR_LOG("Win32Window disable");
                return nullptr;
#endif
            } else if constexpr (std::is_same_v<T, CocoaWindowCreateDescriptor>) {
#if defined(RADRAY_PLATFORM_MACOS)
                return CreateCocoaWindow(specificDesc);
#else
                RADRAY_ERR_LOG("CocoaWindow disable");
                return nullptr;
#endif
            }
        },
        desc);
}

}  // namespace radray
