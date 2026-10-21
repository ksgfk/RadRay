#include <gtest/gtest.h>
#include <cstring>

#include <radray/runtime/render_framework/renderer_resources.h>
#include <radray/runtime/render_framework/renderer.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/scope_guard.h>
#include <radray/render/pipeline_layout_types.h>
#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif
#include "gpu_test_fixture.h"
#include "runtime_test_device.h"
#if defined(RADRAY_RASTER_COMPILER_PATH)
#include <radray/shader_compiler/client.h>
#endif

namespace radray::test {
namespace {
struct RegistryProbeCounts {
    uint32_t Creates{0}, Sets{0}, Flushes{0}, Live{0}, FailCreates{0};
};
class RegistryProbeSet final : public render::ShaderParameterSet {
public:
    RegistryProbeSet(unique_ptr<render::ShaderParameterSet> native, RegistryProbeCounts& counts) : Native(std::move(native)), Counts(counts) { ++Counts.Live; }
    ~RegistryProbeSet() noexcept override { --Counts.Live; }
    bool IsValid() const noexcept override { return Native->IsValid(); }
    void Destroy() noexcept override { Native->Destroy(); }
    bool Set(render::BindingHandle binding, uint32_t element, render::ShaderParameterValue value) noexcept override {
        ++Counts.Sets;
        return Native->Set(binding, element, value);
    }
    bool FlushWrites() noexcept override {
        ++Counts.Flushes;
        return Native->FlushWrites();
    }
    void ResetBindingsForReuse() noexcept override { ADD_FAILURE() << "Immutable registry storage was reset"; }
    unique_ptr<render::ShaderParameterSet> Native;
    RegistryProbeCounts& Counts;
};
class RegistryProbeDevice final : public RuntimeTestDevice {
public:
    RegistryProbeDevice(render::Device* native, RegistryProbeCounts& counts) : Native(native), Counts(counts) { Capabilities = native->GetCapabilities(); }
    render::RenderBackend GetBackend() noexcept override { return Native->GetBackend(); }
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& descriptor) noexcept override { return Native->CreateBuffer(descriptor); }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor& descriptor) noexcept override { return Native->CreateTextureView(descriptor); }
    Nullable<unique_ptr<render::ShaderParameterSet>> CreateShaderParameterSet(const render::ShaderParameterSetDescriptor& descriptor) noexcept override {
        ++Counts.Creates;
        if (Counts.FailCreates) {
            --Counts.FailCreates;
            return nullptr;
        }
        auto native = Native->CreateShaderParameterSet(descriptor);
        return native ? Nullable<unique_ptr<render::ShaderParameterSet>>{make_unique<RegistryProbeSet>(native.Release(), Counts)} : nullptr;
    }
    render::Device* Native;
    RegistryProbeCounts& Counts;
};
/// Records which parameter set `Record` binds to each group. `Renderer::Record` and
/// `BindingRegistry::Bind` only need the encoder interface, so a formatting-free recorder observes group
/// identity without a native backend; the real backend bind path is covered by example_descriptor_reuse.
class RecordingEncoder final : public render::GraphicsCommandEncoder {
public:
    struct Binding {
        uint32_t Group;
        render::ShaderParameterSet* Set;
    };
    vector<Binding> Bindings;
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::CommandBuffer* GetCommandBuffer() const noexcept override { return nullptr; }
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset>) noexcept override { Bindings.push_back({group, set}); }
    bool SetPushConstants(render::BindingHandle, std::span<const byte>) noexcept override { return true; }
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    void BindVertexBuffers(std::span<const render::VertexBufferBinding>) noexcept override {}
    void BindIndexBuffer(render::IndexBufferView) noexcept override {}
    void BindGraphicsPipelineState(render::GraphicsPipelineState*) noexcept override {}
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) noexcept override {}
    void DrawIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override {}
    void DrawIndexedIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override {}
};
class RendererRegistry : public ::testing::TestWithParam<render::RenderBackend> {
protected:
    render::test::DeviceContext Context;
    void SetUp() override { ASSERT_TRUE(render::test::TryCreateDevice(GetParam(), Context)) << Context.Reason; }
    bool Signal(render::Fence* fence) {
#ifdef RADRAY_ENABLE_D3D12
        if (GetParam() == render::RenderBackend::D3D12)
            return SUCCEEDED(static_cast<render::d3d12::FenceD3D12*>(fence)->_fence->Signal(1));
#endif
#ifdef RADRAY_ENABLE_VULKAN
        if (GetParam() == render::RenderBackend::Vulkan) {
            auto* native = static_cast<render::vulkan::FenceVulkan*>(fence);
            const VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, native->_fence->_semaphore, 1};
            return native->_device->_ftb.vkSignalSemaphore(native->_device->_device, &signal) == VK_SUCCESS;
        }
#endif
        return false;
    }
#if defined(RADRAY_RASTER_COMPILER_PATH)
    Nullable<ShaderProgram*> Program(RenderSystem& services, bool mixed, bool textured = false) {
        string source = R"hlsl(
#include <core/platform.hlsli>
struct Values { float4 Data; };
#if MIXED
VK_BINDING(0, 0) ConstantBuffer<Values> ViewData : register(b0);
VK_BINDING(1, 0) ConstantBuffer<Values> Tint : register(b1);
#define GROUPS "DescriptorTable(CBV(b0,numDescriptors=2))"
#else
VK_BINDING(0, 1) ConstantBuffer<Values> ViewData : register(b0, space1);
VK_BINDING(0, 2) ConstantBuffer<Values> Tint : register(b0, space2);
#if TEXTURED
VK_BINDING(0, 3) Texture2D<float4> Albedo : register(t0, space3);
#define GROUPS "DescriptorTable(CBV(b0,space=1)),DescriptorTable(CBV(b0,space=2)),DescriptorTable(SRV(t0,space=3))"
#else
#define GROUPS "DescriptorTable(CBV(b0,space=1)),DescriptorTable(CBV(b0,space=2))"
#endif
#endif
struct Vertex { float4 Position : SV_Position; };
[RootSignature(GROUPS)]
[shader("vertex")] Vertex VSMain(uint index : SV_VertexID) {
    const float2 positions[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
    Vertex output; output.Position = float4(positions[index], ViewData.Data.x, 1); return output;
}
[RootSignature(GROUPS)]
[shader("pixel")] float4 PSMain(Vertex input) : SV_Target {
#if TEXTURED
    return Tint.Data * Albedo.Load(int3(0, 0, 0));
#else
    return Tint.Data;
#endif
}
)hlsl";
        shader_compiler::Client compiler{RADRAY_RASTER_COMPILER_PATH};
        shader::SourceContractRequest discovery;
        discovery.SourceName = "renderer_registry.hlsl";
        const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
        discovery.RootSource.assign(bytes.begin(), bytes.end());
        discovery.Defines = {{"MIXED", mixed ? "1" : "0"}, {"TEXTURED", textured ? "1" : "0"}};
        discovery.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(GetParam() == render::RenderBackend::D3D12 ? shader::ShaderTarget::DXIL : shader::ShaderTarget::SPIRV));
        const vector<std::filesystem::path> includes{RADRAY_SHADERLIB_DIR};
        auto contract = compiler.DiscoverSourceContract(discovery, includes);
        if (!contract.Succeeded()) {
            for (const auto& diagnostic : contract.Diagnostics) ADD_FAILURE() << diagnostic.Message;
            return nullptr;
        }
        shader::CompileVariantRequest request;
        request.SourceName = discovery.SourceName;
        request.RootSource = discovery.RootSource;
        request.Targets = discovery.Targets;
        request.Defines = discovery.Defines;
        request.ExpectedContract = contract.Contract.Hash;
        const auto compiled = compiler.CompileVariant(request, includes);
        if (compiled.Status != shader::CompileStatus::Success || compiled.Lanes.size() != 1) {
            for (const auto& diagnostic : compiled.Diagnostics) ADD_FAILURE() << diagnostic.Message;
            return nullptr;
        }
        const auto& metadata = compiled.Lanes[0].Metadata;
        if (metadata.size() < sizeof(shader::WireMetadataEnvelope)) return nullptr;
        shader::WireMetadataEnvelope envelope;
        std::memcpy(&envelope, metadata.data(), sizeof(envelope));
        return services.GetOrCreateShaderProgram(metadata, envelope.GpuArtifact);
    }
