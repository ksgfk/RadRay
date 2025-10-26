#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <stdexcept>
#include <deque>
#include <array>
#include <limits>

#include <radray/types.h>
#include <radray/logger.h>
#include <radray/platform.h>
#include <radray/channel.h>
#include <radray/stopwatch.h>
#include <radray/window/native_window.h>
#include <radray/render/common.h>
#include <radray/imgui/dear_imgui.h>

const char* RADRAY_APP_NAME = "Hello Dear ImGui";

class HelloImguiException;
class HelloImguiFrame;
class HelloImguiApp;

radray::vector<radray::unique_ptr<HelloImguiApp>> g_apps;

enum class HelloImguiWaitRenderStrategy {
    Wait,
    Discord
};

class HelloImguiException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    template <typename... Args>
    explicit HelloImguiException(fmt::format_string<Args...> fmt, Args&&... args)
        : _msg(radray::format(fmt, std::forward<Args>(args)...)) {}
    ~HelloImguiException() noexcept override = default;

    const char* what() const noexcept override { return _msg.empty() ? std::runtime_error::what() : _msg.c_str(); }

private:
    radray::string _msg;
};

class HelloImguiFrame {
public:
    size_t _frameIndex;
    uint64_t _completeFrame{std::numeric_limits<uint64_t>::max()};
    radray::shared_ptr<radray::render::CommandBuffer> _cmdBuffer{};
    radray::render::Texture* _rt{};
    radray::render::TextureView* _rtView{};
};

class HelloImguiApp {
public:
    explicit HelloImguiApp(
        radray::render::RenderBackend backend,
        HelloImguiWaitRenderStrategy strategy,
        bool multiThreadRender)
        : _imguiContext(radray::make_unique<radray::ImGuiContextRAII>()),
          _waitRenderStrategy(strategy),
          _useRenderThread(multiThreadRender) {
        if (!_useRenderThread) {
            _waitRenderStrategy = HelloImguiWaitRenderStrategy::Discord;
        }

        radray::PlatformId platform = radray::GetPlatform();
        if (platform == radray::PlatformId::Windows) {
            radray::string name = radray::format("{} - {}", RADRAY_APP_NAME, backend);
            _win32ImguiProc = radray::make_shared<std::function<radray::Win32WNDPROC>>(radray::GetImGuiWin32WNDPROCEx(_imguiContext->Get()).Unwrap());
            radray::weak_ptr<std::function<radray::Win32WNDPROC>> weakImguiProc = _win32ImguiProc;
            radray::Win32WindowCreateDescriptor desc{};
            desc.Title = name;
            desc.Width = 1280;
            desc.Height = 720;
            desc.X = -1;
            desc.Y = -1;
            desc.Resizable = false;
            desc.StartMaximized = false;
            desc.Fullscreen = false;
            desc.ExtraWndProcs = std::span{&weakImguiProc, 1};
            _window = radray::CreateNativeWindow(desc).Unwrap();
            radray::ImGuiPlatformInitDescriptor imguiDesc{};
            imguiDesc.Platform = radray::PlatformId::Windows;
            imguiDesc.Window = _window.get();
            imguiDesc.Context = _imguiContext->Get();
            radray::InitPlatformImGui(imguiDesc);
            radray::InitRendererImGui(imguiDesc.Context);
        }
        if (!_window) {
            throw HelloImguiException("failed to create native window");
        }
        if (platform == radray::PlatformId::Windows && backend == radray::render::RenderBackend::D3D12) {
            radray::render::D3D12DeviceDescriptor desc{};
#ifdef RADRAY_IS_DEBUG
            desc.IsEnableDebugLayer = true;
            desc.IsEnableGpuBasedValid = true;
#else
            desc.IsEnableDebugLayer = false;
            desc.IsEnableGpuBasedValid = false;
#endif
            _device = CreateDevice(desc).Unwrap();
        } else if (backend == radray::render::RenderBackend::Vulkan) {
            radray::render::VulkanInstanceDescriptor insDesc{};
            insDesc.AppName = RADRAY_APP_NAME;
            insDesc.AppVersion = 1;
            insDesc.EngineName = "RadRay";
            insDesc.EngineVersion = 1;
#ifdef RADRAY_IS_DEBUG
            insDesc.IsEnableDebugLayer = true;
#else
            insDesc.IsEnableDebugLayer = false;
#endif
            _vkIns = CreateVulkanInstance(insDesc).Unwrap();
            radray::render::VulkanCommandQueueDescriptor queueDesc[] = {
                {radray::render::QueueType::Direct, 1}};
            radray::render::VulkanDeviceDescriptor devDesc{};
            devDesc.Queues = queueDesc;
            _device = CreateDevice(devDesc).Unwrap();
        } else {
            throw HelloImguiException("unsupported platform or backend");
        }
        _cmdQueue = _device->GetCommandQueue(radray::render::QueueType::Direct, 0).Unwrap();
        SetSwapChain();
        _frames.reserve(_swapchain->GetBackBufferCount());
        for (size_t i = 0; i < _swapchain->GetBackBufferCount(); ++i) {
            auto& f = _frames.emplace_back(radray::make_unique<HelloImguiFrame>());
            f->_frameIndex = i;
            f->_cmdBuffer = _device->CreateCommandBuffer(_cmdQueue).Unwrap();
        }
        _currRenderFrame = 0;
        radray::ImGuiDrawDescriptor imguiDrawDesc{};
        imguiDrawDesc.Device = _device.get();
        imguiDrawDesc.RTFormat = radray::render::TextureFormat::RGBA8_UNORM;
        imguiDrawDesc.FrameCount = (int)_frames.size();
        _imguiDrawContext = radray::CreateImGuiDrawContext(imguiDrawDesc).Unwrap();

        for (size_t i = 0; i < _frames.size(); i++) {
            _freeFrame.TryWrite(i);
        }
    }

