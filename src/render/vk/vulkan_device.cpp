#include "vulkan_device.h"

namespace radray::render::vulkan {

bool DeviceVulkan::IsValid() const noexcept { return false; }

void DeviceVulkan::Destroy() noexcept {}

Nullable<CommandQueue> DeviceVulkan::GetCommandQueue(QueueType type, uint32_t slot) noexcept { return nullptr; }

Nullable<radray::shared_ptr<Fence>> DeviceVulkan::CreateFence() noexcept { return nullptr; }

Nullable<radray::shared_ptr<Shader>> DeviceVulkan::CreateShader(
    std::span<const byte> blob,
    ShaderBlobCategory category,
    ShaderStage stage,
    std::string_view entryPoint,
    std::string_view name) noexcept { return nullptr; }

Nullable<radray::shared_ptr<RootSignature>> DeviceVulkan::CreateRootSignature(const RootSignatureDescriptor& info) noexcept { return nullptr; }

Nullable<radray::shared_ptr<GraphicsPipelineState>> DeviceVulkan::CreateGraphicsPipeline(
    const GraphicsPipelineStateDescriptor& desc) noexcept { return nullptr; }

Nullable<radray::shared_ptr<SwapChain>> DeviceVulkan::CreateSwapChain(
    CommandQueue* presentQueue,
    const void* nativeWindow,
    uint32_t width,
    uint32_t height,
    uint32_t backBufferCount,
    TextureFormat format,
    bool enableSync) noexcept { return nullptr; }

Nullable<radray::shared_ptr<Buffer>> DeviceVulkan::CreateBuffer(
    uint64_t size,
    ResourceType type,
    ResourceUsage usage,
    ResourceStates initState,
    ResourceMemoryTips tips,
    std::string_view name) noexcept { return nullptr; }

Nullable<radray::shared_ptr<Texture>> DeviceVulkan::CreateTexture(
    uint64_t width,
    uint64_t height,
    uint64_t depth,
    uint32_t arraySize,
    TextureFormat format,
    uint32_t mipLevels,
    uint32_t sampleCount,
    uint32_t sampleQuality,
    ClearValue clearValue,
    ResourceType type,
    ResourceStates initState,
    ResourceMemoryTips tips,
    std::string_view) noexcept { return nullptr; }

Nullable<radray::shared_ptr<ResourceView>> DeviceVulkan::CreateBufferView(
    Buffer* buffer,
    ResourceType type,
    TextureFormat format,
    uint64_t offset,
    uint32_t count,
    uint32_t stride) noexcept { return nullptr; }

Nullable<radray::shared_ptr<ResourceView>> DeviceVulkan::CreateTextureView(
    Texture* texture,
    ResourceType type,
    TextureFormat format,
    TextureDimension dim,
    uint32_t baseArrayLayer,
    uint32_t arrayLayerCount,
    uint32_t baseMipLevel,
    uint32_t mipLevelCount) noexcept { return nullptr; }

Nullable<radray::shared_ptr<DescriptorSet>> DeviceVulkan::CreateDescriptorSet(const DescriptorSetElementInfo& info) noexcept { return nullptr; }

Nullable<radray::shared_ptr<Sampler>> DeviceVulkan::CreateSampler(const SamplerDescriptor& desc) noexcept { return nullptr; }

uint64_t DeviceVulkan::GetUploadBufferNeedSize(Resource* copyDst, uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) const noexcept { return 0; }

void DeviceVulkan::CopyDataToUploadBuffer(
    Resource* dst,
    const void* src,
    size_t srcSize,
    uint32_t mipLevel,
    uint32_t arrayLayer,
    uint32_t layerCount) const noexcept {}

Nullable<DeviceVulkan> CreateDevice(const VulkanDeviceDescriptor& desc) {
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceVersion failed");
        return nullptr;
    }
    uint32_t extCount = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceExtensionProperties failed");
        return nullptr;
    }
    radray::vector<VkExtensionProperties> extProps{extCount};
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, extProps.data());
    uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceLayerProperties failed");
        return nullptr;
    }
    radray::vector<VkLayerProperties> layerProps{layerCount};
    vkEnumerateInstanceLayerProperties(&layerCount, layerProps.data());

    auto findLayer = [&](std::string_view name) noexcept {
        return std::find_if(layerProps.begin(), layerProps.end(), [&](const VkLayerProperties& i) noexcept { return i.layerName == name; });
    };
    auto findExt = [&](std::string_view name) noexcept {
        return std::find_if(extProps.begin(), extProps.end(), [&](const VkExtensionProperties& i) noexcept { return i.extensionName == name; });
    };

    radray::vector<radray::string> needExts;
    radray::vector<radray::string> needLayers;
    needExts.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    needExts.emplace_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    needExts.emplace_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    needExts.emplace_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
    if (desc.IsEnableDebugLayer) {
        const auto validName = "VK_LAYER_KHRONOS_validation";
        auto layerIt = findLayer(validName);
        if (layerIt == layerProps.end()) {
            RADRAY_WARN_LOG("vk layer {} is not supported", validName);
        } else {
            needLayers.emplace_back(validName);
        }
    }
    for (const auto& i : needLayers) {
        auto layerIt = findLayer(i);
        if (layerIt == layerProps.end()) {
            RADRAY_ERR_LOG("vk layer {} is not supported", i);
            return nullptr;
        }
    }
    for (const auto& i : needExts) {
        auto extIt = findExt(i);
        if (extIt == extProps.end()) {
            RADRAY_ERR_LOG("vk extension {} is not supported", i);
            return nullptr;
        }
    }

    radray::vector<const char*> needExtsCStr;
    radray::vector<const char*> needLayersCStr;
    needExtsCStr.reserve(needExts.size());
    needLayersCStr.reserve(needLayers.size());
    for (const auto& i : needExts) {
        needExtsCStr.emplace_back(i.c_str());
    }
    for (const auto& i : needLayers) {
        needLayersCStr.emplace_back(i.c_str());
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "RadRay";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "null";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    if (version < VK_API_VERSION_1_1) {
        appInfo.apiVersion = VK_API_VERSION_1_0;
    } else {
        appInfo.apiVersion = VK_API_VERSION_1_3;
    }
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(needExtsCStr.size());
    createInfo.ppEnabledExtensionNames = needExtsCStr.empty() ? nullptr : needExtsCStr.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(needLayersCStr.size());
    createInfo.ppEnabledLayerNames = needLayersCStr.empty() ? nullptr : needLayersCStr.data();
    VkInstance instance;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateInstance failed");
        return nullptr;
    }
    return nullptr;
}

void GlobalInit() {
    if (volkInitialize() != VK_SUCCESS) {
        RADRAY_ABORT("vk volk init fail");
        return;
    }
}

void GlobalTerminate() {
    volkFinalize();
}

}  // namespace radray::render::vulkan
