#include <radray/logger.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/imgui/imgui_app.h>
#include <radray/render/dxc.h>

using namespace radray;

const char* RADRAY_APP_NAME = "PBR Example";

class HelloPBRApp : public ImGuiApplication {
public:
    HelloPBRApp() = default;
    ~HelloPBRApp() noexcept override = default;

    void OnStart(const ImGuiAppConfig& config) override {
        this->Init(config);

        auto backend = _device->GetBackend();

        _dxc = render::CreateDxc().Unwrap();
        _cmdBuffers.reserve(_inFlightFrameCount);
        for (uint32_t i = 0; i < _inFlightFrameCount; i++) {
            _cmdBuffers.emplace_back(_device->CreateCommandBuffer(_cmdQueue).Unwrap());
        }

        // unique_ptr<render::Shader> vsShader, psShader;
        // {
        //     string hlsl;
        //     {
        //         auto hlslOpt = ReadText(std::filesystem::path("assets") / RADRAY_APP_NAME / "pbr.hlsl");
        //         if (!hlslOpt.has_value()) {
        //             throw ImGuiApplicationException("Failed to read shader file pbr.hlsl");
        //         }
        //         hlsl = std::move(hlslOpt.value());
        //     }
        //     vector<std::string_view> defines;
        //     if (backend == render::RenderBackend::Vulkan) {
        //         defines.emplace_back("VULKAN");
        //     } else if (backend == render::RenderBackend::D3D12) {
        //         defines.emplace_back("D3D12");
        //     }
        //     vector<std::string_view> includes;
        //     render::DxcOutput vsBin;
        //     {
        //         auto vs = _dxc->Compile(hlsl, "VSMain", render::ShaderStage::Vertex, render::HlslShaderModel::SM60, false, defines, includes, backend == render::RenderBackend::Vulkan);
        //         if (!vs.has_value()) {
        //             throw ImGuiApplicationException("Failed to compile vertex shader");
        //         }
        //         vsBin = std::move(vs.value());
        //     }
        //     render::DxcOutput psBin;
        //     {
        //         auto ps = _dxc->Compile(hlsl, "PSMain", render::ShaderStage::Pixel, render::HlslShaderModel::SM60, false, defines, includes, backend == render::RenderBackend::Vulkan);
        //         if (!ps.has_value()) {
        //             throw ImGuiApplicationException("Failed to compile pixel shader");
        //         }
        //         psBin = std::move(ps.value());
        //     }
        //     render::ShaderDescriptor vsDesc{vsBin.Data, vsBin.Category};
        //     vsShader = _device->CreateShader(vsDesc).Unwrap();
        //     render::ShaderDescriptor psDesc{psBin.Data, psBin.Category};
        //     psShader = _device->CreateShader(psDesc).Unwrap();
        // }

        TriangleMesh sphereMesh{};
        sphereMesh.InitAsUVSphere(0.5f, 32);
        MeshResource sphereModel{};
        sphereMesh.ToSimpleMeshResource(&sphereModel);
    }

    void OnDestroy() noexcept override {
        _dxc.reset();
        _cmdBuffers.clear();
    }

    void OnImGui() override {
        _monitor.OnImGui();
    }

    void OnUpdate() override {
        _fps.OnUpdate();
        _monitor.SetData(_fps);
    }

