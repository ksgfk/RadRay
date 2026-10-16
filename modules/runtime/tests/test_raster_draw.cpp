#include <gtest/gtest.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/renderer.h>
#include <radray/runtime/render_framework/scene_draw.h>
#include "scene_test_support.h"
#include <cstring>
#if defined(RADRAY_RASTER_COMPILER_PATH) && defined(_WIN32)
#include <radray/shader_compiler/client.h>
#include <radray/platform/win32_headers.h>
namespace radray::test {
namespace {
class RasterDraw : public ::testing::Test {
protected:
    FrameTimeline Timeline{3};
    unique_ptr<GpuSystem> Gpu;
    unique_ptr<RenderSystem> Renderer;
    unique_ptr<radray::Renderer> Core;
    std::optional<FrameBuilder> Builder;
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
        Core = make_unique<radray::Renderer>(*Gpu, *Renderer);
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
            if (Core) Core->ProcessCompletions();
            Builder.reset();
            Frame.reset();
        }
        if (Core) EXPECT_TRUE(Core->Shutdown());
        Core.reset();
        if (Gpu) Gpu->AbandonUnpublishedResourcesTerminalGT();
        if (Window) DestroyWindow(Window);
    }
    Nullable<ShaderProgram*> Program(bool textures, bool arrays = false, bool instances = false, bool = false,
                                     bool visible = false, bool scene = false, bool objectPush = false, bool samplerArrays = false, bool staticSamplers = false) {
        string source = "#include <core/platform.hlsli>\n";
        source += scene ? "struct ViewValues { column_major float4x4 ViewProjection; };\n" : "struct ViewValues { float4 Data; };\n";
        source += R"(
struct MaterialValues { float4 Color; };
VK_BINDING(0,1) ConstantBuffer<ViewValues> ViewData : register(b0,space1);
VK_BINDING(0,2) ConstantBuffer<MaterialValues> Tint : register(b0,space2);
struct Vertex { float4 Position : SV_Position; nointerpolation uint Slot : TEXCOORD0; };
)";
        if (visible)
            source += R"(
struct ObjectValues { column_major float4x4 LocalToWorld; };
struct DrawValues { uint Tag; uint VisibleBase; };
VK_BINDING(0,0) StructuredBuffer<ObjectValues> Objects : register(t0);
VK_BINDING(1,0) StructuredBuffer<MaterialValues> InstanceData : register(t1);
VK_BINDING(1,1) StructuredBuffer<uint> VisibleInstanceSlots : register(t0,space1);
VK_PUSH_CONSTANT ConstantBuffer<DrawValues> DrawData : register(b2);
)";
        else if (instances)
            source += "VK_BINDING(0,0) StructuredBuffer<MaterialValues> InstanceData : register(t0);\n";
        if (objectPush) source += "struct DrawValues { uint ObjectSlot; };\nVK_PUSH_CONSTANT ConstantBuffer<DrawValues> DrawData : register(b2);\n";
        if (textures) {
            source += arrays ? "VK_BINDING(1,2) Texture2D<float4> Images[2] : register(t0,space2);\n" : "VK_BINDING(1,2) Texture2D<float4> ImageA : register(t0,space2);\nVK_BINDING(2,2) Texture2D<float4> ImageB : register(t1,space2);\n";
            source += samplerArrays ? "VK_BINDING(0,3) SamplerState Samples[2] : register(s0,space3);\n" : "VK_BINDING(0,3) SamplerState SampleA : register(s0,space3);\nVK_BINDING(1,3) SamplerState SampleB : register(s1,space3);\n";
        }
        string root;
        if (visible)
            root += "DescriptorTable(SRV(t0,numDescriptors=2)),";
        else if (instances)
            root += "DescriptorTable(SRV(t0)),";
        root += visible ? "DescriptorTable(CBV(b0,space=1),SRV(t0,space=1))," : "DescriptorTable(CBV(b0,space=1)),";
        root += textures ? "DescriptorTable(CBV(b0,space=2),SRV(t0,space=2,numDescriptors=2))" : "DescriptorTable(CBV(b0,space=2))";
        if (textures) root += staticSamplers ? ",StaticSampler(s0,space=3,filter=FILTER_MIN_MAG_MIP_POINT),StaticSampler(s1,space=3,filter=FILTER_MIN_MAG_MIP_POINT)" : ",DescriptorTable(Sampler(s0,space=3,numDescriptors=2))";
        if (visible || objectPush) root += visible ? ",RootConstants(num32BitConstants=2,b2)" : ",RootConstants(num32BitConstants=1,b2)";
        source += fmt::format("#define TEST_RS \"{}\"\n", root);
        source += R"(
