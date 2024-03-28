#include <thread>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/window/native_window.h>
#include <radray/d3d12/device.h>
#include <radray/d3d12/command_queue.h>
#include <radray/d3d12/command_allocator.h>
#include <radray/d3d12/command_list.h>
#include <radray/d3d12/swap_chain.h>
#include <radray/d3d12/default_buffer.h>
#include <radray/d3d12/upload_buffer.h>
#include <radray/d3d12/raster_shader.h>

using namespace radray;

const char* shaderSrc = R"(
cbuffer cbPreObject : register(b0) {
    float4x4 g_Model;
    float4x4 g_MVP;
    float4x4 g_InvM;
};

struct VSIn {
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
};

struct VSOut {
    float4 PosClip : SV_POSITION;
    float3 NormalW : NORMAL;
};

VSOut VSMain(VSIn vin) {
    VSOut v;
    v.PosClip = mul(g_MVP, float4(vin.Pos, 1.0f));
    v.NormalW = normalize(mul(transpose((float3x3)g_InvM), vin.Normal));
    return v;
}

float4 PSMain(VSOut v) : SV_Target {
    float3 nw = normalize(v.NormalW);
    float4 color = float4((nw + 1.0f) * 0.5f, 1.0f);
    return color;
}
)";

class App {
public:
    App() { window::GlobalInit(); }
    ~App() {
        window->Destroy();
        window::GlobalTerminate();
    }

    void Init() {
        clearColor[0] = 0.5f;
        clearColor[1] = 0.5f;
        clearColor[2] = 0.5f;
        clearColor[3] = 1;
        window = std::make_unique<window::NativeWindow>("test d3d12", 1280, 720, true);
        winResizeDelegate = {
            [this](const Eigen::Vector2i& size) { OnWindowResize(size); },
            window->EventWindwResize()};
        device = std::make_unique<d3d12::Device>();
        directQueue = std::make_unique<d3d12::CommandQueue>(device.get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        for (int i = 0; i < 3; i++) {
            cmdAllocs[i] = std::make_unique<d3d12::CommandAllocator>(device.get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        }
        Eigen::Vector2i winSize = window->GetSize();
        swapChain = std::make_unique<d3d12::SwapChain>(device.get(), directQueue.get(), (HWND)window->GetNativeHandle(), 3, winSize.x(), winSize.y(), false);
    }

    void Start() {
        {
            colorShader = std::make_unique<d3d12::RasterShader>(device.get());
            d3d12::RasterShaderCompileResult compile = device->shaderCompiler->CompileRaster(shaderSrc, 61, false);
            if (!compile.IsValid()) {
                compile.LogErrorIfInvalid();
                RADRAY_ABORT("compile shader error");
            }
            colorShader->Setup(&compile);
        }
        d3d12::CommandAllocator* cmdAlloc = cmdAllocs[0].get();
        d3d12::CommandList* cmdList = cmdAlloc->cmd.get();
        d3d12::ResourceStateTracker* stateTracker = &cmdAlloc->stateTracker;
        ID3D12GraphicsCommandList* cmd = cmdList->cmd.Get();
        cmdAlloc->Reset();
        TriangleMesh mesh{};
        mesh.InitAsUVSphere(0.5f, 64);
        VertexData vData{};
        mesh.ToVertexData(&vData);
        d3d12::RasterPipelineStateInfo psInfo{};
        psInfo.InitDefault();
        psInfo.InitInputElements(colorShader->inputDefines, &vData, 0);
        cubeMatPso = colorShader->GetOrCreatePso(psInfo);
        cubeVertex = std::make_unique<d3d12::DefaultBuffer>(device.get(), vData.vertexSize);
        cubeIndex = std::make_unique<d3d12::DefaultBuffer>(device.get(), vData.indexSize);
        stateTracker->Track(cubeVertex.get(), D3D12_RESOURCE_STATE_COPY_DEST);
        stateTracker->Track(cubeIndex.get(), D3D12_RESOURCE_STATE_COPY_DEST);
        stateTracker->Update(cmd);
        cmdList->Upload(cubeVertex->GetResource(), 0, {vData.vertexData.get(), vData.vertexSize});
        cmdList->Upload(cubeIndex->GetResource(), 0, {vData.indexData.get(), vData.indexSize});
        cubeVertex->SetInitState(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        cubeIndex->SetInitState(D3D12_RESOURCE_STATE_INDEX_BUFFER);
        stateTracker->Restore(cmd);
        cmdList->Close();
        directQueue->Execute(cmdAlloc);
        directQueue->Flush();
    }

    void Run() {
        while (!window->ShouldClose()) {
            window::GlobalPollEvents();
            Draw();
            std::this_thread::yield();
        }
        directQueue->Flush();
    }

    void Draw() {
        uint32 nowBackBufferIndex = swapChain->backBufferIndex;
        d3d12::SwapChainRenderTarget* swapChainRt = &swapChain->renderTargets[nowBackBufferIndex];
        d3d12::CommandAllocator* cmdAlloc = cmdAllocs[nowBackBufferIndex].get();
        d3d12::CommandList* cmdList = cmdAlloc->cmd.get();
        d3d12::ResourceStateTracker* stateTracker = &cmdAlloc->stateTracker;
        ID3D12GraphicsCommandList* cmd = cmdList->cmd.Get();
        directQueue->WaitFrame(cmdAlloc->lastExecuteFenceIndex);
        cmdAlloc->Reset();
        auto rtDesc = cmdAlloc->rtvHeap.Allocate(1);
        rtDesc.handle->CreateRtv(swapChainRt->GetResource(), swapChainRt->GetRtvDesc(), rtDesc.offset);
        auto rtv = rtDesc.handle->HandleCPU(rtDesc.offset);
        stateTracker->Track(swapChainRt, D3D12_RESOURCE_STATE_RENDER_TARGET);
        stateTracker->Update(cmd);
        cmd->OMSetRenderTargets(1, &rtv, false, nullptr);
        cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        stateTracker->Restore(cmd);
        cmdList->Close();
        directQueue->Execute(cmdAlloc);
        swapChain->Present();
    }

    void OnWindowResize(const Eigen::Vector2i& size) {
        if (size.x() >= 16 && size.y() >= 16) {
            directQueue->Flush();
            swapChain->Resize(size.x(), size.y());
        }
        Draw();
    }

    std::unique_ptr<window::NativeWindow> window;
    std::unique_ptr<d3d12::Device> device;
    std::unique_ptr<d3d12::CommandQueue> directQueue;
    std::unique_ptr<d3d12::CommandAllocator> cmdAllocs[3];
    std::unique_ptr<d3d12::SwapChain> swapChain;

    std::unique_ptr<d3d12::RasterShader> colorShader;

    std::unique_ptr<d3d12::DefaultBuffer> cubeVertex;
    std::unique_ptr<d3d12::DefaultBuffer> cubeIndex;
    d3d12::ComPtr<ID3D12PipelineState> cubeMatPso;

    DelegateHandle<window::WindowResizeCallback> winResizeDelegate;
    float clearColor[4];
    Eigen::Vector3f camPos;
    Eigen::Quaternionf camRot;
    float camFov;
    float camNear;
    float camFar;
};

int main() {
    auto app = std::make_unique<App>();
    app->Init();
    app->Start();
    app->Run();
    return 0;
}
