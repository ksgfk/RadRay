#include <thread>

#include <radray/logger.h>
#include <radray/stopwatch.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/window/glfw_window.h>

#include "../../modules/render/src/d3d12/d3d12_impl.h"

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int RT_COUNT = 3;
const char* RADRAY_APPNAME = "hello_world_d3d12";

unique_ptr<GlfwWindow> glfw;
shared_ptr<d3d12::DeviceD3D12> device;
d3d12::CmdQueueD3D12* cmdQueue;
shared_ptr<d3d12::SwapChainD3D12> swapchain;

void Init() {
    GlobalInitGlfw();
    glfw = make_unique<GlfwWindow>(RADRAY_APPNAME, WIN_WIDTH, WIN_HEIGHT, false, false);
    device = d3d12::CreateDevice({std::nullopt, true, false}).Unwrap();
    cmdQueue = static_cast<d3d12::CmdQueueD3D12*>(device->GetCommandQueue(QueueType::Direct, 0).Unwrap());
    swapchain = std::static_pointer_cast<d3d12::SwapChainD3D12>(device->CreateSwapChain({cmdQueue, glfw->GetNativeHandle(), WIN_WIDTH, WIN_HEIGHT, RT_COUNT, TextureFormat::RGBA8_UNORM, false}).Unwrap());
}

bool Update() {
    GlobalPollEventsGlfw();
    bool isClose = glfw->ShouldClose();

    swapchain->AcquireNext();
    swapchain->Present();

    return !isClose;
}

void End() {
    swapchain = nullptr;
    cmdQueue = nullptr;
    device.reset();
    GlobalTerminateGlfw();
}

int main() {
    Init();
    while (Update()) {
        std::this_thread::yield();
    }
    End();
    return 0;
}
