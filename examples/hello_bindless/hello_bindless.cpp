#include <fstream>

#include <radray/logger.h>
#include <radray/camera_control.h>
#include <radray/file.h>
#include <radray/image_data.h>
#include <radray/imgui/imgui_app.h>
#include <radray/render/dxc.h>
#include <radray/render/bind_bridge.h>

using namespace radray;

const char* RADRAY_APP_NAME = "Bindless Example";

static constexpr uint32_t TEX_COUNT = 4;
static const char* TEX_NAMES[] = {"a.png", "b.png", "c.png", "d.png"};

struct QuadVertex {
    float pos[3];
    float uv[2];
};

class HelloBindlessFrame {
public:
    HelloBindlessFrame(render::Device* device, render::CommandQueue* queue)
        : _cbArena(device, {256 * 256, device->GetDetail().CBufferAlignment}) {
        _cmdBuffer = device->CreateCommandBuffer(queue).Unwrap();
    }
    HelloBindlessFrame(const HelloBindlessFrame&) = delete;
    HelloBindlessFrame& operator=(const HelloBindlessFrame&) = delete;
    ~HelloBindlessFrame() noexcept {
        _dsv.reset();
        _depthStencil.reset();
        _cbArena.Destroy();
        _cmdBuffer.reset();
    }
    unique_ptr<render::CommandBuffer> _cmdBuffer;
    unique_ptr<render::Texture> _depthStencil;
    unique_ptr<render::TextureView> _dsv;
    render::TextureUse _dsvUsage{render::TextureUse::Uninitialized};
    render::CBufferArena _cbArena;
};

class HelloBindlessApp : public ImGuiApplication {
public:
    HelloBindlessApp() = default;
    ~HelloBindlessApp() noexcept override = default;

