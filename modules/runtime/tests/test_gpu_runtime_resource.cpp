#include <atomic>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#ifdef RADRAY_ENABLE_IMGUI
#include <radray/runtime/imgui_system.h>
#endif

using namespace radray;
using namespace radray::render;

namespace {

#ifdef RADRAY_IS_DEBUG
#define EXPECT_GPU_GUARD_ASSERT(statement) \
    EXPECT_DEATH_IF_SUPPORTED({ statement; }, "")
#else
#define EXPECT_GPU_GUARD_ASSERT(statement) \
    do {                                  \
    } while (false)
#endif

std::string_view BackendTestName(RenderBackend backend) noexcept {
    switch (backend) {
        case RenderBackend::D3D12: return "D3D12";
        case RenderBackend::Vulkan: return "Vulkan";
        default: return "Unknown";
    }
}

std::vector<RenderBackend> GetEnabledRuntimeBackends() noexcept {
    std::vector<RenderBackend> backends{};
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(RenderBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.push_back(RenderBackend::Vulkan);
#endif
    return backends;
}

class FakeFence final : public Fence {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}

    uint64_t GetCompletedValue() const noexcept override { return CompletedValue; }
    uint64_t GetLastSignaledValue() const noexcept override { return LastSignaledValue; }

    void Wait() noexcept override { CompletedValue = LastSignaledValue; }
    void Wait(uint64_t value) noexcept override { CompletedValue = std::max(CompletedValue, value); }

    void Signal(uint64_t value) noexcept { LastSignaledValue = std::max(LastSignaledValue, value); }

public:
    uint64_t CompletedValue{0};
    uint64_t LastSignaledValue{0};
};

bool CreateRuntimeForBackend(RenderBackend backend, unique_ptr<GpuRuntime>& runtime, std::string& reason) {
    switch (backend) {
        case RenderBackend::D3D12: {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
            D3D12DeviceDescriptor desc{};
            desc.AdapterIndex = std::nullopt;
            desc.IsEnableDebugLayer = false;
            desc.IsEnableGpuBasedValid = false;
            auto runtimeOpt = GpuRuntime::Create(desc);
            if (!runtimeOpt.HasValue()) {
                reason = "GpuRuntime::Create(D3D12) failed";
                return false;
            }
            runtime = runtimeOpt.Release();
            return true;
#else
            reason = "D3D12 backend is not enabled for this build";
            return false;
#endif
        }
        case RenderBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
            VulkanInstanceDescriptor instanceDesc{};
            instanceDesc.AppName = "GpuRuntimeResourceTest";
            instanceDesc.AppVersion = 1;
            instanceDesc.EngineName = "RadRay";
            instanceDesc.EngineVersion = 1;
            instanceDesc.IsEnableDebugLayer = false;
            instanceDesc.IsEnableGpuBasedValid = false;

            VulkanCommandQueueDescriptor queueDesc{};
            queueDesc.Type = QueueType::Direct;
            queueDesc.Count = 1;

            VulkanDeviceDescriptor deviceDesc{};
            deviceDesc.PhysicalDeviceIndex = std::nullopt;
            deviceDesc.Queues = std::span{&queueDesc, 1};

            auto runtimeOpt = GpuRuntime::Create(deviceDesc, instanceDesc);
            if (!runtimeOpt.HasValue()) {
                reason = "GpuRuntime::Create(Vulkan) failed";
                return false;
            }
            runtime = runtimeOpt.Release();
            return true;
#else
            reason = "Vulkan backend is not enabled for this build";
            return false;
#endif
        }
        default: {
            reason = "Unsupported backend";
            return false;
        }
    }
}

bool CanCreateRuntimeForBackend(RenderBackend backend, std::string& reason) {
    unique_ptr<GpuRuntime> runtime{};
    return CreateRuntimeForBackend(backend, runtime, reason);
}

BufferDescriptor MakeBufferDesc() {
    BufferDescriptor desc{};
    desc.Size = 256;
    desc.Memory = MemoryType::Upload;
    desc.Usage = BufferUse::MapWrite | BufferUse::CopySource;
    desc.Hints = ResourceHint::None;
    return desc;
}

TextureDescriptor MakeTextureDesc() {
    TextureDescriptor desc{};
    desc.Dim = TextureDimension::Dim2D;
    desc.Width = 4;
    desc.Height = 4;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleCount = 1;
    desc.Format = TextureFormat::RGBA8_UNORM;
    desc.Memory = MemoryType::Device;
    desc.Usage = TextureUse::Resource | TextureUse::CopyDestination;
    desc.Hints = ResourceHint::None;
    return desc;
}

GpuTextureViewDescriptor MakeTextureViewDesc(GpuTextureHandle target) {
    GpuTextureViewDescriptor desc{};
    desc.Target = target;
    desc.Dim = TextureDimension::Dim2D;
    desc.Format = TextureFormat::RGBA8_UNORM;
    desc.Range = SubresourceRange{0, 1, 0, 1};
    desc.Usage = TextureViewUsage::Resource;
    return desc;
}

