#include "gpu_test_fixture.h"

namespace radray::render::test {

namespace {
class CleanupQueue final : public CommandQueue {
public:
    explicit CleanupQueue(uint32_t& waits) : Waits(waits) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void Submit(const CommandQueueSubmitDescriptor&) noexcept override {}
    void Wait() noexcept override { ++Waits; }
    QueueType GetQueueType() const noexcept override { return QueueType::Direct; }
    uint32_t& Waits;
};
class CleanupDevice final : public Device {
public:
    CleanupDevice(uint32_t& destroys, uint32_t& waits) : Destroys(destroys), Queue(waits) {}
    ~CleanupDevice() noexcept override { ++Destroys; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    RenderBackend GetBackend() noexcept override { return RenderBackend::D3D12; }
    DeviceDetail GetDetail() const noexcept override { return {}; }
    const RenderDeviceCapabilities& GetCapabilities() const noexcept override { return Caps; }
    TextureSupport QueryTextureSupport(const TextureSupportQuery&) const noexcept override { return {}; }
    Nullable<CommandQueue*> GetCommandQueue(QueueType, uint32_t) noexcept override { return &Queue; }
    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue*) noexcept override { return nullptr; }
    Nullable<unique_ptr<Fence>> CreateFence() noexcept override { return nullptr; }
    Nullable<unique_ptr<QueryPool>> CreateQueryPool(const QueryPoolDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor&) noexcept override { return nullptr; }
    void FlushMappedRanges(std::span<const MappedBufferRange>) noexcept override {}
    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<RenderPass>> CreateRenderPass(const RenderPassDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<Framebuffer>> CreateFramebuffer(const FramebufferDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<ShaderParameterSet>> CreateShaderParameterSet(const ShaderParameterSetDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor&) noexcept override { return nullptr; }
    Nullable<Sampler*> GetOrCreateSampler(const SamplerDescriptor&) noexcept override { return nullptr; }
    uint32_t& Destroys;
    CleanupQueue Queue;
    RenderDeviceCapabilities Caps;
};
class CleanupFactory final : public DXGIFactory {
public:
    explicit CleanupFactory(uint32_t& destroys) : Destroys(destroys) {}
    ~CleanupFactory() noexcept override { ++Destroys; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    vector<DXGIAdapterInfo> GetAdapters() const noexcept override { return {}; }
    std::optional<uint32_t> SelectHighPerformanceAdapter() const noexcept override { return std::nullopt; }
    uint32_t& Destroys;
};
}  // namespace

TEST(GpuTestFixture, H01PartialInitializationCleanupIsIdempotent) {
    for (const auto backend : {RenderBackend::D3D12, RenderBackend::Vulkan})
        for (const auto stage : {DeviceSetupStage::Factory, DeviceSetupStage::Instance, DeviceSetupStage::Device, DeviceSetupStage::Queue, DeviceSetupStage::Complete}) {
            uint32_t factoryDestroys = 0, deviceDestroys = 0, waits = 0;
            static uint32_t environmentCloses = 0;
            environmentCloses = 0;
            DeviceContext context;
            const bool ownsEnvironment = stage >= DeviceSetupStage::Device;
            const bool ownsDevice = stage >= DeviceSetupStage::Queue;
            if (ownsEnvironment && backend == RenderBackend::D3D12) context.Factory = make_unique<CleanupFactory>(factoryDestroys);
            if (ownsEnvironment && backend == RenderBackend::Vulkan) {
                context.VulkanEnvInitialized = true;
                context.ShutdownEnvironment = +[]() noexcept { ++environmentCloses; };
            }
            if (ownsDevice) {
                auto device = make_shared<CleanupDevice>(deviceDestroys, waits);
                if (stage == DeviceSetupStage::Complete) context.Queue = &device->Queue;
                context.Device = std::move(device);
            }
            EXPECT_FALSE(RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, stage, "injected setup failure", false));
            context.Reset();
            context.Reset();
            EXPECT_EQ(context.Status, DeviceSetupStatus::InitializationFailed);
            EXPECT_EQ(context.Stage, stage);
            EXPECT_EQ(context.Queue, nullptr);
            EXPECT_FALSE(context.Device);
            EXPECT_FALSE(context.Factory);
            EXPECT_FALSE(context.VulkanEnvInitialized);
            EXPECT_EQ(factoryDestroys, ownsEnvironment && backend == RenderBackend::D3D12 ? 1u : 0u);
            EXPECT_EQ(environmentCloses, ownsEnvironment && backend == RenderBackend::Vulkan ? 1u : 0u);
            EXPECT_EQ(deviceDestroys, ownsDevice ? 1u : 0u);
            EXPECT_EQ(waits, stage == DeviceSetupStage::Complete ? 1u : 0u);
        }
}

TEST(GpuTestFixture, H02RequiredBackendsCannotDisappearIntoSkips) {
    EXPECT_TRUE(ContainsBackend("d3d12, vulkan", RenderBackend::D3D12));
    EXPECT_TRUE(ContainsBackend("d3d12, vulkan", RenderBackend::Vulkan));
    EXPECT_TRUE(ContainsBackend("D3D12,Vulkan", RenderBackend::D3D12));
    EXPECT_TRUE(ContainsBackend("D3D12,Vulkan", RenderBackend::Vulkan));
    EXPECT_FALSE(ContainsBackend("d3d12x", RenderBackend::D3D12));
    EXPECT_FALSE(ContainsBackend("", RenderBackend::Vulkan));
    for (const auto status : {DeviceSetupStatus::BackendNotBuilt, DeviceSetupStatus::NoAdapter, DeviceSetupStatus::ValidationUnavailable}) {
        EXPECT_FALSE(SetupMustFail(status, false));
        EXPECT_TRUE(SetupMustFail(status, true));
    }
    EXPECT_TRUE(SetupMustFail(DeviceSetupStatus::InitializationFailed, false));
    EXPECT_TRUE(SetupMustFail(DeviceSetupStatus::InitializationFailed, true));
    EXPECT_FALSE(SetupMustFail(DeviceSetupStatus::Ready, true));
}

TEST(GpuTestFixture, H04ValidationCallbacksCountErrorsWithoutCrossTestState) {
    DeviceContext first, second;
    CaptureValidationMessage(LogLevel::Info, "expected informational probe", &first);
    CaptureValidationMessage(LogLevel::Warn, "expected warning probe", &first);
    EXPECT_EQ(first.ValidationErrors.load(), 0u);
    CaptureValidationMessage(LogLevel::Err, "expected isolated validation callback probe", &first);
    EXPECT_EQ(first.ValidationErrors.load(), 1u);
    EXPECT_EQ(second.ValidationErrors.load(), 0u);
}

class GpuValidationProbe : public testing::TestWithParam<RenderBackend> {};
TEST_P(GpuValidationProbe, H04OneExpectedNativeValidationErrorIsCapturedInAnIsolatedProcess) {
    DeviceContext context;
    if (!TryCreateDevice(GetParam(), context, true)) GTEST_SKIP() << context.Reason;
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
    constexpr const char* message = "RadRay H04 expected native validation callback probe";
#if defined(RADRAY_ENABLE_D3D12)
    if (auto* native = dynamic_cast<d3d12::DeviceD3D12*>(context.Device.get())) {
        d3d12::ComPtr<ID3D12InfoQueue> queue;
        ASSERT_TRUE(SUCCEEDED(native->_device.As(&queue)));
        ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, message)));
        native->TryDrainValidationMessages();
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (auto* native = dynamic_cast<vulkan::DeviceVulkan*>(context.Device.get())) {
        VkDebugUtilsMessengerCallbackDataEXT data{};
        data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
        data.pMessageIdName = "RadRay-H04-Expected";
        data.messageIdNumber = 5604;
        data.pMessage = message;
        const auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(vkGetInstanceProcAddr(native->_instance->_instance, "vkSubmitDebugUtilsMessageEXT"));
        ASSERT_NE(submit, nullptr);
        submit(native->_instance->_instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
    }
#endif
    context.Reset();
    EXPECT_EQ(context.ValidationErrors.load(), 1u);
    RecordProperty("expected_validation_errors", 1);
    RecordProperty("observed_validation_errors", context.ValidationErrors.load());
    RecordProperty("evidence_class", "isolated_expected_error");
}
INSTANTIATE_TEST_SUITE_P(Backends, GpuValidationProbe, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));

}  // namespace radray::render::test
