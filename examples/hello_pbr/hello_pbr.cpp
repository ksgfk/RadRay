#include <radray/logger.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/camera_control.h>
#include <radray/file.h>
#include <radray/imgui/imgui_app.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/msl.h>
#include <radray/render/gpu_resource.h>
#include <radray/render/bind_bridge.h>
#include <radray/render/render_utility.h>

using namespace radray;

const char* RADRAY_APP_NAME = "PBR Example";

class HelloPBRFrame {
public:
    class DrawMesh {
    public:
        render::RootSignature* _rs;
        render::GraphicsPipelineState* _pso;
        render::BindBridge* _bind;
        render::RenderMesh* _mesh;
    };

    HelloPBRFrame(render::Device* device, render::CommandQueue* queue)
        : _cbArena(device, {256 * 256, device->GetDetail().CBufferAlignment}) {
        _cmdBuffer = device->CreateCommandBuffer(queue).Unwrap();
    }
    HelloPBRFrame(const HelloPBRFrame&) = delete;
    HelloPBRFrame& operator=(const HelloPBRFrame&) = delete;
    HelloPBRFrame(HelloPBRFrame&&) = delete;
    HelloPBRFrame& operator=(HelloPBRFrame&&) = delete;
    ~HelloPBRFrame() noexcept {
        _dsv.reset();
        _depthStencil.reset();
        _cbArena.Destroy();
        _cmdBuffer.reset();
    }

    unique_ptr<render::CommandBuffer> _cmdBuffer;
    unique_ptr<render::Texture> _depthStencil;
    unique_ptr<render::TextureView> _dsv;
    render::TextureUse _dsvUsage;
    render::CBufferArena _cbArena;
    vector<DrawMesh> _drawDatas;
};

class HelloPBRApp : public ImGuiApplication {
public:
    HelloPBRApp() = default;
    ~HelloPBRApp() noexcept override = default;

    void OnStart(const ImGuiAppConfig& config_) override {
        auto config = config_;
        this->Init(config);

        _cc.SetOrbitTarget(_modelPos);
        _cc.UpdateDistance(_camPos);

        _touchConn = _window->EventTouch().connect(&HelloPBRApp::OnTouch, this);
        _wheelConn = _window->EventMouseWheel().connect(&HelloPBRApp::OnMouseWheel, this);

        _dxc = render::CreateDxc().Unwrap();
        _frames.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _frames.emplace_back(make_unique<HelloPBRFrame>(_device.get(), _cmdQueue));
        }
        this->OnRecreateSwapChain();

