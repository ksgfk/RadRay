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
#include <radray/window/native_window.h>
#include <radray/render/common.h>
#include <radray/imgui/dear_imgui.h>

const char* RADRAY_APP_NAME = "Hello Dear ImGui";

class HelloImguiException;
class HelloImguiFrame;
class HelloImguiApp;

radray::vector<radray::unique_ptr<HelloImguiApp>> g_apps;
sigslot::signal<> g_closeSig;

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
    radray::shared_ptr<radray::render::CommandBuffer> _cmdBuffer{};
    radray::render::Texture* _rt{};
    radray::render::TextureView* _rtView{};
    enum class State : uint8_t { Idle = 0, Writing, Ready, Rendering };
    std::atomic<State> _state{State::Idle};
    uint64_t _writeSeq{0};
};

class HelloImguiApp {
public:
    explicit HelloImguiApp(radray::render::RenderBackend backend, HelloImguiWaitRenderStrategy strategy)
        : _imguiContext(radray::make_unique<radray::ImGuiContextRAII>()),
          _waitRenderStrategy(strategy) {
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
    _writeIndex = 0;
    _renderIndex = 0;
        radray::ImGuiDrawDescriptor imguiDrawDesc{};
        imguiDrawDesc.Device = _device.get();
        imguiDrawDesc.RTFormat = radray::render::TextureFormat::RGBA8_UNORM;
        imguiDrawDesc.FrameCount = (int)_frames.size();
        _imguiDrawContext = radray::CreateImGuiDrawContext(imguiDrawDesc).Unwrap();
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
        swapchainDesc.EnableSync = false;
        _swapchain = _device->CreateSwapChain(swapchainDesc).Unwrap();
    }

    void RunMainThread() {
        _renderThread = radray::make_unique<std::thread>(&HelloImguiApp::RunRenderThread, this);
        while (true) {
            this->GameUpdate();
            if (_needClose) {
                break;
            }
        }
        _renderThread->join();
    }

    void RunRenderThread() {
        while (true) {
            if (_needClose) break;
            radray::Nullable<radray::render::Texture*> rtOpt = _swapchain->AcquireNext();
            if (_needClose) break;
            if (!rtOpt.HasValue()) {
                continue;
            }

            // Wait for the next frame in render order to be ready
            HelloImguiFrame* frame = nullptr;
            {
                std::unique_lock<std::mutex> lk(_mtx);
                _cv.wait(lk, [&]() noexcept {
                    return _needClose || _frames[_renderIndex]->_state.load() == HelloImguiFrame::State::Ready;
                });
                if (_needClose) break;
                frame = _frames[_renderIndex].get();
                frame->_state.store(HelloImguiFrame::State::Rendering);
            }

            // Bind current back buffer to this frame
            frame->_rt = rtOpt.Value();
            frame->_rtView = this->SafeGetRTView(frame->_rt);

            this->RenderUpdate(frame->_frameIndex);
            _swapchain->Present();

            // Mark frame idle and advance
            {
                std::lock_guard<std::mutex> lk(_mtx);
                frame->_state.store(HelloImguiFrame::State::Idle);
                _renderIndex = (_renderIndex + 1) % _frames.size();
            }
            _cv.notify_all();
        }
    }

    void GameUpdate() {
        _imguiContext->SetCurrent();
        _window->DispatchEvents();
        if (_window->ShouldClose()) {
            _needClose = true;
            g_closeSig();
            _cv.notify_all();
        }
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (_showDemo) {
            ImGui::ShowDemoWindow(&_showDemo);
        }
        ImGui::Render();

        radray::Nullable<HelloImguiFrame*> frameOpt = GetAvailableFrame();
        if (frameOpt.HasValue()) {
            HelloImguiFrame* frame = frameOpt.Value();
            ImDrawData* drawData = ImGui::GetDrawData();
            // Write into per-frame imgui cache
            _imguiDrawContext->ExtractDrawData((int)frame->_frameIndex, drawData);
            // Mark ready and stamp sequence
            {
                std::lock_guard<std::mutex> lk(_mtx);
                frame->_writeSeq = ++_writeSeqCounter;
                frame->_state.store(HelloImguiFrame::State::Ready);
            }
            _cv.notify_all();
        }
    }

