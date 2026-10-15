#include <gtest/gtest.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/raster_draw.h>
#include <radray/runtime/gpu_mesh.h>
#include <radray/runtime/render_framework/scene_gpu.h>
#include <radray/runtime/render_framework/scene_draw.h>
#include "scene_test_support.h"
#include "runtime_test_device.h"
#include <cstring>
#if defined(RADRAY_RASTER_COMPILER_PATH) && defined(_WIN32)
#include <radray/shader_compiler/client.h>
#include <radray/platform/win32_headers.h>

namespace radray::test {
namespace {
struct MaterialBindingProbe {
    struct Counts {
        uint32_t Created{0}, Set{0}, Flush{0}, Live{0};
    };
    unordered_map<uint32_t, Counts> Groups;
};
class MaterialProbeSet final : public render::ShaderParameterSet {
public:
    MaterialProbeSet(unique_ptr<render::ShaderParameterSet> native, MaterialBindingProbe::Counts& counts) : _native(std::move(native)), _counts(counts) {
        ++_counts.Created;
        ++_counts.Live;
    }
    ~MaterialProbeSet() noexcept override { --_counts.Live; }
    bool IsValid() const noexcept override { return _native->IsValid(); }
    void Destroy() noexcept override { _native->Destroy(); }
    bool Set(render::BindingHandle binding, uint32_t element, render::ShaderParameterValue value) noexcept override {
        ++_counts.Set;
        return _native->Set(binding, element, value);
    }
    void ResetBindingsForReuse() noexcept override { _native->ResetBindingsForReuse(); }
    bool FlushWrites() noexcept override {
        ++_counts.Flush;
        return _native->FlushWrites();
    }

private:
    unique_ptr<render::ShaderParameterSet> _native;
    MaterialBindingProbe::Counts& _counts;
};
class MaterialProbeDevice final : public RuntimeTestDevice {
public:
    MaterialProbeDevice(render::Device* native, MaterialBindingProbe& probe) : _native(native), _probe(probe) {}
    render::RenderBackend GetBackend() noexcept override { return _native->GetBackend(); }
    render::DeviceDetail GetDetail() const noexcept override { return _native->GetDetail(); }
    const render::RenderDeviceCapabilities& GetCapabilities() const noexcept override { return _native->GetCapabilities(); }
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& desc) noexcept override { return _native->CreateBuffer(desc); }
    void FlushMappedRanges(std::span<const render::MappedBufferRange> ranges) noexcept override { _native->FlushMappedRanges(ranges); }
    Nullable<unique_ptr<render::ShaderParameterSet>> CreateShaderParameterSet(const render::ShaderParameterSetDescriptor& desc) noexcept override {
        auto native = _native->CreateShaderParameterSet(desc);
        if (!native) return nullptr;
        return make_unique<MaterialProbeSet>(native.Release(), _probe.Groups[desc.GroupIndex]);
    }

private:
    render::Device* _native;
    MaterialBindingProbe& _probe;
};
class RasterDraw : public ::testing::Test {
protected:
    FrameTimeline Timeline{3};
    unique_ptr<GpuSystem> Gpu;
    unique_ptr<RenderSystem> Renderer;
    radray::RasterDraw Core;
    unique_ptr<render::Texture> Color, Depth;
    unique_ptr<render::TextureView> ColorView, DepthView;
    unique_ptr<render::Buffer> Readback;
    std::optional<AppFrameContext> Frame;
    HWND Window{};
    vector<byte> Metadata;
    uint32_t Pitch(uint32_t width) const {
        const uint32_t alignment = std::max(1u, Gpu->GetDevice()->GetDetail().TextureDataPitchAlignment);
        return (width * 4 + alignment - 1) / alignment * alignment;
    }
    virtual render::RenderBackend Backend() const { return render::RenderBackend::D3D12; }
    void SetUp() override {
        RuntimeStartupResult startup;
        GpuSystemDescriptor descriptor;
        descriptor.RhiValidation.IsEnabled = false;
        descriptor.FlightDataCount = 3;
        descriptor.EnableFrameProfiler = false;
        descriptor.DXGIFactory.IsEnableDebugLayer = false;
        descriptor.VulkanInstance.IsEnableDebugLayer = false;
        const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
        if (Backend() == render::RenderBackend::Vulkan) {
            render::VulkanDeviceDescriptor device;
            device.Queues = std::span{&queue, 1};
            descriptor.Device = device;
        }
        Gpu = GpuSystem::TryCreate(descriptor, Timeline, startup);
        if (startup.Status == RuntimeStartupStatus::BackendNotBuilt || startup.Status == RuntimeStartupStatus::NoAdapter) GTEST_SKIP() << startup.Reason;
        ASSERT_TRUE(Gpu) << startup.Reason;
        Renderer = make_unique<RenderSystem>(Gpu->GetDevice());
        ASSERT_TRUE(Renderer->OnInitialize());
        auto* device = Gpu->GetDevice();
        Color = device->CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                       render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::RenderTarget | render::TextureUse::CopySource})
                    .Unwrap();
        Depth = device->CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::D32_FLOAT,
                                       render::MemoryType::Device, render::TextureUse::DepthStencilWrite})
                    .Unwrap();
        ColorView = device->CreateTextureView({Color.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget}).Unwrap();
        DepthView = device->CreateTextureView({Depth.get(), render::TextureDimension::Dim2D, render::TextureFormat::D32_FLOAT, {0, 1, 0, 1}, render::TextureViewUsage::DepthWrite}).Unwrap();
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
    Nullable<ShaderProgram*> Program(bool textures, bool arrays = false, bool instances = false, bool pure = false, bool visible = false, bool scene = false, bool objectPush = false) {
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
        if (textures)
            source += R"(
BINDING(2,0) Texture2D<float4> ImageA : register(t0);
BINDING(3,0) Texture2D<float4> ImageB : register(t1);
BINDING(0,1) SamplerState SampleA : register(s0, space1);
BINDING(1,1) SamplerState SampleB : register(s1, space1);
[shader("pixel")] float4 PSMain(Vertex v) : SV_Target {
    return float4(Tint.Color.rgb + 0.5 * (ImageA.Sample(SampleA,float2(ViewData.Data.y,0.5)).rgb +
        ImageB.Sample(SampleB,float2(ViewData.Data.y,0.5)).rgb), Tint.Color.a);
}
)";
        else if (instances)
            source += R"(
BINDING(2,0) StructuredBuffer<MaterialValues> InstanceData : register(t0);
[shader("pixel")] float4 PSMain(Vertex v) : SV_Target { return InstanceData[(uint)ViewData.Data.z].Color * Tint.Color; }
)";
        else
            source += R"([shader("pixel")] float4 PSMain(Vertex v) : SV_Target { return Tint.Color; })";
        if (arrays) {
            const auto replace = [&](std::string_view from, std::string_view to) {
                const auto at = source.find(from);
                if (at != string::npos) source.replace(at, from.size(), to);
            };
            replace("Texture2D<float4> ImageA : register(t0);", "Texture2D<float4> Images[2] : register(t0);");
            replace("BINDING(3,0) Texture2D<float4> ImageB : register(t1);", "");
            replace("ImageA.Sample", "Images[0].Sample");
            replace("ImageB.Sample", "Images[1].Sample");
        }
        if (pure) {
            const auto at = source.find("BINDING(1,0) ConstantBuffer<MaterialValues> Tint : register(b1);");
            source.replace(at, string{"BINDING(1,0) ConstantBuffer<MaterialValues> Tint : register(b1);"}.size(),
                           "BINDING(0,1) ConstantBuffer<MaterialValues> Tint : register(b1, space1);");
        }
        if (visible) source = R"hlsl(