    vector<render::CommandBuffer*> OnRender(uint32_t frameIndex) override {
        _fps.OnRender();

        auto currBackBufferIndex = _swapchain->GetCurrentBackBufferIndex();
        auto cmdBuffer = _cmdBuffers[frameIndex].get();
        auto rt = _backBuffers[currBackBufferIndex];
        auto rtView = this->GetDefaultRTV(currBackBufferIndex);

        cmdBuffer->Begin();
        _imguiRenderer->OnRenderBegin(frameIndex, cmdBuffer);
        {
            render::BarrierTextureDescriptor barrier{};
            barrier.Target = rt;
            barrier.Before = render::TextureUse::Uninitialized;
            barrier.After = render::TextureUse::RenderTarget;
            barrier.IsFromOrToOtherQueue = false;
            barrier.IsSubresourceBarrier = false;
            cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        unique_ptr<render::CommandEncoder> pass;
        {
            render::RenderPassDescriptor rpDesc{};
            render::ColorAttachment rtAttach{};
            rtAttach.Target = rtView;
            rtAttach.Load = render::LoadAction::Clear;
            rtAttach.Store = render::StoreAction::Store;
            rtAttach.ClearValue = render::ColorClearValue{0.1f, 0.1f, 0.1f, 1.0f};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            pass = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        }
        _imguiRenderer->OnRender(frameIndex, pass.get());
        cmdBuffer->EndRenderPass(std::move(pass));
        {
            render::BarrierTextureDescriptor barrier{};
            barrier.Target = rt;
            barrier.Before = render::TextureUse::RenderTarget;
            barrier.After = render::TextureUse::Present;
            barrier.IsFromOrToOtherQueue = false;
            barrier.IsSubresourceBarrier = false;
            cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        cmdBuffer->End();
        return {cmdBuffer};
    }

private:
    shared_ptr<render::Dxc> _dxc;
    vector<unique_ptr<render::CommandBuffer>> _cmdBuffers;
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

// #include <radray/logger.h>
// #include <radray/basic_math.h>
// #include <radray/enum_flags.h>
// #include <radray/utility.h>
// #include <radray/triangle_mesh.h>
// #include <radray/vertex_data.h>
// #include <radray/camera_control.h>
// #include <radray/render/common.h>
// #include <radray/render/render_utility.h>
// #include <radray/render/dxc.h>
// #include <radray/imgui/dear_imgui.h>

// const char* RADRAY_APP_NAME = "hello_pbr";

// using namespace radray;
// using namespace render;

// struct alignas(256) HelloCameraData {
//     float view[16];
//     float proj[16];
//     float viewProj[16];
//     float posW[3];
// };

// struct alignas(256) MaterialData {
//     float albedo[3];
//     float metallic;

//     float specularTint[3];
//     float specular;

//     float roughness;
//     float anisotropy;
//     float ior;
// };

// constexpr size_t CBUFFER_SIZE = sizeof(HelloCameraData) + sizeof(MaterialData);

// class HelloMesh {
// public:
//     shared_ptr<Buffer> _vertexBuffer;
//     shared_ptr<Buffer> _indexBuffer;
//     uint32_t _vertexSize;
//     uint32_t _indexCount;
//     uint32_t _indexStride;
// };

// class HelloPbr : public ImGuiApplication {
// public:
//     HelloPbr(RenderBackend backend, bool multiThread)
//         : ImGuiApplication(),
//           backend(backend),
//           multiThread(multiThread) {}

//     ~HelloPbr() noexcept override = default;

//     void OnStart() override {
//         auto name = format("{} - {} {}", string{RADRAY_APP_NAME}, backend, multiThread ? "MultiThread" : "");
//         std::optional<DeviceDescriptor> deviceDesc;
//         if (backend == RenderBackend::Vulkan) {
//             render::VulkanCommandQueueDescriptor queueDesc[] = {
//                 {render::QueueType::Direct, 1},
//                 {render::QueueType::Copy, 1}};
//             render::VulkanDeviceDescriptor devDesc{};
//             devDesc.Queues = queueDesc;
//             deviceDesc = devDesc;
//         }
//         ImGuiApplicationDescriptor desc{
//             name,
//             {1280, 720},
//             true,
//             false,
//             backend,
//             3,
//             TextureFormat::RGBA8_UNORM,
// #ifdef RADRAY_IS_DEBUG
//             true,
// #else
//             false,
// #endif
//             false,
//             false,
//             multiThread,
//             deviceDesc};
//         this->Init(desc);
//         _dxc = CreateDxc().Unwrap();
//         shared_ptr<Shader> vsShader, psShader;
//         {
//             string hlsl;
//             {
//                 auto hlslOpt = ReadText(std::filesystem::path("assets") / RADRAY_APP_NAME / "pbr.hlsl");
//                 if (!hlslOpt.has_value()) {
//                     throw ImGuiApplicationException("Failed to read shader file pbr.hlsl");
//                 }
//                 hlsl = std::move(hlslOpt.value());
//             }
//             DxcOutput vsBin;
//             {
//                 auto vs = _dxc->Compile(hlsl, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, false, {}, {}, backend == RenderBackend::Vulkan);
//                 if (!vs.has_value()) {
//                     throw ImGuiApplicationException("Failed to compile vertex shader");
//                 }
//                 vsBin = std::move(vs.value());
//             }
//             DxcOutput psBin;
//             {
//                 auto ps = _dxc->Compile(hlsl, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, false, {}, {}, backend == RenderBackend::Vulkan);
//                 if (!ps.has_value()) {
//                     throw ImGuiApplicationException("Failed to compile pixel shader");
//                 }
//                 psBin = std::move(ps.value());
//             }
//             ShaderDescriptor vsDesc{vsBin.Data, vsBin.Category};
//             vsShader = _device->CreateShader(vsDesc).Unwrap();
//             ShaderDescriptor psDesc{psBin.Data, psBin.Category};
//             psShader = _device->CreateShader(psDesc).Unwrap();
//         }
//         {
//             TriangleMesh sphereMesh{};
//             sphereMesh.InitAsUVSphere(0.5f, 32);
//             MeshResource sphereModel{};
//             sphereMesh.ToSimpleMeshResource(&sphereModel);

//             vector<RootSignatureSetElement> cbLayoutElems;
//             if (backend == RenderBackend::Vulkan) {
//                 cbLayoutElems.push_back({0, 0, ResourceBindType::CBuffer, 1, ShaderStage::Graphics, {}});
//                 cbLayoutElems.push_back({1, 0, ResourceBindType::CBuffer, 1, ShaderStage::Graphics, {}});
//             } else if (backend == RenderBackend::D3D12) {
//                 cbLayoutElems.push_back({1, 0, ResourceBindType::CBuffer, 2, ShaderStage::Graphics, {}});
//             }
//             RootSignatureDescriptorSet cbLayoutDesc{cbLayoutElems};
//             RootSignatureConstant rscDesc{};
//             rscDesc.Slot = 0;
//             rscDesc.Space = 0;
//             rscDesc.Size = 64 * 3;
//             rscDesc.Stages = ShaderStage::Vertex | ShaderStage::Pixel;
//             RootSignatureDescriptor rsDesc{};
//             rsDesc.DescriptorSets = std::span{&cbLayoutDesc, 1};
//             rsDesc.Constant = rscDesc;
//             _rs = _device->CreateRootSignature(rsDesc).Unwrap();
//             const auto& prim = sphereModel.Primitives[0];
//             vector<VertexElement> vertElems;
//             {
//                 SemanticMapping mapping[] = {
//                     {VertexSemantics::POSITION, 0, 0, VertexFormat::FLOAT32X3},
//                     {VertexSemantics::NORMAL, 0, 1, VertexFormat::FLOAT32X3},
//                     {VertexSemantics::TEXCOORD, 0, 2, VertexFormat::FLOAT32X2}};
//                 auto mves = MapVertexElements(prim.VertexBuffers, mapping);
//                 if (!mves.has_value()) {
//                     throw ImGuiApplicationException("Failed to map vertex elements");
//                 }
//                 vertElems = std::move(mves.value());
//             }
//             uint32_t stride = 0;
//             for (const auto& vb : prim.VertexBuffers) {
//                 stride += GetVertexDataSizeInBytes(vb.Type, vb.ComponentCount);
//             }
//             VertexBufferLayout vertLayout{};
//             vertLayout.ArrayStride = stride;
//             vertLayout.StepMode = VertexStepMode::Vertex;
//             vertLayout.Elements = vertElems;
//             ColorTargetState rtState = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
//             GraphicsPipelineStateDescriptor psoDesc{};
//             psoDesc.RootSig = _rs.get();
//             psoDesc.VS = {vsShader.get(), "VSMain"};
//             psoDesc.PS = {psShader.get(), "PSMain"};
//             psoDesc.VertexLayouts = std::span{&vertLayout, 1};
//             psoDesc.Primitive = PrimitiveState::Default();
//             psoDesc.DepthStencil = DepthStencilState::Default();
//             psoDesc.MultiSample = MultiSampleState::Default();
//             psoDesc.ColorTargets = std::span{&rtState, 1};
//             _pso = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();

//             BufferDescriptor vertDesc{
//                 sphereModel.Bins[0].GetSize(),
//                 MemoryType::Device,
//                 BufferUse::Vertex | BufferUse::CopyDestination,
//                 ResourceHint::None,
//                 "sphere_vertex"};
//             auto vert = _device->CreateBuffer(vertDesc).Unwrap();
//             BufferDescriptor indexDesc{
//                 sphereModel.Bins[1].GetSize(),
//                 MemoryType::Device,
//                 BufferUse::Index | BufferUse::CopyDestination,
//                 ResourceHint::None,
//                 "sphere_index"};
//             auto index = _device->CreateBuffer(indexDesc).Unwrap();

//             BufferDescriptor uploadDesc{
//                 sphereModel.Bins[0].GetSize() + sphereModel.Bins[1].GetSize(),
//                 MemoryType::Upload,
//                 BufferUse::CopySource,
//                 ResourceHint::None,
//                 "sphere_upload"};
//             auto upload = _device->CreateBuffer(uploadDesc).Unwrap();
//             void* dst = upload->Map(0, uploadDesc.Size);
//             std::memcpy(dst, sphereModel.Bins[0].GetData().data(), sphereModel.Bins[0].GetSize());
//             std::memcpy(static_cast<uint8_t*>(dst) + sphereModel.Bins[0].GetSize(), sphereModel.Bins[1].GetData().data(), sphereModel.Bins[1].GetSize());
//             upload->Unmap(0, uploadDesc.Size);

//             CommandQueue* copyQueue = _device->GetCommandQueue(QueueType::Copy).Unwrap();
//             shared_ptr<CommandBuffer> cmdBuffer = _device->CreateCommandBuffer(copyQueue).Unwrap();
//             cmdBuffer->Begin();
//             {
//                 BarrierBufferDescriptor barrierBefore[] = {
//                     {vert.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false},
//                     {index.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false}};
//                 cmdBuffer->ResourceBarrier(barrierBefore, {});
//             }
//             cmdBuffer->CopyBufferToBuffer(vert.get(), 0, upload.get(), 0, sphereModel.Bins[0].GetSize());
//             cmdBuffer->CopyBufferToBuffer(index.get(), 0, upload.get(), sphereModel.Bins[0].GetSize(), sphereModel.Bins[1].GetSize());
//             {
//                 BarrierBufferDescriptor barrierAfter[] = {
//                     {vert.get(), BufferUse::CopyDestination, BufferUse::Common, nullptr, false},
//                     {index.get(), BufferUse::CopyDestination, BufferUse::Common, nullptr, false}};
//                 cmdBuffer->ResourceBarrier(barrierAfter, {});
//             }
//             cmdBuffer->End();
//             CommandQueueSubmitDescriptor submitDesc{};
//             auto cmdBufferRef = cmdBuffer.get();
//             submitDesc.CmdBuffers = std::span{&cmdBufferRef, 1};
//             copyQueue->Submit(submitDesc);
//             copyQueue->Wait();

//             uint32_t indexStride = sphereModel.Primitives[0].IndexBuffer.Stride;
//             _meshes.emplace_back(HelloMesh{
//                 std::move(vert),
//                 std::move(index),
//                 sphereModel.Primitives[0].VertexCount,
//                 sphereModel.Primitives[0].IndexBuffer.IndexCount,
//                 indexStride});
//         }
//         RecreateDepthTexture();

//         BufferDescriptor cbDesc{
//             CBUFFER_SIZE * _frameCount,
//             MemoryType::Upload,
//             BufferUse::CBuffer,
//             ResourceHint::None,
//             "cbuffer"};
//         _cbuffer = _device->CreateBuffer(cbDesc).Unwrap();
//         _cbMappedPtr = _cbuffer->Map(0, cbDesc.Size);
//         _descSets.reserve(_frameCount);
//         for (size_t i = 0; i < _frameCount; i++) {
//             auto set = _device->CreateDescriptorSet(_rs.get(), 0).Unwrap();
//             _descSets.emplace_back(std::move(set));
//         }
//         _cbCamViews.reserve(_frameCount);
//         _cbMatViews.reserve(_frameCount);
//         for (size_t i = 0; i < _frameCount; i++) {
//             BufferViewDescriptor bvd{
//                 _cbuffer.get(),
//                 {i * CBUFFER_SIZE, sizeof(HelloCameraData)},
//                 0,
//                 TextureFormat::UNKNOWN,
//                 BufferUse::CBuffer};
//             _cbCamViews.emplace_back(_device->CreateBufferView(bvd).Unwrap());

//             BufferViewDescriptor mat{
//                 _cbuffer.get(),
//                 {i * CBUFFER_SIZE + sizeof(HelloCameraData), sizeof(MaterialData)},
//                 0,
//                 TextureFormat::UNKNOWN,
//                 BufferUse::CBuffer};
//             _cbMatViews.emplace_back(_device->CreateBufferView(mat).Unwrap());
//         }

//         _window->EventTouch().connect(&HelloPbr::OnTouch, this);
//         _window->EventMouseWheel().connect(&HelloPbr::OnMouseWheel, this);
//         _cc.Distance = (_modelPos - _camPos).norm();

//         MaterialData matData{};
//         matData.albedo[0] = 1.0f;
//         matData.albedo[1] = 0.766f;
//         matData.albedo[2] = 0.336f;
//         matData.metallic = 1.0;
//         matData.specularTint[0] = 1.0;
//         matData.specularTint[1] = 1.0;
//         matData.specularTint[2] = 1.0;
//         matData.specular = 0.5f;
//         matData.roughness = 0.2f;
//         matData.anisotropy = 0;
//         matData.ior = 1.5;
//         _mat = matData;
//     }

//     void OnImGui() override {
//         {
//             ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
//             int location = 0;
//             const float PAD = 10.0f;
//             const ImGuiViewport* viewport = ImGui::GetMainViewport();
//             ImVec2 workPos = viewport->WorkPos;
//             ImVec2 workSize = viewport->WorkSize;
//             ImVec2 windowPos, windowPosPivot;
//             windowPos.x = (location & 1) ? (workPos.x + workSize.x - PAD) : (workPos.x + PAD);
//             windowPos.y = (location & 2) ? (workPos.y + workSize.y - PAD) : (workPos.y + PAD);
//             windowPosPivot.x = (location & 1) ? 1.0f : 0.0f;
//             windowPosPivot.y = (location & 2) ? 1.0f : 0.0f;
//             ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPosPivot);
//             windowFlags |= ImGuiWindowFlags_NoMove;
//             ImGui::SetNextWindowBgAlpha(0.35f);
//             if (ImGui::Begin("RadrayMonitor", &_showMonitor, windowFlags)) {
//                 ImGui::Text("Logic  Time: (%09.4f ms)", _logicTime);
//                 ImGui::Text("Render Time: (%09.4f ms)", _renderTime.load());
//                 ImGui::Separator();
//                 if (ImGui::Checkbox("VSync", &_enableVSync)) {
//                     this->ExecuteOnRenderThreadBeforeAcquire([this]() {
//                         this->RecreateSwapChain();
//                     });
//                 }
//                 if (_multithreadRender) {
//                     ImGui::Checkbox("Wait Frame", &_isWaitFrame);
//                 }
//             }
//             ImGui::End();
//         }
//     }

//     void OnUpdate() override {}

//     void OnRender(ImGuiApplication::Frame* currFrame) override {
//         currFrame->_cmdBuffer->Begin();
//         _imguiDrawContext->BeforeDraw((int)currFrame->_frameIndex, currFrame->_cmdBuffer.get());
//         {
//             vector<BarrierTextureDescriptor> barriers;
//             {
//                 BarrierTextureDescriptor& barrier = barriers.emplace_back();
//                 barrier.Target = currFrame->_rt;
//                 barrier.Before = TextureUse::Uninitialized;
//                 barrier.After = TextureUse::RenderTarget;
//                 barrier.IsFromOrToOtherQueue = false;
//                 barrier.IsSubresourceBarrier = false;
//             }
//             if (_depthTexState == TextureUse::Uninitialized) {
//                 BarrierTextureDescriptor& barrier = barriers.emplace_back();
//                 barrier.Target = _depthTex.get();
//                 barrier.Before = TextureUse::Uninitialized;
//                 barrier.After = TextureUse::DepthStencilWrite;
//                 barrier.IsFromOrToOtherQueue = false;
//                 barrier.IsSubresourceBarrier = false;
//                 _depthTexState = TextureUse::DepthStencilWrite;
//             }
//             currFrame->_cmdBuffer->ResourceBarrier({}, barriers);
//         }
//         {
//             unique_ptr<CommandEncoder> pass;
//             {
//                 RenderPassDescriptor rpDesc{};
//                 ColorAttachment rtAttach{};
//                 rtAttach.Target = currFrame->_rtView;
//                 rtAttach.Load = LoadAction::Clear;
//                 rtAttach.Store = StoreAction::Store;
//                 rtAttach.ClearValue = ColorClearValue{0.1f, 0.1f, 0.1f, 1.0f};
//                 rpDesc.ColorAttachments = std::span(&rtAttach, 1);
//                 DepthStencilAttachment dsAttach{};
//                 dsAttach.Target = _depthTexView.get();
//                 dsAttach.DepthLoad = LoadAction::Clear;
//                 dsAttach.DepthStore = StoreAction::Store;
//                 dsAttach.StencilLoad = LoadAction::DontCare;
//                 dsAttach.StencilStore = StoreAction::Discard;
//                 dsAttach.ClearValue = DepthStencilClearValue{1.0f, 0};
//                 rpDesc.ColorAttachments = std::span(&rtAttach, 1);
//                 rpDesc.DepthStencilAttachment = dsAttach;
//                 pass = currFrame->_cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
//             }
//             {
//                 pass->BindRootSignature(_rs.get());
//                 pass->BindGraphicsPipelineState(_pso.get());
//                 Viewport viewport{0, 0, (float)_renderRtSize.x(), (float)_renderRtSize.y(), 0.0f, 1.0f};
//                 if (_device->GetBackend() == RenderBackend::Vulkan) {
//                     viewport.Y = (float)_renderRtSize.y();
//                     viewport.Height = -(float)_renderRtSize.y();
//                 }
//                 pass->SetViewport(viewport);
//                 pass->SetScissor({0, 0, (uint32_t)_renderRtSize.x(), (uint32_t)_renderRtSize.y()});

//                 Eigen::Matrix4f view = LookAt(_camRot, _camPos);

//                 Eigen::Matrix4f proj = PerspectiveLH(Radian(_camFovY), (float)_renderRtSize.x() / (float)_renderRtSize.y(), _camNear, _camFar);

//                 Eigen::Translation3f trans{_modelPos};
//                 Eigen::AlignedScaling3f scale{_modelScale};
//                 Eigen::Matrix4f model = (trans * _modelRot.toRotationMatrix() * scale).matrix();
//                 Eigen::Matrix4f modelInv = model.inverse();

//                 Eigen::Matrix4f vp = proj * view;
//                 Eigen::Matrix4f mvp = vp * model;
//                 byte preModel[64 * 3];
//                 std::memcpy(preModel, model.data(), sizeof(Eigen::Matrix4f));
//                 std::memcpy(preModel + 64, mvp.data(), sizeof(Eigen::Matrix4f));
//                 std::memcpy(preModel + 128, modelInv.data(), sizeof(Eigen::Matrix4f));
//                 pass->PushConstant(preModel, sizeof(preModel));
//                 DescriptorSet* set = _descSets[currFrame->_frameIndex].get();
//                 BufferView* cbView = _cbCamViews[currFrame->_frameIndex].get();
//                 HelloCameraData camData{};
//                 std::memcpy(camData.view, view.data(), sizeof(Eigen::Matrix4f));
//                 std::memcpy(camData.proj, proj.data(), sizeof(Eigen::Matrix4f));
//                 std::memcpy(camData.viewProj, vp.data(), sizeof(Eigen::Matrix4f));
//                 std::memcpy(camData.posW, _camPos.data(), sizeof(Eigen::Vector3f));
//                 std::memcpy(static_cast<uint8_t*>(_cbMappedPtr) + CBUFFER_SIZE * currFrame->_frameIndex, &camData, sizeof(HelloCameraData));
//                 if (backend == RenderBackend::Vulkan) {
//                     set->SetResource(1, 0, cbView);
//                 } else if (backend == RenderBackend::D3D12) {
//                     set->SetResource(0, 1, cbView);
//                 }
//                 BufferView* matView = _cbMatViews[currFrame->_frameIndex].get();
//                 std::memcpy(static_cast<uint8_t*>(_cbMappedPtr) + CBUFFER_SIZE * currFrame->_frameIndex + sizeof(HelloCameraData), &_mat, sizeof(MaterialData));
//                 if (backend == RenderBackend::Vulkan) {
//                     set->SetResource(0, 0, matView);
//                 } else if (backend == RenderBackend::D3D12) {
//                     set->SetResource(0, 0, matView);
//                 }
//                 pass->BindDescriptorSet(0, set);
//                 HelloMesh& mesh = _meshes[0];
//                 VertexBufferView vbv{
//                     mesh._vertexBuffer.get(),
//                     0,
//                     mesh._vertexSize};
//                 pass->BindVertexBuffer(std::span{&vbv, 1});
//                 pass->BindIndexBuffer({mesh._indexBuffer.get(), 0, mesh._indexStride});
//                 pass->DrawIndexed(mesh._indexCount, 1, 0, 0, 0);
//             }
//             currFrame->_cmdBuffer->EndRenderPass(std::move(pass));
//         }
//         {
//             unique_ptr<CommandEncoder> pass;
//             {
//                 RenderPassDescriptor rpDesc{};
//                 ColorAttachment rtAttach{};
//                 rtAttach.Target = currFrame->_rtView;
//                 rtAttach.Load = LoadAction::Load;
//                 rtAttach.Store = StoreAction::Store;
//                 rtAttach.ClearValue = ColorClearValue{};
//                 rpDesc.ColorAttachments = std::span(&rtAttach, 1);
//                 pass = currFrame->_cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
//             }
//             _imguiDrawContext->Draw((int)currFrame->_frameIndex, pass.get());
//             currFrame->_cmdBuffer->EndRenderPass(std::move(pass));
//         }
//         {
//             BarrierTextureDescriptor barrier{};
//             barrier.Target = currFrame->_rt;
//             barrier.Before = TextureUse::RenderTarget;
//             barrier.After = TextureUse::Present;
//             barrier.IsFromOrToOtherQueue = false;
//             barrier.IsSubresourceBarrier = false;
//             currFrame->_cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
//         }
//         currFrame->_cmdBuffer->End();
//         {
//             CommandQueueSubmitDescriptor submitDesc{};
//             auto cmdBuffer = currFrame->_cmdBuffer.get();
//             submitDesc.CmdBuffers = std::span{&cmdBuffer, 1};
//             _cmdQueue->Submit(submitDesc);
//         }
//     }

//     void OnDestroy() noexcept override {
//         _pso.reset();
//         _rs.reset();
//         _meshes.clear();
//         _depthTexView.reset();
//         _depthTex.reset();
//         _descSets.clear();
//         _cbCamViews.clear();
//         _cbMatViews.clear();
//         _cbuffer->Unmap(0, CBUFFER_SIZE * _frameCount);
//         _cbuffer.reset();
//     }

//     void OnResizing(int width, int height) override {
//         ExecuteOnRenderThreadBeforeAcquire([this, width, height]() {
//             this->_renderRtSize = Eigen::Vector2i{width, height};
//             this->_isResizingRender = true;
//         });
//     }

//     void OnResized(int width, int height) override {
//         ExecuteOnRenderThreadBeforeAcquire([this, width, height]() {
//             this->_renderRtSize = Eigen::Vector2i(width, height);
//             this->_isResizingRender = false;
//             if (width > 0 && height > 0) {
//                 this->RecreateSwapChain();
//                 this->RecreateDepthTexture();
//             }
//         });
//     }

//     void OnTouch(int x, int y, MouseButton button, Action action) {
//         Eigen::Vector2f curr{(float)x, (float)y};
//         switch (action) {
//             case Action::PRESSED: {
//                 switch (button) {
//                     case MouseButton::BUTTON_LEFT:
//                         _dragL = true;
//                         _cc.CanOrbit = true;
//                         _cc.CanFly = false;
//                         break;
//                     case MouseButton::BUTTON_MIDDLE:
//                         _dragM = true;
//                         _cc.CanPanXY = true;
//                         break;
//                     case MouseButton::BUTTON_RIGHT:
//                         _dragR = true;
//                         _cc.CanFly = true;
//                         _cc.CanOrbit = false;
//                         break;
//                     default:
//                         break;
//                 }
//                 _cc.LastPos = curr;
//                 _cc.NowPos = curr;
//                 _cc.PanDelta = 0.0f;
//                 break;
//             }
//             case Action::REPEATED: {
//                 _cc.NowPos = curr;
//                 if (_dragM) {
//                     _cc.CanPanXY = true;
//                     _cc.PanXY(_camPos, _camRot);
//                 } else if (_dragR) {
//                     _cc.CanFly = true;
//                     _cc.CanOrbit = false;
//                     _cc.Orbit(_camPos, _camRot);
//                 } else if (_dragL) {
//                     _cc.CanOrbit = true;
//                     _cc.CanFly = false;
//                     _cc.Orbit(_camPos, _camRot);
//                 }
//                 break;
//             }
//             case Action::RELEASED:
//                 switch (button) {
//                     case MouseButton::BUTTON_LEFT:
//                         _dragL = false;
//                         _cc.CanOrbit = false;
//                         break;
//                     case MouseButton::BUTTON_MIDDLE:
//                         _dragM = false;
//                         _cc.CanPanXY = false;
//                         break;
//                     case MouseButton::BUTTON_RIGHT:
//                         _dragR = false;
//                         _cc.CanFly = false;
//                         _cc.PanDelta = 0.0f;
//                         break;
//                     default:
//                         break;
//                 }
//                 break;
//             default:
//                 break;
//         }
//     }

//     void OnMouseWheel(int delta) {
//         _cc.PanDelta = static_cast<float>(delta) / 120.0f;
//         _cc.CanPanZWithDist = false;
//         _cc.PanZ(_camPos, _camRot);
//         _cc.PanDelta = 0.0f;
//     }

// private:
//     void RecreateDepthTexture() {
//         TextureDescriptor depthDesc{
//             TextureDimension::Dim2D,
//             (uint32_t)_renderRtSize.x(),
//             (uint32_t)_renderRtSize.y(),
//             1,
//             1,
//             1,
//             TextureFormat::D32_FLOAT,
//             TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite,
//             ResourceHint::None,
//             "depth_tex"};
//         _depthTex = _device->CreateTexture(depthDesc).Unwrap();
//         _depthTexState = TextureUse::Uninitialized;
//         TextureViewDescriptor depthViewDesc{
//             _depthTex.get(),
//             TextureViewDimension::Dim2D,
//             TextureFormat::D32_FLOAT,
//             SubresourceRange::AllSub(),
//             TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite};
//         _depthTexView = _device->CreateTextureView(depthViewDesc).Unwrap();
//     }

//     shared_ptr<Dxc> _dxc;
//     shared_ptr<RootSignature> _rs;
//     shared_ptr<GraphicsPipelineState> _pso;

//     shared_ptr<Texture> _depthTex;
//     shared_ptr<TextureView> _depthTexView;
//     TextureUse _depthTexState;

//     vector<HelloMesh> _meshes;

//     Eigen::Vector3f _camPos{0.0f, 0.0f, -3.0f};
//     Eigen::Quaternionf _camRot{Eigen::Quaternionf::Identity()};
//     float _camFovY{45.0f};
//     float _camNear{0.1f};
//     float _camFar{100.0f};

//     Eigen::Vector3f _modelPos{0.0f, 0.0f, 0.0f};
//     Eigen::Vector3f _modelScale{1.0f, 1.0f, 1.0f};
//     Eigen::Quaternionf _modelRot{Eigen::Quaternionf::Identity()};
//     CameraControl _cc{};
//     MaterialData _mat{};

//     vector<shared_ptr<DescriptorSet>> _descSets;
//     vector<shared_ptr<BufferView>> _cbCamViews;
//     vector<shared_ptr<BufferView>> _cbMatViews;
//     shared_ptr<Buffer> _cbuffer;
//     void* _cbMappedPtr;

//     RenderBackend backend;
//     bool multiThread;
//     bool _showMonitor{true};

//     bool _dragL{false};
//     bool _dragM{false};
//     bool _dragR{false};
// };

// int main(int argc, char** argv) {
//     InitImGui();
//     unique_ptr<HelloPbr> app;
//     {
//         RenderBackend backend{RenderBackend::Vulkan};
//         bool isMultiThread = true;
//         if (argc > 1) {
//             string backendStr = argv[1];
//             std::transform(backendStr.begin(), backendStr.end(), backendStr.begin(), [](char c) { return std::tolower(c); });
//             if (backendStr == "vulkan") {
//                 backend = RenderBackend::Vulkan;
//             } else if (backendStr == "d3d12") {
//                 backend = RenderBackend::D3D12;
//             } else {
//                 fmt::print("Unsupported backend: {}, using default Vulkan backend.\n", backendStr);
//                 return -1;
//             }
//         }
//         if (argc > 2) {
//             string mtStr = argv[2];
//             if (mtStr == "-st") {
//                 isMultiThread = false;
//             }
//         }
//         app = make_unique<HelloPbr>(backend, isMultiThread);
//     }
//     app->Run();
//     app->Destroy();
//     FlushLog();
//     return 0;
// }
