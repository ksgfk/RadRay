#include <thread>
#include <mutex>

#include <radray/logger.h>
#include <radray/stopwatch.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/window/native_window.h>
#include <radray/render/backend/vulkan_impl.h>

using namespace radray;
using namespace radray::render;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int RT_COUNT = 3;
const char* RADRAY_APPNAME = "hello_world_vk";
const char* SHADER_SRC = R"(const static float3 g_Color[3] = {
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
}
)";

struct FrameData {
    unique_ptr<vulkan::CommandBufferVulkan> cmdBuffer;
};

unique_ptr<NativeWindow> window;
unique_ptr<InstanceVulkan> vkInstance;
shared_ptr<vulkan::DeviceVulkan> device;
vulkan::QueueVulkan* cmdQueue = nullptr;
unique_ptr<vulkan::SwapChainVulkan> swapchain;
vector<FrameData> frames;
unordered_map<Texture*, unique_ptr<vulkan::ImageViewVulkan>> rtViews;
uint32_t currentFrameIndex = 0;
ColorClearValue clear{0.0f, 0.0f, 0.0f, 1.0f};
int clearIndex = 0;
Stopwatch sw;
uint64_t last;
unique_ptr<vulkan::BufferVulkan> vertBuf;
unique_ptr<vulkan::BufferVulkan> idxBuf;
shared_ptr<Dxc> dxc;
unique_ptr<vulkan::PipelineLayoutVulkan> pipelineLayout;
unique_ptr<vulkan::GraphicsPipelineVulkan> pso;
Eigen::Vector2i winSize{WIN_WIDTH, WIN_HEIGHT};
sigslot::connection resizedConn;
sigslot::connection resizingConn;
std::mutex mtx;
std::optional<Eigen::Vector2i> resizeVal;
bool isResizing = false;
bool isMinimized = false;