    void OnStart(const ImGuiAppConfig& config_) override {
        auto config = config_;
        this->Init(config);
        _cc.SetOrbitTarget(_modelPos);
        _cc.UpdateDistance(_camPos);
        _touchConn = _window->EventTouch().connect(&HelloBindlessApp::OnTouch, this);
        _wheelConn = _window->EventMouseWheel().connect(&HelloBindlessApp::OnMouseWheel, this);
        _dxc = render::CreateDxc().Unwrap();
        _frames.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _frames.emplace_back(make_unique<HelloBindlessFrame>(_device.get(), _cmdQueue));
        }
        this->OnRecreateSwapChain();
        LoadTextures();
        CompileAndCreatePipeline();
        CreateQuadMesh();
        _ready = true;
        RADRAY_INFO_LOG("hello_bindless init done");
    }

    void OnDestroy() noexcept override {
        _touchConn.disconnect();
        _wheelConn.disconnect();
        _dxc.reset();
        _binds.clear();
        _pso.reset();
        _rs.reset();
        _bindlessArray.reset();
        for (auto& b : _uploadBuffers) b.reset();
        for (auto& v : _texViews) v.reset();
        for (auto& t : _textures) t.reset();
        _ib.reset();
        _vb.reset();
        _frames.clear();
    }

    void OnImGui() override { _monitor.OnImGui(); }

    void OnUpdate() override {
        _fps.OnUpdate();
        _monitor.SetData(_fps);
    }

    void OnExtractDrawData(uint32_t frameIndex) override {
        ImGuiApplication::OnExtractDrawData(frameIndex);
        if (!_ready) return;
        Eigen::Vector2i rtSize = this->GetRTSize();
        Eigen::Matrix4f view = LookAt(_camRot, _camPos);
        float aspect = (float)rtSize.x() / (float)rtSize.y();
        Eigen::Matrix4f proj = PerspectiveLH(Radian(_camFovY), aspect, _camNear, _camFar);
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        model.col(3).head<3>() = _modelPos;
        _mvp = proj * view * model;
    }

    vector<render::CommandBuffer*> OnRender(uint32_t frameIndex) override {
        auto& frame = _frames[frameIndex];
        auto cmdBuffer = frame->_cmdBuffer.get();
        _fps.OnRender();
        auto currBB = _swapchain->GetCurrentBackBufferIndex();
        auto rt = _backBuffers[currBB];
        auto rtView = this->GetDefaultRTV(currBB);
        cmdBuffer->Begin();
        _imguiRenderer->OnRenderBegin(frameIndex, cmdBuffer);
        if (_needsUpload) {
            UploadTextures(cmdBuffer);
        }
        {
            render::BarrierTextureDescriptor barriers[] = {
                {rt, render::TextureUse::Uninitialized, render::TextureUse::RenderTarget},
                {frame->_depthStencil.get(), frame->_dsvUsage, render::TextureUse::DepthStencilWrite}};
            cmdBuffer->ResourceBarrier({}, barriers);
            frame->_dsvUsage = render::TextureUse::DepthStencilWrite;
        }
        unique_ptr<render::CommandEncoder> pass;
        {
            render::ColorAttachment rtAttach{
                rtView, render::LoadAction::Clear, render::StoreAction::Store,
                render::ColorClearValue{0.2f, 0.2f, 0.2f, 1.0f}};
            render::DepthStencilAttachment dsAttach{
                frame->_dsv.get(),
                render::LoadAction::Clear,
                render::StoreAction::Discard,
                render::LoadAction::DontCare,
                render::StoreAction::Discard,
                {1.0f, 0}};
            render::RenderPassDescriptor rpDesc{};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            rpDesc.DepthStencilAttachment = dsAttach;
            pass = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        }
        Eigen::Vector2i rtSize = this->GetRTSize();
        Viewport viewport{0.0f, 0.0f, (float)rtSize.x(), (float)rtSize.y(), 0.0f, 1.0f};
        if (_device->GetBackend() == render::RenderBackend::Vulkan) {
            viewport.Y = (float)rtSize.y();
            viewport.Height = -(float)rtSize.y();
        }
        pass->SetViewport(viewport);
        pass->SetScissor({0, 0, (uint32_t)rtSize.x(), (uint32_t)rtSize.y()});
        if (_ready) {
            auto bind = _binds[frameIndex].get();
            bind->Upload(_device.get(), frame->_cbArena);
            pass->BindRootSignature(_rs.get());
            pass->BindGraphicsPipelineState(_pso.get());
            pass->BindBindlessArray(0, _bindlessArray.get());
            render::VertexBufferView vbv{_vb.get(), 0, _vb->GetDesc().Size};
            render::IndexBufferView ibv{_ib.get(), 0, sizeof(uint16_t)};
            pass->BindVertexBuffer(std::span{&vbv, 1});
            pass->BindIndexBuffer(ibv);
            for (uint32_t i = 0; i < TEX_COUNT; i++) {
                bind->GetCBuffer("_Obj").GetVar("mvp").SetValue(_mvp);
                bind->GetCBuffer("_Obj").GetVar("texIndex").SetValue(i);
                bind->Bind(pass.get());
                pass->DrawIndexed(6, 1, i * 6, 0, 0);
            }
        }
        cmdBuffer->EndRenderPass(std::move(pass));
        {
            render::ColorAttachment rtAttach{
                rtView, render::LoadAction::Load, render::StoreAction::Store,
                render::ColorClearValue{}};
            render::RenderPassDescriptor rpDesc{};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            pass = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        }
        _imguiRenderer->OnRender(frameIndex, pass.get());
        cmdBuffer->EndRenderPass(std::move(pass));
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
            auto frame = _frames[i].get();
            frame->_depthStencil.reset();
            frame->_dsv.reset();
            string dsName = format("DepthStencil_{}", i);
            render::TextureDescriptor dsDesc{
                render::TextureDimension::Dim2D,
                (uint32_t)_rtWidth, (uint32_t)_rtHeight, 1, 1, 1,
                render::TextureFormat::D32_FLOAT,
                render::TextureUse::DepthStencilWrite,
                render::ResourceHint::None,
                dsName};
            frame->_depthStencil = _device->CreateTexture(dsDesc).Unwrap();
            render::TextureViewDescriptor dsvDesc{
                frame->_depthStencil.get(),
                render::TextureViewDimension::Dim2D,
                dsDesc.Format,
                render::SubresourceRange::AllSub(),
                render::TextureUse::DepthStencilWrite};
            frame->_dsv = _device->CreateTextureView(dsvDesc).Unwrap();
            frame->_dsvUsage = render::TextureUse::Uninitialized;
        }
    }

