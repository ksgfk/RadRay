#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>

#include <algorithm>
#include <cstring>

using namespace radray;

class ExampleApp : public Application {
    struct FrameResource {
        AppWindow* Window{nullptr};
        render::TextureStates BackBufferState{render::TextureState::Undefined};
        render::Texture* BackBuffer{nullptr};
    };

    struct Frame {
        void Init(ExampleApp* app) {
            CmdBuffer = app->_device->CreateCommandBuffer(app->_renderSystem->_mainQueue).Unwrap();

            render::QueryPoolDescriptor queryDesc{
                .Type = render::QueryType::Timestamp,
                .Count = TimestampQueryCount,
                .DebugName = "ImGui Frame Timestamp Pool"};
            TimestampPool = app->_device->CreateQueryPool(queryDesc).Unwrap();

            render::BufferDescriptor readbackDesc{
                .Size = sizeof(uint64_t) * TimestampQueryCount,
                .Memory = render::MemoryType::ReadBack,
                .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead};
            TimestampReadback = app->_device->CreateBuffer(readbackDesc).Unwrap();
        }

        FrameResource& FindWindowFrameResource(AppWindow* window) {
            auto iter = std::ranges::find_if(WindowTargets, [window](const FrameResource& target) {
                return target.Window == window;
            });
            if (iter != WindowTargets.end()) {
                return *iter;
            }

            FrameResource target;
            target.Window = window;
            WindowTargets.emplace_back(std::move(target));
            return WindowTargets.back();
        }

        unique_ptr<render::CommandBuffer> CmdBuffer;
        unique_ptr<render::QueryPool> TimestampPool;
        unique_ptr<render::Buffer> TimestampReadback;
        bool TimestampPending{false};
        vector<FrameResource> WindowTargets;
    };

    struct ViewportRenderTarget {
        ImGuiSystem::ViewportWindow* ViewportWindow{nullptr};
        render::SwapChain* SwapChain{nullptr};
        render::SwapChainFrame SwapChainFrame;
        render::TextureView* BackBufferView{nullptr};
    };

public:
    static constexpr uint32_t BackBufferCount = 3;
    static constexpr uint32_t FlightDataCount = 2;
    static constexpr uint32_t TimestampQueryCount = 2;
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;