#endif
    unique_ptr<render::PipelineLayout> Layout(uint32_t group, string name, uint32_t count = 1) {
        if (GetParam() == render::RenderBackend::D3D12) {
            auto* factory = dynamic_cast<render::detail::D3D12PipelineLayoutFactory*>(Context.Device.get());
            if (!factory) return {};
            render::ResolvedD3D12Layout desc;
            desc.Bindings = {{name, shader::ShaderBindingKind::CBuffer, group, 0, count, render::ShaderStage::Vertex, shader::ShaderBindingPlacement::Table}};
            desc.Metadata = {{name, render::ShaderLayoutRecordKind::Descriptor, 0}};
            auto result = factory->CreatePipelineLayout(desc);
            return result ? result.Release() : nullptr;
        }
        auto* factory = dynamic_cast<render::detail::VulkanPipelineLayoutFactory*>(Context.Device.get());
        if (!factory) return {};
        render::ResolvedVulkanLayout desc;
        desc.SetCount = group + 1;
        desc.Bindings = {{name, shader::ShaderBindingKind::CBuffer, group, 0, count, render::ShaderStage::Vertex}};
        desc.Metadata = {{name, render::ShaderLayoutRecordKind::Descriptor, 0}};
        auto result = factory->CreatePipelineLayout(desc);
        return result ? result.Release() : nullptr;
    }
};

TEST_P(RendererRegistry, CompatibleLayoutsReuseFinalValuesAndRejectForeignBindingHandles) {
    const auto a = Layout(0, "A"), b = Layout(2, "Renamed"), array = Layout(1, "Array", 2);
    ASSERT_TRUE(a && b && array);
    ASSERT_EQ(a->GetParameterGroupCompatibility(0), b->GetParameterGroupCompatibility(2));
    EXPECT_NE(a->GetParameterGroupCompatibility(0), array->GetParameterGroupCompatibility(1));
    auto buffer = Context.Device->CreateBuffer({512, render::MemoryType::Upload, render::BufferUse::CBuffer});
    ASSERT_TRUE(buffer);
    RegistryProbeCounts counts;
    RegistryProbeDevice device{Context.Device.get(), counts};
    const auto owner = Guid::NewGuid();
    BindingRegistry registry{&device, owner};
    const ResourceId resource{owner, 0, 1};
    ParameterWrite write{a->FindBinding("A"), 0, render::ShaderBufferBinding{buffer.Get()}};
    const auto first = registry.Resolve(a.get(), 0, std::span{&write, 1}, std::span{&resource, 1});
    ASSERT_TRUE(first);
    ASSERT_TRUE(registry.Retain(*first));
    EXPECT_FALSE(registry.Resolve(b.get(), 2, std::span{&write, 1}, std::span{&resource, 1}));
    write.Binding = b->FindBinding("Renamed");
    write.Value = render::ShaderBufferBinding{buffer.Get(), {0, 512}, 0};
    const auto second = registry.Resolve(b.get(), 2, std::span{&write, 1}, std::span{&resource, 1});
    ASSERT_EQ(first, second);
    ASSERT_TRUE(registry.Retain(*second));
    EXPECT_EQ(counts.Creates, 1u);
    EXPECT_EQ(counts.Sets, 1u);
    EXPECT_EQ(counts.Flushes, 1u);
    registry.RetireResource(resource);
    EXPECT_TRUE(registry.HasResource(resource));
    ASSERT_TRUE(registry.Release(*first));
    EXPECT_EQ(counts.Live, 1u);
    ASSERT_TRUE(registry.Release(*second));
    EXPECT_EQ(counts.Live, 0u);
    EXPECT_FALSE(registry.IsValid(*first));
}

TEST_P(RendererRegistry, PressureEvictsIdleStorageAndRetriesWithoutEvictingActiveUses) {
    const auto layout = Layout(0, "Data");
    ASSERT_TRUE(layout);
    auto buffer = Context.Device->CreateBuffer({512, render::MemoryType::Upload, render::BufferUse::CBuffer});
    ASSERT_TRUE(buffer);
    RegistryProbeCounts counts;
    RegistryProbeDevice device{Context.Device.get(), counts};
    BindingRegistry registry{&device, Guid::NewGuid()};
    ParameterWrite write{layout->FindBinding("Data"), 0, render::ShaderBufferBinding{buffer.Get(), {0, 256}, 0}};
    const auto active = registry.Resolve(layout.get(), 0, std::span{&write, 1}, {});
    ASSERT_TRUE(active);
    ASSERT_TRUE(registry.Retain(*active));
    write.Value = render::ShaderBufferBinding{buffer.Get(), {256, 256}, 0};
    const auto idle = registry.Resolve(layout.get(), 0, std::span{&write, 1}, {});
    ASSERT_TRUE(idle);
    write.Value = render::ShaderBufferBinding{buffer.Get(), {0, 512}, 0};
    counts.FailCreates = 1;
    const auto replacement = registry.Resolve(layout.get(), 0, std::span{&write, 1}, {});
    ASSERT_TRUE(replacement);
    EXPECT_FALSE(registry.IsValid(*idle));
    EXPECT_TRUE(registry.IsValid(*active));
    EXPECT_NE(*idle, *replacement);
    EXPECT_EQ(counts.Creates, 4u);
    auto foreign = *active;
    foreign.Owner = Guid::NewGuid();
    EXPECT_FALSE(registry.Retain(foreign));
    registry.SetBudget({0, 0, 0});
    EXPECT_EQ(counts.Live, 1u);
    EXPECT_TRUE(registry.Release(*active));
    EXPECT_EQ(counts.Live, 0u);
}

