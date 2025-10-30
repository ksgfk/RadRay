#include <radray/logger.h>
#include <radray/basic_math.h>
#include <radray/enum_flags.h>
#include <radray/utility.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/camera_control.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/utility.h>
#include <radray/imgui/dear_imgui.h>

const char* RADRAY_APP_NAME = "hello_pbr";

using namespace radray;
using namespace radray::render;

struct alignas(256) HelloCameraData {
    float view[16];
    float proj[16];
    float viewProj[16];
    float posW[3];
};

class HelloMesh {
public:
    shared_ptr<Buffer> _vertexBuffer;
    shared_ptr<Buffer> _indexBuffer;
    uint32_t _vertexSize;
    uint32_t _indexCount;
    uint32_t _indexStride;
};

class HelloPbr : public ImGuiApplication {
public:
    HelloPbr(RenderBackend backend, bool multiThread)
        : ImGuiApplication(),
          backend(backend),
          multiThread(multiThread) {}

    ~HelloPbr() noexcept override = default;

    void OnStart() override {
        auto name = radray::format("{} - {} {}", string{RADRAY_APP_NAME}, backend, multiThread ? "MultiThread" : "");
        std::optional<DeviceDescriptor> deviceDesc;
        if (backend == RenderBackend::Vulkan) {
            render::VulkanCommandQueueDescriptor queueDesc[] = {
                {render::QueueType::Direct, 1},
                {render::QueueType::Copy, 1}};
            render::VulkanDeviceDescriptor devDesc{};
            devDesc.Queues = queueDesc;
            deviceDesc = devDesc;
        }
        ImGuiApplicationDescriptor desc{
            name,
            {1280, 720},
            true,
            false,
            backend,
            3,
            TextureFormat::RGBA8_UNORM,
#ifdef RADRAY_IS_DEBUG
            true,
#else
            false,
#endif
            false,
            false,
            multiThread,
            deviceDesc};
        this->Init(desc);
        _dxc = CreateDxc().Unwrap();
        shared_ptr<Shader> vsShader, psShader;
        {
            string hlsl;
            {
                auto hlslOpt = ReadText(std::filesystem::path("assets") / RADRAY_APP_NAME / "pbr.hlsl");
                if (!hlslOpt.has_value()) {
                    throw ImGuiApplicationException("Failed to read shader file pbr.hlsl");
                }
                hlsl = std::move(hlslOpt.value());
            }
            DxcOutput vsBin;
            {
                auto vs = _dxc->Compile(hlsl, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, false, {}, {}, backend == RenderBackend::Vulkan);
                if (!vs.has_value()) {
                    throw ImGuiApplicationException("Failed to compile vertex shader");
                }
                vsBin = std::move(vs.value());
            }
            DxcOutput psBin;
            {
                auto ps = _dxc->Compile(hlsl, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, false, {}, {}, backend == RenderBackend::Vulkan);
                if (!ps.has_value()) {
                    throw ImGuiApplicationException("Failed to compile pixel shader");
                }
                psBin = std::move(ps.value());
            }
            ShaderDescriptor vsDesc{vsBin.Data, vsBin.Category};
            vsShader = _device->CreateShader(vsDesc).Unwrap();
            ShaderDescriptor psDesc{psBin.Data, psBin.Category};
            psShader = _device->CreateShader(psDesc).Unwrap();
        }
        {
            TriangleMesh sphereMesh{};
            sphereMesh.InitAsUVSphere(0.5f, 32);
            VertexData sphereModel{};
            sphereMesh.ToVertexData(&sphereModel);

            RootSignatureSetElement cbLayoutElems[] = {
                {1, 0, ResourceBindType::CBuffer, 1, ShaderStage::Graphics, {}}};
            RootSignatureBindingSet cbLayoutDesc{cbLayoutElems};
            _cbLayout = _device->CreateDescriptorSetLayout(cbLayoutDesc).Unwrap();
            RootSignatureConstant rscDesc{};
            rscDesc.Slot = 0;
            rscDesc.Space = 0;
            rscDesc.Size = 128;
            rscDesc.Stages = ShaderStage::Vertex | ShaderStage::Pixel;
            DescriptorSetLayout* layouts[] = {_cbLayout.get()};
            RootSignatureDescriptor rsDesc{};
            rsDesc.BindingSets = layouts;
            rsDesc.Constant = rscDesc;
            _rs = _device->CreateRootSignature(rsDesc).Unwrap();
            vector<VertexElement> vertElems;
            {
                SemanticMapping mapping[] = {
                    {VertexSemantic::POSITION, 0, 0, VertexFormat::FLOAT32X3},
                    {VertexSemantic::NORMAL, 0, 1, VertexFormat::FLOAT32X3},
                    {VertexSemantic::TEXCOORD, 0, 2, VertexFormat::FLOAT32X2}};
                auto mves = MapVertexElements(sphereModel.Layouts, mapping);
                if (!mves.has_value()) {
                    throw ImGuiApplicationException("Failed to map vertex elements");
                }
                vertElems = std::move(mves.value());
            }
            VertexBufferLayout vertLayout{};
            vertLayout.ArrayStride = sphereModel.GetStride();
            vertLayout.StepMode = VertexStepMode::Vertex;
            vertLayout.Elements = vertElems;
            ColorTargetState rtState = DefaultColorTargetState(TextureFormat::RGBA8_UNORM);
            GraphicsPipelineStateDescriptor psoDesc{};
            psoDesc.RootSig = _rs.get();
            psoDesc.VS = {vsShader.get(), "VSMain"};
            psoDesc.PS = {psShader.get(), "PSMain"};
            psoDesc.VertexLayouts = std::span{&vertLayout, 1};
            psoDesc.Primitive = DefaultPrimitiveState();
            psoDesc.DepthStencil = DefaultDepthStencilState();
            psoDesc.MultiSample = DefaultMultiSampleState();
            psoDesc.ColorTargets = std::span{&rtState, 1};
            _pso = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();

            BufferDescriptor vertDesc{
                sphereModel.VertexSize,
                MemoryType::Device,
                BufferUse::Vertex | BufferUse::CopyDestination,
                ResourceHint::None,
                "sphere_vertex"};
            auto vert = _device->CreateBuffer(vertDesc).Unwrap();
            BufferDescriptor indexDesc{
                sphereModel.IndexSize,
                MemoryType::Device,
                BufferUse::Index | BufferUse::CopyDestination,
                ResourceHint::None,
                "sphere_index"};
            auto index = _device->CreateBuffer(indexDesc).Unwrap();

            BufferDescriptor uploadDesc{
                sphereModel.VertexSize + sphereModel.IndexSize,
                MemoryType::Upload,
                BufferUse::CopySource,
                ResourceHint::None,
                "sphere_upload"};
            auto upload = _device->CreateBuffer(uploadDesc).Unwrap();
            void* dst = upload->Map(0, uploadDesc.Size);
            std::memcpy(dst, sphereModel.VertexData.get(), sphereModel.VertexSize);
            std::memcpy(static_cast<uint8_t*>(dst) + sphereModel.VertexSize, sphereModel.IndexData.get(), sphereModel.IndexSize);
            upload->Unmap(0, uploadDesc.Size);

            CommandQueue* copyQueue = _device->GetCommandQueue(QueueType::Copy).Unwrap();
            shared_ptr<CommandBuffer> cmdBuffer = _device->CreateCommandBuffer(copyQueue).Unwrap();
            cmdBuffer->Begin();
            {
                BarrierBufferDescriptor barrierBefore[] = {
                    {vert.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false},
                    {index.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false}};
                cmdBuffer->ResourceBarrier(barrierBefore, {});
            }
            cmdBuffer->CopyBufferToBuffer(vert.get(), 0, upload.get(), 0, sphereModel.VertexSize);
            cmdBuffer->CopyBufferToBuffer(index.get(), 0, upload.get(), sphereModel.VertexSize, sphereModel.IndexSize);
            {
                BarrierBufferDescriptor barrierAfter[] = {
                    {vert.get(), BufferUse::CopyDestination, BufferUse::Common, nullptr, false},
                    {index.get(), BufferUse::CopyDestination, BufferUse::Common, nullptr, false}};
                cmdBuffer->ResourceBarrier(barrierAfter, {});
            }
            cmdBuffer->End();
            CommandQueueSubmitDescriptor submitDesc{};
            auto cmdBufferRef = cmdBuffer.get();
            submitDesc.CmdBuffers = std::span{&cmdBufferRef, 1};
            copyQueue->Submit(submitDesc);
            copyQueue->Wait();

            uint32_t indexStride = GetIndexFormatSize(MapIndexType(sphereModel.IndexType));
            _meshes.emplace_back(HelloMesh{
                std::move(vert),
                std::move(index),
                sphereModel.VertexSize,
                sphereModel.IndexCount,
                indexStride});
        }
        RecreateDepthTexture();

        BufferDescriptor cbDesc{
            sizeof(HelloCameraData) * _frameCount,
            MemoryType::Upload,
            BufferUse::CBuffer,
            ResourceHint::None,
            "cbuffer"};
        _cbuffer = _device->CreateBuffer(cbDesc).Unwrap();
        _cbMappedPtr = _cbuffer->Map(0, cbDesc.Size);
        _descSets.reserve(_frameCount);
        for (size_t i = 0; i < _frameCount; i++) {
            auto set = _device->CreateDescriptorSet(_cbLayout.get()).Unwrap();
            _descSets.emplace_back(std::move(set));
        }
        _cbViews.reserve(_frameCount);
        for (size_t i = 0; i < _frameCount; i++) {
            BufferViewDescriptor bvd{
                _cbuffer.get(),
                {i * sizeof(HelloCameraData), sizeof(HelloCameraData)},
                0,
                TextureFormat::UNKNOWN,
                BufferUse::CBuffer};
            _cbViews.emplace_back(_device->CreateBufferView(bvd).Unwrap());
        }
    }

