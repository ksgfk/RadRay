#include <thread>
#include <mutex>
#include <stdexcept>

#include <radray/types.h>
#include <radray/logger.h>
#include <radray/platform.h>
#include <radray/window/native_window.h>
#include <radray/render/common.h>
#include <radray/imgui/dear_imgui.h>

const char* RADRAY_APP_NAME = "Hello Dear ImGui";

using namespace radray;
using namespace radray::render;

class HelloImguiException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    template <typename... Args>
    explicit HelloImguiException(fmt::format_string<Args...> fmt, Args&&... args)
        : _msg(radray::format(fmt, std::forward<Args>(args)...)) {}
    ~HelloImguiException() noexcept override = default;

    const char* what() const noexcept override { return _msg.empty() ? std::runtime_error::what() : _msg.c_str(); }

private:
    string _msg;
};

class HelloImguiApp {
public:
    HelloImguiApp(
        unique_ptr<NativeWindow> window,
        unique_ptr<InstanceVulkan> vkIns,
        shared_ptr<Device> device)
        : _window(std::move(window)),
          _vkIns(std::move(vkIns)),
          _device(std::move(device)) {
        _window->EventResizing().connect(&HelloImguiApp::OnResizing, this);
        _window->EventResized().connect(&HelloImguiApp::OnResized, this);
    }

    ~HelloImguiApp() noexcept {
        Close();
    }

    void Close() {
        _device.reset();
        if (_vkIns) DestroyVulkanInstance(std::move(_vkIns));
        if (_resizedCb.valid()) _resizedCb.disconnect();
        if (_resizingCb.valid()) _resizingCb.disconnect();
        if (_mainLoopResizeCb.valid()) _mainLoopResizeCb.disconnect();
        _window.reset();
    }

    void OnResizing(int, int) {
        std::lock_guard lock(_mutex);
        _isResizing = true;
    }

    void OnResized(int, int) {
        std::lock_guard lock(_mutex);
        _isResizing = false;
    }

    unique_ptr<NativeWindow> _window;
    unique_ptr<InstanceVulkan> _vkIns;
    shared_ptr<Device> _device;
    sigslot::scoped_connection _resizedCb;
    sigslot::scoped_connection _resizingCb;
    sigslot::connection _mainLoopResizeCb;

    mutable std::mutex _mutex{};
    bool _isResizing = false;
};

unique_ptr<HelloImguiApp> CreateApp(RenderBackend backend) {
    unique_ptr<NativeWindow> window;
    PlatformId platform = GetPlatform();
    if (platform == PlatformId::Windows) {
        string name = format("{} - {}", RADRAY_APP_NAME, backend);
        Win32WindowCreateDescriptor desc{};
        desc.Title = name;
        desc.Width = 1280;
        desc.Height = 720;
        desc.X = -1;
        desc.Y = -1;
        desc.Resizable = true;
        desc.StartMaximized = false;
        desc.Fullscreen = false;
        window = CreateNativeWindow(desc).Unwrap();
    }
    if (!window) {
        throw HelloImguiException("Failed to create native window");
    }
    unique_ptr<InstanceVulkan> instance;
    shared_ptr<Device> device;
    if (platform == PlatformId::Windows && backend == RenderBackend::D3D12) {
        D3D12DeviceDescriptor desc{};
        desc.IsEnableDebugLayer = true;
        device = CreateDevice(desc).Unwrap();
    } else if (backend == RenderBackend::Vulkan) {
        VulkanInstanceDescriptor insDesc{};
        insDesc.AppName = RADRAY_APP_NAME;
        insDesc.AppVersion = 1;
        insDesc.EngineName = "RadRay";
        insDesc.EngineVersion = 1;
        insDesc.IsEnableDebugLayer = true;
        instance = CreateVulkanInstance(insDesc).Unwrap();
        VulkanCommandQueueDescriptor queueDesc[] = {{QueueType::Direct, 1}};
        VulkanDeviceDescriptor devDesc{};
        devDesc.Queues = queueDesc;
        device = CreateDevice(devDesc).Unwrap();
    } else {
        throw HelloImguiException("Unsupported platform or backend");
    }
    if (!device) {
        throw HelloImguiException("Failed to create device");
    }
    auto result = make_unique<HelloImguiApp>(std::move(window), std::move(instance), std::move(device));
    return result;
}

vector<unique_ptr<HelloImguiApp>> g_apps;
bool g_anyResize = false;
bool g_lastAnyResize = false;
WindowVec2i g_lastSize;

int main() {
    GlobalInitDearImGui();

    g_apps.emplace_back(CreateApp(RenderBackend::D3D12));
    g_apps.emplace_back(CreateApp(RenderBackend::Vulkan));
    for (auto& app : g_apps) {
        app->_mainLoopResizeCb = app->_window->EventResized().connect([](int width, int height) {
            g_anyResize = true;
            g_lastSize = {width, height};
        });
    }
    while (true) {
        if (g_lastAnyResize) {
            for (const auto& app : g_apps) {
                app->_window->SetSize(g_lastSize.X, g_lastSize.Y);
            }
        }
        g_lastAnyResize = g_anyResize;
        g_anyResize = false;
        for (const auto& app : g_apps) {
            app->_window->DispatchEvents();
        }
        bool shouldClose = false;
        for (const auto& app : g_apps) {
            if (app->_window->ShouldClose()) {
                shouldClose = true;
                break;
            }
        }
        if (shouldClose) {
            break;
        }
        std::this_thread::yield();
    }

    g_apps.clear();

    GlobalTerminateDearImGui();
    FlushLog();
    return 0;
}