[RootSignature(TEST_RS)] [shader("vertex")]
Vertex VSMain(uint index : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 positions[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
    Vertex output;
)";
        source += visible ? "output.Slot = VisibleInstanceSlots[DrawData.VisibleBase + instance];\n" : "output.Slot = 0;\n";
        source += scene ? "output.Position = float4(positions[index],.5,1);\n" : "output.Position = float4(positions[index],ViewData.Data.x,1);\n";
        if (visible) source += "output.Position = mul(Objects[output.Slot].LocalToWorld, output.Position);\n";
        if (objectPush) source += "output.Position.z += DrawData.ObjectSlot * .00001;\n";
        if (scene) source += "output.Position = mul(ViewData.ViewProjection, output.Position);\n";
        source += "return output; }\n[RootSignature(TEST_RS)] [shader(\"pixel\")] float4 PSMain(Vertex input) : SV_Target0 {\n";
        if (textures)
            source += fmt::format("return float4(Tint.Color.rgb + .5 * ({}.Sample({},float2(ViewData.Data.y,.5)).rgb + {}.Sample({},float2(ViewData.Data.y,.5)).rgb), Tint.Color.a);\n", arrays ? "Images[0]" : "ImageA", samplerArrays ? "Samples[0]" : "SampleA", arrays ? "Images[1]" : "ImageB", samplerArrays ? "Samples[1]" : "SampleB");
        else if (visible)
            source += "return DrawData.Tag == 7 ? InstanceData[input.Slot].Color * Tint.Color : float4(1,0,1,1);\n";
        else if (instances)
            source += "return InstanceData[(uint)ViewData.Data.z].Color * Tint.Color;\n";
        else
            source += "return Tint.Color;\n";
        source += "}\n";
        const vector<std::filesystem::path> includes{RADRAY_SHADERLIB_DIR};
        shader_compiler::Client compiler{RADRAY_RASTER_COMPILER_PATH};
        shader::SourceContractRequest discovery;
        discovery.SourceName = "native-raster.hlsl";
        const auto input = std::as_bytes(std::span{source.data(), source.size()});
        discovery.RootSource.assign(input.begin(), input.end());
        discovery.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(Backend() == render::RenderBackend::Vulkan ? shader::ShaderTarget::SPIRV : shader::ShaderTarget::DXIL));
        const auto contract = compiler.DiscoverSourceContract(discovery, includes);
        for (const auto& d : contract.Diagnostics)
            if (!contract.Succeeded()) ADD_FAILURE() << d.Message;
        EXPECT_TRUE(contract.Succeeded());
        if (!contract.Succeeded()) return nullptr;
        shader::CompileVariantRequest request;
        request.SourceName = discovery.SourceName;
        request.RootSource = discovery.RootSource;
        request.Targets = discovery.Targets;
        request.ExpectedContract = contract.Contract.Hash;
        const auto compiled = compiler.CompileVariant(request, includes);
        for (const auto& d : compiled.Diagnostics)
            if (compiled.Status != shader::CompileStatus::Success) ADD_FAILURE() << d.Message;
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
    MaterialRenderData Material(ShaderProgram* program, bool textures, bool arrays = false, bool samplerArrays = false) {
        MaterialTemplateDescriptor pass;
        pass.Name = "Raster";
        pass.RegisteredProgram = program->GetId();
        pass.Primitive.Cull = render::CullMode::None;
        pass.Inputs = {{"ViewData", MaterialInputSource::ViewConstants}, {"Tint", MaterialInputSource::MaterialConstants}};
        if (textures) {
            if (arrays)
                pass.Inputs.push_back({"Images", MaterialInputSource::Texture, 0});
            else {
                pass.Inputs.push_back({"ImageA", MaterialInputSource::Texture, 0});
                pass.Inputs.push_back({"ImageB", MaterialInputSource::Texture, 1});
            }
            if (samplerArrays)
                pass.Inputs.push_back({"Samples", MaterialInputSource::Sampler, 0});
            else {
                pass.Inputs.push_back({"SampleA", MaterialInputSource::Sampler, 0});
                pass.Inputs.push_back({"SampleB", MaterialInputSource::Sampler, 1});
            }
        }
        return {{std::move(pass)}, {vector<byte>(16)}, {}, {}};
    }
    void Begin(uint32_t flight = 0) {
        Gpu->BeginUpdateForFlight(flight);
        Frame = Gpu->BeginFrameRecord(flight, {}, {}, false);
        Builder = Core->BeginFrame(*Frame);
        ASSERT_TRUE(Builder);
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
        Core->ProcessCompletions();
        Builder.reset();
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
    struct Registered {
        MaterialId Material;
        TemplateId Template;
    };
    std::optional<Registered> Register(const MaterialRenderData& data) {
        vector<TemplateId> templates;
        for (const auto& definition : data.Passes) {
            const auto id = Core->RegisterTemplate(definition);
            if (!id) return std::nullopt;
            templates.push_back(*id);
        }
        RendererMaterialDescriptor descriptor{templates, data.Constants, {}, data.Samplers};
        for (const auto* texture : data.Textures) {
            const auto root = Core->ImportTexture(texture->GetDesc().Target);
            if (!root) return std::nullopt;
            const auto view = Core->CreateTextureView(*root, texture->GetDesc());
            if (!view) return std::nullopt;
            descriptor.Textures.push_back(*view);
        }
        const auto material = Core->RegisterMaterial(descriptor);
        for (const auto id : templates) Core->UnregisterTemplate(id);
        return material ? std::optional{Registered{*material, templates.front()}} : std::nullopt;
    }
    vector<byte> Bytes(std::span<const float> values) {
        const auto bytes = std::as_bytes(values);
        return {bytes.begin(), bytes.end()};
    }
    RendererDrawDescriptor Draw() {
        RendererDrawDescriptor draw;
        draw.Count = 3;
        draw.Viewport = {0, 0, 16, 16, 0, 1};
        draw.Scissor = {0, 0, 16, 16};
        return draw;
    }
    std::optional<FrameDrawId> Queue(Registered material, std::span<const float> view, RendererDrawDescriptor draw, RendererDrawValues values = {}, bool depth = false) {
        const auto viewId = Builder->AddView({{Bytes(view)}});
        if (!viewId) return std::nullopt;
        const auto pass = Builder->AddPass({*viewId, material.Template, {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, depth ? std::optional{render::TextureFormat::D32_FLOAT} : std::nullopt}});
        return pass ? Builder->AddDraw(*pass, material.Material, std::move(draw), std::move(values)) : std::nullopt;
    }
    vector<uint8_t> Render(std::span<const FrameDrawId> draws, bool depth = false, float clearDepth = 1) {
        if (!Builder->Compile()) {
            ADD_FAILURE() << Core->GetLastError();
            return {};
        }
        auto* command = Frame->AllocateCommandBuffer();
        auto* encoder = Start(command, depth, clearDepth);
        for (const auto draw : draws) EXPECT_TRUE(Builder->Record(encoder, draw));
        return Read(command, encoder);
    }
    void Mixed(bool arrays = false);
    void DepthStates(bool reverse = false);
    void Regions();
    void Instances();
    void PersistentDefaults(bool pure);
    void VisibleInstances();
    void Batching();
    void ProbePersistentGroups();
    void SharedSamplerDefaults(bool arrays);
    void StaticSamplers();
    void RetainedMaterial();
    void PushModes();
    void PresentExternal(bool hide);
};
class VulkanRasterDraw : public RasterDraw {
    render::RenderBackend Backend() const override { return render::RenderBackend::Vulkan; }
};

