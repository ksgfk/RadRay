#include "imgui_osx_bridge.h"

#ifdef RADRAY_PLATFORM_MACOS

#import <imgui_impl_osx.h>
#import <Cocoa/Cocoa.h>

namespace radray {

bool ImGuiOSXBridge_Init(void* nsView) {
    return ImGui_ImplOSX_Init((__bridge NSView*)nsView);
}

void ImGuiOSXBridge_Shutdown() {
    ImGui_ImplOSX_Shutdown();
}

void ImGuiOSXBridge_NewFrame(void* nsView) {
    ImGui_ImplOSX_NewFrame((__bridge NSView*)nsView);
}

}  // namespace radray

#endif