private:
    void LoadTextures() {
        auto assetDir = std::filesystem::path("assets");
        for (uint32_t i = 0; i < TEX_COUNT; i++) {
            std::ifstream file(assetDir / TEX_NAMES[i], std::ios::binary);
            if (!file.is_open()) {
                throw ImGuiApplicationException(fmt::format("Failed to open {}", TEX_NAMES[i]));
            }
            PNGLoadSettings settings;
            settings.AddAlphaIfRGB = 255;
            auto imgOpt = ImageData::LoadPNG(file, settings);
            if (!imgOpt.has_value()) {
                throw ImGuiApplicationException(fmt::format("Failed to load {}", TEX_NAMES[i]));
            }
            auto& img = imgOpt.value();
            size_t srcRowPitch = static_cast<size_t>(img.Width) * 4;
            size_t dstRowPitch = srcRowPitch;
            if (_device->GetBackend() == render::RenderBackend::D3D12) {
                // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256
                dstRowPitch = (srcRowPitch + 255) & ~size_t(255);
            }
            size_t uploadSize = dstRowPitch * img.Height;
            string texName = string("Tex_") + TEX_NAMES[i];
            render::TextureDescriptor texDesc{
                render::TextureDimension::Dim2D,
                img.Width,
                img.Height,
                1,
                1,
                0,
                render::TextureFormat::RGBA8_UNORM,
                render::TextureUse::Resource | render::TextureUse::CopyDestination,
                {},
                texName};
            _textures[i] = _device->CreateTexture(texDesc).Unwrap();
            render::TextureViewDescriptor tvDesc{
                _textures[i].get(),
                render::TextureViewDimension::Dim2D,
                render::TextureFormat::RGBA8_UNORM,
                render::SubresourceRange::AllSub(),
                render::TextureUse::Resource};
            _texViews[i] = _device->CreateTextureView(tvDesc).Unwrap();
            string uploadName = string("Upload_") + TEX_NAMES[i];
            render::BufferDescriptor uploadDesc{
                uploadSize, render::MemoryType::Upload, render::BufferUse::CopySource | render::BufferUse::MapWrite, {}, uploadName};
            _uploadBuffers[i] = _device->CreateBuffer(uploadDesc).Unwrap();
            void* dst = _uploadBuffers[i]->Map(0, uploadDesc.Size);
            if (srcRowPitch == dstRowPitch) {
                std::memcpy(dst, img.Data.get(), uploadSize);
            } else {
                auto* dstBytes = static_cast<uint8_t*>(dst);
                auto* srcBytes = reinterpret_cast<const uint8_t*>(img.Data.get());
                for (uint32_t row = 0; row < img.Height; row++) {
                    std::memcpy(dstBytes + row * dstRowPitch, srcBytes + row * srcRowPitch, srcRowPitch);
                }
            }
        }
        render::BindlessArrayDescriptor bdlsDesc{TEX_COUNT, render::BindlessSlotType::Texture2DOnly, "BindlessTex"};
        _bindlessArray = _device->CreateBindlessArray(bdlsDesc).Unwrap();
        for (uint32_t i = 0; i < TEX_COUNT; i++) {
            _bindlessArray->SetTexture(i, _texViews[i].get(), nullptr);
        }
        _needsUpload = true;
    }

    void UploadTextures(render::CommandBuffer* cmdBuffer) {
        vector<render::BarrierTextureDescriptor> barriers;
        for (uint32_t i = 0; i < TEX_COUNT; i++) {
            auto& b = barriers.emplace_back();
            b.Target = _textures[i].get();
            b.Before = render::TextureUse::Uninitialized;
            b.After = render::TextureUse::CopyDestination;
        }
        cmdBuffer->ResourceBarrier({}, barriers);
        for (uint32_t i = 0; i < TEX_COUNT; i++) {
            render::SubresourceRange range{0, 1, 0, 1};
            cmdBuffer->CopyBufferToTexture(_textures[i].get(), range, _uploadBuffers[i].get(), 0);
        }
        barriers.clear();
        for (uint32_t i = 0; i < TEX_COUNT; i++) {
            auto& b = barriers.emplace_back();
            b.Target = _textures[i].get();
            b.Before = render::TextureUse::CopyDestination;
            b.After = render::TextureUse::Resource;
        }
        cmdBuffer->ResourceBarrier({}, barriers);
        _needsUpload = false;
    }

    void CompileAndCreatePipeline() {
        auto backend = _device->GetBackend();
        auto hlslOpt = file::ReadText(std::filesystem::path("assets") / "hello_bindless" / "bindless.hlsl");
        if (!hlslOpt.has_value()) {
            throw ImGuiApplicationException("Failed to read bindless.hlsl");
        }
        string hlsl = std::move(hlslOpt.value());
        vector<std::string_view> defines;
        if (backend == render::RenderBackend::Vulkan) {
            defines.emplace_back("VULKAN");
        } else if (backend == render::RenderBackend::D3D12) {
            defines.emplace_back("D3D12");
        }
        vector<std::string_view> includes;
        includes.emplace_back("shaderlib");
        auto vsOut = _dxc->Compile(
            hlsl, "VSMain", render::ShaderStage::Vertex,
            render::HlslShaderModel::SM60, true, defines, includes,
            backend == render::RenderBackend::Vulkan);
        if (!vsOut.has_value()) throw ImGuiApplicationException("Failed to compile VS");
        auto vsBin = std::move(vsOut.value());
        auto psOut = _dxc->Compile(
            hlsl, "PSMain", render::ShaderStage::Pixel,
            render::HlslShaderModel::SM60, true, defines, includes,
            backend == render::RenderBackend::Vulkan);
        if (!psOut.has_value()) throw ImGuiApplicationException("Failed to compile PS");
        auto psBin = std::move(psOut.value());
        auto vsShader = _device->CreateShader({vsBin.Data, vsBin.Category}).Unwrap();
        auto psShader = _device->CreateShader({psBin.Data, psBin.Category}).Unwrap();
        render::BindBridgeStaticSampler staticSampler;
        staticSampler.Name = "_Sampler";
        staticSampler.Samplers.push_back(render::SamplerDescriptor{
            render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
            render::AddressMode::ClampToEdge,
            render::FilterMode::Linear, render::FilterMode::Linear, render::FilterMode::Linear,
            0.0f, 0.0f, std::nullopt, 0});
        render::BindBridgeLayout bindLayout;
        if (backend == render::RenderBackend::D3D12) {
            auto vsRefl = _dxc->GetShaderDescFromOutput(render::ShaderStage::Vertex, vsBin.Refl, vsBin.ReflExt).value();
            auto psRefl = _dxc->GetShaderDescFromOutput(render::ShaderStage::Pixel, psBin.Refl, psBin.ReflExt).value();
            const render::HlslShaderDesc* descs[] = {&vsRefl, &psRefl};
            auto merged = render::MergeHlslShaderDesc(descs).value();
            bindLayout = render::BindBridgeLayout{merged, std::span{&staticSampler, 1}};
        } else if (backend == render::RenderBackend::Vulkan) {
            render::SpirvBytecodeView spvs[] = {
                {vsBin.Data, "VSMain", render::ShaderStage::Vertex},
                {psBin.Data, "PSMain", render::ShaderStage::Pixel}};
            const render::DxcReflectionRadrayExt* extInfos[] = {&vsBin.ReflExt, &psBin.ReflExt};
            auto spirvDesc = render::ReflectSpirv(spvs, extInfos).value();
            bindLayout = render::BindBridgeLayout{spirvDesc, std::span{&staticSampler, 1}};
        }
        auto rsDesc = bindLayout.GetDescriptor();
        _rs = _device->CreateRootSignature(rsDesc.Get()).Unwrap();
        _binds.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _binds.emplace_back(std::make_unique<render::BindBridge>(_device.get(), _rs.get(), bindLayout));
        }
        render::VertexElement vertElems[] = {
            {0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0},
            {12, "TEXCOORD", 0, render::VertexFormat::FLOAT32X2, 1}};
        render::VertexBufferLayout vertLayout{};
        vertLayout.ArrayStride = sizeof(QuadVertex);
        vertLayout.StepMode = render::VertexStepMode::Vertex;
        vertLayout.Elements = vertElems;
        render::ColorTargetState rtState = render::ColorTargetState::Default(render::TextureFormat::RGBA8_UNORM);
        render::GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _rs.get();
        psoDesc.VS = render::ShaderEntry{vsShader.get(), "VSMain"};
        psoDesc.PS = render::ShaderEntry{psShader.get(), "PSMain"};
        psoDesc.VertexLayouts = std::span{&vertLayout, 1};
        psoDesc.Primitive = render::PrimitiveState::Default();
        psoDesc.Primitive.Cull = render::CullMode::None;
        psoDesc.DepthStencil = render::DepthStencilState::Default();
        psoDesc.MultiSample = render::MultiSampleState::Default();
        psoDesc.ColorTargets = std::span{&rtState, 1};
        _pso = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();
    }

    void CreateQuadMesh() {
        // 4 sub-quads forming a 1x1 plane: top-left, top-right, bottom-left, bottom-right
        // Each sub-quad is 0.5x0.5 with full UV range
        QuadVertex vertices[] = {
            // quad 0 top-left (a.png)
            {{-0.5f, 0.5f, 0}, {0, 0}},
            {{0, 0.5f, 0}, {1, 0}},
            {{0, 0, 0}, {1, 1}},
            {{-0.5f, 0, 0}, {0, 1}},
            // quad 1 top-right (b.png)
            {{0, 0.5f, 0}, {0, 0}},
            {{0.5f, 0.5f, 0}, {1, 0}},
            {{0.5f, 0, 0}, {1, 1}},
            {{0, 0, 0}, {0, 1}},
            // quad 2 bottom-left (c.png)
            {{-0.5f, 0, 0}, {0, 0}},
            {{0, 0, 0}, {1, 0}},
            {{0, -0.5f, 0}, {1, 1}},
            {{-0.5f, -0.5f, 0}, {0, 1}},
            // quad 3 bottom-right (d.png)
            {{0, 0, 0}, {0, 0}},
            {{0.5f, 0, 0}, {1, 0}},
            {{0.5f, -0.5f, 0}, {1, 1}},
            {{0, -0.5f, 0}, {0, 1}},
        };
        uint16_t indices[] = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7,
            8, 9, 10, 8, 10, 11,
            12, 13, 14, 12, 14, 15};
        render::BufferDescriptor vbDesc{
            sizeof(vertices), render::MemoryType::Upload, render::BufferUse::Vertex | render::BufferUse::MapWrite, {}, "QuadVB"};
        _vb = _device->CreateBuffer(vbDesc).Unwrap();
        void* vbDst = _vb->Map(0, sizeof(vertices));
        std::memcpy(vbDst, vertices, sizeof(vertices));
        render::BufferDescriptor ibDesc{
            sizeof(indices), render::MemoryType::Upload, render::BufferUse::Index | render::BufferUse::MapWrite, {}, "QuadIB"};
        _ib = _device->CreateBuffer(ibDesc).Unwrap();
        void* ibDst = _ib->Map(0, sizeof(indices));
        std::memcpy(ibDst, indices, sizeof(indices));
    }

    Eigen::Vector2f NormalizeMousePos(int x, int y) const {
        if (!_window) return Eigen::Vector2f::Zero();
        const auto size = _window->GetSize();
        const float w = (float)std::max(1, size.X);
        const float h = (float)std::max(1, size.Y);
        return {(static_cast<float>(x) / w) * 2.0f - 1.0f,
                (static_cast<float>(y) / h) * 2.0f - 1.0f};
    }

    void OnTouch(int x, int y, MouseButton button, Action action) {
        if (!ImGui::GetCurrentContext()) return;
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && action != Action::RELEASED) return;
        _cc.CurrentMousePos = NormalizeMousePos(x, y);
        switch (action) {
            case Action::PRESSED:
                _cc.LastMousePos = _cc.CurrentMousePos;
                if (button == MouseButton::BUTTON_LEFT) {
                    _cc.IsOrbiting = true;
                    _cc.IsPanning = false;
                    _cc.IsDollying = false;
                } else if (button == MouseButton::BUTTON_MIDDLE) {
                    _cc.IsPanning = true;
                    _cc.IsOrbiting = false;
                    _cc.IsDollying = false;
                } else if (button == MouseButton::BUTTON_RIGHT) {
                    _cc.IsDollying = true;
                    _cc.IsOrbiting = false;
                    _cc.IsPanning = false;
                }
                break;
            case Action::REPEATED:
                if (_cc.IsOrbiting)
                    _cc.Orbit(_camPos, _camRot);
                else if (_cc.IsPanning)
                    _cc.Pan(_camPos, _camRot);
                else if (_cc.IsDollying) {
                    const Eigen::Vector2f delta = _cc.CurrentMousePos - _cc.LastMousePos;
                    _cc.WheelDelta = -delta.y() * 5.0f;
                    _cc.Dolly(_camPos, _camRot);
                    _cc.LastMousePos = _cc.CurrentMousePos;
                }
                break;
            case Action::RELEASED:
                if (button == MouseButton::BUTTON_LEFT)
                    _cc.IsOrbiting = false;
                else if (button == MouseButton::BUTTON_MIDDLE)
                    _cc.IsPanning = false;
                else if (button == MouseButton::BUTTON_RIGHT)
                    _cc.IsDollying = false;
                _cc.LastMousePos = _cc.CurrentMousePos;
                break;
            default: break;
        }
    }

    void OnMouseWheel(int delta) {
        if (!ImGui::GetCurrentContext()) return;
        if (ImGui::GetIO().WantCaptureMouse) return;
        _cc.WheelDelta = static_cast<float>(delta) / 120.0f;
        _cc.Dolly(_camPos, _camRot);
    }

    shared_ptr<render::Dxc> _dxc;
    vector<unique_ptr<HelloBindlessFrame>> _frames;
    // camera
    Eigen::Vector3f _camPos{0.0f, 0.0f, -2.0f};
    Eigen::Quaternionf _camRot{Eigen::Quaternionf::Identity()};
    float _camFovY{45.0f};
    float _camNear{0.1f};
    float _camFar{100.0f};
    CameraControl _cc;
    sigslot::scoped_connection _touchConn;
    sigslot::scoped_connection _wheelConn;
    Eigen::Vector3f _modelPos{0.0f, 0.0f, 0.0f};
    // pipeline
    unique_ptr<render::RootSignature> _rs;
    unique_ptr<render::GraphicsPipelineState> _pso;
    vector<unique_ptr<render::BindBridge>> _binds;
    unique_ptr<render::BindlessArray> _bindlessArray;
    // textures
    unique_ptr<render::Texture> _textures[TEX_COUNT];
    unique_ptr<render::TextureView> _texViews[TEX_COUNT];
    unique_ptr<render::Buffer> _uploadBuffers[TEX_COUNT];
    bool _needsUpload{false};
    // mesh
    unique_ptr<render::Buffer> _vb;
    unique_ptr<render::Buffer> _ib;
    // state
    bool _ready{false};
    Eigen::Matrix4f _mvp{Eigen::Matrix4f::Identity()};
    SimpleFPSCounter _fps{*this, 125};
    SimpleMonitorIMGUI _monitor{*this};
};

unique_ptr<HelloBindlessApp> app;

int main(int argc, char** argv) {
    try {
        app = make_unique<HelloBindlessApp>();
        ImGuiAppConfig config = HelloBindlessApp::ParseArgsSimple(argc, argv);
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