void RasterDraw::Mixed(bool arrays) {
    auto program = Program(true, arrays);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), true, arrays);
    auto missing = material.Passes[0];
    missing.Inputs.pop_back();
    EXPECT_FALSE(Core->RegisterTemplate(missing));
    auto wrong = material.Passes[0];
    wrong.Inputs[0].Source = MaterialInputSource::Texture;
    EXPECT_FALSE(Core->RegisterTemplate(wrong));
    vector<unique_ptr<render::Texture>> images;
    vector<unique_ptr<render::TextureView>> imageViews;
    for (uint32_t i = 0; i < 2; ++i) {
        images.push_back(Gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2D, 2, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                                          render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::CopyDestination})
                             .Unwrap());
        imageViews.push_back(Gpu->GetDevice()->CreateTextureView({images.back().get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap());
        material.Textures.push_back(imageViews.back().get());
    }
    unique_ptr<render::Buffer> indices;
    auto shutdown = MakeScopeGuard([&] { if (Frame) Complete(); EXPECT_TRUE(Core->Shutdown()); });
    render::SamplerDescriptor nearest{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
                                      render::FilterMode::Nearest, render::FilterMode::Nearest, render::FilterMode::Nearest, 0, 1};
    auto linear = nearest;
    linear.MinFilter = linear.MagFilter = render::FilterMode::Linear;
    material.Samplers = {nearest, linear};
    material.Constants = {Bytes(array<float, 4>{0, 0, 0, 1})};
    const auto registered = Register(material);
    ASSERT_TRUE(registered) << Core->GetLastError();
    auto bad = material;
    bad.Passes[0].Inputs[2].ValueIndex = 2;
    EXPECT_FALSE(Register(bad));
    Begin();
    auto* upload = Frame->AllocateCommandBuffer();
    const uint32_t a[]{0xff0000ff, 0xffff0000}, b[]{0xff00ff00, 0xffff0000};
    const TextureRegionUploadRequest uploads[]{{std::as_bytes(std::span{a}), images[0].get(), {.Width = 2, .Height = 1}},
                                               {std::as_bytes(std::span{b}), images[1].get(), {.Width = 2, .Height = 1}}};
    ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(upload, uploads));
    Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&upload, 1}});
    const array<float, 4> view{.25f, .4f, 0, 0};
    const auto original = Queue(*registered, view, Draw());
    ASSERT_TRUE(original);
    indices = Gpu->GetDevice()->CreateBuffer({12, render::MemoryType::Upload, render::BufferUse::Index | render::BufferUse::MapWrite}).Unwrap();
    const uint32_t triangle[]{0, 1, 2};
    std::memcpy(indices->Map(0, 12), triangle, 12);
    indices->FlushMappedRange({0, 12});
    indices->Unmap();
    const auto imported = Core->ImportBuffer(indices.get());
    ASSERT_TRUE(imported);
    auto draw = Draw();
    draw.Indices = {*imported, 0, 4};
    draw.Scissor.Width = 8;
    const auto changed = Queue(*registered, view, draw, {.Constants = {Bytes(array<float, 4>{.25f, 0, 0, 1})}});
    ASSERT_TRUE(changed);
    const auto pixels = Render(array{*original, *changed});
    ASSERT_EQ(pixels.size(), 4096u);
    const auto left = 8 * Pitch(16) + 4 * 4, right = 8 * Pitch(16) + 12 * 4;
    EXPECT_NEAR(pixels[left], 191, 1);
    EXPECT_NEAR(pixels[right], 128, 1);
    EXPECT_NEAR(pixels[right + 1], 89, 1);
    EXPECT_NEAR(pixels[right + 2], 38, 1);
    EXPECT_EQ(Renderer->GetGraphicsPipelineCache().GetSize(), 1u);
    EXPECT_TRUE(Core->RequestResourceExit(*imported));
    EXPECT_TRUE(Core->IsResourceRetired(*imported));
    material.Samplers = {nearest, nearest};
    const auto defaults = Register(material), equal = Register(material);
    ASSERT_TRUE(defaults && equal);
    Begin(1);
    const auto root = Core->ImportTexture(images[1].get());
    ASSERT_TRUE(root);
    const auto green = Core->CreateTextureView(*root, imageViews[1]->GetDesc());
    ASSERT_TRUE(green);
    draw = Draw();
    draw.Scissor = {0, 0, 5, 16};
    const auto textureOverride = Queue(*defaults, view, draw, {.Textures = {*green, *green}});
    draw.Scissor = {5, 0, 5, 16};
    const auto samplerOverride = Queue(*defaults, view, draw, {.Samplers = {linear, nearest}});
    draw.Scissor = {10, 0, 6, 16};
    const auto restored = Queue(*equal, view, draw);
    ASSERT_TRUE(textureOverride && samplerOverride && restored);
    const auto next = Render(array{*textureOverride, *samplerOverride, *restored});
    ASSERT_EQ(next.size(), 4096u);
    EXPECT_EQ(next[Pitch(16) * 8 + 2 * 4], 0);
    EXPECT_EQ(next[Pitch(16) * 8 + 2 * 4 + 1], 255);
    EXPECT_NEAR(next[Pitch(16) * 8 + 7 * 4], 89, 1);
    EXPECT_NEAR(next[Pitch(16) * 8 + 7 * 4 + 2], 38, 1);
    EXPECT_NEAR(next[Pitch(16) * 8 + 13 * 4], 128, 1);
    EXPECT_NEAR(next[Pitch(16) * 8 + 13 * 4 + 1], 128, 1);
}

