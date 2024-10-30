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
    auto queue = device->GetCommandQueue(QueueType::Direct, 0).value();
    auto same = device->GetCommandQueue(QueueType::Direct, 0).value();
    if (queue != same) {
        throw std::exception{};
    }
    return 0;
}
