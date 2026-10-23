#include "gpu_test_fixture.h"
#include "rhi_validation.h"
#include <filesystem>
#include <radray/file.h>
#include <radray/scope_guard.h>

#if defined(RADRAY_ENABLE_VULKAN)

namespace radray::render::vulkan {
namespace {

template <class Visitor>
void VisitFeatureBits(DeviceFeaturesVulkan& features, Visitor visitor) {
    auto visit = [&](auto& native, size_t first, size_t last) {
        auto* bytes = reinterpret_cast<unsigned char*>(&native);
        for (size_t offset = first; offset <= last; offset += sizeof(VkBool32)) {
            VkBool32 value{};
            std::memcpy(&value, bytes + offset, sizeof(value));
            visitor(value);
            std::memcpy(bytes + offset, &value, sizeof(value));
        }
    };
    visit(features.Core, offsetof(VkPhysicalDeviceFeatures, robustBufferAccess), offsetof(VkPhysicalDeviceFeatures, inheritedQueries));
    visit(features.Vulkan11, offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess), offsetof(VkPhysicalDeviceVulkan11Features, shaderDrawParameters));
    visit(features.Vulkan12, offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge), offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId));
}

vector<VkBool32> FeatureBits(DeviceFeaturesVulkan features) {
    vector<VkBool32> result;
    VisitFeatureBits(features, [&](VkBool32 value) { result.push_back(value); });
    return result;
}

DeviceFeaturesVulkan AllSupportedFeatures() {
    DeviceFeaturesVulkan result;
    VisitFeatureBits(result, [](VkBool32& value) { value = VK_TRUE; });
    return result;
}

DeviceFeaturesVulkan ExpectedRhiFeatures() {
    DeviceFeaturesVulkan result;
    result.Core.fullDrawIndexUint32 = VK_TRUE;
    result.Core.imageCubeArray = VK_TRUE;
    result.Core.depthClamp = VK_TRUE;
    result.Core.depthBiasClamp = VK_TRUE;
    result.Core.fillModeNonSolid = VK_TRUE;
    result.Core.independentBlend = VK_TRUE;
    result.Core.dualSrcBlend = VK_TRUE;
    result.Core.multiDrawIndirect = VK_TRUE;
    result.Core.drawIndirectFirstInstance = VK_TRUE;
    result.Core.samplerAnisotropy = VK_TRUE;
    result.Core.vertexPipelineStoresAndAtomics = VK_TRUE;
    result.Core.fragmentStoresAndAtomics = VK_TRUE;
    result.Vulkan12.timelineSemaphore = VK_TRUE;
    result.Vulkan12.samplerMirrorClampToEdge = VK_TRUE;
    result.Vulkan12.samplerFilterMinmax = VK_TRUE;
    result.Vulkan12.imagelessFramebuffer = VK_TRUE;
    result.Vulkan12.separateDepthStencilLayouts = VK_TRUE;
    return result;
}

TEST(VulkanInitialization, AllocatesMultipleQueuesAndSharesFamilyCapacity) {
    const VkQueueFamilyProperties families[]{
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, .queueCount = 4, .timestampValidBits = 0, .minImageTransferGranularity = {}},
        {.queueFlags = VK_QUEUE_COMPUTE_BIT, .queueCount = 2, .timestampValidBits = 0, .minImageTransferGranularity = {}}};
    std::array<uint32_t, 2> used{};
    const auto direct = AllocateQueueIndicesVulkan(QueueType::Direct, 2, families, used);
    ASSERT_EQ(direct.size(), 2u);
    EXPECT_EQ(direct[0].Family, 0u);
    EXPECT_EQ(direct[0].IndexInFamily, 0u);
    EXPECT_EQ(direct[1].Family, 0u);
    EXPECT_EQ(direct[1].IndexInFamily, 1u);
    const auto compute = AllocateQueueIndicesVulkan(QueueType::Compute, 3, families, used);
    ASSERT_EQ(compute.size(), 3u);
    EXPECT_EQ(compute[0].Family, 0u);
    EXPECT_EQ(compute[0].IndexInFamily, 2u);
    EXPECT_EQ(compute[1].Family, 0u);
    EXPECT_EQ(compute[1].IndexInFamily, 3u);
    EXPECT_EQ(compute[2].Family, 1u);
    EXPECT_EQ(compute[2].IndexInFamily, 0u);
    const auto remaining = AllocateQueueIndicesVulkan(QueueType::Compute, 2, families, used);
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0].Family, 1u);
    EXPECT_EQ(remaining[0].IndexInFamily, 1u);
    EXPECT_TRUE(AllocateQueueIndicesVulkan(QueueType::Copy, 1, families, used).empty());
}

TEST(VulkanInitialization, DirectRequiresBothGraphicsAndCompute) {
    const VkQueueFamilyProperties families[]{
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT, .queueCount = 2, .timestampValidBits = 0, .minImageTransferGranularity = {}},
        {.queueFlags = VK_QUEUE_COMPUTE_BIT, .queueCount = 2, .timestampValidBits = 0, .minImageTransferGranularity = {}},
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, .queueCount = 1, .timestampValidBits = 0, .minImageTransferGranularity = {}}};
    std::array<uint32_t, 3> used{};
    const auto queues = AllocateQueueIndicesVulkan(QueueType::Direct, 2, families, used);
    ASSERT_EQ(queues.size(), 1u);
    EXPECT_EQ(queues[0].Family, 2u);
    EXPECT_EQ(used, (std::array<uint32_t, 3>{0, 0, 1}));
}