    ~HelloImguiApp() noexcept {
        _cmdQueue->Wait();
        _cmdQueue = nullptr;
        radray::TerminateRendererImGui(_imguiContext->Get());
        radray::TerminatePlatformImGui(_imguiContext->Get());
        _imguiDrawContext.reset();
        _frames.clear();
        _rtViews.clear();
        _swapchain.reset();
        _device.reset();
        _vkIns.reset();
        _window.reset();
        _win32ImguiProc.reset();
        _imguiContext.reset();
    }
    HelloImguiApp(const HelloImguiApp&) = delete;
    HelloImguiApp& operator=(const HelloImguiApp&) = delete;
    HelloImguiApp(HelloImguiApp&&) = delete;
    HelloImguiApp& operator=(HelloImguiApp&&) = delete;

    void SetSwapChain() {
        _swapchain.reset();
        auto [winW, winH] = _window->GetSize();
        radray::render::SwapChainDescriptor swapchainDesc{};
        swapchainDesc.PresentQueue = _cmdQueue;
        swapchainDesc.NativeHandler = _window->GetNativeHandler().Handle;
        swapchainDesc.Width = (uint32_t)winW;
        swapchainDesc.Height = (uint32_t)winH;
        swapchainDesc.BackBufferCount = 3;
        swapchainDesc.Format = radray::render::TextureFormat::RGBA8_UNORM;
        swapchainDesc.EnableSync = true;
        _swapchain = _device->CreateSwapChain(swapchainDesc).Unwrap();
    }

    void RunMainThread() {
        if (_useRenderThread) {
            this->RunMultiThreadRender();
        } else {
            this->RunSingleThreadRender();
        }
    }

    void RunMultiThreadRender() {
        _renderThread = radray::make_unique<std::thread>(&HelloImguiApp::RunRenderThread, this);
        while (true) {
            radray::Stopwatch sw{};
            sw.Start();
            this->GameUpdate();
            if (_needClose) {
                break;
            }
            sw.Stop();
            _tps = sw.ElapsedNanoseconds() / 1000000.0;
        }
        _freeFrame.Complete();
        _waitFrame.Complete();
        _renderThread->join();
    }

    void RunSingleThreadRender() {
        while (true) {
            radray::Stopwatch sw{};
            sw.Start();
            this->GameUpdate();
            sw.Stop();
            _tps = sw.ElapsedNanoseconds() / 1000000.0;
            sw.Start();
            if (_needClose) {
                break;
            }
            this->RenderUpdateST();
            sw.Stop();
            _fps = sw.ElapsedNanoseconds() / 1000000.0;
        }
    }

    void RunRenderThread() {
        while (true) {
            radray::Stopwatch sw{};
            sw.Start();
            if (_needClose) {
                break;
            }
            this->RenderUpdateMT();
            sw.Stop();
            _fps = sw.ElapsedNanoseconds() / 1000000.0;
        }
    }