void Init() {
#ifdef RADRAY_PLATFORM_WINDOWS
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
#endif
    if (!window) {
        throw std::runtime_error("Failed to create native window");
        return;
    }
    VulkanInstanceDescriptor vkInsDesc{};
    vkInsDesc.AppName = RADRAY_APPNAME;
    vkInsDesc.AppVersion = 1;
    vkInsDesc.EngineName = "RadRay";
    vkInsDesc.EngineVersion = 1;
    vkInsDesc.IsEnableDebugLayer = true;
    vkInstance = CreateVulkanInstance(vkInsDesc).Unwrap();
    VulkanDeviceDescriptor deviceDesc{};
    VulkanCommandQueueDescriptor queueDesc[] = {
        {QueueType::Direct, 1}};
    deviceDesc.Queues = queueDesc;
    device = std::static_pointer_cast<vulkan::DeviceVulkan>(CreateDevice(deviceDesc).Unwrap());
    cmdQueue = static_cast<vulkan::QueueVulkan*>(device->GetCommandQueue(QueueType::Direct, 0).Unwrap());
    SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = cmdQueue;
    swapchainDesc.NativeHandler = window->GetNativeHandler().Handle;
    swapchainDesc.Width = (uint32_t)winSize.x();
    swapchainDesc.Height = (uint32_t)winSize.y();
    swapchainDesc.BackBufferCount = RT_COUNT;
    swapchainDesc.Format = TextureFormat::RGBA8_UNORM;
    swapchainDesc.EnableSync = false;
    swapchain = StaticCastUniquePtr<vulkan::SwapChainVulkan>(device->CreateSwapChain(swapchainDesc).Unwrap());
    frames.reserve(swapchain->_frames.size());
    for (size_t i = 0; i < swapchain->_frames.size(); ++i) {
        auto& f = frames.emplace_back();
        f.cmdBuffer = StaticCastUniquePtr<vulkan::CommandBufferVulkan>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    }
    currentFrameIndex = 0;
    sw.Reset();
    sw.Start();
    last = 0;

    float pos[] = {0, 0.5f, 0, -0.5f, -0.366f, 0, 0.5f, -0.366f, 0};
    uint16_t i[] = {0, 1, 2};
    const size_t vertexSize = sizeof(pos);
    const size_t indexSize = sizeof(i);
    auto vertUpload = device->CreateBuffer({vertexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, {}}).Unwrap();
    auto idxUpload = device->CreateBuffer({indexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, {}}).Unwrap();
    {
        auto vert = device->CreateBuffer({vertexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Vertex, ResourceHint::None, {}}).Unwrap();
        auto idx = device->CreateBuffer({indexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Index, ResourceHint::None, {}}).Unwrap();
        vertBuf = StaticCastUniquePtr<vulkan::BufferVulkan>(std::move(vert));
        idxBuf = StaticCastUniquePtr<vulkan::BufferVulkan>(std::move(idx));
    }
    {
        void* vertMap = vertUpload->Map(0, vertexSize);
        std::memcpy(vertMap, pos, vertexSize);
        vertUpload->Unmap(0, vertexSize);
    }
    {
        void* idxMap = idxUpload->Map(0, indexSize);
        std::memcpy(idxMap, i, indexSize);
        idxUpload->Unmap(0, indexSize);
    }

    auto cmdBuffer = StaticCastUniquePtr<vulkan::CommandBufferVulkan>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    cmdBuffer->Begin();
    {
        BarrierBufferDescriptor barriers[] = {
            {vertBuf.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false},
            {idxBuf.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false}};
        cmdBuffer->ResourceBarrier(barriers, {});
    }
    cmdBuffer->CopyBufferToBuffer(vertBuf.get(), 0, vertUpload.get(), 0, vertexSize);
    cmdBuffer->CopyBufferToBuffer(idxBuf.get(), 0, idxUpload.get(), 0, indexSize);
    {
        BarrierBufferDescriptor barriers[] = {
            {vertBuf.get(), BufferUse::CopyDestination, BufferUse::Vertex, nullptr, false},
            {idxBuf.get(), BufferUse::CopyDestination, BufferUse::Index, nullptr, false}};
        cmdBuffer->ResourceBarrier(barriers, {});
    }
    cmdBuffer->End();
    CommandBuffer* submitCmdBuffers[] = {cmdBuffer.get()};
    cmdQueue->Submit({submitCmdBuffers, {}, {}, {}, {}});
    cmdQueue->Wait();

    dxc = CreateDxc().Unwrap();
    {
        auto hlslStr = SHADER_SRC;
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
        pipelineLayout = StaticCastUniquePtr<vulkan::PipelineLayoutVulkan>(device->CreateRootSignature(rootSigDesc).Unwrap());

        VertexElement ve[] = {
            {0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
        VertexBufferLayout vl[] = {
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
        pso = StaticCastUniquePtr<vulkan::GraphicsPipelineVulkan>(device->CreateGraphicsPipelineState(psoDesc).Unwrap());
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
    DestroyVulkanInstance(std::move(vkInstance));
    resizedConn.disconnect();
    resizingConn.disconnect();
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

    {
        bool mresize = false;
        std::optional<Eigen::Vector2i> mr;
        std::lock_guard<std::mutex> lock{mtx};
        if (isResizing || resizeVal.has_value()) {
            mresize = true;
        }
        mr = resizeVal;
        resizeVal = std::nullopt;

        if (mresize) {
            if (mr.has_value()) {
                winSize = mr.value();
                if (winSize.x() == 0 || winSize.y() == 0) {
                    isMinimized = true;
                    return;
                }
                isMinimized = false;
                cmdQueue->Wait();
                rtViews.clear();
                swapchain->Destroy();
                swapchain = StaticCastUniquePtr<vulkan::SwapChainVulkan>(
                    device->CreateSwapChain(
                              {cmdQueue,
                               window->GetNativeHandler().Handle,
                               (uint32_t)winSize.x(),
                               (uint32_t)winSize.y(),
                               RT_COUNT,
                               TextureFormat::RGBA8_UNORM,
                               false})
                        .Unwrap());
            }
            return;
        }

        if (isMinimized) {
            return;
        }
    }

    auto& frameData = frames[currentFrameIndex];
    auto acqRes = swapchain->AcquireNext();
    if (acqRes == nullptr) {
        return;
    }
    auto rt = swapchain->GetCurrentBackBuffer().Unwrap();
    vulkan::ImageViewVulkan* rtView = nullptr;
    {
        auto it = rtViews.find(rt);
        if (it == rtViews.end()) {
            TextureViewDescriptor rtViewDesc{};
            rtViewDesc.Target = rt;
            rtViewDesc.Dim = TextureViewDimension::Dim2D;
            rtViewDesc.Format = TextureFormat::RGBA8_UNORM;
            rtViewDesc.Range.BaseMipLevel = 0;
            rtViewDesc.Range.MipLevelCount = SubresourceRange::All;
            rtViewDesc.Range.BaseArrayLayer = 0;
            rtViewDesc.Range.ArrayLayerCount = SubresourceRange::All;
            it = rtViews.emplace(rt, StaticCastUniquePtr<vulkan::ImageViewVulkan>(device->CreateTextureView(rtViewDesc).Unwrap())).first;
        }
        rtView = it->second.get();
    }
    frameData.cmdBuffer->Begin();
    {
        BarrierTextureDescriptor texDesc[] = {{rt, TextureUse::Uninitialized, TextureUse::RenderTarget, {}, false, false, {}}};
        frameData.cmdBuffer->ResourceBarrier({}, texDesc);
    }
    {
        ColorAttachment rpColorAttch[] = {{rtView, LoadAction::Clear, StoreAction::Store, clear}};
        RenderPassDescriptor rpDesc{};
        rpDesc.ColorAttachments = rpColorAttch;
        rpDesc.DepthStencilAttachment = std::nullopt;
        auto rp = frameData.cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        rp->BindRootSignature(pipelineLayout.get());
        rp->BindGraphicsPipelineState(pso.get());
        rp->SetViewport({0, 0, (float)winSize.x(), (float)winSize.y(), 0.0f, 1.0f});
        rp->SetScissor({0, 0, (uint32_t)winSize.x(), (uint32_t)winSize.y()});
        VertexBufferView vbv[] = {
            {vertBuf.get(), 0, vertBuf->_mdesc.Size}};
        rp->BindVertexBuffer(vbv);
        rp->BindIndexBuffer({idxBuf.get(), 0, 2});
        rp->DrawIndexed(3, 1, 0, 0, 0);
        frameData.cmdBuffer->EndRenderPass(std::move(rp));
    }
    {
        BarrierTextureDescriptor texDesc[] = {{rt, TextureUse::RenderTarget, TextureUse::Present, {}, false, false, {}}};
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
