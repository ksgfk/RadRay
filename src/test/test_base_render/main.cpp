#include <thread>

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/window/glfw_window.h>
#include <radray/render/device.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_pool.h>
#include <radray/render/command_buffer.h>
#include <radray/render/command_encoder.h>
#include <radray/render/shader.h>
#include <radray/render/root_signature.h>
#include <radray/render/pipeline_state.h>
#include <radray/render/swap_chain.h>
#include <radray/render/resource.h>

using namespace radray;
using namespace radray::render;
using namespace radray::window;

const GraphicsPipelineStateDescriptor DEFAULT_PSO_DESC{
    "color pso",
    nullptr,
    nullptr,
    nullptr,
    {VertexBufferLayout{
        56,
        VertexStepMode::Vertex,
        {
            VertexElement{0, VertexSemantic::Position, 0, VertexFormat::FLOAT32X3, 0},
        }}},
    PrimitiveState{
        PrimitiveTopology::TriangleList,
        IndexFormat::UINT32,
        FrontFace::CCW,
        CullMode::Back,
        PolygonMode::Fill,
        false,
        false},
    DepthStencilState{
        TextureFormat::D24_UNORM_S8_UINT,
        CompareFunction::LessEqual,
        StencilState{
            StencilFaceState{
                CompareFunction::Always,
                StencilOperation::Keep,
                StencilOperation::Keep,
                StencilOperation::Keep},
            StencilFaceState{
                CompareFunction::Always,
                StencilOperation::Keep,
                StencilOperation::Keep,
                StencilOperation::Keep},
            0xFF,
            0xFF},
        DepthBiasState{0, 0.0f, 0.0f},
        true,
        false},
    MultiSampleState{
        1,
        0xFFFFFFFF,
        false},
    {ColorTargetState{
        TextureFormat::RGBA8_UNORM,
        {{BlendFactor::One,
          BlendFactor::Zero,
          BlendOperation::Add},
         {BlendFactor::One,
          BlendFactor::Zero,
          BlendOperation::Add}},
        ToFlag(ColorWrite::All),
        false}},
    true};

class TestApp {
public:
    TestApp() = default;

    ~TestApp() noexcept {
        verts = nullptr;
        dxc = nullptr;
        pso = nullptr;
        cmdBuffer = nullptr;
        cmdPool = nullptr;
        swapchain = nullptr;
        device = nullptr;
        window = nullptr;
        GlobalTerminateGlfw();
    }