    void GameUpdate() {
        _imguiContext->SetCurrent();
        _window->DispatchEvents();
        if (_window->ShouldClose()) {
            _needClose = true;
            return;
        }

        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (_showDemo) {
            ImGui::ShowDemoWindow(&_showDemo);
        }
        {
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            int location = 0;
            const float PAD = 10.0f;
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 work_pos = viewport->WorkPos;
            ImVec2 work_size = viewport->WorkSize;
            ImVec2 window_pos, window_pos_pivot;
            window_pos.x = (location & 1) ? (work_pos.x + work_size.x - PAD) : (work_pos.x + PAD);
            window_pos.y = (location & 2) ? (work_pos.y + work_size.y - PAD) : (work_pos.y + PAD);
            window_pos_pivot.x = (location & 1) ? 1.0f : 0.0f;
            window_pos_pivot.y = (location & 2) ? 1.0f : 0.0f;
            ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
            ImGui::SetNextWindowViewport(viewport->ID);
            window_flags |= ImGuiWindowFlags_NoMove;
            ImGui::SetNextWindowBgAlpha(0.35f);
            if (ImGui::Begin("RadrayMonitor", &_showMonitor, window_flags)) {
                ImGui::Text("TPS: (%09.4f ms)", _tps);
                ImGui::Text("FPS: (%09.4f ms)", _fps.load());
                ImGui::End();
            }
        }
        ImGui::Render();

        radray::Nullable<HelloImguiFrame*> frameOpt = GetAvailableFrame();
        if (frameOpt.HasValue()) {
            HelloImguiFrame* frame = frameOpt.Value();
            ImDrawData* drawData = ImGui::GetDrawData();
            _imguiDrawContext->ExtractDrawData((int)frame->_frameIndex, drawData);
            _waitFrame.WaitWrite(frame->_frameIndex);
        }
    }

    void RenderUpdateMT() {
        radray::Nullable<radray::render::Texture*> rtOpt = _swapchain->AcquireNext();
        if (_needClose) {
            return;
        }
        if (!rtOpt.HasValue()) {
            return;
        }
        {
            size_t completeFrameIndex = _currRenderFrame % _frames.size();
            if (_frames[completeFrameIndex]->_completeFrame == _currRenderFrame) {
                _imguiDrawContext->AfterDraw(completeFrameIndex);
                _frames[completeFrameIndex]->_completeFrame = std::numeric_limits<uint64_t>::max();
                if (!_freeFrame.WaitWrite(completeFrameIndex)) {
                    return;
                }
            }
        }
        size_t frameIndex = std::numeric_limits<size_t>::max();
        if (!_waitFrame.WaitRead(frameIndex)) {
            return;
        }
        HelloImguiFrame* frame = _frames[frameIndex].get();
        frame->_completeFrame = _currRenderFrame + _frames.size();
        frame->_rt = rtOpt.Value();
        frame->_rtView = this->SafeGetRTView(frame->_rt);
        this->OnRender(frame->_frameIndex);
        _swapchain->Present();
        _currRenderFrame++;
    }

    void RenderUpdateST() {
        radray::Nullable<radray::render::Texture*> rtOpt;
        if (_acquiredRt) {
            rtOpt = _swapchain->GetCurrentBackBuffer();
            _acquiredRt = false;
        } else {
            rtOpt = _swapchain->AcquireNext();
        }
        if (!rtOpt.HasValue()) {
            return;
        }
        {
            size_t completeFrameIndex = _currRenderFrame % _frames.size();
            if (_frames[completeFrameIndex]->_completeFrame == _currRenderFrame) {
                _imguiDrawContext->AfterDraw(completeFrameIndex);
                _frames[completeFrameIndex]->_completeFrame = std::numeric_limits<uint64_t>::max();
                if (!_freeFrame.WaitWrite(completeFrameIndex)) {
                    return;
                }
            }
        }
        size_t frameIndex = std::numeric_limits<size_t>::max();
        if (!_waitFrame.TryRead(frameIndex)) {
            _acquiredRt = true;
            return;
        }
        HelloImguiFrame* frame = _frames[frameIndex].get();
        frame->_completeFrame = _currRenderFrame + _frames.size();
        frame->_rt = rtOpt.Value();
        frame->_rtView = this->SafeGetRTView(frame->_rt);
        this->OnRender(frame->_frameIndex);
        _swapchain->Present();
        _currRenderFrame++;
    }