void RasterDraw::DepthStates(bool reverse) {
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    auto& definition = material.Passes[0];
    definition.DepthStencil = render::DepthStencilState::Default();
    definition.DepthStencil->DepthCompare = reverse ? render::CompareFunction::Greater : render::CompareFunction::Less;
    Begin();
    vector<FrameDrawId> draws;
    for (uint32_t i = 0; i < 4; ++i) {
        definition.DepthStencil->DepthWriteEnable = i >= 2;
        material.Constants = {Bytes(array<float, 4>{i == 0 || i == 3 ? 1.0f : 0, i == 1 ? 1.0f : 0, i == 2 ? 1.0f : 0, 1})};
        const auto registered = Register(material);
        ASSERT_TRUE(registered);
        const array<float, 4> view{(i % 2 != 0) == reverse ? .2f : .8f, 0, 0, 0};
        const auto draw = Queue(*registered, view, Draw(), {}, true);
        ASSERT_TRUE(draw);
        draws.push_back(*draw);
    }
    const auto pixels = Render(draws, true, reverse ? 0.0f : 1.0f);
    ASSERT_EQ(pixels.size(), 4096u);
    EXPECT_EQ(Renderer->GetGraphicsPipelineCache().GetSize(), 2u);
    EXPECT_EQ(pixels[8 * Pitch(16) + 8 * 4], 0);
    EXPECT_EQ(pixels[8 * Pitch(16) + 8 * 4 + 2], 255);
}

void RasterDraw::Instances() {
    auto program = Program(false, false, true);
    ASSERT_TRUE(program);
    const auto layout = ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Tint", "TintStream");
    ASSERT_TRUE(layout);
    RenderScene scene;
    SceneGpuData mirror{*Core, 3};
    SceneApplyChanges changes;
    SceneUpdateBatch update;
    const ShapeId id{3, 0};
    update.CreateShapes = {id};
    update.MeshStates = {{.Id = id}};
    const array<float, 4> red{1, 0, 0, 1};
    ASSERT_TRUE(update.SetInstanceParameters(id, *layout, std::as_bytes(std::span{red})));
    scene.Apply(update, &changes);
    mirror.ApplyChanges(changes);
    Begin();
    const auto objects = mirror.Prepare(scene, *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto material = Material(program.Get(), false);
    material.Constants = {Bytes(array<float, 4>{1, 1, 1, 1})};
    material.Passes[0].ParameterLayouts = {*layout};
    material.Passes[0].Inputs.push_back({"InstanceData", MaterialInputSource::InstanceParameters});
    const auto registered = Register(material);
    ASSERT_TRUE(registered);
    const auto draw = Queue(*registered, array<float, 4>{.5f, 0, 3, 0}, Draw(), {.Parameters = objects->ParameterResources});
    ASSERT_TRUE(draw);
    const auto pixels = Render(array{*draw});
    ASSERT_EQ(pixels.size(), 4096u);
    EXPECT_EQ(pixels[Pitch(16) * 8 + 8 * 4], 255);
    EXPECT_EQ(pixels[Pitch(16) * 8 + 8 * 4 + 1], 0);
    Begin(1);
    const auto next = mirror.Prepare(scene, *Frame, *Builder);
    ASSERT_TRUE(next);
    auto wrong = next->ParameterResources;
    wrong[0].Layout.Fields[0].Flags ^= 1;
    ASSERT_TRUE(Queue(*registered, array<float, 4>{.5f, 0, 3, 0}, Draw(), {.Parameters = wrong}));
    EXPECT_FALSE(Builder->Compile());
    Complete();
}

void RasterDraw::PersistentDefaults(bool equalOverride) {
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    const array<float, 4> red{1, 0, 0, 1}, green{0, 1, 0, 1};
    material.Constants = {Bytes(red)};
    const auto registered = Register(material);
    ASSERT_TRUE(registered);
    std::optional<ResourceId> original;
    for (uint32_t index = 0; index < 18; ++index) {
        Begin(index % 3);
        const auto data = Builder->StoreData(std::as_bytes(std::span{red}));
        ASSERT_TRUE(data);
        if (original)
            EXPECT_EQ(original, data);
        else
            original = data;
        vector<FrameDrawId> draws;
        for (uint32_t i = 0; i < 3; ++i) {
            auto draw = Draw();
            draw.Scissor = {int32_t(i * 5), 0, i == 2 ? 6u : 5u, 16};
            RendererDrawValues values;
            if (i == 1)
                values.Constants = {Bytes(green)};
            else if (equalOverride)
                values.Constants = {Bytes(red)};
            const auto queued = Queue(*registered, array<float, 4>{.5f, float(i), 0, 0}, draw, std::move(values));
            ASSERT_TRUE(queued);
            draws.push_back(*queued);
        }
        const auto pixels = Render(draws);
        ASSERT_EQ(pixels.size(), 4096u);
        EXPECT_EQ(pixels[Pitch(16) * 8 + 2 * 4], 255);
        EXPECT_EQ(pixels[Pitch(16) * 8 + 7 * 4 + 1], 255);
        EXPECT_EQ(pixels[Pitch(16) * 8 + 13 * 4], 255);
    }
}

void RasterDraw::RetainedMaterial() {
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto data = Material(program.Get(), false);
    data.Constants = {Bytes(array<float, 4>{1, 0, 0, 1})};
    const auto material = Register(data);
    ASSERT_TRUE(material);
    Begin();
    const auto draw = Queue(*material, array<float, 4>{.5f, 0, 0, 0}, Draw());
    ASSERT_TRUE(draw);
    data = {};
    ASSERT_TRUE(Core->UnregisterMaterial(material->Material));
    EXPECT_FALSE(Core->IsMaterialRetired(material->Material));
    const auto pixels = Render(array{*draw});
    ASSERT_EQ(pixels.size(), 4096u);
    EXPECT_EQ(pixels[Pitch(16) * 8 + 8 * 4], 255);
    EXPECT_TRUE(Core->IsMaterialRetired(material->Material));
}

void RasterDraw::ProbePersistentGroups() {
    auto first = Program(false);
    ASSERT_TRUE(first);
    auto material = Material(first.Get(), false);
    material.Constants = {Bytes(array<float, 4>{1, 0, 0, 1})};
    const auto old = Register(material);
    ASSERT_TRUE(old);
    Begin();
    const auto draw = Queue(*old, array<float, 4>{.5f, 0, 0, 0}, Draw());
    ASSERT_TRUE(draw);
    ASSERT_EQ(Render(array{*draw}).size(), 4096u);
    EXPECT_TRUE(Core->Shutdown());
    Core.reset();
    Renderer->OnShutdown();
    ASSERT_TRUE(Renderer->OnInitialize());
    Core = make_unique<radray::Renderer>(*Gpu, *Renderer);
    EXPECT_FALSE(Core->GetTemplate(old->Template));
    EXPECT_FALSE(Core->IsMaterialValid(old->Material));
    EXPECT_FALSE(Core->RegisterTemplate(material.Passes[0]));
    auto next = Program(false);
    ASSERT_TRUE(next);
    material.Passes[0].RegisteredProgram = next->GetId();
    const auto replacement = Register(material);
    ASSERT_TRUE(replacement);
    Begin();
    const auto nextDraw = Queue(*replacement, array<float, 4>{.5f, 0, 0, 0}, Draw());
    ASSERT_TRUE(nextDraw);
    ASSERT_EQ(Render(array{*nextDraw}).size(), 4096u);
}

void RasterDraw::StaticSamplers() {
    auto program = Program(true, false, false, false, false, false, false, false, true);
    ASSERT_TRUE(program);
    auto data = Material(program.Get(), true);
    std::erase_if(data.Passes[0].Inputs, [](const auto& input) { return input.Source == MaterialInputSource::Sampler; });
    data.Constants = {Bytes(array<float, 4>{0, 0, 0, 1})};
    auto image = Gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2D, 2, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                                  render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::CopyDestination})
                     .Unwrap();
    auto view = Gpu->GetDevice()->CreateTextureView({image.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap();
    auto cleanup = MakeScopeGuard([&] { if (Frame) Complete(); EXPECT_TRUE(Core->Shutdown()); });
    data.Textures = {view.get(), view.get()};
    const auto material = Register(data);
    ASSERT_TRUE(material) << Core->GetLastError();
    Begin();
    auto* upload = Frame->AllocateCommandBuffer();
    const uint32_t pixels[]{0xff0000ff, 0xff00ff00};
    const TextureRegionUploadRequest request{std::as_bytes(std::span{pixels}), image.get(), {.Width = 2, .Height = 1}};
    ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(upload, std::span{&request, 1}));
    Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&upload, 1}});
    const auto draw = Queue(*material, array<float, 4>{.5f, .25f, 0, 0}, Draw());
    ASSERT_TRUE(draw);
    const auto result = Render(array{*draw});
    ASSERT_EQ(result.size(), 4096u);
    EXPECT_EQ(result[Pitch(16) * 8 + 8 * 4], 255);
    EXPECT_EQ(result[Pitch(16) * 8 + 8 * 4 + 1], 0);
}

