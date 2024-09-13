#include <stdexcept>
#include <thread>
#include <chrono>

#include <radray/window/glfw_window.h>
#include <radray/basic_math.h>
#include <radray/rhi/device_interface.h>

constexpr int FRAME_COUNT = 3;

radray::shared_ptr<radray::window::GlfwWindow> glfw;
radray::rhi::DeviceInterface* device;
RadrayCommandQueue cmdQueue;
RadrayCommandAllocator cmdAlloc;
RadrayCommandList cmdList;
RadraySwapChain swapchain;
RadrayFence fence;
RadrayTexture nowRt = RADRAY_RHI_EMPTY_RES(RadrayTexture);

void start() {
    radray::window::GlobalInitGlfw();
    glfw = radray::make_shared<radray::window::GlfwWindow>("init rhi", 1280, 720);
#ifdef RADRAY_ENABLE_D3D12
    RadrayDeviceDescriptorD3D12 desc{
        .AdapterIndex = RADRAY_RHI_AUTO_SELECT_DEVICE,
        .IsEnableDebugLayer = true};
    device = reinterpret_cast<radray::rhi::DeviceInterface*>(RadrayCreateDeviceD3D12(&desc));
#endif
#ifdef RADRAY_ENABLE_METAL
    RadrayDeviceDescriptorMetal desc{
        .DeviceIndex = RADRAY_RHI_AUTO_SELECT_DEVICE};
    device = reinterpret_cast<radray::rhi::DeviceInterface*>(RadrayCreateDeviceMetal(&desc));
#endif
    if (device == nullptr) {
        throw std::runtime_error{"cannot create device"};
    }
    cmdQueue = device->CreateCommandQueue(RADRAY_QUEUE_TYPE_DIRECT);
    RadraySwapChainDescriptor chainDesc{
        cmdQueue,
        glfw->GetNativeHandle(),
        static_cast<uint32_t>(glfw->GetSize().x()),
        static_cast<uint32_t>(glfw->GetSize().y()),
        FRAME_COUNT,
        RADRAY_FORMAT_BGRA8_UNORM,
        true};
    cmdAlloc = device->CreateCommandAllocator(cmdQueue);
    cmdList = device->CreateCommandList(cmdAlloc);
    swapchain = device->CreateSwapChain(chainDesc);
    fence = device->CreateFence();
}

void update() {
    radray::vector<RadrayTextureView> lastTv;
    RadrayClearValue rtClear{.R = 0, .G = 0, .B = 0, .A = 1};
    auto i = 0;
    auto mod = 1;
    auto last = std::chrono::system_clock::now();
    while (!glfw->ShouldClose()) {
        radray::window::GlobalPollEventsGlfw();
        {
            auto now = std::chrono::system_clock::now();
            float delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() / 1000.0f;
            float v = rtClear.Color[i];
            v += mod * delta * 2.0f;
            if (mod == 1 && v >= 1.0f) {
                v = 1;
                mod = -1;
            }
            if (mod == -1 && v <= 0.0f) {
                v = 0;
                mod = 1;
                rtClear.Color[i] = 0;
                i = (i + 1) % 3;
            }
            rtClear.Color[i] = v;
            last = now;
        }
        RadrayFence fences[]{fence};
        device->WaitFences(fences);
        for (auto&& i : lastTv) {
            device->DestroyTextureView(i);
        }
        lastTv.clear();
        nowRt = device->AcquireNextRenderTarget(swapchain, nowRt);
        device->ResetCommandAllocator(cmdAlloc);
        device->BeginCommandList(cmdList);
        auto rtv = device->CreateTextureView(
            {nowRt,
             RADRAY_FORMAT_BGRA8_UNORM,
             RADRAY_RESOURCE_TYPE_RENDER_TARGET,
             RADRAY_TEXTURE_DIM_2D,
             0, 1, 0, 1});
        lastTv.emplace_back(rtv);
        RadrayColorAttachment colorAttach{
            rtv,
            RADRAY_LOAD_ACTION_CLEAR,
            RADRAY_STORE_ACTION_STORE,
            rtClear};
        auto colorPass = device->BeginRenderPass(
            {"Color Pass",
             cmdList,
             &colorAttach,
             nullptr,
             1});
        device->EndRenderPass(colorPass);
        device->EndCommandList(cmdList);
        device->SubmitQueue({cmdQueue, &cmdList, 1, fence});
        device->Present(swapchain, nowRt);
        std::this_thread::yield();
    }
}

void destroy() {
    device->WaitQueue(cmdQueue);
    device->DestroyFence(fence);
    device->DestroyTexture(nowRt);
    device->DestroySwapChian(swapchain);
    device->DestroyCommandList(cmdList);
    device->DestroyCommandAllocator(cmdAlloc);
    device->DestroyCommandQueue(cmdQueue);
    RadrayReleaseDevice(device);
    radray::window::GlobalTerminateGlfw();
}

int main() {
    try {
        start();
        update();
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("{}", e.what());
    } catch (...) {
        RADRAY_ERR_LOG("crital error");
    }
    destroy();
    return 0;
}
