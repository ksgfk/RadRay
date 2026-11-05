#include <gtest/gtest.h>

#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_helper.h>

namespace radray::render::vulkan {
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
}  // namespace radray::render::vulkan
#endif