    void Start() {
        GlobalInitGlfw();
        window = radray::make_unique<GlfwWindow>(RADRAY_APPNAME, 1280, 720);
#if defined(RADRAY_PLATFORM_WINDOWS)
        device = CreateDevice(D3D12DeviceDescriptor{std::nullopt, true, false}).Unwrap();
#elif defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
        device = CreateDevice(MetalDeviceDescriptor{}).value();
#endif
        auto cmdQueue = device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        cmdPool = device->CreateCommandPool(cmdQueue).Unwrap();
        cmdBuffer = device->CreateCommandBuffer(cmdPool.get()).Unwrap();
        dxc = CreateDxc().Unwrap();
        // #if defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
        //         bool isSpirv = true;
        // #elif defined(RADRAY_PLATFORM_WINDOWS)
        //         bool isSpirv = false;
        // #endif
        //         radray::shared_ptr<Shader> vs, ps;
        //         {
        //             std::string_view includes[] = {std::string_view{"shaders"}};
        //             auto color = ReadText(std::filesystem::path("shaders") / "DefaultVS.hlsl").value();
        //             auto outv = dxc->Compile(
        //                 color,
        //                 "main",
        //                 ShaderStage::Vertex,
        //                 HlslShaderModel::SM60,
        //                 true,
        //                 {},
        //                 includes,
        //                 isSpirv);
        //             auto outp = outv.value();
        //             RADRAY_INFO_LOG("type={} size={}", outp.category, outp.data.size());
        // #if defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
        //             auto msl = SpirvToMsl(outp.data, MslVersion::MSL24, MslPlatform::Macos).value();
        //             // RADRAY_INFO_LOG("to msl\n{}", msl.Msl);
        //             // RADRAY_INFO_LOG("refl\n{}", msl.SpvReflJson);
        //             std::span<const byte> blob{reinterpret_cast<const byte*>(msl.Msl.data()), msl.Msl.size()};
        //             radray::string entry = msl.EntryPoints.at(0).Name;
        //             ShaderReflection refl = MslReflection{};
        // #elif defined(RADRAY_PLATFORM_WINDOWS)
        //             std::span<const byte> blob = outp.data;
        //             std::string_view entry = "outv";
        //             DxilReflection refl = dxc->GetDxilReflection(ShaderStage::Vertex, outp.refl).value();
        // #endif
        //             auto shader = device->CreateShader(blob, refl, ShaderStage::Vertex, entry, "colorVS").value();
        //             RADRAY_INFO_LOG("shader name {}", shader->Name);
        //             vs = std::move(shader);
        //         }
        //         {
        //             std::string_view includes[] = {std::string_view{"shaders"}};
        //             auto color = ReadText(std::filesystem::path("shaders") / "DefaultPS.hlsl").value();
        //             auto outv = dxc->Compile(
        //                 color,
        //                 "main",
        //                 ShaderStage::Pixel,
        //                 HlslShaderModel::SM60,
        //                 true,
        //                 {},
        //                 includes,
        //                 isSpirv);
        //             auto outp = outv.value();
        //             RADRAY_INFO_LOG("type={} size={}", outp.category, outp.data.size());
        // #if defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
        //             auto msl = SpirvToMsl(outp.data, MslVersion::MSL24, MslPlatform::Macos).value();
        //             // RADRAY_INFO_LOG("to msl\n{}", msl.Msl);
        //             // RADRAY_INFO_LOG("refl\n{}", msl.SpvReflJson);
        //             std::span<const byte> blob{reinterpret_cast<const byte*>(msl.Msl.data()), msl.Msl.size()};
        //             radray::string entry = msl.EntryPoints.at(0).Name;
        //             ShaderReflection refl = MslReflection{};
        // #elif defined(RADRAY_PLATFORM_WINDOWS)
        //             std::span<const byte> blob = outp.data;
        //             std::string_view entry = "outv";
        //             DxilReflection refl = dxc->GetDxilReflection(ShaderStage::Pixel, outp.refl).value();
        //             refl.StaticSamplers.emplace_back(DxilReflection::StaticSampler{
        //                 {AddressMode::ClampToEdge,
        //                  AddressMode::ClampToEdge,
        //                  AddressMode::ClampToEdge,
        //                  FilterMode::Linear,
        //                  FilterMode::Linear,
        //                  FilterMode::Linear,
        //                  0.0f,
        //                  0.0f,
        //                  CompareFunction::Always,
        //                  1,
        //                  false},
        //                 "baseColorSampler"});
        // #endif
        //             auto shader = device->CreateShader(blob, refl, ShaderStage::Pixel, entry, "colorPS").value();
        //             RADRAY_INFO_LOG("shader name {}", shader->Name);
        //             ps = std::move(shader);
        //         }
        //         {
        //             Shader* shaders[] = {vs.get(), ps.get()};
        //             auto rootSig = device->CreateRootSignature(shaders);
        //             RADRAY_INFO_LOG("root sig done? {}", rootSig.has_value());
        //             pso = device->CreateGraphicsPipeline(desc).value();
        //         }
        {
            radray::string color = ReadText(std::filesystem::path("shaders") / RADRAY_APPNAME / "color.hlsl").value();
            DxcOutput outv = *dxc->Compile(
                color,
                "VSMain",
                ShaderStage::Vertex,
                HlslShaderModel::SM60,
                true,
                {},
                {},
                false);
            DxilReflection reflv = dxc->GetDxilReflection(ShaderStage::Vertex, outv.refl).value();
            auto vs = device->CreateShader(
                outv.data,
                reflv,
                ShaderStage::Vertex,
                "VSMain",
                "colorVS");

            DxcOutput outp = *dxc->Compile(
                color,
                "PSMain",
                ShaderStage::Pixel,
                HlslShaderModel::SM60,
                true,
                {},
                {},
                false);
            DxilReflection reflp = dxc->GetDxilReflection(ShaderStage::Pixel, outp.refl).value();
            auto ps = device->CreateShader(
                outp.data,
                reflp,
                ShaderStage::Pixel,
                "PSMain",
                "colorPS");

            Shader* shaders[] = {vs.Value(), ps.Value()};
            auto rootSig = device->CreateRootSignature(shaders);
            auto psoDesc = DEFAULT_PSO_DESC;
            psoDesc.RootSig = rootSig.Value();
            psoDesc.VS = vs.Value();
            psoDesc.PS = ps.Value();
            pso = device->CreateGraphicsPipeline(psoDesc).Unwrap();
        }
        {
            auto q = device->GetCommandQueue(QueueType::Direct, 0);
            auto size = window->GetSize();
            swapchain = device->CreateSwapChain(
                                  q.Value(),
                                  window->GetNativeHandle(),
                                  size.x(), size.y(), 2,
                                  TextureFormat::RGBA8_UNORM, true)
                            .Unwrap();
        }
        {
            Eigen::Vector3f vertices[] = {
                {0.0f, 0.5f, 0.0f},
                {0.5f, -0.5f, 0.0f},
                {-0.5f, -0.5f, 0.0f}};
            auto upload = device->CreateBuffer(
                sizeof(vertices),
                ResourceType::Buffer,
                ResourceUsage::Upload,
                ToFlag(ResourceState::GenericRead),
                ToFlag(ResourceMemoryTip::None));
            {
                auto ptr = upload->Map(0, upload->GetSize());
                std::memcpy(ptr.Value(), vertices, sizeof(vertices));
                upload->Unmap();
            }
            verts = device->CreateBuffer(
                              sizeof(vertices),
                              ResourceType::Buffer,
                              ResourceUsage::Default,
                              ToFlag(ResourceState::Common),
                              ToFlag(ResourceMemoryTip::None))
                        .Unwrap();
            cmdPool->Reset();
            cmdBuffer->Begin();
            {
                BufferBarrier barriers[] = {
                    {verts.get(),
                     ToFlag(ResourceState::Common),
                     ToFlag(ResourceState::CopyDestination)}};
                ResourceBarriers rb{barriers, {}};
                cmdBuffer->ResourceBarrier(rb);
            }
            cmdBuffer->CopyBuffer(upload.Value(), 0, verts.get(), 0, sizeof(vertices));
            {
                BufferBarrier barriers[] = {
                    {verts.get(),
                     ToFlag(ResourceState::CopyDestination),
                     ToFlag(ResourceState::Common)}};
                ResourceBarriers rb{barriers, {}};
                cmdBuffer->ResourceBarrier(rb);
            }
            cmdBuffer->End();
            CommandBuffer* t[] = {cmdBuffer.get()};
            cmdQueue->Submit(t, Nullable<Fence>{});
            cmdQueue->Wait();
        }
    }