SamplerDescriptor MakeSamplerDesc() {
    SamplerDescriptor desc{};
    desc.AddressS = AddressMode::ClampToEdge;
    desc.AddressT = AddressMode::ClampToEdge;
    desc.AddressR = AddressMode::ClampToEdge;
    desc.MinFilter = FilterMode::Nearest;
    desc.MagFilter = FilterMode::Nearest;
    desc.MipmapFilter = FilterMode::Nearest;
    desc.LodMin = 0.0f;
    desc.LodMax = 1.0f;
    return desc;
}

#ifdef RADRAY_ENABLE_IMGUI
struct ImGuiHandleProgram {
    GpuShaderHandle VS{};
    GpuShaderHandle PS{};
    GpuRootSignatureHandle RootSig{};
    GpuGraphicsPipelineStateHandle Pso{};
};

void DestroyImGuiHandleProgram(GpuRuntime& runtime, ImGuiHandleProgram& program) {
    auto destroyHandle = [&runtime](auto& handle) {
        if (handle.IsValid()) {
            runtime.DestroyResourceImmediate(handle);
            handle.Invalidate();
        }
    };
    destroyHandle(program.Pso);
    destroyHandle(program.RootSig);
    destroyHandle(program.PS);
    destroyHandle(program.VS);
}

bool FillImGuiShaderDescriptors(RenderBackend backend, ShaderDescriptor& vsDesc, ShaderDescriptor& psDesc, std::string& reason) {
    vsDesc.Stages = ShaderStage::Vertex;
    psDesc.Stages = ShaderStage::Pixel;
    switch (backend) {
        case RenderBackend::D3D12: {
            HlslShaderDesc vsRefl{};
            vsRefl.ConstantBuffers.push_back(HlslShaderBufferDesc{
                .Name = "gPush",
                .Variables = {},
                .Type = HlslCBufferType::CBUFFER,
                .Size = 16,
                .IsViewInHlsl = true});
            vsRefl.BoundResources.push_back(HlslInputBindDesc{
                .Name = "gPush",
                .Type = HlslShaderInputType::CBUFFER,
                .BindPoint = 0,
                .BindCount = 1,
                .Space = 0});
            HlslShaderDesc psRefl{};
            psRefl.BoundResources.push_back(HlslInputBindDesc{
                .Name = "gTexture",
                .Type = HlslShaderInputType::TEXTURE,
                .BindPoint = 0,
                .BindCount = 1,
                .ReturnType = HlslResourceReturnType::FLOAT,
                .Dimension = HlslSRVDimension::TEXTURE2D,
                .Space = 1,
                .VkBinding = 0,
                .VkSet = 1});
            psRefl.BoundResources.push_back(HlslInputBindDesc{
                .Name = "gSampler",
                .Type = HlslShaderInputType::SAMPLER,
                .BindPoint = 1,
                .BindCount = 1,
                .Space = 1,
                .VkBinding = 1,
                .VkSet = 1});
            vsDesc.Source = GetImGuiVertexShaderDXIL();
            vsDesc.Category = ShaderBlobCategory::DXIL;
            vsDesc.Reflection = vsRefl;
            psDesc.Source = GetImGuiPixelShaderDXIL();
            psDesc.Category = ShaderBlobCategory::DXIL;
            psDesc.Reflection = psRefl;
            return true;
        }
        case RenderBackend::Vulkan: {
            SpirvShaderDesc vsRefl{};
            vsRefl.PushConstants.push_back(SpirvPushConstantRange{
                .Name = "gPush",
                .Offset = 0,
                .Size = 16,
            });
            SpirvShaderDesc psRefl{};
            psRefl.ResourceBindings.push_back(SpirvResourceBinding{
                .Name = "gTexture",
                .Kind = SpirvResourceKind::SeparateImage,
                .Set = 1,
                .Binding = 0,
                .HlslRegister = 0,
                .HlslSpace = 1,
                .ArraySize = 1,
                .ImageInfo = SpirvImageInfo{
                    .Dim = SpirvImageDim::Dim2D,
                },
                .ReadOnly = true});
            psRefl.ResourceBindings.push_back(SpirvResourceBinding{
                .Name = "gSampler",
                .Kind = SpirvResourceKind::SeparateSampler,
                .Set = 1,
                .Binding = 1,
                .HlslRegister = 1,
                .HlslSpace = 1,
                .ArraySize = 1,
                .ImageInfo = std::nullopt,
                .ReadOnly = true});
            vsDesc.Source = GetImGuiVertexShaderSPIRV();
            vsDesc.Category = ShaderBlobCategory::SPIRV;
            vsDesc.Reflection = vsRefl;
            psDesc.Source = GetImGuiPixelShaderSPIRV();
            psDesc.Category = ShaderBlobCategory::SPIRV;
            psDesc.Reflection = psRefl;
            return true;
        }
        default:
            reason = "ImGui shader descriptors only cover D3D12 and Vulkan.";
            return false;
    }
}