// Mirrors the Renderer material group cache: the same stored writes and dependencies reuse one block
// across frames, and a zero budget forces a rebuild that creates a fresh block.
TEST_P(RendererRegistry, MaterialGroupWritesReuseOneBlockAndRebuildAfterTrim) {
    const auto layout = Layout(0, "Tint");
    ASSERT_TRUE(layout);
    auto buffer = Context.Device->CreateBuffer({512, render::MemoryType::Upload, render::BufferUse::CBuffer});
    ASSERT_TRUE(buffer);
    RegistryProbeCounts counts;
    RegistryProbeDevice device{Context.Device.get(), counts};
    const auto owner = Guid::NewGuid();
    BindingRegistry registry{&device, owner};
    const ResourceId dependency{owner, 0, 1};
    const ParameterWrite write{layout->FindBinding("Tint"), 0, render::ShaderBufferBinding{buffer.Get(), {0, 16}, 0}};
    const auto first = registry.Resolve(layout.get(), 0, std::span{&write, 1}, std::span{&dependency, 1});
    ASSERT_TRUE(first);
    ASSERT_TRUE(registry.Retain(*first));
    EXPECT_EQ(counts.Creates, 1u);
    // A second frame re-resolves the stored writes and reuses the block with no new native work.
    const auto second = registry.Resolve(layout.get(), 0, std::span{&write, 1}, std::span{&dependency, 1});
    ASSERT_TRUE(second);
    EXPECT_EQ(*second, *first);
    EXPECT_EQ(counts.Creates, 1u);
    EXPECT_EQ(counts.Sets, 1u);
    EXPECT_EQ(counts.Flushes, 1u);
    // Draining the frame and trimming evicts the idle block; the next resolve rebuilds it.
    ASSERT_TRUE(registry.Release(*first));
    registry.SetBudget({0, 0, 0});
    EXPECT_FALSE(registry.IsValid(*first));
    const auto rebuilt = registry.Resolve(layout.get(), 0, std::span{&write, 1}, std::span{&dependency, 1});
    ASSERT_TRUE(rebuilt);
    EXPECT_NE(*rebuilt, *first);
    EXPECT_EQ(counts.Creates, 2u);
    EXPECT_EQ(counts.Sets, 2u);
    EXPECT_EQ(counts.Flushes, 2u);
    ASSERT_TRUE(registry.Retain(*rebuilt));
    ASSERT_TRUE(registry.Release(*rebuilt));
}

// Frame stamps keep hot frame loops allocation-free while still deduplicating one pin per generation.
// The reverse resource index must drop the dependency when the block is finally destroyed.
TEST_P(RendererRegistry, FrameStampRetainPinsOnceAndReverseIndexDropsOnDestroy) {
    const auto layout = Layout(0, "Data");
    ASSERT_TRUE(layout);
    auto buffer = Context.Device->CreateBuffer({512, render::MemoryType::Upload, render::BufferUse::CBuffer});
    ASSERT_TRUE(buffer);
    const auto owner = Guid::NewGuid();
    BindingRegistry registry{Context.Device.get(), owner};
    const ResourceId resource{owner, 0, 1};
    ParameterWrite write{layout->FindBinding("Data"), 0, render::ShaderBufferBinding{buffer.Get(), {0, 256}, 0}};
    const auto block = registry.Resolve(layout.get(), 0, std::span{&write, 1}, std::span{&resource, 1});
    ASSERT_TRUE(block);
    EXPECT_TRUE(registry.HasResource(resource));
    bool newly = false;
    ASSERT_TRUE(registry.Retain(*block, 7, newly));
    EXPECT_TRUE(newly);
    ASSERT_TRUE(registry.Retain(*block, 7, newly));
    EXPECT_FALSE(newly);
    ASSERT_TRUE(registry.Retain(*block, 8, newly));
    EXPECT_TRUE(newly);
    ASSERT_TRUE(registry.Retain(*block, 9, newly));
    EXPECT_TRUE(newly);
    // Three flights pin once each; the block only dies after all three release.
    registry.SetBudget({0, 0, 0});
    ASSERT_TRUE(registry.Release(*block));
    EXPECT_TRUE(registry.IsValid(*block));
    EXPECT_TRUE(registry.HasResource(resource));
    ASSERT_TRUE(registry.Release(*block));
    EXPECT_TRUE(registry.IsValid(*block));
    EXPECT_TRUE(registry.HasResource(resource));
    ASSERT_TRUE(registry.Release(*block));
    EXPECT_FALSE(registry.IsValid(*block));
    EXPECT_FALSE(registry.HasResource(resource));
}

TEST_P(RendererRegistry, EqualBytesShareRangesUntilBothFrameAndDescriptorUsesEnd) {
    const auto layout = Layout(0, "Data");
    ASSERT_TRUE(layout);
    const auto owner = Guid::NewGuid();
    RegistryProbeCounts counts;
    RegistryProbeDevice device{Context.Device.get(), counts};
    RendererResources resources{&device, owner};
    BindingRegistry bindings{&device, owner};
    const array<float, 4> data{1, .5f, .25f, 1};
    const auto first = resources.StoreData(std::as_bytes(std::span{data}));
    ASSERT_TRUE(first);
    EXPECT_EQ(first, resources.StoreData(std::as_bytes(std::span{data})));
    ASSERT_TRUE(resources.RetainFrame(*first));
    ParameterWrite write{layout->FindBinding("Data"), 0, *resources.Value(*first)};
    const auto block = bindings.Resolve(layout.get(), 0, std::span{&write, 1}, std::span{&*first, 1});
    ASSERT_TRUE(block);
    ASSERT_TRUE(bindings.Retain(*block));
    ASSERT_TRUE(resources.RequestExit(*first, bindings));
    EXPECT_FALSE(resources.RetainFrame(*first));
    EXPECT_TRUE(resources.Collect(bindings).empty());
    ASSERT_TRUE(resources.ReleaseFrame(*first));
    EXPECT_TRUE(resources.Collect(bindings).empty());
    ASSERT_TRUE(bindings.Release(*block));
    EXPECT_EQ(resources.Collect(bindings), vector<ResourceId>{*first});
    const auto replacement = resources.StoreData(std::as_bytes(std::span{data}));
    ASSERT_TRUE(replacement);
    EXPECT_NE(first, replacement);
    EXPECT_FALSE(resources.IsValid(*first, true));
}

TEST_P(RendererRegistry, TextureExitWaitsForDependentViewsRegistrationsAndFrames) {
    auto texture = Context.Device->CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                                  render::MemoryType::Device, render::TextureUse::Resource});
    ASSERT_TRUE(texture);
    const auto owner = Guid::NewGuid();
    RendererResources resources{Context.Device.get(), owner};
    BindingRegistry bindings{Context.Device.get(), owner};
    const auto imported = resources.ImportTexture(texture.Get());
    ASSERT_TRUE(imported);
    const render::TextureViewDescriptor desc{texture.Get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource};
    const auto view = resources.CreateView(*imported, desc);
    ASSERT_TRUE(view);
    EXPECT_EQ(view, resources.CreateView(*imported, desc));
    ASSERT_TRUE(resources.RetainRegistration(*view));
    ASSERT_TRUE(resources.RetainFrame(*view));
    ASSERT_TRUE(resources.RequestExit(*imported, bindings));
    EXPECT_TRUE(resources.Collect(bindings).empty());
    EXPECT_FALSE(resources.CreateView(*imported, desc));
    ASSERT_TRUE(resources.ReleaseRegistration(*view));
    EXPECT_TRUE(resources.Collect(bindings).empty());
    ASSERT_TRUE(resources.ReleaseFrame(*view));
    const auto completed = resources.Collect(bindings);
    EXPECT_EQ(completed.size(), 2u);
    EXPECT_NE(std::find(completed.begin(), completed.end(), *imported), completed.end());
    EXPECT_FALSE(resources.IsValid(*imported, true));
}

