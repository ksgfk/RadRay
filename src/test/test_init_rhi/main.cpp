#include <stdexcept>
#include <thread>

#include <radray/window/glfw_window.h>
#include <radray/basic_math.h>
#include <radray/rhi/device_interface.h>

using namespace radray;

int main() {
    {
        rhi::SupportApiArray api{};
        rhi::GetSupportApi(api);
        for (size_t i = 0; i < api.size(); i++) {
            if (api[i]) {
                RADRAY_INFO_LOG("support api: {}", (rhi::ApiType)i);
            }
        }
    }
    window::GlobalInitGlfw();
    try {
        window::GlfwWindow glfw{"Test RHI", 1280, 720};
#ifdef RADRAY_ENABLE_D3D12
        auto device = rhi::CreateDeviceD3D12({std::nullopt, true});
#endif
#ifdef RADRAY_ENABLE_METAL
        auto device = rhi::CreateDeviceMetal({});
#endif
        if (device == nullptr) {
            throw std::runtime_error{"cannot create device"};
        }
        auto cmdQueue = device->CreateCommandQueue(rhi::CommandListType::Graphics);
        auto sch = device->CreateSwapChain(
            {glfw.GetNativeHandle(),
             1280, 720,
             3,
             false},
            cmdQueue.Handle);
        while (!glfw.ShouldClose()) {
            window::GlobalPollEventsGlfw();
            std::this_thread::yield();
        }
        device->DestroySwapChain(sch);
        device->DestroyCommandQueue(cmdQueue);
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("exception {}", e.what());
    }
    window::GlobalTerminateGlfw();
    return 0;
}
