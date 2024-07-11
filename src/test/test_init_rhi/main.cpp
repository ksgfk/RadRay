#include <stdexcept>
#include <thread>
#include <chrono>

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
        auto fence = device->CreateFence();
        auto sch = device->CreateSwapChain(
            {glfw.GetNativeHandle(),
             1280, 720,
             3,
             true},
            cmdQueue.Handle);
        uint64_t fenceValue = 0;
        auto last = std::chrono::system_clock::now();
        std::array<float, 4> clearColor{0.5f, 0.2f, 0.1f, 1.0f};
        while (!glfw.ShouldClose()) {
            window::GlobalPollEventsGlfw();
            auto now = std::chrono::system_clock::now();
            float delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() / 1000.0f;
            for (auto&& i : clearColor) {
                i += delta * 0.25f;
                i -= std::floor(i);
            }
            clearColor[3] = 1.0f;
            last = now;
            if (fenceValue > 0) {
                device->Synchronize(fence, fenceValue);
            }
            device->StartFrame(cmdQueue, sch);
            {
                rhi::CommandList list{};
                list.Add(rhi::ClearRenderTargetCommand{sch, clearColor});
                device->DispatchCommand(cmdQueue, std::move(list));
            }
            device->Signal(fence, cmdQueue, ++fenceValue);
            device->FinishFrame(cmdQueue, sch);
            std::this_thread::yield();
        }
        device->DestroySwapChain(sch);
        device->DestroyFence(fence);
        device->DestroyCommandQueue(cmdQueue);
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("exception {}", e.what());
    }
    window::GlobalTerminateGlfw();
    return 0;
}
