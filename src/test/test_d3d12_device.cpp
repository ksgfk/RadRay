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

const float c[] = {
    1, 1, 1,
    2, 2, 2,
    3, 3, 3};

const char* vs = R"(
float3 ToSRGB(float3 value) {
  return select(value <= 0.0031308f, value * 12.92f, (1.0f + 0.055f) * pow(value, 1.0f / 2.4f) - 0.055f);
}

float3 ToLinear(float3 value) {
  return select(value <= 0.04045f, value * (1.0f / 12.92f), pow((value + 0.055f) * (1.0f / 1.055f), 2.4f));
}

cbuffer cbPreObject : register(b0) {
  float4x4 g_Model;
  float4x4 g_MVP;
  float4x4 g_InvM;
};

cbuffer cbGlobal : register(b1) {
  float4x4 g_View;
  float4x4 g_Proj;
  float4x4 g_VP;
};
struct Test {
  float4 a;
};
ConstantBuffer<Test> cbTest[4] : register(b2);

Texture2D<float4> g_Color : register(t0);
SamplerState g_Sampler : register(s1);

RWStructuredBuffer<Test> textures[2] : register(u1);
StructuredBuffer<Test> buffers[2] : register(t1);
ByteAddressBuffer bytes : register(t3);

struct VSIn {
  float3 Pos : POSITION;
  float2 UV0 : TEXCOORD0;
};

struct VSOut {
  float4 PosClip : SV_POSITION;
  float2 UV : TEXCOORD0;
};

VSOut VSMain(VSIn vin) {
  VSOut v;
  v.PosClip = mul(g_MVP, float4(vin.Pos, 1.0f));
  v.UV = vin.UV0;
  float4 color = cbTest[0].a;
  v.PosClip *= color;
  return v;
}

float4 PSMain(VSOut vout) : SV_Target {
  float4 color = g_Color.Sample(g_Sampler, vout.UV);
  RWStructuredBuffer<Test> a = textures[0];
  a[0].a = color;
  color /= cbTest[0].a;
  color += buffers[0][0].a;
  uint4 t = bytes.Load4(0);
  color /= t;
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
        d3d12::RasterShaderCompileResult shaderResult = device->shaderCompiler->CompileRaster(vs, 61, false);
        if (!shaderResult.vs.data) {
            RADRAY_ABORT("compile vs error\n{}", shaderResult.vs.error);
        }
        if (!shaderResult.ps.data) {
            RADRAY_ABORT("compile ps error\n{}", shaderResult.ps.error);
        }
        d3d12::RasterShader raster{device.get()};
        raster.Setup(&shaderResult);
        directQueue = std::make_unique<d3d12::CommandQueue>(device.get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        for (int i = 0; i < 3; i++) {
            cmdAllocs[i] = std::make_unique<d3d12::CommandAllocator>(device.get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        }
        swapChain = std::make_unique<d3d12::SwapChain>(device.get(), directQueue.get(), (HWND)window->GetNativeHandle(), 3, 1280, 720, false);
    }

    void Start() {
        d3d12::CommandAllocator* cmdAlloc = cmdAllocs[0].get();
        d3d12::CommandList* cmdList = cmdAlloc->cmd.get();
        d3d12::ResourceStateTracker* stateTracker = &cmdAlloc->stateTracker;
        ID3D12GraphicsCommandList* cmd = cmdList->cmd.Get();
        cmdAlloc->Reset();
        cube = std::make_unique<d3d12::DefaultBuffer>(device.get(), ArrayByteSize(c));
        stateTracker->Track(cube.get(), D3D12_RESOURCE_STATE_COPY_DEST);
        stateTracker->Update(cmd);
        cmdList->Upload(cube->GetResource(), 0, {reinterpret_cast<const uint8*>(c), ArrayByteSize(c)});
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
};

int main() {
    auto app = std::make_unique<App>();
    app->Init();
    app->Start();
    app->Run();
    return 0;
}
