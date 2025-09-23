#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <imgui.h>

namespace radray {

bool GlobalInitDearImGui();

void GlobalTerminateDearImGui();

}  // namespace radray

#endif
