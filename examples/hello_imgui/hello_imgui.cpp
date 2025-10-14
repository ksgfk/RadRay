#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
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
    shared_ptr<CommandBuffer> _cmdBuffer{};
    Texture* _rt{};
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
            auto& f = _frames.emplace_back(std::make_unique<HelloImguiFrame>());
            f->_cmdBuffer = _device->CreateCommandBuffer(_cmdQueue).Unwrap();
        }
        _currentRenderFrameIndex = 0;

        ImGuiDrawDescriptor desc{};
        desc.Device = _device.get();
        desc.RTFormat = TextureFormat::RGBA8_UNORM;
        desc.FrameCount = (int)_frames.size();
        _imguiDrawContext = CreateImGuiDrawContext(desc).Unwrap();

        _renderThread = std::thread{&HelloImguiApp::Render, this};
    }

    ~HelloImguiApp() noexcept {
        Close();
    }

    void Close() noexcept {
        if (_context != nullptr) {
            ImGui::DestroyContext(_context);
            _context = nullptr;
        }

        _isWaiting = false;
        _cv.notify_one();
        _renderThread.join();
        _cmdQueue->Wait();
        _rtViews.clear();
        _imguiDrawContext.reset();
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
        auto readParam = [this]() {
            std::lock_guard lock(_mutex);
            return _safeParams;
        };

        while (true) {
            {
                auto [isResizing, isResized, isCloseRequested] = readParam();
                if (isCloseRequested) {
                    break;
                }
                if (isResizing) {
                    continue;
                }
                if (isResized) {
                    std::lock_guard lock(_mutex);
                    _cmdQueue->Wait();
                    _rtViews.clear();
                    SetSwapChain();
                    _safeParams.isResized = false;
                    continue;
                }
            }
            Nullable<Texture*> acqTex = _swapchain->AcquireNext();
            if (acqTex == nullptr) {
                continue;
            }
            auto& currFrame = _frames[_currentRenderFrameIndex];
            {
                std::unique_lock<std::mutex> lk{_mtx};
                currFrame->_rt = acqTex.Unwrap();
                _isWaiting = true;
                _cv.wait(lk, [this]() { return !_isWaiting.load(); });
                auto [isResizing, isResized, isCloseRequested] = readParam();
                if (isCloseRequested) {
                    break;
                }
                if (isResizing) {
                    continue;
                }
            }
            {
                CommandQueueSubmitDescriptor submitDesc{};
                auto cmdBuffer = currFrame->_cmdBuffer.get();
                submitDesc.CmdBuffers = std::span{&cmdBuffer, 1};
                _cmdQueue->Submit(submitDesc);
            }
            _swapchain->Present();
            currFrame->_rt = nullptr;
            _currentRenderFrameIndex = (_currentRenderFrameIndex + 1) % _frames.size();
        }
    }

    void OnResizing(int, int) {
        std::lock_guard lock(_mutex);
        _safeParams.isResizing = true;
    }

    void OnResized(int, int) {
        std::lock_guard lock(_mutex);
        _safeParams.isResizing = false;
        _safeParams.isResized = true;
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

    ImGuiContext* _context{nullptr};
    unique_ptr<NativeWindow> _window;
    unique_ptr<InstanceVulkan> _vkIns;
    shared_ptr<Device> _device;
    CommandQueue* _cmdQueue;
    shared_ptr<SwapChain> _swapchain;
    vector<unique_ptr<HelloImguiFrame>> _frames;
    uint64_t _currentRenderFrameIndex;
    unique_ptr<ImGuiDrawContext> _imguiDrawContext;
    unordered_map<Texture*, shared_ptr<TextureView>> _rtViews;

    std::thread _renderThread;
    sigslot::connection _resizedCb;
    sigslot::connection _resizingCb;
    sigslot::connection _mainLoopResizeCb;
    sigslot::connection _closeCb;

    std::mutex _mtx{};
    std::condition_variable _cv{};
    std::atomic_bool _isWaiting{false};

    mutable std::mutex _mutex{};
    struct ThreadSafeParams {
        bool isResizing = false;
        bool isResized = false;
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
        desc.IsEnableGpuBasedValid = true;
        device = CreateDevice(desc).Unwrap();
    } else if (backend == RenderBackend::Vulkan) {
        VulkanInstanceDescriptor insDesc{};
        insDesc.AppName = RADRAY_APP_NAME;
        insDesc.AppVersion = 1;
        insDesc.EngineName = "RadRay";
        insDesc.EngineVersion = 1;
        insDesc.IsEnableDebugLayer = true;
        instance = CreateVulkanInstance(insDesc).Unwrap();
        VulkanCommandQueueDescriptor queueDesc[] = {
            {QueueType::Direct, 1}};
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
    g_apps.emplace_back(CreateApp(RenderBackend::D3D12));
    g_apps.emplace_back(CreateApp(RenderBackend::Vulkan));

    if (g_apps.size() == 0) {
        throw HelloImguiException("No app created");
    }

    InitImGui();
    if (GetPlatform() == PlatformId::Windows) {
        for (auto& app : g_apps) {
            ImGuiPlatformInitDescriptor desc{};
            desc.Platform = PlatformId::Windows;
            desc.Window = app->_window.get();
            desc.Context = ImGui::CreateContext();
            InitPlatformImGui(desc);
            InitRendererImGui(desc.Context);
            app->_context = desc.Context;
        }
    } else {
        throw HelloImguiException("Unsupported platform");
    }
    {
        for (auto& app : g_apps) {
            HelloImguiApp* self = app.get();
            app->_mainLoopResizeCb = app->_window->EventResized().connect([self](int w, int h) {
                for (const auto& other : g_apps) {
                    if (other.get() != self) {
                        other->_window->SetSize(w, h);
                    }
                }
            });
        }
    }
    while (true) {
        for (const auto& app : g_apps) {
            ImGui::SetCurrentContext(app->_context);
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

        for (const auto& app : g_apps) {
            ImGui::SetCurrentContext(app->_context);
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (g_showDemo) {
                ImGui::ShowDemoWindow(&g_showDemo);
            }
            ImGui::Render();
        }

        bool canRender = true;
        for (const auto& app : g_apps) {
            if (!app->_isWaiting) {
                canRender = false;
                break;
            }
        }
        if (canRender) {
            for (const auto& app : g_apps) {
                ImGui::SetCurrentContext(app->_context);
                auto& frame = app->_frames[app->_currentRenderFrameIndex];
                frame->_cmdBuffer->Begin();
                app->_imguiDrawContext->NewFrame();
                app->_imguiDrawContext->UpdateDrawData(ImGui::GetDrawData(), frame->_cmdBuffer.get());
                app->_imguiDrawContext->EndUpdateDrawData(ImGui::GetDrawData());
            }
            for (const auto& app : g_apps) {
                ImGui::SetCurrentContext(app->_context);
                auto& frame = app->_frames[app->_currentRenderFrameIndex];
                {
                    BarrierTextureDescriptor barrier{};
                    barrier.Target = frame->_rt;
                    barrier.Before = TextureUse::Uninitialized;
                    barrier.After = TextureUse::RenderTarget;
                    barrier.IsFromOrToOtherQueue = false;
                    barrier.IsSubresourceBarrier = false;
                    frame->_cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
                }
                unique_ptr<CommandEncoder> pass;
                {
                    RenderPassDescriptor rpDesc{};
                    ColorAttachment rtAttach{};
                    if (app->_rtViews.contains(frame->_rt)) {
                        rtAttach.Target = app->_rtViews[frame->_rt].get();
                    } else {
                        TextureViewDescriptor rtViewDesc{};
                        rtViewDesc.Target = frame->_rt;
                        rtViewDesc.Dim = TextureViewDimension::Dim2D;
                        rtViewDesc.Format = TextureFormat::RGBA8_UNORM;
                        rtViewDesc.Range = SubresourceRange::AllSub();
                        rtViewDesc.Usage = TextureUse::RenderTarget;
                        auto rtView = app->_device->CreateTextureView(rtViewDesc).Unwrap();
                        app->_rtViews[frame->_rt] = rtView;
                        rtAttach.Target = rtView.get();
                    }
                    rtAttach.Load = LoadAction::Clear;
                    rtAttach.Store = StoreAction::Store;
                    rtAttach.ClearValue = ColorClearValue{0.1f, 0.1f, 0.1f, 1.0f};
                    rpDesc.ColorAttachments = std::span(&rtAttach, 1);
                    pass = frame->_cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
                }
                app->_imguiDrawContext->Draw(ImGui::GetDrawData(), pass.get());
                frame->_cmdBuffer->EndRenderPass(std::move(pass));
                {
                    BarrierTextureDescriptor barrier{};
                    barrier.Target = frame->_rt;
                    barrier.Before = TextureUse::RenderTarget;
                    barrier.After = TextureUse::Present;
                    barrier.IsFromOrToOtherQueue = false;
                    barrier.IsSubresourceBarrier = false;
                    frame->_cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
                }
            }
            for (const auto& app : g_apps) {
                ImGui::SetCurrentContext(app->_context);
                auto& frame = app->_frames[app->_currentRenderFrameIndex];
                frame->_cmdBuffer->End();
                app->_imguiDrawContext->EndFrame();
                app->_isWaiting = false;
                app->_cv.notify_one();
            }
        }
    }

    g_closeSig();

    for (auto& app : g_apps) {
        TerminateRendererImGui(app->_context);
        TerminatePlatformImGui(app->_context);
    }

    g_apps.clear();

    FlushLog();
    return 0;
}
