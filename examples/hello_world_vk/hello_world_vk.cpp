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
constexpr int BACK_BUFFER_COUNT = 3;
constexpr int INFLIGHT_FRAME_COUNT = 2;
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

class Frame {
public:
    unique_ptr<vulkan::CommandBufferVulkan> cmdBuffer;
    unique_ptr<vulkan::FenceVulkan> execFence;
    unique_ptr<vulkan::SemaphoreVulkan> imageAvailable;
    uint32_t backBufferIndex = std::numeric_limits<uint32_t>::max();
};

shared_ptr<Dxc> dxc;
Eigen::Vector2i winSize;
unique_ptr<NativeWindow> window;
unique_ptr<InstanceVulkan> vkInstance;
shared_ptr<vulkan::DeviceVulkan> device;
vulkan::QueueVulkan* cmdQueue;
unique_ptr<vulkan::SwapChainVulkan> swapchain;
vector<Frame> frames;
vector<unique_ptr<vulkan::ImageViewVulkan>> rtViews;
vector<unique_ptr<vulkan::SemaphoreVulkan>> backBufRenderFinished;
unique_ptr<vulkan::PipelineLayoutVulkan> pipelineLayout;
unique_ptr<vulkan::GraphicsPipelineVulkan> pso;
unique_ptr<vulkan::BufferVulkan> vertBuf;
unique_ptr<vulkan::BufferVulkan> idxBuf;
sigslot::connection resizedConn;

void CreateSwapChain() {
    SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = cmdQueue;
    swapchainDesc.NativeHandler = window->GetNativeHandler().Handle;
    swapchainDesc.Width = (uint32_t)winSize.x();
    swapchainDesc.Height = (uint32_t)winSize.y();
    swapchainDesc.BackBufferCount = BACK_BUFFER_COUNT;
    swapchainDesc.FlightFrameCount = INFLIGHT_FRAME_COUNT;
    swapchainDesc.Format = TextureFormat::RGBA8_UNORM;
    swapchainDesc.EnableSync = false;
    swapchain = StaticCastUniquePtr<vulkan::SwapChainVulkan>(device->CreateSwapChain(swapchainDesc).Unwrap());
    if (backBufRenderFinished.size() != swapchainDesc.BackBufferCount) {
        backBufRenderFinished.clear();
        backBufRenderFinished.reserve(BACK_BUFFER_COUNT);
        for (size_t i = 0; i < swapchain->_frames.size(); i++) {
            auto sem = StaticCastUniquePtr<vulkan::SemaphoreVulkan>(device->CreateSemaphoreDevice().Unwrap());
            backBufRenderFinished.emplace_back(std::move(sem));
        }
    }
}

void OnResized(int width, int height) {
    winSize = {width, height};
    if (swapchain && width > 0 && height > 0) {
        for (auto& i : frames) {
            i.execFence->Wait();
        }
        cmdQueue->Wait();
        for (auto& i : rtViews) {
            i.reset();
        }
        swapchain.reset();
        CreateSwapChain();
    }
}

void Init() {
    dxc = CreateDxc().Unwrap();
    winSize = {WIN_WIDTH, WIN_HEIGHT};
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
    }
    VulkanInstanceDescriptor vkInsDesc{};
    vkInsDesc.AppName = RADRAY_APPNAME;
    vkInsDesc.AppVersion = 1;
    vkInsDesc.EngineName = "RadRay";
    vkInsDesc.EngineVersion = 1;
    vkInsDesc.IsEnableDebugLayer = true;
    vkInsDesc.IsEnableGpuBasedValid = true;
    vkInstance = CreateVulkanInstance(vkInsDesc).Unwrap();
    VulkanDeviceDescriptor deviceDesc{};
    VulkanCommandQueueDescriptor queueDesc[] = {
        {QueueType::Direct, 1}};
    deviceDesc.Queues = queueDesc;
    device = std::static_pointer_cast<vulkan::DeviceVulkan>(CreateDevice(deviceDesc).Unwrap());
    cmdQueue = static_cast<vulkan::QueueVulkan*>(device->GetCommandQueue(QueueType::Direct, 0).Unwrap());
    CreateSwapChain();
    rtViews.reserve(BACK_BUFFER_COUNT);
    for (size_t i = 0; i < BACK_BUFFER_COUNT; i++) {
        rtViews.emplace_back();
    }
    frames.reserve(INFLIGHT_FRAME_COUNT);
    for (size_t i = 0; i < INFLIGHT_FRAME_COUNT; i++) {
        auto& f = frames.emplace_back();
        f.cmdBuffer = StaticCastUniquePtr<vulkan::CommandBufferVulkan>(device->CreateCommandBuffer(cmdQueue).Unwrap());
        f.execFence = StaticCastUniquePtr<vulkan::FenceVulkan>(device->CreateFence().Unwrap());
        f.imageAvailable = StaticCastUniquePtr<vulkan::SemaphoreVulkan>(device->CreateSemaphoreDevice().Unwrap());
    }
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
        VertexBufferLayout vl{12, VertexStepMode::Vertex, ve};
        ColorTargetState cts = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
        GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = pipelineLayout.get();
        psoDesc.VS = {vs.get(), "VSMain"};
        psoDesc.PS = {ps.get(), "PSMain"};
        psoDesc.VertexLayouts = std::span{&vl, 1};
        psoDesc.Primitive = PrimitiveState::Default();
        psoDesc.DepthStencil = std::nullopt;
        psoDesc.MultiSample = MultiSampleState::Default();
        psoDesc.ColorTargets = std::span{&cts, 1};
        pso = StaticCastUniquePtr<vulkan::GraphicsPipelineVulkan>(device->CreateGraphicsPipelineState(psoDesc).Unwrap());
    }
    {
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
        cmdQueue->Submit({submitCmdBuffers, {}, {}, {}});
        cmdQueue->Wait();
    }
    resizedConn = window->EventResized().connect(OnResized);
}

