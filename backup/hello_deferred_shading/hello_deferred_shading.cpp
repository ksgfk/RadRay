#include <algorithm>
#include <array>
#include <cstring>

#include <radray/logger.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/camera_control.h>
#include <radray/file.h>
#include <radray/imgui/imgui_app.h>
#include <radray/render/dxc.h>
#include <radray/render/gpu_resource.h>
#include <radray/render/resource_binder.h>
#include <radray/render/render_graph.h>
#include <radray/render/render_graph_executor.h>
#include <radray/render/render_utility.h>

using namespace radray;

const char* RADRAY_APP_NAME = "Deferred Shading Example";

class HelloDeferredFrame {
public:
    HelloDeferredFrame(const shared_ptr<render::Device>& device, render::CommandQueue* queue)
        : _cbArena(device.get(), {256 * 256, device->GetDetail().CBufferAlignment}),
          _registry(make_unique<render::RGRegistry>(device)) {
        _cmdBuffer = device->CreateCommandBuffer(queue).Unwrap();
    }
    HelloDeferredFrame(const HelloDeferredFrame&) = delete;
    HelloDeferredFrame& operator=(const HelloDeferredFrame&) = delete;
    HelloDeferredFrame(HelloDeferredFrame&&) = delete;
    HelloDeferredFrame& operator=(HelloDeferredFrame&&) = delete;
    ~HelloDeferredFrame() noexcept {
        _registry.reset();
        _cbArena.Destroy();
        _cmdBuffer.reset();
    }

    void ResetRegistry(const shared_ptr<render::Device>& device) {
        _registry = make_unique<render::RGRegistry>(device);
    }

    void ResetTransientViews() {
        _transientTextureViews.clear();
    }

public:
    unique_ptr<render::CommandBuffer> _cmdBuffer;
    render::CBufferArena _cbArena;
    unique_ptr<render::RGRegistry> _registry;
    vector<unique_ptr<render::TextureView>> _transientTextureViews;
};

class HelloDeferredShadingApp : public ImGuiApplication {
public:
    HelloDeferredShadingApp() = default;
    ~HelloDeferredShadingApp() noexcept override = default;

    void OnStart(const ImGuiAppConfig& config_) override {
        auto config = config_;
        this->Init(config);

        _cc.SetOrbitTarget(_modelPos);
        _cc.UpdateDistance(_camPos);
        _touchConn = _window->EventTouch().connect(&HelloDeferredShadingApp::OnTouch, this);
        _wheelConn = _window->EventMouseWheel().connect(&HelloDeferredShadingApp::OnMouseWheel, this);

        _dxc = render::CreateDxc().Unwrap();
        _executor = render::RGExecutor::Create(_device);
        if (_executor == nullptr) {
            throw ImGuiApplicationException("Failed to create RenderGraph executor");
        }
        _frames.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; ++i) {
            _frames.emplace_back(make_unique<HelloDeferredFrame>(_device, _cmdQueue));
        }

        Load();
        this->OnRecreateSwapChain();