TEST(VulkanInitialization, CopyAcceptsImplicitAndExplicitTransferSupport) {
    const VkQueueFlags flags[]{VK_QUEUE_GRAPHICS_BIT, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, VK_QUEUE_TRANSFER_BIT};
    for (const auto flag : flags) {
        SCOPED_TRACE(flag);
        const VkQueueFamilyProperties family{.queueFlags = flag, .queueCount = 1, .timestampValidBits = 0, .minImageTransferGranularity = {}};
        uint32_t used = 0;
        const auto queues = AllocateQueueIndicesVulkan(QueueType::Copy, 1, std::span{&family, 1}, std::span{&used, 1});
        ASSERT_EQ(queues.size(), 1u);
        EXPECT_EQ(queues[0].Family, 0u);
        EXPECT_EQ(queues[0].IndexInFamily, 0u);
    }
    const VkQueueFamilyProperties unsupported{.queueFlags = VK_QUEUE_SPARSE_BINDING_BIT, .queueCount = 1, .timestampValidBits = 0, .minImageTransferGranularity = {}};
    uint32_t used = 0;
    EXPECT_TRUE(AllocateQueueIndicesVulkan(QueueType::Copy, 1, std::span{&unsupported, 1}, std::span{&used, 1}).empty());
    EXPECT_EQ(used, 0u);
}

TEST(VulkanInitialization, EnumerationUsesWrittenCountAndRetriesIncomplete) {
    vector<uint32_t> values;
    uint32_t attempts = 0;
    const auto enumerate = [&](uint32_t* count, uint32_t* output) {
        if (!output) {
            *count = attempts == 0 ? 1 : 4;
            return VK_SUCCESS;
        }
        output[0] = 17;
        if (attempts++ == 0) return VK_INCOMPLETE;
        output[1] = 29;
        *count = 2;
        return VK_SUCCESS;
    };
    EXPECT_EQ(EnumerateVectorFromVkFunc(values, enumerate), VK_SUCCESS);
    EXPECT_EQ(attempts, 2u);
    EXPECT_EQ(values, (vector<uint32_t>{17, 29}));
}

TEST(VulkanInitialization, EmptyEnumerationClearsPreviousResults) {
    vector<uint32_t> values{1, 2, 3};
    uint32_t calls = 0;
    EXPECT_EQ(EnumerateVectorFromVkFunc(values, [&](uint32_t* count, uint32_t* output) {
                  ++calls;
                  EXPECT_EQ(output, nullptr);
                  *count = 0;
                  return VK_SUCCESS;
              }),
              VK_SUCCESS);
    EXPECT_TRUE(values.empty());
    EXPECT_EQ(calls, 1u);
    values = {1, 2, 3};
    EnumerateVectorFromVkFunc(values, [](uint32_t* count, uint32_t* output) {
        EXPECT_EQ(output, nullptr);
        *count = 0;
    });
    EXPECT_TRUE(values.empty());
}

TEST(VulkanInitialization, VoidEnumerationUsesWrittenCount) {
    vector<uint32_t> values;
    EnumerateVectorFromVkFunc(values, [](uint32_t* count, uint32_t* output) {
        if (!output) {
            *count = 3;
        } else {
            output[0] = 42;
            *count = 1;
        }
    });
    EXPECT_EQ(values, (vector<uint32_t>{42}));
}