TEST_P(RendererRegistry, SamplerIdentityUsesNativeStateInsteadOfObjectAddress) {
    const render::SamplerDescriptor desc{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
                                         render::FilterMode::Nearest, render::FilterMode::Nearest, render::FilterMode::Nearest, 0, 1};
    auto a = Context.Device->CreateSampler(desc), b = Context.Device->CreateSampler(desc);
    ASSERT_TRUE(a && b);
    EXPECT_NE(a.Get(), b.Get());
    EXPECT_EQ(render::GetShaderParameterValueIdentity(a.Get()), render::GetShaderParameterValueIdentity(b.Get()));
    auto changed = desc;
    changed.MagFilter = render::FilterMode::Linear;
    auto c = Context.Device->CreateSampler(changed);
    ASSERT_TRUE(c);
    EXPECT_NE(render::GetShaderParameterValueIdentity(a.Get()), render::GetShaderParameterValueIdentity(c.Get()));
}

TEST_P(RendererRegistry, AdmittedExitWaitsForSubmissionFenceAndHostReleaseAcrossFlightCounts) {
    for (const uint32_t flights : {1u, 2u, 3u}) {
        SCOPED_TRACE(flights);
        FrameTimeline timeline{flights};
        RuntimeStartupResult startup;
        auto host = GpuSystem::TryAttachDevice({Context.Device, 0, 3, flights, false}, timeline, startup);
        ASSERT_TRUE(host) << startup.Reason;
        RenderSystem services{Context.Device.get()};
        ASSERT_TRUE(services.OnInitialize());
        const auto layout = Layout(0, "Data");
        ASSERT_TRUE(layout);
        Renderer renderer{*host, services};
        auto gate = Context.Device->CreateFence().Unwrap();
        auto context = host->BeginFrameRecord(0, {}, {}, false);
        bool submitted = false, signaled = false;
        auto cleanup = MakeScopeGuard([&]() noexcept {
            if (!signaled) Signal(gate.get());
            if (!submitted) context.SubmitFrame();
            host->WaitAndRetireFlights();
            FlightCompletion complete;
            while (timeline.TryReadCompletion(complete)) host->ReleaseFrameResourcesGT(complete);
            renderer.ProcessCompletions();
        });
        auto frame = renderer.BeginFrame(context);
        ASSERT_TRUE(frame);
        const array<float, 4> bytes{1, 2, 3, 4};
        const auto data = frame->StoreData(std::as_bytes(std::span{bytes}));
        ASSERT_TRUE(data);
        ParameterWrite write{layout->FindBinding("Data"), 0, *renderer.GetResourceValue(*data)};
        ASSERT_TRUE(frame->AddBindings(layout.get(), 0, std::span{&write, 1}, std::span{&*data, 1}));
        ASSERT_TRUE(renderer.RequestResourceExit(*data));
        EXPECT_TRUE(renderer.TakeResourceExitConfirmations().empty());
        EXPECT_FALSE(renderer.Shutdown());
        // The accepted use can still compile after an exit request, but another frame cannot acquire it.
        ASSERT_TRUE(frame->Compile());
        render::Fence* wait = gate.get();
        uint64_t value = 1;
        context.ReturnCommandBuffers({.WaitFences = std::span{&wait, 1}, .WaitValues = std::span{&value, 1}});
        context.SubmitFrame();
        submitted = true;
        EXPECT_FALSE(renderer.BeginFrame(context));
        EXPECT_FALSE(frame->StoreData(std::as_bytes(std::span{bytes})));
        EXPECT_FALSE(host->CompleteFlightIfReady(0, false));
        renderer.ProcessCompletions();
        EXPECT_TRUE(renderer.TakeResourceExitConfirmations().empty());
        EXPECT_TRUE(renderer.GetResourceValue(*data));
        signaled = Signal(gate.get());
        ASSERT_TRUE(signaled);
        ASSERT_TRUE(host->CompleteFlightIfReady(0, true));
        renderer.ProcessCompletions();
        EXPECT_TRUE(renderer.TakeResourceExitConfirmations().empty());
        FlightCompletion complete;
        ASSERT_TRUE(timeline.TryReadCompletion(complete));
        host->ReleaseFrameResourcesGT(complete);
        renderer.ProcessCompletions();
        EXPECT_EQ(renderer.TakeResourceExitConfirmations(), vector<ResourceId>{*data});
        EXPECT_FALSE(renderer.GetResourceValue(*data));
        EXPECT_FALSE(frame->StoreData(std::as_bytes(std::span{bytes})));
        EXPECT_TRUE(renderer.Shutdown());
    }
}

TEST_P(RendererRegistry, ForeignHostAndFailedCompileCannotReleaseAcceptedWork) {
    FrameTimeline timeline{1}, foreignTimeline{1};
    RuntimeStartupResult startup;
    auto host = GpuSystem::TryAttachDevice({Context.Device, 0, 3, 1, false}, timeline, startup);
    auto foreign = GpuSystem::TryAttachDevice({Context.Device, 0, 3, 1, false}, foreignTimeline, startup);
    ASSERT_TRUE(host && foreign);
    RenderSystem services{Context.Device.get()};
    ASSERT_TRUE(services.OnInitialize());
    const auto layout = Layout(0, "Data");
    ASSERT_TRUE(layout);
    Renderer renderer{*host, services};
    auto context = host->BeginFrameRecord(0, {}, {}, false);
    auto other = foreign->BeginFrameRecord(0, {}, {}, false);
    auto cleanup = MakeScopeGuard([&]() noexcept {
        context.SubmitFrame();
        other.SubmitFrame();
        host->WaitAndRetireFlights();
        foreign->WaitAndRetireFlights();
        FlightCompletion complete;
        while (timeline.TryReadCompletion(complete)) host->ReleaseFrameResourcesGT(complete);
        while (foreignTimeline.TryReadCompletion(complete)) foreign->ReleaseFrameResourcesGT(complete);
        renderer.ProcessCompletions();
    });
    EXPECT_FALSE(renderer.BeginFrame(other));
    auto frame = renderer.BeginFrame(context);
    ASSERT_TRUE(frame);
    const array<float, 4> bytes{1, 2, 3, 4};
    const auto data = frame->StoreData(std::as_bytes(std::span{bytes}));
    ASSERT_TRUE(data);
    // Incomplete bindings fail compilation after the data has already been accepted.
    ASSERT_TRUE(frame->AddBindings(layout.get(), 0, {}, std::span{&*data, 1}));
    EXPECT_FALSE(frame->Compile());
    ASSERT_TRUE(renderer.RequestResourceExit(*data));
    renderer.TrimIdleBindings();
    EXPECT_TRUE(renderer.TakeResourceExitConfirmations().empty());
    EXPECT_FALSE(renderer.Shutdown());
}

