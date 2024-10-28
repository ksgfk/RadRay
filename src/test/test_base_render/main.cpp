#include <stdexcept>
#include <thread>
#include <chrono>

#include <radray/utility.h>
#include <radray/basic_math.h>
#include <radray/window/glfw_window.h>
#include <radray/render/device.h>

using namespace radray;
using namespace radray::render;

int main() {
    auto device = CreateDevice(D3D12DeviceDescriptor{}).value();
    return 0;
}
