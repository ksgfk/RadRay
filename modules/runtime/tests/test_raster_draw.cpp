#include <gtest/gtest.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/raster_draw.h>
#include <radray/runtime/gpu_mesh.h>
#include <cstring>
#if defined(RADRAY_RASTER_COMPILER_PATH) && defined(_WIN32)
#include <radray/shader_compiler/client.h>
#include <radray/platform/win32_headers.h>

namespace radray::test {
namespace {
class RasterDraw : public ::testing::Test {
protected:
    FrameTimeline Timeline{2};
    unique_ptr<GpuSystem> Gpu;
    unique_ptr<RenderSystem> Renderer;
    radray::RasterDraw Core;
    unique_ptr<render::Texture> Color, Depth;
    unique_ptr<render::TextureView> ColorView, DepthView;
    unique_ptr<render::Buffer> Readback;
    std::optional<AppFrameContext> Frame;
    HWND Window{};
    uint32_t Pitch(uint32_t width) const {
        const uint32_t alignment = std::max(1u, Gpu->GetDevice()->GetDetail().TextureDataPitchAlignment);
        return (width * 4 + alignment - 1) / alignment * alignment;
    }
    virtual render::RenderBackend Backend() const { return render::RenderBackend::D3D12; }
    void SetUp() override {
        RuntimeStartupResult startup;
        GpuSystemDescriptor descriptor;
        descriptor.EnableFrameProfiler = false;
        descriptor.DXGIFactory.IsEnableDebugLayer = false;
        descriptor.VulkanInstance.IsEnableDebugLayer = false;
        const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
        if (Backend() == render::RenderBackend::Vulkan) {
            render::VulkanDeviceDescriptor device; device.Queues = std::span{&queue, 1}; descriptor.Device = device;
        }
        Gpu = GpuSystem::TryCreate(descriptor, Timeline, startup);
        if (startup.Status == RuntimeStartupStatus::BackendNotBuilt || startup.Status == RuntimeStartupStatus::NoAdapter) GTEST_SKIP() << startup.Reason;
        ASSERT_TRUE(Gpu) << startup.Reason;
        Renderer = make_unique<RenderSystem>(Gpu->GetDevice());
        ASSERT_TRUE(Renderer->OnInitialize());
        auto* device = Gpu->GetDevice();
        Color = device->CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
            render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::RenderTarget | render::TextureUse::CopySource}).Unwrap();
        Depth = device->CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::D32_FLOAT,
            render::MemoryType::Device, render::TextureUse::DepthStencilWrite}).Unwrap();
        ColorView = device->CreateTextureView({Color.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM,
            {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget}).Unwrap();
        DepthView = device->CreateTextureView({Depth.get(), render::TextureDimension::Dim2D, render::TextureFormat::D32_FLOAT,
            {0, 1, 0, 1}, render::TextureViewUsage::DepthWrite}).Unwrap();
        Readback = device->CreateBuffer({4096, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
    }
    void TearDown() override {
        if (Frame) {
            Frame->SubmitFrame();
            Gpu->WaitAndRetireFlights();
            FlightCompletion completion;
            while (Timeline.TryReadCompletion(completion)) Gpu->ReleaseFrameResourcesGT(completion);
            Frame.reset();
        }
        if (Gpu) Gpu->AbandonUnpublishedResourcesTerminalGT();
        if (Window) DestroyWindow(Window);
    }
    Nullable<ShaderProgram*> Program(bool textures, bool arrays = false) {
        string source = R"(
#ifdef __spirv__
#define BINDING(b,s) [[vk::binding(b,s)]]
#else
#define BINDING(b,s)
#endif
struct ViewValues { float4 Data; };
struct MaterialValues { float4 Color; };
BINDING(0,0) ConstantBuffer<ViewValues> ViewData : register(b0);
BINDING(1,0) ConstantBuffer<MaterialValues> Tint : register(b1);
struct Vertex { float4 Position : SV_Position; };
[shader("vertex")] Vertex VSMain(uint id : SV_VertexID) {
    Vertex v; float2 positions[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
    v.Position = float4(positions[id], ViewData.Data.x, 1); return v;
}
)";
        if (textures) source += R"(
BINDING(2,0) Texture2D<float4> ImageA : register(t0);
BINDING(3,0) Texture2D<float4> ImageB : register(t1);
BINDING(0,1) SamplerState SampleA : register(s0, space1);
BINDING(1,1) SamplerState SampleB : register(s1, space1);
[shader("pixel")] float4 PSMain(Vertex v) : SV_Target {
    return float4(Tint.Color.rgb + 0.5 * (ImageA.Sample(SampleA,float2(ViewData.Data.y,0.5)).rgb +
        ImageB.Sample(SampleB,float2(ViewData.Data.y,0.5)).rgb), Tint.Color.a);
}
)";
        else source += R"([shader("pixel")] float4 PSMain(Vertex v) : SV_Target { return Tint.Color; })";
        if (arrays) {
            const auto replace = [&](std::string_view from, std::string_view to) {
                const auto at = source.find(from); if (at != string::npos) source.replace(at, from.size(), to);
            };
            replace("Texture2D<float4> ImageA : register(t0);", "Texture2D<float4> Images[2] : register(t0);");
            replace("BINDING(3,0) Texture2D<float4> ImageB : register(t1);", "");
            replace("ImageA.Sample", "Images[0].Sample"); replace("ImageB.Sample", "Images[1].Sample");
        }
        shader_compiler::Client compiler{RADRAY_RASTER_COMPILER_PATH};
        shader::SourceContractRequest discovery;
        discovery.SourceName = "native-raster.hlsl";
        const auto input = std::as_bytes(std::span{source.data(), source.size()});
        discovery.RootSource.assign(input.begin(), input.end());
        discovery.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(Backend() == render::RenderBackend::Vulkan ? shader::ShaderTarget::SPIRV : shader::ShaderTarget::DXIL));
        const auto contract = compiler.DiscoverSourceContract(discovery, {});
        EXPECT_TRUE(contract.Succeeded());
        if (!contract.Succeeded()) return nullptr;
        shader::CompileVariantRequest request;
        request.SourceName = discovery.SourceName; request.RootSource = discovery.RootSource;
        request.Targets = discovery.Targets; request.ExpectedContract = contract.Contract.Hash;
        const auto compiled = compiler.CompileVariant(request, {});
        EXPECT_EQ(compiled.Status, shader::CompileStatus::Success);
        if (compiled.Lanes.size() != 1) return nullptr;
        const auto& bytes = compiled.Lanes[0].Metadata;
        shader::WireMetadataEnvelope envelope{}; std::memcpy(&envelope, bytes.data(), sizeof(envelope));
        auto wrong = envelope.GpuArtifact; wrong.Bytes[0] ^= 1;
        EXPECT_FALSE(Renderer->GetOrCreateShaderProgram(bytes, wrong));
        return Renderer->GetOrCreateShaderProgram(bytes, envelope.GpuArtifact);
    }
    MaterialRenderData Material(ShaderProgram* program, bool textures, bool arrays = false) {
        MaterialPass pass; pass.Name = "Raster"; pass.RegisteredProgram = program;
        pass.Primitive.Cull = render::CullMode::None;
        pass.Inputs = {{"ViewData", MaterialInputSource::ViewConstants}, {"Tint", MaterialInputSource::MaterialConstants}};
        if (textures) {
            if (arrays) pass.Inputs.push_back({"Images", MaterialInputSource::Texture, 0});
            else {
                pass.Inputs.push_back({"ImageA", MaterialInputSource::Texture, 0});
                pass.Inputs.push_back({"ImageB", MaterialInputSource::Texture, 1});
            }
            pass.Inputs.push_back({"SampleA", MaterialInputSource::Sampler, 0});
            pass.Inputs.push_back({"SampleB", MaterialInputSource::Sampler, 1});
        }
        return {{std::move(pass)}, {vector<byte>(16)}, {}, {}};
    }
    void Begin() {
        Gpu->BeginUpdateForFlight(0); Frame = Gpu->BeginFrameRecord(0, {}, {}, false);
        if (Backend() == render::RenderBackend::Vulkan) {
            auto* commands = Frame->AllocateCommandBuffer();
            const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = Readback.get(), .Before = render::BufferState::Undefined, .After = render::BufferState::CopyDestination};
            commands->ResourceBarrier(std::span{&barrier, 1}); Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        }
    }
    void ReadbackBarrier(render::CommandBuffer* commands) {
        if (Backend() == render::RenderBackend::Vulkan) {
            const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = Readback.get(), .Before = render::BufferState::CopyDestination, .After = render::BufferState::HostRead};
            commands->ResourceBarrier(std::span{&barrier, 1});
        }
    }
    void Complete() {
        Frame->SubmitFrame();
        const auto signal = Gpu->GetFlightGpuSignal(0);
        ASSERT_TRUE(signal.Fence->WaitFor(signal.Value, 10000));
        ASSERT_TRUE(Gpu->CompleteFlightIfReady(0, false));
        FlightCompletion completion; ASSERT_TRUE(Timeline.TryReadCompletion(completion));
        EXPECT_TRUE(completion.GpuWorkCompleted);
        Gpu->ReleaseFrameResourcesGT(completion); Timeline.CleanupCompletedFlights(); Frame.reset();
    }
    render::RenderPass* Pass(bool depth) {
        const render::RenderPassColorAttachmentDescriptor color{render::TextureFormat::RGBA8_UNORM, 1, render::LoadAction::Clear, render::StoreAction::Store};
        std::optional<render::RenderPassDepthStencilAttachmentDescriptor> d;
        if (depth) d = {render::TextureFormat::D32_FLOAT, 1, render::LoadAction::Clear, render::StoreAction::Store, render::LoadAction::DontCare, render::StoreAction::Discard};
        return Renderer->GetRenderPassRegistry()->GetOrCreateRenderPass({std::span{&color, 1}, d}).Unwrap();
    }
    render::GraphicsCommandEncoder* Start(render::CommandBuffer* commands, render::RenderPass* pass, bool depth, float clearDepth = 1) {
        render::TextureView* colors[]{ColorView.get()};
        auto framebuffer = Renderer->GetRenderPassRegistry()->GetOrCreateFramebuffer({pass, colors, depth ? DepthView.get() : nullptr, 16, 16, 1}).Unwrap();
        const render::ResourceBarrierDescriptor barriers[]{
            render::BarrierTextureDescriptor{.Target = Color.get(), .Before = render::TextureState::Undefined, .After = render::TextureState::RenderTarget},
            render::BarrierTextureDescriptor{.Target = Depth.get(), .Before = render::TextureState::Undefined, .After = render::TextureState::DepthWrite}};
        commands->ResourceBarrier(std::span{barriers, depth ? 2u : 1u});
        const render::ColorClearValue clear{{0, 0, 0, 1}};
        return commands->BeginRenderPass({pass, framebuffer, std::span{&clear, 1}, depth ? std::optional{render::DepthStencilClearValue{clearDepth, 0}} : std::nullopt}).Unwrap().release();
    }
    vector<uint8_t> Read(render::CommandBuffer* commands, render::GraphicsCommandEncoder* encoder) {
        commands->EndRenderPass(unique_ptr<render::GraphicsCommandEncoder>{encoder});
        const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{.Target = Color.get(), .Before = render::TextureState::RenderTarget, .After = render::TextureState::CopySource};
        commands->ResourceBarrier(std::span{&barrier, 1});
        commands->CopyTextureToBuffer(Readback.get(), 0, Color.get(), {0, 1, 0, 1});
        ReadbackBarrier(commands);
        Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        Complete();
        auto* data = static_cast<const uint8_t*>(Readback->Map(0, 4096));
        Readback->InvalidateMappedRange({0, 4096}); vector<uint8_t> pixels(data, data + 4096); Readback->Unmap(); return pixels;
    }
    PreparedRasterDraw Draw(render::GraphicsPipelineState* pipeline) {
        PreparedRasterDraw draw; draw.Pipeline = pipeline; draw.Count = 3;
        draw.Viewport = {0, 0, 16, 16, 0, 1}; draw.Scissor = {0, 0, 16, 16}; return draw;
    }
    void Mixed(bool arrays = false);
    void DepthStates(bool reverse = false);
    void Regions();
    void PresentExternal(bool hide) {
        Window = CreateWindowExW(0, L"STATIC", L"RadRay raster producer", WS_OVERLAPPEDWINDOW,
            40, 40, 160, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(Window, nullptr); ShowWindow(Window, SW_SHOW); UpdateWindow(Window);
        auto chain = Gpu->GetDevice()->CreateSwapChain({.PresentQueue = Gpu->GetMainQueue(), .NativeHandler = Window,
            .Width = 16, .Height = 16, .BackBufferCount = 2, .Format = render::TextureFormat::RGBA8_UNORM, .PresentMode = render::PresentMode::Immediate}).Unwrap();
        auto program = Program(false); ASSERT_TRUE(program); auto material = Material(program.Get(), false);
        auto plan = Core.ResolveMaterial(*Renderer, material.Passes[0]); ASSERT_TRUE(plan);
        Begin(); auto context = MakePipelineContext(*Renderer, *Frame);
        const float view[]{0.25f, 0, 0, 0}, tint[]{1, 0, 0, 1};
        const std::span<const byte> views[]{std::as_bytes(std::span{view})}, constants[]{std::as_bytes(std::span{tint})};
        auto parameters = Core.PrepareMaterial(context, *plan.Get(), material, {.Constants = constants, .Views = views}); ASSERT_TRUE(parameters);
        auto pass = Pass(false);
        auto pipeline = Core.PreparePipeline(context, *plan.Get(), material.Passes[0], {}, render::PrimitiveTopology::TriangleList, pass,
            {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}}); ASSERT_TRUE(pipeline);
        auto* commands = Frame->AllocateCommandBuffer(); auto* encoder = Start(commands, pass, false);
        Draw(pipeline.Get()).Record(encoder, *parameters);
        commands->EndRenderPass(unique_ptr<render::GraphicsCommandEncoder>{encoder});
        const render::ResourceBarrierDescriptor source = render::BarrierTextureDescriptor{.Target = Color.get(), .Before = render::TextureState::RenderTarget, .After = render::TextureState::CopySource};
        commands->ResourceBarrier(std::span{&source, 1}); commands->CopyTextureToBuffer(Readback.get(), 0, Color.get(), {0, 1, 0, 1});
        Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        auto acquired = chain->AcquireNext(1000); ASSERT_EQ(acquired.Status, render::SwapChainStatus::Success);
        uint32_t presents = 0; render::SwapChainStatus status = render::SwapChainStatus::Error;
        auto target = Frame->AdoptSwapChainFrame(chain.get(), std::move(*acquired.Frame), [this] { return IsWindowVisible(Window) != 0; },
            [&](render::SwapChainPresentResult result) { ++presents; status = result.Status; });
        auto* output = Frame->AllocateCommandBuffer();
        const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{.Target = target.BackBuffer, .Before = render::TextureState::Present, .After = render::TextureState::CopyDestination};
        output->ResourceBarrier(std::span{&before, 1});
        output->CopyTextureToTexture({.Destination = target.BackBuffer, .Source = Color.get(), .Width = 16, .Height = 16});
        const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{.Target = target.BackBuffer, .Before = render::TextureState::CopyDestination, .After = render::TextureState::Present};
        output->ResourceBarrier(std::span{&after, 1});
        Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&output, 1}}, std::move(target));
        if (hide) ShowWindow(Window, SW_HIDE);
        Complete(); EXPECT_EQ(presents, 1u);
        EXPECT_EQ(status, hide ? render::SwapChainStatus::RetryLater : render::SwapChainStatus::Success);
        auto* pixels = static_cast<const uint8_t*>(Readback->Map(0, 4096)); Readback->InvalidateMappedRange({0, 4096});
        EXPECT_EQ(pixels[8 * Pitch(16) + 8 * 4], 255); Readback->Unmap();
    }
};