        _ready = true;
        RADRAY_INFO_LOG("hello_deferred_shading init done");
    }

    void OnDestroy() noexcept override {
        _touchConn.disconnect();
        _wheelConn.disconnect();

        _dxc.reset();
        _executor.reset();

        _gbufferBinds.clear();
        _lightingBinds.clear();
        _tonemapBinds.clear();

        _gbufferPSO.reset();
        _lightingPSO.reset();
        _tonemapPSO.reset();

        _gbufferRS.reset();
        _lightingRS.reset();
        _tonemapRS.reset();

        _meshUploadBuffers.clear();
        _renderMesh.reset();

        _frames.clear();
    }

    void OnImGui() override {
        _monitor.OnImGui();

        ImGui::Begin("Deferred Shading");
        ImGui::SliderFloat3("Base Color", _baseColor.data(), 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &_roughness, 0.02f, 1.0f);
        ImGui::SliderFloat("Metallic", &_metallic, 0.0f, 1.0f);

        ImGui::Separator();
        ImGui::SliderFloat3("Light Direction", _lightDir.data(), -1.0f, 1.0f);
        ImGui::SliderFloat3("Light Color", _lightColor.data(), 0.0f, 4.0f);
        ImGui::SliderFloat("Light Intensity", &_lightIntensity, 0.0f, 30.0f);
        ImGui::SliderFloat("Ambient", &_ambient, 0.0f, 0.5f);

        ImGui::Separator();
        ImGui::SliderFloat("Exposure", &_exposure, 0.05f, 4.0f);
        const char* viewModes[] = {
            "Lit",
            "GBuffer Albedo",
            "GBuffer Normal",
            "GBuffer Material",
            "GBuffer Depth"};
        ImGui::Combo("Debug View", &_debugView, viewModes, IM_ARRAYSIZE(viewModes));

        ImGui::Separator();
        ImGui::Text("RG passes: %u", _lastCompiledPassCount);
        ImGui::Text("RG barriers: %u", _lastCompiledBarrierCount);
        if (!_lastGraphError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", _lastGraphError.c_str());
        }
        ImGui::End();
    }

    void OnUpdate() override {
        _fps.OnUpdate();
        _monitor.SetData(_fps);
    }

    void OnExtractDrawData(uint32_t frameIndex) override {
        ImGuiApplication::OnExtractDrawData(frameIndex);

        if (!_ready) {
            return;
        }

        _gbufferBinds[frameIndex]->ResetState();
        _lightingBinds[frameIndex]->ResetState();
        _tonemapBinds[frameIndex]->ResetState();

        auto& frame = _frames[frameIndex];
        auto& cbArena = frame->_cbArena;
        Eigen::Vector2i rtSize = this->GetRTSize();
        if (rtSize.x() <= 0 || rtSize.y() <= 0) {
            return;
        }

        Eigen::Matrix4f view = LookAt(_camRot, _camPos);
        Eigen::Matrix4f proj = PerspectiveLH(Radian(_camFovY), (float)rtSize.x() / (float)rtSize.y(), _camNear, _camFar);
        Eigen::Matrix4f model = (Eigen::Translation3f{_modelPos} * _modelRot.toRotationMatrix() * Eigen::AlignedScaling3f{_modelScale}).matrix();
        Eigen::Matrix4f modelInv = model.inverse();
        Eigen::Matrix4f vp = proj * view;
        Eigen::Matrix4f invViewProj = vp.inverse();
        Eigen::Matrix4f mvp = vp * model;

        {
            auto* bind = _gbufferBinds[frameIndex].get();
            auto obj = bind->GetCBuffer("_Obj");
            obj.GetVar("model").SetValue(model);
            obj.GetVar("mvp").SetValue(mvp);
            obj.GetVar("modelInv").SetValue(modelInv);
            obj.GetVar("baseColor").SetValue(Eigen::Vector4f{_baseColor.x(), _baseColor.y(), _baseColor.z(), 1.0f});
            obj.GetVar("roughness").SetValue(_roughness);
            obj.GetVar("metallic").SetValue(_metallic);
            bind->Upload(_device.get(), cbArena);
        }

        {
            auto* bind = _lightingBinds[frameIndex].get();
            auto lighting = bind->GetCBuffer("_Lighting");
            Eigen::Vector3f dir = _lightDir.normalized();
            lighting.GetVar("invViewProj").SetValue(invViewProj);
            lighting.GetVar("cameraPos").SetValue(Eigen::Vector4f{_camPos.x(), _camPos.y(), _camPos.z(), 1.0f});
            lighting.GetVar("lightDirIntensity").SetValue(Eigen::Vector4f{dir.x(), dir.y(), dir.z(), _lightIntensity});
            lighting.GetVar("lightColorAmbient").SetValue(Eigen::Vector4f{_lightColor.x(), _lightColor.y(), _lightColor.z(), _ambient});
            lighting.GetVar("screenAndDebug").SetValue(Eigen::Vector4f{(float)rtSize.x(), (float)rtSize.y(), (float)_debugView, 0.0f});
            bind->Upload(_device.get(), cbArena);
        }

        {
            auto* bind = _tonemapBinds[frameIndex].get();
            auto tone = bind->GetCBuffer("_Tone");
            tone.GetVar("toneParams").SetValue(Eigen::Vector4f{_exposure, 0.0f, 0.0f, 0.0f});
            bind->Upload(_device.get(), cbArena);
        }
    }

    vector<render::CommandBuffer*> OnRender(uint32_t frameIndex) override {
        auto& frame = _frames[frameIndex];
        frame->ResetTransientViews();
        auto* cmd = frame->_cmdBuffer.get();
        _fps.OnRender();

        const auto currBackBufferIndex = _swapchain->GetCurrentBackBufferIndex();
        auto* backBuffer = _backBuffers[currBackBufferIndex];
        auto* backBufferRTV = this->GetDefaultRTV(currBackBufferIndex);

        cmd->Begin();
        _imguiRenderer->OnRenderBegin(frameIndex, cmd);

        if (_needsMeshUpload) {
            UploadMesh(cmd);
        }

        bool graphSucceeded = false;
        {
            render::RGGraphBuilder graph{};
            DeferredGraphHandles handles = BuildGraph(frameIndex, graph);
            auto compiled = graph.Compile();

            _lastCompiledPassCount = static_cast<uint32_t>(compiled.SortedPasses.size());
            _lastCompiledBarrierCount = 0;
            for (const auto& passBarriers : compiled.PassBarriers) {
                _lastCompiledBarrierCount += static_cast<uint32_t>(passBarriers.size());
            }
            _lastGraphError.clear();

            if (!compiled.Success) {
                _lastGraphError = compiled.ErrorMessage.empty() ? "RenderGraph compile failed" : compiled.ErrorMessage;
            } else if (!frame->_registry->ImportPhysicalTexture(handles.Backbuffer, backBuffer)) {
                _lastGraphError = "Failed to import swapchain backbuffer to RG registry";
            } else if (_executor == nullptr) {
                _lastGraphError = "RenderGraph executor is null";
            } else {
                if (!_loggedCompiledGraph) {
                    _loggedCompiledGraph = true;
                    RADRAY_INFO_LOG("{}", graph.DumpCompiledGraph(compiled));
                }
                render::RGRecordOptions options{};
                options.EmitBarriers = true;
                options.ValidateQueueClass = true;
                options.RecordQueueClass = render::QueueType::Direct;
                if (_executor->Record(cmd, graph, compiled, frame->_registry.get(), options)) {
                    graphSucceeded = true;
                } else {
                    _lastGraphError = "RenderGraph record failed";
                }
            }
        }

        if (!graphSucceeded) {
            RADRAY_ERR_LOG("hello_deferred_shading fallback render: {}", _lastGraphError);
            FallbackRender(frameIndex, cmd, backBuffer, backBufferRTV);
        } else {
            render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
                backBuffer,
                render::TextureState::RenderTarget,
                render::TextureState::Present};
            cmd->ResourceBarrier(std::span{&barrier, 1});
        }

        cmd->End();
        return {cmd};
    }

    void OnRenderComplete(uint32_t frameIndex) override {
        ImGuiApplication::OnRenderComplete(frameIndex);

        _meshUploadBuffers.clear();
        _frames[frameIndex]->_cbArena.Reset();
    }

    void OnRecreateSwapChain() override {
        for (auto& frame : _frames) {
            frame->ResetRegistry(_device);
        }
    }

