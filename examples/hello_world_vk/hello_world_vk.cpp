#include <thread>

#include <radray/logger.h>
#include <radray/stopwatch.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/window/native_window.h>

#include "../../modules/render/src/vk/vulkan_impl.h"

using namespace radray;
using namespace radray::render;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int RT_COUNT = 3;
const char* RADRAY_APPNAME = "hello_world_vk";

struct FrameData {
    shared_ptr<vulkan::CommandBufferVulkan> cmdBuffer;
};

unique_ptr<NativeWindow> window;
shared_ptr<vulkan::DeviceVulkan> device;
vulkan::QueueVulkan* cmdQueue = nullptr;
shared_ptr<vulkan::SwapChainVulkan> swapchain;
vector<FrameData> frames;
unordered_map<Texture*, shared_ptr<vulkan::ImageViewVulkan>> rtViews;
uint32_t currentFrameIndex = 0;
ColorClearValue clear{0.0f, 0.0f, 0.0f, 1.0f};
int clearIndex = 0;
Stopwatch sw;
uint64_t last;
shared_ptr<vulkan::BufferVulkan> vertBuf;
shared_ptr<vulkan::BufferVulkan> idxBuf;
shared_ptr<Dxc> dxc;
shared_ptr<vulkan::PipelineLayoutVulkan> pipelineLayout;
shared_ptr<vulkan::GraphicsPipelineVulkan> pso;

void Init() {
#ifdef RADRAY_PLATFORM_WINDOWS
    Win32WindowCreateDescriptor windowDesc{
        RADRAY_APPNAME,
        WIN_WIDTH,
        WIN_HEIGHT,
        -1,
        -1,
        true,
        false,
        false};
    window = CreateNativeWindow(windowDesc).Unwrap();
#endif
    if (!window) {
        RADRAY_ABORT("no window");
        return;
    }
    VulkanBackendInitDescriptor backendDesc{};
    backendDesc.IsEnableDebugLayer = true;
    vulkan::GlobalInitVulkan(backendDesc);
    VulkanDeviceDescriptor deviceDesc{};
    VulkanCommandQueueDescriptor queueDesc[] = {
        {QueueType::Direct, 1}};
    deviceDesc.Queues = queueDesc;
    device = std::static_pointer_cast<vulkan::DeviceVulkan>(CreateDevice(deviceDesc).Unwrap());
    cmdQueue = static_cast<vulkan::QueueVulkan*>(device->GetCommandQueue(QueueType::Direct, 0).Unwrap());
    SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = cmdQueue;
    swapchainDesc.NativeHandler = window->GetNativeHandler().Handle;
    swapchainDesc.Width = WIN_WIDTH;
    swapchainDesc.Height = WIN_HEIGHT;
    swapchainDesc.BackBufferCount = RT_COUNT;
    swapchainDesc.Format = TextureFormat::RGBA8_UNORM;
    swapchainDesc.EnableSync = false;
    swapchain = std::static_pointer_cast<vulkan::SwapChainVulkan>(device->CreateSwapChain(swapchainDesc).Unwrap());
    frames.reserve(swapchain->_frames.size());
    for (size_t i = 0; i < swapchain->_frames.size(); ++i) {
        auto& f = frames.emplace_back();
        f.cmdBuffer = std::static_pointer_cast<vulkan::CommandBufferVulkan>(device->CreateCommandBuffer(cmdQueue).Unwrap());
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
    vertBuf = std::static_pointer_cast<vulkan::BufferVulkan>(vert);
    idxBuf = std::static_pointer_cast<vulkan::BufferVulkan>(idx);
    vertUpload->CopyFromHost({model.VertexData.get(), model.VertexSize}, 0);
    idxUpload->CopyFromHost({model.IndexData.get(), model.IndexSize}, 0);

    auto cmdBuffer = std::static_pointer_cast<vulkan::CommandBufferVulkan>(device->CreateCommandBuffer(cmdQueue).Unwrap());
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
        auto hlslStr = ReadText(std::filesystem::path("assets") / RADRAY_APPNAME / "color.hlsl").value();
        auto vsSpv = dxc->Compile(hlslStr, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, false, {}, {}, true).value();
        auto psSpv = dxc->Compile(hlslStr, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, false, {}, {}, true).value();
        ShaderDescriptor vsDesc{};
        vsDesc.Source = vsSpv.Data;
        vsDesc.Category = vsSpv.Category;
        ShaderDescriptor psDesc{};
        psDesc.Source = psSpv.Data;
        psDesc.Category = psSpv.Category;
        auto vs = device->CreateShader(vsDesc).Unwrap();
        auto ps = device->CreateShader(psDesc).Unwrap();

        RootSignatureDescriptor rootSigDesc{};
        pipelineLayout = std::static_pointer_cast<vulkan::PipelineLayoutVulkan>(device->CreateRootSignature(rootSigDesc).Unwrap());

        VertexElement ve[] = {
            {0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
        VertexInfo vl[] = {
            {12,
             VertexStepMode::Vertex,
             ve}};
        ColorTargetState cts[] = {
            DefaultColorTargetState(TextureFormat::RGBA8_UNORM)};
        GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = pipelineLayout.get();
        psoDesc.VS = {vs.get(), "VSMain"};
        psoDesc.PS = {ps.get(), "PSMain"};
        psoDesc.VertexLayouts = vl;
        psoDesc.Primitive = DefaultPrimitiveState();
        psoDesc.DepthStencil = std::nullopt;
        psoDesc.MultiSample = DefaultMultiSampleState();
        psoDesc.ColorTargets = cts;
        pso = std::static_pointer_cast<vulkan::GraphicsPipelineVulkan>(device->CreateGraphicsPipelineState(psoDesc).Unwrap());
    }
}

void End() {
    cmdQueue->Wait();

    pso.reset();
    pipelineLayout.reset();

    vertBuf.reset();
    idxBuf.reset();

    rtViews.clear();
    frames.clear();
    swapchain = nullptr;
    cmdQueue = nullptr;
    device = nullptr;
    vulkan::GlobalTerminateVulkan();
    window = nullptr;
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

    auto& frameData = frames[currentFrameIndex];
    swapchain->AcquireNext();
    auto rt = swapchain->GetCurrentBackBuffer().Unwrap();
    shared_ptr<vulkan::ImageViewVulkan> rtView = nullptr;
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
            it = rtViews.emplace(rt, std::static_pointer_cast<vulkan::ImageViewVulkan>(device->CreateTextureView(rtViewDesc).Unwrap())).first;
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
        rp->BindRootSignature(pipelineLayout.get());
        rp->BindGraphicsPipelineState(pso.get());
        rp->SetViewport({0, 0, WIN_WIDTH, WIN_HEIGHT, 0.0f, 1.0f});
        rp->SetScissor({0, 0, WIN_WIDTH, WIN_HEIGHT});
        VertexBufferView vbv[] = {
            {vertBuf.get(), 0, vertBuf->_mdesc.Size}};
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

int main() {
    Init();
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