void RasterDraw::SharedSamplerDefaults(bool arrays) {
    auto program = Program(true, false, false, false, false, false, false, arrays);
    ASSERT_TRUE(program);
    auto image = Gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                                  render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::CopyDestination})
                     .Unwrap();
    auto view = Gpu->GetDevice()->CreateTextureView({image.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap();
    auto cleanup = MakeScopeGuard([&] { if (Frame) Complete(); EXPECT_TRUE(Core->Shutdown()); });
    auto data = Material(program.Get(), true, false, arrays);
    data.Constants = {Bytes(array<float, 4>{0, 0, 0, 1})};
    data.Textures = {view.get(), view.get()};
    const render::SamplerDescriptor nearest{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
                                            render::FilterMode::Nearest, render::FilterMode::Nearest, render::FilterMode::Nearest, 0, 1};
    data.Samplers = {nearest, nearest};
    vector<Registered> materials;
    for (uint32_t i = 0; i < 4096; ++i) {
        const auto registered = Register(data);
        ASSERT_TRUE(registered) << Core->GetLastError();
        materials.push_back(*registered);
    }
    for (uint32_t flight = 0; flight < 3; ++flight) {
        Begin(flight);
        if (!flight) {
            auto* command = Frame->AllocateCommandBuffer();
            const uint32_t pixel = 0xff0000ff;
            TextureRegionUploadRequest upload{std::as_bytes(std::span{&pixel, 1}), image.get(), {.Width = 1, .Height = 1}};
            ASSERT_TRUE(Frame->GetUploader().TryUploadTextureRegions(command, std::span{&upload, 1}));
            Frame->ReturnCommandBuffers({.CmdBuffers = std::span{&command, 1}});
        }
        vector<FrameDrawId> draws;
        for (const auto& material : materials) {
            const auto draw = Queue(material, array<float, 4>{.5f, .5f, 0, 0}, Draw());
            ASSERT_TRUE(draw);
            draws.push_back(*draw);
        }
        // Compile all 4096 accepted versions against the unchanged 2048-slot D3D12 sampler heap.
        const auto pixels = Render(draws);
        ASSERT_EQ(pixels.size(), 4096u);
        EXPECT_EQ(pixels[Pitch(16) * 8 + 8 * 4], 255);
    }
}

void RasterDraw::VisibleInstances() {
    auto program = Program(false, false, false, false, true);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    const auto layout = ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "Tint", "TintStream");
    ASSERT_TRUE(layout);
    auto& definition = material.Passes[0];
    definition.Inputs.push_back({"Objects", MaterialInputSource::SceneObjects});
    definition.Inputs.push_back({"VisibleInstanceSlots", MaterialInputSource::VisibleInstanceSlots});
    definition.Inputs.push_back({"InstanceData", MaterialInputSource::InstanceParameters});
    definition.ParameterLayouts = {*layout};
    definition.InstanceAddressing = MaterialInstanceAddressing::VisibleSlots;
    definition.PushConstant = "DrawData";
    definition.SceneIndexField = "VisibleBase";
    material.Constants = {Bytes(array<float, 4>{1, 1, 1, 1})};
    const auto registered = Register(material);
    ASSERT_TRUE(registered);
    EXPECT_EQ(Core->GetTemplate(registered->Template)->SceneIndexOffset, 4u);
    auto invalid = definition;
    invalid.SceneIndexField = "Missing";
    EXPECT_FALSE(Core->RegisterTemplate(invalid));
    vector<Eigen::Matrix4f> matrices(208, Eigen::Matrix4f::Identity());
    vector<byte> tints(208 * 16);
    const array<uint32_t, 3> used{8, 41, 207};
    for (size_t i = 0; i < used.size(); ++i) {
        auto& matrix = matrices[used[i]];
        matrix(0, 0) = matrix(1, 1) = .2f;
        matrix(0, 3) = -.6f + float(i) * .6f;
        array<float, 4> tint{};
        tint[i] = tint[3] = 1;
        std::memcpy(tints.data() + used[i] * 16, tint.data(), 16);
    }
    Begin();
    const auto objects = Builder->StoreData(std::as_bytes(std::span{matrices}), 64);
    const auto parameters = Builder->StoreData(tints, 16);
    ASSERT_TRUE(objects && parameters);
    EXPECT_EQ(objects, Builder->StoreData(std::as_bytes(std::span{matrices}), 64));
    EXPECT_EQ(parameters, Builder->StoreData(tints, 16));
    EXPECT_EQ(std::get<render::ShaderBufferBinding>(*Core->GetResourceValue(*objects)).Range.Offset % 64, 0u);
    const auto odd = Builder->StoreData(vector<byte>(144), 48);
    ASSERT_TRUE(odd);
    EXPECT_EQ(std::get<render::ShaderBufferBinding>(*Core->GetResourceValue(*odd)).Range.Offset % 48, 0u);
    const auto view = Builder->AddView({{Bytes(array<float, 4>{.5f, 0, 0, 0})}, {207, 8, 41, 207, 41}});
    ASSERT_TRUE(view);
    const auto pass = Builder->AddPass({*view, registered->Template, {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}}});
    ASSERT_TRUE(pass);
    matrices.clear();
    matrices.shrink_to_fit();
    tints.clear();
    tints.shrink_to_fit();
    RendererDrawValues values{.Objects = objects, .Parameters = {{*layout, *parameters}}};
    auto draw = Draw();
    const array<uint32_t, 2> push{7, 1};
    const auto bytes = std::as_bytes(std::span{push});
    draw.Push.assign(bytes.begin(), bytes.end());
    draw.Instances = 2;
    const auto first = Builder->AddDraw(*pass, registered->Material, draw, values);
    const uint32_t base = 3;
    std::memcpy(draw.Push.data() + 4, &base, 4);
    draw.Instances = 1;
    const auto second = Builder->AddDraw(*pass, registered->Material, draw, values);
    ASSERT_TRUE(first && second);
    auto pixels = Render(array{*first, *second});
    ASSERT_EQ(pixels.size(), 4096u);
    const array<uint32_t, 3> columns{3, 8, 13};
    for (size_t i = 0; i < columns.size(); ++i)
        for (size_t color = 0; color < 3; ++color)
            EXPECT_EQ(pixels[Pitch(16) * 8 + columns[i] * 4 + color], i == color ? 255 : 0);
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