class RegistryExternalAsset final : public Asset {
public:
    RegistryExternalAsset(unique_ptr<render::Buffer> buffer, uint32_t& destroyed) : Buffer(std::move(buffer)), Destroyed(destroyed) {}
    ~RegistryExternalAsset() noexcept override { ++Destroyed; }
    void OnUnload(AssetManager&) override {}
    unique_ptr<render::Buffer> Buffer;
    uint32_t& Destroyed;
};
TEST_P(RendererRegistry, SharedSceneAssetExitRequiresRendererConfirmationAfterTrueCompletion) {
    uint32_t destroyed = 0;
    AssetManager assets;
    RenderAssetLifetime firstLifetime{1}, secondLifetime{1};
    FrameTimeline timeline{1};
    RuntimeStartupResult startup;
    auto host = GpuSystem::TryAttachDevice({Context.Device, 0, 3, 1, false}, timeline, startup);
    ASSERT_TRUE(host);
    RenderSystem services{Context.Device.get()};
    ASSERT_TRUE(services.OnInitialize());
    Renderer renderer{*host, services};
    auto buffer = Context.Device->CreateBuffer({512, render::MemoryType::Upload, render::BufferUse::CBuffer | render::BufferUse::Vertex | render::BufferUse::MapWrite});
    ASSERT_TRUE(buffer);
    std::memset(buffer->Map(0, 512), 0, 512);
    buffer->FlushMappedRange({0, 512});
    buffer->Unmap();
    const auto assetId = Guid::NewGuid();
    auto owner = assets.AddReady(assetId, make_unique<RegistryExternalAsset>(buffer.Release(), destroyed));
    firstLifetime.AddUse(owner.AsAny());
    secondLifetime.AddUse(owner.AsAny());
    GpuMesh::DrawData geometry;
    geometry.VertexBuffers.push_back({0, {owner->Buffer.get(), 0, 512}});
    const RendererSceneKey first{Guid::NewGuid(), {0, 1}}, second{Guid::NewGuid(), {0, 1}};
    const auto a = renderer.GetScenes().Geometry(first, assetId, geometry);
    const auto b = renderer.GetScenes().Geometry(second, assetId, geometry);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->Vertices[0].Buffer, b->Vertices[0].Buffer);
    owner.Reset();
    const auto root = a->Vertices[0].Buffer;
    const auto layout = Layout(0, "Data");
    ASSERT_TRUE(layout);
    auto gate = Context.Device->CreateFence().Unwrap();
    auto context = host->BeginFrameRecord(0, {}, {}, false);
    bool signaled = false;
    auto cleanup = MakeScopeGuard([&] {
        if (!signaled) Signal(gate.get());
        if (context.IsRecording()) context.SubmitFrame();
        host->WaitAndRetireFlights();
        FlightCompletion completion;
        while (timeline.TryReadCompletion(completion)) host->ReleaseFrameResourcesGT(completion);
        renderer.ProcessCompletions();
        renderer.Shutdown();
    });
    auto frame = renderer.BeginFrame(context);
    ASSERT_TRUE(frame);
    const auto range = renderer.CreateBufferRange(root, {0, 256});
    ASSERT_TRUE(range);
    const ParameterWrite write{layout->FindBinding("Data"), 0, *renderer.GetResourceValue(*range)};
    ASSERT_TRUE(frame->AddBindings(layout.get(), 0, std::span{&write, 1}, std::span{&*range, 1}));
    ASSERT_TRUE(frame->Compile());
    firstLifetime.RemoveUse(assetId);
    auto exits = firstLifetime.SealRetirements(0, true);
    renderer.GetScenes().Apply(first, {}, exits, true);
    auto confirmed = renderer.GetScenes().TakeConfirmations(first.Delivery);
    ASSERT_EQ(confirmed.size(), 1u);
    firstLifetime.ConfirmExit(confirmed[0].Token);
    secondLifetime.RemoveUse(assetId);
    exits = secondLifetime.SealRetirements(0, true);
    renderer.GetScenes().Apply(second, {}, exits, true);
    EXPECT_TRUE(renderer.GetScenes().TakeConfirmations(second.Delivery).empty());
    render::Fence* wait = gate.get();
    uint64_t value = 1;
    context.ReturnCommandBuffers({.WaitFences = std::span{&wait, 1}, .WaitValues = std::span{&value, 1}});
    context.SubmitFrame();
    EXPECT_FALSE(host->CompleteFlightIfReady(0, false));
    assets.Pump();
    EXPECT_EQ(destroyed, 0u);
    signaled = Signal(gate.get());
    ASSERT_TRUE(signaled);
    ASSERT_TRUE(host->CompleteFlightIfReady(0, true));
    FlightCompletion completion;
    ASSERT_TRUE(timeline.TryReadCompletion(completion));
    host->ReleaseFrameResourcesGT(completion);
    firstLifetime.ReleaseFlight(0);
    secondLifetime.ReleaseFlight(0);
    assets.Pump();
    EXPECT_EQ(destroyed, 0u);
    renderer.ProcessCompletions();
    confirmed = renderer.GetScenes().TakeConfirmations(second.Delivery);
    ASSERT_EQ(confirmed.size(), 1u);
    EXPECT_TRUE(renderer.IsResourceRetired(root));
    EXPECT_TRUE(renderer.TakeResourceExitConfirmations().empty());
    secondLifetime.ConfirmExit(confirmed[0].Token);
    assets.Pump();
    EXPECT_EQ(destroyed, 1u);
}
struct FramePreparationProbe {
    uint32_t Prepared{0}, Destroyed{0};
    vector<ResourceId> Data;
};
class RegistryPreparation final : public FramePreparation {
public:
    RegistryPreparation(FramePreparationProbe& probe, Renderer& renderer, render::PipelineLayout* layout) : Probe(probe), Owner(renderer), Layout(layout) {}
    ~RegistryPreparation() noexcept override { ++Probe.Destroyed; }
    bool Prepare(FrameBuilder& builder) override {
        ++Probe.Prepared;
        EXPECT_FALSE(builder.Compile());
        const array<float, 4> bytes{1, 2, 3, 4};
        const auto data = builder.StoreData(std::as_bytes(std::span{bytes}));
        if (!data) return false;
        Probe.Data.push_back(*data);
        const ParameterWrite write{Layout->FindBinding("Data"), 0, *Owner.GetResourceValue(*data)};
        return builder.AddBindings(Layout, 0, std::span{&write, 1}, std::span{&*data, 1}).has_value();
    }
    FramePreparationProbe& Probe;
    Renderer& Owner;
    render::PipelineLayout* Layout;
};
TEST_P(RendererRegistry, FrameOwnsAllPreparationsAndCompilesOnlyAfterCollection) {
    FramePreparationProbe probe;
    FrameTimeline timeline{1};
    RuntimeStartupResult startup;
    auto host = GpuSystem::TryAttachDevice({Context.Device, 0, 3, 1, false}, timeline, startup);
    ASSERT_TRUE(host);
    RenderSystem services{Context.Device.get()};
    ASSERT_TRUE(services.OnInitialize());
    const auto layout = Layout(0, "Data");
    ASSERT_TRUE(layout);
    Renderer renderer{*host, services};
    auto context = host->BeginFrameRecord(0, {}, {}, false);
    auto cleanup = MakeScopeGuard([&] {
        if (context.IsRecording()) context.SubmitFrame();
        host->WaitAndRetireFlights();
        FlightCompletion completion;
        while (timeline.TryReadCompletion(completion)) host->ReleaseFrameResourcesGT(completion);
        renderer.ProcessCompletions();
    });
    auto frame = renderer.BeginFrame(context);
    ASSERT_TRUE(frame);
    const auto first = frame->AddPreparation(make_unique<RegistryPreparation>(probe, renderer, layout.get()));
    const auto second = frame->AddPreparation(make_unique<RegistryPreparation>(probe, renderer, layout.get()));
    ASSERT_TRUE(first && second);
    EXPECT_EQ(probe.Prepared, 0u);
    EXPECT_EQ(probe.Destroyed, 0u);
    EXPECT_FALSE(frame->GetPreparation(*first));
    ASSERT_TRUE(frame->Compile());
    EXPECT_EQ(probe.Prepared, 2u);
    EXPECT_EQ(probe.Data[0], probe.Data[1]);
    EXPECT_TRUE(frame->GetPreparation(*first));
    EXPECT_FALSE(frame->AddPreparation(make_unique<RegistryPreparation>(probe, renderer, layout.get())));
    EXPECT_EQ(probe.Destroyed, 1u);
    context.SubmitFrame();
    host->WaitAndRetireFlights();
    EXPECT_EQ(probe.Destroyed, 1u);
    FlightCompletion completion;
    ASSERT_TRUE(timeline.TryReadCompletion(completion));
    host->ReleaseFrameResourcesGT(completion);
    renderer.ProcessCompletions();
    EXPECT_EQ(probe.Destroyed, 3u);
    EXPECT_FALSE(frame->GetPreparation(*first));
}

