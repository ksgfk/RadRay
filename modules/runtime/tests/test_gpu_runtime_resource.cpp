#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <radray/render/common.h>
#define private public
#define protected public
#include <radray/runtime/gpu_system.h>
#undef protected
#undef private

using namespace radray;
using namespace radray::render;

namespace {

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

GpuTask SubmitNoOpAsync(GpuRuntime& runtime) {
    auto context = runtime.BeginAsync(QueueType::Direct);
    auto* cmd = context->CreateCommandBuffer();
    cmd->Begin();
    cmd->End();
    return runtime.SubmitAsync(std::move(context));
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
    EXPECT_THROW(runtime->DestroyResourceImmediate(invalidHandle), GpuSystemException);
}

TEST_P(GpuRuntimeResourceTest, RuntimeTextureViewRejectsTransientTextureTarget) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    auto context = runtime->BeginAsync(QueueType::Direct);
    const auto transientTexture = context->CreateTransientTexture(MakeTextureDesc());
    EXPECT_THROW(runtime->CreateTextureView(MakeTextureViewDesc(transientTexture)), GpuSystemException);
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceImmediateRetiresPersistentResourceWithoutProcessTasks) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    runtime->DestroyResourceImmediate(texture);

    EXPECT_TRUE(runtime->_resourceRetirements.empty());
    EXPECT_THROW(runtime->CreateTextureView(MakeTextureViewDesc(texture)), GpuSystemException);

    auto context = runtime->BeginAsync(QueueType::Direct);
    EXPECT_THROW(context->CreateTransientTextureView(MakeTextureViewDesc(texture)), GpuSystemException);
    EXPECT_THROW(runtime->DestroyResourceImmediate(texture), GpuSystemException);
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceAfterWaitsForTaskAndProcessTasks) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto buffer = runtime->CreateBuffer(MakeBufferDesc());
    auto task = SubmitNoOpAsync(*runtime);
    runtime->DestroyResourceAfter(buffer, task);

    ASSERT_EQ(runtime->_resourceRetirements.size(), 1u);
    ASSERT_EQ(runtime->_pendings.size(), 1u);

    task.Wait();
    EXPECT_EQ(runtime->_resourceRetirements.size(), 1u);
    EXPECT_EQ(runtime->_pendings.size(), 1u);

    runtime->ProcessTasks();
    EXPECT_TRUE(runtime->_resourceRetirements.empty());
    EXPECT_TRUE(runtime->_pendings.empty());
}

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
    EXPECT_THROW(contextB->CreateTransientTextureView(MakeTextureViewDesc(transientTexture)), GpuSystemException);
    EXPECT_THROW(runtime->DestroyResourceImmediate(transientTexture), GpuSystemException);

    auto task = SubmitNoOpAsync(*runtime);
    EXPECT_THROW(runtime->DestroyResourceAfter(transientTexture, task), GpuSystemException);
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

    EXPECT_THROW(runtime->DestroyResourceImmediate(texture), GpuSystemException);
    EXPECT_TRUE(runtime->_resourceRetirements.empty());

    context.reset();
    runtime->DestroyResourceImmediate(texture);
    EXPECT_THROW(runtime->CreateTextureView(MakeTextureViewDesc(texture)), GpuSystemException);
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

    EXPECT_THROW(runtime->DestroyResourceImmediate(texture), GpuSystemException);
    task.Wait();

    EXPECT_TRUE(runtime->_resourceRetirements.empty());
    EXPECT_EQ(runtime->_pendings.size(), 1u);

    runtime->ProcessTasks();
    EXPECT_TRUE(runtime->_pendings.empty());
    runtime->DestroyResourceImmediate(texture);
    EXPECT_THROW(runtime->CreateTextureView(MakeTextureViewDesc(texture)), GpuSystemException);
}

TEST_P(GpuRuntimeResourceTest, PersistentTextureImmediateDestroyRejectsLivePersistentViews) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    const auto view = runtime->CreateTextureView(MakeTextureViewDesc(texture));

    EXPECT_THROW(runtime->DestroyResourceImmediate(texture), GpuSystemException);
    EXPECT_TRUE(runtime->_resourceRetirements.empty());

    runtime->DestroyResourceImmediate(view);
    runtime->DestroyResourceImmediate(texture);
    EXPECT_THROW(runtime->CreateTextureView(MakeTextureViewDesc(texture)), GpuSystemException);
}

TEST_P(GpuRuntimeResourceTest, DestroyResourceImmediateTwiceThrows) {
    unique_ptr<GpuRuntime> runtime{};
    std::string reason{};
    if (!CreateRuntimeForBackend(GetParam(), runtime, reason)) {
        GTEST_SKIP() << reason;
    }

    const auto texture = runtime->CreateTexture(MakeTextureDesc());
    runtime->DestroyResourceImmediate(texture);
    EXPECT_THROW(runtime->DestroyResourceImmediate(texture), GpuSystemException);
}

#ifdef RADRAY_IS_DEBUG
TEST_P(GpuRuntimeResourceTest, DestroyResourceAfterTwiceDiesInDebug) {
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
            auto task = SubmitNoOpAsync(*runtime);
            runtime->DestroyResourceAfter(texture, task);
            runtime->DestroyResourceAfter(texture, task);
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

}  // namespace