void RasterDraw::PushModes() {
    auto program = Program(false, false, false, false, false, false, true);
    ASSERT_TRUE(program);
    auto data = Material(program.Get(), false);
    auto legacy = data.Passes[0];
    legacy.ObjectIndexPushConstant = "DrawData";
    auto general = legacy;
    general.ObjectIndexPushConstant.clear();
    general.PushConstant = "DrawData";
    auto oldPlan = Core->RegisterTemplate(legacy);
    auto nativePlan = Core->RegisterTemplate(general);
    ASSERT_TRUE(oldPlan);
    ASSERT_TRUE(nativePlan);
    EXPECT_NE(*oldPlan, *nativePlan);
    EXPECT_EQ(Core->GetTemplate(*oldPlan)->SceneIndexOffset, 0u);
    EXPECT_FALSE(Core->GetTemplate(*nativePlan)->SceneIndexOffset);
    auto first = Core->RegisterTemplate(general);
    auto second = Core->RegisterTemplate(legacy);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_FALSE(Core->GetTemplate(*first)->SceneIndexOffset);
    EXPECT_EQ(Core->GetTemplate(*second)->SceneIndexOffset, 0u);
    general.SceneIndexField = "ObjectSlot";
    auto explicitPlan = Core->RegisterTemplate(general);
    ASSERT_TRUE(explicitPlan);
    EXPECT_EQ(Core->GetTemplate(*explicitPlan)->SceneIndexOffset, 0u);
}

