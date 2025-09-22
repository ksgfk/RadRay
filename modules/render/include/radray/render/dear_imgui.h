#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <imgui.h>

namespace radray::render {

bool GlobalInitDearImGui();

void GlobalTerminateDearImGui();

}  // namespace radray::render

#endif
