#include <radray/logger.h>
#include <radray/imgui/imgui_app.h>

const char* RADRAY_APP_NAME = "Hello Dear ImGui";

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
        _monitor.OnImGui();
    }

    void OnUpdate() override {
        _fps.OnUpdate();
        _monitor.SetData(_fps);
    }

    radray::vector<radray::render::CommandBuffer*> OnRender(uint32_t frameIndex) override {
        _fps.OnRender();

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
    radray::vector<radray::unique_ptr<radray::render::CommandBuffer>> _cmdBuffers;
    SimpleFPSCounter _fps{*this, 125};
    SimpleMonitorIMGUI _monitor{*this};
    bool _showDemo{true};
};

radray::unique_ptr<HelloImguiApp> app;

int main(int argc, char** argv) {
    try {
        app = radray::make_unique<HelloImguiApp>();
        radray::ImGuiAppConfig config = HelloImguiApp::ParseArgsSimple(argc, argv);
        config.AppName = radray::string{RADRAY_APP_NAME};
        config.Title = radray::string{RADRAY_APP_NAME};
        app->Setup(config);
        app->Run();
    } catch (std::exception& e) {
        RADRAY_ERR_LOG("Fatal error: {}", e.what());
    } catch (...) {
        RADRAY_ERR_LOG("Fatal unknown error.");
    }
    app->Destroy();
    app.reset();
    radray::FlushLog();
    return 0;
}