void RasterDraw::PresentExternal(bool hide) {
    Window = CreateWindowExW(0, L"STATIC", L"RadRay raster producer", WS_OVERLAPPEDWINDOW,
                             40, 40, 160, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(Window, nullptr);
    ShowWindow(Window, SW_SHOW);
    UpdateWindow(Window);
    auto chain = Gpu->GetDevice()->CreateSwapChain({.PresentQueue = Gpu->GetMainQueue(), .NativeHandler = Window, .Width = 16, .Height = 16, .BackBufferCount = 2, .Format = render::TextureFormat::RGBA8_UNORM, .PresentMode = render::PresentMode::Immediate}).Unwrap();
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto material = Material(program.Get(), false);
    material.Constants = {Bytes(array<float, 4>{1, 0, 0, 1})};
    const auto registered = Register(material);
    ASSERT_TRUE(registered);
    Begin();
    const auto draw = Queue(*registered, array<float, 4>{.25f, 0, 0, 0}, Draw());
    ASSERT_TRUE(draw);
    ASSERT_TRUE(Builder->Compile());
    auto* commands = Frame->AllocateCommandBuffer();
    auto* encoder = Start(commands, false);
    ASSERT_TRUE(Builder->Record(encoder, *draw));
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
TEST_F(RasterDraw, MaterialAcceptsRegisteredProgramAndRejectsAmbiguousSourceOwnership) {
    auto program = Program(false);
    ASSERT_TRUE(program);
    auto descriptor = Material(program.Get(), false);
    radray::Material registered{descriptor.Passes, descriptor.Constants};
    EXPECT_TRUE(registered.IsValid());
    descriptor.Passes[0].Program.SourceName = "ambiguous.hlsl";
    radray::Material ambiguous{descriptor.Passes, descriptor.Constants};
    EXPECT_FALSE(ambiguous.IsValid());
    EXPECT_FALSE(Core->RegisterTemplate(descriptor.Passes[0]));
    descriptor.Passes[0].Program.SourceName.clear();
    descriptor.Passes[0].RegisteredProgram.reset();
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
    scenes.AttachRenderer(*Core);
    unordered_map<const MaterialRenderData*, Registered> fallbackMaterials;
    const auto fallback = [&](const MaterialRenderData* data) -> std::optional<MaterialId> {
        const auto found = fallbackMaterials.find(data);
        if (found != fallbackMaterials.end()) return found->second.Material;
        const auto registered = Register(*data);
        if (!registered) return std::nullopt;
        fallbackMaterials.emplace(data, *registered);
        return registered->Material;
    };
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
    auto start = [&](uint32_t flight) {
        scenes.SealFrameGT(flight);
        scenes.PublishFrameGT(flight);
        Begin(flight);
        scenes.ConsumeRenderUpdates(flight, Frame->FrameSerial());
    };
    auto finish = [&] {
        const auto flight = Frame->FlightIndex();
        const auto serial = Frame->FrameSerial();
        if (!Builder->IsCompiled()) ASSERT_TRUE(Builder->Compile()) << Core->GetLastError();
        Complete();
        scenes.OnFlightCompletedGT({.FlightIndex = flight, .OutputsExecuted = true, .FrameSerial = serial});
    };
    SceneViewRequest view;
    view.Scene = sceneId;
    view.EvaluatedProjection = Eigen::Matrix4f::Identity();
    const RenderOutputInfo output{16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}};
    start(0);
    auto objects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(sceneId), *scenes.GetSceneRT(sceneId), *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto context = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto first = builder.Prepare(context, view, *objects, output, "Raster", fallback(&material), false);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->Draws.size(), 1u);
    const auto* templateAddress = scenes.GetSceneRT(sceneId)->GetDrawPrimitives(ids[0]).data();
    for (uint32_t i = 1; i < 3; ++i) {
        auto other = view;
        other.View(0, 3) = float(i) * .05f;
        const auto next = builder.Prepare(context, other, *objects, output, "Raster", fallback(&material), false);
        ASSERT_TRUE(next);
        EXPECT_EQ(next->Draws.size(), 1u);
    }
    auto* commands = Frame->AllocateCommandBuffer();
    auto* encoder = Start(commands, false);
    ASSERT_TRUE(Builder->Compile()) << Core->GetLastError();
    ASSERT_TRUE(first->Record(*Builder, encoder));
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
    objects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(sceneId), *scenes.GetSceneRT(sceneId), *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto splitContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    EXPECT_EQ(scenes.GetSceneRT(sceneId)->GetDrawPrimitives(ids[0]).data(), templateAddress);
    auto split = builder.Prepare(splitContext, view, *objects, output, "Raster", fallback(&material), false);
    ASSERT_TRUE(split);
    ASSERT_EQ(split->Draws.size(), 2u);
    finish();
    transforms[0](0, 3) = 50;
    writer->SetTransform(ids[0], transforms[0]);
    writer->SetDrawState(ids[1], {.VisibilityMask = 0});
    auto disabled = material;
    disabled.Passes[0].AllowAutomaticInstancing = false;
    start(0);
    objects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(sceneId), *scenes.GetSceneRT(sceneId), *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto unmergedContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto unmerged = builder.Prepare(unmergedContext, view, *objects, output, "Raster", fallback(&disabled), false);
    ASSERT_TRUE(unmerged);
    EXPECT_EQ(unmerged->Draws.size(), 9998u);
    EXPECT_EQ(unmerged->Stats.MaskCulled, 1u);
    EXPECT_EQ(unmerged->Stats.BoundsCulled, 1u);
    EXPECT_EQ(unmerged->Stats.AutomaticDisabled, 9998u);
    finish();
    auto sections = meshFactory(901, {{0, 0, 3, 0, 2}, {0, 3, 6, 0, 2}, {0, 0, 3, 0, 2, 1}});
    writer->SetStaticMeshes(ids, sections, transforms);
    start(1);
    objects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(sceneId), *scenes.GetSceneRT(sceneId), *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto rangedContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto ranged = builder.Prepare(rangedContext, view, *objects, output, "Raster", fallback(&material), false);
    ASSERT_TRUE(ranged);
    EXPECT_EQ(ranged->Draws.size(), 6u);
    finish();
    vector<ShapeDrawUpdate> explicitStates;
    for (size_t i = 0; i < ids.size(); ++i) explicitStates.push_back({ids[i], {.VisibilityMask = i == 1 ? 0u : UINT32_MAX, .BatchDomain = 1u + i % 2, .ExplicitInstancing = true}});
    writer->SetDrawStates(explicitStates);
    start(0);
    objects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(sceneId), *scenes.GetSceneRT(sceneId), *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto explicitContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto explicitBatches = builder.Prepare(explicitContext, view, *objects, output, "Raster", fallback(&disabled), false);
    ASSERT_TRUE(explicitBatches);
    EXPECT_EQ(explicitBatches->Draws.size(), 12u);
    EXPECT_EQ(explicitBatches->Stats.AutomaticDisabled, 0u);
    finish();
    auto directProgram = Program(false, false, false, true);
    ASSERT_TRUE(directProgram);
    auto direct = Material(directProgram.Get(), false);
    start(1);
    objects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(sceneId), *scenes.GetSceneRT(sceneId), *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto directContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto unsupported = builder.Prepare(directContext, view, *objects, output, "Raster", fallback(&direct), false);
    ASSERT_TRUE(unsupported);
    EXPECT_TRUE(unsupported->Draws.empty());
    EXPECT_EQ(unsupported->Stats.ExplicitUnsupported, 29994u);
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
    auto alternateObjects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(otherScene), *scenes.GetSceneRT(otherScene), *Frame, *Builder);
    ASSERT_TRUE(alternateObjects);
    auto alternateContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto alternate = builder.Prepare(alternateContext, alternateView, *alternateObjects, output, "Raster", fallback(&material), false);
    ASSERT_TRUE(alternate);
    ASSERT_EQ(alternate->Draws.size(), 1u);
    EXPECT_EQ(scenes.GetSceneRT(sceneId)->GetDrawPrimitives(ids[2]).data(), originalTemplate);
    finish();
    scenes.DestroySceneGT(otherScene);
    start(1);
    EXPECT_FALSE(scenes.GetSceneRT(otherScene));
    auto emptyContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    UnlitRenderPipeline emptyPipeline;
    auto empty = emptyPipeline.Collect(emptyContext, {});
    ASSERT_TRUE(Builder->Compile());
    EXPECT_EQ(RecordRenderPipeline(*empty, emptyContext, {}).Status, PipelineRecordStatus::NoWork);
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
    alternateObjects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(reconnected), *scenes.GetSceneRT(reconnected), *Frame, *Builder);
    ASSERT_TRUE(alternateObjects);
    auto replacedContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto replaced = builder.Prepare(replacedContext, alternateView, *alternateObjects, output, "Raster", fallback(&material), false);
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
    objects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(sceneId), *scenes.GetSceneRT(sceneId), *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto transparentContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto transparent = builder.Prepare(transparentContext, view, *objects, output, "Raster", std::nullopt, false);
    ASSERT_TRUE(transparent);
    ASSERT_EQ(transparent->Draws.size(), 3u);
    finish();
    writer->SetDrawState(ids[0], {.SortGroup = 2});
    writer->SetDrawState(ids[1], {.SortGroup = 1});
    writer->SetDrawState(ids[2], {.SortGroup = 2, .Order = 1});
    start(1);
    objects = Core->GetScenes().Prepare(scenes.GetRendererSceneKey(sceneId), *scenes.GetSceneRT(sceneId), *Frame, *Builder);
    ASSERT_TRUE(objects);
    auto sortedContext = MakePipelineContext(*Core, *Builder, *Frame, &scenes);
    auto sorted = builder.Prepare(sortedContext, view, *objects, output, "Raster", std::nullopt, false);
    ASSERT_TRUE(sorted);
    ASSERT_EQ(sorted->Draws.size(), 3u);
    finish();
    EXPECT_TRUE(Core->Shutdown());
    scenes.OnShutdown();
}