TEST(VulkanInitialization, UnsupportedTimestampsAndCopyQueuesDoNotInitializeProfiler) {
    InstanceVulkanImpl instance{VK_NULL_HANDLE, std::nullopt, {}, {}};
    DeviceVulkan device{&instance, VK_NULL_HANDLE, VK_NULL_HANDLE};
    device._ftb = {};
    device._ftb.vkCreateCommandPool = [](VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*) {
        ADD_FAILURE() << "Unsupported queue must not create profiler command resources";
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    const VkQueueFlags flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    QueueVulkan direct{&device, VK_NULL_HANDLE, {}, QueueType::Direct, flags, 0};
    QueueVulkan compute{&device, VK_NULL_HANDLE, {}, QueueType::Compute, flags, 0};
    QueueVulkan copy{&device, VK_NULL_HANDLE, {}, QueueType::Copy, flags, 64};
#ifdef RADRAY_ENABLE_PROFILER
    EXPECT_EQ(direct._profilerContext, nullptr);
    EXPECT_EQ(compute._profilerContext, nullptr);
    EXPECT_EQ(copy._profilerContext, nullptr);
#endif
}

TEST(VulkanDeviceFeaturePolicy, CoupledLayoutsDoNotRequireIndependentTransitions) {
    auto requests = GetRhiFeatureRequestsVulkan();
    requests.SeparateDepthStencilLayouts = FeatureRequirementVulkan::Disabled;
    EXPECT_FALSE(SelectDeviceFeaturesVulkan(AllSupportedFeatures(), requests).Enabled.Vulkan12.separateDepthStencilLayouts);
    EXPECT_EQ(TextureStateToLayout(TextureState::DepthReadStencilWrite), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(TextureStateToLayout(TextureState::DepthWriteStencilRead), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL);
    EXPECT_EQ(TextureStateToLayout(TextureState::DepthReadStencilWrite, TextureFormat::D32_FLOAT_S8_UINT, TextureAspect::Stencil, true), VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(TextureStateForAspect(TextureState::DepthReadStencilWrite | TextureState::ShaderRead, TextureAspect::Stencil), TextureState::DepthWrite);
}

TEST(VulkanDeviceFeaturePolicy, AllSupportedEnablesExactlyRhiRequests) {
    const auto supported = AllSupportedFeatures();
    const auto before = FeatureBits(supported);
    const auto selection = SelectDeviceFeaturesVulkan(supported, GetRhiFeatureRequestsVulkan());
    EXPECT_TRUE(selection.MissingRequired.empty());
    EXPECT_EQ(FeatureBits(selection.Enabled), FeatureBits(ExpectedRhiFeatures()));
    EXPECT_EQ(selection.EnabledNames.size(), 17u);
    EXPECT_EQ(FeatureBits(supported), before);
    EXPECT_EQ(selection.Enabled.Vulkan11.pNext, nullptr);
    EXPECT_EQ(selection.Enabled.Vulkan12.pNext, nullptr);
}

TEST(VulkanDeviceFeaturePolicy, EverySupportBitIsIndependentAndOnlyTimelineIsRequired) {
    const auto expected = FeatureBits(ExpectedRhiFeatures());
    for (size_t missing = 0; missing < expected.size(); ++missing) {
        SCOPED_TRACE(missing);
        auto supported = AllSupportedFeatures();
        size_t index = 0;
        VisitFeatureBits(supported, [&](VkBool32& value) {
            if (index++ == missing) value = VK_FALSE;
        });
        const auto before = FeatureBits(supported);
        const auto selection = SelectDeviceFeaturesVulkan(supported, GetRhiFeatureRequestsVulkan());
        auto masked = expected;
        masked[missing] = VK_FALSE;
        EXPECT_EQ(FeatureBits(selection.Enabled), masked);
        EXPECT_EQ(FeatureBits(supported), before);
        if (supported.Vulkan12.timelineSemaphore) {
            EXPECT_TRUE(selection.MissingRequired.empty());
        } else {
            EXPECT_EQ(selection.MissingRequired, (vector<std::string_view>{"timelineSemaphore"}));
        }
    }
}

TEST(VulkanDeviceFeaturePolicy, TimelineAloneIsSufficientAndEmptyRequestsEnableNothing) {
    DeviceFeaturesVulkan supported;
    supported.Vulkan12.timelineSemaphore = VK_TRUE;
    const auto selection = SelectDeviceFeaturesVulkan(supported, GetRhiFeatureRequestsVulkan());
    EXPECT_TRUE(selection.MissingRequired.empty());
    EXPECT_EQ(FeatureBits(selection.Enabled), FeatureBits(supported));
    const auto disabled = SelectDeviceFeaturesVulkan(AllSupportedFeatures(), {});
    EXPECT_TRUE(disabled.MissingRequired.empty());
    EXPECT_TRUE(disabled.EnabledNames.empty());
    EXPECT_EQ(FeatureBits(disabled.Enabled), FeatureBits(DeviceFeaturesVulkan{}));
}

TEST(VulkanDeviceFeaturePolicy, RequirementsAreSelectedBeforeCreation) {
    auto requested = GetRhiFeatureRequestsVulkan();
    requested.SamplerAnisotropy = FeatureRequirementVulkan::Required;
    requested.TimelineSemaphore = FeatureRequirementVulkan::Disabled;
    const auto selection = SelectDeviceFeaturesVulkan({}, requested);
    EXPECT_EQ(selection.MissingRequired, (vector<std::string_view>{"samplerAnisotropy"}));
    EXPECT_TRUE(selection.EnabledNames.empty());
}

TEST(VulkanDeviceFeaturePolicy, QueryAndCreateChainsAreIndependentAndSnapshotsOwnNoLinks) {
    DeviceFeaturesVulkan supported;
    DeviceFeaturesVulkan enabled;
    {
        FeatureChainVulkan query{AllSupportedFeatures()};
        supported = query.Snapshot();
        const auto selection = SelectDeviceFeaturesVulkan(supported, GetRhiFeatureRequestsVulkan());
        FeatureChainVulkan create{selection.Enabled};
        VkPhysicalDeviceFeatures legacy{};
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pEnabledFeatures = &legacy;
        create.ApplyTo(info);
        EXPECT_EQ(info.pEnabledFeatures, nullptr);
        EXPECT_EQ(info.pNext, &create.Core);
        EXPECT_EQ(create.Core.sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        EXPECT_EQ(create.Core.pNext, &create.Vulkan11);
        EXPECT_EQ(create.Vulkan11.sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        EXPECT_EQ(create.Vulkan11.pNext, &create.Vulkan12);
        EXPECT_EQ(create.Vulkan12.sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        EXPECT_EQ(create.Vulkan12.pNext, nullptr);
        EXPECT_EQ(query.Core.pNext, &query.Vulkan11);
        EXPECT_EQ(query.Vulkan11.pNext, &query.Vulkan12);
        enabled = create.Snapshot();
        EXPECT_EQ(FeatureBits(query.Snapshot()), FeatureBits(AllSupportedFeatures()));
    }
    EXPECT_EQ(supported.Vulkan11.pNext, nullptr);
    EXPECT_EQ(supported.Vulkan12.pNext, nullptr);
    EXPECT_EQ(enabled.Vulkan11.pNext, nullptr);
    EXPECT_EQ(enabled.Vulkan12.pNext, nullptr);
    EXPECT_EQ(FeatureBits(enabled), FeatureBits(ExpectedRhiFeatures()));
}

TEST(VulkanDeviceFeaturePolicy, MissingUsedCoreDeviceEntryPointsAreRejected) {
    const auto stub = +[]() {};
    VolkDeviceTable functions{};
    functions.vkWaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(stub);
    functions.vkGetSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(stub);
    functions.vkGetDescriptorSetLayoutSupport = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(stub);
    functions.vkGetBufferMemoryRequirements2 = reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(stub);
    functions.vkGetImageMemoryRequirements2 = reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(stub);
    functions.vkBindBufferMemory2 = reinterpret_cast<PFN_vkBindBufferMemory2>(stub);
    functions.vkBindImageMemory2 = reinterpret_cast<PFN_vkBindImageMemory2>(stub);
    functions.vkCreateRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(stub);
    functions.vkCmdBeginRenderPass2 = reinterpret_cast<PFN_vkCmdBeginRenderPass2>(stub);
    functions.vkCmdEndRenderPass2 = reinterpret_cast<PFN_vkCmdEndRenderPass2>(stub);
    EXPECT_TRUE(GetMissingDeviceFunctionVulkan(functions).empty());
    auto checkMissing = [&](auto member, std::string_view name) {
        auto incomplete = functions;
        incomplete.*member = nullptr;
        EXPECT_EQ(GetMissingDeviceFunctionVulkan(incomplete), name);
    };
    checkMissing(&VolkDeviceTable::vkWaitSemaphores, "vkWaitSemaphores");
    checkMissing(&VolkDeviceTable::vkGetSemaphoreCounterValue, "vkGetSemaphoreCounterValue");
    checkMissing(&VolkDeviceTable::vkGetDescriptorSetLayoutSupport, "vkGetDescriptorSetLayoutSupport");
    checkMissing(&VolkDeviceTable::vkGetBufferMemoryRequirements2, "vkGetBufferMemoryRequirements2");
    checkMissing(&VolkDeviceTable::vkGetImageMemoryRequirements2, "vkGetImageMemoryRequirements2");
    checkMissing(&VolkDeviceTable::vkBindBufferMemory2, "vkBindBufferMemory2");
    checkMissing(&VolkDeviceTable::vkBindImageMemory2, "vkBindImageMemory2");
    checkMissing(&VolkDeviceTable::vkCreateRenderPass2, "vkCreateRenderPass2");
    checkMissing(&VolkDeviceTable::vkCmdBeginRenderPass2, "vkCmdBeginRenderPass2");
    checkMissing(&VolkDeviceTable::vkCmdEndRenderPass2, "vkCmdEndRenderPass2");
}

struct FeatureConsumerProbe {
    struct DrawCall {
        VkDeviceSize Offset;
        uint32_t Count;
        uint32_t Stride;
    };
    vector<DrawCall> Draws;
    vector<DrawCall> IndexedDraws;
    uint32_t SamplerCalls{0};
    VkSamplerCreateInfo SamplerInfo{};
    VkSamplerReductionMode Reduction{VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    vector<uint32_t> ShaderCode;
    InstanceVulkanImpl Instance{VK_NULL_HANDLE, std::nullopt, {}, {}};
    DeviceVulkan Device{&Instance, VK_NULL_HANDLE, reinterpret_cast<VkDevice>(this)};
    QueueVulkan Queue{&Device, VK_NULL_HANDLE, {}, QueueType::Direct, 0};
    CommandAllocatorVulkan Allocator{&Device, &Queue, VK_NULL_HANDLE};
    CommandBufferVulkan Command{&Device, &Queue, &Allocator, reinterpret_cast<VkCommandBuffer>(this)};
    BufferVulkan Arguments{&Device, VK_NULL_HANDLE, VK_NULL_HANDLE, {}};

    FeatureConsumerProbe() {
        Device._ftb = {};
        Device._properties = {};
        Device._properties.limits.maxDrawIndirectCount = 2;
        Device._properties.limits.maxSamplerAnisotropy = 16;
        Device._supportedFeatures = AllSupportedFeatures();
        Device._ftb.vkDestroyDevice = [](VkDevice, const VkAllocationCallbacks*) {};
        Device._ftb.vkFreeCommandBuffers = [](VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*) {};
        Device._ftb.vkDestroySampler = [](VkDevice, VkSampler, const VkAllocationCallbacks*) {};
        Device._ftb.vkDestroyShaderModule = [](VkDevice, VkShaderModule, const VkAllocationCallbacks*) {};
        Device._ftb.vkCreateShaderModule = [](VkDevice device, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks*, VkShaderModule* shader) {
            auto& probe = *reinterpret_cast<FeatureConsumerProbe*>(device);
            probe.ShaderCode.assign(info->pCode, info->pCode + info->codeSize / sizeof(uint32_t));
            *shader = reinterpret_cast<VkShaderModule>(&probe);
            return VK_SUCCESS;
        };
        Device._ftb.vkCreateSampler = [](VkDevice device, const VkSamplerCreateInfo* info, const VkAllocationCallbacks*, VkSampler* sampler) {
            auto& probe = *reinterpret_cast<FeatureConsumerProbe*>(device);
            ++probe.SamplerCalls;
            probe.SamplerInfo = *info;
            probe.SamplerInfo.pNext = nullptr;
            probe.Reduction = info->pNext ? static_cast<const VkSamplerReductionModeCreateInfo*>(info->pNext)->reductionMode : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
            *sampler = reinterpret_cast<VkSampler>(&probe);
            return VK_SUCCESS;
        };
        Device._ftb.vkCmdDrawIndirect = [](VkCommandBuffer command, VkBuffer, VkDeviceSize offset, uint32_t count, uint32_t stride) {
            reinterpret_cast<FeatureConsumerProbe*>(command)->Draws.push_back({offset, count, stride});
        };
        Device._ftb.vkCmdDrawIndexedIndirect = [](VkCommandBuffer command, VkBuffer, VkDeviceSize offset, uint32_t count, uint32_t stride) {
            reinterpret_cast<FeatureConsumerProbe*>(command)->IndexedDraws.push_back({offset, count, stride});
        };
    }
};

TEST(VulkanShaderBytecode, FixturesNeedNoGoogleReflectionAndReachDriverUnmodified) {
    FeatureConsumerProbe probe;
    const auto root = std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_bytecode";
    std::error_code error;
    std::filesystem::directory_iterator files{root, error};
    ASSERT_FALSE(error) << error.message();
    size_t checked = 0;
    for (const auto& file : files) {
        if (!file.path().filename().string().ends_with(".spirv.bin")) continue;
        SCOPED_TRACE(file.path().filename().string());
        const auto bytes = ReadBinaryFile(file.path());
        ASSERT_TRUE(bytes);
        ASSERT_EQ(bytes->size() % sizeof(uint32_t), 0u);
        vector<uint32_t> words(bytes->size() / sizeof(uint32_t));
        ASSERT_GE(words.size(), 5u);
        std::memcpy(words.data(), bytes->data(), bytes->size());
        ASSERT_EQ(words[0], 0x07230203u);
        for (size_t offset = 5; offset < words.size();) {
            const uint32_t count = words[offset] >> 16;
            ASSERT_GT(count, 0u);
            ASSERT_LE(count, words.size() - offset);
            if ((words[offset] & 0xffffu) == 10u) {
                const std::string_view text{reinterpret_cast<const char*>(words.data() + offset + 1), (count - 1) * sizeof(uint32_t)};
                const auto terminator = text.find('\0');
                ASSERT_NE(terminator, std::string_view::npos);
                EXPECT_FALSE(text.substr(0, terminator).starts_with("SPV_GOOGLE_"));
            }
            offset += count;
        }
        EXPECT_TRUE(probe.Device.CreateShader({*bytes, ShaderBlobCategory::SPIRV, ShaderStage::Compute}));
        EXPECT_EQ(probe.ShaderCode, words);
        vector<byte> unaligned(bytes->size() + 1);
        std::memcpy(unaligned.data() + 1, bytes->data(), bytes->size());
        EXPECT_TRUE(probe.Device.CreateShader({std::span<const byte>{unaligned}.subspan(1), ShaderBlobCategory::SPIRV, ShaderStage::Compute}));
        EXPECT_EQ(probe.ShaderCode, words);
        ++checked;
    }
    EXPECT_GT(checked, 0u);
    RecordProperty("spirv_fixture_count", checked);
}

TEST(VulkanDeviceFeatureConsumers, SamplersReadEnabledInsteadOfSupported) {
    FeatureConsumerProbe probe;
    EXPECT_TRUE(probe.Device.CreateSampler({}));
    SamplerDescriptor anisotropic;
    anisotropic.AnisotropyClamp = 4;
    SamplerDescriptor mirror;
    mirror.AddressS = AddressMode::MirrorClampToEdge;
    SamplerDescriptor minmax;
    minmax.Reduction = SamplerReductionMode::Min;
    EXPECT_FALSE(probe.Device.CreateSampler(anisotropic));
    EXPECT_FALSE(probe.Device.CreateSampler(mirror));
    EXPECT_FALSE(probe.Device.CreateSampler(minmax));
    EXPECT_EQ(probe.SamplerCalls, 1u);
    probe.Device._enabledFeatures = ExpectedRhiFeatures();
    EXPECT_TRUE(probe.Device.CreateSampler(anisotropic));
    EXPECT_EQ(probe.SamplerInfo.anisotropyEnable, VK_TRUE);
    EXPECT_EQ(probe.SamplerInfo.maxAnisotropy, 4.0f);
    EXPECT_TRUE(probe.Device.CreateSampler(mirror));
    EXPECT_EQ(probe.SamplerInfo.addressModeU, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
    EXPECT_TRUE(probe.Device.CreateSampler(minmax));
    EXPECT_EQ(probe.Reduction, VK_SAMPLER_REDUCTION_MODE_MIN);
    EXPECT_EQ(probe.SamplerCalls, 4u);
}

TEST(VulkanDeviceFeatureConsumers, SamplerLodBiasUsesAdvertisedRange) {
    FeatureConsumerProbe probe;
    probe.Device._properties.limits.maxSamplerLodBias = 16;
    for (const float bias : {-1.0f, 1.0f}) EXPECT_FALSE(probe.Device.CreateSampler({.MipLodBias = bias}));
    EXPECT_EQ(probe.SamplerCalls, 0u);
    EXPECT_TRUE(probe.Device.CreateSampler({}));
    probe.Device._capabilities.Limits.MinSamplerLodBias = -16;
    probe.Device._capabilities.Limits.MaxSamplerLodBias = 16;
    for (const float bias : {-16.0f, 16.0f}) {
        EXPECT_TRUE(probe.Device.CreateSampler({.MipLodBias = bias}));
        EXPECT_EQ(probe.SamplerInfo.mipLodBias, bias);
    }
    for (const float bias : {-17.0f, 17.0f}) EXPECT_FALSE(probe.Device.CreateSampler({.MipLodBias = bias}));
    EXPECT_EQ(probe.SamplerCalls, 3u);
}

TEST(VulkanDeviceFeatureConsumers, IndirectBatchingAndSingleDrawFallbackReadEnabled) {
    FeatureConsumerProbe probe;
    SimulateCommandEncoderVulkan encoder{&probe.Device, &probe.Command};
    for (const bool enabled : {false, true}) {
        SCOPED_TRACE(enabled);
        probe.Device._enabledFeatures.Core.multiDrawIndirect = enabled;
        probe.Draws.clear();
        probe.IndexedDraws.clear();
        encoder.DrawIndirect(&probe.Arguments, 32, 5);
        encoder.DrawIndexedIndirect(&probe.Arguments, 64, 5);
        ASSERT_EQ(probe.Draws.size(), enabled ? 3u : 5u);
        ASSERT_EQ(probe.IndexedDraws.size(), probe.Draws.size());
        uint32_t first = 0;
        for (size_t index = 0; index < probe.Draws.size(); ++index) {
            const uint32_t count = enabled ? std::min(2u, 5u - first) : 1u;
            EXPECT_EQ(probe.Draws[index].Count, count);
            EXPECT_EQ(probe.Draws[index].Offset, 32 + first * sizeof(DrawIndirectArguments));
            EXPECT_EQ(probe.Draws[index].Stride, sizeof(DrawIndirectArguments));
            EXPECT_EQ(probe.IndexedDraws[index].Count, count);
            EXPECT_EQ(probe.IndexedDraws[index].Offset, 64 + first * sizeof(DrawIndexedIndirectArguments));
            EXPECT_EQ(probe.IndexedDraws[index].Stride, sizeof(DrawIndexedIndirectArguments));
            first += count;
        }
        encoder.DrawIndirect(&probe.Arguments, 0, 0);
        encoder.DrawIndexedIndirect(&probe.Arguments, 0, 0);
        EXPECT_EQ(probe.Draws.size(), enabled ? 3u : 5u);
        EXPECT_EQ(probe.IndexedDraws.size(), probe.Draws.size());
    }
}

class VulkanGpuTest : public testing::Test {
protected:
    test::DeviceContext Context;

    void SetUp() override {
        if (!test::TryCreateDevice(RenderBackend::Vulkan, Context, true)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(RenderBackend::Vulkan))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
    }

    void TearDown() override {
        Context.Reset();
    }
};

TEST_F(VulkanGpuTest, TexelOffsetsUseTheirOwnAlignment) {
    auto& native = static_cast<DeviceVulkan&>(*Context.Device);
    const auto original = native._capabilities.Limits;
    const auto restore = MakeScopeGuard([&] { native._capabilities.Limits = original; });
    const auto alignment = std::max(uint64_t{4}, original.TexelBufferOffsetAlignment);
    auto device = render::detail::CreateValidationDevice(Context.Device, {.IsEnabled = true, .LogCallback = test::CaptureRhiValidationMessage, .LogUserData = &Context}).Unwrap();
    auto buffer = device->CreateBuffer({.Size = alignment * 4, .Memory = MemoryType::Device, .Usage = BufferUse::Resource | BufferUse::UnorderedAccess}).Unwrap();
    for (const auto kind : {BindingKind::TypedBuffer, BindingKind::RWTypedBuffer}) {
        BindingDescriptor binding{.BindingId = 0, .Kind = kind, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = {}, .Vulkan = VulkanBindingDescriptor{0, 0}, .ImmutableSampler = {}};
        auto group = device->CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
        auto parameters = device->CreateShaderParameterSet({group.get()}).Unwrap();
        const ShaderTexelBufferBinding value{buffer.get(), {alignment, 4}, TextureFormat::R32_UINT};
        native._capabilities.Limits.TexelBufferOffsetAlignment = alignment * 2;
        native._capabilities.Limits.StorageBufferOffsetAlignment = alignment;
        EXPECT_FALSE(parameters->Set(0, 0, value));
        native._capabilities.Limits.TexelBufferOffsetAlignment = alignment;
        native._capabilities.Limits.StorageBufferOffsetAlignment = alignment * 2;
        EXPECT_TRUE(parameters->Set(0, 0, value));
    }
    EXPECT_EQ(Context.RhiValidationErrors.load(), 2u);
}

TEST_F(VulkanGpuTest, StorageRangesRespectLimitForExplicitAndWholeBuffer) {
    auto& native = static_cast<DeviceVulkan&>(*Context.Device);
    const auto original = native._capabilities.Limits;
    const auto restore = MakeScopeGuard([&] { native._capabilities.Limits = original; });
    const auto size = std::max(uint64_t{64}, original.StorageBufferOffsetAlignment);
    native._capabilities.Limits.MaxStorageBufferRange = size;
    auto device = render::detail::CreateValidationDevice(Context.Device, {.IsEnabled = true, .LogCallback = test::CaptureRhiValidationMessage, .LogUserData = &Context}).Unwrap();
    auto buffer = device->CreateBuffer({.Size = size * 4, .Memory = MemoryType::Device, .Usage = BufferUse::Resource | BufferUse::UnorderedAccess}).Unwrap();
    for (const bool dynamic : {false, true})
        for (const auto kind : {BindingKind::StructuredBuffer, BindingKind::RWStructuredBuffer, BindingKind::RawBuffer, BindingKind::RWRawBuffer}) {
            BindingDescriptor binding{.BindingId = 0, .Kind = kind, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = {}, .Vulkan = VulkanBindingDescriptor{0, 0, dynamic ? VulkanBufferDescriptorPlacement::Dynamic : VulkanBufferDescriptorPlacement::Regular}, .ImmutableSampler = {}};
            auto group = device->CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
            auto parameters = device->CreateShaderParameterSet({group.get()}).Unwrap();
            const uint32_t stride = kind == BindingKind::StructuredBuffer || kind == BindingKind::RWStructuredBuffer ? 4 : 0;
            EXPECT_TRUE(parameters->Set(0, 0, ShaderBufferBinding{buffer.get(), {0, size}, stride}));
            EXPECT_FALSE(parameters->Set(0, 0, ShaderBufferBinding{buffer.get(), {0, size + 4}, stride}));
            EXPECT_FALSE(parameters->Set(0, 0, ShaderBufferBinding{buffer.get(), {0, BufferRange::All()}, stride}));
            EXPECT_TRUE(parameters->Set(0, 0, ShaderBufferBinding{buffer.get(), {3 * size, BufferRange::All()}, stride}));
        }
    EXPECT_EQ(Context.RhiValidationErrors.load(), 16u);
}

class VulkanDeviceFeatureGpu : public VulkanGpuTest, public testing::WithParamInterface<uint32_t> {};
class VulkanFramebufferGraphicsGpu : public VulkanGpuTest, public testing::WithParamInterface<FramebufferModeVulkan> {};

TEST_P(VulkanFramebufferGraphicsGpu, GoogleFreeGraphicsDrawsThroughFramebuffer) {
    auto& device = *Context.Device;
    auto& native = static_cast<DeviceVulkan&>(device);
    if (GetParam() == FramebufferModeVulkan::Imageless && !native._enabledFeatures.Vulkan12.imagelessFramebuffer)
        GTEST_SKIP() << "imagelessFramebuffer is unavailable";
    native._renderPassCache = make_unique<VulkanRenderPassCache>(&native, GetParam());
    const auto root = std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_bytecode";
    const auto vertexBytes = ReadBinaryFile(root / "no_resource_graphics.VSMain.spirv.bin");
    const auto pixelBytes = ReadBinaryFile(root / "no_resource_graphics.PSMain.spirv.bin");
    ASSERT_TRUE(vertexBytes);
    ASSERT_TRUE(pixelBytes);
    auto vertexShader = device.CreateShader({*vertexBytes, ShaderBlobCategory::SPIRV, ShaderStage::Vertex}).Unwrap();
    auto pixelShader = device.CreateShader({*pixelBytes, ShaderBlobCategory::SPIRV, ShaderStage::Pixel}).Unwrap();
    auto layout = device.CreatePipelineLayout({}).Unwrap();
    const VertexBufferLayout vertexLayout{0, 3 * sizeof(float), VertexStepMode::Vertex};
    const VertexAttribute attribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0};
    const auto color = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    auto primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    primitive.UnclippedDepth = false;
    auto pipeline = device.CreateGraphicsPipelineState({.PipelineLayout = layout.get(),
                                                        .VS = ShaderEntry{vertexShader.get(), "VSMain"},
                                                        .PS = ShaderEntry{pixelShader.get(), "PSMain"},
                                                        .VertexInput = {std::span{&vertexLayout, 1}, std::span{&attribute, 1}},
                                                        .Primitive = primitive,
                                                        .DepthStencil = {},
                                                        .MultiSample = MultiSampleState::Default(),
                                                        .ColorTargets = std::span{&color, 1}})
                        .Unwrap();
    const float vertices[]{-1, -1, 0, 3, -1, 0, -1, 3, 0};
    auto vertexBuffer = test::MakeUploadBuffer(device, std::as_bytes(std::span{vertices}), BufferUse::Vertex).Unwrap();
    auto target = test::MakeRenderTarget(&device, TextureFormat::RGBA8_UNORM, 8, 8, TextureUse::RenderTarget | TextureUse::CopySource);
    ASSERT_TRUE(target);
    const uint64_t alignment = std::max<uint64_t>(1, device.GetCapabilities().Detail.TextureDataPitchAlignment);
    const uint64_t rowPitch = (32 + alignment - 1) / alignment * alignment;
    auto readback = device.CreateBuffer({.Size = rowPitch * 8, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor toRender = BarrierTextureDescriptor{target->Tex.get(), TextureState::Undefined, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toRender, 1});
    const RenderPassColorAttachment attachment{target->View.get(), LoadAction::Clear, StoreAction::Store, {}};
    auto encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8}).Unwrap();
    encoder->BindGraphicsPipelineState(pipeline.get());
    encoder->SetViewport({0, 0, 8, 8, 0, 1});
    encoder->SetScissor({0, 0, 8, 8});
    const VertexBufferBinding vertexBinding{0, {vertexBuffer.get(), 0, sizeof(vertices)}};
    encoder->BindVertexBuffers(std::span{&vertexBinding, 1});
    encoder->Draw(3, 1, 0, 0);
    command->EndRenderPass(std::move(encoder));
    const ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{target->Tex.get(), TextureState::RenderTarget, TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyTextureToBuffer(readback.get(), 0, target->Tex.get(), {0, 1, 0, 1});
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard gpuWork{Context};
    Context.Queue->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    const auto* pixels = static_cast<const uint8_t*>(readback->Map(0, rowPitch * 8));
    ASSERT_NE(pixels, nullptr);
    readback->InvalidateMappedRange({0, rowPitch * 8});
    const auto center = rowPitch * 4 + 4 * 4;
    EXPECT_EQ(pixels[center], 255);
    EXPECT_EQ(pixels[center + 1], 0);
    EXPECT_EQ(pixels[center + 2], 255);
    EXPECT_EQ(pixels[center + 3], 255);
    readback->Unmap();
}

TEST_P(VulkanDeviceFeatureGpu, CreatesMultipleQueuesWithStablePriorities) {
    auto* initial = static_cast<DeviceVulkan*>(Context.Device.get());
    const auto adapters = initial->_instance->GetPhysicalDevices();
    if (GetParam() >= adapters.size()) GTEST_SKIP() << "Vulkan adapter index " << GetParam() << " is unavailable";
    const auto& adapter = adapters[GetParam()];
    RecordProperty("adapter", adapter.Name);
    const VulkanCommandQueueDescriptor initialQueues[]{{QueueType::Direct, 1}};
    auto selected = CreateDeviceVulkan({.PhysicalDeviceIndex = adapter.Index, .Queues = initialQueues});
    ASSERT_TRUE(selected);
    vector<VkQueueFamilyProperties> families;
    EnumerateVectorFromVkFunc(families, vkGetPhysicalDeviceQueueFamilyProperties, selected.Get()->_physicalDevice);
    vector<uint32_t> used(families.size(), 0);
    const auto direct = AllocateQueueIndicesVulkan(QueueType::Direct, 1, families, used);
    ASSERT_EQ(direct.size(), 1u);
    const auto family = direct[0].Family;
    const auto directCount = families[family].queueCount;
    used[family] = directCount;
    const auto compute = AllocateQueueIndicesVulkan(QueueType::Compute, 1, families, used);
    if (directCount < 2 && compute.empty()) GTEST_SKIP() << "Adapter exposes only one usable queue";
    vector<VulkanCommandQueueDescriptor> requests{{QueueType::Direct, directCount}};
    if (!compute.empty()) requests.push_back({QueueType::Compute, 1});
    auto created = CreateDeviceVulkan({.PhysicalDeviceIndex = adapter.Index, .Queues = requests});
    ASSERT_TRUE(created);
    for (uint32_t i = 0; i < directCount; ++i) {
        auto queue = created.Get()->GetCommandQueue(QueueType::Direct, i);
        ASSERT_TRUE(queue);
        const auto* native = static_cast<QueueVulkan*>(queue.Get());
        EXPECT_EQ(native->_family.Family, family);
        EXPECT_EQ(native->_family.IndexInFamily, i);
        queue.Get()->Wait();
    }
    if (!compute.empty()) {
        auto queue = created.Get()->GetCommandQueue(QueueType::Compute, 0);
        ASSERT_TRUE(queue);
        const auto* native = static_cast<QueueVulkan*>(queue.Get());
        EXPECT_EQ(native->_family.Family, compute[0].Family);
        EXPECT_EQ(native->_family.IndexInFamily, compute[0].IndexInFamily);
        queue.Get()->Wait();
    }
}

TEST_P(VulkanDeviceFeatureGpu, MatchesPolicyAndCompletesTimeline) {
    auto* initial = static_cast<DeviceVulkan*>(Context.Device.get());
    const auto adapters = initial->_instance->GetPhysicalDevices();
    ASSERT_FALSE(adapters.empty());
    if (GetParam() >= adapters.size()) GTEST_SKIP() << "Vulkan adapter index " << GetParam() << " is unavailable";
    {
        const auto& adapter = adapters[GetParam()];
        SCOPED_TRACE(adapter.Name);
        Context.Queue->Wait();
        Context.Queue = nullptr;
        Context.Device.reset();
        const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
        auto created = CreateDeviceVulkan({.PhysicalDeviceIndex = adapter.Index, .Queues = queues});
        ASSERT_TRUE(created);
        auto* device = created.Get();
        Context.Device = created.Release();
        Context.Queue = device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        Context.AdapterName = adapter.Name;
        RecordProperty("adapter", adapter.Name);
        RecordProperty("vulkan_adapter_index", adapter.Index);
        const auto expected = SelectDeviceFeaturesVulkan(device->_supportedFeatures, GetRhiFeatureRequestsVulkan());
        EXPECT_TRUE(expected.MissingRequired.empty());
        EXPECT_EQ(FeatureBits(device->_enabledFeatures), FeatureBits(expected.Enabled));
        EXPECT_EQ(device->_renderPassCache->IsImageless(), device->_enabledFeatures.Vulkan12.imagelessFramebuffer != 0);
        FeatureChainVulkan queried;
        vkGetPhysicalDeviceFeatures2(device->_physicalDevice, &queried.Core);
        EXPECT_EQ(FeatureBits(device->_supportedFeatures), FeatureBits(queried.Snapshot()));
        EXPECT_EQ(device->_supportedFeatures.Vulkan11.pNext, nullptr);
        EXPECT_EQ(device->_supportedFeatures.Vulkan12.pNext, nullptr);
        EXPECT_EQ(device->_enabledFeatures.Vulkan11.pNext, nullptr);
        EXPECT_EQ(device->_enabledFeatures.Vulkan12.pNext, nullptr);
        EXPECT_EQ(device->_extProperties.Vulkan11.pNext, nullptr);
        EXPECT_EQ(device->_extProperties.Vulkan12.pNext, nullptr);
        if (device->_extProperties.conservativeRasterization) EXPECT_EQ(device->_extProperties.conservativeRasterization->pNext, nullptr);
        const auto& caps = device->GetCapabilities();
        EXPECT_FALSE(caps.Detail.IsLayeredRenderingFromVertexShaderSupported);
        EXPECT_EQ(caps.Features.UavWriteStages.HasFlag(ShaderStage::Pixel), device->_enabledFeatures.Core.fragmentStoresAndAtomics != 0);
        EXPECT_EQ(caps.Features.UavWriteStages.HasFlag(ShaderStage::Vertex), device->_enabledFeatures.Core.vertexPipelineStoresAndAtomics != 0);
        EXPECT_EQ(caps.Limits.MaxBufferSize, device->_extProperties.Vulkan11.maxMemoryAllocationSize);
        EXPECT_EQ(caps.Limits.MaxStorageBufferRange, device->_properties.limits.maxStorageBufferRange);
        EXPECT_EQ(caps.Limits.TexelBufferOffsetAlignment, device->_properties.limits.minTexelBufferOffsetAlignment);
        ASSERT_NE(device->_ftb.vkWaitSemaphores, nullptr);
        ASSERT_NE(device->_ftb.vkGetSemaphoreCounterValue, nullptr);
        if (const auto* callbacks = device->GetAllocationCallbacks()) {
            void* allocation = callbacks->pfnAllocation(callbacks->pUserData, 64, 16, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            ASSERT_NE(allocation, nullptr);
            void* resized = callbacks->pfnReallocation(callbacks->pUserData, allocation, 0, 16, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            EXPECT_EQ(resized, nullptr);
            if (resized) callbacks->pfnFree(callbacks->pUserData, resized);
        }
        auto fence = device->CreateFence().Unwrap();
        EXPECT_EQ(fence->GetCompletedValue(), 0u);
        EXPECT_FALSE(fence->WaitFor(1, 0));
        Fence* signals[]{fence.get()};
        uint64_t values[]{1};
        test::GpuWorkGuard gpuWork{Context};
        Context.Queue->Submit({.SignalFences = signals, .SignalValues = values});
        EXPECT_TRUE(fence->WaitFor(1, 10000));
        EXPECT_EQ(fence->GetCompletedValue(), 1u);
        EXPECT_EQ(fence->GetLastSignaledValue(), 1u);
        vector<SamplerDescriptor> samplers(1);
        if (device->_enabledFeatures.Core.samplerAnisotropy) samplers.push_back({.AnisotropyClamp = 2});
        if (device->_enabledFeatures.Vulkan12.samplerMirrorClampToEdge) samplers.push_back({.AddressS = AddressMode::MirrorClampToEdge});
        if (device->_enabledFeatures.Vulkan12.samplerFilterMinmax) samplers.push_back({.Reduction = SamplerReductionMode::Min});
        for (const auto& samplerDesc : samplers) {
            ASSERT_TRUE(device->CreateSampler(samplerDesc));
            BindingDescriptor binding{.BindingId = 1, .Kind = BindingKind::Sampler, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = {}, .Vulkan = VulkanBindingDescriptor{0, 0}, .ImmutableSampler = {}};
            binding.ImmutableSampler = samplerDesc;
            EXPECT_TRUE(device->CreateBindingGroupLayout({std::span{&binding, 1}}));
        }
        auto disabledRequests = GetRhiFeatureRequestsVulkan();
        disabledRequests.SamplerAnisotropy = FeatureRequirementVulkan::Disabled;
        disabledRequests.SamplerMirrorClampToEdge = FeatureRequirementVulkan::Disabled;
        disabledRequests.SamplerFilterMinmax = FeatureRequirementVulkan::Disabled;
        auto disabled = CreateDeviceVulkan({.PhysicalDeviceIndex = adapter.Index, .Queues = queues}, disabledRequests).Unwrap();
        auto validated = render::detail::CreateValidationDevice(disabled, {.IsEnabled = true, .LogCallback = test::CaptureRhiValidationMessage, .LogUserData = &Context}).Unwrap();
        EXPECT_FALSE(validated->GetCapabilities().Features.SamplerFilterMinmax);
        EXPECT_FALSE(validated->QueryTextureSupport({TextureDimension::Dim2D, TextureFormat::R32_FLOAT, TextureUse::Resource}).MinMaxFiltering);
        for (size_t index = 1; index < samplers.size(); ++index) {
            const auto beforeSampler = Context.RhiValidationErrors.load();
            EXPECT_FALSE(validated->CreateSampler(samplers[index]));
            EXPECT_GT(Context.RhiValidationErrors.load(), beforeSampler);
            BindingDescriptor binding{.BindingId = 1, .Kind = BindingKind::Sampler, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = {}, .Vulkan = VulkanBindingDescriptor{0, 0}, .ImmutableSampler = samplers[index]};
            const auto beforeLayout = Context.RhiValidationErrors.load();
            EXPECT_FALSE(validated->CreateBindingGroupLayout({std::span{&binding, 1}}));
            EXPECT_GT(Context.RhiValidationErrors.load(), beforeLayout);
        }
        Context.Queue->Wait();
        RecordProperty("adapter_" + std::to_string(adapter.Index), adapter.Name);
    }
}

INSTANTIATE_TEST_SUITE_P(Adapters, VulkanDeviceFeatureGpu, testing::Values(0u, 1u));
INSTANTIATE_TEST_SUITE_P(Modes, VulkanFramebufferGraphicsGpu, testing::Values(FramebufferModeVulkan::Traditional, FramebufferModeVulkan::Imageless));

}  // namespace
}  // namespace radray::render::vulkan

#endif
