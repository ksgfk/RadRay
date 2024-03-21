#include <radray/triangle_mesh.h>
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
        window = std::make_unique<window::NativeWindow>("test d3d12", 1280, 720);
        device = std::make_unique<d3d12::Device>();
        directQueue = std::make_unique<d3d12::CommandQueue>(device.get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        for (int i = 0; i < 3; i++) {
            cmdAllocs[i] = std::make_unique<d3d12::CommandAllocator>(device.get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        }
        swapChain = std::make_unique<d3d12::SwapChain>(device.get(), directQueue.get(), (HWND)window->GetNativeHandle(), 3, 1280, 720, false);
    }

    void Start() {
        {
            shader = std::make_unique<d3d12::RasterShader>(device.get());
            d3d12::RasterShaderCompileResult compile = device->shaderCompiler->CompileRaster(shaderSrc, 61, false);
            if (!compile.IsValid()) {
                compile.LogErrorIfInvalid();
                RADRAY_ABORT("compile shader error");
            }
            shader->Setup(&compile);
        }
        d3d12::CommandAllocator* cmdAlloc = cmdAllocs[0].get();
        d3d12::CommandList* cmdList = cmdAlloc->cmd.get();
        d3d12::ResourceStateTracker* stateTracker = &cmdAlloc->stateTracker;
        ID3D12GraphicsCommandList* cmd = cmdList->cmd.Get();
        cmdAlloc->Reset();
        TriangleMesh mesh{};
        mesh.InitAsCube(0.5f);
        cube = std::make_unique<d3d12::DefaultBuffer>(device.get(), mesh.positions.size() * sizeof(Eigen::Vector3f));
        stateTracker->Track(cube.get(), D3D12_RESOURCE_STATE_COPY_DEST);
        stateTracker->Update(cmd);
        cmdList->Upload(cube->GetResource(), 0, {reinterpret_cast<const uint8*>(mesh.positions.data()), cube->GetByteSize()});
        cube->SetInitState(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        stateTracker->Restore(cmd);
        cmdList->Close();
        directQueue->Execute(cmdAlloc);
        directQueue->Flush();
    }

    void Run() {
        while (!window->ShouldClose()) {
            uint32 nowBackBufferIndex = swapChain->backBufferIndex;
            d3d12::SwapChainRenderTarget* swapChainRt = &swapChain->renderTargets[nowBackBufferIndex];
            d3d12::CommandAllocator* cmdAlloc = cmdAllocs[nowBackBufferIndex].get();
            d3d12::CommandList* cmdList = cmdAlloc->cmd.get();
            d3d12::ResourceStateTracker* stateTracker = &cmdAlloc->stateTracker;
            ID3D12GraphicsCommandList* cmd = cmdList->cmd.Get();
            directQueue->WaitFrame(cmdAlloc->lastExecuteFenceIndex);
            window::GlobalPollEvents();
            cmdAlloc->Reset();
            auto rtDesc = cmdAlloc->rtvHeap.Allocate(1);
            rtDesc.handle->CreateRtv(swapChainRt->GetResource(), swapChainRt->GetRtvDesc(), rtDesc.offset);
            auto rtv = rtDesc.handle->HandleCPU(rtDesc.offset);
            stateTracker->Track(swapChainRt, D3D12_RESOURCE_STATE_RENDER_TARGET);
            stateTracker->Update(cmd);
            cmd->OMSetRenderTargets(1, &rtv, false, nullptr);
            float c[] = {0.5f, 0.5f, 0.5f, 1};
            cmd->ClearRenderTargetView(rtv, c, 0, nullptr);
            stateTracker->Restore(cmd);
            cmdList->Close();
            directQueue->Execute(cmdAlloc);
            swapChain->Present();
        }
        directQueue->Flush();
    }

    std::unique_ptr<window::NativeWindow> window;
    std::unique_ptr<d3d12::Device> device;
    std::unique_ptr<d3d12::CommandQueue> directQueue;
    std::unique_ptr<d3d12::CommandAllocator> cmdAllocs[3];
    std::unique_ptr<d3d12::SwapChain> swapChain;
    std::unique_ptr<d3d12::DefaultBuffer> cube;
    std::unique_ptr<d3d12::RasterShader> shader;
};

int main() {
    auto app = std::make_unique<App>();
    app->Init();
    app->Start();
    app->Run();
    return 0;
}