#include <core/platform.hlsli>
struct ViewValues { float4 Data; };
struct MaterialValues { float4 Color; };
struct ObjectValues { column_major float4x4 LocalToWorld; };
struct DrawValues { uint Tag; uint VisibleBase; };
VK_BINDING(0,0) ConstantBuffer<ViewValues> ViewData : register(b0);
VK_BINDING(1,0) ConstantBuffer<MaterialValues> Tint : register(b1);
VK_BINDING(2,0) StructuredBuffer<ObjectValues> Objects : register(t0);
VK_BINDING(3,0) StructuredBuffer<uint> VisibleInstanceSlots : register(t1);
VK_BINDING(4,0) StructuredBuffer<MaterialValues> InstanceData : register(t2);
VK_PUSH_CONSTANT ConstantBuffer<DrawValues> DrawData : register(b2);
#define VISIBLE_RS "DescriptorTable(CBV(b0),CBV(b1),SRV(t0,numDescriptors=3)),RootConstants(num32BitConstants=2,b2)"
struct Vertex { float4 Position : SV_Position; nointerpolation uint Slot : TEXCOORD0; };
[RootSignature(VISIBLE_RS)]
[shader("vertex")] Vertex VSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 positions[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
    Vertex v; v.Slot = VisibleInstanceSlots[DrawData.VisibleBase + instance];
    v.Position = mul(Objects[v.Slot].LocalToWorld, float4(positions[vertex], ViewData.Data.x, 1));
    return v;
}
[RootSignature(VISIBLE_RS)]
[shader("pixel")] float4 PSMain(Vertex v) : SV_Target {
    return DrawData.Tag == 7 ? InstanceData[v.Slot].Color * Tint.Color : float4(1,0,1,1);
}
)hlsl";
        if (scene) {
            const auto replace = [&](std::string_view from, std::string_view to) { const auto at = source.find(from); if (at != string::npos) source.replace(at, from.size(), to); };
            replace("struct ViewValues { float4 Data; };", "struct ViewValues { column_major float4x4 ViewProjection; };");
            replace("ViewData.Data.x", "0.5");
            replace("return v;", "v.Position = mul(ViewData.ViewProjection, v.Position); return v;");
        }
        if (objectPush) {
            source.insert(0, "#include <core/platform.hlsli>\nstruct DrawValues { uint ObjectSlot; };\nVK_PUSH_CONSTANT ConstantBuffer<DrawValues> DrawData : register(b2);\n#define PROBE_RS \"DescriptorTable(CBV(b0),CBV(b1)),RootConstants(num32BitConstants=1,b2)\"\n");
            const auto depth = source.find("ViewData.Data.x");
            source.replace(depth, string{"ViewData.Data.x"}.size(), "ViewData.Data.x + DrawData.ObjectSlot * 0.00001");
            for (std::string_view stage : {"vertex", "pixel"}) {
                const auto marker = fmt::format("[shader(\"{}\")]", stage);
                source.insert(source.find(marker), "[RootSignature(PROBE_RS)]\n");
            }
        }
        const vector<std::filesystem::path> includes{RADRAY_SHADERLIB_DIR};
        shader_compiler::Client compiler{RADRAY_RASTER_COMPILER_PATH};
        shader::SourceContractRequest discovery;
        discovery.SourceName = "native-raster.hlsl";
        const auto input = std::as_bytes(std::span{source.data(), source.size()});
        discovery.RootSource.assign(input.begin(), input.end());
        discovery.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(Backend() == render::RenderBackend::Vulkan ? shader::ShaderTarget::SPIRV : shader::ShaderTarget::DXIL));
        const auto contract = compiler.DiscoverSourceContract(discovery, includes);
        EXPECT_TRUE(contract.Succeeded());
        if (!contract.Succeeded()) return nullptr;
        shader::CompileVariantRequest request;
        request.SourceName = discovery.SourceName;
        request.RootSource = discovery.RootSource;
        request.Targets = discovery.Targets;
        request.ExpectedContract = contract.Contract.Hash;
        const auto compiled = compiler.CompileVariant(request, includes);
        EXPECT_EQ(compiled.Status, shader::CompileStatus::Success);
        if (compiled.Lanes.size() != 1) return nullptr;
        const auto& bytes = compiled.Lanes[0].Metadata;
        Metadata = bytes;
        shader::WireMetadataEnvelope envelope{};
        std::memcpy(&envelope, bytes.data(), sizeof(envelope));
        auto wrong = envelope.GpuArtifact;
        wrong.Bytes[0] ^= 1;
        EXPECT_FALSE(Renderer->GetOrCreateShaderProgram(bytes, wrong));
        return Renderer->GetOrCreateShaderProgram(bytes, envelope.GpuArtifact);
    }
    MaterialRenderData Material(ShaderProgram* program, bool textures, bool arrays = false) {
        MaterialPass pass;
        pass.Name = "Raster";
        pass.RegisteredProgram = program;
        pass.Primitive.Cull = render::CullMode::None;
        pass.Inputs = {{"ViewData", MaterialInputSource::ViewConstants}, {"Tint", MaterialInputSource::MaterialConstants}};
        if (textures) {
            if (arrays)
                pass.Inputs.push_back({"Images", MaterialInputSource::Texture, 0});
            else {
                pass.Inputs.push_back({"ImageA", MaterialInputSource::Texture, 0});
                pass.Inputs.push_back({"ImageB", MaterialInputSource::Texture, 1});
            }
            pass.Inputs.push_back({"SampleA", MaterialInputSource::Sampler, 0});
            pass.Inputs.push_back({"SampleB", MaterialInputSource::Sampler, 1});
        }
        return {{std::move(pass)}, {vector<byte>(16)}, {}, {}};
    }
    void Begin(uint32_t flight = 0) {
        Gpu->BeginUpdateForFlight(flight);
        Frame = Gpu->BeginFrameRecord(flight, {}, {}, false);
        if (Backend() == render::RenderBackend::Vulkan) {
            auto* commands = Frame->AllocateCommandBuffer();
            const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = Readback.get(), .Before = render::BufferState::Undefined, .After = render::BufferState::CopyDestination};
            commands->ResourceBarrier(std::span{&barrier, 1});
            Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        }
    }
    void ReadbackBarrier(render::CommandBuffer* commands) {
        if (Backend() == render::RenderBackend::Vulkan) {
            const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = Readback.get(), .Before = render::BufferState::CopyDestination, .After = render::BufferState::HostRead};
            commands->ResourceBarrier(std::span{&barrier, 1});
        }
    }
    void Complete(bool outputsExpected = true) {
        const auto flight = Frame->FlightIndex();
        Frame->SubmitFrame();
        const auto signal = Gpu->GetFlightGpuSignal(flight);
        ASSERT_TRUE(signal.Fence->WaitFor(signal.Value, 10000));
        ASSERT_TRUE(Gpu->CompleteFlightIfReady(flight, false));
        FlightCompletion completion;
        ASSERT_TRUE(Timeline.TryReadCompletion(completion));
        EXPECT_EQ(completion.OutputsExecuted, outputsExpected);
        Gpu->ReleaseFrameResourcesGT(completion);
        Timeline.CleanupCompletedFlights();
        Frame.reset();
    }
    render::GraphicsCommandEncoder* Start(render::CommandBuffer* commands, bool depth, float clearDepth = 1) {
        const render::RenderPassColorAttachment color{ColorView.get(), render::LoadAction::Clear, render::StoreAction::Store, {{0, 0, 0, 1}}};
        std::optional<render::RenderPassDepthStencilAttachment> d;
        if (depth) d = {DepthView.get(), render::LoadAction::Clear, render::StoreAction::Store, render::LoadAction::DontCare, render::StoreAction::Discard, false, {clearDepth, 0}};
        const render::ResourceBarrierDescriptor barriers[]{
            render::BarrierTextureDescriptor{.Target = Color.get(), .Before = render::TextureState::Undefined, .After = render::TextureState::RenderTarget},
            render::BarrierTextureDescriptor{.Target = Depth.get(), .Before = render::TextureState::Undefined, .After = render::TextureState::DepthWrite}};
        commands->ResourceBarrier(std::span{barriers, depth ? 2u : 1u});
        return commands->BeginRenderPass({std::span{&color, 1}, d, 16, 16, 1}).Unwrap().release();
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
        Readback->InvalidateMappedRange({0, 4096});
        vector<uint8_t> pixels(data, data + 4096);
        Readback->Unmap();
        return pixels;
    }
    PreparedRasterDraw Draw(render::GraphicsPipelineState* pipeline) {
        PreparedRasterDraw draw;
        draw.Pipeline = pipeline;
        draw.Count = 3;
        draw.Viewport = {0, 0, 16, 16, 0, 1};
        draw.Scissor = {0, 0, 16, 16};
        return draw;
    }
    void Mixed(bool arrays = false);
    void DepthStates(bool reverse = false);
    void Regions();
    void Instances();
    void PersistentDefaults(bool pure);
    void VisibleInstances();
    void Batching();
    void ProbePersistentGroups();
    void RetainedMaterial();
    void PushModes();
    void PresentExternal(bool hide) {
        Window = CreateWindowExW(0, L"STATIC", L"RadRay raster producer", WS_OVERLAPPEDWINDOW,
                                 40, 40, 160, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(Window, nullptr);
        ShowWindow(Window, SW_SHOW);
        UpdateWindow(Window);
        auto chain = Gpu->GetDevice()->CreateSwapChain({.PresentQueue = Gpu->GetMainQueue(), .NativeHandler = Window, .Width = 16, .Height = 16, .BackBufferCount = 2, .Format = render::TextureFormat::RGBA8_UNORM, .PresentMode = render::PresentMode::Immediate}).Unwrap();
        auto program = Program(false);
        ASSERT_TRUE(program);
        auto material = Material(program.Get(), false);
        auto plan = Core.ResolveMaterial(*Renderer, material.Passes[0]);
        ASSERT_TRUE(plan);
        Begin();
        auto context = MakePipelineContext(*Renderer, *Frame);
        const float view[]{0.25f, 0, 0, 0}, tint[]{1, 0, 0, 1};
        const std::span<const byte> views[]{std::as_bytes(std::span{view})}, constants[]{std::as_bytes(std::span{tint})};
        auto parameters = Core.PrepareMaterial(context, *plan.Get(), material, {.Constants = constants, .Views = views});
        ASSERT_TRUE(parameters);
        auto pipeline = Core.PreparePipeline(context, *plan.Get(), material.Passes[0], {}, render::PrimitiveTopology::TriangleList,
                                             {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}});
        ASSERT_TRUE(pipeline);
        auto* commands = Frame->AllocateCommandBuffer();
        auto* encoder = Start(commands, false);
        Draw(pipeline.Get()).Record(encoder, *parameters);
        commands->EndRenderPass(unique_ptr<render::GraphicsCommandEncoder>{encoder});
        const render::ResourceBarrierDescriptor source = render::BarrierTextureDescriptor{.Target = Color.get(), .Before = render::TextureState::RenderTarget, .After = render::TextureState::CopySource};
        commands->ResourceBarrier(std::span{&source, 1});
        commands->CopyTextureToBuffer(Readback.get(), 0, Color.get(), {0, 1, 0, 1});
        Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        auto acquired = chain->AcquireNext(1000);
        ASSERT_EQ(acquired.Status, render::SwapChainStatus::Success);
        uint32_t presents = 0;
        render::SwapChainStatus status = render::SwapChainStatus::Error;
        auto target = Frame->AdoptSwapChainFrame(chain.get(), std::move(*acquired.Frame), [this] { return IsWindowVisible(Window) != 0; }, [&](render::SwapChainPresentResult result) { ++presents; status = result.Status; });
        bool outputSubmitted = !hide;
        target.OnOutput = [&](const AppFrameOutputResult& result) { outputSubmitted = result.CommandsSubmitted; };
        auto* output = Frame->AllocateCommandBuffer();
        const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{.Target = target.BackBuffer, .Before = render::TextureState::Present, .After = render::TextureState::CopyDestination};
        output->ResourceBarrier(std::span{&before, 1});
        output->CopyTextureToTexture({.Destination = target.BackBuffer, .Source = Color.get(), .Width = 16, .Height = 16});
        const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{.Target = target.BackBuffer, .Before = render::TextureState::CopyDestination, .After = render::TextureState::Present};
        output->ResourceBarrier(std::span{&after, 1});
        Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&output, 1}}, std::move(target));
        if (hide) ShowWindow(Window, SW_HIDE);
        Complete(!hide);
        EXPECT_EQ(outputSubmitted, !hide);
        EXPECT_EQ(presents, 1u);
        EXPECT_EQ(status, hide ? render::SwapChainStatus::RetryLater : render::SwapChainStatus::Success);
        auto* pixels = static_cast<const uint8_t*>(Readback->Map(0, 4096));
        Readback->InvalidateMappedRange({0, 4096});
        EXPECT_EQ(pixels[8 * Pitch(16) + 8 * 4], 255);
        Readback->Unmap();
    }
};