bool CreateImGuiHandleProgram(GpuRuntime& runtime, ImGuiHandleProgram& program, std::string& reason) {
    auto* device = runtime.GetDevice();
    if (device == nullptr) {
        reason = "Runtime device is null.";
        return false;
    }

    try {
        ShaderDescriptor vsDesc{};
        ShaderDescriptor psDesc{};
        if (!FillImGuiShaderDescriptors(device->GetBackend(), vsDesc, psDesc, reason)) {
            return false;
        }

        program.VS = runtime.CreateShader(vsDesc);
        program.PS = runtime.CreateShader(psDesc);

        GpuShaderHandle shaders[] = {program.VS, program.PS};
        SamplerDescriptor samplerDesc{};
        samplerDesc.AddressS = AddressMode::ClampToEdge;
        samplerDesc.AddressT = AddressMode::ClampToEdge;
        samplerDesc.AddressR = AddressMode::ClampToEdge;
        samplerDesc.MinFilter = FilterMode::Linear;
        samplerDesc.MagFilter = FilterMode::Linear;
        samplerDesc.MipmapFilter = FilterMode::Linear;
        samplerDesc.LodMin = 0.0f;
        samplerDesc.LodMax = std::numeric_limits<float>::max();
        samplerDesc.Compare = std::nullopt;
        samplerDesc.AnisotropyClamp = 1;
        const StaticSamplerDescriptor staticSamplers[] = {
            StaticSamplerDescriptor{
                .Name = "gSampler",
                .Set = DescriptorSetIndex{1},
                .Binding = 1,
                .Stages = ShaderStage::Pixel,
                .Desc = samplerDesc,
            },
        };
        GpuRootSignatureDescriptor rootDesc{};
        rootDesc.Shaders = shaders;
        rootDesc.StaticSamplers = staticSamplers;
        program.RootSig = runtime.CreateRootSignature(rootDesc);

        const VertexElement vertexElements[] = {
            VertexElement{
                .Offset = offsetof(ImDrawVert, pos),
                .Semantic = "POSITION",
                .SemanticIndex = 0,
                .Format = VertexFormat::FLOAT32X2,
                .Location = 0,
            },
            VertexElement{
                .Offset = offsetof(ImDrawVert, uv),
                .Semantic = "TEXCOORD",
                .SemanticIndex = 0,
                .Format = VertexFormat::FLOAT32X2,
                .Location = 1,
            },
            VertexElement{
                .Offset = offsetof(ImDrawVert, col),
                .Semantic = "COLOR",
                .SemanticIndex = 0,
                .Format = VertexFormat::UNORM8X4,
                .Location = 2,
            },
        };
        const VertexBufferLayout vertexLayouts[] = {
            VertexBufferLayout{
                .ArrayStride = sizeof(ImDrawVert),
                .StepMode = VertexStepMode::Vertex,
                .Elements = vertexElements,
            },
        };
        BlendState blend{};
        blend.Color.Src = BlendFactor::SrcAlpha;
        blend.Color.Dst = BlendFactor::OneMinusSrcAlpha;
        blend.Color.Op = BlendOperation::Add;
        blend.Alpha.Src = BlendFactor::One;
        blend.Alpha.Dst = BlendFactor::OneMinusSrcAlpha;
        blend.Alpha.Op = BlendOperation::Add;
        const ColorTargetState colorTargets[] = {
            ColorTargetState{
                .Format = TextureFormat::RGBA8_UNORM,
                .Blend = blend,
                .WriteMask = ColorWrite::All,
            },
        };
        PrimitiveState primitive{};
        primitive.Topology = PrimitiveTopology::TriangleList;
        primitive.FaceClockwise = FrontFace::CW;
        primitive.Cull = CullMode::None;
        primitive.Poly = PolygonMode::Fill;
        primitive.StripIndexFormat = std::nullopt;
        primitive.UnclippedDepth = false;
        primitive.Conservative = false;

        GpuGraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = program.RootSig;
        psoDesc.VS = GpuShaderEntry{
            .Target = program.VS,
            .EntryPoint = "VSMain",
        };
        psoDesc.PS = GpuShaderEntry{
            .Target = program.PS,
            .EntryPoint = "PSMain",
        };
        psoDesc.VertexLayouts = vertexLayouts;
        psoDesc.Primitive = primitive;
        psoDesc.DepthStencil = std::nullopt;
        psoDesc.MultiSample = MultiSampleState{
            .Count = 1,
            .Mask = 0xFFFFFFFF,
            .AlphaToCoverageEnable = false,
        };
        psoDesc.ColorTargets = colorTargets;
        program.Pso = runtime.CreateGraphicsPipelineState(psoDesc);
        return true;
    } catch (const std::exception& ex) {
        reason = ex.what();
        DestroyImGuiHandleProgram(runtime, program);
        return false;
    }
}
#endif

GpuTask SubmitNoOpAsync(GpuRuntime& runtime) {
    auto context = runtime.BeginAsync(QueueType::Direct);
    auto* cmd = context->CreateCommandBuffer();
    cmd->Begin();
    cmd->End();
    return runtime.SubmitAsync(std::move(context));
}

