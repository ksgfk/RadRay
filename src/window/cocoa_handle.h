#pragma once

#include <cstddef>

struct GLFWwindow;

extern "C" const void* RadrayGetCocoaHandlerFromGlfw(GLFWwindow* glfw);
extern "C" void RadrayReleaseCocoaHandlerFromGlfw(GLFWwindow* glfw, const void* ptr);

namespace radray::window {}