TEST_F(RasterDraw, ExternalSurfacePresentsOnceAndRetiresOnTheFinalFence) { PresentExternal(false); }
TEST_F(RasterDraw, HiddenExternalSurfacePreservesIndependentApplicationWork) { PresentExternal(true); }

void RasterDraw::Mixed(bool arrays) {
    auto program = Program(true, arrays); ASSERT_TRUE(program);
    auto material = Material(program.Get(), true, arrays);
    auto& pass = material.Passes[0];
    auto plan = Core.ResolveMaterial(*Renderer, pass); ASSERT_TRUE(plan);
    auto missing = pass; missing.Inputs.pop_back(); EXPECT_FALSE(Core.ResolveMaterial(*Renderer, missing));
    auto wrong = pass; wrong.Inputs[0].Source = MaterialInputSource::Texture; EXPECT_FALSE(Core.ResolveMaterial(*Renderer, wrong));
    Begin(); auto context = MakePipelineContext(*Renderer, *Frame);
    vector<unique_ptr<render::Texture>> images;
    vector<unique_ptr<render::TextureView>> imageViews;
    for (uint32_t i = 0; i < 2; ++i) {
        images.push_back(Gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2D, 2, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
            render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::CopyDestination}).Unwrap());
        imageViews.push_back(Gpu->GetDevice()->CreateTextureView({images.back().get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM,
            {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap());
    }
    auto* commands = Frame->AllocateCommandBuffer();
    const uint32_t a[]{0xff0000ff, 0xffff0000}, b[]{0xff00ff00, 0xffff0000};
    const TextureRegionUploadRequest uploads[]{{std::as_bytes(std::span{a}), images[0].get(), {.Width = 2, .Height = 1}},
        {std::as_bytes(std::span{b}), images[1].get(), {.Width = 2, .Height = 1}}};
    ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(commands, uploads));
    render::TextureView* textures[]{imageViews[0].get(), imageViews[1].get()};
    render::SamplerDescriptor nearest{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
        render::FilterMode::Nearest, render::FilterMode::Nearest, render::FilterMode::Nearest, 0, 1};
    auto linear = nearest; linear.MinFilter = linear.MagFilter = render::FilterMode::Linear;
    const render::SamplerDescriptor samplers[]{nearest, linear};
    float view[]{0.25f, 0.4f, 0, 0}, tint[]{0, 0, 0, 1};
    const std::span<const byte> views[]{std::as_bytes(std::span{view})}, constants[]{std::as_bytes(std::span{tint})};
    RasterMaterialValues values{.Constants = constants, .Textures = textures, .Samplers = samplers, .Views = views};
    const std::span<const byte> tooSmall[]{std::as_bytes(std::span{view, size_t{1}})};
    auto invalid = values; invalid.Views = tooSmall; EXPECT_FALSE(Core.PrepareMaterial(context, *plan.Get(), material, invalid));
    auto badIndex = pass; badIndex.Inputs[2].ValueIndex = 2;
    auto badPlan = Core.ResolveMaterial(*Renderer, badIndex); ASSERT_TRUE(badPlan);
    EXPECT_FALSE(Core.PrepareMaterial(context, *badPlan.Get(), material, values));
    auto parameters = Core.PrepareMaterial(context, *plan.Get(), material, values); ASSERT_TRUE(parameters);
    auto nativePass = Pass(false);
    const RenderOutputInfo output{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}};
    auto pipeline = Core.PreparePipeline(context, *plan.Get(), pass, {}, render::PrimitiveTopology::TriangleList, nativePass, output); ASSERT_TRUE(pipeline);
    auto* encoder = Start(commands, nativePass, false);
    auto draw = Draw(pipeline.Get()); draw.Record(encoder, *parameters);
    tint[0] = 0.25f;
    auto second = Core.PrepareMaterial(context, *plan.Get(), material, values); ASSERT_TRUE(second);
    EXPECT_EQ(second->Groups.back().Parameters, parameters->Groups.back().Parameters);
    EXPECT_EQ(Core.PreparePipeline(context, *plan.Get(), pass, {}, render::PrimitiveTopology::TriangleList, nativePass, output).Get(), pipeline.Get());
    EXPECT_EQ(Renderer->GetGraphicsPipelineCache().GetSize(), 1u);
    auto indices = Gpu->GetDevice()->CreateBuffer({12, render::MemoryType::Upload, render::BufferUse::Index | render::BufferUse::MapWrite}).Unwrap();
    const uint32_t triangle[]{0, 1, 2}; auto mapped = indices->Map(0, 12); std::memcpy(mapped, triangle, 12); indices->FlushMappedRange({0, 12}); indices->Unmap();
    draw.Indices = render::IndexBufferView{indices.get(), 0, 4}; draw.Scissor.Width = 8; draw.Record(encoder, *second);
    const auto pixels = Read(commands, encoder);
    const auto left = 8 * Pitch(16) + 4 * 4, right = 8 * Pitch(16) + 12 * 4;
    EXPECT_NEAR(pixels[left], 191, 1); EXPECT_NEAR(pixels[right], 128, 1);
    EXPECT_NEAR(pixels[right + 1], 89, 1); EXPECT_NEAR(pixels[right + 2], 38, 1);
}

