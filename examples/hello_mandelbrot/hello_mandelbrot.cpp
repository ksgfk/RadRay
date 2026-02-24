#include <cmath>

#include <radray/logger.h>
#include <radray/file.h>
#include <radray/imgui/imgui_app.h>
#include <radray/render/dxc.h>
#include <radray/render/bind_bridge.h>
#include <radray/render/render_utility.h>

using namespace radray;

const char* RADRAY_APP_NAME = "Mandelbrot";

class HelloMandelbrotFrame {
public:
    HelloMandelbrotFrame(render::Device* device, render::CommandQueue* queue)
        : _cbArena(device, {256 * 256, device->GetDetail().CBufferAlignment}) {
        _cmdBuffer = device->CreateCommandBuffer(queue).Unwrap();
    }
    HelloMandelbrotFrame(const HelloMandelbrotFrame&) = delete;
    HelloMandelbrotFrame& operator=(const HelloMandelbrotFrame&) = delete;
    ~HelloMandelbrotFrame() noexcept {
        _srv.reset();
        _uav.reset();
        _computeOutput.reset();
        _cbArena.Destroy();
        _cmdBuffer.reset();
    }

    unique_ptr<render::CommandBuffer> _cmdBuffer;
    unique_ptr<render::Texture> _computeOutput;
    unique_ptr<render::TextureView> _uav;
    unique_ptr<render::TextureView> _srv;
    render::TextureUse _computeOutputUsage{render::TextureUse::Uninitialized};
    render::CBufferArena _cbArena;
};

class HelloMandelbrotApp : public ImGuiApplication {
public:
    HelloMandelbrotApp() = default;
    ~HelloMandelbrotApp() noexcept override = default;

