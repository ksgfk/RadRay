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

class HelloImguiException;
class HelloImguiFrame;
class HelloImguiApp;

vector<unique_ptr<HelloImguiApp>> g_apps;
bool g_anyResize = false;
bool g_lastAnyResize = false;
WindowVec2i g_lastSize;
sigslot::signal<> g_closeSig;
bool g_showDemo = true;

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

class HelloImguiFrame {
public:
    shared_ptr<CommandBuffer> _cmdBuffer;
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
        _closeCb = g_closeSig.connect(&HelloImguiApp::OnGlobalClose, this);

        _cmdQueue = _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        SetSwapChain();
        _frames.reserve(_swapchain->GetBackBufferCount());
        for (size_t i = 0; i < _swapchain->GetBackBufferCount(); ++i) {
            auto& f = _frames.emplace_back();
            f._cmdBuffer = _device->CreateCommandBuffer(_cmdQueue).Unwrap();
        }
        _currentFrameIndex = 0;
        ImGuiDrawDescriptor desc{};
        desc.Device = _device.get();
        desc.RTFormat = TextureFormat::RGBA8_UNORM;
        desc.FrameCount = (int)_frames.size();
        // _imguiDrawContext = CreateImGuiDrawContext(desc).Unwrap();

        _renderThread = std::thread{&HelloImguiApp::Render, this};
    }

    ~HelloImguiApp() noexcept {
        Close();
    }

    void Close() noexcept {
        _renderThread.join();
        _cmdQueue->Wait();
        // _imguiDrawContext.reset();
        _frames.clear();
        _swapchain.reset();
        _cmdQueue = nullptr;
        _device.reset();
        if (_vkIns) DestroyVulkanInstance(std::move(_vkIns));
        if (_resizedCb.valid()) _resizedCb.disconnect();
        if (_resizingCb.valid()) _resizingCb.disconnect();
        if (_mainLoopResizeCb.valid()) _mainLoopResizeCb.disconnect();
        if (_closeCb.valid()) _closeCb.disconnect();
        _window.reset();
    }

    void Render() {
        while (true) {
            ThreadSafeParams params;
            {
                std::lock_guard lock(_mutex);
                params = _safeParams;
            }

            if (params.isCloseRequested) {
                break;
            }

            Nullable<Texture*> acqTex = _swapchain->AcquireNext();
            if (acqTex == nullptr) {
                continue;
            }
            _swapchain->Present();
            _currentFrameIndex = (_currentFrameIndex + 1) % _frames.size();
        }
    }

    void OnResizing(int, int) {
        std::lock_guard lock(_mutex);
        _safeParams.isResizing = true;
    }

    void OnResized(int, int) {
        std::lock_guard lock(_mutex);
        _safeParams.isResizing = false;
    }

    void OnGlobalClose() {
        std::lock_guard lock(_mutex);
        _safeParams.isCloseRequested = true;
    }

    void SetSwapChain() {
        _swapchain.reset();
        auto [winW, winH] = _window->GetSize();
        SwapChainDescriptor swapchainDesc{};
        swapchainDesc.PresentQueue = _cmdQueue;
        swapchainDesc.NativeHandler = _window->GetNativeHandler().Handle;
        swapchainDesc.Width = (uint32_t)winW;
        swapchainDesc.Height = (uint32_t)winH;
        swapchainDesc.BackBufferCount = 3;
        swapchainDesc.Format = TextureFormat::RGBA8_UNORM;
        swapchainDesc.EnableSync = false;
        _swapchain = _device->CreateSwapChain(swapchainDesc).Unwrap();
    }

    unique_ptr<NativeWindow> _window;
    unique_ptr<InstanceVulkan> _vkIns;
    shared_ptr<Device> _device;
    CommandQueue* _cmdQueue;
    shared_ptr<SwapChain> _swapchain;
    vector<HelloImguiFrame> _frames;
    uint32_t _currentFrameIndex;
    // unique_ptr<ImGuiDrawContext> _imguiDrawContext;

    std::thread _renderThread;
    sigslot::connection _resizedCb;
    sigslot::connection _resizingCb;
    sigslot::connection _mainLoopResizeCb;
    sigslot::connection _closeCb;

    mutable std::mutex _mutex{};
    struct ThreadSafeParams {
        bool isResizing = false;
        bool isCloseRequested = false;
    } _safeParams;
};

unique_ptr<HelloImguiApp> CreateApp(RenderBackend backend) {
    unique_ptr<NativeWindow> window;
    PlatformId platform = GetPlatform();
    if (platform == PlatformId::Windows) {
        string name = format("{} - {}", RADRAY_APP_NAME, backend);
        vector<Win32WNDPROC> extraWndProcs;
        extraWndProcs.push_back(GetImGuiWin32WNDPROC().Unwrap());
        Win32WindowCreateDescriptor desc{};
        desc.Title = name;
        desc.Width = 1280;
        desc.Height = 720;
        desc.X = -1;
        desc.Y = -1;
        desc.Resizable = true;
        desc.StartMaximized = false;
        desc.Fullscreen = false;
        desc.ExtraWndProcs = extraWndProcs;
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

int main() {
    // g_apps.emplace_back(CreateApp(RenderBackend::D3D12));
    g_apps.emplace_back(CreateApp(RenderBackend::Vulkan));

    InitImGui();
    if (GetPlatform() == PlatformId::Windows) {
        ImGuiPlatformInitDescriptor desc{};
        desc.Platform = PlatformId::Windows;
        desc.Window = g_apps[0]->_window.get();
        InitPlatformImGui(desc);
    } else {
        throw HelloImguiException("Unsupported platform");
    }
    InitRendererImGui();

    unique_ptr<ImGuiDrawContext> _imguiDrawContext;
    {
        ImGuiDrawDescriptor desc{};
        desc.Device = g_apps[0]->_device.get();
        desc.RTFormat = TextureFormat::RGBA8_UNORM;
        desc.FrameCount = 3;
        _imguiDrawContext = CreateImGuiDrawContext(desc).Unwrap();
    }

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

        ImGui_ImplWin32_NewFrame();

        ImGui::NewFrame();
        if (g_showDemo) {
            ImGui::ShowDemoWindow(&g_showDemo);
        }
        ImGui::Render();

        _imguiDrawContext->Draw(ImGui::GetDrawData());

        std::this_thread::yield();
    }

    g_closeSig();

    _imguiDrawContext.reset();

    TerminateRendererImGui();
    TerminatePlatformImGui();
    TerminateImGui();

    g_apps.clear();

    FlushLog();
    return 0;
}