void RasterDraw::DepthStates(bool reverse) {
    auto program = Program(false); ASSERT_TRUE(program);
    auto material = Material(program.Get(), false); auto& pass = material.Passes[0];
    pass.DepthStencil = render::DepthStencilState::Default(); pass.DepthStencil->DepthWriteEnable = false;
    pass.DepthStencil->DepthCompare = reverse ? render::CompareFunction::Greater : render::CompareFunction::Less;
    auto plan = Core.ResolveMaterial(*Renderer, pass); ASSERT_TRUE(plan);
    Begin(); auto context = MakePipelineContext(*Renderer, *Frame); auto nativePass = Pass(true);
    auto* commands = Frame->AllocateCommandBuffer(); auto* encoder = Start(commands, nativePass, true, reverse ? 0.0f : 1.0f);
    const RenderOutputInfo output{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, render::TextureFormat::D32_FLOAT};
    for (uint32_t i = 0; i < 4; ++i) {
        pass.DepthStencil->DepthWriteEnable = i >= 2;
        float view[]{(i % 2 != 0) == reverse ? 0.2f : 0.8f, 0, 0, 0}, tint[]{i == 0 || i == 3 ? 1.0f : 0, i == 1 ? 1.0f : 0, i == 2 ? 1.0f : 0, 1};
        const std::span<const byte> views[]{std::as_bytes(std::span{view})}, constants[]{std::as_bytes(std::span{tint})};
        auto parameters = Core.PrepareMaterial(context, *plan.Get(), material, {.Constants = constants, .Views = views}); ASSERT_TRUE(parameters);
        auto pipeline = Core.PreparePipeline(context, *plan.Get(), pass, {}, render::PrimitiveTopology::TriangleList, nativePass, output); ASSERT_TRUE(pipeline);
        Draw(pipeline.Get()).Record(encoder, *parameters);
    }
    EXPECT_EQ(Renderer->GetGraphicsPipelineCache().GetSize(), 2u);
    const auto pixels = Read(commands, encoder); EXPECT_EQ(pixels[8 * Pitch(16) + 8 * 4], 0); EXPECT_EQ(pixels[8 * Pitch(16) + 8 * 4 + 2], 255);
}

