#include <radray/logger.h>
#include <radray/stopwatch.h>
#include <radray/basic_math.h>
#include <radray/render/common.h>
#include <radray/window/native_window.h>

using namespace radray;
using namespace radray::render;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int BACK_BUFFER_COUNT = 3;
constexpr int INFLIGHT_FRAME_COUNT = 2;
const char* RADRAY_APPNAME = "hello_world_metal";
const char* SHADER_SRC = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 color;
};

constant float3 g_Color[3] = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1)
};

vertex VertexOut VSMain(VertexIn in [[stage_in]], uint vertId [[vertex_id]]) {
    VertexOut out;
    out.position = float4(in.position, 1);
    out.color = g_Color[vertId % 3];
    return out;
}

fragment float4 PSMain(VertexOut in [[stage_in]]) {
    return float4(in.color, 1);
}
)";

class Frame {
public:
    unique_ptr<CommandBuffer> cmdBuffer;
    unique_ptr<Fence> execFence;
    unique_ptr<Semaphore> imageAvailable;
};

Eigen::Vector2i winSize;
unique_ptr<NativeWindow> window;
shared_ptr<Device> device;
CommandQueue* cmdQueue;
unique_ptr<SwapChain> swapchain;
vector<Frame> frames;
vector<unique_ptr<Semaphore>> backBufRenderFinished;
unique_ptr<RootSignature> rootSig;
unique_ptr<GraphicsPipelineState> pso;
unique_ptr<Buffer> vertBuf;
unique_ptr<Buffer> idxBuf;
sigslot::connection resizedConn;