TEST_F(RasterDraw, ExternalSurfacePresentsOnceAndRetiresOnTheFinalFence) { PresentExternal(false); }
TEST_F(RasterDraw, HiddenExternalSurfacePreservesIndependentApplicationWork) { PresentExternal(true); }
TEST_F(RasterDraw, PrecompiledGroupedBindingsTwoTexturesSamplersIndexedAndNonIndexed) { Mixed(); }
TEST_F(VulkanRasterDraw, PrecompiledGroupedBindingsTwoTexturesSamplersIndexedAndNonIndexed) { Mixed(); }
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
TEST_F(RasterDraw, PersistentPureDefaultsSurviveViewsFlightsAndTemporaryOverride) { PersistentDefaults(true); }
TEST_F(VulkanRasterDraw, PersistentPureDefaultsSurviveViewsFlightsAndTemporaryOverride) { PersistentDefaults(true); }
TEST_F(RasterDraw, EqualOverridesSurviveViewsFlightsAndTemporaryOverride) { PersistentDefaults(false); }
TEST_F(VulkanRasterDraw, EqualOverridesSurviveViewsFlightsAndTemporaryOverride) { PersistentDefaults(false); }
TEST_F(RasterDraw, ServiceReplacementRequiresDrainedRendererAndInvalidatesOldIds) { ProbePersistentGroups(); }
TEST_F(VulkanRasterDraw, ServiceReplacementRequiresDrainedRendererAndInvalidatesOldIds) { ProbePersistentGroups(); }
TEST_F(RasterDraw, StaticSamplerGroupWithoutRuntimeInputsReadsBack) { StaticSamplers(); }
TEST_F(VulkanRasterDraw, StaticSamplerGroupWithoutRuntimeInputsReadsBack) { StaticSamplers(); }
TEST_F(RasterDraw, ImmutableSamplerDefaultsShareAcross4096VersionsAndFlights) { SharedSamplerDefaults(false); }
TEST_F(VulkanRasterDraw, ImmutableSamplerDefaultsShareAcross4096VersionsAndFlights) { SharedSamplerDefaults(false); }
TEST_F(RasterDraw, ImmutableSamplerArraysShareAcross4096VersionsAndFlights) { SharedSamplerDefaults(true); }
TEST_F(VulkanRasterDraw, ImmutableSamplerArraysShareAcross4096VersionsAndFlights) { SharedSamplerDefaults(true); }
TEST_F(RasterDraw, UnregisteredMaterialVersionRetiresAfterItsRealGpuUse) { RetainedMaterial(); }
TEST_F(VulkanRasterDraw, UnregisteredMaterialVersionRetiresAfterItsRealGpuUse) { RetainedMaterial(); }
TEST_F(RasterDraw, GeneralPushDoesNotInheritTheCachedLegacyObjectSlotMapping) { PushModes(); }
TEST_F(VulkanRasterDraw, GeneralPushDoesNotInheritTheCachedLegacyObjectSlotMapping) { PushModes(); }
TEST_F(RasterDraw, SparseVisibleSlotsUseNonzeroMappedBaseAfterSourceArraysAreReleased) { VisibleInstances(); }
TEST_F(VulkanRasterDraw, SparseVisibleSlotsUseNonzeroMappedBaseAfterSourceArraysAreReleased) { VisibleInstances(); }
TEST_F(RasterDraw, CompatibleBatchesRespectSectionsWindingCullingAndTransparentOrder) { Batching(); }
TEST_F(VulkanRasterDraw, CompatibleBatchesRespectSectionsWindingCullingAndTransparentOrder) { Batching(); }

}  // namespace
}  // namespace radray::test
#endif