TEST_F(RasterDraw, ExternalSurfacePresentsOnceAndRetiresOnTheFinalFence) { PresentExternal(false); }
TEST_F(RasterDraw, HiddenExternalSurfacePreservesIndependentApplicationWork) { PresentExternal(true); }

void RasterDraw::Mixed(bool arrays) {
    auto program = Program(true, arrays);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), true, arrays);
    auto& pass = material.Passes[0];
    auto plan = Core.ResolveMaterial(*Renderer, pass);
    ASSERT_TRUE(plan);
    auto missing = pass;
    missing.Inputs.pop_back();
    EXPECT_FALSE(Core.ResolveMaterial(*Renderer, missing));
    auto wrong = pass;
    wrong.Inputs[0].Source = MaterialInputSource::Texture;
    EXPECT_FALSE(Core.ResolveMaterial(*Renderer, wrong));
    Begin();
    auto context = MakePipelineContext(*Renderer, *Frame);
    vector<unique_ptr<render::Texture>> images;
    vector<unique_ptr<render::TextureView>> imageViews;
    for (uint32_t i = 0; i < 2; ++i) {
        images.push_back(Gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2D, 2, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                                          render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::CopyDestination})
                             .Unwrap());
        imageViews.push_back(Gpu->GetDevice()->CreateTextureView({images.back().get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap());
    }
    auto* commands = Frame->AllocateCommandBuffer();
    const uint32_t a[]{0xff0000ff, 0xffff0000}, b[]{0xff00ff00, 0xffff0000};
    const TextureRegionUploadRequest uploads[]{{std::as_bytes(std::span{a}), images[0].get(), {.Width = 2, .Height = 1}},
                                               {std::as_bytes(std::span{b}), images[1].get(), {.Width = 2, .Height = 1}}};
    ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(commands, uploads));
    render::TextureView* textures[]{imageViews[0].get(), imageViews[1].get()};
    render::SamplerDescriptor nearest{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
                                      render::FilterMode::Nearest, render::FilterMode::Nearest, render::FilterMode::Nearest, 0, 1};
    auto linear = nearest;
    linear.MinFilter = linear.MagFilter = render::FilterMode::Linear;
    const render::SamplerDescriptor samplers[]{nearest, linear};
    float view[]{0.25f, 0.4f, 0, 0}, tint[]{0, 0, 0, 1};
    const std::span<const byte> views[]{std::as_bytes(std::span{view})}, constants[]{std::as_bytes(std::span{tint})};
    RasterMaterialValues values{.Constants = constants, .Textures = textures, .Samplers = samplers, .Views = views};
    const std::span<const byte> tooSmall[]{std::as_bytes(std::span{view, size_t{1}})};
    auto invalid = values;
    invalid.Views = tooSmall;
    EXPECT_FALSE(Core.PrepareMaterial(context, *plan.Get(), material, invalid));
    auto badIndex = pass;
    badIndex.Inputs[2].ValueIndex = 2;
    auto badPlan = Core.ResolveMaterial(*Renderer, badIndex);
    ASSERT_TRUE(badPlan);
    EXPECT_FALSE(Core.PrepareMaterial(context, *badPlan.Get(), material, values));
    auto parameters = Core.PrepareMaterial(context, *plan.Get(), material, values);
    ASSERT_TRUE(parameters);
    const RenderOutputInfo output{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}};
    auto pipeline = Core.PreparePipeline(context, *plan.Get(), pass, {}, render::PrimitiveTopology::TriangleList, output);
    ASSERT_TRUE(pipeline);
    auto* encoder = Start(commands, false);
    auto draw = Draw(pipeline.Get());
    draw.Record(encoder, *parameters);
    tint[0] = 0.25f;
    auto second = Core.PrepareMaterial(context, *plan.Get(), material, values);
    ASSERT_TRUE(second);
    EXPECT_NE(second->Groups.back().Parameters, parameters->Groups.back().Parameters);
    EXPECT_EQ(Core.PreparePipeline(context, *plan.Get(), pass, {}, render::PrimitiveTopology::TriangleList, output).Get(), pipeline.Get());
    EXPECT_EQ(Renderer->GetGraphicsPipelineCache().GetSize(), 1u);
    auto indices = Gpu->GetDevice()->CreateBuffer({12, render::MemoryType::Upload, render::BufferUse::Index | render::BufferUse::MapWrite}).Unwrap();
    const uint32_t triangle[]{0, 1, 2};
    auto mapped = indices->Map(0, 12);
    std::memcpy(mapped, triangle, 12);
    indices->FlushMappedRange({0, 12});
    indices->Unmap();
    draw.Indices = render::IndexBufferView{indices.get(), 0, 4};
    draw.Scissor.Width = 8;
    draw.Record(encoder, *second);
    const auto pixels = Read(commands, encoder);
    const auto left = 8 * Pitch(16) + 4 * 4, right = 8 * Pitch(16) + 12 * 4;
    EXPECT_NEAR(pixels[left], 191, 1);
    EXPECT_NEAR(pixels[right], 128, 1);
    EXPECT_NEAR(pixels[right + 1], 89, 1);
    EXPECT_NEAR(pixels[right + 2], 38, 1);
    auto defaults = material;
    defaults.Textures.assign(textures, textures + 2);
    defaults.Samplers = {nearest, nearest};
    const array<float, 4> black{0, 0, 0, 1};
    const auto defaultBytes = std::as_bytes(std::span{black});
    defaults.Constants[0].assign(defaultBytes.begin(), defaultBytes.end());
    MaterialGpuState defaultGpu;
    Begin();
    auto nextContext = MakePipelineContext(*Renderer, *Frame);
    RasterMaterialValues defaultValues{.Views = views};
    auto original = Core.PrepareMaterial(nextContext, *plan.Get(), MaterialBindingView{defaults, defaultGpu}, defaultValues);
    ASSERT_TRUE(original);
    render::TextureView* greenImages[]{imageViews[1].get(), imageViews[1].get()};
    auto textureValues = defaultValues;
    textureValues.Textures = greenImages;
    auto textureOverride = Core.PrepareMaterial(nextContext, *plan.Get(), MaterialBindingView{defaults, defaultGpu}, textureValues);
    ASSERT_TRUE(textureOverride);
    const render::SamplerDescriptor temporarySamplers[]{linear, nearest};
    auto samplerValues = defaultValues;
    samplerValues.Samplers = temporarySamplers;
    auto samplerOverride = Core.PrepareMaterial(nextContext, *plan.Get(), MaterialBindingView{defaults, defaultGpu}, samplerValues);
    ASSERT_TRUE(samplerOverride);
    auto restored = Core.PrepareMaterial(nextContext, *plan.Get(), MaterialBindingView{defaults, defaultGpu}, defaultValues);
    ASSERT_TRUE(restored);
    EXPECT_EQ(original->Groups.back().Parameters, restored->Groups.back().Parameters);
    EXPECT_NE(original->Groups.back().Parameters, samplerOverride->Groups.back().Parameters);
    commands = Frame->AllocateCommandBuffer();
    encoder = Start(commands, false);
    draw.Indices.reset();
    draw.Scissor = {0, 0, 5, 16};
    draw.Record(encoder, *textureOverride);
    draw.Scissor = {5, 0, 5, 16};
    draw.Record(encoder, *samplerOverride);
    draw.Scissor = {10, 0, 6, 16};
    draw.Record(encoder, *restored);
    const auto defaultsPixels = Read(commands, encoder);
    EXPECT_EQ(defaultsPixels[Pitch(16) * 8 + 2 * 4], 0);
    EXPECT_EQ(defaultsPixels[Pitch(16) * 8 + 2 * 4 + 1], 255);
    EXPECT_NEAR(defaultsPixels[Pitch(16) * 8 + 7 * 4], 89, 1);
    EXPECT_NEAR(defaultsPixels[Pitch(16) * 8 + 7 * 4 + 2], 38, 1);
    EXPECT_NEAR(defaultsPixels[Pitch(16) * 8 + 13 * 4], 128, 1);
    EXPECT_NEAR(defaultsPixels[Pitch(16) * 8 + 13 * 4 + 1], 128, 1);
    EXPECT_EQ(defaultsPixels[Pitch(16) * 8 + 13 * 4 + 2], 0);
}