void RecordFailureOnce(
    std::atomic_bool& failed,
    std::mutex& failureMutex,
    std::string& failure,
    std::string message) {
    if (failed.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock{failureMutex};
    if (failure.empty()) {
        failure = std::move(message);
        failed.store(true);
    }
}

bool DrainRuntime(GpuRuntime& runtime, std::string& reason) {
    try {
        runtime.Wait(QueueType::Direct);
        runtime.ProcessTasks();
    } catch (const std::exception& ex) {
        reason = std::string{"Failed to drain runtime: "} + ex.what();
        return false;
    } catch (...) {
        reason = "Failed to drain runtime with unknown exception.";
        return false;
    }

    if (!runtime._pendings.empty()) {
        reason = "Runtime still has pending submissions after drain.";
        return false;
    }
    if (runtime.HasPendingResourceDestroysForTest()) {
        reason = "Runtime still has pending resource destroys after drain.";
        return false;
    }
    return true;
}

class GpuRuntimeResourceTest : public ::testing::TestWithParam<RenderBackend> {};

TEST_P(GpuRuntimeResourceTest, PersistentResourcesCreateValidHandles) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    const auto view = runtime->CreateTextureView(MakeTextureViewDesc(texture));
    const auto sampler = runtime->CreateSampler(MakeSamplerDesc());

    EXPECT_TRUE(buffer.IsValid());
    EXPECT_TRUE(texture.IsValid());
    EXPECT_TRUE(view.IsValid());
    EXPECT_TRUE(sampler.IsValid());
    EXPECT_NE(buffer.NativeHandle, nullptr);
    EXPECT_NE(texture.NativeHandle, nullptr);
    EXPECT_NE(view.NativeHandle, nullptr);
    EXPECT_NE(sampler.NativeHandle, nullptr);

    GpuBufferHandle invalidHandle{};
    invalidHandle.Handle = 42;
    invalidHandle.NativeHandle = reinterpret_cast<void*>(1);
    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(invalidHandle));
}

TEST_P(GpuRuntimeResourceTest, RuntimeTextureViewRejectsTransientTextureTarget) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    auto context = runtime->BeginAsync(QueueType::Direct);
    const auto transientTexture = context->CreateTransientTexture(MakeTextureDesc());
    EXPECT_GPU_GUARD_ASSERT(runtime->CreateTextureView(MakeTextureViewDesc(transientTexture)));
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceImmediateRetiresPersistentResourceWithoutProcessTasks) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    runtime->DestroyResourceImmediate(texture);

    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
    EXPECT_GPU_GUARD_ASSERT(runtime->CreateTextureView(MakeTextureViewDesc(texture)));

    auto context = runtime->BeginAsync(QueueType::Direct);
    EXPECT_GPU_GUARD_ASSERT(context->CreateTransientTextureView(MakeTextureViewDesc(texture)));
    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(texture));
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceWithoutLastUseReleasesChildlessResource) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    ASSERT_TRUE(runtime->IsResourceTrackedForTest(buffer));

    runtime->DestroyResource(buffer);

    EXPECT_FALSE(runtime->IsResourceTrackedForTest(buffer));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResource(buffer));
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceDoesNotWaitForUnmarkedSubmission) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    auto task = SubmitNoOpAsync(*runtime);
    ASSERT_TRUE(task.IsValid());
    ASSERT_EQ(runtime->_pendings.size(), 1u);

    runtime->DestroyResource(buffer);

    EXPECT_FALSE(runtime->IsResourceTrackedForTest(buffer));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
    EXPECT_EQ(runtime->_pendings.size(), 1u);

    task.Wait();
    runtime->ProcessTasks();
    EXPECT_TRUE(runtime->_pendings.empty());
}

TEST_P(GpuRuntimeResourceTest, PreparedResourceDestroyWaitsUntilDiscard) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    GpuPreparedResourceList prepared{};
    prepared.Use(runtime.get(), buffer);
    prepared.Use(runtime.get(), buffer);

    runtime->DestroyResource(buffer);

    ASSERT_TRUE(runtime->IsResourceTrackedForTest(buffer));
    ASSERT_TRUE(runtime->IsResourcePendingDestroyForTest(buffer));
    ASSERT_TRUE(runtime->HasPendingResourceDestroysForTest());

    runtime->ProcessTasks();
    EXPECT_TRUE(runtime->IsResourcePendingDestroyForTest(buffer));

    prepared.Discard();
    EXPECT_FALSE(runtime->IsResourceTrackedForTest(buffer));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
}

TEST_P(GpuRuntimeResourceTest, PreparedResourceSubmitBecomesLastUse) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    GpuPreparedResourceList prepared{};
    prepared.Use(runtime.get(), buffer);

    FakeFence fence{};
    fence.Signal(1);
    GpuTask task{runtime.get(), &fence, 1};

    runtime->DestroyResource(buffer);
    prepared.Submit(task);

    ASSERT_TRUE(runtime->IsResourceTrackedForTest(buffer));
    ASSERT_TRUE(runtime->IsResourcePendingDestroyForTest(buffer));

    runtime->ProcessTasks();
    EXPECT_TRUE(runtime->IsResourcePendingDestroyForTest(buffer));

    task.Wait();
    runtime->ProcessTasks();
    EXPECT_FALSE(runtime->IsResourceTrackedForTest(buffer));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
}