    void OnRender(size_t frameIndex) {
        HelloImguiFrame* currFrame = _frames[frameIndex].get();
        currFrame->_cmdBuffer->Begin();
        _imguiDrawContext->BeforeDraw((int)currFrame->_frameIndex, currFrame->_cmdBuffer.get());
        {
            radray::render::BarrierTextureDescriptor barrier{};
            barrier.Target = currFrame->_rt;
            barrier.Before = radray::render::TextureUse::Uninitialized;
            barrier.After = radray::render::TextureUse::RenderTarget;
            barrier.IsFromOrToOtherQueue = false;
            barrier.IsSubresourceBarrier = false;
            currFrame->_cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        radray::unique_ptr<radray::render::CommandEncoder> pass;
        {
            radray::render::RenderPassDescriptor rpDesc{};
            radray::render::ColorAttachment rtAttach{};
            rtAttach.Target = currFrame->_rtView;
            rtAttach.Load = radray::render::LoadAction::Clear;
            rtAttach.Store = radray::render::StoreAction::Store;
            rtAttach.ClearValue = radray::render::ColorClearValue{0.1f, 0.1f, 0.1f, 1.0f};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            pass = currFrame->_cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        }
        _imguiDrawContext->Draw((int)currFrame->_frameIndex, pass.get());
        currFrame->_cmdBuffer->EndRenderPass(std::move(pass));
        {
            radray::render::BarrierTextureDescriptor barrier{};
            barrier.Target = currFrame->_rt;
            barrier.Before = radray::render::TextureUse::RenderTarget;
            barrier.After = radray::render::TextureUse::Present;
            barrier.IsFromOrToOtherQueue = false;
            barrier.IsSubresourceBarrier = false;
            currFrame->_cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        currFrame->_cmdBuffer->End();
        {
            radray::render::CommandQueueSubmitDescriptor submitDesc{};
            auto cmdBuffer = currFrame->_cmdBuffer.get();
            submitDesc.CmdBuffers = std::span{&cmdBuffer, 1};
            _cmdQueue->Submit(submitDesc);
        }
    }

    radray::Nullable<HelloImguiFrame*> GetAvailableFrame() {
        if (_waitRenderStrategy == HelloImguiWaitRenderStrategy::Wait) {
            size_t frameIndex;
            if (_freeFrame.WaitRead(frameIndex)) {
                return _frames[frameIndex].get();
            } else {
                return nullptr;
            }
        } else if (_waitRenderStrategy == HelloImguiWaitRenderStrategy::Discord) {
            size_t frameIndex;
            if (_freeFrame.TryRead(frameIndex)) {
                return _frames[frameIndex].get();
            } else {
                return nullptr;
            }
        }
        return nullptr;
    }

    radray::render::TextureView* SafeGetRTView(radray::render::Texture* rt) {
        auto it = _rtViews.find(rt);
        if (it == _rtViews.end()) {
            radray::render::TextureViewDescriptor rtViewDesc{};
            rtViewDesc.Target = rt;
            rtViewDesc.Dim = radray::render::TextureViewDimension::Dim2D;
            rtViewDesc.Format = radray::render::TextureFormat::RGBA8_UNORM;
            rtViewDesc.Range = radray::render::SubresourceRange::AllSub();
            rtViewDesc.Usage = radray::render::TextureUse::RenderTarget;
            auto rtView = _device->CreateTextureView(rtViewDesc).Unwrap();
            it = _rtViews.emplace(rt, rtView).first;
        }
        return it->second.get();
    }

public:
    radray::unique_ptr<radray::ImGuiContextRAII> _imguiContext;
    radray::shared_ptr<std::function<radray::Win32WNDPROC>> _win32ImguiProc;
    radray::unique_ptr<radray::NativeWindow> _window;
    radray::unique_ptr<radray::render::InstanceVulkan> _vkIns;
    radray::shared_ptr<radray::render::Device> _device;
    radray::render::CommandQueue* _cmdQueue;
    radray::shared_ptr<radray::render::SwapChain> _swapchain;
    radray::unique_ptr<radray::ImGuiDrawContext> _imguiDrawContext;
    radray::unique_ptr<std::thread> _renderThread;
    HelloImguiWaitRenderStrategy _waitRenderStrategy;
    bool _useRenderThread{false};

    radray::unordered_map<radray::render::Texture*, radray::shared_ptr<radray::render::TextureView>> _rtViews;
    radray::vector<radray::unique_ptr<HelloImguiFrame>> _frames;
    uint64_t _currRenderFrame;
    radray::BoundedChannel<size_t> _freeFrame{3};
    radray::BoundedChannel<size_t> _waitFrame{3};
    std::atomic_bool _needClose{false};
    std::atomic<double> _fps;
    double _tps;
    bool _acquiredRt{false};
    bool _showMonitor{true};
    bool _showDemo{true};
};

int main() {
    radray::InitImGui();
    HelloImguiApp app{radray::render::RenderBackend::Vulkan, HelloImguiWaitRenderStrategy::Wait, true};
    app.RunMainThread();
    radray::FlushLog();
    return 0;
}
