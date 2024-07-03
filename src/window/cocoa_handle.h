#pragma once

#include <cstddef>

struct GLFWwindow;

extern "C" size_t RadrayGetCocoaHandlerFromGlfw(GLFWwindow* glfw);

namespace radray::window {}
