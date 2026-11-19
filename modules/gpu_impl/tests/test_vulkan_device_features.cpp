#include <gtest/gtest.h>

#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/gpu_impl/backend/vulkan_helper.h>

namespace radray::gpu::vulkan {
namespace {

// Every feature in the RHI request table, plus features the RHI never requests.
DeviceFeaturesVulkan SupportEverything() {
    DeviceFeaturesVulkan features;
    auto& core = features.Core;
    core.fullDrawIndexUint32 = core.imageCubeArray = core.depthClamp = core.depthBiasClamp = VK_TRUE;
    core.fillModeNonSolid = core.independentBlend = core.dualSrcBlend = VK_TRUE;
    core.multiDrawIndirect = core.drawIndirectFirstInstance = core.samplerAnisotropy = VK_TRUE;
    core.vertexPipelineStoresAndAtomics = core.fragmentStoresAndAtomics = core.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    core.robustBufferAccess = core.shaderStorageImageReadWithoutFormat = VK_TRUE;
    features.Vulkan11.multiview = VK_TRUE;
    auto& v12 = features.Vulkan12;
    v12.timelineSemaphore = v12.samplerMirrorClampToEdge = v12.samplerFilterMinmax = VK_TRUE;
    v12.imagelessFramebuffer = v12.separateDepthStencilLayouts = VK_TRUE;
    v12.shaderOutputLayer = v12.descriptorIndexing = v12.bufferDeviceAddress = VK_TRUE;
    return features;
}

TEST(VulkanColorWriteMask, PreservesAllSixteenChannelSubsets) {
    auto target = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    const GraphicsPipelineStateDescriptor desc{.ColorTargets = {&target, 1}};
    const auto features = SupportEverything();
    for (uint32_t bits = 0; bits <= uint32_t(ColorWrite::All); ++bits) {
        target.WriteMask = static_cast<ColorWrite>(bits);
        EXPECT_EQ(MapType(target.WriteMask), bits);
        EXPECT_FALSE(GetUnsupportedGraphicsStateVulkan(desc, features, false));
    }
    target.WriteMask = static_cast<ColorWrite>(16);
    EXPECT_TRUE(GetUnsupportedGraphicsStateVulkan(desc, features, false));
}

TEST(VulkanDeviceFeaturePolicy, EnablesRequestedFeaturesOnly) {
    const auto selection = SelectDeviceFeaturesVulkan(SupportEverything(), GetRhiFeatureRequestsVulkan());

    EXPECT_TRUE(selection.MissingRequired.empty());
    EXPECT_TRUE(selection.Enabled.Vulkan12.timelineSemaphore);
    EXPECT_TRUE(selection.Enabled.Core.samplerAnisotropy);
    EXPECT_TRUE(selection.Enabled.Vulkan12.imagelessFramebuffer);
    EXPECT_FALSE(selection.Enabled.Core.robustBufferAccess);
    EXPECT_FALSE(selection.Enabled.Core.shaderStorageImageReadWithoutFormat);
    EXPECT_FALSE(selection.Enabled.Vulkan11.multiview);
    EXPECT_FALSE(selection.Enabled.Vulkan12.shaderOutputLayer);
    EXPECT_FALSE(selection.Enabled.Vulkan12.descriptorIndexing);
    EXPECT_FALSE(selection.Enabled.Vulkan12.bufferDeviceAddress);
}

TEST(VulkanDeviceFeaturePolicy, MissingOptionalFeatureStaysDisabled) {
    auto supported = SupportEverything();
    supported.Core.samplerAnisotropy = VK_FALSE;

    const auto selection = SelectDeviceFeaturesVulkan(supported, GetRhiFeatureRequestsVulkan());

    EXPECT_TRUE(selection.MissingRequired.empty());
    EXPECT_FALSE(selection.Enabled.Core.samplerAnisotropy);
}

TEST(VulkanDeviceFeaturePolicy, MissingTimelineSemaphoreIsReportedAsRequired) {
    auto supported = SupportEverything();
    supported.Vulkan12.timelineSemaphore = VK_FALSE;

    const auto selection = SelectDeviceFeaturesVulkan(supported, GetRhiFeatureRequestsVulkan());

    EXPECT_EQ(selection.MissingRequired, vector<std::string_view>{"timelineSemaphore"});
}

TEST(VulkanDeviceFeaturePolicy, DisabledRequestIsNotEnabledWhenSupported) {
    auto requests = GetRhiFeatureRequestsVulkan();
    requests.ImagelessFramebuffer = FeatureRequirementVulkan::Disabled;

    const auto selection = SelectDeviceFeaturesVulkan(SupportEverything(), requests);

    EXPECT_FALSE(selection.Enabled.Vulkan12.imagelessFramebuffer);
}

}  // namespace
}  // namespace radray::gpu::vulkan
#endif
