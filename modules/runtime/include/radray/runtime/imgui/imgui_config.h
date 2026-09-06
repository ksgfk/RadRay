#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <radray/logger.h>

#define IM_ASSERT(expression)                                                       \
    do {                                                                            \
        if (!(expression)) RADRAY_ABORT("ImGui assertion failed: {}", #expression); \
    } while (false)
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_WIN32_FUNCTIONS
#define IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_ALLOCATORS
#define IMGUI_USE_WCHAR32

#endif  // RADRAY_ENABLE_IMGUI
