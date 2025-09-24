#include <thread>
#include <mutex>

#include <radray/logger.h>
#include <radray/stopwatch.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/window/native_window.h>

#include "../../modules/render/src/d3d12/d3d12_impl.h"

using namespace radray;
using namespace radray::render;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int RT_COUNT = 3;
const char* RADRAY_APPNAME = "hello_world_d3d12";
const char* SHADER_SRC = R"(
const static float3 g_Color[3] = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1)};

struct V2P {
    float4 pos : SV_POSITION;
    [[vk::location(0)]] float3 color: COLOR0;
};

V2P VSMain([[vk::location(0)]] float3 v_vert: POSITION, uint vertId: SV_VertexID) {
    V2P v2p;
    v2p.pos = float4(v_vert, 1);
    v2p.color = g_Color[vertId % 3];
    return v2p;
}

float4 PSMain(V2P v2p) : SV_Target {
    return float4(v2p.color, 1);
})";

struct FrameData {
    shared_ptr<d3d12::CmdListD3D12> cmdBuffer;
};

unique_ptr<NativeWindow> window;
shared_ptr<d3d12::DeviceD3D12> device;
d3d12::CmdQueueD3D12* cmdQueue;
shared_ptr<d3d12::SwapChainD3D12> swapchain;
vector<FrameData> frames;
unordered_map<Texture*, shared_ptr<d3d12::TextureViewD3D12>> rtViews;
uint32_t currentFrameIndex = 0;
ColorClearValue clear{0.0f, 0.0f, 0.0f, 1.0f};
int clearIndex = 0;
Stopwatch sw;
uint64_t last;
shared_ptr<d3d12::BufferD3D12> vertBuf;
shared_ptr<d3d12::BufferD3D12> idxBuf;
shared_ptr<Dxc> dxc;
shared_ptr<d3d12::RootSigD3D12> rootSig;
shared_ptr<d3d12::GraphicsPsoD3D12> pso;
Eigen::Vector2i winSize{WIN_WIDTH, WIN_HEIGHT};
sigslot::connection resizedConn;
sigslot::connection resizingConn;
std::mutex mtx;
std::optional<Eigen::Vector2i> resizeVal;
bool isResizing = false;

