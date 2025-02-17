#include <thread>

#include <radray/logger.h>
#include <radray/utility.h>

#include <radray/window/glfw_window.h>

#include <radray/render/device.h>

using namespace radray;

constexpr auto WIN_WIDTH = 1280;
constexpr auto WIN_HEIGHT = 720;

class TestApp1 {
public:
    TestApp1() {
        RADRAY_INFO_LOG("{} start", RADRAY_APPNAME);
        window::GlobalInitGlfw();
    }

    ~TestApp1() {
        _dxc = nullptr;
        _cmdBuffer = nullptr;
        _cmdPool = nullptr;
        _swapchain = nullptr;
        _device = nullptr;
        _window = nullptr;
        window::GlobalTerminateGlfw();
        RADRAY_INFO_LOG("{} end", RADRAY_APPNAME);
    }

    void ShowWindow() {
        _window =
            make_unique<window::GlfwWindow>(RADRAY_APPNAME, WIN_WIDTH, WIN_HEIGHT);
        RADRAY_INFO_LOG("show window");
    }

    void InitGraphics() {
        RADRAY_INFO_LOG("start init graphics");
        render::D3D12DeviceDescriptor d3d12Desc{
            std::nullopt,
            true,
            false};
        _device = CreateDevice(d3d12Desc).Unwrap();
        render::CommandQueue* cmdQueue =
            _device->GetCommandQueue(render::QueueType::Direct, 0).Unwrap();
        _cmdPool = _device->CreateCommandPool(cmdQueue).Unwrap();
        _cmdBuffer = _device->CreateCommandBuffer(_cmdPool.get()).Unwrap();
        _dxc = render::CreateDxc().Unwrap();
        _swapchain = _device->CreateSwapChain(
                                cmdQueue,
                                _window->GetNativeHandle(),
                                WIN_WIDTH,
                                WIN_HEIGHT,
                                2,
                                render::TextureFormat::RGBA8_UNORM,
                                true)
                         .Unwrap();
        RADRAY_INFO_LOG("end init graphics");
    }

    void Start() {
        ShowWindow();
        InitGraphics();
    }

    void Update() {
        while (true) {
            window::GlobalPollEventsGlfw();
            if (_window->ShouldClose()) {
                break;
            }
            std::this_thread::yield();
        }
    }

    unique_ptr<window::GlfwWindow> _window;

    shared_ptr<render::Device> _device;
    shared_ptr<render::CommandPool> _cmdPool;
    shared_ptr<render::CommandBuffer> _cmdBuffer;
    shared_ptr<render::SwapChain> _swapchain;
    shared_ptr<render::Dxc> _dxc;
};

int main() {
    TestApp1 app{};
    app.Start();
    app.Update();
    return 0;
}
