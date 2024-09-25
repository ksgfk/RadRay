#include <stdexcept>
#include <thread>
#include <chrono>

#include <radray/utility.h>
#include <radray/basic_math.h>
#include <radray/window/glfw_window.h>
#include <radray/rhi/device_interface.h>

constexpr int FRAME_COUNT = 3;

radray::shared_ptr<radray::window::GlfwWindow> glfw;
radray::rhi::DeviceInterface* device;
RadrayCommandQueue cmdQueue;
RadrayCommandAllocator cmdAlloc;
RadrayCommandList cmdList;
RadraySwapChain swapchain;
RadrayFence fence;
radray::string shaderStr;
RadrayShader vs;
RadrayShader ps;
RadrayRootSignature sig;

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
#ifdef RADRAY_ENABLE_METAL
        RADRAY_FORMAT_BGRA8_UNORM,
#else
        RADRAY_FORMAT_RGBA8_UNORM,
#endif
        true};
    cmdAlloc = device->CreateCommandAllocator(cmdQueue);
    cmdList = device->CreateCommandList(cmdAlloc);
    swapchain = device->CreateSwapChain(chainDesc);
    fence = device->CreateFence();

    {
        auto path = std::filesystem::path{"shaders"} / RADRAY_APPNAME / "color.hlsl";
        auto tmp = radray::ReadText(path);
        if (!tmp.has_value()) {
            auto tips = radray::format("cannot read {}", path.generic_string());
            throw std::runtime_error{tips.c_str()};
        }
        shaderStr = tmp.value();
    }
    // vs = device->CompileShader(
    //     {"color",
    //      shaderStr.data(),
    //      shaderStr.size(),
    //      "VSMain",
    //      RADRAY_SHADER_STAGE_VERTEX,
    //      61,
    //      nullptr,
    //      0,
    //      nullptr,
    //      0,
    //      false});
    // ps = device->CompileShader(
    //     {"color",
    //      shaderStr.data(),
    //      shaderStr.size(),
    //      "PSMain",
    //      RADRAY_SHADER_STAGE_PIXEL,
    //      61,
    //      nullptr,
    //      0,
    //      nullptr,
    //      0,
    //      false});
    {
        // test
        std::filesystem::path fullpath{"shader_lib"};
        std::string root = fullpath.generic_string();
        auto data = root.c_str();
        auto tvsopt = radray::ReadText(fullpath / "DefaultVS.hlsl").value();
        auto tpsopt = radray::ReadText(fullpath / "DefaultPS.hlsl").value();
        RadrayShader tvs = device->CompileShader(
            {"DefaultVS",
             tvsopt.data(),
             tvsopt.size(),
             "main",
             RADRAY_SHADER_STAGE_VERTEX,
             61,
             nullptr,
             0,
             &data,
             1,
             false});
        RadrayShader tps = device->CompileShader(
            {"DefaultPS",
             tpsopt.data(),
             tpsopt.size(),
             "main",
             RADRAY_SHADER_STAGE_PIXEL,
             61,
             nullptr,
             0,
             &data,
             1,
             false});

        // RadrayShader shaders[] = {tvs, tps};
        // RadrayRootSignature tsig = device->CreateRootSignature(
        //     {shaders,
        //      2,
        //      nullptr,
        //      nullptr,
        //      0});

        device->DestroyShader(tvs);
        device->DestroyShader(tps);
        // device->DestroyRootSignature(tsig);
    }
    // {
    //     RadrayShader shaders[] = {vs, ps};
    //     sig = device->CreateRootSignature(
    //         {shaders,
    //          2,
    //          nullptr,
    //          nullptr,
    //          0});
    // }
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
        RadrayTexture nowRt = device->AcquireNextRenderTarget(swapchain);
        device->ResetCommandAllocator(cmdAlloc);
        device->BeginCommandList(cmdList);
        auto rtv = device->CreateTextureView(
            {nowRt,
#ifdef RADRAY_ENABLE_METAL
             RADRAY_FORMAT_BGRA8_UNORM,
#else
             RADRAY_FORMAT_RGBA8_UNORM,
#endif
             RADRAY_RESOURCE_TYPE_RENDER_TARGET,
             RADRAY_TEXTURE_DIM_2D,
             0, 1, 0, 1});
        lastTv.emplace_back(rtv);
        RadrayTextureBarrier bbToRt{
            nowRt,
            RADRAY_RESOURCE_STATE_PRESENT,
            RADRAY_RESOURCE_STATE_RENDER_TARGET,
            false,
            0, 0};
        device->ResourceBarriers(cmdList, {&bbToRt, 1});
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
        RadrayTextureBarrier rtToBb{
            nowRt,
            RADRAY_RESOURCE_STATE_RENDER_TARGET,
            RADRAY_RESOURCE_STATE_PRESENT,
            false,
            0, 0};
        device->ResourceBarriers(cmdList, {&rtToBb, 1});
        device->EndCommandList(cmdList);
        device->SubmitQueue({cmdQueue, &cmdList, 1, fence});
        device->Present(swapchain, nowRt);
        std::this_thread::yield();
    }
}

void destroy() {
    device->WaitQueue(cmdQueue);
    // device->DestroyShader(vs);
    // device->DestroyShader(ps);
    device->DestroyFence(fence);
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