private:
    struct DeferredGraphHandles {
        render::RGResourceHandle GBufferAlbedo{};
        render::RGResourceHandle GBufferNormal{};
        render::RGResourceHandle GBufferMaterial{};
        render::RGResourceHandle GBufferDepth{};
        render::RGResourceHandle HDRColor{};
        render::RGResourceHandle Backbuffer{};
    };

    void SetViewportAndScissor(render::GraphicsCommandEncoder* pass, const Eigen::Vector2i& rtSize) {
        Viewport viewport{0.0f, 0.0f, (float)rtSize.x(), (float)rtSize.y(), 0.0f, 1.0f};
        if (_device->GetBackend() == render::RenderBackend::Vulkan) {
            viewport.Y = (float)rtSize.y();
            viewport.Height = -(float)rtSize.y();
        }
        pass->SetViewport(viewport);
        pass->SetScissor({0, 0, (uint32_t)rtSize.x(), (uint32_t)rtSize.y()});
    }

    render::TextureView* CreateTransientTextureView(uint32_t frameIndex, const render::TextureViewDescriptor& desc) {
        if (frameIndex >= _frames.size()) {
            return nullptr;
        }
        auto view = _device->CreateTextureView(desc);
        if (!view.HasValue()) {
            return nullptr;
        }
        auto owned = view.Unwrap();
        auto* raw = owned.get();
        _frames[frameIndex]->_transientTextureViews.emplace_back(std::move(owned));
        return raw;
    }

    void DrawSphere(render::GraphicsCommandEncoder* pass) {
        if (_renderMesh == nullptr) {
            return;
        }
        for (const auto& meshDraw : _renderMesh->_drawDatas) {
            auto vbv = meshDraw.Vbv;
            pass->BindVertexBuffer(std::span{&vbv, 1});
            auto ibv = meshDraw.Ibv;
            pass->BindIndexBuffer(ibv);
            const uint64_t ibSize = ibv.Target->GetDesc().Size;
            const uint64_t offset = ibv.Offset;
            const uint64_t count = (ibSize > offset && ibv.Stride > 0) ? ((ibSize - offset) / ibv.Stride) : 0;
            pass->DrawIndexed(static_cast<uint32_t>(count), 1, 0, 0, 0);
        }
    }

    DeferredGraphHandles BuildGraph(uint32_t frameIndex, render::RGGraphBuilder& graph) {
        const Eigen::Vector2i rtSize = this->GetRTSize();

        DeferredGraphHandles handles{};

        render::RGTextureDescriptor gbufferAlbedo{};
        gbufferAlbedo.Dim = render::TextureDimension::Dim2D;
        gbufferAlbedo.Width = (uint32_t)rtSize.x();
        gbufferAlbedo.Height = (uint32_t)rtSize.y();
        gbufferAlbedo.DepthOrArraySize = 1;
        gbufferAlbedo.MipLevels = 1;
        gbufferAlbedo.SampleCount = 1;
        gbufferAlbedo.Format = render::TextureFormat::RGBA8_UNORM;
        gbufferAlbedo.Name = "GBuffer_Albedo";
        handles.GBufferAlbedo = graph.CreateTexture(gbufferAlbedo);

        render::RGTextureDescriptor gbufferNormal = gbufferAlbedo;
        gbufferNormal.Format = render::TextureFormat::RGBA16_FLOAT;
        gbufferNormal.Name = "GBuffer_Normal";
        handles.GBufferNormal = graph.CreateTexture(gbufferNormal);

        render::RGTextureDescriptor gbufferMaterial = gbufferAlbedo;
        gbufferMaterial.Format = render::TextureFormat::RG8_UNORM;
        gbufferMaterial.Name = "GBuffer_Material";
        handles.GBufferMaterial = graph.CreateTexture(gbufferMaterial);

        render::RGTextureDescriptor gbufferDepth = gbufferAlbedo;
        gbufferDepth.Format = render::TextureFormat::D32_FLOAT;
        gbufferDepth.Name = "GBuffer_Depth";
        handles.GBufferDepth = graph.CreateTexture(gbufferDepth);

        render::RGTextureDescriptor hdrColor = gbufferAlbedo;
        hdrColor.Format = render::TextureFormat::RGBA16_FLOAT;
        hdrColor.Name = "HDR_Color";
        handles.HDRColor = graph.CreateTexture(hdrColor);

        handles.Backbuffer = graph.ImportExternalTexture(
            "Backbuffer",
            render::RGResourceFlag::External | render::RGResourceFlag::Persistent | render::RGResourceFlag::Output,
            render::RGAccessMode::Unknown);

        {
            auto pass = graph.AddPass("GBuffer");
            pass.WriteTexture(handles.GBufferAlbedo, {}, render::RGAccessMode::ColorAttachmentWrite);
            pass.WriteTexture(handles.GBufferNormal, {}, render::RGAccessMode::ColorAttachmentWrite);
            pass.WriteTexture(handles.GBufferMaterial, {}, render::RGAccessMode::ColorAttachmentWrite);
            pass.WriteTexture(handles.GBufferDepth, {}, render::RGAccessMode::DepthStencilWrite);
            pass.SetExecuteFunc([this, frameIndex, handles](render::RGPassContext& ctx) {
                auto* gbufferAlbedo = ctx.GetTexture(handles.GBufferAlbedo);
                auto* gbufferNormal = ctx.GetTexture(handles.GBufferNormal);
                auto* gbufferMaterial = ctx.GetTexture(handles.GBufferMaterial);
                auto* gbufferDepth = ctx.GetTexture(handles.GBufferDepth);
                if (gbufferAlbedo == nullptr || gbufferNormal == nullptr || gbufferMaterial == nullptr || gbufferDepth == nullptr) {
                    return;
                }

                auto* albedoRTV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    gbufferAlbedo,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::RGBA8_UNORM,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::RenderTarget});
                auto* normalRTV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    gbufferNormal,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::RGBA16_FLOAT,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::RenderTarget});
                auto* materialRTV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    gbufferMaterial,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::RG8_UNORM,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::RenderTarget});
                auto* depthDSV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    gbufferDepth,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::D32_FLOAT,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::DepthWrite});
                if (albedoRTV == nullptr || normalRTV == nullptr || materialRTV == nullptr || depthDSV == nullptr) {
                    return;
                }

                render::ColorAttachment colorAttachments[] = {
                    {albedoRTV, render::LoadAction::Clear, render::StoreAction::Store, render::ColorClearValue{{{0.0f, 0.0f, 0.0f, 1.0f}}}},
                    {normalRTV, render::LoadAction::Clear, render::StoreAction::Store, render::ColorClearValue{{{0.5f, 0.5f, 1.0f, 1.0f}}}},
                    {materialRTV, render::LoadAction::Clear, render::StoreAction::Store, render::ColorClearValue{{{0.0f, 0.0f, 0.0f, 1.0f}}}}};
                render::DepthStencilAttachment depthAttachment{
                    depthDSV,
                    render::LoadAction::Clear,
                    render::StoreAction::Store,
                    render::LoadAction::DontCare,
                    render::StoreAction::Discard,
                    {1.0f, 0}};
                render::RenderPassDescriptor rpDesc{};
                rpDesc.Name = "GBuffer Pass";
                rpDesc.ColorAttachments = std::span(colorAttachments);
                rpDesc.DepthStencilAttachment = depthAttachment;
                auto passEncoder = ctx.Cmd->BeginRenderPass(rpDesc).Unwrap();
                SetViewportAndScissor(passEncoder.get(), this->GetRTSize());
                passEncoder->BindRootSignature(_gbufferRS.get());
                passEncoder->BindGraphicsPipelineState(_gbufferPSO.get());
                _gbufferBinds[frameIndex]->Bind(passEncoder.get());
                DrawSphere(passEncoder.get());
                ctx.Cmd->EndRenderPass(std::move(passEncoder));
            });
        }

        {
            auto pass = graph.AddPass("DeferredLighting");
            pass.ReadTexture(handles.GBufferAlbedo, {}, render::RGAccessMode::SampledRead);
            pass.ReadTexture(handles.GBufferNormal, {}, render::RGAccessMode::SampledRead);
            pass.ReadTexture(handles.GBufferMaterial, {}, render::RGAccessMode::SampledRead);
            pass.ReadTexture(handles.GBufferDepth, {}, render::RGAccessMode::SampledRead);
            pass.WriteTexture(handles.HDRColor, {}, render::RGAccessMode::ColorAttachmentWrite);
            pass.SetExecuteFunc([this, frameIndex, handles](render::RGPassContext& ctx) {
                auto* gbufferAlbedo = ctx.GetTexture(handles.GBufferAlbedo);
                auto* gbufferNormal = ctx.GetTexture(handles.GBufferNormal);
                auto* gbufferMaterial = ctx.GetTexture(handles.GBufferMaterial);
                auto* gbufferDepth = ctx.GetTexture(handles.GBufferDepth);
                auto* hdrColor = ctx.GetTexture(handles.HDRColor);
                if (gbufferAlbedo == nullptr || gbufferNormal == nullptr || gbufferMaterial == nullptr || gbufferDepth == nullptr || hdrColor == nullptr) {
                    return;
                }

                auto* albedoSRV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    gbufferAlbedo,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::RGBA8_UNORM,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::Resource});
                auto* normalSRV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    gbufferNormal,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::RGBA16_FLOAT,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::Resource});
                auto* materialSRV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    gbufferMaterial,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::RG8_UNORM,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::Resource});
                auto* depthSRV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    gbufferDepth,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::D32_FLOAT,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::Resource});
                auto* hdrRTV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    hdrColor,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::RGBA16_FLOAT,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::RenderTarget});
                if (albedoSRV == nullptr || normalSRV == nullptr || materialSRV == nullptr || depthSRV == nullptr || hdrRTV == nullptr) {
                    return;
                }

                auto* bind = _lightingBinds[frameIndex].get();
                bind->SetResource("_GAlbedo", albedoSRV);
                bind->SetResource("_GNormal", normalSRV);
                bind->SetResource("_GMaterial", materialSRV);
                bind->SetResource("_GDepth", depthSRV);

                render::ColorAttachment colorAttachment{
                    hdrRTV,
                    render::LoadAction::Clear,
                    render::StoreAction::Store,
                    render::ColorClearValue{{{0.0f, 0.0f, 0.0f, 1.0f}}}};
                render::RenderPassDescriptor rpDesc{};
                rpDesc.Name = "Deferred Lighting";
                rpDesc.ColorAttachments = std::span{&colorAttachment, 1};
                auto passEncoder = ctx.Cmd->BeginRenderPass(rpDesc).Unwrap();
                SetViewportAndScissor(passEncoder.get(), this->GetRTSize());
                passEncoder->BindRootSignature(_lightingRS.get());
                passEncoder->BindGraphicsPipelineState(_lightingPSO.get());
                bind->Bind(passEncoder.get());
                passEncoder->Draw(3, 1, 0, 0);
                ctx.Cmd->EndRenderPass(std::move(passEncoder));
            });
        }

        {
            auto pass = graph.AddPass("ToneMapping");
            pass.ReadTexture(handles.HDRColor, {}, render::RGAccessMode::SampledRead);
            pass.WriteTexture(handles.Backbuffer, {}, render::RGAccessMode::ColorAttachmentWrite);
            pass.SetExecuteFunc([this, frameIndex, handles](render::RGPassContext& ctx) {
                auto* hdrColor = ctx.GetTexture(handles.HDRColor);
                auto* backBuffer = ctx.GetTexture(handles.Backbuffer);
                if (hdrColor == nullptr || backBuffer == nullptr) {
                    return;
                }

                auto* hdrSRV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    hdrColor,
                    render::TextureDimension::Dim2D,
                    render::TextureFormat::RGBA16_FLOAT,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::Resource});
                auto* backBufferRTV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    backBuffer,
                    render::TextureDimension::Dim2D,
                    _rtFormat,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::RenderTarget});
                if (hdrSRV == nullptr || backBufferRTV == nullptr) {
                    return;
                }

                auto* bind = _tonemapBinds[frameIndex].get();
                bind->SetResource("_HDRTex", hdrSRV);

                render::ColorAttachment colorAttachment{
                    backBufferRTV,
                    render::LoadAction::Clear,
                    render::StoreAction::Store,
                    render::ColorClearValue{{{0.0f, 0.0f, 0.0f, 1.0f}}}};
                render::RenderPassDescriptor rpDesc{};
                rpDesc.Name = "Tone Mapping";
                rpDesc.ColorAttachments = std::span{&colorAttachment, 1};
                auto passEncoder = ctx.Cmd->BeginRenderPass(rpDesc).Unwrap();
                SetViewportAndScissor(passEncoder.get(), this->GetRTSize());
                passEncoder->BindRootSignature(_tonemapRS.get());
                passEncoder->BindGraphicsPipelineState(_tonemapPSO.get());
                bind->Bind(passEncoder.get());
                passEncoder->Draw(3, 1, 0, 0);
                ctx.Cmd->EndRenderPass(std::move(passEncoder));
            });
        }

        {
            auto pass = graph.AddPass("ImGuiOverlay");
            pass.WriteTexture(handles.Backbuffer, {}, render::RGAccessMode::ColorAttachmentWrite);
            pass.SetExecuteFunc([this, frameIndex, handles](render::RGPassContext& ctx) {
                auto* backBuffer = ctx.GetTexture(handles.Backbuffer);
                if (backBuffer == nullptr) {
                    return;
                }

                auto* backBufferRTV = CreateTransientTextureView(frameIndex, render::TextureViewDescriptor{
                    backBuffer,
                    render::TextureDimension::Dim2D,
                    _rtFormat,
                    render::SubresourceRange::AllSub(),
                    render::TextureViewUsage::RenderTarget});
                if (backBufferRTV == nullptr) {
                    return;
                }

                render::ColorAttachment colorAttachment{
                    backBufferRTV,
                    render::LoadAction::Load,
                    render::StoreAction::Store,
                    render::ColorClearValue{}};
                render::RenderPassDescriptor rpDesc{};
                rpDesc.Name = "ImGui Overlay";
                rpDesc.ColorAttachments = std::span{&colorAttachment, 1};
                auto passEncoder = ctx.Cmd->BeginRenderPass(rpDesc).Unwrap();
                _imguiRenderer->OnRender(frameIndex, passEncoder.get());
                ctx.Cmd->EndRenderPass(std::move(passEncoder));
            });
        }

        graph.MarkOutput(handles.Backbuffer);
        return handles;
    }

    void FallbackRender(
        uint32_t frameIndex,
        render::CommandBuffer* cmd,
        render::Texture* backBuffer,
        render::TextureView* backBufferRTV) {
        render::ResourceBarrierDescriptor beginBarrier = render::BarrierTextureDescriptor{
            backBuffer,
            render::TextureState::Undefined,
            render::TextureState::RenderTarget};
        cmd->ResourceBarrier(std::span{&beginBarrier, 1});

        render::ColorAttachment colorAttachment{
            backBufferRTV,
            render::LoadAction::Clear,
            render::StoreAction::Store,
            render::ColorClearValue{{{0.2f, 0.0f, 0.0f, 1.0f}}}};
        render::RenderPassDescriptor rpDesc{};
        rpDesc.ColorAttachments = std::span{&colorAttachment, 1};
        auto passEncoder = cmd->BeginRenderPass(rpDesc).Unwrap();
        _imguiRenderer->OnRender(frameIndex, passEncoder.get());
        cmd->EndRenderPass(std::move(passEncoder));

        render::ResourceBarrierDescriptor endBarrier = render::BarrierTextureDescriptor{
            backBuffer,
            render::TextureState::RenderTarget,
            render::TextureState::Present};
        cmd->ResourceBarrier(std::span{&endBarrier, 1});
    }

    void UploadMesh(render::CommandBuffer* cmd) {
        if (_renderMesh == nullptr || _meshUploadBuffers.empty()) {
            _needsMeshUpload = false;
            return;
        }

        for (size_t i = 0; i < _meshUploadBuffers.size(); ++i) {
            auto* src = _meshUploadBuffers[i].get();
            auto* dst = _renderMesh->_buffers[i].get();
            cmd->CopyBufferToBuffer(dst, 0, src, 0, src->GetDesc().Size);
        }

        vector<render::ResourceBarrierDescriptor> barriers{};
        barriers.reserve(_renderMesh->_buffers.size());
        for (auto& buffer : _renderMesh->_buffers) {
            barriers.emplace_back(render::BarrierBufferDescriptor{
                buffer.get(),
                render::BufferState::CopyDestination,
                render::BufferState::Vertex | render::BufferState::Index});
        }
        cmd->ResourceBarrier(barriers);

        _needsMeshUpload = false;
    }

    void Load() {
        LoadSphereMesh();
        CompilePipelines();
    }

    void LoadSphereMesh() {
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
                        const uint64_t required = static_cast<uint64_t>(primitive.VertexCount) * vbEntry.Stride;
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
    }

    static string ReadExampleShader(std::string_view fileName) {
        auto path = std::filesystem::path("assets") / "hello_deferred_shading" / fileName;
        auto hlslOpt = file::ReadText(path);
        if (!hlslOpt.has_value()) {
            throw ImGuiApplicationException(fmt::format("Failed to read shader file: {}", fileName));
        }
        return std::move(hlslOpt.value());
    }

    void CompilePipelines() {
        const auto backend = _device->GetBackend();

        const string gbufferHlsl = ReadExampleShader("deferred_gbuffer.hlsl");
        const string lightingHlsl = ReadExampleShader("deferred_lighting.hlsl");
        const string tonemapHlsl = ReadExampleShader("deferred_tonemap.hlsl");

        render::StaticSamplerBinding staticSampler{
            "_Sampler",
            {{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
              render::FilterMode::Linear, render::FilterMode::Linear, render::FilterMode::Linear,
              0.0f, 0.0f, std::nullopt, 0}}};

        vector<render::VertexElement> vertElems;
        uint32_t stride = 0;
        {
            TriangleMesh sphereMesh{};
            sphereMesh.InitAsUVSphere(0.5f, 8);
            MeshResource sphereModel{};
            sphereMesh.ToSimpleMeshResource(&sphereModel);
            const auto& prim = sphereModel.Primitives[0];
            render::SemanticMapping mapping[] = {
                {VertexSemantics::POSITION, 0, 0, render::VertexFormat::FLOAT32X3},
                {VertexSemantics::NORMAL, 0, 1, render::VertexFormat::FLOAT32X3},
                {VertexSemantics::TEXCOORD, 0, 2, render::VertexFormat::FLOAT32X2}};
            auto mapped = render::MapVertexElements(prim.VertexBuffers, mapping);
            if (!mapped.has_value()) {
                throw ImGuiApplicationException("failed to map vertex elements for gbuffer pipeline");
            }
            vertElems = std::move(mapped.value());
            for (const auto& vb : prim.VertexBuffers) {
                stride += vertex_utility::GetVertexDataSizeInBytes(vb.Type, vb.ComponentCount);
            }
        }

        render::VertexBufferLayout vertexLayout{};
        vertexLayout.ArrayStride = stride;
        vertexLayout.StepMode = render::VertexStepMode::Vertex;
        vertexLayout.Elements = vertElems;

        {
            auto shaderResult = render::CompileShaderFromHLSL(_dxc.get(), _device.get(), gbufferHlsl, backend);
            if (!shaderResult.has_value()) {
                throw ImGuiApplicationException("failed to compile deferred_gbuffer.hlsl");
            }
            auto [vsShader, psShader, bindLayout] = std::move(shaderResult.value());
            auto rsDesc = bindLayout.GetDescriptor();
            _gbufferRS = _device->CreateRootSignature(rsDesc.Get()).Unwrap();
            _gbufferBinds.reserve(_inFlightFrameCount);
            for (uint32_t i = 0; i < _inFlightFrameCount; ++i) {
                _gbufferBinds.emplace_back(make_unique<render::ResourceBinder>(_device.get(), _gbufferRS.get(), bindLayout));
            }

            std::array<render::ColorTargetState, 3> gbufferTargets{
                render::ColorTargetState::Default(render::TextureFormat::RGBA8_UNORM),
                render::ColorTargetState::Default(render::TextureFormat::RGBA16_FLOAT),
                render::ColorTargetState::Default(render::TextureFormat::RG8_UNORM)};

            render::GraphicsPipelineStateDescriptor psoDesc{};
            psoDesc.RootSig = _gbufferRS.get();
            psoDesc.VS = render::ShaderEntry{vsShader.get(), "VSMain"};
            psoDesc.PS = render::ShaderEntry{psShader.get(), "PSMain"};
            psoDesc.VertexLayouts = std::span{&vertexLayout, 1};
            psoDesc.Primitive = render::PrimitiveState::Default();
            if (backend == render::RenderBackend::Vulkan) {
                // With Vulkan Y-flip viewport, default front-face winding may cull the whole mesh.
                psoDesc.Primitive.Cull = render::CullMode::None;
            }
            psoDesc.DepthStencil = render::DepthStencilState::Default();
            psoDesc.MultiSample = render::MultiSampleState::Default();
            psoDesc.ColorTargets = gbufferTargets;
            _gbufferPSO = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();
        }

        {
            auto shaderResult = render::CompileShaderFromHLSL(_dxc.get(), _device.get(), lightingHlsl, backend);
            if (!shaderResult.has_value()) {
                throw ImGuiApplicationException("failed to compile deferred_lighting.hlsl");
            }
            auto [vsShader, psShader, bindLayout] = std::move(shaderResult.value());
            auto rsDesc = bindLayout.GetDescriptor();
            _lightingRS = _device->CreateRootSignature(rsDesc.Get()).Unwrap();
            _lightingBinds.reserve(_inFlightFrameCount);
            for (uint32_t i = 0; i < _inFlightFrameCount; ++i) {
                _lightingBinds.emplace_back(make_unique<render::ResourceBinder>(_device.get(), _lightingRS.get(), bindLayout));
            }

            auto rtState = render::ColorTargetState::Default(render::TextureFormat::RGBA16_FLOAT);
            render::GraphicsPipelineStateDescriptor psoDesc{};
            psoDesc.RootSig = _lightingRS.get();
            psoDesc.VS = render::ShaderEntry{vsShader.get(), "VSMain"};
            psoDesc.PS = render::ShaderEntry{psShader.get(), "PSMain"};
            psoDesc.VertexLayouts = {};
            psoDesc.Primitive = render::PrimitiveState::Default();
            psoDesc.Primitive.Cull = render::CullMode::None;
            psoDesc.DepthStencil = std::nullopt;
            psoDesc.MultiSample = render::MultiSampleState::Default();
            psoDesc.ColorTargets = std::span{&rtState, 1};
            _lightingPSO = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();
        }

        {
            auto shaderResult = render::CompileShaderFromHLSL(_dxc.get(), _device.get(), tonemapHlsl, backend);
            if (!shaderResult.has_value()) {
                throw ImGuiApplicationException("failed to compile deferred_tonemap.hlsl");
            }
            auto [vsShader, psShader, bindLayout] = std::move(shaderResult.value());
            auto rsDesc = bindLayout.GetDescriptor();
            _tonemapRS = _device->CreateRootSignature(rsDesc.Get()).Unwrap();
            _tonemapBinds.reserve(_inFlightFrameCount);
            for (uint32_t i = 0; i < _inFlightFrameCount; ++i) {
                _tonemapBinds.emplace_back(make_unique<render::ResourceBinder>(_device.get(), _tonemapRS.get(), bindLayout));
            }

            auto rtState = render::ColorTargetState::Default(_rtFormat);
            render::GraphicsPipelineStateDescriptor psoDesc{};
            psoDesc.RootSig = _tonemapRS.get();
            psoDesc.VS = render::ShaderEntry{vsShader.get(), "VSMain"};
            psoDesc.PS = render::ShaderEntry{psShader.get(), "PSMain"};
            psoDesc.VertexLayouts = {};
            psoDesc.Primitive = render::PrimitiveState::Default();
            psoDesc.Primitive.Cull = render::CullMode::None;
            psoDesc.DepthStencil = std::nullopt;
            psoDesc.MultiSample = render::MultiSampleState::Default();
            psoDesc.ColorTargets = std::span{&rtState, 1};
            _tonemapPSO = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();
        }
    }

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
        if (ImGui::GetIO().WantCaptureMouse) {
            return;
        }
        _cc.WheelDelta = static_cast<float>(delta) / 120.0f;
        _cc.Dolly(_camPos, _camRot);
    }