#if defined(RADRAY_RASTER_COMPILER_PATH)
TEST_P(RendererRegistry, MaterialReplacementPreservesAcceptedVersionAndRejectsMixedDomains) {
    FrameTimeline timeline{1};
    RuntimeStartupResult startup;
    auto host = GpuSystem::TryAttachDevice({Context.Device, 0, 3, 1, false}, timeline, startup);
    ASSERT_TRUE(host);
    RenderSystem services{Context.Device.get()};
    ASSERT_TRUE(services.OnInitialize());
    Renderer renderer{*host, services};
    const auto program = Program(services, false), mixed = Program(services, true);
    ASSERT_TRUE(program && mixed);
    MaterialTemplateDescriptor definition;
    definition.Name = "Color";
    definition.RegisteredProgram = mixed->GetId();
    definition.Inputs = {{"ViewData", MaterialInputSource::ViewConstants}, {"Tint", MaterialInputSource::MaterialConstants}};
    EXPECT_FALSE(renderer.RegisterTemplate(definition));
    EXPECT_NE(renderer.GetLastError().find("mixes binding domains"), string::npos);
    definition.RegisteredProgram = program->GetId();
    const auto templateId = renderer.RegisterTemplate(definition);
    ASSERT_TRUE(templateId) << renderer.GetLastError();
    const array<float, 4> red{1, 0, 0, 1}, green{0, 1, 0, 1}, view{.5f, 0, 0, 0};
    const auto copy = [](const auto& values) { const auto bytes = std::as_bytes(std::span{values}); return vector<byte>{bytes.begin(), bytes.end()}; };
    RendererMaterialDescriptor descriptor{{*templateId}, {copy(red)}, {}, {}};
    const auto old = renderer.RegisterMaterial(descriptor);
    ASSERT_TRUE(old);
    auto context = host->BeginFrameRecord(0, {}, {}, false);
    auto cleanup = MakeScopeGuard([&]() noexcept {
        if (context.IsRecording()) context.SubmitFrame();
        host->WaitAndRetireFlights();
        FlightCompletion complete;
        while (timeline.TryReadCompletion(complete)) host->ReleaseFrameResourcesGT(complete);
        renderer.ProcessCompletions();
    });
    auto frame = renderer.BeginFrame(context);
    ASSERT_TRUE(frame);
    const auto viewId = frame->AddView({{copy(view)}});
    ASSERT_TRUE(viewId);
    const auto pass = frame->AddPass({*viewId, *templateId, {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}}});
    ASSERT_TRUE(pass);
    RendererDrawDescriptor draw;
    draw.Count = 3;
    draw.Viewport = {0, 0, 16, 16, 0, 1};
    draw.Scissor = {0, 0, 16, 16};
    ASSERT_TRUE(frame->AddDraw(*pass, *old, draw));
    descriptor.Constants = {copy(green)};
    const auto replacement = renderer.ReplaceMaterial(*old, descriptor);
    ASSERT_TRUE(replacement);
    EXPECT_NE(*old, *replacement);
    EXPECT_FALSE(frame->AddDraw(*pass, *old, draw));
    auto foreign = *replacement;
    foreign.Owner = Guid::NewGuid();
    EXPECT_FALSE(frame->AddDraw(*pass, foreign, draw));
    ASSERT_TRUE(frame->AddDraw(*pass, *replacement, draw));
    ASSERT_TRUE(renderer.UnregisterTemplate(*templateId));
    ASSERT_TRUE(frame->Compile()) << renderer.GetLastError();
    context.SubmitFrame();
    host->WaitAndRetireFlights();
    FlightCompletion complete;
    while (timeline.TryReadCompletion(complete)) host->ReleaseFrameResourcesGT(complete);
    renderer.ProcessCompletions();
    EXPECT_TRUE(renderer.UnregisterMaterial(*replacement));
    EXPECT_FALSE(renderer.GetTemplate(*templateId));
    const auto nextTemplate = renderer.RegisterTemplate(definition);
    ASSERT_TRUE(nextTemplate);
    EXPECT_EQ(nextTemplate->Index, templateId->Index);
    EXPECT_NE(nextTemplate->Generation, templateId->Generation);
    descriptor.Templates = {*nextTemplate};
    const auto next = renderer.RegisterMaterial(descriptor);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->Index, old->Index);
    EXPECT_NE(next->Generation, old->Generation);
    EXPECT_FALSE(renderer.UnregisterMaterial(*old));
}