    void Update() {
        while (true) {
            GlobalPollEventsGlfw();
            if (window->ShouldClose()) {
                break;
            }
            swapchain->AcquireNextRenderTarget();
            cmdPool->Reset();
            cmdBuffer->Begin();
            Texture* rt = swapchain->GetCurrentRenderTarget();
            {
                TextureBarrier barriers[] = {
                    {rt,
                     ToFlag(ResourceState::Present),
                     ToFlag(ResourceState::RenderTarget),
                     0, 0, false}};
                ResourceBarriers rb{{}, barriers};
                cmdBuffer->ResourceBarrier(rb);
            }
            auto rtView = device->CreateTextureView(
                                    rt,
                                    ResourceType::RenderTarget,
                                    TextureFormat::RGBA8_UNORM,
                                    TextureDimension::Dim2D,
                                    0, 0,
                                    0, 0)
                              .Unwrap();
            RenderPassDesc rpDesc{};
            ColorAttachment colors[] = {
                {rtView.get(),
                 LoadAction::Clear,
                 StoreAction::Store,
                 ColorClearValue{0.0f, 0.2f, 0.4f, 1.0f}}};
            rpDesc.ColorAttachments = colors;
            auto rp = cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
            cmdBuffer->EndRenderPass(std::move(rp));
            {
                TextureBarrier barriers[] = {
                    {rt,
                     ToFlag(ResourceState::RenderTarget),
                     ToFlag(ResourceState::Present),
                     0, 0, false}};
                ResourceBarriers rb{{}, barriers};
                cmdBuffer->ResourceBarrier(rb);
            }
            cmdBuffer->End();
            CommandBuffer* t[] = {cmdBuffer.get()};
            auto cmdQueue = device->GetCommandQueue(QueueType::Direct, 0);
            cmdQueue->Submit(t, Nullable<Fence>{});
            swapchain->Present();
            cmdQueue->Wait();
            std::this_thread::yield();
        }
    }

public:
    radray::unique_ptr<GlfwWindow> window;
    radray::shared_ptr<Device> device;
    radray::shared_ptr<CommandPool> cmdPool;
    radray::shared_ptr<CommandBuffer> cmdBuffer;
    radray::shared_ptr<SwapChain> swapchain;
    radray::shared_ptr<Dxc> dxc;
    radray::shared_ptr<GraphicsPipelineState> pso;

    radray::shared_ptr<Buffer> verts;
};

int main() {
    TestApp app{};
    app.Start();
    app.Update();
    return 0;
}
