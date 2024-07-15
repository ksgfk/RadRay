#include <stdexcept>
#include <thread>
#include <chrono>

#include <radray/window/glfw_window.h>
#include <radray/basic_math.h>
#include <radray/rhi/device_interface.h>

using namespace radray;

constexpr int FRAME_COUNT = 3;

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
             FRAME_COUNT,
             true},
            cmdQueue.Handle);
        uint64_t fenceValue = 0;
        std::array<uint64_t, FRAME_COUNT> frameRes{};
        for (uint64_t& i : frameRes) {
            i = 0;
        }
        uint64_t frameValue = 0;
        auto last = std::chrono::system_clock::now();
        float fpsTime = 0;
        uint32_t fpsCount = 0;
        std::array<float, 4> clearColor{0.5f, 0.2f, 0.1f, 1.0f};
        while (!glfw.ShouldClose()) {
            window::GlobalPollEventsGlfw();
            {  // update
                auto now = std::chrono::system_clock::now();
                float delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() / 1000.0f;
                for (auto&& i : clearColor) {
                    i += delta * 0.25f;
                    i -= std::floor(i);
                }
                clearColor[3] = 1.0f;
                fpsTime += delta;
                fpsCount++;
                if (fpsTime >= 1) {
                    RADRAY_INFO_LOG("fps {}", fpsCount / fpsTime);
                    fpsTime = 0;
                    fpsCount = 0;
                }
                last = now;
            }
            {  // render
                auto renderFrame = frameValue;
                auto showFrame = (renderFrame + 1) % FRAME_COUNT;
                auto idleFrame = (showFrame + 1) % FRAME_COUNT;
                if (frameRes[idleFrame] > 0) {
                    device->Synchronize(fence, frameRes[idleFrame]);
                }
                device->StartFrame(cmdQueue, sch);
                {
                    rhi::CommandList list{};
                    list.Add(rhi::ClearRenderTargetCommand{sch, clearColor});
                    device->DispatchCommand(cmdQueue, std::move(list));
                }
                frameRes[idleFrame] = ++fenceValue;
                device->Signal(fence, cmdQueue, frameRes[idleFrame]);
                device->FinishFrame(cmdQueue, sch);
                frameValue = (frameValue + 1) % FRAME_COUNT;
            }
            std::this_thread::yield();
        }
        for (uint64_t i : frameRes) {
            device->Synchronize(fence, i);
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
