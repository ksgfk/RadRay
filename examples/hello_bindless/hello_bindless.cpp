#include <fstream>

#include <radray/logger.h>
#include <radray/camera_control.h>
#include <radray/file.h>
#include <radray/image_data.h>
#include <radray/imgui/imgui_app.h>
#include <radray/render/dxc.h>
#include <radray/render/resource_binder.h>
#include <radray/render/render_utility.h>

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
    render::TextureState _dsvUsage{render::TextureState::Undefined};
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
            render::ResourceBarrierDescriptor barriers[] = {
                render::BarrierTextureDescriptor{rt, render::TextureState::Undefined, render::TextureState::RenderTarget},
                render::BarrierTextureDescriptor{frame->_depthStencil.get(), frame->_dsvUsage, render::TextureState::DepthWrite}};
            cmdBuffer->ResourceBarrier(barriers);
            frame->_dsvUsage = render::TextureState::DepthWrite;
        }
        unique_ptr<render::GraphicsCommandEncoder> pass;
        {
            render::ColorAttachment rtAttach{
                rtView, render::LoadAction::Clear, render::StoreAction::Store,
                render::ColorClearValue{{{0.2f, 0.2f, 0.2f, 1.0f}}}};
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
            pass->BindBindlessArray(_bindlessSlot, _bindlessArray.get());
            render::VertexBufferView vbv{_vb.get(), 0, _vb->GetDesc().Size};
            render::IndexBufferView ibv{_ib.get(), 0, sizeof(uint16_t)};
            pass->BindVertexBuffer(std::span{&vbv, 1});
            pass->BindIndexBuffer(ibv);
            bind->GetCBuffer("_Obj").GetVar("mvp").SetValue(_mvp);
            bind->GetCBuffer("_Obj").GetVar("baseTexIndex").SetValue((uint32_t)0);
            bind->Bind(pass.get());
            pass->DrawIndexed(6, 1, 0, 0, 0);
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
            render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
                rt, render::TextureState::RenderTarget, render::TextureState::Present};
            cmdBuffer->ResourceBarrier(std::span{&barrier, 1});
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
            string dsName = fmt::format("DepthStencil_{}", i);
            render::TextureDescriptor dsDesc{
                render::TextureDimension::Dim2D,
                (uint32_t)_rtWidth, (uint32_t)_rtHeight, 1, 1, 1,
                render::TextureFormat::D32_FLOAT,
                render::MemoryType::Device,
                render::TextureUse::DepthStencilWrite,
                render::ResourceHint::None,
                dsName};
            frame->_depthStencil = _device->CreateTexture(dsDesc).Unwrap();
            render::TextureViewDescriptor dsvDesc{
                frame->_depthStencil.get(),
                render::TextureDimension::Dim2D,
                dsDesc.Format,
                render::SubresourceRange::AllSub(),
                render::TextureViewUsage::DepthWrite};
            frame->_dsv = _device->CreateTextureView(dsvDesc).Unwrap();
            frame->_dsvUsage = render::TextureState::Undefined;
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
            size_t dstRowPitch = Align(srcRowPitch, (size_t)_device->GetDetail().TextureDataPitchAlignment);
            size_t uploadSize = dstRowPitch * img.Height;
            string texName = string("Tex_") + TEX_NAMES[i];
            render::TextureDescriptor texDesc{
                render::TextureDimension::Dim2D,
                img.Width,
                img.Height,
                1,
                1,
                1,
                render::TextureFormat::RGBA8_UNORM,
                render::MemoryType::Device,
                render::TextureUse::Resource | render::TextureUse::CopyDestination,
                {},
                texName};
            _textures[i] = _device->CreateTexture(texDesc).Unwrap();
            render::TextureViewDescriptor tvDesc{
                _textures[i].get(),
                render::TextureDimension::Dim2D,
                render::TextureFormat::RGBA8_UNORM,
                render::SubresourceRange::AllSub(),
                render::TextureViewUsage::Resource};
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
        vector<render::ResourceBarrierDescriptor> barriers;
        for (uint32_t i = 0; i < TEX_COUNT; i++) {
            barriers.emplace_back(render::BarrierTextureDescriptor{_textures[i].get(), render::TextureState::Undefined, render::TextureState::CopyDestination});
        }
        cmdBuffer->ResourceBarrier(barriers);
        for (uint32_t i = 0; i < TEX_COUNT; i++) {
            render::SubresourceRange range{0, 1, 0, 1};
            cmdBuffer->CopyBufferToTexture(_textures[i].get(), range, _uploadBuffers[i].get(), 0);
        }
        barriers.clear();
        for (uint32_t i = 0; i < TEX_COUNT; i++) {
            barriers.emplace_back(render::BarrierTextureDescriptor{_textures[i].get(), render::TextureState::CopyDestination, render::TextureState::ShaderRead});
        }
        cmdBuffer->ResourceBarrier(barriers);
        _needsUpload = false;
    }

    void CompileAndCreatePipeline() {
        auto backend = _device->GetBackend();
        auto hlslOpt = file::ReadText(std::filesystem::path("assets") / "hello_bindless" / "bindless.hlsl");
        if (!hlslOpt.has_value()) {
            throw ImGuiApplicationException("Failed to read bindless.hlsl");
        }
        string hlsl = std::move(hlslOpt.value());
        render::StaticSamplerBinding staticSampler{
            "_Sampler",
            {{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
              render::FilterMode::Nearest, render::FilterMode::Nearest, render::FilterMode::Nearest,
              0.0f, 0.0f, std::nullopt, 0}}};
        render::PipelineLayout bindLayout;
        unique_ptr<render::Shader> vsShader, psShader;
        {
            auto [VSResult, PSResult, BindLayout] = render::CompileShaderFromHLSL(_dxc.get(), _device.get(), hlsl, backend, {staticSampler}).value();
            vsShader = std::move(VSResult);
            psShader = std::move(PSResult);
            bindLayout = std::move(BindLayout);
        }
        auto rsDesc = bindLayout.GetDescriptor();
        _rs = _device->CreateRootSignature(rsDesc.Get()).Unwrap();
        {
            auto texId = bindLayout.GetParameterId("_Tex");
            if (texId.has_value()) {
                auto mappings = bindLayout.GetMappings();
                const auto& mapping = mappings[texId.value()];
                auto* location = std::get_if<render::DescriptorTableLocation>(&mapping.Location);
                if (!location) {
                    throw ImGuiApplicationException("Binding _Tex is not in descriptor table");
                }
                _bindlessSlot = location->SetIndex;
            } else {
                throw ImGuiApplicationException("Failed to find binding for _Tex");
            }
        }
        _binds.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _binds.emplace_back(std::make_unique<render::ResourceBinder>(_device.get(), _rs.get(), bindLayout));
        }
        render::VertexElement vertElems[] = {
            {0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0},
            {12, "TEXCOORD", 0, render::VertexFormat::FLOAT32X2, 1}};
        render::VertexBufferLayout vertLayout{};
        vertLayout.ArrayStride = sizeof(QuadVertex);
        vertLayout.StepMode = render::VertexStepMode::Vertex;
        vertLayout.Elements = vertElems;
        render::ColorTargetState rtState = render::ColorTargetState::Default(render::TextureFormat::BGRA8_UNORM);
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
        // 单个矩形，UV [0,1]，shader中根据象限选择纹理
        QuadVertex vertices[] = {
            {{-0.5f, 0.5f, 0}, {0, 0}},
            {{0.5f, 0.5f, 0}, {1, 0}},
            {{0.5f, -0.5f, 0}, {1, 1}},
            {{-0.5f, -0.5f, 0}, {0, 1}},
        };
        uint16_t indices[] = {0, 1, 2, 0, 2, 3};
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
    vector<unique_ptr<render::ResourceBinder>> _binds;
    uint32_t _bindlessSlot{0};
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