void Init() {
    Win32WindowCreateDescriptor windowDesc{
        RADRAY_APPNAME,
        winSize.x(),
        winSize.y(),
        -1,
        -1,
        true,
        false,
        false,
        {}};
    window = CreateNativeWindow(windowDesc).Unwrap();
    device = std::static_pointer_cast<d3d12::DeviceD3D12>(CreateDevice(D3D12DeviceDescriptor{std::nullopt, true, false}).Unwrap());
    cmdQueue = static_cast<d3d12::CmdQueueD3D12*>(device->GetCommandQueue(QueueType::Direct, 0).Unwrap());
    swapchain = std::static_pointer_cast<d3d12::SwapChainD3D12>(device->CreateSwapChain({cmdQueue, window->GetNativeHandler().Handle, (uint32_t)winSize.x(), (uint32_t)winSize.y(), RT_COUNT, TextureFormat::RGBA8_UNORM, false}).Unwrap());
    frames.reserve(swapchain->_frames.size());
    for (size_t i = 0; i < swapchain->_frames.size(); ++i) {
        auto& f = frames.emplace_back();
        f.cmdBuffer = std::static_pointer_cast<d3d12::CmdListD3D12>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    }
    currentFrameIndex = 0;
    sw.Reset();
    sw.Start();
    last = 0;

    TriangleMesh mesh;
    mesh.Positions = {{0, 0.5f, 0}, {-0.5f, -0.366f, 0}, {0.5f, -0.366f, 0}};
    mesh.Indices = {0, 1, 2};
    VertexData model;
    mesh.ToVertexData(&model);
    auto vertUpload = device->CreateBuffer({model.VertexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, {}}).Unwrap();
    auto vert = device->CreateBuffer({model.VertexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Vertex, ResourceHint::None, {}}).Unwrap();
    auto idxUpload = device->CreateBuffer({model.IndexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, {}}).Unwrap();
    auto idx = device->CreateBuffer({model.IndexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Index, ResourceHint::None, {}}).Unwrap();
    vertBuf = std::static_pointer_cast<d3d12::BufferD3D12>(vert);
    idxBuf = std::static_pointer_cast<d3d12::BufferD3D12>(idx);
    vertUpload->CopyFromHost({model.VertexData.get(), model.VertexSize}, 0);
    idxUpload->CopyFromHost({model.IndexData.get(), model.IndexSize}, 0);

    auto cmdBuffer = std::static_pointer_cast<d3d12::CmdListD3D12>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    cmdBuffer->Begin();
    {
        BarrierBufferDescriptor barriers[] = {
            {vert.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false},
            {idx.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false}};
        cmdBuffer->ResourceBarrier(barriers, {});
    }
    cmdBuffer->CopyBufferToBuffer(vert.get(), 0, vertUpload.get(), 0, model.VertexSize);
    cmdBuffer->CopyBufferToBuffer(idx.get(), 0, idxUpload.get(), 0, model.IndexSize);
    {
        BarrierBufferDescriptor barriers[] = {
            {vert.get(), BufferUse::CopyDestination, BufferUse::Vertex, nullptr, false},
            {idx.get(), BufferUse::CopyDestination, BufferUse::Index, nullptr, false}};
        cmdBuffer->ResourceBarrier(barriers, {});
    }
    cmdBuffer->End();
    CommandBuffer* submitCmdBuffers[] = {cmdBuffer.get()};
    cmdQueue->Submit({submitCmdBuffers, {}, {}, {}, {}});
    cmdQueue->Wait();

    dxc = CreateDxc().Unwrap();
    {
        auto vsSpv = dxc->Compile(SHADER_SRC, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, false, {}, {}, false).value();
        auto psSpv = dxc->Compile(SHADER_SRC, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, false, {}, {}, false).value();
        ShaderDescriptor vsDesc{};
        vsDesc.Source = vsSpv.Data;
        vsDesc.Category = vsSpv.Category;
        ShaderDescriptor psDesc{};
        psDesc.Source = psSpv.Data;
        psDesc.Category = psSpv.Category;
        auto vs = device->CreateShader(vsDesc).Unwrap();
        auto ps = device->CreateShader(psDesc).Unwrap();

        RootSignatureDescriptor rootSigDesc{};
        rootSig = std::static_pointer_cast<d3d12::RootSigD3D12>(device->CreateRootSignature(rootSigDesc).Unwrap());

        VertexElement ve[] = {
            {0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
        VertexInfo vl[] = {
            {12,
             VertexStepMode::Vertex,
             ve}};
        ColorTargetState cts[] = {
            DefaultColorTargetState(TextureFormat::RGBA8_UNORM)};
        GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = rootSig.get();
        psoDesc.VS = {vs.get(), "VSMain"};
        psoDesc.PS = {ps.get(), "PSMain"};
        psoDesc.VertexLayouts = vl;
        psoDesc.Primitive = DefaultPrimitiveState();
        psoDesc.Primitive.FaceClockwise = FrontFace::CCW;
        psoDesc.DepthStencil = std::nullopt;
        psoDesc.MultiSample = DefaultMultiSampleState();
        psoDesc.ColorTargets = cts;
        pso = std::static_pointer_cast<d3d12::GraphicsPsoD3D12>(device->CreateGraphicsPipelineState(psoDesc).Unwrap());
    }
}

void Update() {
    uint64_t now = sw.RunningMilliseconds();
    auto delta = now - last;
    last = now;

    float* v = nullptr;
    if (clearIndex >= 0 && clearIndex < 2) {
        v = &clear.R;
    } else if (clearIndex >= 2 && clearIndex < 4) {
        v = &clear.G;
    } else if (clearIndex >= 4 && clearIndex < 6) {
        v = &clear.B;
    }
    if (clearIndex % 2 == 0) {
        *v += delta / 1000.0f;
        if (*v >= 1.0f) {
            *v = 1.0f;
            clearIndex++;
        }
    } else {
        *v -= delta / 1000.0f;
        if (*v <= 0.0f) {
            *v = 0.0f;
            clearIndex++;
            if (clearIndex >= 5) clearIndex = 0;
        }
    }

    bool mresize = false;
    std::optional<Eigen::Vector2i> mr;
    {
        std::lock_guard<std::mutex> lock{mtx};
        if (isResizing || resizeVal.has_value()) {
            mresize = true;
        }
        mr = resizeVal;
        resizeVal = std::nullopt;
    }
    if (mresize) {
        if (mr.has_value()) {
            Eigen::Vector2i sz = mr.value();
            if (sz.x() == 0 || sz.y() == 0) {
                return;
            }
            cmdQueue->Wait();
            rtViews.clear();
            swapchain->Destroy();
            swapchain = std::static_pointer_cast<d3d12::SwapChainD3D12>(
                device->CreateSwapChain(
                          {cmdQueue,
                           window->GetNativeHandler().Handle,
                           (uint32_t)sz.x(),
                           (uint32_t)sz.y(),
                           RT_COUNT,
                           TextureFormat::RGBA8_UNORM,
                           false})
                    .Unwrap());
            winSize = sz;
        }
        return;
    }

    auto& frameData = frames[currentFrameIndex];
    swapchain->AcquireNext();
    auto rt = swapchain->GetCurrentBackBuffer().Unwrap();
    shared_ptr<d3d12::TextureViewD3D12> rtView = nullptr;
    {
        auto it = rtViews.find(rt);
        if (it == rtViews.end()) {
            TextureViewDescriptor rtViewDesc{};
            rtViewDesc.Target = rt;
            rtViewDesc.Dim = TextureViewDimension::Dim2D;
            rtViewDesc.Format = TextureFormat::RGBA8_UNORM;
            rtViewDesc.BaseMipLevel = 0;
            rtViewDesc.MipLevelCount = std::nullopt;
            rtViewDesc.BaseArrayLayer = 0;
            rtViewDesc.ArrayLayerCount = std::nullopt;
            rtViewDesc.Usage = TextureUse::RenderTarget;
            it = rtViews.emplace(rt, std::static_pointer_cast<d3d12::TextureViewD3D12>(device->CreateTextureView(rtViewDesc).Unwrap())).first;
        }
        rtView = it->second;
    }
    frameData.cmdBuffer->Begin();
    {
        BarrierTextureDescriptor texDesc[] = {{rt, TextureUse::Uninitialized, TextureUse::RenderTarget, {}, false, 0, 0, 0, 0, 0}};
        frameData.cmdBuffer->ResourceBarrier({}, texDesc);
    }
    {
        ColorAttachment rpColorAttch[] = {{rtView.get(), LoadAction::Clear, StoreAction::Store, clear}};
        RenderPassDescriptor rpDesc{};
        rpDesc.ColorAttachments = rpColorAttch;
        rpDesc.DepthStencilAttachment = std::nullopt;
        auto rp = frameData.cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        rp->BindRootSignature(rootSig.get());
        rp->BindGraphicsPipelineState(pso.get());
        rp->SetViewport({0, 0, (float)winSize.x(), (float)winSize.y(), 0.0f, 1.0f});
        rp->SetScissor({0, 0, (uint32_t)winSize.x(), (uint32_t)winSize.y()});
        VertexBufferView vbv[] = {
            {vertBuf.get(), 0, vertBuf->_desc.Size}};
        rp->BindVertexBuffer(vbv);
        rp->BindIndexBuffer({idxBuf.get(), 0, 2});
        rp->DrawIndexed(3, 1, 0, 0, 0);
        frameData.cmdBuffer->EndRenderPass(std::move(rp));
    }
    {
        BarrierTextureDescriptor texDesc[] = {{rt, TextureUse::RenderTarget, TextureUse::Present, {}, false, 0, 0, 0, 0, 0}};
        frameData.cmdBuffer->ResourceBarrier({}, texDesc);
    }
    frameData.cmdBuffer->End();
    CommandBuffer* submitCmdBuffer[] = {frameData.cmdBuffer.get()};
    CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = submitCmdBuffer;
    cmdQueue->Submit(submitDesc);
    swapchain->Present();
    currentFrameIndex = (currentFrameIndex + 1) % frames.size();
}

void End() {
    cmdQueue->Wait();

    pso.reset();
    rootSig.reset();

    vertBuf.reset();
    idxBuf.reset();

    rtViews.clear();
    frames.clear();
    swapchain = nullptr;
    cmdQueue = nullptr;
    device.reset();
    resizedConn.disconnect();
    resizingConn.disconnect();
    window.reset();
}

int main() {
    Init();
    resizedConn = window->EventResized().connect([](int width, int height) {
        std::lock_guard<std::mutex> lock{mtx};
        isResizing = false;
        resizeVal = Eigen::Vector2i(width, height);
    });
    resizingConn = window->EventResizing().connect([](int width, int height) {
        std::lock_guard<std::mutex> lock{mtx};
        isResizing = true;
        resizeVal = Eigen::Vector2i(width, height);
    });
    std::thread renderThread{[]() {
        while (true) {
            Update();
            if (window->ShouldClose()) {
                break;
            }
            std::this_thread::yield();
        }
    }};
    while (true) {
        window->DispatchEvents();
        if (window->ShouldClose()) {
            break;
        }
        std::this_thread::yield();
    }
    renderThread.join();
    End();
    return 0;
}
