#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <imgui.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <imgui_impl_win32.h>
#endif

#include <radray/window/native_window.h>

namespace radray {

bool GlobalInitDearImGui();

void GlobalTerminateDearImGui();

}  // namespace radray

#endif