void CreateSwapChain() {
    SwapChainDescriptor swapchainDesc{};
    swapchainDesc.PresentQueue = cmdQueue;
    swapchainDesc.NativeHandler = window->GetNativeHandler().Handle;
    swapchainDesc.Width = (uint32_t)winSize.x();
    swapchainDesc.Height = (uint32_t)winSize.y();
    swapchainDesc.BackBufferCount = BACK_BUFFER_COUNT;
    swapchainDesc.FlightFrameCount = INFLIGHT_FRAME_COUNT;
    swapchainDesc.Format = TextureFormat::BGRA8_UNORM;
    swapchainDesc.PresentMode = PresentMode::FIFO;
    swapchain = device->CreateSwapChain(swapchainDesc).Unwrap();
    if (backBufRenderFinished.size() != swapchainDesc.BackBufferCount) {
        backBufRenderFinished.clear();
        backBufRenderFinished.reserve(BACK_BUFFER_COUNT);
        for (uint32_t i = 0; i < swapchain->GetBackBufferCount(); i++) {
            auto sem = device->CreateSemaphoreDevice().Unwrap();
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
        swapchain.reset();
        CreateSwapChain();
    }
}

void Init() {
    winSize = {WIN_WIDTH, WIN_HEIGHT};
    CocoaWindowCreateDescriptor windowDesc{
        RADRAY_APPNAME,
        winSize.x(),
        winSize.y(),
        -1,
        -1,
        true,
        false,
        false};
    window = CreateNativeWindow(windowDesc).Unwrap();
    if (!window) {
        throw std::runtime_error("Failed to create native window");
    }
    MetalDeviceDescriptor deviceDesc{};
    device = CreateDevice(deviceDesc).Unwrap();
    cmdQueue = device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
    CreateSwapChain();
    frames.reserve(INFLIGHT_FRAME_COUNT);
    for (size_t i = 0; i < INFLIGHT_FRAME_COUNT; i++) {
        auto& f = frames.emplace_back();
        f.cmdBuffer = device->CreateCommandBuffer(cmdQueue).Unwrap();
        f.execFence = device->CreateFence().Unwrap();
        f.imageAvailable = device->CreateSemaphoreDevice().Unwrap();
    }
    {
        auto mslStr = SHADER_SRC;
        auto mslBytes = std::span<const byte>(
            reinterpret_cast<const byte*>(mslStr),
            std::strlen(mslStr));
        ShaderDescriptor shaderDesc{};
        shaderDesc.Source = mslBytes;
        shaderDesc.Category = ShaderBlobCategory::MSL;
        auto shader = device->CreateShader(shaderDesc).Unwrap();

        RootSignatureDescriptor rootSigDesc{};
        rootSig = device->CreateRootSignature(rootSigDesc).Unwrap();

        VertexElement ve[] = {
            {0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
        VertexBufferLayout vl{12, VertexStepMode::Vertex, ve};
        ColorTargetState cts = ColorTargetState::Default(TextureFormat::BGRA8_UNORM);
        GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = rootSig.get();
        psoDesc.VS = {shader.get(), "VSMain"};
        psoDesc.PS = {shader.get(), "PSMain"};
        psoDesc.VertexLayouts = std::span{&vl, 1};
        psoDesc.Primitive = PrimitiveState::Default();
        psoDesc.DepthStencil = std::nullopt;
        psoDesc.MultiSample = MultiSampleState::Default();
        psoDesc.ColorTargets = std::span{&cts, 1};
        pso = device->CreateGraphicsPipelineState(psoDesc).Unwrap();
    }
    {
        float pos[] = {0, 0.5f, 0, -0.5f, -0.366f, 0, 0.5f, -0.366f, 0};
        uint16_t idx[] = {0, 2, 1};
        const size_t vertexSize = sizeof(pos);
        const size_t indexSize = sizeof(idx);
        vertBuf = device->CreateBuffer({vertexSize, MemoryType::Upload, BufferUse::Vertex | BufferUse::MapWrite, ResourceHint::None, {}}).Unwrap();
        idxBuf = device->CreateBuffer({indexSize, MemoryType::Upload, BufferUse::Index | BufferUse::MapWrite, ResourceHint::None, {}}).Unwrap();
        {
            void* vertMap = vertBuf->Map(0, vertexSize);
            std::memcpy(vertMap, pos, vertexSize);
            vertBuf->Unmap(0, vertexSize);
        }
        {
            void* idxMap = idxBuf->Map(0, indexSize);
            std::memcpy(idxMap, idx, indexSize);
            idxBuf->Unmap(0, indexSize);
        }
    }
    resizedConn = window->EventResized().connect(OnResized);
}

void Update() {
    uint32_t currentFrame = 0;
    ColorClearValue clear{{{0.0f, 0.0f, 0.0f, 1.0f}}};
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
        clear.Value = {color.x(), color.y(), color.z(), 1.0f};
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
        TextureViewDescriptor rtViewDesc{};
        rtViewDesc.Target = rt;
        rtViewDesc.Dim = TextureViewDimension::Dim2D;
        rtViewDesc.Format = TextureFormat::BGRA8_UNORM;
        rtViewDesc.Usage = TextureUse::RenderTarget;
        rtViewDesc.Range.BaseMipLevel = 0;
        rtViewDesc.Range.MipLevelCount = SubresourceRange::All;
        rtViewDesc.Range.BaseArrayLayer = 0;
        rtViewDesc.Range.ArrayLayerCount = SubresourceRange::All;
        auto rtView = device->CreateTextureView(rtViewDesc).Unwrap();

        cmdBuffer->Begin();
        {
            ColorAttachment rpColorAttch[] = {{rtView.get(), LoadAction::Clear, StoreAction::Store, clear}};
            RenderPassDescriptor rpDesc{};
            rpDesc.ColorAttachments = rpColorAttch;
            rpDesc.DepthStencilAttachment = std::nullopt;
            auto rp = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
            rp->BindRootSignature(rootSig.get());
            rp->BindGraphicsPipelineState(pso.get());
            rp->SetViewport({0, (float)winSize.y(), (float)winSize.x(), -(float)winSize.y(), 0.0f, 1.0f});
            rp->SetScissor({0, 0, (uint32_t)winSize.x(), (uint32_t)winSize.y()});
            VertexBufferView vbv[] = {{vertBuf.get(), 0, vertBuf->GetDesc().Size}};
            rp->BindVertexBuffer(vbv);
            rp->BindIndexBuffer({idxBuf.get(), 0, 2});
            rp->DrawIndexed(3, 1, 0, 0, 0);
            cmdBuffer->EndRenderPass(std::move(rp));
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
    rootSig.reset();
    backBufRenderFinished.clear();
    frames.clear();
    swapchain.reset();
    cmdQueue = nullptr;
    device.reset();
    window.reset();
}

int main() {
    Init();
    Update();
    End();
    return 0;
}