TEST_P(GpuRuntimeResourceTest, PreparedParentAndChildRetireChildBeforeParentAfterDiscard) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    const auto view = runtime->CreateTextureView(MakeTextureViewDesc(texture));
    GpuPreparedResourceList prepared{};
    prepared.Use(runtime.get(), texture);
    prepared.Use(runtime.get(), view);

    runtime->DestroyResource(texture);
    runtime->DestroyResource(view);

    EXPECT_TRUE(runtime->IsResourcePendingDestroyForTest(texture));
    EXPECT_TRUE(runtime->IsResourcePendingDestroyForTest(view));

    prepared.Discard();
    EXPECT_FALSE(runtime->IsResourceTrackedForTest(texture));
    EXPECT_FALSE(runtime->IsResourceTrackedForTest(view));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
}

TEST_P(GpuRuntimeResourceTest, PreparedResourceListRejectsInvalidTransientAndForeignInputsInDebug) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    GpuPreparedResourceList prepared{};

    GpuBufferHandle invalidHandle{};
    invalidHandle.Handle = 42;
    invalidHandle.NativeHandle = reinterpret_cast<void*>(1);
    EXPECT_GPU_GUARD_ASSERT(prepared.Use(runtime.get(), invalidHandle));

    auto context = runtime->BeginAsync(QueueType::Direct);
    const auto transientBuffer = context->CreateTransientBuffer(MakeBufferDesc());
    EXPECT_GPU_GUARD_ASSERT(prepared.Use(runtime.get(), transientBuffer));

    unique_ptr<GpuRuntime> foreignRuntime{};
    std::string foreignReason{};
    if (CreateRuntimeForBackend(GetParam(), foreignRuntime, foreignReason)) {
        const auto foreignBuffer = foreignRuntime->CreateBuffer(MakeBufferDesc());
        EXPECT_GPU_GUARD_ASSERT(prepared.Use(runtime.get(), foreignBuffer));
        foreignRuntime->DestroyResourceImmediate(foreignBuffer);
    }

    prepared.Use(runtime.get(), buffer);
    FakeFence fence{};
    fence.Signal(1);
    GpuRuntime directForeignRuntime(shared_ptr<Device>{}, unique_ptr<InstanceVulkan>{});
    GpuTask foreignTask{&directForeignRuntime, &fence, 1};
    EXPECT_GPU_GUARD_ASSERT(prepared.Submit(foreignTask));

    prepared.Discard();
    runtime->DestroyResourceImmediate(buffer);
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceWaitsForMarkedLastUseAndProcessTasks) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    FakeFence fence{};
    fence.Signal(1);
    GpuTask task{runtime.get(), &fence, 1};
    runtime->MarkResourceUsed(buffer, task);
    runtime->DestroyResource(buffer);

    ASSERT_TRUE(runtime->IsResourceTrackedForTest(buffer));
    ASSERT_TRUE(runtime->IsResourcePendingDestroyForTest(buffer));
    ASSERT_TRUE(runtime->HasPendingResourceDestroysForTest());

    runtime->ProcessTasks();
    EXPECT_TRUE(runtime->IsResourcePendingDestroyForTest(buffer));
    EXPECT_TRUE(runtime->HasPendingResourceDestroysForTest());

    task.Wait();
    runtime->ProcessTasks();
    EXPECT_FALSE(runtime->IsResourceTrackedForTest(buffer));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
    EXPECT_TRUE(runtime->_pendings.empty());
}

TEST_P(GpuRuntimeResourceTest, GpuTaskCopyCanWaitAndDriveMarkedDestroy) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    FakeFence fence{};
    fence.Signal(1);
    const GpuTask task{runtime.get(), &fence, 1};
    const GpuTask copied = task;

    EXPECT_TRUE(task.IsValid());
    EXPECT_TRUE(copied.IsValid());
    EXPECT_EQ(task._runtime, copied._runtime);
    EXPECT_EQ(task._fence, copied._fence);
    EXPECT_EQ(task._signalValue, copied._signalValue);
    EXPECT_EQ(task.IsCompleted(), copied.IsCompleted());

    runtime->MarkResourceUsed(buffer, copied);
    runtime->DestroyResource(buffer);
    copied.Wait();
    EXPECT_TRUE(task.IsCompleted());
    EXPECT_TRUE(copied.IsCompleted());

    runtime->ProcessTasks();
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
    EXPECT_TRUE(runtime->_pendings.empty());
}