void RasterDraw::Regions() {
    Begin(); auto* commands = Frame->AllocateCommandBuffer();
    auto texture = Gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2DArray, 8, 8, 2, 2, 1, render::TextureFormat::RGBA8_UNORM,
        render::MemoryType::Device, render::TextureUse::CopySource | render::TextureUse::CopyDestination}).Unwrap();
    const vector<uint32_t> initial(16, 0xff332211);
    TextureRegionUploadRequest request{std::as_bytes(std::span{initial}), texture.get(), {.MipLevel = 1, .ArrayLayer = 1, .Width = 4, .Height = 4}, 0,
        render::TextureState::Undefined, render::TextureState::CopySource};
    ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(commands, std::span{&request, 1}));
    const uint32_t patch[]{0xff998877, 0xff998877, 0, 0xff998877, 0xff998877};
    request = {std::as_bytes(std::span{patch}), texture.get(), {.MipLevel = 1, .ArrayLayer = 1, .X = 1, .Y = 1, .Width = 2, .Height = 2}, 12,
        render::TextureState::CopySource, render::TextureState::CopySource};
    auto invalid = request; invalid.Region.X = UINT32_MAX;
    const TextureRegionUploadRequest invalidBatch[]{request, invalid};
    EXPECT_FALSE(Frame->GetUploader().TryUploadTextureRegions(commands, invalidBatch));
    commands->CopyTextureToBuffer(Readback.get(), 0, texture.get(), {1, 1, 1, 1});
    invalid = request; invalid.SrcRowPitch = UINT64_MAX; EXPECT_FALSE(Frame->GetUploader().TryUploadTextureRegions(commands, std::span{&invalid, 1}));
    ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(commands, std::span{&request, 1}));
    commands->CopyTextureToBuffer(Readback.get(), 1024, texture.get(), {1, 1, 1, 1});
    ReadbackBarrier(commands); Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}); Complete();
    auto* pixels = static_cast<const uint8_t*>(Readback->Map(0, 4096)); Readback->InvalidateMappedRange({0, 4096});
    EXPECT_EQ(pixels[Pitch(4) + 4], 0x11); EXPECT_EQ(pixels[1024], 0x11); EXPECT_EQ(pixels[1024 + Pitch(4) + 4], 0x77);
    EXPECT_EQ(pixels[1024 + 3 * Pitch(4) + 3 * 4], 0x11); Readback->Unmap();
}
class VulkanRasterDraw : public RasterDraw {
    render::RenderBackend Backend() const override { return render::RenderBackend::Vulkan; }
};
TEST_F(RasterDraw, PrecompiledMixedGroupsTwoTexturesSamplersIndexedAndNonIndexed) { Mixed(); }
TEST_F(VulkanRasterDraw, PrecompiledMixedGroupsTwoTexturesSamplersIndexedAndNonIndexed) { Mixed(); }
TEST_F(RasterDraw, IndependentDepthWriteAndCompareDriveTheExistingPipelineCache) { DepthStates(); }
TEST_F(VulkanRasterDraw, IndependentDepthWriteAndCompareDriveTheExistingPipelineCache) { DepthStates(); }
TEST_F(RasterDraw, TextureRegionsValidateWholeBatchAndPreservePixelsOutsidePaddedMipLayerPatch) { Regions(); }
TEST_F(VulkanRasterDraw, TextureRegionsValidateWholeBatchAndPreservePixelsOutsidePaddedMipLayerPatch) { Regions(); }
TEST_F(RasterDraw, ReflectedTextureArrayUsesEveryValueAndRejectsAnOutOfRangeIndex) { Mixed(true); }
TEST_F(VulkanRasterDraw, ReflectedTextureArrayUsesEveryValueAndRejectsAnOutOfRangeIndex) { Mixed(true); }
TEST_F(RasterDraw, ReversedDepthUsesGreaterCompareAndZeroClearWithIndependentWrite) { DepthStates(true); }
TEST_F(VulkanRasterDraw, ReversedDepthUsesGreaterCompareAndZeroClearWithIndependentWrite) { DepthStates(true); }
TEST_F(RasterDraw, MaterialAcceptsRegisteredProgramAndRejectsAmbiguousSourceOwnership) {
    auto program = Program(false); ASSERT_TRUE(program);
    auto descriptor = Material(program.Get(), false);
    radray::Material registered{descriptor.Passes, descriptor.Constants}; EXPECT_TRUE(registered.IsValid());
    descriptor.Passes[0].Program.SourceName = "ambiguous.hlsl";
    radray::Material ambiguous{descriptor.Passes, descriptor.Constants}; EXPECT_FALSE(ambiguous.IsValid());
    EXPECT_FALSE(Core.ResolveMaterial(*Renderer, descriptor.Passes[0]));
    descriptor.Passes[0].Program.SourceName.clear(); descriptor.Passes[0].RegisteredProgram = nullptr;
    radray::Material missing{descriptor.Passes, descriptor.Constants}; EXPECT_FALSE(missing.IsValid());
}
TEST_F(RasterDraw, RuntimeMeshUsesExistingUploaderAndPreservesPackedBinsAfterProducerRelease) {
    MeshResource source;float vertices[]{0,0,0,1,0,0,0,1,0};uint32_t indices[]{0,1,2};
    source.Bins.emplace_back(std::as_bytes(std::span{vertices}));source.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;primitive.VertexCount=3;primitive.VertexBuffers={{"POSITION",0,0,VertexDataType::FLOAT,3,0,12}};primitive.IndexBuffer={1,3,0,4};source.Primitives.push_back(primitive);
    EXPECT_FALSE(AllocateRuntimeMesh(*Gpu->GetDevice(),source,32));auto mesh=AllocateRuntimeMesh(*Gpu->GetDevice(),source);ASSERT_TRUE(mesh);EXPECT_EQ(mesh->Draws[0].Ibv.Offset,36u);
    Begin();auto* command=Frame->AllocateCommandBuffer();ASSERT_TRUE(TryUploadRuntimeMesh(Frame->GetUploader(),command,source,*mesh.Get()));
    source.Bins[0]=MeshBuffer(std::span<const byte>{});
    render::ResourceBarrierDescriptor barrier=render::BarrierBufferDescriptor{.Target=mesh->Buffers[0].get(),.Before=render::BufferState::Vertex|render::BufferState::Index,.After=render::BufferState::CopySource};command->ResourceBarrier(std::span{&barrier,1});
    command->CopyBufferToBuffer(Readback.get(),0,mesh->Buffers[0].get(),0,48);ReadbackBarrier(command);Frame->ReturnCommandBuffers({.CmdBuffers=std::span{&command,1}});Complete();
    const auto* data=Readback->Map(0,48);Readback->InvalidateMappedRange({0,48});EXPECT_EQ(std::memcmp(data,vertices,36),0);EXPECT_EQ(std::memcmp(static_cast<const byte*>(data)+36,indices,12),0);Readback->Unmap();
}
TEST_F(RasterDraw, InstanceSchemaComesFromCompilerPayloadAndRejectsMissingOrOverlappingFields) {
    auto program = Program(false); ASSERT_TRUE(program);
    auto layout = ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Tint", "InstanceTint");
    ASSERT_TRUE(layout); EXPECT_EQ(layout->Stride, 16u); ASSERT_EQ(layout->Fields.size(), 1u);
    EXPECT_EQ(layout->Fields[0].Name, "Color"); EXPECT_EQ(layout->Fields[0].Offset, 0u);
    EXPECT_FALSE(ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Missing", "Bad"));
    layout->Fields.push_back(layout->Fields[0]); EXPECT_FALSE(layout->IsValid());
}
}  // namespace
}  // namespace radray::test
#endif