// The material group cache compiles once, is reused across frames and rebuilt after an explicit trim;
// every frame still acquires and releases its own pin, so completion drains cleanly. Block-level reuse
// counts are asserted at the registry layer (`MaterialGroupWritesReuseOneBlockAndRebuildAfterTrim`); the
// Renderer device cannot be wrapped by the test probe because the runtime casts it to backend types.
TEST_P(RendererRegistry, MaterialGroupCacheSurvivesFramesAndRebuildsAfterTrim) {
    FrameTimeline timeline{1};
    RuntimeStartupResult startup;
    auto host = GpuSystem::TryAttachDevice({Context.Device, 0, 3, 1, false}, timeline, startup);
    ASSERT_TRUE(host);
    RenderSystem services{Context.Device.get()};
    ASSERT_TRUE(services.OnInitialize());
    Renderer renderer{*host, services};
    const auto program = Program(services, false);
    ASSERT_TRUE(program);
    MaterialTemplateDescriptor definition;
    definition.Name = "Cached";
    definition.RegisteredProgram = program->GetId();
    definition.Inputs = {{"ViewData", MaterialInputSource::ViewConstants}, {"Tint", MaterialInputSource::MaterialConstants}};
    const auto templateId = renderer.RegisterTemplate(definition);
    ASSERT_TRUE(templateId) << renderer.GetLastError();
    const auto copy = [](const auto& values) { const auto bytes = std::as_bytes(std::span{values}); return vector<byte>{bytes.begin(), bytes.end()}; };
    const array<float, 4> red{1, 0, 0, 1}, view{.5f, 0, 0, 0};
    RendererMaterialDescriptor descriptor{{*templateId}, {copy(red)}, {}, {}};
    const auto material = renderer.RegisterMaterial(descriptor);
    ASSERT_TRUE(material);
    for (uint32_t iteration = 0; iteration < 3; ++iteration) {
        SCOPED_TRACE(iteration);
        auto context = host->BeginFrameRecord(0, {}, {}, false);
        bool submitted = false;
        auto cleanup = MakeScopeGuard([&]() noexcept {
            if (!submitted && context.IsRecording()) context.SubmitFrame();
            host->WaitAndRetireFlights();
            FlightCompletion complete;
            while (timeline.TryReadCompletion(complete)) host->ReleaseFrameResourcesGT(complete);
            renderer.ProcessCompletions();
        });
        auto frame = renderer.BeginFrame(context);
        ASSERT_TRUE(frame);
        const auto viewId = frame->AddView({{copy(view)}});
        ASSERT_TRUE(viewId);
        const auto pass = frame->AddPass({*viewId, *templateId, {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}}});
        ASSERT_TRUE(pass);
        RendererDrawDescriptor draw;
        draw.Count = 3;
        draw.Viewport = {0, 0, 16, 16, 0, 1};
        draw.Scissor = {0, 0, 16, 16};
        ASSERT_TRUE(frame->AddDraw(*pass, *material, draw));
        if (iteration == 1) renderer.TrimIdleBindings();
        ASSERT_TRUE(frame->Compile()) << renderer.GetLastError();
        context.SubmitFrame();
        submitted = true;
        host->WaitAndRetireFlights();
        FlightCompletion complete;
        while (timeline.TryReadCompletion(complete)) host->ReleaseFrameResourcesGT(complete);
        renderer.ProcessCompletions();
    }
    EXPECT_TRUE(renderer.Shutdown());
}

// The frame memo must bind the same parameter set exactly when a group reads the same ordered resources;
// a wrong hit binds the wrong descriptor and is invisible to a "Compile succeeded" assertion. This
// observes the sets through a recording encoder and names the two groups by which one follows the view.
TEST_P(RendererRegistry, FrameGroupMemoSharesIdenticalInputsAndSplitsDistinctOnes) {
    FrameTimeline timeline{1};
    RuntimeStartupResult startup;
    auto host = GpuSystem::TryAttachDevice({Context.Device, 0, 3, 1, false}, timeline, startup);
    ASSERT_TRUE(host);
    RenderSystem services{Context.Device.get()};
    ASSERT_TRUE(services.OnInitialize());
    Renderer renderer{*host, services};
    const auto program = Program(services, false);
    ASSERT_TRUE(program);
    MaterialTemplateDescriptor definition;
    definition.Name = "Memo";
    definition.RegisteredProgram = program->GetId();
    definition.Inputs = {{"ViewData", MaterialInputSource::ViewConstants}, {"Tint", MaterialInputSource::MaterialConstants}};
    const auto templateId = renderer.RegisterTemplate(definition);
    ASSERT_TRUE(templateId) << renderer.GetLastError();
    const auto copy = [](const auto& values) { const auto bytes = std::as_bytes(std::span{values}); return vector<byte>{bytes.begin(), bytes.end()}; };
    const array<float, 4> tint{1, 0, 0, 1}, overrideTint{0, 1, 0, 1};
    RendererMaterialDescriptor descriptor{{*templateId}, {copy(tint)}, {}, {}};
    const auto material = renderer.RegisterMaterial(descriptor);
    ASSERT_TRUE(material);
    struct ViewSpec {
        float Seed;
        uint32_t Passes;
    };
    const auto run = [&](std::span<const ViewSpec> views, std::span<const byte> overrideBytes) -> std::optional<vector<vector<RecordingEncoder::Binding>>> {
        auto context = host->BeginFrameRecord(0, {}, {}, false);
        bool submitted = false;
        auto cleanup = MakeScopeGuard([&]() noexcept {
            if (!submitted && context.IsRecording()) context.SubmitFrame();
            host->WaitAndRetireFlights();
            FlightCompletion complete;
            while (timeline.TryReadCompletion(complete)) host->ReleaseFrameResourcesGT(complete);
            renderer.ProcessCompletions();
        });
        auto frame = renderer.BeginFrame(context);
        if (!frame) return std::nullopt;
        vector<FrameDrawId> draws;
        for (const auto& spec : views) {
            const array<float, 4> view{spec.Seed, 0, 0, 0};
            const auto viewId = frame->AddView({{copy(view)}});
            if (!viewId) return std::nullopt;
            for (uint32_t p = 0; p < spec.Passes; ++p) {
                const auto pass = frame->AddPass({*viewId, *templateId, {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}}});
                if (!pass) return std::nullopt;
                RendererDrawDescriptor draw;
                draw.Count = 3;
                draw.Viewport = {0, 0, 16, 16, 0, 1};
                draw.Scissor = {0, 0, 16, 16};
                RendererDrawValues values;
                if (!overrideBytes.empty()) values.Constants.push_back({overrideBytes.begin(), overrideBytes.end()});
                const auto id = frame->AddDraw(*pass, *material, draw, std::move(values));
                if (!id) return std::nullopt;
                draws.push_back(*id);
            }
        }
        if (!frame->Compile()) {
            EXPECT_TRUE(false) << renderer.GetLastError();
            return std::nullopt;
        }
        RecordingEncoder encoder;
        vector<vector<RecordingEncoder::Binding>> recorded;
        for (const auto id : draws) {
            encoder.Bindings.clear();
            if (!frame->Record(&encoder, id)) return std::nullopt;
            recorded.push_back(encoder.Bindings);
        }
        context.SubmitFrame();
        submitted = true;
        return recorded;
    };
    const auto setOf = [](const vector<RecordingEncoder::Binding>& bindings, uint32_t group) -> render::ShaderParameterSet* {
        for (const auto& binding : bindings)
            if (binding.Group == group) return binding.Set;
        return nullptr;
    };
    const ViewSpec oneView[]{{0.25f, 2}}, twoViews[]{{0.5f, 1}, {0.75f, 1}}, firstView[]{{0.25f, 1}}, secondView[]{{0.5f, 1}};
    const auto same = run(oneView, {});
    ASSERT_TRUE(same);
    const auto distinct = run(twoViews, {});
    ASSERT_TRUE(distinct);
    const auto equalOverride = run(firstView, std::as_bytes(std::span{tint}));
    ASSERT_TRUE(equalOverride);
    const auto otherOverride = run(secondView, std::as_bytes(std::span{overrideTint}));
    ASSERT_TRUE(otherOverride);
    ASSERT_EQ(same->at(0).size(), distinct->at(0).size());
    uint32_t viewGroup = UINT32_MAX, materialGroup = UINT32_MAX;
    for (const auto& binding : same->at(0)) {
        if (binding.Set == setOf(distinct->at(0), binding.Group)) materialGroup = binding.Group;
        else viewGroup = binding.Group;
    }
    ASSERT_NE(viewGroup, UINT32_MAX) << "no group follows the view; the memo would alias distinct views";
    ASSERT_NE(materialGroup, UINT32_MAX);
    // Two passes on one view are the same inputs, so every group binds the same set.
    for (const auto& binding : same->at(1)) EXPECT_EQ(binding.Set, setOf(same->at(0), binding.Group));
    // Distinct views split the view group and keep the material group.
    EXPECT_NE(setOf(distinct->at(0), viewGroup), setOf(distinct->at(1), viewGroup));
    EXPECT_EQ(setOf(distinct->at(0), materialGroup), setOf(distinct->at(1), materialGroup));
    // The material group cache is reused across frames, and a same-valued override reuses the default block.
    EXPECT_EQ(setOf(distinct->at(0), materialGroup), setOf(same->at(0), materialGroup));
    EXPECT_EQ(setOf(equalOverride->at(0), materialGroup), setOf(same->at(0), materialGroup));
    // A different override, and a different view, each get their own block.
    EXPECT_NE(setOf(otherOverride->at(0), materialGroup), setOf(same->at(0), materialGroup));
    EXPECT_NE(setOf(otherOverride->at(0), viewGroup), setOf(same->at(0), viewGroup));
    EXPECT_TRUE(renderer.Shutdown());
}