        Load();
    }

    void OnDestroy() noexcept override {
        _touchConn.disconnect();
        _wheelConn.disconnect();

        _dxc.reset();

        _binds.clear();
        _meshUploadBuffers.clear();
        _renderMesh.reset();
        _pso.reset();
        _rs.reset();

        _frames.clear();
    }

    void OnImGui() override {
        _monitor.OnImGui();
    }

    void OnUpdate() override {
        _fps.OnUpdate();
        _monitor.SetData(_fps);
    }

    void OnExtractDrawData(uint32_t frameIndex) override {
        ImGuiApplication::OnExtractDrawData(frameIndex);

        auto& frame = _frames[frameIndex];
        auto& cbArena = frame->_cbArena;
        if (_ready) {
            Eigen::Vector2i rtSize = this->GetRTSize();
            Eigen::Matrix4f view = LookAt(_camRot, _camPos);
            Eigen::Matrix4f proj = PerspectiveLH(Radian(_camFovY), (float)rtSize.x() / rtSize.y(), _camNear, _camFar);
            Eigen::Matrix4f model = (Eigen::Translation3f{_modelPos} * _modelRot.toRotationMatrix() * Eigen::AlignedScaling3f{_modelScale}).matrix();
            Eigen::Matrix4f modelInv = model.inverse();
            Eigen::Matrix4f vp = proj * view;
            Eigen::Matrix4f mvp = vp * model;
            auto bind = _binds[frameIndex].get();
            auto objCb = bind->GetCBuffer("_Obj");
            auto cameraCb = bind->GetCBuffer("_Camera");
            if (!objCb || !cameraCb) {
                RADRAY_ERR_LOG("PBR cbuffer missing: _Obj valid={}, _Camera valid={}", objCb.IsValid(), cameraCb.IsValid());
                return;
            }
            objCb.GetVar("model").SetValue(model);
            objCb.GetVar("mvp").SetValue(mvp);
            objCb.GetVar("modelInv").SetValue(modelInv);
            cameraCb.GetVar("view").SetValue(view);
            cameraCb.GetVar("proj").SetValue(proj);
            cameraCb.GetVar("viewProj").SetValue(vp);
            cameraCb.GetVar("posW").SetValue(_camPos);
            bind->Upload(_device.get(), cbArena);
            auto& drawData = frame->_drawDatas.emplace_back();
            drawData._rs = _rs.get();
            drawData._pso = _pso.get();
            drawData._bind = bind;
            drawData._mesh = _renderMesh.get();
        }
    }

    vector<render::CommandBuffer*> OnRender(uint32_t frameIndex) override {
        auto& frame = _frames[frameIndex];
        auto cmdBuffer = frame->_cmdBuffer.get();
        // auto& cbArena = frame->_cbArena;

        _fps.OnRender();

        auto currBackBufferIndex = _swapchain->GetCurrentBackBufferIndex();
        auto rt = _backBuffers[currBackBufferIndex];
        auto rtView = this->GetDefaultRTV(currBackBufferIndex);

        cmdBuffer->Begin();
        _imguiRenderer->OnRenderBegin(frameIndex, cmdBuffer);
        if (_needsMeshUpload) {
            for (size_t i = 0; i < _meshUploadBuffers.size(); i++) {
                auto* src = _meshUploadBuffers[i].get();
                auto* dst = _renderMesh->_buffers[i].get();
                cmdBuffer->CopyBufferToBuffer(dst, 0, src, 0, src->GetDesc().Size);
            }
            vector<render::BarrierBufferDescriptor> bufBarriers;
            for (auto& buf : _renderMesh->_buffers) {
                auto& b = bufBarriers.emplace_back();
                b.Target = buf.get();
                b.Before = render::BufferUse::CopyDestination;
                b.After = render::BufferUse::Vertex | render::BufferUse::Index;
            }
            cmdBuffer->ResourceBarrier(bufBarriers, {});
            _needsMeshUpload = false;
        }
        {
            render::BarrierTextureDescriptor barriers[] = {
                {rt,
                 render::TextureUse::Uninitialized,
                 render::TextureUse::RenderTarget},
                {frame->_depthStencil.get(),
                 frame->_dsvUsage,
                 render::TextureUse::DepthStencilWrite}};
            cmdBuffer->ResourceBarrier({}, barriers);
            frame->_dsvUsage = render::TextureUse::DepthStencilWrite;
        }
        unique_ptr<render::GraphicsCommandEncoder> pass;
        {
            render::RenderPassDescriptor rpDesc{};
            render::ColorAttachment rtAttach{
                rtView,
                render::LoadAction::Clear,
                render::StoreAction::Store,
                render::ColorClearValue{{{0.0f, 0.0f, 0.0f, 1.0f}}}};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            render::DepthStencilAttachment dsAttach{
                frame->_dsv.get(),
                render::LoadAction::Clear,
                render::StoreAction::Store,
                render::LoadAction::DontCare,
                render::StoreAction::Discard,
                {1.0f, 0}};
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
        for (auto& drawData : frame->_drawDatas) {
            pass->BindRootSignature(drawData._rs);
            pass->BindGraphicsPipelineState(drawData._pso);
            drawData._bind->Bind(pass.get());
            for (const auto& meshDraw : drawData._mesh->_drawDatas) {
                auto vbv = meshDraw.Vbv;
                pass->BindVertexBuffer(std::span{&vbv, 1});
                auto ibv = meshDraw.Ibv;
                pass->BindIndexBuffer(ibv);
                uint64_t ibSize = ibv.Target->GetDesc().Size;
                uint64_t offset = ibv.Offset;
                uint64_t count = (ibSize > offset) ? ((ibSize - offset) / ibv.Stride) : 0;
                pass->DrawIndexed(static_cast<uint32_t>(count), 1, 0, 0, 0);
            }
        }
        cmdBuffer->EndRenderPass(std::move(pass));
        {
            render::RenderPassDescriptor rpDesc{};
            render::ColorAttachment rtAttach{
                rtView,
                render::LoadAction::Load,
                render::StoreAction::Store,
                render::ColorClearValue{}};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            pass = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        }
        _imguiRenderer->OnRender(frameIndex, pass.get());
        cmdBuffer->EndRenderPass(std::move(pass));
        {
            render::BarrierTextureDescriptor barrier{
                rt,
                render::TextureUse::RenderTarget,
                render::TextureUse::Present};
            cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        cmdBuffer->End();
        return {cmdBuffer};
    }

    void OnRenderComplete(uint32_t frameIndex) override {
        ImGuiApplication::OnRenderComplete(frameIndex);

        _meshUploadBuffers.clear();
        auto& frame = _frames[frameIndex];
        frame->_drawDatas.clear();
        frame->_cbArena.Reset();
    }

    void OnRecreateSwapChain() override {
        for (size_t i = 0; i < _frames.size(); i++) {
            auto frame = _frames[i].get();
            frame->_depthStencil.reset();
            frame->_dsv.reset();
            string dsName = fmt::format("DepthStencil_{}", i);
            render::TextureDescriptor dsDesc{
                render::TextureDimension::Dim2D,
                (uint32_t)_rtWidth,
                (uint32_t)_rtHeight,
                1,
                1,
                1,
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
                render::TextureUse::DepthStencilWrite};
            frame->_dsv = _device->CreateTextureView(dsvDesc).Unwrap();
            frame->_dsvUsage = render::TextureUse::Uninitialized;
        }
    }

    void Load() {
        auto backend = _device->GetBackend();
        TriangleMesh sphereMesh{};
        sphereMesh.InitAsUVSphere(0.5f, 128);
        MeshResource sphereModel{};
        sphereMesh.ToSimpleMeshResource(&sphereModel);

        _renderMesh = std::make_unique<render::RenderMesh>();
        _renderMesh->_buffers.reserve(sphereModel.Bins.size());
        _meshUploadBuffers.reserve(sphereModel.Bins.size());
        for (const auto& bufData : sphereModel.Bins) {
            auto data = bufData.GetData();
            render::BufferDescriptor uploadDesc{
                bufData.GetSize(),
                render::MemoryType::Upload,
                render::BufferUse::CopySource | render::BufferUse::MapWrite,
                {},
                {}};
            auto uploadBuf = _device->CreateBuffer(uploadDesc).Unwrap();
            void* dst = uploadBuf->Map(0, data.size());
            std::memcpy(dst, data.data(), data.size());
            _meshUploadBuffers.emplace_back(std::move(uploadBuf));
            render::BufferDescriptor deviceDesc{
                bufData.GetSize(),
                render::MemoryType::Device,
                render::BufferUse::CopyDestination | render::BufferUse::Vertex | render::BufferUse::Index,
                {},
                {}};
            _renderMesh->_buffers.emplace_back(_device->CreateBuffer(deviceDesc).Unwrap());
        }
        _renderMesh->_drawDatas.reserve(sphereModel.Primitives.size());
        for (const auto& primitive : sphereModel.Primitives) {
            render::RenderMesh::DrawData drawData{};
            if (!primitive.VertexBuffers.empty()) {
                const auto& vbEntry = primitive.VertexBuffers.front();
                if (vbEntry.BufferIndex < _renderMesh->_buffers.size()) {
                    auto* vb = _renderMesh->_buffers[vbEntry.BufferIndex].get();
                    uint64_t vbSize = vb->GetDesc().Size;
                    uint64_t viewSize = vbSize;
                    if (primitive.VertexCount > 0 && vbEntry.Stride > 0) {
                        uint64_t required = static_cast<uint64_t>(primitive.VertexCount) * vbEntry.Stride;
                        if (required > 0) {
                            viewSize = std::min(vbSize, required);
                        }
                    }
                    drawData.Vbv = {vb, 0, viewSize};
                }
            }
            const auto& ibEntry = primitive.IndexBuffer;
            if (ibEntry.BufferIndex < _renderMesh->_buffers.size()) {
                auto* ib = _renderMesh->_buffers[ibEntry.BufferIndex].get();
                drawData.Ibv = {ib, ibEntry.Offset, ibEntry.Stride};
            }
            _renderMesh->_drawDatas.emplace_back(drawData);
        }
        _needsMeshUpload = true;

        unique_ptr<render::Shader> vsShader, psShader;
        {
            string hlsl;
            {
                auto hlslOpt = file::ReadText(std::filesystem::path("assets") / "hello_pbr" / "pbr.hlsl");
                if (!hlslOpt.has_value()) {
                    throw ImGuiApplicationException("Failed to read shader file pbr.hlsl");
                }
                hlsl = std::move(hlslOpt.value());
            }
            vector<std::string_view> defines;
            if (backend == render::RenderBackend::Vulkan) {
                defines.emplace_back("VULKAN");
            } else if (backend == render::RenderBackend::D3D12) {
                defines.emplace_back("D3D12");
            } else if (backend == render::RenderBackend::Metal) {
                defines.emplace_back("METAL");
            } else {
                throw ImGuiApplicationException("unsupported render backend for shader compilation");
            }
            vector<std::string_view> includes;
            includes.emplace_back("shaderlib");
            render::DxcOutput vsBin;
            {
                auto vs = _dxc->Compile(hlsl, "VSMain", render::ShaderStage::Vertex, render::HlslShaderModel::SM60, true, defines, includes, backend != render::RenderBackend::D3D12);
                if (!vs.has_value()) {
                    throw ImGuiApplicationException("Failed to compile vertex shader");
                }
                vsBin = std::move(vs.value());
            }
            render::DxcOutput psBin;
            {
                auto ps = _dxc->Compile(hlsl, "PSMain", render::ShaderStage::Pixel, render::HlslShaderModel::SM60, true, defines, includes, backend != render::RenderBackend::D3D12);
                if (!ps.has_value()) {
                    throw ImGuiApplicationException("Failed to compile pixel shader");
                }
                psBin = std::move(ps.value());
            }
            if (backend == render::RenderBackend::Metal) {
                render::SpirvToMslOption mslOption{
                    3,
                    0,
                    0,
#ifdef RADRAY_PLATFORM_MACOS
                    render::MslPlatform::MacOS,
#elif defined(RADRAY_PLATFORM_IOS)
                    render::MslPlatform::IOS,
#endif
                    true,
                    false};
                auto vsMsl = render::ConvertSpirvToMsl(vsBin.Data, "VSMain", render::ShaderStage::Vertex, mslOption).value();
                render::ShaderDescriptor vsDesc{vsMsl.GetBlob(), render::ShaderBlobCategory::MSL};
                vsShader = _device->CreateShader(vsDesc).Unwrap();
                auto psMsl = render::ConvertSpirvToMsl(psBin.Data, "PSMain", render::ShaderStage::Pixel, mslOption).value();
                render::ShaderDescriptor psDesc{psMsl.GetBlob(), render::ShaderBlobCategory::MSL};
                psShader = _device->CreateShader(psDesc).Unwrap();
            } else {
                render::ShaderDescriptor vsDesc{vsBin.Data, vsBin.Category};
                vsShader = _device->CreateShader(vsDesc).Unwrap();
                render::ShaderDescriptor psDesc{psBin.Data, psBin.Category};
                psShader = _device->CreateShader(psDesc).Unwrap();
            }

            render::BindBridgeLayout bindLayout;
            if (backend == render::RenderBackend::D3D12) {
                auto vsRefl = _dxc->GetShaderDescFromOutput(render::ShaderStage::Vertex, vsBin.Refl, vsBin.ReflExt).value();
                auto psRefl = _dxc->GetShaderDescFromOutput(render::ShaderStage::Pixel, psBin.Refl, psBin.ReflExt).value();
                const render::HlslShaderDesc* descs[] = {&vsRefl, &psRefl};
                auto mergedDesc = render::MergeHlslShaderDesc(descs).value();
                bindLayout = render::BindBridgeLayout{mergedDesc};
            } else if (backend == render::RenderBackend::Vulkan) {
                render::SpirvBytecodeView spvs[] = {
                    {vsBin.Data, "VSMain", render::ShaderStage::Vertex},
                    {psBin.Data, "PSMain", render::ShaderStage::Pixel}};
                const render::DxcReflectionRadrayExt* extInfos[] = {&vsBin.ReflExt, &psBin.ReflExt};
                auto spirvDesc = render::ReflectSpirv(spvs, extInfos).value();
                bindLayout = render::BindBridgeLayout{spirvDesc};
            } else if (backend == render::RenderBackend::Metal) {
                render::MslReflectParams mslParams[] = {
                    {vsBin.Data, "VSMain", render::ShaderStage::Vertex, true},
                    {psBin.Data, "PSMain", render::ShaderStage::Pixel, true}};
                auto mslRefl = render::ReflectMsl(mslParams).value();
                bindLayout = render::BindBridgeLayout{mslRefl};
            } else {
                throw ImGuiApplicationException("unsupported render backend for shader reflection");
            }
            auto rsDesc = bindLayout.GetDescriptor();
            _rs = _device->CreateRootSignature(rsDesc.Get()).Unwrap();
            _binds.reserve(_inFlightFrameCount);
            for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
                _binds.emplace_back(std::make_unique<render::BindBridge>(_device.get(), _rs.get(), bindLayout));
            }
        }
        const auto& prim = sphereModel.Primitives[0];
        vector<render::VertexElement> vertElems;
        {
            render::SemanticMapping mapping[] = {
                {VertexSemantics::POSITION, 0, 0, render::VertexFormat::FLOAT32X3},
                {VertexSemantics::NORMAL, 0, 1, render::VertexFormat::FLOAT32X3},
                {VertexSemantics::TEXCOORD, 0, 2, render::VertexFormat::FLOAT32X2}};
            auto mves = render::MapVertexElements(prim.VertexBuffers, mapping);
            if (!mves.has_value()) {
                throw ImGuiApplicationException("failed to map vertex elements");
            }
            vertElems = std::move(mves.value());
        }
        uint32_t stride = 0;
        for (const auto& vb : prim.VertexBuffers) {
            stride += vertex_utility::GetVertexDataSizeInBytes(vb.Type, vb.ComponentCount);
        }
        render::VertexBufferLayout vertLayout{};
        vertLayout.ArrayStride = stride;
        vertLayout.StepMode = render::VertexStepMode::Vertex;
        vertLayout.Elements = vertElems;
        render::ColorTargetState rtState = render::ColorTargetState::Default(render::TextureFormat::BGRA8_UNORM);
        render::GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _rs.get();
        psoDesc.VS = {vsShader.get(), "VSMain"};
        psoDesc.PS = {psShader.get(), "PSMain"};
        psoDesc.VertexLayouts = std::span{&vertLayout, 1};
        psoDesc.Primitive = render::PrimitiveState::Default();
        psoDesc.DepthStencil = render::DepthStencilState::Default();
        psoDesc.MultiSample = render::MultiSampleState::Default();
        psoDesc.ColorTargets = std::span{&rtState, 1};
        _pso = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();

        _ready = true;

        RADRAY_INFO_LOG("upload done");
    }

private:
    Eigen::Vector2f NormalizeMousePos(int x, int y) const {
        if (!_window) {
            return Eigen::Vector2f::Zero();
        }
        const auto size = _window->GetSize();
        const float w = (float)std::max(1, size.X);
        const float h = (float)std::max(1, size.Y);
        const float nx = (static_cast<float>(x) / w) * 2.0f - 1.0f;
        const float ny = (static_cast<float>(y) / h) * 2.0f - 1.0f;
        return {nx, ny};
    }

    void OnTouch(int x, int y, MouseButton button, Action action) {
        if (!ImGui::GetCurrentContext()) {
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && action != Action::RELEASED) {
            return;
        }

        _cc.CurrentMousePos = NormalizeMousePos(x, y);

        switch (action) {
            case Action::PRESSED: {
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
            }
            case Action::REPEATED: {
                if (_cc.IsOrbiting) {
                    _cc.Orbit(_camPos, _camRot);
                } else if (_cc.IsPanning) {
                    _cc.Pan(_camPos, _camRot);
                } else if (_cc.IsDollying) {
                    const Eigen::Vector2f delta = _cc.CurrentMousePos - _cc.LastMousePos;
                    _cc.WheelDelta = -delta.y() * 5.0f;
                    _cc.Dolly(_camPos, _camRot);
                    _cc.LastMousePos = _cc.CurrentMousePos;
                }
                break;
            }
            case Action::RELEASED: {
                if (button == MouseButton::BUTTON_LEFT) {
                    _cc.IsOrbiting = false;
                } else if (button == MouseButton::BUTTON_MIDDLE) {
                    _cc.IsPanning = false;
                } else if (button == MouseButton::BUTTON_RIGHT) {
                    _cc.IsDollying = false;
                }
                _cc.LastMousePos = _cc.CurrentMousePos;
                break;
            }
            default:
                break;
        }
    }

    void OnMouseWheel(int delta) {
        if (!ImGui::GetCurrentContext()) {
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            return;
        }
        _cc.WheelDelta = static_cast<float>(delta) / 120.0f;
        _cc.Dolly(_camPos, _camRot);
    }

    shared_ptr<render::Dxc> _dxc;
    vector<unique_ptr<HelloPBRFrame>> _frames;
    // camera
    Eigen::Vector3f _camPos{0.0f, 0.0f, -3.0f};
    Eigen::Quaternionf _camRot{Eigen::Quaternionf::Identity()};
    float _camFovY{45.0f};
    float _camNear{0.1f};
    float _camFar{100.0f};
    CameraControl _cc;
    sigslot::scoped_connection _touchConn;
    sigslot::scoped_connection _wheelConn;
    // mesh
    unique_ptr<render::RootSignature> _rs;
    unique_ptr<render::GraphicsPipelineState> _pso;
    unique_ptr<render::RenderMesh> _renderMesh;
    vector<unique_ptr<render::Buffer>> _meshUploadBuffers;
    bool _needsMeshUpload{false};
    vector<unique_ptr<render::BindBridge>> _binds;
    Eigen::Vector3f _modelPos{0.0f, 0.0f, 0.0f};
    Eigen::Vector3f _modelScale{1.0f, 1.0f, 1.0f};
    Eigen::Quaternionf _modelRot{Eigen::Quaternionf::Identity()};
    bool _ready{false};

    SimpleFPSCounter _fps{*this, 125};
    SimpleMonitorIMGUI _monitor{*this};
};

unique_ptr<HelloPBRApp> app;

int main(int argc, char** argv) {
    try {
        app = make_unique<HelloPBRApp>();
        ImGuiAppConfig config = HelloPBRApp::ParseArgsSimple(argc, argv);
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
