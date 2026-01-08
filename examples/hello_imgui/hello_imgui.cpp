#include <radray/types.h>
#include <radray/logger.h>
#include <radray/platform.h>
#include <radray/channel.h>
#include <radray/stopwatch.h>
#include <radray/window/native_window.h>
#include <radray/render/common.h>
#include <radray/imgui/imgui_app.h>

const char* RADRAY_APP_NAME = "Hello Dear ImGui";

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

class HelloImguiApp : public radray::ImGuiApplication {
public:
    HelloImguiApp() = default;
    ~HelloImguiApp() noexcept override = default;

    void OnStart(const radray::ImGuiAppConfig& config) override {
        this->Init(config);
        _cmdBuffers.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _cmdBuffers.emplace_back(_device->CreateCommandBuffer(_cmdQueue).Unwrap());
        }
    }

    void OnDestroy() noexcept override {
        _cmdBuffers.clear();
    }

    void OnImGui() override {
        if (_showDemo) {
            ImGui::ShowDemoWindow(&_showDemo);
        }
        {
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
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
            window_flags |= ImGuiWindowFlags_NoMove;
            ImGui::SetNextWindowBgAlpha(0.35f);
            if (ImGui::Begin("RadrayMonitor", &_showMonitor, window_flags)) {
                ImGui::Text("CPU: (%09.4f ms) (%.2f fps)", _cpuAvgTime, _cpuFps);
                ImGui::Text("GPU: (%09.4f ms) (%.2f fps)", _gpuAvgTime, _gpuFps);
                ImGui::Separator();
                bool vsync = _enableVSync;
                if (ImGui::Checkbox("VSync", &vsync)) {
                    this->RequestRecreateSwapChain([this, vsync]() { _enableVSync = vsync; });
                }
                ImGui::Checkbox("Frame Drop", &_enableFrameDropping);
                if (ImGui::Checkbox("Multi Thread", &_enableMultiThreading)) {
                    this->RequestReloadLoop();
                }
            }
            ImGui::End();
        }
    }

    void OnUpdate() override {
        _cpuAccum++;
        auto now = _nowCpuTimePoint;
        auto last = _cpuLastPoint;
        auto delta = now - last;
        if (delta >= 125) {
            _cpuAvgTime = delta / _cpuAccum;
            _cpuFps = _cpuAccum / (delta * 0.001);
            _cpuLastPoint = now;
            _cpuAccum = 0;

            auto lastGpuTime = _lastGpuTime.load();
            _gpuAvgTime = lastGpuTime;
            _gpuFps = 1 / (lastGpuTime * 0.001);
        }
    }

    radray::vector<radray::render::CommandBuffer*> OnRender(uint32_t frameIndex) override {
        auto currBackBufferIndex = _swapchain->GetCurrentBackBufferIndex();
        auto cmdBuffer = _cmdBuffers[frameIndex].get();
        auto rt = _backBuffers[currBackBufferIndex];
        auto rtView = this->GetDefaultRTV(currBackBufferIndex);

        cmdBuffer->Begin();
        _imguiRenderer->OnRenderBegin(frameIndex, cmdBuffer);
        {
            radray::render::BarrierTextureDescriptor barrier{};
            barrier.Target = rt;
            barrier.Before = radray::render::TextureUse::Uninitialized;
            barrier.After = radray::render::TextureUse::RenderTarget;
            barrier.IsFromOrToOtherQueue = false;
            barrier.IsSubresourceBarrier = false;
            cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        radray::unique_ptr<radray::render::CommandEncoder> pass;
        {
            radray::render::RenderPassDescriptor rpDesc{};
            radray::render::ColorAttachment rtAttach{};
            rtAttach.Target = rtView;
            rtAttach.Load = radray::render::LoadAction::Clear;
            rtAttach.Store = radray::render::StoreAction::Store;
            rtAttach.ClearValue = radray::render::ColorClearValue{0.1f, 0.1f, 0.1f, 1.0f};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            pass = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        }
        _imguiRenderer->OnRender(frameIndex, pass.get());
        cmdBuffer->EndRenderPass(std::move(pass));
        {
            radray::render::BarrierTextureDescriptor barrier{};
            barrier.Target = rt;
            barrier.Before = radray::render::TextureUse::RenderTarget;
            barrier.After = radray::render::TextureUse::Present;
            barrier.IsFromOrToOtherQueue = false;
            barrier.IsSubresourceBarrier = false;
            cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        cmdBuffer->End();
        return {cmdBuffer};
    }

private:
    uint64_t _cpuAccum{0};
    double _cpuLastPoint{0}, _cpuAvgTime{0}, _cpuFps{0};
    double _gpuAvgTime{0}, _gpuFps{0};

    radray::vector<radray::unique_ptr<radray::render::CommandBuffer>> _cmdBuffers;
    bool _showDemo{true};
    bool _showMonitor{true};
};

radray::unique_ptr<HelloImguiApp> app;

void Init(int argc, char** argv) {
    radray::vector<radray::string> args;
    for (int i = 0; i < argc; i++) {
        args.emplace_back(argv[i]);
    }
    radray::render::RenderBackend backend{radray::render::RenderBackend::Vulkan};
    bool isMultiThread = false, enableValid = false;
    {
        auto bIt = std::find_if(args.begin(), args.end(), [](const radray::string& arg) { return arg == "--backend"; });
        if (bIt != args.end() && (bIt + 1) != args.end()) {
            radray::string backendStr = *(bIt + 1);
            std::transform(backendStr.begin(), backendStr.end(), backendStr.begin(), [](char c) { return std::tolower(c); });
            if (backendStr == "vulkan") {
                backend = radray::render::RenderBackend::Vulkan;
            } else if (backendStr == "d3d12") {
                backend = radray::render::RenderBackend::D3D12;
            } else {
                RADRAY_WARN_LOG("Unsupported backend: {}, using default Vulkan backend.", backendStr);
            }
        }
    }
    {
        auto mtIt = std::find_if(args.begin(), args.end(), [](const radray::string& arg) { return arg == "--multithread"; });
        if (mtIt != args.end()) {
            isMultiThread = true;
        }
    }
    {
        auto validIt = std::find_if(args.begin(), args.end(), [](const radray::string& arg) { return arg == "--valid-layer"; });
        if (validIt != args.end()) {
            enableValid = true;
        }
    }

    app = radray::make_unique<HelloImguiApp>();
    auto name = radray::format("{} - {} {}", radray::string{RADRAY_APP_NAME}, backend, isMultiThread ? "MultiThread" : "");
    radray::ImGuiAppConfig config{
        RADRAY_APP_NAME,
        name,
        1280,
        720,
        backend,
        std::nullopt,
        3,
        2,
        radray::render::TextureFormat::RGBA8_UNORM,
        false,
        isMultiThread,
        false,
        enableValid};
    app->Setup(config);
}

int main(int argc, char** argv) {
    try {
        Init(argc, argv);
        app->Run();
    } catch (std::exception& e) {
        RADRAY_ERR_LOG("Fatal error: {}", e.what());
    } catch (...) {
        RADRAY_ERR_LOG("Fatal unknown error.");
    }
    app->Destroy();
    radray::FlushLog();
    return 0;
}