// The exit path must interrupt an already-cached material group: frame N builds and pins the material
// block, the exit stays pending until frame N completes and the material registration is released, and
// frame N+1's draw is rejected in the meantime. The pending confirmation is the observable proof that a
// dependent block still existed at request time and is gone only after completion.
TEST_P(RendererRegistry, ExitingMaterialTextureRejectsDrawAndConfirmsAfterCachedFrame) {
    FrameTimeline timeline{1};
    RuntimeStartupResult startup;
    auto host = GpuSystem::TryAttachDevice({Context.Device, 0, 3, 1, false}, timeline, startup);
    ASSERT_TRUE(host);
    RenderSystem services{Context.Device.get()};
    ASSERT_TRUE(services.OnInitialize());
    Renderer renderer{*host, services};
    const auto program = Program(services, false, true);
    ASSERT_TRUE(program);
    MaterialTemplateDescriptor definition;
    definition.Name = "Textured";
    definition.RegisteredProgram = program->GetId();
    definition.Inputs = {{"ViewData", MaterialInputSource::ViewConstants}, {"Tint", MaterialInputSource::MaterialConstants}, {"Albedo", MaterialInputSource::Texture}};
    const auto templateId = renderer.RegisterTemplate(definition);
    ASSERT_TRUE(templateId) << renderer.GetLastError();
    auto texture = Context.Device->CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                                  render::MemoryType::Device, render::TextureUse::Resource});
    ASSERT_TRUE(texture);
    const auto root = renderer.ImportTexture(texture.Get());
    ASSERT_TRUE(root);
    const render::TextureViewDescriptor viewDesc{texture.Get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource};
    const auto view = renderer.CreateTextureView(*root, viewDesc);
    ASSERT_TRUE(view);
    const auto copy = [](const auto& values) { const auto bytes = std::as_bytes(std::span{values}); return vector<byte>{bytes.begin(), bytes.end()}; };
    const array<float, 4> red{1, 0, 0, 1}, viewConstants{.5f, 0, 0, 0};
    RendererMaterialDescriptor descriptor{{*templateId}, {copy(red)}, {*view}, {}};
    const auto material = renderer.RegisterMaterial(descriptor);
    ASSERT_TRUE(material);
    const auto settle = [&]() {
        host->WaitAndRetireFlights();
        FlightCompletion completion;
        while (timeline.TryReadCompletion(completion)) host->ReleaseFrameResourcesGT(completion);
        renderer.ProcessCompletions();
    };
    const auto addDraw = [&](FrameBuilder& frame) {
        const auto viewId = frame.AddView({{copy(viewConstants)}});
        if (!viewId) return std::optional<FrameDrawId>{};
        const auto pass = frame.AddPass({*viewId, *templateId, {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}}});
        if (!pass) return std::optional<FrameDrawId>{};
        RendererDrawDescriptor draw;
        draw.Count = 3;
        draw.Viewport = {0, 0, 16, 16, 0, 1};
        draw.Scissor = {0, 0, 16, 16};
        return frame.AddDraw(*pass, *material, draw);
    };
    // Frame N: a normal draw builds the material block through the cache and pinning it keeps the block
    // alive while the frame is still in flight.
    auto context = host->BeginFrameRecord(0, {}, {}, false);
    auto frame = renderer.BeginFrame(context);
    ASSERT_TRUE(frame);
    ASSERT_TRUE(addDraw(*frame));
    ASSERT_TRUE(frame->Compile()) << renderer.GetLastError();
    context.SubmitFrame();
    // Requesting the exit while frame N still pins its block must not confirm: the retiring block only
    // dies when the frame releases it.
    ASSERT_TRUE(renderer.RequestResourceExit(*view));
    EXPECT_TRUE(renderer.TakeResourceExitConfirmations().empty());
    settle();
    // The block is gone now, but the material registration still holds the view, so the exit stays pending.
    EXPECT_TRUE(renderer.TakeResourceExitConfirmations().empty());
    // Frame N+1: the retiring texture view rejects the draw through UseMaterial.
    auto next = host->BeginFrameRecord(0, {}, {}, false);
    auto nextFrame = renderer.BeginFrame(next);
    ASSERT_TRUE(nextFrame);
    const auto nextView = nextFrame->AddView({{copy(viewConstants)}});
    ASSERT_TRUE(nextView);
    const auto nextPass = nextFrame->AddPass({*nextView, *templateId, {16, 16, 1, {render::TextureFormat::RGBA8_UNORM}, {}}});
    ASSERT_TRUE(nextPass);
    RendererDrawDescriptor rejected;
    rejected.Count = 3;
    rejected.Viewport = {0, 0, 16, 16, 0, 1};
    rejected.Scissor = {0, 0, 16, 16};
    EXPECT_FALSE(nextFrame->AddDraw(*nextPass, *material, rejected));
    ASSERT_TRUE(nextFrame->Compile());
    next.SubmitFrame();
    settle();
    EXPECT_TRUE(renderer.TakeResourceExitConfirmations().empty());
    // Releasing the registration lets the exit complete; the confirmation proves the cached block is gone.
    ASSERT_TRUE(renderer.UnregisterMaterial(*material));
    const auto confirmations = renderer.TakeResourceExitConfirmations();
    EXPECT_NE(std::find(confirmations.begin(), confirmations.end(), *view), confirmations.end());
    EXPECT_TRUE(renderer.Shutdown());
}
#endif
INSTANTIATE_TEST_SUITE_P(Backends, RendererRegistry, ::testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray::test
