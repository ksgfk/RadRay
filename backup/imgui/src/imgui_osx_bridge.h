#pragma once

#ifdef RADRAY_PLATFORM_MACOS

namespace radray {

bool ImGuiOSXBridge_Init(void* nsView);
void ImGuiOSXBridge_Shutdown();
void ImGuiOSXBridge_NewFrame(void* nsView);

}  // namespace radray

#endif