    void OnStart(const ImGuiAppConfig& config_) override {
        auto config = config_;
        this->Init(config);
        _touchConn = _window->EventTouch().connect(&HelloMandelbrotApp::OnTouch, this);
        _wheelConn = _window->EventMouseWheel().connect(&HelloMandelbrotApp::OnMouseWheel, this);
        _dxc = render::CreateDxc().Unwrap();
        _frames.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _frames.emplace_back(make_unique<HelloMandelbrotFrame>(_device.get(), _cmdQueue));
        }
        this->OnRecreateSwapChain();
        CompileComputePipeline();
        CompileBlitPipeline();
        _ready = true;
        RADRAY_INFO_LOG("hello_mandelbrot init done");
    }

    void OnDestroy() noexcept override {
        _touchConn.disconnect();
        _wheelConn.disconnect();
        _dxc.reset();
        _computeBinds.clear();
        _computePSO.reset();
        _computeRS.reset();
        _blitBinds.clear();
        _blitPSO.reset();
        _blitRS.reset();
        _frames.clear();
    }

    void OnImGui() override {
        _monitor.OnImGui();
        ImGui::Begin("Mandelbrot");
        ImGui::SliderInt("Max Iter", &_maxIter, 64, 1024);
        ImGui::SliderInt("AA Level", &_aaLevel, 1, 128);
        ImGui::Text("Center: (%.6f, %.6f)", _centerX, _centerY);
        ImGui::Text("Scale: %.6f", _scale);
        ImGui::End();
    }

    void OnUpdate() override {
        _fps.OnUpdate();
        _monitor.SetData(_fps);
    }

    void OnExtractDrawData(uint32_t frameIndex) override {
        ImGuiApplication::OnExtractDrawData(frameIndex);
        if (!_ready) return;
        auto& frame = _frames[frameIndex];
        Eigen::Vector2i rtSize = this->GetRTSize();
        float aspect = (float)rtSize.x() / (float)rtSize.y();
        // Update compute bind: push constant _Params
        auto& cb = _computeBinds[frameIndex];
        auto params = cb->GetCBuffer("_Params");
        params.GetVar("centerX").SetValue((float)_centerX);
        params.GetVar("centerY").SetValue((float)_centerY);
        params.GetVar("scale").SetValue((float)_scale);
        params.GetVar("aspect").SetValue(aspect);
        params.GetVar("maxIter").SetValue((uint32_t)_maxIter);
        params.GetVar("width").SetValue((uint32_t)rtSize.x());
        params.GetVar("height").SetValue((uint32_t)rtSize.y());
        params.GetVar("aaLevel").SetValue((uint32_t)_aaLevel);
        cb->Upload(_device.get(), frame->_cbArena);
        // Update blit bind: set SRV
        _blitBinds[frameIndex]->SetResource("_Tex", frame->_srv.get());
        _blitBinds[frameIndex]->Upload(_device.get(), frame->_cbArena);
    }

    vector<render::CommandBuffer*> OnRender(uint32_t frameIndex) override {
        auto& frame = _frames[frameIndex];
        auto cmdBuffer = frame->_cmdBuffer.get();
        _fps.OnRender();
        auto currBB = _swapchain->GetCurrentBackBufferIndex();
        auto rt = _backBuffers[currBB];
        auto rtView = this->GetDefaultRTV(currBB);
        Eigen::Vector2i rtSize = this->GetRTSize();

        cmdBuffer->Begin();
        _imguiRenderer->OnRenderBegin(frameIndex, cmdBuffer);

        if (_ready && frame->_computeOutput) {
            // 1. Barrier: compute output -> UAV
            {
                render::BarrierTextureDescriptor barrier{
                    frame->_computeOutput.get(),
                    frame->_computeOutputUsage,
                    render::TextureUse::UnorderedAccess};
                cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
                frame->_computeOutputUsage = render::TextureUse::UnorderedAccess;
            }
            // 2. Compute pass
            {
                auto computeEncoder = cmdBuffer->BeginComputePass().Unwrap();
                computeEncoder->BindRootSignature(_computeRS.get());
                computeEncoder->BindComputePipelineState(_computePSO.get());
                computeEncoder->SetThreadGroupSize(8, 8, 1);
                _computeBinds[frameIndex]->Bind(computeEncoder.get());
                uint32_t gx = ((uint32_t)rtSize.x() + 7) / 8;
                uint32_t gy = ((uint32_t)rtSize.y() + 7) / 8;
                computeEncoder->Dispatch(gx, gy, 1);
                cmdBuffer->EndComputePass(std::move(computeEncoder));
            }
            // 3. Barrier: UAV -> SRV
            {
                render::BarrierTextureDescriptor barrier{
                    frame->_computeOutput.get(),
                    render::TextureUse::UnorderedAccess,
                    render::TextureUse::Resource};
                cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
                frame->_computeOutputUsage = render::TextureUse::Resource;
            }
        }

        // 4. Barrier: back buffer -> RenderTarget
        {
            render::BarrierTextureDescriptor barrier{
                rt, render::TextureUse::Uninitialized, render::TextureUse::RenderTarget};
            cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }

        // 5. Blit pass
        {
            render::ColorAttachment rtAttach{
                rtView, render::LoadAction::Clear, render::StoreAction::Store,
                render::ColorClearValue{{{0.0f, 0.0f, 0.0f, 1.0f}}}};
            render::RenderPassDescriptor rpDesc{};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            auto pass = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
            Viewport viewport{0.0f, 0.0f, (float)rtSize.x(), (float)rtSize.y(), 0.0f, 1.0f};
            if (_device->GetBackend() == render::RenderBackend::Vulkan) {
                viewport.Y = (float)rtSize.y();
                viewport.Height = -(float)rtSize.y();
            }
            pass->SetViewport(viewport);
            pass->SetScissor({0, 0, (uint32_t)rtSize.x(), (uint32_t)rtSize.y()});
            if (_ready && frame->_computeOutput) {
                pass->BindRootSignature(_blitRS.get());
                pass->BindGraphicsPipelineState(_blitPSO.get());
                _blitBinds[frameIndex]->Bind(pass.get());
                pass->Draw(3, 1, 0, 0);
            }
            cmdBuffer->EndRenderPass(std::move(pass));
        }

        // 6. ImGui pass
        {
            render::ColorAttachment rtAttach{
                rtView, render::LoadAction::Load, render::StoreAction::Store,
                render::ColorClearValue{}};
            render::RenderPassDescriptor rpDesc{};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            auto pass = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
            _imguiRenderer->OnRender(frameIndex, pass.get());
            cmdBuffer->EndRenderPass(std::move(pass));
        }

        // 7. Barrier: RenderTarget -> Present
        {
            render::BarrierTextureDescriptor barrier{
                rt, render::TextureUse::RenderTarget, render::TextureUse::Present};
            cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        cmdBuffer->End();
        return {cmdBuffer};
    }

    void OnRenderComplete(uint32_t frameIndex) override {
        ImGuiApplication::OnRenderComplete(frameIndex);
        _frames[frameIndex]->_cbArena.Reset();
    }

    void OnRecreateSwapChain() override {
        for (size_t i = 0; i < _frames.size(); i++) {
            auto& frame = _frames[i];
            frame->_srv.reset();
            frame->_uav.reset();
            frame->_computeOutput.reset();
            string name = fmt::format("ComputeOutput_{}", i);
            render::TextureDescriptor texDesc{
                render::TextureDimension::Dim2D,
                (uint32_t)_rtWidth, (uint32_t)_rtHeight, 1, 1, 1,
                render::TextureFormat::RGBA8_UNORM,
                render::MemoryType::Device,
                render::TextureUse::UnorderedAccess | render::TextureUse::Resource,
                render::ResourceHint::None,
                name};
            frame->_computeOutput = _device->CreateTexture(texDesc).Unwrap();
            render::TextureViewDescriptor uavDesc{
                frame->_computeOutput.get(),
                render::TextureDimension::Dim2D,
                render::TextureFormat::RGBA8_UNORM,
                render::SubresourceRange::AllSub(),
                render::TextureUse::UnorderedAccess};
            frame->_uav = _device->CreateTextureView(uavDesc).Unwrap();
            render::TextureViewDescriptor srvDesc{
                frame->_computeOutput.get(),
                render::TextureDimension::Dim2D,
                render::TextureFormat::RGBA8_UNORM,
                render::SubresourceRange::AllSub(),
                render::TextureUse::Resource};
            frame->_srv = _device->CreateTextureView(srvDesc).Unwrap();
            frame->_computeOutputUsage = render::TextureUse::Uninitialized;
        }
        // Update compute binds to point to new UAV views (after resize)
        if (!_computeBinds.empty()) {
            for (size_t i = 0; i < _frames.size(); i++) {
                _computeBinds[i]->SetResource("_Output", _frames[i]->_uav.get());
            }
        }
    }

private:
    void CompileComputePipeline() {
        auto backend = _device->GetBackend();
        auto hlslOpt = file::ReadText(std::filesystem::path("assets") / "hello_mandelbrot" / "mandelbrot.hlsl");
        if (!hlslOpt.has_value()) {
            throw ImGuiApplicationException("Failed to read mandelbrot.hlsl");
        }
        auto csResult = render::CompileComputeShaderFromHLSL(
            _dxc.get(), _device.get(), hlslOpt.value(), backend);
        if (!csResult.has_value()) {
            throw ImGuiApplicationException("Failed to compile mandelbrot.hlsl");
        }
        auto& [csShader, bindLayout] = csResult.value();
        auto rsDesc = bindLayout.GetDescriptor();
        _computeRS = _device->CreateRootSignature(rsDesc.Get()).Unwrap();
        _computeBinds.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _computeBinds.emplace_back(make_unique<render::BindBridge>(_device.get(), _computeRS.get(), bindLayout));
            _computeBinds[i]->SetResource("_Output", _frames[i]->_uav.get());
        }
        render::ComputePipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _computeRS.get();
        psoDesc.CS = render::ShaderEntry{csShader.get(), "CSMain"};
        _computePSO = _device->CreateComputePipelineState(psoDesc).Unwrap();
    }

    void CompileBlitPipeline() {
        auto backend = _device->GetBackend();
        auto hlslOpt = file::ReadText(std::filesystem::path("assets") / "hello_mandelbrot" / "blit.hlsl");
        if (!hlslOpt.has_value()) {
            throw ImGuiApplicationException("Failed to read blit.hlsl");
        }
        render::BindBridgeStaticSampler staticSampler{
            "_Sampler",
            {{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
              render::FilterMode::Nearest, render::FilterMode::Nearest, render::FilterMode::Nearest,
              0.0f, 0.0f, std::nullopt, 0}}};
        auto blitResult = render::CompileShaderFromHLSL(
            _dxc.get(), _device.get(), hlslOpt.value(), backend, {staticSampler});
        if (!blitResult.has_value()) {
            throw ImGuiApplicationException("Failed to compile blit.hlsl");
        }
        auto& [vsShader, psShader, bindLayout] = blitResult.value();
        auto rsDesc = bindLayout.GetDescriptor();
        _blitRS = _device->CreateRootSignature(rsDesc.Get()).Unwrap();
        _blitBinds.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _blitBinds.emplace_back(make_unique<render::BindBridge>(_device.get(), _blitRS.get(), bindLayout));
        }
        render::ColorTargetState rtState = render::ColorTargetState::Default(_rtFormat);
        render::GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _blitRS.get();
        psoDesc.VS = render::ShaderEntry{vsShader.get(), "VSMain"};
        psoDesc.PS = render::ShaderEntry{psShader.get(), "PSMain"};
        psoDesc.VertexLayouts = {};
        psoDesc.Primitive = render::PrimitiveState::Default();
        psoDesc.Primitive.Cull = render::CullMode::None;
        // No depth stencil needed for fullscreen blit
        psoDesc.MultiSample = render::MultiSampleState::Default();
        psoDesc.ColorTargets = std::span{&rtState, 1};
        _blitPSO = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();
    }

    void OnTouch(int x, int y, MouseButton button, Action action) {
        if (!ImGui::GetCurrentContext()) return;
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && action != Action::RELEASED) return;
        switch (action) {
            case Action::PRESSED:
                if (button == MouseButton::BUTTON_LEFT) {
                    _isDragging = true;
                    _lastMouseX = x;
                    _lastMouseY = y;
                }
                break;
            case Action::REPEATED:
                if (_isDragging && button == MouseButton::BUTTON_LEFT) {
                    auto rtSize = this->GetRTSize();
                    float h = (float)std::max(1, rtSize.y());
                    double dx = (double)(x - _lastMouseX) / h * _scale * 2.0;
                    double dy = (double)(y - _lastMouseY) / h * _scale * 2.0;
                    _centerX -= dx;
                    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
                        _centerY -= dy;
                    } else {
                        _centerY += dy;
                    }
                    _lastMouseX = x;
                    _lastMouseY = y;
                }
                break;
            case Action::RELEASED:
                if (button == MouseButton::BUTTON_LEFT) _isDragging = false;
                break;
            default: break;
        }
    }

    void OnMouseWheel(int delta) {
        if (!ImGui::GetCurrentContext()) return;
        if (ImGui::GetIO().WantCaptureMouse) return;
        auto rtSize = this->GetRTSize();
        float w = (float)std::max(1, rtSize.x());
        float h = (float)std::max(1, rtSize.y());
        float aspect = w / h;
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        float ndcX = (mousePos.x / w - 0.5f);
        float ndcY = (mousePos.y / h - 0.5f);
        if (_device->GetBackend() != render::RenderBackend::Vulkan) {
            ndcY = -ndcY;
        }
        double mx = _centerX + (double)ndcX * aspect * _scale * 2.0;
        double my = _centerY + (double)ndcY * _scale * 2.0;
        double factor = (delta > 0) ? 0.85 : 1.0 / 0.85;
        _scale *= factor;
        _scale = std::clamp(_scale, 1e-13, 10.0);
        _centerX = mx - (double)ndcX * aspect * _scale * 2.0;
        _centerY = my - (double)ndcY * _scale * 2.0;
    }

    shared_ptr<render::Dxc> _dxc;
    vector<unique_ptr<HelloMandelbrotFrame>> _frames;
    unique_ptr<render::RootSignature> _computeRS;
    unique_ptr<render::ComputePipelineState> _computePSO;
    vector<unique_ptr<render::BindBridge>> _computeBinds;
    unique_ptr<render::RootSignature> _blitRS;
    unique_ptr<render::GraphicsPipelineState> _blitPSO;
    vector<unique_ptr<render::BindBridge>> _blitBinds;
    double _centerX{-0.5};
    double _centerY{0.0};
    double _scale{1.5};
    int _maxIter{512};
    int _aaLevel{1};
    bool _isDragging{false};
    int _lastMouseX{0};
    int _lastMouseY{0};
    sigslot::scoped_connection _touchConn;
    sigslot::scoped_connection _wheelConn;
    bool _ready{false};
    SimpleFPSCounter _fps{*this, 125};
    SimpleMonitorIMGUI _monitor{*this};
};

unique_ptr<HelloMandelbrotApp> app;

int main(int argc, char** argv) {
    try {
        app = make_unique<HelloMandelbrotApp>();
        ImGuiAppConfig config = HelloMandelbrotApp::ParseArgsSimple(argc, argv);
        config.AppName = string{RADRAY_APP_NAME};
        config.Title = string{RADRAY_APP_NAME};
        app->Setup(config);
        app->Run();
    } catch (std::exception& e) {
        RADRAY_ERR_LOG("Fatal error: {}", e.what());
    } catch (...) {
        RADRAY_ERR_LOG("Fatal unknown error.");
    }
    app->Destroy();
    app.reset();
    FlushLog();
    return 0;
}