void RasterDraw::DepthStates(bool reverse) {
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    auto& pass = material.Passes[0];
    pass.DepthStencil = render::DepthStencilState::Default();
    pass.DepthStencil->DepthWriteEnable = false;
    pass.DepthStencil->DepthCompare = reverse ? render::CompareFunction::Greater : render::CompareFunction::Less;
    auto plan = Core.ResolveMaterial(*Renderer, pass);
    ASSERT_TRUE(plan);
    Begin();
    auto context = MakePipelineContext(*Renderer, *Frame);
    auto* commands = Frame->AllocateCommandBuffer();
    auto* encoder = Start(commands, true, reverse ? 0.0f : 1.0f);
    const RenderOutputInfo output{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, render::TextureFormat::D32_FLOAT};
    for (uint32_t i = 0; i < 4; ++i) {
        pass.DepthStencil->DepthWriteEnable = i >= 2;
        float view[]{(i % 2 != 0) == reverse ? 0.2f : 0.8f, 0, 0, 0}, tint[]{i == 0 || i == 3 ? 1.0f : 0, i == 1 ? 1.0f : 0, i == 2 ? 1.0f : 0, 1};
        const std::span<const byte> views[]{std::as_bytes(std::span{view})}, constants[]{std::as_bytes(std::span{tint})};
        auto parameters = Core.PrepareMaterial(context, *plan.Get(), material, {.Constants = constants, .Views = views});
        ASSERT_TRUE(parameters);
        auto pipeline = Core.PreparePipeline(context, *plan.Get(), pass, {}, render::PrimitiveTopology::TriangleList, output);
        ASSERT_TRUE(pipeline);
        Draw(pipeline.Get()).Record(encoder, *parameters);
    }
    EXPECT_EQ(Renderer->GetGraphicsPipelineCache().GetSize(), 2u);
    const auto pixels = Read(commands, encoder);
    EXPECT_EQ(pixels[8 * Pitch(16) + 8 * 4], 0);
    EXPECT_EQ(pixels[8 * Pitch(16) + 8 * 4 + 2], 255);
}