TEST_P(GpuRuntimeResourceTest, DestroyAndMarkResourceUsedRejectInvalidAndForeignInputsInDebug) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    FakeFence fence{};
    fence.Signal(1);
    GpuTask task{runtime.get(), &fence, 1};

    GpuBufferHandle invalidHandle{};
    invalidHandle.Handle = 42;
    invalidHandle.NativeHandle = reinterpret_cast<void*>(1);
    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResource(invalidHandle));
    EXPECT_GPU_GUARD_ASSERT(runtime->MarkResourceUsed(invalidHandle, task));

    GpuRuntime foreignRuntime(shared_ptr<Device>{}, unique_ptr<InstanceVulkan>{});
    GpuTask foreignTask{&foreignRuntime, &fence, 1};
    EXPECT_GPU_GUARD_ASSERT(runtime->MarkResourceUsed(buffer, foreignTask));

    runtime->DestroyResourceImmediate(buffer);
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceRetiresChildBeforeParentWhenBothReady) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    const auto view = runtime->CreateTextureView(MakeTextureViewDesc(texture));
    FakeFence fence{};
    fence.Signal(1);
    GpuTask task{runtime.get(), &fence, 1};

    runtime->MarkResourceUsed(texture, task);
    runtime->MarkResourceUsed(view, task);
    runtime->DestroyResource(texture);
    runtime->DestroyResource(view);

    runtime->ProcessTasks();
    EXPECT_TRUE(runtime->IsResourcePendingDestroyForTest(texture));
    EXPECT_TRUE(runtime->IsResourcePendingDestroyForTest(view));

    task.Wait();
    runtime->ProcessTasks();

    EXPECT_FALSE(runtime->IsResourceTrackedForTest(texture));
    EXPECT_FALSE(runtime->IsResourceTrackedForTest(view));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
    EXPECT_GPU_GUARD_ASSERT(runtime->CreateTextureView(MakeTextureViewDesc(texture)));
}

#ifdef RADRAY_ENABLE_IMGUI
TEST_P(GpuRuntimeResourceTest, RuntimeRenderObjectHandlesCreateAndDestroy) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    ImGuiHandleProgram program{};
    ASSERT_TRUE(CreateImGuiHandleProgram(*runtime, program, reason)) << reason;
    const auto descriptorSet = runtime->CreateDescriptorSet(program.RootSig, DescriptorSetIndex{1});

    EXPECT_TRUE(program.VS.IsValid());
    EXPECT_TRUE(program.PS.IsValid());
    EXPECT_TRUE(program.RootSig.IsValid());
    EXPECT_TRUE(program.Pso.IsValid());
    EXPECT_TRUE(descriptorSet.IsValid());
    EXPECT_NE(program.VS.NativeHandle, nullptr);
    EXPECT_NE(program.PS.NativeHandle, nullptr);
    EXPECT_NE(program.RootSig.NativeHandle, nullptr);
    EXPECT_NE(program.Pso.NativeHandle, nullptr);
    EXPECT_NE(descriptorSet.NativeHandle, nullptr);

    runtime->DestroyResourceImmediate(descriptorSet);
    DestroyImGuiHandleProgram(*runtime, program);
}

TEST_P(GpuRuntimeResourceTest, RootSignatureImmediateDestroyRejectsLiveChildren) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    ImGuiHandleProgram program{};
    ASSERT_TRUE(CreateImGuiHandleProgram(*runtime, program, reason)) << reason;
    const auto descriptorSet = runtime->CreateDescriptorSet(program.RootSig, DescriptorSetIndex{1});

    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(program.RootSig));

    runtime->DestroyResourceImmediate(program.Pso);
    program.Pso.Invalidate();
    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(program.RootSig));

    runtime->DestroyResourceImmediate(descriptorSet);
    DestroyImGuiHandleProgram(*runtime, program);
}
#endif

TEST_P(GpuRuntimeResourceTest, TransientViewScopeRulesAndImmediateDestroyRejectsTransient) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto persistentTexture = runtime->CreateTexture(MakeTextureDesc());
    auto contextA = runtime->BeginAsync(QueueType::Direct);
    auto contextB = runtime->BeginAsync(QueueType::Direct);

    const auto transientTexture = contextA->CreateTransientTexture(MakeTextureDesc());
    const auto localView = contextA->CreateTransientTextureView(MakeTextureViewDesc(transientTexture));
    const auto persistentView = contextA->CreateTransientTextureView(MakeTextureViewDesc(persistentTexture));

    EXPECT_TRUE(localView.IsValid());
    EXPECT_TRUE(persistentView.IsValid());
    EXPECT_GPU_GUARD_ASSERT(contextB->CreateTransientTextureView(MakeTextureViewDesc(transientTexture)));
    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(transientTexture));
    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResource(transientTexture));

    auto task = SubmitNoOpAsync(*runtime);
    EXPECT_GPU_GUARD_ASSERT(runtime->MarkResourceUsed(transientTexture, task));
    task.Wait();
    runtime->ProcessTasks();
}

TEST_P(GpuRuntimeResourceTest, UnsubmittedTransientViewBlocksImmediatePersistentTextureDestroyUntilContextDies) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    auto context = runtime->BeginAsync(QueueType::Direct);
    const auto transientView = context->CreateTransientTextureView(MakeTextureViewDesc(texture));
    ASSERT_TRUE(transientView.IsValid());

    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(texture));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());

    context.reset();
    runtime->DestroyResourceImmediate(texture);
    EXPECT_GPU_GUARD_ASSERT(runtime->CreateTextureView(MakeTextureViewDesc(texture)));
}

TEST_P(GpuRuntimeResourceTest, SubmittedTransientViewBlocksImmediatePersistentTextureDestroyUntilProcessTasksDrainsContext) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    auto context = runtime->BeginAsync(QueueType::Direct);
    const auto transientView = context->CreateTransientTextureView(MakeTextureViewDesc(texture));
    ASSERT_TRUE(transientView.IsValid());

    auto* cmd = context->CreateCommandBuffer();
    cmd->Begin();
    cmd->End();
    auto task = runtime->SubmitAsync(std::move(context));

    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(texture));
    task.Wait();

    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
    EXPECT_EQ(runtime->_pendings.size(), 1u);

    runtime->ProcessTasks();
    EXPECT_TRUE(runtime->_pendings.empty());
    runtime->DestroyResourceImmediate(texture);
    EXPECT_GPU_GUARD_ASSERT(runtime->CreateTextureView(MakeTextureViewDesc(texture)));
}

