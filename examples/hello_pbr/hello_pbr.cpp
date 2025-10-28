#include <radray/logger.h>
#include <radray/enum_flags.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/render/common.h>
#include <radray/imgui/dear_imgui.h>

const char* RADRAY_APP_NAME = "Hello PBR";

using namespace radray;
using namespace radray::render;

constexpr IndexFormat MapIndexType(VertexIndexType type) noexcept {
    switch (type) {
        case VertexIndexType::UInt16: return IndexFormat::UINT16;
        case VertexIndexType::UInt32: return IndexFormat::UINT32;
        default: return IndexFormat::UINT16;
    }
}

class HelloMesh {
public:
    shared_ptr<Buffer> _vertexBuffer;
    shared_ptr<Buffer> _indexBuffer;
    uint32_t _indexCount{0};
    IndexFormat _indexFormat;
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
        render::VulkanCommandQueueDescriptor queueDesc[] = {
            {render::QueueType::Direct, 1},
            {render::QueueType::Copy, 1}};
        render::VulkanDeviceDescriptor devDesc{};
        devDesc.Queues = queueDesc;
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
            devDesc};
        this->Init(desc);
        {
            TriangleMesh sphereMesh{};
            sphereMesh.InitAsUVSphere(0.5f, 32);
            VertexData sphereModel{};
            sphereMesh.ToVertexData(&sphereModel);
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
            _meshes.emplace_back(HelloMesh{std::move(vert), std::move(index), sphereModel.IndexCount, MapIndexType(sphereModel.IndexType)});

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
                    {vert.get(), BufferUse::CopyDestination, BufferUse::Vertex, nullptr, false},
                    {index.get(), BufferUse::CopyDestination, BufferUse::Index, nullptr, false}};
                cmdBuffer->ResourceBarrier(barrierAfter, {});
            }
            cmdBuffer->End();
            CommandQueueSubmitDescriptor submitDesc{};
            auto cmdBufferRef = cmdBuffer.get();
            submitDesc.CmdBuffers = std::span{&cmdBufferRef, 1};
            copyQueue->Submit(submitDesc);
            copyQueue->Wait();
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
            BarrierTextureDescriptor barrier{};
            barrier.Target = currFrame->_rt;
            barrier.Before = TextureUse::Uninitialized;
            barrier.After = TextureUse::RenderTarget;
            barrier.IsFromOrToOtherQueue = false;
            barrier.IsSubresourceBarrier = false;
            currFrame->_cmdBuffer->ResourceBarrier({}, std::span{&barrier, 1});
        }
        unique_ptr<CommandEncoder> pass;
        {
            RenderPassDescriptor rpDesc{};
            ColorAttachment rtAttach{};
            rtAttach.Target = currFrame->_rtView;
            rtAttach.Load = LoadAction::Clear;
            rtAttach.Store = StoreAction::Store;
            rtAttach.ClearValue = ColorClearValue{0.1f, 0.1f, 0.1f, 1.0f};
            rpDesc.ColorAttachments = std::span(&rtAttach, 1);
            pass = currFrame->_cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
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
        _meshes.clear();
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
            }
        });
    }

private:
    vector<HelloMesh> _meshes;

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