void RasterDraw::Regions() {
    Begin();
    auto* commands = Frame->AllocateCommandBuffer();
    auto texture = Gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2DArray, 8, 8, 2, 2, 1, render::TextureFormat::RGBA8_UNORM,
                                                    render::MemoryType::Device, render::TextureUse::CopySource | render::TextureUse::CopyDestination})
                       .Unwrap();
    const vector<uint32_t> initial(16, 0xff332211);
    TextureRegionUploadRequest request{std::as_bytes(std::span{initial}), texture.get(), {.MipLevel = 1, .ArrayLayer = 1, .Width = 4, .Height = 4}, 0, render::TextureState::Undefined, render::TextureState::CopySource};
    ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(commands, std::span{&request, 1}));
    const uint32_t patch[]{0xff998877, 0xff998877, 0, 0xff998877, 0xff998877};
    request = {std::as_bytes(std::span{patch}), texture.get(), {.MipLevel = 1, .ArrayLayer = 1, .X = 1, .Y = 1, .Width = 2, .Height = 2}, 12, render::TextureState::CopySource, render::TextureState::CopySource};
    auto invalid = request;
    invalid.Region.X = UINT32_MAX;
    const TextureRegionUploadRequest invalidBatch[]{request, invalid};
    EXPECT_FALSE(Frame->GetUploader().TryUploadTextureRegions(commands, invalidBatch));
    commands->CopyTextureToBuffer(Readback.get(), 0, texture.get(), {1, 1, 1, 1});
    invalid = request;
    invalid.SrcRowPitch = UINT64_MAX;
    EXPECT_FALSE(Frame->GetUploader().TryUploadTextureRegions(commands, std::span{&invalid, 1}));
    ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(commands, std::span{&request, 1}));
    commands->CopyTextureToBuffer(Readback.get(), 1024, texture.get(), {1, 1, 1, 1});
    ReadbackBarrier(commands);
    Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
    Complete();
    auto* pixels = static_cast<const uint8_t*>(Readback->Map(0, 4096));
    Readback->InvalidateMappedRange({0, 4096});
    EXPECT_EQ(pixels[Pitch(4) + 4], 0x11);
    EXPECT_EQ(pixels[1024], 0x11);
    EXPECT_EQ(pixels[1024 + Pitch(4) + 4], 0x77);
    EXPECT_EQ(pixels[1024 + 3 * Pitch(4) + 3 * 4], 0x11);
    Readback->Unmap();
}
void RasterDraw::Instances() {
    auto program = Program(false, false, true);
    ASSERT_TRUE(program);
    auto layout = ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Tint", "TintStream");
    ASSERT_TRUE(layout);
    RenderScene scene;
    radray::SceneApplyChanges changes;
    SceneUpdateBatch update;
    ShapeId id{3, 0};
    update.CreateShapes = {id};
    update.MeshStates = {{.Id = id}};
    const float red[]{1, 0, 0, 1};
    auto bytes = std::as_bytes(std::span{red});
    update.Parameters = {{id, *layout, vector<byte>{bytes.begin(), bytes.end()}}};
    scene.Apply(update, &changes);
    SceneGpuData mirror{Gpu->GetDevice(), 2};
    mirror.ApplyChanges(changes);
    Begin();
    auto objects = mirror.Prepare(scene, *Frame);
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->Parameters.size(), 1u);
    auto material = Material(program.Get(), false);
    const float white[]{1, 1, 1, 1};
    material.Constants[0].assign(std::as_bytes(std::span{white}).begin(), std::as_bytes(std::span{white}).end());
    material.Passes[0].ParameterLayouts = {*layout};
    material.Passes[0].Inputs.push_back({"InstanceData", MaterialInputSource::InstanceParameters});
    auto plan = Core.ResolveMaterial(*Renderer, material.Passes[0]);
    ASSERT_TRUE(plan);
    auto context = MakePipelineContext(*Renderer, *Frame);
    const float view[]{.5f, 0, 3, 0};
    const std::span<const byte> views[]{std::as_bytes(std::span{view})};
    auto prepared = Core.PrepareMaterial(context, *plan.Get(), material, {.Views = views, .Parameters = objects->Parameters});
    ASSERT_TRUE(prepared);
    auto wrong = objects->Parameters;
    auto changed = *layout;
    changed.Fields[0].Flags ^= 1;
    wrong[0].Layout = &changed;
    EXPECT_FALSE(Core.PrepareMaterial(context, *plan.Get(), material, {.Views = views, .Parameters = wrong}));
    wrong = objects->Parameters;
    wrong[0].Values.StructureByteStride = 32;
    EXPECT_FALSE(Core.PrepareMaterial(context, *plan.Get(), material, {.Views = views, .Parameters = wrong}));
    RenderOutputInfo output{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}};
    auto pipeline = Core.PreparePipeline(context, *plan.Get(), material.Passes[0], {}, render::PrimitiveTopology::TriangleList, output);
    ASSERT_TRUE(pipeline);
    auto* command = Frame->AllocateCommandBuffer();
    auto* encoder = Start(command, false);
    auto draw = Draw(pipeline.Get());
    draw.Record(encoder, *prepared);
    auto pixels = Read(command, encoder);
    EXPECT_EQ(pixels[Pitch(16) * 8 + 8 * 4], 255);
    EXPECT_EQ(pixels[Pitch(16) * 8 + 8 * 4 + 1], 0);
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
TEST_F(RasterDraw, IndependentNativeProducerReadsTypedSparseInstanceParametersOnTheGpu) { Instances(); }
TEST_F(VulkanRasterDraw, IndependentNativeProducerReadsTypedSparseInstanceParametersOnTheGpu) { Instances(); }
void RasterDraw::PersistentDefaults(bool pure) {
    auto program = Program(false, false, false, pure);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    const array<float, 4> red{1, 0, 0, 1}, green{0, 1, 0, 1};
    const auto redBytes = std::as_bytes(std::span{red});
    material.Constants[0].assign(redBytes.begin(), redBytes.end());
    MaterialGpuState gpu;
    auto plan = Core.ResolveMaterial(*Renderer, material.Passes[0]);
    ASSERT_TRUE(plan);
    Nullable<render::ShaderParameterSet*> fixed{nullptr};
    const auto tintGroup = program->GetArtifact().FindBindingInfo("Tint")->Group;
    for (uint32_t frame = 0; frame < 100; ++frame) {
        Begin(frame % 2);
        auto context = MakePipelineContext(*Renderer, *Frame);
        for (uint32_t view = 0; view < 3; ++view) {
            const array<float, 4> camera{.5f, float(view), 0, 0};
            const std::span<const byte> views[]{std::as_bytes(std::span{camera})};
            const auto before = Frame->GetHostWrites().GetStats().CommittedBytes;
            auto prepared = Core.PrepareMaterial(context, *plan.Get(), MaterialBindingView{material, gpu}, {.Views = views});
            ASSERT_TRUE(prepared);
            EXPECT_EQ(Frame->GetHostWrites().GetStats().CommittedBytes - before, 16u);
            const auto group = std::find_if(prepared->Groups.begin(), prepared->Groups.end(), [&](const auto& g) { return g.Index == tintGroup; });
            ASSERT_NE(group, prepared->Groups.end());
            if (pure) {
                if (fixed)
                    EXPECT_EQ(group->Parameters, fixed.Get());
                else
                    fixed = group->Parameters;
            }
            if (frame == 99 && view == 2) {
                const std::span<const byte> overrides[]{std::as_bytes(std::span{green})};
                auto changed = Core.PrepareMaterial(context, *plan.Get(), MaterialBindingView{material, gpu}, {.Constants = overrides, .Views = views});
                ASSERT_TRUE(changed);
                auto restored = Core.PrepareMaterial(context, *plan.Get(), MaterialBindingView{material, gpu}, {.Views = views});
                ASSERT_TRUE(restored);
                const auto output = RenderOutputInfo{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}};
                auto pipeline = Core.PreparePipeline(context, *plan.Get(), material.Passes[0], {}, render::PrimitiveTopology::TriangleList, output);
                ASSERT_TRUE(pipeline);
                auto* commands = Frame->AllocateCommandBuffer();
                auto* encoder = Start(commands, false);
                auto draw = Draw(pipeline.Get());
                draw.Scissor.Width = 8;
                draw.Record(encoder, *changed);
                draw.Scissor.X = 8;
                draw.Record(encoder, *restored);
                auto pixels = Read(commands, encoder);
                EXPECT_EQ(pixels[Pitch(16) * 8 + 4 * 4], 0);
                EXPECT_EQ(pixels[Pitch(16) * 8 + 4 * 4 + 1], 255);
                EXPECT_EQ(pixels[Pitch(16) * 8 + 12 * 4], 255);
                EXPECT_EQ(pixels[Pitch(16) * 8 + 12 * 4 + 1], 0);
            }
        }
        if (Frame) Complete();
    }
}
TEST_F(RasterDraw, PersistentPureDefaultsSurviveViewsFlightsAndTemporaryOverride) { PersistentDefaults(true); }
TEST_F(VulkanRasterDraw, PersistentPureDefaultsSurviveViewsFlightsAndTemporaryOverride) { PersistentDefaults(true); }
TEST_F(RasterDraw, PersistentConstantsInMixedGroupsSurviveViewsFlightsAndTemporaryOverride) { PersistentDefaults(false); }
TEST_F(VulkanRasterDraw, PersistentConstantsInMixedGroupsSurviveViewsFlightsAndTemporaryOverride) { PersistentDefaults(false); }
void RasterDraw::ProbePersistentGroups() {
    ASSERT_TRUE(Program(false, false, false, true));
    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, Metadata.data(), sizeof(envelope));
    const shader::ShaderArtifactDecodeOptions options{Backend() == render::RenderBackend::Vulkan ? shader::ShaderTarget::SPIRV : shader::ShaderTarget::DXIL, envelope.GpuArtifact};
    auto artifact = render::CreateBackendShaderArtifact(*Gpu->GetDevice(), Metadata, options);
    ASSERT_TRUE(artifact);
    MaterialBindingProbe counts;
    MaterialProbeDevice device{Gpu->GetDevice(), counts};
    RenderSystem renderer{&device};
    ASSERT_TRUE(renderer.OnInitialize());
    ShaderProgram program{&device, std::move(*artifact), nullptr, {}, nullptr, {}, nullptr, {}};
    auto data = Material(&program, false);
    radray::RasterDraw core;
    auto owner = make_unique<MaterialGpuState>();
    auto plan = core.ResolveMaterial(renderer, data.Passes[0]);
    ASSERT_TRUE(plan);
    for (uint32_t frame = 0; frame < 100; ++frame) {
        Begin(frame % 3);
        GpuFrameResources resources{&device};
        resources.BeginEpoch();
        auto native = MakePipelineContext(renderer, *Frame);
        PipelineContext context{renderer, native.Frame, native.Commands, resources};
        for (uint32_t view = 0; view < 3; ++view) {
            const array<float, 4> camera{.5f, float(frame), float(view), 0};
            const std::span<const byte> views[]{std::as_bytes(std::span{camera})};
            ASSERT_TRUE(core.PrepareMaterial(context, *plan.Get(), data, {.Views = views}, owner.get()));
        }
        resources.SealEpoch();
        Complete();
        resources.CompleteEpoch();
        EXPECT_EQ(counts.Groups[1].Created, 1u);
        EXPECT_EQ(counts.Groups[1].Set, 1u);
        EXPECT_EQ(counts.Groups[1].Flush, 1u);
    }
    EXPECT_EQ(counts.Groups[1].Live, 1u);
    {
        ASSERT_TRUE(Program(false));
        shader::WireMetadataEnvelope secondEnvelope{};
        std::memcpy(&secondEnvelope, Metadata.data(), sizeof(secondEnvelope));
        auto mixedArtifact = render::CreateBackendShaderArtifact(*Gpu->GetDevice(), Metadata, {options.Target, secondEnvelope.GpuArtifact});
        ASSERT_TRUE(mixedArtifact);
        ShaderProgram mixed{&device, std::move(*mixedArtifact), nullptr, {}, nullptr, {}, nullptr, {}};
        auto multi = data;
        multi.Passes.push_back(multi.Passes[0]);
        multi.Passes[1].Name = "Mixed";
        multi.Passes[1].RegisteredProgram = &mixed;
        MaterialGpuState multiOwner;
        auto purePlan = core.ResolveMaterial(renderer, multi.Passes[0]);
        auto mixedPlan = core.ResolveMaterial(renderer, multi.Passes[1]);
        ASSERT_TRUE(purePlan);
        ASSERT_TRUE(mixedPlan);
        Begin();
        auto nativeContext = MakePipelineContext(renderer, *Frame);
        GpuFrameResources multiResources{&device};
        multiResources.BeginEpoch();
        PipelineContext multiContext{renderer, nativeContext.Frame, nativeContext.Commands, multiResources};
        const array<float, 4> camera{.5f, 0, 0, 0};
        const std::span<const byte> views[]{std::as_bytes(std::span{camera})};
        auto pureBinding = core.PrepareMaterial(multiContext, *purePlan.Get(), multi, {.Views = views}, &multiOwner, &multi.Passes[0]);
        auto mixedBinding = core.PrepareMaterial(multiContext, *mixedPlan.Get(), multi, {.Views = views}, &multiOwner, &multi.Passes[1]);
        ASSERT_TRUE(pureBinding);
        ASSERT_TRUE(mixedBinding);
        EXPECT_EQ(pureBinding->Groups.size(), 2u);
        EXPECT_EQ(mixedBinding->Groups.size(), 1u);
        EXPECT_NE(purePlan->Program->GetPipelineLayout(), mixedPlan->Program->GetPipelineLayout());
        multiResources.SealEpoch();
        Complete();
        multiResources.CompleteEpoch();
    }
    const auto createdBeforeReplacement = counts.Groups[1].Created;
    RenderSystem other{&device};
    ASSERT_TRUE(other.OnInitialize());
    radray::RasterDraw otherCore;
    auto foreign = otherCore.ResolveMaterial(other, data.Passes[0]);
    ASSERT_TRUE(foreign);
    Begin();
    GpuFrameResources resources{&device};
    resources.BeginEpoch();
    auto native = MakePipelineContext(other, *Frame);
    PipelineContext context{other, native.Frame, native.Commands, resources};
    const array<float, 4> camera{.5f, 0, 0, 0};
    const std::span<const byte> views[]{std::as_bytes(std::span{camera})};
    EXPECT_FALSE(otherCore.PrepareMaterial(context, *foreign.Get(), data, {.Views = views}, owner.get()));
    PipelineContext oldContext{renderer, native.Frame, native.Commands, resources};
    auto preserved = core.PrepareMaterial(oldContext, *plan.Get(), data, {.Views = views}, owner.get());
    ASSERT_TRUE(preserved);
    const auto frameSetsCreated = counts.Groups[0].Created;
    const auto frameSetsLive = counts.Groups[0].Live;
    EXPECT_FALSE(otherCore.PrepareMaterial(context, *foreign.Get(), data, {.Views = views}));
    ASSERT_EQ(counts.Groups[0].Created, frameSetsCreated);
    ASSERT_EQ(counts.Groups[0].Live, frameSetsLive);
    EXPECT_TRUE(preserved->Groups[0].Parameters->FlushWrites());
    resources.SealEpoch();
    Complete();
    resources.CompleteEpoch();
    renderer.OnShutdown();
    Begin();
    auto nextNative = MakePipelineContext(other, *Frame);
    GpuFrameResources nextResources{&device};
    nextResources.BeginEpoch();
    PipelineContext nextContext{other, nextNative.Frame, nextNative.Commands, nextResources};
    ASSERT_TRUE(otherCore.PrepareMaterial(nextContext, *foreign.Get(), data, {.Views = views}, owner.get()));
    EXPECT_EQ(counts.Groups[1].Created, createdBeforeReplacement + 1);
    EXPECT_EQ(counts.Groups[1].Live, 1u);
    nextResources.SealEpoch();
    Complete();
    nextResources.CompleteEpoch();
    owner.reset();
    EXPECT_EQ(counts.Groups[1].Live, 0u);
}
TEST_F(RasterDraw, ProbeShowsNoStaticGroupWritesAfterWarmupAndSafeServiceReplacement) { ProbePersistentGroups(); }
TEST_F(VulkanRasterDraw, ProbeShowsNoStaticGroupWritesAfterWarmupAndSafeServiceReplacement) { ProbePersistentGroups(); }
void RasterDraw::RetainedMaterial() {
    auto program = Program(false, false, false, true);
    ASSERT_TRUE(program);
    auto data = Material(program.Get(), false);
    const array<float, 4> red{1, 0, 0, 1};
    const auto bytes = std::as_bytes(std::span{red});
    data.Constants[0].assign(bytes.begin(), bytes.end());
    AssetManager assets;
    const AssetId id{920, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    auto material = assets.AddReady<radray::Material>(id, make_unique<radray::Material>(data.Passes, data.Constants));
    Gpu->BeginUpdateForFlight(0);
    Gpu->RetainForFrameGT(0, material);
    Frame = Gpu->BeginFrameRecord(0, {}, {}, false);
    auto context = MakePipelineContext(*Renderer, *Frame);
    auto plan = Core.ResolveMaterial(*Renderer, material->GetRenderData().Passes[0]);
    ASSERT_TRUE(plan);
    const array<float, 4> camera{.5f, 0, 0, 0};
    const std::span<const byte> views[]{std::as_bytes(std::span{camera})};
    auto binding = Core.PrepareMaterial(context, *plan.Get(), MaterialBindingView{material->GetRenderData(), material->GetGpuState()}, {.Views = views});
    ASSERT_TRUE(binding);
    auto pipeline = Core.PreparePipeline(context, *plan.Get(), material->GetRenderData().Passes[0], {}, render::PrimitiveTopology::TriangleList,
                                         {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}});
    ASSERT_TRUE(pipeline);
    material.Reset();
    assets.Pump();
    EXPECT_TRUE(assets.Find<radray::Material>(id));
    auto* commands = Frame->AllocateCommandBuffer();
    if (Backend() == render::RenderBackend::Vulkan) {
        const render::ResourceBarrierDescriptor before = render::BarrierBufferDescriptor{.Target = Readback.get(), .Before = render::BufferState::Undefined, .After = render::BufferState::CopyDestination};
        commands->ResourceBarrier(std::span{&before, 1});
    }
    auto* encoder = Start(commands, false);
    Draw(pipeline.Get()).Record(encoder, *binding);
    const auto pixels = Read(commands, encoder);
    EXPECT_EQ(pixels[Pitch(16) * 8 + 8 * 4], 255);
    assets.Pump();
    EXPECT_FALSE(assets.Find<radray::Material>(id));
}
TEST_F(RasterDraw, LastNativeMaterialReferenceRetiresAfterItsRealGpuUse) { RetainedMaterial(); }
TEST_F(VulkanRasterDraw, LastNativeMaterialReferenceRetiresAfterItsRealGpuUse) { RetainedMaterial(); }
void RasterDraw::PushModes() {
    auto program = Program(false, false, false, false, false, false, true);
    ASSERT_TRUE(program);
    auto data = Material(program.Get(), false);
    auto legacy = data.Passes[0];
    legacy.ObjectIndexPushConstant = "DrawData";
    auto general = legacy;
    general.ObjectIndexPushConstant.clear();
    general.PushConstant = "DrawData";
    auto oldPlan = Core.ResolveMaterial(*Renderer, legacy);
    auto nativePlan = Core.ResolveMaterial(*Renderer, general);
    ASSERT_TRUE(oldPlan);
    ASSERT_TRUE(nativePlan);
    EXPECT_NE(oldPlan.Get(), nativePlan.Get());
    EXPECT_EQ(oldPlan->SceneIndexOffset, 0u);
    EXPECT_FALSE(nativePlan->SceneIndexOffset);
    radray::RasterDraw reverse;
    auto first = reverse.ResolveMaterial(*Renderer, general);
    auto second = reverse.ResolveMaterial(*Renderer, legacy);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_FALSE(first->SceneIndexOffset);
    EXPECT_EQ(second->SceneIndexOffset, 0u);
    general.SceneIndexField = "ObjectSlot";
    auto explicitPlan = Core.ResolveMaterial(*Renderer, general);
    ASSERT_TRUE(explicitPlan);
    EXPECT_EQ(explicitPlan->SceneIndexOffset, 0u);
}
TEST_F(RasterDraw, GeneralPushDoesNotInheritTheCachedLegacyObjectSlotMapping) { PushModes(); }
TEST_F(VulkanRasterDraw, GeneralPushDoesNotInheritTheCachedLegacyObjectSlotMapping) { PushModes(); }
void RasterDraw::VisibleInstances() {
    auto program = Program(false, false, false, false, true);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    const auto layout = ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Tint", "TintStream");
    ASSERT_TRUE(layout);
    auto& pass = material.Passes[0];
    pass.Inputs.push_back({"Objects", MaterialInputSource::SceneObjects});
    pass.Inputs.push_back({"VisibleInstanceSlots", MaterialInputSource::VisibleInstanceSlots});
    pass.Inputs.push_back({"InstanceData", MaterialInputSource::InstanceParameters});
    pass.ParameterLayouts = {*layout};
    pass.InstanceAddressing = MaterialInstanceAddressing::VisibleSlots;
    pass.PushConstant = "DrawData";
    pass.SceneIndexField = "VisibleBase";
    pass.ScenePushValues.resize(8);
    const uint32_t tag = 7;
    std::memcpy(pass.ScenePushValues.data(), &tag, 4);
    auto plan = Core.ResolveMaterial(*Renderer, pass);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->SceneIndexOffset, 4u);
    auto invalid = pass;
    invalid.SceneIndexField = "Missing";
    EXPECT_FALSE(Core.ResolveMaterial(*Renderer, invalid));
    vector<Eigen::Matrix4f> matrices(208, Eigen::Matrix4f::Identity());
    vector<byte> tints(208 * 16);
    const array<uint32_t, 3> used{8, 41, 207};
    for (size_t i = 0; i < used.size(); ++i) {
        auto& matrix = matrices[used[i]];
        matrix(0, 0) = matrix(1, 1) = 0.2f;
        matrix(0, 3) = -0.6f + float(i) * 0.6f;
        array<float, 4> tint{};
        tint[i] = tint[3] = 1;
        std::memcpy(tints.data() + used[i] * 16, tint.data(), 16);
    }
    const array<float, 4> white{1, 1, 1, 1};
    auto whiteBytes = std::as_bytes(std::span{white});
    material.Constants[0].assign(whiteBytes.begin(), whiteBytes.end());
    Begin();
    auto context = MakePipelineContext(*Renderer, *Frame);
    const array<uint32_t, 5> slots{207, 8, 41, 207, 41};
    const RasterFrameParameters parameters{*layout, tints};
    auto data = PrepareRasterInstances(context, *plan.Get(), matrices, slots, std::span{&parameters, 1});
    ASSERT_EQ(data.Status, RasterInstancePrepareStatus::Prepared);
    ASSERT_TRUE(data.Data);
    data = PrepareRasterInstances(context, *plan.Get(), matrices, slots, std::span{&parameters, 1});
    ASSERT_EQ(data.Status, RasterInstancePrepareStatus::Prepared);
    ASSERT_EQ(data.Data->Objects.Range.Offset % 64, 0u);
    RasterInstanceData copied{*data.Data};
    EXPECT_EQ(copied.Parameters[0].Layout, &copied.Layouts[0]);
    data.Data.reset();
    data.Data = std::move(copied);
    auto oddStride = context.Resources.AllocateStorage(144, 48);
    ASSERT_TRUE(oddStride.IsValid());
    EXPECT_EQ(oddStride.Offset() % 48, 0u);
    oddStride.Commit(0);
    matrices.clear();
    matrices.shrink_to_fit();
    tints.clear();
    tints.shrink_to_fit();
    const array<float, 4> view{.5f, 0, 0, 0};
    const std::span<const byte> views[]{std::as_bytes(std::span{view})};
    MaterialGpuState gpu;
    auto prepared = Core.PrepareMaterial(context, *plan.Get(), MaterialBindingView{material, gpu},
                                         {.Views = views, .Objects = data.Data->Objects, .VisibleSlots = data.Data->VisibleSlots, .Parameters = data.Data->Parameters});
    ASSERT_TRUE(prepared);
    const RenderOutputInfo output{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}};
    auto pipeline = Core.PreparePipeline(context, *plan.Get(), pass, {}, render::PrimitiveTopology::TriangleList, output);
    ASSERT_TRUE(pipeline);
    auto* commands = Frame->AllocateCommandBuffer();
    auto* encoder = Start(commands, false);
    auto draw = Draw(pipeline.Get());
    draw.Push = pass.ScenePushValues;
    const uint32_t base = 1;
    std::memcpy(draw.Push.data() + *plan->SceneIndexOffset, &base, 4);
    draw.Instances = 2;
    draw.Record(encoder, *prepared);
    const uint32_t secondBase = 3;
    std::memcpy(draw.Push.data() + *plan->SceneIndexOffset, &secondBase, 4);
    draw.Instances = 1;
    draw.Record(encoder, *prepared);
    auto pixels = Read(commands, encoder);
    const array<uint32_t, 3> columns{3, 8, 13};
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto pixel = Pitch(16) * 8 + columns[i] * 4;
        for (size_t color = 0; color < 3; ++color) EXPECT_EQ(pixels[pixel + color], i == color ? 255 : 0);
    }
}
TEST_F(RasterDraw, SparseVisibleSlotsUseNonzeroMappedBaseAfterSourceArraysAreReleased) { VisibleInstances(); }
TEST_F(VulkanRasterDraw, SparseVisibleSlotsUseNonzeroMappedBaseAfterSourceArraysAreReleased) { VisibleInstances(); }
void RasterDraw::Batching() {
    auto program = Program(false, false, false, false, true, true);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    auto& contract = material.Passes[0];
    contract.Inputs.push_back({"Objects", MaterialInputSource::SceneObjects});
    contract.Inputs.push_back({"VisibleInstanceSlots", MaterialInputSource::VisibleInstanceSlots});
    contract.Inputs.push_back({"InstanceData", MaterialInputSource::InstanceParameters});
    auto layout = ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Tint", "TintStream");
    ASSERT_TRUE(layout);
    contract.ParameterLayouts = {*layout};
    contract.InstanceAddressing = MaterialInstanceAddressing::VisibleSlots;
    contract.AllowAutomaticInstancing = true;
    contract.PushConstant = "DrawData";
    contract.SceneIndexField = "VisibleBase";
    contract.ScenePushValues.resize(8);
    const uint32_t tag = 7;
    std::memcpy(contract.ScenePushValues.data(), &tag, 4);
    const array<float, 4> white{1, 1, 1, 1};
    const auto whiteBytes = std::as_bytes(std::span{white});
    material.Constants[0].assign(whiteBytes.begin(), whiteBytes.end());
    AssetManager assets;
    SceneManager scenes{2};
    auto meshFactory = [&](uint32_t assetId, vector<StaticMeshSection> sections) {
        MeshResource cpu;
        const array<float, 9> vertices{-1, -1, .5f, -1, 3, .5f, 3, -1, .5f};
        const array<uint32_t, 9> indices{0, 1, 2, 0, 1, 2, 0, 1, 2};
        cpu.Bins.emplace_back(std::as_bytes(std::span{vertices}));
        cpu.Bins.emplace_back(std::as_bytes(std::span{indices}));
        MeshPrimitive primitive;
        primitive.VertexCount = 3;
        primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
        primitive.IndexBuffer = {1, 9, 0, 4};
        cpu.Primitives.push_back(primitive);
        GpuMesh gpu;
        gpu.Buffers.push_back(Gpu->GetDevice()->CreateBuffer({36, render::MemoryType::Upload, render::BufferUse::Index | render::BufferUse::MapWrite}).Unwrap());
        auto mapped = gpu.Buffers[0]->Map(0, 36);
        std::memcpy(mapped, indices.data(), 36);
        gpu.Buffers[0]->FlushMappedRange({0, 36});
        gpu.Buffers[0]->Unmap();
        GpuMesh::DrawData draw;
        draw.Ibv = {gpu.Buffers[0].get(), 0, 4};
        gpu.Draws.push_back(std::move(draw));
        return assets.AddReady<StaticMesh>({assetId, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
                                           make_unique<StaticMesh>(std::move(cpu), std::move(sections), Eigen::Vector3f{-1, -1, .5f}, Eigen::Vector3f{3, 3, .5f}, std::move(gpu)));
    };
    auto mesh = meshFactory(900, {{0, 0, 3, 0, 2}});
    const auto sceneId = scenes.CreateSceneGT();
    auto writer = scenes.GetSceneWriterGT(sceneId);
    vector<ShapeId> ids(10000);
    ASSERT_TRUE(writer->CreateShapes(ids));
    vector<Eigen::Matrix4f> transforms(ids.size(), Eigen::Matrix4f::Identity());
    for (auto& transform : transforms) {
        transform(0, 0) = .2f;
        transform(1, 1) = .2f;
    }
    writer->SetStaticMeshes(ids, mesh, transforms);
    vector<byte> tints(ids.size() * 16);
    for (size_t i = 0; i < ids.size(); ++i) std::memcpy(tints.data() + i * 16, white.data(), 16);
    ASSERT_TRUE(writer->SetInstanceParameters(ids, *layout, tints));
    radray::SceneDraw builder;
    MaterialGpuState gpuState;
    auto start = [&](uint32_t flight) {
        scenes.SealFrameGT(flight);
        scenes.PublishFrameGT(flight);
        Begin(flight);
        scenes.ConsumeRenderUpdates(flight, Frame->FrameSerial());
    };
    auto finish = [&] {
        const auto flight = Frame->FlightIndex();
        const auto serial = Frame->FrameSerial();
        Complete();
        scenes.OnFlightCompletedGT({.FlightIndex = flight, .OutputsExecuted = true, .FrameSerial = serial});
    };
    SceneViewRequest view;
    view.Scene = sceneId;
    view.EvaluatedProjection = Eigen::Matrix4f::Identity();
    const RenderOutputInfo output{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}};
    start(0);
    auto objects = scenes.PrepareSceneGpuRT(sceneId, *Frame);
    ASSERT_TRUE(objects);
    auto context = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto first = builder.Prepare(context, view, *objects, output, "Raster", &material, false, &gpuState);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->Draws.size(), 1u);
    EXPECT_EQ(first->Draws[0].Draw.Instances, 10000u);
    EXPECT_EQ(first->Draws[0].Draw.FirstInstance, 0u);
    const auto* templateAddress = scenes.GetSceneRT(sceneId)->GetDrawPrimitives(ids[0]).data();
    for (uint32_t i = 1; i < 3; ++i) {
        auto other = view;
        other.View(0, 3) = float(i) * .05f;
        const auto next = builder.Prepare(context, other, *objects, output, "Raster", &material, false, &gpuState);
        ASSERT_TRUE(next);
        EXPECT_EQ(next->Draws.size(), 1u);
        EXPECT_NE(next->Materials[0].Groups[0].Parameters, first->Materials[0].Groups[0].Parameters);
    }
    auto* commands = Frame->AllocateCommandBuffer();
    auto* encoder = Start(commands, false);
    first->Record(encoder);
    const auto serial = Frame->FrameSerial();
    const auto pixels = Read(commands, encoder);
    EXPECT_EQ(pixels[Pitch(16) * 8 + 8 * 4], 255);
    scenes.OnFlightCompletedGT({.FlightIndex = 0, .OutputsExecuted = true, .FrameSerial = serial});
    vector<ShapeTransformUpdate> updates;
    for (size_t i = 0; i < 5000; ++i) {
        transforms[i](0, 0) *= -1;
        updates.push_back({ids[i], transforms[i]});
    }
    writer->SetTransforms(updates);
    start(1);
    objects = scenes.PrepareSceneGpuRT(sceneId, *Frame);
    ASSERT_TRUE(objects);
    auto splitContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    EXPECT_EQ(scenes.GetSceneRT(sceneId)->GetDrawPrimitives(ids[0]).data(), templateAddress);
    auto split = builder.Prepare(splitContext, view, *objects, output, "Raster", &material, false, &gpuState);
    ASSERT_TRUE(split);
    ASSERT_EQ(split->Draws.size(), 2u);
    EXPECT_EQ(split->Draws[0].Draw.Instances, 5000u);
    EXPECT_EQ(split->Draws[1].Draw.Instances, 5000u);
    EXPECT_NE(split->Draws[0].Draw.Pipeline, split->Draws[1].Draw.Pipeline);
    finish();
    transforms[0](0, 3) = 50;
    writer->SetTransform(ids[0], transforms[0]);
    writer->SetDrawState(ids[1], {.VisibilityMask = 0});
    auto disabled = material;
    disabled.Passes[0].AllowAutomaticInstancing = false;
    MaterialGpuState disabledGpu;
    start(0);
    objects = scenes.PrepareSceneGpuRT(sceneId, *Frame);
    ASSERT_TRUE(objects);
    auto unmergedContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto unmerged = builder.Prepare(unmergedContext, view, *objects, output, "Raster", &disabled, false, &disabledGpu);
    ASSERT_TRUE(unmerged);
    EXPECT_EQ(unmerged->Draws.size(), 9998u);
    EXPECT_EQ(unmerged->Stats.MaskCulled, 1u);
    EXPECT_EQ(unmerged->Stats.BoundsCulled, 1u);
    EXPECT_EQ(unmerged->Stats.AutomaticDisabled, 9998u);
    finish();
    auto sections = meshFactory(901, {{0, 0, 3, 0, 2}, {0, 3, 6, 0, 2}, {0, 0, 3, 0, 2, 1}});
    writer->SetStaticMeshes(ids, sections, transforms);
    start(1);
    objects = scenes.PrepareSceneGpuRT(sceneId, *Frame);
    ASSERT_TRUE(objects);
    auto rangedContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto ranged = builder.Prepare(rangedContext, view, *objects, output, "Raster", &material, false, &gpuState);
    ASSERT_TRUE(ranged);
    EXPECT_EQ(ranged->Draws.size(), 6u);
    finish();
    vector<ShapeDrawUpdate> explicitStates;
    for (size_t i = 0; i < ids.size(); ++i) explicitStates.push_back({ids[i], {.VisibilityMask = i == 1 ? 0u : UINT32_MAX, .BatchDomain = 1u + i % 2, .ExplicitInstancing = true}});
    writer->SetDrawStates(explicitStates);
    start(0);
    objects = scenes.PrepareSceneGpuRT(sceneId, *Frame);
    ASSERT_TRUE(objects);
    auto explicitContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto explicitBatches = builder.Prepare(explicitContext, view, *objects, output, "Raster", &disabled, false, &disabledGpu);
    ASSERT_TRUE(explicitBatches);
    EXPECT_EQ(explicitBatches->Draws.size(), 12u);
    EXPECT_EQ(explicitBatches->Stats.AutomaticDisabled, 0u);
    finish();
    auto directProgram = Program(false, false, false, true);
    ASSERT_TRUE(directProgram);
    auto direct = Material(directProgram.Get(), false);
    MaterialGpuState directGpu;
    start(1);
    objects = scenes.PrepareSceneGpuRT(sceneId, *Frame);
    ASSERT_TRUE(objects);
    auto directContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto unsupported = builder.Prepare(directContext, view, *objects, output, "Raster", &direct, false, &directGpu);
    ASSERT_TRUE(unsupported);
    EXPECT_TRUE(unsupported->Draws.empty());
    EXPECT_EQ(unsupported->Stats.ExplicitUnsupported, 29994u);
    auto directPlan = Core.ResolveMaterial(*Renderer, direct.Passes[0]);
    ASSERT_TRUE(directPlan);
    const uint32_t visible = 0;
    EXPECT_EQ(PrepareRasterInstances(directContext, *directPlan.Get(), std::span{transforms}.first(1), std::span{&visible, 1}).Status, RasterInstancePrepareStatus::Unsupported);
    finish();
    const auto otherScene = scenes.CreateSceneGT();
    auto otherWriter = scenes.GetSceneWriterGT(otherScene);
    const auto otherId = otherWriter->CreateShape();
    otherWriter->SetStaticMesh(otherId, mesh, transforms[2]);
    ASSERT_TRUE(otherWriter->SetInstanceParameters(otherId, *layout, whiteBytes));
    start(0);
    const auto* originalTemplate = scenes.GetSceneRT(sceneId)->GetDrawPrimitives(ids[2]).data();
    auto alternateView = view;
    alternateView.Scene = otherScene;
    auto alternateObjects = scenes.PrepareSceneGpuRT(otherScene, *Frame);
    ASSERT_TRUE(alternateObjects);
    auto alternateContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto alternate = builder.Prepare(alternateContext, alternateView, *alternateObjects, output, "Raster", &material, false, &gpuState);
    ASSERT_TRUE(alternate);
    ASSERT_EQ(alternate->Draws.size(), 1u);
    EXPECT_EQ(alternate->Draws[0].Draw.Instances, 1u);
    EXPECT_EQ(scenes.GetSceneRT(sceneId)->GetDrawPrimitives(ids[2]).data(), originalTemplate);
    finish();
    scenes.DestroySceneGT(otherScene);
    start(1);
    EXPECT_FALSE(scenes.GetSceneRT(otherScene));
    auto emptyContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    UnlitRenderPipeline emptyPipeline;
    EXPECT_EQ(emptyPipeline.Record(emptyContext, {}).Status, PipelineRecordStatus::NoWork);
    finish();
    const auto reconnected = scenes.CreateSceneGT();
    EXPECT_EQ(reconnected.Index, otherScene.Index);
    EXPECT_NE(reconnected.Generation, otherScene.Generation);
    auto replacementWriter = scenes.GetSceneWriterGT(reconnected);
    const auto replacementId = replacementWriter->CreateShape();
    replacementWriter->SetStaticMesh(replacementId, sections, transforms[2]);
    ASSERT_TRUE(replacementWriter->SetInstanceParameters(replacementId, *layout, whiteBytes));
    start(0);
    alternateView.Scene = reconnected;
    alternateObjects = scenes.PrepareSceneGpuRT(reconnected, *Frame);
    ASSERT_TRUE(alternateObjects);
    auto replacedContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto replaced = builder.Prepare(replacedContext, alternateView, *alternateObjects, output, "Raster", &material, false, &gpuState);
    ASSERT_TRUE(replaced);
    EXPECT_EQ(replaced->Draws.size(), 3u);
    finish();
    writer->RemoveShapes(std::span{ids}.subspan(3));
    writer->SetStaticMeshes(std::span{ids}.first(3), mesh, std::span{transforms}.first(3));
    transforms[0](0, 3) = 0;
    writer->SetTransform(ids[0], transforms[0]);
    for (size_t i = 0; i < 3; ++i) writer->SetDrawState(ids[i], {});
    auto ordered = material;
    ordered.Passes[0].Blend = render::BlendState::Default();
    auto a = assets.AddReady<radray::Material>({902, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<radray::Material>(ordered.Passes, ordered.Constants));
    auto b = assets.AddReady<radray::Material>({903, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<radray::Material>(ordered.Passes, ordered.Constants));
    writer->SetMaterials(ids[0], std::span{&a, 1});
    writer->SetMaterials(ids[1], std::span{&b, 1});
    writer->SetMaterials(ids[2], std::span{&a, 1});
    start(0);
    objects = scenes.PrepareSceneGpuRT(sceneId, *Frame);
    ASSERT_TRUE(objects);
    auto transparentContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto transparent = builder.Prepare(transparentContext, view, *objects, output, "Raster", nullptr, false);
    ASSERT_TRUE(transparent);
    ASSERT_EQ(transparent->Draws.size(), 3u);
    EXPECT_EQ(transparent->Draws[0].Material, transparent->Draws[2].Material);
    EXPECT_NE(transparent->Draws[0].Material, transparent->Draws[1].Material);
    EXPECT_EQ(transparent->Draws[0].Draw.Instances, 1u);
    finish();
    writer->SetDrawState(ids[0], {.SortGroup = 2});
    writer->SetDrawState(ids[1], {.SortGroup = 1});
    writer->SetDrawState(ids[2], {.SortGroup = 2, .Order = 1});
    start(1);
    objects = scenes.PrepareSceneGpuRT(sceneId, *Frame);
    ASSERT_TRUE(objects);
    auto sortedContext = MakePipelineContext(*Renderer, *Frame, &scenes);
    auto sorted = builder.Prepare(sortedContext, view, *objects, output, "Raster", nullptr, false);
    ASSERT_TRUE(sorted);
    ASSERT_EQ(sorted->Draws.size(), 3u);
    EXPECT_EQ(sorted->Draws[0].Material, transparent->Draws[1].Material);
    finish();
    scenes.OnShutdown();
}
TEST_F(RasterDraw, CompatibleBatchesRespectSectionsWindingCullingAndTransparentOrder) { Batching(); }
TEST_F(VulkanRasterDraw, CompatibleBatchesRespectSectionsWindingCullingAndTransparentOrder) { Batching(); }
TEST_F(RasterDraw, MaterialAcceptsRegisteredProgramAndRejectsAmbiguousSourceOwnership) {
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto descriptor = Material(program.Get(), false);
    radray::Material registered{descriptor.Passes, descriptor.Constants};
    EXPECT_TRUE(registered.IsValid());
    descriptor.Passes[0].Program.SourceName = "ambiguous.hlsl";
    radray::Material ambiguous{descriptor.Passes, descriptor.Constants};
    EXPECT_FALSE(ambiguous.IsValid());
    EXPECT_FALSE(Core.ResolveMaterial(*Renderer, descriptor.Passes[0]));
    descriptor.Passes[0].Program.SourceName.clear();
    descriptor.Passes[0].RegisteredProgram = nullptr;
    radray::Material missing{descriptor.Passes, descriptor.Constants};
    EXPECT_FALSE(missing.IsValid());
}
TEST_F(RasterDraw, RuntimeMeshUsesExistingUploaderAndPreservesPackedBinsAfterProducerRelease) {
    MeshResource source;
    float vertices[]{0, 0, 0, 1, 0, 0, 0, 1, 0};
    uint32_t indices[]{0, 1, 2};
    source.Bins.emplace_back(std::as_bytes(std::span{vertices}));
    source.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers = {{"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12}};
    primitive.IndexBuffer = {1, 3, 0, 4};
    source.Primitives.push_back(primitive);
    EXPECT_FALSE(AllocateRuntimeMesh(*Gpu->GetDevice(), source, 32));
    auto mesh = AllocateRuntimeMesh(*Gpu->GetDevice(), source);
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->Draws[0].Ibv.Offset, 36u);
    Begin();
    auto* command = Frame->AllocateCommandBuffer();
    ASSERT_TRUE(TryUploadRuntimeMesh(Frame->GetUploader(), command, source, *mesh.Get()));
    source.Bins[0] = MeshBuffer(std::span<const byte>{});
    render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = mesh->Buffers[0].get(), .Before = render::BufferState::Vertex | render::BufferState::Index, .After = render::BufferState::CopySource};
    command->ResourceBarrier(std::span{&barrier, 1});
    command->CopyBufferToBuffer(Readback.get(), 0, mesh->Buffers[0].get(), 0, 48);
    ReadbackBarrier(command);
    Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&command, 1}});
    Complete();
    const auto* data = Readback->Map(0, 48);
    Readback->InvalidateMappedRange({0, 48});
    EXPECT_EQ(std::memcmp(data, vertices, 36), 0);
    EXPECT_EQ(std::memcmp(static_cast<const byte*>(data) + 36, indices, 12), 0);
    Readback->Unmap();
}
TEST_F(RasterDraw, BorrowedConstantSlicesReuseTheFlightArenaWithoutPerDrawCopiesOrNewPipelines) {
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    auto plan = Core.ResolveMaterial(*Renderer, material.Passes[0]);
    ASSERT_TRUE(plan);
    Begin();
    auto context = MakePipelineContext(*Renderer, *Frame);
    const float view[]{.5f, 0, 0, 0}, color[]{1, 0, 0, 1};
    auto a = context.Resources.AllocateConstants(16), b = context.Resources.AllocateConstants(16);
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());
    std::memcpy(a.Data(), view, 16);
    std::memcpy(b.Data(), color, 16);
    auto av = a.Commit(16), bv = b.Commit(16);
    const render::ShaderBufferBinding views[]{{av.Target, {av.Offset, av.Size}}}, constants[]{{bv.Target, {bv.Offset, bv.Size}}};
    const auto committed = Frame->GetHostWrites().GetStats().CommittedBytes;
    for (uint32_t i = 0; i < 64; ++i) ASSERT_TRUE(Core.PrepareMaterial(context, *plan.Get(), material, {.ConstantBuffers = constants, .ViewBuffers = views}));
    EXPECT_EQ(Frame->GetHostWrites().GetStats().CommittedBytes, committed);
    auto invalid = views[0];
    invalid.Range.Size = 8;
    EXPECT_FALSE(Core.PrepareMaterial(context, *plan.Get(), material, {.ConstantBuffers = constants, .ViewBuffers = std::span{&invalid, 1}}));
}
TEST_F(RasterDraw, InstanceSchemaComesFromCompilerPayloadAndRejectsMissingOrOverlappingFields) {
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto layout = ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Tint", "InstanceTint");
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->Stride, 16u);
    ASSERT_EQ(layout->Fields.size(), 1u);
    EXPECT_EQ(layout->Fields[0].Name, "Color");
    EXPECT_EQ(layout->Fields[0].Offset, 0u);
    EXPECT_FALSE(ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Missing", "Bad"));
    layout->Fields.push_back(layout->Fields[0]);
    EXPECT_FALSE(layout->IsValid());
}
}  // namespace
}  // namespace radray::test
#endif
