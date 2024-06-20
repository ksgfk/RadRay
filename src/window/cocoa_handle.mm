#include "cocoa_handle.h"

#import <Cocoa/Cocoa.h>

#define GLFW_NATIVE_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

static_assert(sizeof(id) == sizeof(size_t), "id is not size_t");

namespace radray::window {

size_t GetCocoaHandler(GLFWwindow *glfw) {
  id h = glfwGetCocoaWindow(glfw);
  return reinterpret_cast<size_t>(h);
}

} // namespace radray::window