TEST_P(GpuRuntimeResourceTest, PersistentTextureImmediateDestroyRejectsLivePersistentViews) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    const auto view = runtime->CreateTextureView(MakeTextureViewDesc(texture));

    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(texture));
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());

    runtime->DestroyResourceImmediate(view);
    runtime->DestroyResourceImmediate(texture);
    EXPECT_GPU_GUARD_ASSERT(runtime->CreateTextureView(MakeTextureViewDesc(texture)));
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceImmediateTwiceDiesInDebug) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    runtime->DestroyResourceImmediate(texture);
    EXPECT_GPU_GUARD_ASSERT(runtime->DestroyResourceImmediate(texture));
}

TEST_P(GpuRuntimeResourceTest, ConcurrentAsyncSubmitWaitAndProcessTasksDrainPendings) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    constexpr int kSubmitThreadCount = 4;
    constexpr int kSubmitIterations = 12;
    constexpr int kMaintenanceIterations = 24;

    std::atomic_bool failed{false};
    std::mutex failureMutex{};
    std::string failure{};

    std::thread waiter([&]() {
        for (int i = 0; i < kMaintenanceIterations && !failed.load(); ++i) {
            try {
                runtime->Wait(QueueType::Direct);
            } catch (const std::exception& ex) {
                RecordFailureOnce(
                    failed,
                    failureMutex,
                    failure,
                    std::string{"Concurrent waiter failed: "} + ex.what());
                break;
            } catch (...) {
                RecordFailureOnce(failed, failureMutex, failure, "Concurrent waiter failed with unknown exception.");
                break;
            }
            std::this_thread::yield();
        }
    });

    std::thread drainer([&]() {
        for (int i = 0; i < kMaintenanceIterations * 2 && !failed.load(); ++i) {
            try {
                runtime->ProcessTasks();
            } catch (const std::exception& ex) {
                RecordFailureOnce(
                    failed,
                    failureMutex,
                    failure,
                    std::string{"Concurrent drainer failed: "} + ex.what());
                break;
            } catch (...) {
                RecordFailureOnce(failed, failureMutex, failure, "Concurrent drainer failed with unknown exception.");
                break;
            }
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> submitters{};
    submitters.reserve(kSubmitThreadCount);
    for (int threadIndex = 0; threadIndex < kSubmitThreadCount; ++threadIndex) {
        submitters.emplace_back([&, threadIndex]() {
            for (int iteration = 0; iteration < kSubmitIterations && !failed.load(); ++iteration) {
                try {
                    auto task = SubmitNoOpAsync(*runtime);
                    if (!task.IsValid()) {
                        RecordFailureOnce(failed, failureMutex, failure, "SubmitNoOpAsync returned an invalid task.");
                        return;
                    }

                    if (((threadIndex + iteration) & 1) == 0) {
                        task.Wait();
                    } else if (((threadIndex + iteration) % 3) == 0) {
                        runtime->ProcessTasks();
                    }
                } catch (const std::exception& ex) {
                    RecordFailureOnce(
                        failed,
                        failureMutex,
                        failure,
                        std::string{"Concurrent submitter failed: "} + ex.what());
                    return;
                } catch (...) {
                    RecordFailureOnce(failed, failureMutex, failure, "Concurrent submitter failed with unknown exception.");
                    return;
                }
            }
        });
    }

    for (auto& submitter : submitters) {
        submitter.join();
    }
    waiter.join();
    drainer.join();

    ASSERT_FALSE(failed.load()) << failure;
    ASSERT_TRUE(DrainRuntime(*runtime, reason)) << reason;
}

TEST_P(GpuRuntimeResourceTest, ConcurrentPersistentResourceCreateAndRetireRemainConsistent) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    constexpr int kWorkerCount = 4;
    constexpr int kIterations = 8;

    std::atomic_bool failed{false};
    std::mutex failureMutex{};
    std::string failure{};

    std::vector<std::thread> workers{};
    workers.reserve(kWorkerCount);
    for (int workerIndex = 0; workerIndex < kWorkerCount; ++workerIndex) {
        workers.emplace_back([&, workerIndex]() {
            for (int iteration = 0; iteration < kIterations && !failed.load(); ++iteration) {
                try {
                    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
                    const auto texture = runtime->CreateTexture(MakeTextureDesc());
                    const auto view = runtime->CreateTextureView(MakeTextureViewDesc(texture));
                    const auto sampler = runtime->CreateSampler(MakeSamplerDesc());

                    if (!buffer.IsValid() || !texture.IsValid() || !view.IsValid() || !sampler.IsValid()) {
                        RecordFailureOnce(failed, failureMutex, failure, "Persistent resource creation returned invalid handles.");
                        return;
                    }

                    auto task = SubmitNoOpAsync(*runtime);
                    runtime->MarkResourceUsed(buffer, task);
                    runtime->DestroyResource(buffer);
                    task.Wait();
                    if (((workerIndex + iteration) & 1) == 0) {
                        runtime->ProcessTasks();
                    }
                    runtime->DestroyResource(view);
                    runtime->DestroyResource(texture);
                    runtime->DestroyResource(sampler);
                    runtime->ProcessTasks();
                } catch (const std::exception& ex) {
                    RecordFailureOnce(
                        failed,
                        failureMutex,
                        failure,
                        std::string{"Concurrent persistent resource worker failed: "} + ex.what());
                    return;
                } catch (...) {
                    RecordFailureOnce(failed, failureMutex, failure, "Concurrent persistent resource worker failed with unknown exception.");
                    return;
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    ASSERT_FALSE(failed.load()) << failure;
    ASSERT_TRUE(DrainRuntime(*runtime, reason)) << reason;
    EXPECT_FALSE(runtime->HasPendingResourceDestroysForTest());
    EXPECT_TRUE(runtime->_pendings.empty());
}

TEST_P(GpuRuntimeResourceTest, TransientPersistentTextureViewsSurviveInterleavedRuntimeOperations) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    constexpr int kIterations = 12;

    const auto persistentTexture = runtime->CreateTexture(MakeTextureDesc());
    ASSERT_TRUE(persistentTexture.IsValid());

    std::atomic_bool failed{false};
    std::mutex failureMutex{};
    std::string failure{};

    std::thread transientViewWorker([&]() {
        for (int iteration = 0; iteration < kIterations && !failed.load(); ++iteration) {
            try {
                auto context = runtime->BeginAsync(QueueType::Direct);
                const auto view = context->CreateTransientTextureView(MakeTextureViewDesc(persistentTexture));
                if (!view.IsValid()) {
                    RecordFailureOnce(failed, failureMutex, failure, "Transient texture view creation returned an invalid handle.");
                    return;
                }

                auto* cmd = context->CreateCommandBuffer();
                cmd->Begin();
                cmd->End();

                auto task = runtime->SubmitAsync(std::move(context));
                task.Wait();
                runtime->ProcessTasks();
            } catch (const std::exception& ex) {
                RecordFailureOnce(
                    failed,
                    failureMutex,
                    failure,
                    std::string{"Transient view worker failed: "} + ex.what());
                return;
            } catch (...) {
                RecordFailureOnce(failed, failureMutex, failure, "Transient view worker failed with unknown exception.");
                return;
            }
        }
    });

    std::thread interleavedRuntimeWorker([&]() {
        for (int iteration = 0; iteration < kIterations && !failed.load(); ++iteration) {
            try {
                const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
                const auto sampler = runtime->CreateSampler(MakeSamplerDesc());
                if (!buffer.IsValid() || !sampler.IsValid()) {
                    RecordFailureOnce(failed, failureMutex, failure, "Interleaved runtime operation returned invalid handles.");
                    return;
                }

                auto task = SubmitNoOpAsync(*runtime);
                runtime->MarkResourceUsed(buffer, task);
                runtime->DestroyResource(buffer);
                task.Wait();
                runtime->DestroyResource(sampler);
                runtime->ProcessTasks();
            } catch (const std::exception& ex) {
                RecordFailureOnce(
                    failed,
                    failureMutex,
                    failure,
                    std::string{"Interleaved runtime worker failed: "} + ex.what());
                return;
            } catch (...) {
                RecordFailureOnce(failed, failureMutex, failure, "Interleaved runtime worker failed with unknown exception.");
                return;
            }
        }
    });

    transientViewWorker.join();
    interleavedRuntimeWorker.join();

    ASSERT_FALSE(failed.load()) << failure;
    ASSERT_TRUE(DrainRuntime(*runtime, reason)) << reason;

    runtime->DestroyResourceImmediate(persistentTexture);
    EXPECT_GPU_GUARD_ASSERT(runtime->CreateTextureView(MakeTextureViewDesc(persistentTexture)));
}

#ifdef RADRAY_IS_DEBUG
TEST_P(GpuRuntimeResourceTest, DestroyResourceTwiceDiesInDebug) {
    const RenderBackend backend = GetParam();
    std::string reason{};
    if (!CanCreateRuntimeForBackend(backend, reason)) {
        GTEST_SKIP() << reason;
    }

    EXPECT_DEATH_IF_SUPPORTED(
        {
            unique_ptr<GpuRuntime> runtime{};
            std::string localReason{};
            if (!CreateRuntimeForBackend(backend, runtime, localReason)) {
                RADRAY_ABORT("{}", localReason);
            }
            const auto texture = runtime->CreateTexture(MakeTextureDesc());
            runtime->DestroyResource(texture);
            runtime->DestroyResource(texture);
        },
        "");
}
#endif

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    GpuRuntimeResourceTest,
    ::testing::ValuesIn(GetEnabledRuntimeBackends()),
    [](const ::testing::TestParamInfo<RenderBackend>& info) {
        return std::string{BackendTestName(info.param)};
    });

#undef EXPECT_GPU_GUARD_ASSERT

}  // namespace