    void RenderUpdate(size_t frameIndex) {
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
        if (_needClose) return nullptr;
        std::unique_lock<std::mutex> lk(_mtx);
        auto try_select_write_index = [&]() -> HelloImguiFrame* {
            HelloImguiFrame* f = _frames[_writeIndex].get();
            auto st = f->_state.load();
            if (st != HelloImguiFrame::State::Rendering && st != HelloImguiFrame::State::Writing) {
                f->_state.store(HelloImguiFrame::State::Writing);
                return f;
            }
            return nullptr;
        };

        if (_waitRenderStrategy == HelloImguiWaitRenderStrategy::Wait) {
            // Strictly wait until the next in-order frame is writable
            _cv.wait(lk, [&]() noexcept {
                if (_needClose) return true;
                HelloImguiFrame* f = _frames[_writeIndex].get();
                auto st = f->_state.load();
                return st == HelloImguiFrame::State::Idle; // wait until writable
            });
            if (_needClose) return nullptr;
            HelloImguiFrame* f = _frames[_writeIndex].get();
            f->_state.store(HelloImguiFrame::State::Writing);
            // advance write index for next time (in-order)
            _writeIndex = (_writeIndex + 1) % _frames.size();
            return f;
        } else { // Discard strategy
            // First try the in-order frame if not rendering
            if (HelloImguiFrame* sel = try_select_write_index(); sel != nullptr) {
                // advance only if we wrote to the in-order frame
                _writeIndex = (_writeIndex + 1) % _frames.size();
                return sel;
            }
            // Otherwise, find the earliest written but not rendered frame (state Ready with smallest writeSeq)
            HelloImguiFrame* oldestReady = nullptr;
            uint64_t bestSeq = std::numeric_limits<uint64_t>::max();
            for (auto& upf : _frames) {
                HelloImguiFrame* f = upf.get();
                if (f->_state.load() == HelloImguiFrame::State::Ready) {
                    if (f->_writeSeq < bestSeq) {
                        bestSeq = f->_writeSeq;
                        oldestReady = f;
                    }
                }
            }
            if (oldestReady) {
                oldestReady->_state.store(HelloImguiFrame::State::Writing);
                // do NOT advance _writeIndex because we didn't use the in-order one
                return oldestReady;
            }
            // If none Ready, pick any Idle frame
            for (auto& upf : _frames) {
                HelloImguiFrame* f = upf.get();
                if (f->_state.load() == HelloImguiFrame::State::Idle) {
                    f->_state.store(HelloImguiFrame::State::Writing);
                    return f;
                }
            }
            // All are rendering; wait until some becomes writable
            _cv.wait(lk, [&]() noexcept {
                if (_needClose) return true;
                for (auto& upf : _frames) {
                    auto st = upf->_state.load();
                    if (st == HelloImguiFrame::State::Idle || st == HelloImguiFrame::State::Ready) return true;
                }
                return false;
            });
            if (_needClose) return nullptr;
            // Retry selection quickly
            if (HelloImguiFrame* sel = try_select_write_index(); sel != nullptr) {
                _writeIndex = (_writeIndex + 1) % _frames.size();
                return sel;
            }
            for (auto& upf : _frames) {
                HelloImguiFrame* f = upf.get();
                auto st = f->_state.load();
                if (st == HelloImguiFrame::State::Ready || st == HelloImguiFrame::State::Idle) {
                    f->_state.store(HelloImguiFrame::State::Writing);
                    return f;
                }
            }
            return nullptr;
        }
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

    std::mutex _mtx;
    std::condition_variable _cv;
    size_t _writeIndex{0};
    size_t _renderIndex{0};
    uint64_t _writeSeqCounter{0};

    radray::unordered_map<radray::render::Texture*, radray::shared_ptr<radray::render::TextureView>> _rtViews;
    radray::vector<radray::unique_ptr<HelloImguiFrame>> _frames;
    std::atomic_bool _needClose{false};
    bool _showDemo{true};
};

int main() {
    radray::InitImGui();
    HelloImguiApp app{radray::render::RenderBackend::D3D12, HelloImguiWaitRenderStrategy::Wait};
    app.RunMainThread();
    radray::FlushLog();
    return 0;
}
