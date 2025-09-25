#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <imgui.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <imgui_impl_win32.h>
#endif

#include <radray/window/native_window.h>

namespace radray {

class ImGuiPlatformInitDescriptor {
public:
    PlatformId Platform;

    void* Hwnd;
};

bool InitImGui();
bool InitPlatformImGui(const ImGuiPlatformInitDescriptor& desc);
void TerminatePlatformImGui();
void TerminateImGui();

Nullable<Win32WNDPROC> GetWin32WNDPROCImGui() noexcept;

}  // namespace radray

#endif