private:
    shared_ptr<render::Dxc> _dxc;
    unique_ptr<render::RGExecutor> _executor;
    vector<unique_ptr<HelloDeferredFrame>> _frames;

    unique_ptr<render::RootSignature> _gbufferRS;
    unique_ptr<render::RootSignature> _lightingRS;
    unique_ptr<render::RootSignature> _tonemapRS;

    unique_ptr<render::GraphicsPipelineState> _gbufferPSO;
    unique_ptr<render::GraphicsPipelineState> _lightingPSO;
    unique_ptr<render::GraphicsPipelineState> _tonemapPSO;

    vector<unique_ptr<render::ResourceBinder>> _gbufferBinds;
    vector<unique_ptr<render::ResourceBinder>> _lightingBinds;
    vector<unique_ptr<render::ResourceBinder>> _tonemapBinds;

    unique_ptr<render::RenderMesh> _renderMesh;
    vector<unique_ptr<render::Buffer>> _meshUploadBuffers;
    bool _needsMeshUpload{false};

    Eigen::Vector3f _camPos{0.0f, 0.0f, -3.0f};
    Eigen::Quaternionf _camRot{Eigen::Quaternionf::Identity()};
    float _camFovY{45.0f};
    float _camNear{0.1f};
    float _camFar{100.0f};
    CameraControl _cc;
    sigslot::scoped_connection _touchConn;
    sigslot::scoped_connection _wheelConn;

    Eigen::Vector3f _modelPos{0.0f, 0.0f, 0.0f};
    Eigen::Vector3f _modelScale{1.0f, 1.0f, 1.0f};
    Eigen::Quaternionf _modelRot{Eigen::Quaternionf::Identity()};

    Eigen::Vector3f _baseColor{0.95f, 0.45f, 0.2f};
    float _roughness{0.35f};
    float _metallic{0.05f};

    Eigen::Vector3f _lightDir{0.35f, -1.0f, 0.2f};
    Eigen::Vector3f _lightColor{1.0f, 1.0f, 1.0f};
    float _lightIntensity{8.0f};
    float _ambient{0.04f};
    float _exposure{1.0f};
    int _debugView{0};

    uint32_t _lastCompiledPassCount{0};
    uint32_t _lastCompiledBarrierCount{0};
    string _lastGraphError{};
    bool _loggedCompiledGraph{false};

    bool _ready{false};

    SimpleFPSCounter _fps{*this, 125};
    SimpleMonitorIMGUI _monitor{*this};
};

unique_ptr<HelloDeferredShadingApp> app;

int main(int argc, char** argv) {
    try {
        app = make_unique<HelloDeferredShadingApp>();
        ImGuiAppConfig config = HelloDeferredShadingApp::ParseArgsSimple(argc, argv);
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
