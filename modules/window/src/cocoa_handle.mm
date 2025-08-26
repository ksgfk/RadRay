#include "cocoa_handle.h"

#import <Cocoa/Cocoa.h>

#define GLFW_NATIVE_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <radray/logger.h>

const void *RadrayGetCocoaHandlerFromGlfw(GLFWwindow *glfw) {
  id nsWin = glfwGetCocoaWindow(glfw);
  return CFBridgingRetain(nsWin);
}

void RadrayReleaseCocoaHandlerFromGlfw(GLFWwindow *glfw, const void *ptr) {
  id nsWin = glfwGetCocoaWindow(glfw);
  id extRef = CFBridgingRelease(ptr);
  if (nsWin != extRef) {
    RADRAY_ABORT(
        "cannod use RadrayReleaseCocoaHandlerFromGlfw on unknown ptr {}", ptr);
  }
}