    ExampleApp(int argc, char* argv[]) noexcept {
        _args.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            _args.emplace_back(argv[i]);
        }
    }

    int Run() {
        Init();
        return StartLoop();
    }

    bool AcquireViewportWindow(const AppRenderContext& ctx, ImGuiSystem::ViewportWindow* viewportWindow, vector<ViewportRenderTarget>& renderTargets) {
        if (viewportWindow == nullptr || viewportWindow->Viewport == nullptr || viewportWindow->Window == nullptr) {
            return false;
        }
        if ((viewportWindow->Viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0) {
            return false;
        }
        NativeWindow* window = viewportWindow->GetWindow();
        if (window != nullptr && window->IsMinimized()) {
            return false;
        }

        render::SwapChain* swapChain = viewportWindow->GetSwapChain();
        if (swapChain == nullptr) {
            return false;
        }

        render::SwapChainAcquireResult acquire = ctx.IsInModalLoop ? swapChain->AcquireNext(0) : swapChain->AcquireNext();
        if (acquire.Status == render::SwapChainStatus::RetryLater || acquire.Status == render::SwapChainStatus::RequireRecreate) {
            return false;
        }
        if (acquire.Status != render::SwapChainStatus::Success || !acquire.Frame.has_value()) {
            RADRAY_ERR_LOG("failed to acquire imgui viewport backbuffer: status={}, native={}", acquire.Status, acquire.NativeStatusCode);
            return false;
        }

        render::SwapChainFrame frame = std::move(acquire.Frame.value());
        render::TextureView* backBufferView = viewportWindow->GetCurrentBackBufferView(frame);
        if (backBufferView == nullptr) {
            return false;
        }

        renderTargets.emplace_back(ViewportRenderTarget{
            .ViewportWindow = viewportWindow,
            .SwapChain = swapChain,
            .SwapChainFrame = std::move(frame),
            .BackBufferView = backBufferView});
        return true;
    }

    void RecordViewportWindow(const AppRenderContext& ctx, Frame& frame, ViewportRenderTarget& target, render::CommandBuffer* cmdBuffer) {
        FrameResource& targetResource = frame.FindWindowFrameResource(target.ViewportWindow->Window);
        render::Texture* backBuffer = target.SwapChainFrame.GetBackBuffer();
        if (targetResource.BackBuffer != backBuffer) {
            targetResource.BackBuffer = backBuffer;
            targetResource.BackBufferState = render::TextureState::Undefined;
        }

        render::ResourceBarrierDescriptor toRenderTarget = render::BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = targetResource.BackBufferState,
            .After = render::TextureState::RenderTarget};
        cmdBuffer->ResourceBarrier(std::span{&toRenderTarget, 1});

        render::ColorAttachment colorAttachment{
            .Target = target.BackBufferView,
            .Load = render::LoadAction::Clear,
            .Store = render::StoreAction::Store,
            .ClearValue = render::ColorClearValue{{0.08f, 0.10f, 0.14f, 1.0f}}};
        render::RenderPassDescriptor renderPassDesc{
            .ColorAttachments = std::span{&colorAttachment, 1},
            .Name = target.ViewportWindow->Viewport == ImGui::GetMainViewport() ? "Main ImGui Viewport" : "ImGui Viewport"};
        auto encoderOpt = cmdBuffer->BeginRenderPass(renderPassDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ABORT("failed to begin imgui render pass");
        }
        auto encoder = encoderOpt.Release();
        _imguiSystem->_renderer->OnRenderViewport(ctx.FlightIndex, target.ViewportWindow->Viewport, encoder.get());
        cmdBuffer->EndRenderPass(std::move(encoder));

        render::ResourceBarrierDescriptor toPresent = render::BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = render::TextureState::RenderTarget,
            .After = render::TextureState::Present};
        cmdBuffer->ResourceBarrier(std::span{&toPresent, 1});
        targetResource.BackBufferState = render::TextureState::Present;
    }

    void SubmitFrame(const AppRenderContext& ctx, std::span<ViewportRenderTarget> renderTargets, render::CommandBuffer* cmdBuffer) {
        auto& queueTrack = _renderSystem->_mainQueueTrack;
        render::Fence* frameFence = queueTrack.Fence.get();
        vector<render::SwapChainSyncObject*> waitToExecute;
        vector<render::SwapChainSyncObject*> readyToPresent;
        waitToExecute.reserve(renderTargets.size());
        readyToPresent.reserve(renderTargets.size());
        for (ViewportRenderTarget& target : renderTargets) {
            if (render::SwapChainSyncObject* syncObject = target.SwapChainFrame.GetWaitToDraw()) {
                waitToExecute.emplace_back(syncObject);
            }
            if (render::SwapChainSyncObject* syncObject = target.SwapChainFrame.GetReadyToPresent()) {
                readyToPresent.emplace_back(syncObject);
            }
        }

        render::CommandBuffer* submitCmdBuffers[] = {cmdBuffer};
        render::Fence* signalFences[] = {frameFence};
        uint64_t signalValues[] = {queueTrack.NextFenceValue++};
        render::CommandQueueSubmitDescriptor submitDesc{
            .CmdBuffers = std::span{submitCmdBuffers},
            .SignalFences = std::span{signalFences},
            .SignalValues = std::span{signalValues},
            .WaitToExecute = std::span{waitToExecute},
            .ReadyToPresent = std::span{readyToPresent}};
        _renderSystem->_mainQueue->Submit(submitDesc);
        _renderSystem->_flight[ctx.FlightIndex].Signal = AppRenderSystem::FenceSignal{
            .Fence = frameFence,
            .Value = signalValues[0]};
    }

    void PresentViewportWindow(ViewportRenderTarget& target) {
        render::SwapChainPresentResult present = target.SwapChain->Present(std::move(target.SwapChainFrame));
        if (present.Status == render::SwapChainStatus::RequireRecreate) {
            return;
        }
        if (present.Status != render::SwapChainStatus::Success) {
            RADRAY_ERR_LOG("failed to present imgui viewport: status={}, native={}", present.Status, present.NativeStatusCode);
        }
    }

    void Render(const AppRenderContext& ctx) override {
        if (_imguiSystem == nullptr || _imguiSystem->_renderer == nullptr) {
            return;
        }

        vector<ViewportRenderTarget> renderTargets;
        renderTargets.reserve(_imguiSystem->_viewportWindows.size());
        for (const unique_ptr<ImGuiSystem::ViewportWindow>& viewportWindow : _imguiSystem->_viewportWindows) {
            if (viewportWindow == nullptr) {
                continue;
            }
            AcquireViewportWindow(ctx, viewportWindow.get(), renderTargets);
        }
        if (renderTargets.empty()) {
            return;
        }

        Frame& frame = _frames[ctx.FlightIndex];
        render::CommandBuffer* cmdBuffer = frame.CmdBuffer.get();
        cmdBuffer->Begin();
        cmdBuffer->ResetQueryPool(frame.TimestampPool.get(), 0, TimestampQueryCount);
        cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
            .Pool = frame.TimestampPool.get(),
            .Stage = render::QueryPipelineStage::Top,
            .Index = 0});
        _imguiSystem->_renderer->OnRenderBegin(ctx.FlightIndex, cmdBuffer);
        for (ViewportRenderTarget& target : renderTargets) {
            RecordViewportWindow(ctx, frame, target, cmdBuffer);
        }
        cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
            .Pool = frame.TimestampPool.get(),
            .Stage = render::QueryPipelineStage::Bottom,
            .Index = 1});
        // Vulkan needs explicit transitions around the readback copy; D3D12 READBACK heaps stay in COPY_DEST.
        if (_readbackNeedsBarrier) {
            render::ResourceBarrierDescriptor toCopyDst = render::BarrierBufferDescriptor{
                .Target = frame.TimestampReadback.get(),
                .Before = render::BufferState::Common,
                .After = render::BufferState::CopyDestination};
            cmdBuffer->ResourceBarrier(std::span{&toCopyDst, 1});
        }
        cmdBuffer->ResolveQueryData(render::QueryResolveDescriptor{
            .Pool = frame.TimestampPool.get(),
            .FirstIndex = 0,
            .Count = TimestampQueryCount,
            .Destination = frame.TimestampReadback.get(),
            .DestinationOffset = 0});
        if (_readbackNeedsBarrier) {
            render::ResourceBarrierDescriptor toHostRead = render::BarrierBufferDescriptor{
                .Target = frame.TimestampReadback.get(),
                .Before = render::BufferState::CopyDestination,
                .After = render::BufferState::HostRead};
            cmdBuffer->ResourceBarrier(std::span{&toHostRead, 1});
        }
        cmdBuffer->End();
        frame.TimestampPending = true;

        SubmitFrame(ctx, renderTargets, cmdBuffer);
        for (ViewportRenderTarget& target : renderTargets) {
            PresentViewportWindow(target);
        }
    }

    void OnRenderComplete(const AppRenderCompleteContext& ctx) override {
        if (_imguiSystem != nullptr && _imguiSystem->_renderer != nullptr) {
            _imguiSystem->_renderer->OnRenderComplete(ctx.FlightIndex);
        }
        ResolveGpuTime(ctx.FlightIndex);
    }

    void ResolveGpuTime(uint32_t flightIndex) {
        Frame& frame = _frames[flightIndex];
        if (!frame.TimestampPending) {
            return;
        }
        frame.TimestampPending = false;

        const uint64_t mappedSize = sizeof(uint64_t) * TimestampQueryCount;
        void* mapped = frame.TimestampReadback->Map(0, mappedSize);
        if (mapped == nullptr) {
            return;
        }
        uint64_t ticks[TimestampQueryCount]{};
        std::memcpy(ticks, mapped, mappedSize);
        frame.TimestampReadback->Unmap(0, mappedSize);

        if (ticks[1] <= ticks[0]) {
            return;
        }
        const render::TimestampQueryCalibration calibration =
            frame.TimestampPool->GetTimestampCalibration(_renderSystem->_mainQueue);
        if (calibration.TickPeriodNs <= 0.0) {
            return;
        }
        const double elapsedNs = static_cast<double>(ticks[1] - ticks[0]) * calibration.TickPeriodNs;
        _gpuTimeMs = static_cast<float>(elapsedNs / 1'000'000.0);
    }

    void OnSwapChainRecreate(const AppSwapChainRecreateContext& ctx) override {
        if (_imguiSystem != nullptr && _imguiSystem->_renderer != nullptr) {
            _imguiSystem->_renderer->OnSwapChainRecreate(ctx);
        }
        auto window = ctx.Window;
        for (Frame& frame : _frames) {
            auto iter = std::ranges::find_if(frame.WindowTargets, [window](const FrameResource& target) {
                return target.Window == window;
            });
            if (iter == frame.WindowTargets.end()) {
                continue;
            }
            iter->BackBuffer = nullptr;
            iter->BackBufferState = render::TextureState::Undefined;
        }
    }

    void Init() {
        render::RenderBackend backend = render::RenderBackend::Vulkan;
        bool enableValid = false;
        for (size_t i = 0; i < _args.size(); ++i) {
            if (_args[i] == "--backend" && i + 1 < _args.size()) {
                const auto& backendStr = _args[i + 1];
                if (backendStr == "vulkan") {
                    backend = render::RenderBackend::Vulkan;
                } else if (backendStr == "d3d12") {
                    backend = render::RenderBackend::D3D12;
                }
            }
            if (_args[i] == "--valid-layer") {
                enableValid = true;
            }
        }
        if (backend == render::RenderBackend::Vulkan) {
            render::VulkanInstanceDescriptor insDesc{
                .AppName = "Example ImGui App",
                .EngineName = "RadRay",
                .IsEnableDebugLayer = enableValid,
                .IsEnableGpuBasedValid = false};
            render::InstanceVulkan::InitEnv(insDesc).Unwrap();
            render::VulkanCommandQueueDescriptor queueDesc{render::QueueType::Direct, 1};
            render::VulkanDeviceDescriptor deviceDesc{
                .Queues = std::span{&queueDesc, 1}};
            _device = render::Device::Create(deviceDesc).Unwrap();
        } else if (backend == render::RenderBackend::D3D12) {
            render::DXGIFactoryDescriptor dxgiDesc{
                enableValid,
                enableValid};
            _dxgiFactory = render::DXGIFactory::Create(dxgiDesc).Unwrap();
            render::D3D12DeviceDescriptor deviceDesc{_dxgiFactory.get()};
            _device = render::Device::Create(deviceDesc).Unwrap();
        }
        _readbackNeedsBarrier = _device->GetBackend() == render::RenderBackend::Vulkan;
        AppWindowSystemDescriptor wndSysDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
        wndSysDesc.Type = NativeWindowType::Win32HWND;
#endif
        InitWindowSystem(wndSysDesc);

        AppRenderSystemDescriptor renderSysDesc{
            .Device = _device.get(),
            .MainQueueIndex = 0,
            .BackBufferCount = BackBufferCount,
            .FlightDataCount = FlightDataCount};
        InitRenderSystem(renderSysDesc);
        _frames.resize(renderSysDesc.FlightDataCount);
        for (Frame& frame : _frames) {
            frame.Init(this);
        }

#ifdef RADRAY_PLATFORM_WINDOWS
        Win32WindowCreateDescriptor wndDesc{
            .Title = "Example ImGui App",
            .Width = 1280,
            .Height = 720,
            .Resizable = true,
            .StartVisible = true};
        _mainWindow = _windowSystem->CreateWindow(wndDesc, true);
#else
        RADRAY_ABORT("unsupported platform");
#endif

        render::SwapChainDescriptor swapchainDesc{
            .Width = static_cast<uint32_t>(wndDesc.Width),
            .Format = BackBufferFormat,
            .PresentMode = render::PresentMode::FIFO};
        _mainWindow->AttachSwapChain(swapchainDesc);

        ImGuiSystemDescriptor imguiDesc{
            .MainWindow = _mainWindow,
            .WindowSystem = _windowSystem.get(),
            .Device = _device.get(),
            .RenderTargetFormat = BackBufferFormat,
            .FlightDataCount = renderSysDesc.FlightDataCount,
            .DirectQueue = _renderSystem->_mainQueue,
            .BackBufferCount = renderSysDesc.BackBufferCount,
            .PresentMode = render::PresentMode::FIFO};
        _imguiSystem = ImGuiSystem::Create(imguiDesc).Unwrap();
    }

    int Shutdown(const AppShutdownContext& ctx) override {
        (void)ctx;
        _renderSystem->WaitAndCleanupCompletedFlights();
        _frames.clear();
        _imguiSystem.reset();
        _mainWindow->DetachSwapChain();
        _mainWindow = nullptr;
        ShutdownRenderSystem();
        ShutdownWindowSystem();
        _device.reset();
        _dxgiFactory.reset();
        return 0;
    }

    AppUpdateResult Update(const AppUpdateContext& ctx) override {
        bool shouldClose = _mainWindow->_window->ShouldClose();
        if (shouldClose) {
            return AppUpdateResult{shouldClose};
        }

        bool imguiSucc = _imguiSystem->BeginFrame(ctx.FlightIndex, ctx.DeltaTime.count());
        if (imguiSucc) {
            ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
                                           ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoMove;
            int location = 0;
            constexpr float PAD = 10.0f;
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 workPos = viewport->WorkPos;
            ImVec2 workSize = viewport->WorkSize;
            ImVec2 windowPos;
            ImVec2 windowPosPivot;
            windowPos.x = (location & 1) ? (workPos.x + workSize.x - PAD) : (workPos.x + PAD);
            windowPos.y = (location & 2) ? (workPos.y + workSize.y - PAD) : (workPos.y + PAD);
            windowPosPivot.x = (location & 1) ? 1.0f : 0.0f;
            windowPosPivot.y = (location & 2) ? 1.0f : 0.0f;
            ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPosPivot);
            ImGui::SetNextWindowBgAlpha(0.35f);
            if (ImGui::Begin("RadrayMonitor", &_showMonitor, windowFlags)) {
                ImGui::Text("Delta time: %06.3f ms", ctx.DeltaTime.count() * 1000.0f);
                ImGui::Text("Frame latency: %06.3f ms", ctx.LastFrameLatency.count() * 1000.0f);
                ImGui::Text("GPU time: %06.3f ms", _gpuTimeMs);
                static constexpr render::PresentMode kModes[] = {
                    render::PresentMode::FIFO,
                    render::PresentMode::Mailbox,
                    render::PresentMode::Immediate};
                render::PresentMode currentMode = render::PresentMode::FIFO;
                if (_mainWindow != nullptr && _mainWindow->_swapchain) {
                    currentMode = _mainWindow->_swapchain.Get()->GetDesc().PresentMode;
                }
                std::string preview{render::format_as(currentMode)};
                if (ImGui::BeginCombo("Present Mode", preview.c_str())) {
                    for (render::PresentMode mode : kModes) {
                        std::string item{render::format_as(mode)};
                        const bool selected = mode == currentMode;
                        if (ImGui::Selectable(item.c_str(), selected) && mode != currentMode) {
                            _windowSystem->SetPresentMode(mode);
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::End();

            ImGui::ShowDemoWindow();
            _imguiSystem->EndFrame();
        }
        if (imguiSucc) {
            _imguiSystem->_renderer->ExtractDrawData(ctx.FlightIndex);
        }

        return AppUpdateResult{shouldClose};
    }

    vector<string> _args;
    unique_ptr<render::DXGIFactory> _dxgiFactory;
    shared_ptr<render::Device> _device;
    unique_ptr<ImGuiSystem> _imguiSystem;
    vector<Frame> _frames;

    AppWindow* _mainWindow{nullptr};
    bool _showMonitor{true};
    float _gpuTimeMs{0.0f};
    bool _readbackNeedsBarrier{false};
};

int main(int argc, char* argv[]) {
    ExampleApp app{argc, argv};
    return app.Run();
}