void Update() {
    uint32_t currentFrame = 0;
    ColorClearValue clear{0.0f, 0.0f, 0.0f, 1.0f};
    Stopwatch sw = Stopwatch::StartNew();
    int64_t time = 0;
    while (true) {
        time = sw.ElapsedMilliseconds() / 2;
        window->DispatchEvents();
        float timeMinus = time / 1000.0f;
        int colorElement = static_cast<int>(timeMinus) % 3;
        float t = timeMinus - std::floor(timeMinus);
        float colorValue = t < 0.5 ? Lerp(0.0f, 1.0f, t * 2) : Lerp(1.0f, 0.0f, (t - 0.5f) * 2);
        Eigen::Vector3f color{0.0f, 0.0f, 0.0f};
        color[colorElement] = colorValue;
        clear = {color.x(), color.y(), color.z(), 1.0f};
        if (window->ShouldClose()) {
            break;
        }
        if (winSize.x() == 0 || winSize.y() == 0) {
            continue;
        }

        Frame& frame = frames[currentFrame];
        frame.execFence->Wait();
        Semaphore* imageAvailSem = frame.imageAvailable.get();
        auto acq = swapchain->AcquireNext(imageAvailSem, nullptr);
        if (!acq.HasValue()) {
            continue;
        }

        uint32_t backBufferIndex = swapchain->GetCurrentBackBufferIndex();
        CommandBuffer* cmdBuffer = frame.cmdBuffer.get();
        auto rt = acq.Get();
        vulkan::ImageViewVulkan* rtView = nullptr;
        if (rtViews[backBufferIndex] == nullptr) {
            TextureViewDescriptor rtViewDesc{};
            rtViewDesc.Target = rt;
            rtViewDesc.Dim = TextureViewDimension::Dim2D;
            rtViewDesc.Format = TextureFormat::RGBA8_UNORM;
            rtViewDesc.Usage = TextureUse::RenderTarget;
            rtViewDesc.Range.BaseMipLevel = 0;
            rtViewDesc.Range.MipLevelCount = SubresourceRange::All;
            rtViewDesc.Range.BaseArrayLayer = 0;
            rtViewDesc.Range.ArrayLayerCount = SubresourceRange::All;
            rtViews[backBufferIndex] = StaticCastUniquePtr<vulkan::ImageViewVulkan>(device->CreateTextureView(rtViewDesc).Unwrap());
        }
        rtView = rtViews[backBufferIndex].get();

        cmdBuffer->Begin();
        {
            BarrierTextureDescriptor texDesc[] = {{rt, TextureUse::Uninitialized, TextureUse::RenderTarget, {}, false, false, {}}};
            cmdBuffer->ResourceBarrier({}, texDesc);
        }
        {
            ColorAttachment rpColorAttch[] = {{rtView, LoadAction::Clear, StoreAction::Store, clear}};
            RenderPassDescriptor rpDesc{};
            rpDesc.ColorAttachments = rpColorAttch;
            rpDesc.DepthStencilAttachment = std::nullopt;
            auto rp = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
            rp->BindRootSignature(pipelineLayout.get());
            rp->BindGraphicsPipelineState(pso.get());
            rp->SetViewport({0, 0, (float)winSize.x(), (float)winSize.y(), 0.0f, 1.0f});
            rp->SetScissor({0, 0, (uint32_t)winSize.x(), (uint32_t)winSize.y()});
            VertexBufferView vbv[] = {{vertBuf.get(), 0, vertBuf->_allocInfo.size}};
            rp->BindVertexBuffer(vbv);
            rp->BindIndexBuffer({idxBuf.get(), 0, 2});
            rp->DrawIndexed(3, 1, 0, 0, 0);
            cmdBuffer->EndRenderPass(std::move(rp));
        }
        {
            BarrierTextureDescriptor texDesc[] = {{rt, TextureUse::RenderTarget, TextureUse::Present, {}, false, false, {}}};
            cmdBuffer->ResourceBarrier({}, texDesc);
        }
        cmdBuffer->End();

        Semaphore* signalSems[] = {backBufRenderFinished[backBufferIndex].get()};
        CommandQueueSubmitDescriptor submitDesc{};
        submitDesc.CmdBuffers = std::span{&cmdBuffer, 1};
        submitDesc.WaitSemaphores = std::span{&imageAvailSem, 1};
        submitDesc.SignalSemaphores = signalSems;
        submitDesc.SignalFence = frame.execFence.get();
        cmdQueue->Submit(submitDesc);

        swapchain->Present(signalSems);

        currentFrame = (currentFrame + 1) % frames.size();
    }
}

void End() {
    resizedConn.disconnect();
    for (auto& i : frames) {
        i.execFence->Wait();
    }
    cmdQueue->Wait();
    idxBuf.reset();
    vertBuf.reset();
    pso.reset();
    pipelineLayout.reset();
    backBufRenderFinished.clear();
    frames.clear();
    rtViews.clear();
    swapchain.reset();
    cmdQueue = nullptr;
    device.reset();
    DestroyVulkanInstance(std::move(vkInstance));
    window.reset();
    dxc.reset();
}

int main() {
    Init();
    Update();
    End();
    return 0;
}