    void OnImGui() override {
        {
            ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            int location = 0;
            const float PAD = 10.0f;
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 workPos = viewport->WorkPos;
            ImVec2 workSize = viewport->WorkSize;
            ImVec2 windowPos, windowPosPivot;
            windowPos.x = (location & 1) ? (workPos.x + workSize.x - PAD) : (workPos.x + PAD);
            windowPos.y = (location & 2) ? (workPos.y + workSize.y - PAD) : (workPos.y + PAD);
            windowPosPivot.x = (location & 1) ? 1.0f : 0.0f;
            windowPosPivot.y = (location & 2) ? 1.0f : 0.0f;
            ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPosPivot);
            ImGui::SetNextWindowViewport(viewport->ID);
            windowFlags |= ImGuiWindowFlags_NoMove;
            ImGui::SetNextWindowBgAlpha(0.35f);
            if (ImGui::Begin("RadrayMonitor", &_showMonitor, windowFlags)) {
                ImGui::Text("Logic  Time: (%09.4f ms)", _logicTime);
                ImGui::Text("Render Time: (%09.4f ms)", _renderTime.load());
                ImGui::Separator();
                if (ImGui::Checkbox("VSync", &_enableVSync)) {
                    this->ExecuteOnRenderThreadBeforeAcquire([this]() {
                        this->RecreateSwapChain();
                    });
                }
                if (_multithreadRender) {
                    ImGui::Checkbox("Wait Frame", &_isWaitFrame);
                }
            }
            ImGui::End();
        }
    }

    void OnUpdate() override {}

    void OnRender(ImGuiApplication::Frame* currFrame) override {
        currFrame->_cmdBuffer->Begin();
        _imguiDrawContext->BeforeDraw((int)currFrame->_frameIndex, currFrame->_cmdBuffer.get());
        {
            vector<BarrierTextureDescriptor> barriers;
            {
                BarrierTextureDescriptor& barrier = barriers.emplace_back();
                barrier.Target = currFrame->_rt;
                barrier.Before = TextureUse::Uninitialized;
                barrier.After = TextureUse::RenderTarget;
                barrier.IsFromOrToOtherQueue = false;
                barrier.IsSubresourceBarrier = false;
            }
            if (_depthTexState == TextureUse::Uninitialized) {
                BarrierTextureDescriptor& barrier = barriers.emplace_back();
                barrier.Target = _depthTex.get();
                barrier.Before = TextureUse::Uninitialized;
                barrier.After = TextureUse::DepthStencilWrite;
                barrier.IsFromOrToOtherQueue = false;
                barrier.IsSubresourceBarrier = false;
                _depthTexState = TextureUse::DepthStencilWrite;
            }
            currFrame->_cmdBuffer->ResourceBarrier({}, barriers);
        }
        // {
        //     unique_ptr<CommandEncoder> pass;
        //     {
        //         RenderPassDescriptor rpDesc{};
        //         ColorAttachment rtAttach{};
        //         rtAttach.Target = currFrame->_rtView;
        //         rtAttach.Load = LoadAction::Clear;
        //         rtAttach.Store = StoreAction::Store;
        //         rtAttach.ClearValue = ColorClearValue{0.1f, 0.1f, 0.1f, 1.0f};
        //         DepthStencilAttachment dsAttach{};
        //         dsAttach.Target = _depthTexView.get();
        //         dsAttach.DepthLoad = LoadAction::Clear;
        //         dsAttach.DepthStore = StoreAction::Store;
        //         dsAttach.StencilLoad = LoadAction::DontCare;
        //         dsAttach.StencilStore = StoreAction::Discard;
        //         dsAttach.ClearValue = DepthStencilClearValue{1.0f, 0};
        //         rpDesc.ColorAttachments = std::span(&rtAttach, 1);
        //         rpDesc.DepthStencilAttachment = dsAttach;
        //         pass = currFrame->_cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        //     }
        //     pass->BindRootSignature(_rs.get());
        //     pass->BindGraphicsPipelineState(_pso.get());
        //     pass->SetViewport({0, 0, (float)_renderRtSize.x(), (float)_renderRtSize.y(), 0.0f, 1.0f});
        //     pass->SetScissor({0, 0, (uint32_t)_renderRtSize.x(), (uint32_t)_renderRtSize.y()});
        //     const HelloMesh& mesh = _meshes[0];
        //     {
        //         VertexBufferView vbv{};
        //         vbv.Target = mesh._vertexBuffer.get();
        //         vbv.Offset = 0;
        //         vbv.Size = mesh._vertexSize;
        //         pass->BindVertexBuffer(std::span{&vbv, 1});
        //     }
        //     {
        //         IndexBufferView ibv{};
        //         ibv.Target = mesh._indexBuffer.get();
        //         ibv.Offset = 0;
        //         ibv.Stride = mesh._indexStride;
        //         pass->BindIndexBuffer(ibv);
        //     }
        //     pass->DrawIndexed(mesh._indexCount, 0, 0, 0, 0);
        //     currFrame->_cmdBuffer->EndRenderPass(std::move(pass));
        // }

        unique_ptr<CommandEncoder> pass;
        {
            RenderPassDescriptor rpDesc{};
            ColorAttachment rtAttach{};
            rtAttach.Target = currFrame->_rtView;
            rtAttach.Load = LoadAction::Clear;
            rtAttach.Store = StoreAction::Store;
            rtAttach.ClearValue = ColorClearValue{0.1f, 0.1f, 0.1f, 1.0f};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            DepthStencilAttachment dsAttach{};
            dsAttach.Target = _depthTexView.get();
            dsAttach.DepthLoad = LoadAction::Clear;
            dsAttach.DepthStore = StoreAction::Store;
            dsAttach.StencilLoad = LoadAction::DontCare;
            dsAttach.StencilStore = StoreAction::Discard;
            dsAttach.ClearValue = DepthStencilClearValue{1.0f, 0};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            rpDesc.DepthStencilAttachment = dsAttach;
            pass = currFrame->_cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        }
        {
            pass->BindRootSignature(_rs.get());
            pass->BindGraphicsPipelineState(_pso.get());
            pass->SetViewport({0, 0, (float)_renderRtSize.x(), (float)_renderRtSize.y(), 0.0f, 1.0f});
            pass->SetScissor({0, 0, (uint32_t)_renderRtSize.x(), (uint32_t)_renderRtSize.y()});

            Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
            Eigen::Matrix3f invRot = _camRot.conjugate().toRotationMatrix();
            view.block<3, 3>(0, 0) = invRot;
            view.block<3, 1>(0, 3) = -invRot * _camPos;

            Eigen::Matrix4f proj = PerspectiveLH(Radian(_camFovY), (float)_renderRtSize.x() / (float)_renderRtSize.y(), _camNear, _camFar);

            Eigen::Translation3f trans{_modelPos};
            Eigen::AlignedScaling3f scale{_modelScale};
            Eigen::Matrix4f model = (trans * _modelRot.toRotationMatrix() * scale).matrix();

            Eigen::Matrix4f vp = proj * view;
            Eigen::Matrix4f mvp = vp * model;
            byte preModel[128];
            std::memcpy(preModel, model.data(), sizeof(Eigen::Matrix4f));
            std::memcpy(preModel + 64, mvp.data(), sizeof(Eigen::Matrix4f));
            pass->PushConstant(preModel, 128);
            DescriptorSet* set = _descSets[currFrame->_frameIndex].get();
            BufferView* cbView = _cbViews[currFrame->_frameIndex].get();
            HelloCameraData camData{};
            std::memcpy(camData.view, view.data(), sizeof(Eigen::Matrix4f));
            std::memcpy(camData.proj, proj.data(), sizeof(Eigen::Matrix4f));
            std::memcpy(camData.viewProj, vp.data(), sizeof(Eigen::Matrix4f));
            std::memcpy(camData.posW, _modelPos.data(), sizeof(Eigen::Vector3f));
            std::memcpy(static_cast<uint8_t*>(_cbMappedPtr) + sizeof(HelloCameraData) * currFrame->_frameIndex, &camData, sizeof(HelloCameraData));
            set->SetResource(0, cbView);
            pass->BindDescriptorSet(0, set);
            HelloMesh& mesh = _meshes[0];
            VertexBufferView vbv{
                mesh._vertexBuffer.get(),
                0,
                mesh._vertexSize};
            pass->BindVertexBuffer(std::span{&vbv, 1});
            pass->BindIndexBuffer({mesh._indexBuffer.get(), 0, mesh._indexStride});
            pass->DrawIndexed(mesh._indexCount, 1, 0, 0, 0);
        }
        _imguiDrawContext->Draw((int)currFrame->_frameIndex, pass.get());
        currFrame->_cmdBuffer->EndRenderPass(std::move(pass));
        {
            BarrierTextureDescriptor barrier{};
            barrier.Target = currFrame->_rt;
            barrier.Before = TextureUse::RenderTarget;
            barrier.After = TextureUse::Present;
            barrier.IsFromOrToOtherQueue = false;
            barrier.IsSubresourceBarrier = false;
            currFrame->_cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        currFrame->_cmdBuffer->End();
        {
            CommandQueueSubmitDescriptor submitDesc{};
            auto cmdBuffer = currFrame->_cmdBuffer.get();
            submitDesc.CmdBuffers = std::span{&cmdBuffer, 1};
            _cmdQueue->Submit(submitDesc);
        }
    }

    void OnDestroy() noexcept override {
        _pso.reset();
        _rs.reset();
        _meshes.clear();
        _depthTexView.reset();
        _depthTex.reset();
        _cbLayout.reset();
        _descSets.clear();
        _cbViews.clear();
        _cbuffer.reset();
    }

    void OnResizing(int width, int height) override {
        ExecuteOnRenderThreadBeforeAcquire([this, width, height]() {
            this->_renderRtSize = Eigen::Vector2i{width, height};
            this->_isResizingRender = true;
        });
    }

    void OnResized(int width, int height) override {
        ExecuteOnRenderThreadBeforeAcquire([this, width, height]() {
            this->_renderRtSize = Eigen::Vector2i(width, height);
            this->_isResizingRender = false;
            if (width > 0 && height > 0) {
                this->RecreateSwapChain();
                this->RecreateDepthTexture();
            }
        });
    }

private:
    void RecreateDepthTexture() {
        TextureDescriptor depthDesc{
            TextureDimension::Dim2D,
            (uint32_t)_renderRtSize.x(),
            (uint32_t)_renderRtSize.y(),
            1,
            1,
            1,
            TextureFormat::D32_FLOAT,
            TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite,
            ResourceHint::None,
            "depth_tex"};
        _depthTex = _device->CreateTexture(depthDesc).Unwrap();
        _depthTexState = TextureUse::Uninitialized;
        TextureViewDescriptor depthViewDesc{
            _depthTex.get(),
            TextureViewDimension::Dim2D,
            TextureFormat::D32_FLOAT,
            SubresourceRange::AllSub(),
            TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite};
        _depthTexView = _device->CreateTextureView(depthViewDesc).Unwrap();
    }

    shared_ptr<Dxc> _dxc;
    shared_ptr<RootSignature> _rs;
    shared_ptr<GraphicsPipelineState> _pso;
    shared_ptr<DescriptorSetLayout> _cbLayout;

    shared_ptr<Texture> _depthTex;
    shared_ptr<TextureView> _depthTexView;
    TextureUse _depthTexState;

    vector<HelloMesh> _meshes;

    Eigen::Vector3f _camPos{0.0f, 0.0f, 3.0f};
    Eigen::Quaternionf _camRot{Eigen::Quaternionf::Identity()};
    float _camFovY{45.0f};
    float _camNear{0.1f};
    float _camFar{100.0f};

    Eigen::Vector3f _modelPos{0.0f, 0.0f, 0.0f};
    Eigen::Vector3f _modelScale{1.0f, 1.0f, 1.0f};
    Eigen::Quaternionf _modelRot{Eigen::Quaternionf::Identity()};

    vector<shared_ptr<DescriptorSet>> _descSets;
    vector<shared_ptr<BufferView>> _cbViews;
    shared_ptr<Buffer> _cbuffer;
    void* _cbMappedPtr;

    RenderBackend backend;
    bool multiThread;
    bool _showMonitor{true};
};

int main(int argc, char** argv) {
    InitImGui();
    unique_ptr<HelloPbr> app;
    {
        RenderBackend backend{RenderBackend::Vulkan};
        bool isMultiThread = true;
        if (argc > 1) {
            string backendStr = argv[1];
            std::transform(backendStr.begin(), backendStr.end(), backendStr.begin(), [](char c) { return std::tolower(c); });
            if (backendStr == "vulkan") {
                backend = RenderBackend::Vulkan;
            } else if (backendStr == "d3d12") {
                backend = RenderBackend::D3D12;
            } else {
                fmt::print("Unsupported backend: {}, using default Vulkan backend.\n", backendStr);
                return -1;
            }
        }
        if (argc > 2) {
            string mtStr = argv[2];
            if (mtStr == "-st") {
                isMultiThread = false;
            }
        }
        app = make_unique<HelloPbr>(backend, isMultiThread);
    }
    app->Run();
    app->Destroy();
    FlushLog();
    return 0;
}
