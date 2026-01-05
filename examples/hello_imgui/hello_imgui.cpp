#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <stdexcept>
#include <deque>
#include <array>
#include <limits>
#include <algorithm>

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
    explicit HelloImguiApp(radray::render::RenderBackend backend, bool vsync, bool waitFrame, bool multiThread)
        : radray::ImGuiApplication(), backend(backend), vsync(vsync), waitFrame(waitFrame), multiThread(multiThread) {}
    ~HelloImguiApp() noexcept override = default;

    void OnStart() override {
        auto name = radray::format("{} - {} {}", radray::string{RADRAY_APP_NAME}, backend, multiThread ? "MultiThread" : "");
        radray::ImGuiApplicationDescriptor desc{
            name,
            {1280, 720},
            true,
            false,
            backend,
            3,
            2,
            radray::render::TextureFormat::RGBA8_UNORM,
#ifdef RADRAY_IS_DEBUG
            true,
#else
            false,
#endif
            vsync,
            waitFrame,
            multiThread};
        this->Init(desc);

        auto device = _renderContext->GetDevice();
        _cmdBuffers.reserve(desc.FrameCount);
        for (uint32_t i = 0; i < desc.FrameCount; i++) {
            auto q = device->GetCommandQueue(radray::render::QueueType::Direct, 0).Unwrap();
            _cmdBuffers.emplace_back(device->CreateCommandBuffer(q).Unwrap());
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
                //     ImGui::Text("Logic  Time: (%09.4f ms)", _logicTime);
                //     ImGui::Text("Render Time: (%09.4f ms)", _renderTime.load());
                //     ImGui::Separator();
                if (ImGui::Checkbox("VSync", &_enableVSync)) {
                    //         this->ExecuteOnRenderThreadBeforeAcquire([this]() {
                    //             this->RecreateSwapChain();
                    //         });
                }
                //     if (_multithreadRender) {
                //         ImGui::Checkbox("Wait Frame", &_isWaitFrame);
                //     }
            }
            ImGui::End();
        }
    }

    void OnUpdate() override {}

    radray::vector<radray::render::CommandBuffer*> OnRender() override {
        auto currFrameIndex = _renderContext->GetCurrentFrameIndex();
        auto currBackBufferIndex = _renderContext->GetCurrentBackBufferIndex();
        auto cmdBuffer = _cmdBuffers[currFrameIndex].get();
        auto rt = _renderContext->GetBackBuffer(currBackBufferIndex);
        auto rtView = _renderContext->GetBackBufferDefaultRTV(currBackBufferIndex);

        cmdBuffer->Begin();
        _imguiDrawContext->BeforeDraw((int)currFrameIndex, cmdBuffer);
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
        _imguiDrawContext->Draw((int)currFrameIndex, pass.get());
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
        _imguiDrawContext->AfterDraw((int)currFrameIndex);
        return {cmdBuffer};
    }

    void OnResizing(int width, int height) override {
        // ExecuteOnRenderThreadBeforeAcquire([this, width, height]() {
        //     this->_renderRtSize = Eigen::Vector2i{width, height};
        //     this->_isResizingRender = true;
        // });
    }

    void OnResized(int width, int height) override {
        // ExecuteOnRenderThreadBeforeAcquire([this, width, height]() {
        //     this->_renderRtSize = Eigen::Vector2i(width, height);
        //     this->_isResizingRender = false;
        //     if (width > 0 && height > 0) {
        //         this->RecreateSwapChain();
        //     }
        // });
    }

private:
    radray::vector<radray::unique_ptr<radray::render::CommandBuffer>> _cmdBuffers;

    radray::render::RenderBackend backend;
    bool vsync, waitFrame, multiThread;
    bool _showDemo{true};
    bool _showMonitor{true};
};

int main(int argc, char** argv) {
    radray::render::RenderBackend backend{radray::render::RenderBackend::Vulkan};
    bool isMultiThread = true;
    if (argc > 1) {
        radray::string backendStr = argv[1];
        std::transform(backendStr.begin(), backendStr.end(), backendStr.begin(), [](char c) { return std::tolower(c); });
        if (backendStr == "vulkan") {
            backend = radray::render::RenderBackend::Vulkan;
        } else if (backendStr == "d3d12") {
            backend = radray::render::RenderBackend::D3D12;
        } else {
            fmt::print("Unsupported backend: {}, using default Vulkan backend.\n", backendStr);
            return -1;
        }
    }
    if (argc > 2) {
        radray::string mtStr = argv[2];
        if (mtStr == "-st") {
            isMultiThread = false;
        }
    }
    radray::InitImGui();
    {
        HelloImguiApp app{backend, true, false, isMultiThread};
        app.Run();
        app.Destroy();
    }
    radray::FlushLog();
    return 0;
}
