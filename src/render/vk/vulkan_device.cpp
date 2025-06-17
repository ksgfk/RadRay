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

static VKAPI_ATTR VkBool32 VKAPI_CALL VKDebugUtilsMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) noexcept {
    RADRAY_UNUSED(pUserData);
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        RADRAY_ERR_LOG("vk {}: {}", formatVkDebugMsgType(messageType), pCallbackData->pMessage);
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        RADRAY_WARN_LOG("vk {}: {}", formatVkDebugMsgType(messageType), pCallbackData->pMessage);
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        RADRAY_INFO_LOG("vk {}: {}", formatVkDebugMsgType(messageType), pCallbackData->pMessage);
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        RADRAY_DEBUG_LOG("vk {}: {}", formatVkDebugMsgType(messageType), pCallbackData->pMessage);
    }
    return VK_FALSE;
}

class InstanceVulkan {
public:
    InstanceVulkan() = default;
    InstanceVulkan(const InstanceVulkan&) = delete;
    InstanceVulkan& operator=(const InstanceVulkan&) = delete;
    InstanceVulkan(InstanceVulkan&&) = delete;
    InstanceVulkan& operator=(InstanceVulkan&&) = delete;
    ~InstanceVulkan() {
        if (_instance != nullptr) {
            VkAllocationCallbacks* allocCbPtr = _allocCb.has_value() ? &_allocCb.value() : nullptr;
            vkDestroyInstance(_instance, allocCbPtr);
            _instance = nullptr;
        }
    }

public:
    VkInstance _instance;
    std::optional<VkAllocationCallbacks> _allocCb;
    radray::vector<radray::string> _exts;
    radray::vector<radray::string> _layers;
};

static Nullable<std::unique_ptr<InstanceVulkan>> g_instance;

Nullable<DeviceVulkan> CreateDevice(const VulkanDeviceDescriptor& desc) {
    if (!g_instance.HasValue()) {
        RADRAY_ERR_LOG("vk not init");
        return nullptr;
    }
    VkInstance instance = g_instance->_instance;
    uint32_t physicalDeviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumeratePhysicalDevices failed");
        return nullptr;
    }
    radray::vector<VkPhysicalDevice> physicalDevices{physicalDeviceCount};
    vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data());
    return nullptr;
}

bool GlobalInit(std::span<BackendInitDescriptor> _desc) {
    if (g_instance.HasValue()) {
        RADRAY_WARN_LOG("vk already init");
        return true;
    }
    if (volkInitialize() != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk volk init fail");
        return false;
    }
    VulkanBackendInitDdescriptor* descPtr = nullptr;
    for (auto&& d : _desc) {
        if (std::holds_alternative<VulkanBackendInitDdescriptor>(d)) {
            descPtr = &std::get<VulkanBackendInitDdescriptor>(d);
            break;
        }
    }
    VulkanBackendInitDdescriptor desc;
    if (descPtr == nullptr) {
        desc = {
#ifdef RADRAY_IS_DEBUG
            true,
            true,
#else
            false,
            false,
#endif
        };
    } else {
        desc = *descPtr;
    }

    uint32_t version = 0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceVersion failed");
        return false;
    }
    uint32_t extCount = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceExtensionProperties failed");
        return false;
    }
    radray::vector<VkExtensionProperties> extProps{extCount};
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, extProps.data());
    uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceLayerProperties failed");
        return false;
    }
    radray::vector<VkLayerProperties> layerProps{layerCount};
    vkEnumerateInstanceLayerProperties(&layerCount, layerProps.data());

    auto findLayer = [&](std::string_view name) noexcept {
        return std::find_if(layerProps.begin(), layerProps.end(), [&](const VkLayerProperties& i) noexcept { return i.layerName == name; });
    };
    auto findExt = [&](std::string_view name) noexcept {
        return std::find_if(extProps.begin(), extProps.end(), [&](const VkExtensionProperties& i) noexcept { return i.extensionName == name; });
    };

    radray::unordered_set<radray::string> needExts;
    radray::unordered_set<radray::string> needLayers;
    bool isValidFeatureExtEnable = false;
    needExts.emplace(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    needExts.emplace(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    needExts.emplace(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    needExts.emplace(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
    if (desc.IsEnableDebugLayer) {
        const auto validName = "VK_LAYER_KHRONOS_validation";
        auto layerIt = findLayer(validName);
        if (layerIt == layerProps.end()) {
            RADRAY_WARN_LOG("vk layer {} is not supported", validName);
        } else {
            needLayers.emplace(validName);
            needExts.emplace(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            uint32_t dbgExtCount = 0;
            vkEnumerateInstanceExtensionProperties(validName, &dbgExtCount, nullptr);
            radray::vector<VkExtensionProperties> dbgExtProps{dbgExtCount};
            vkEnumerateInstanceExtensionProperties(validName, &dbgExtCount, dbgExtProps.data());
            for (const auto& i : dbgExtProps) {
                if (std::strcmp(i.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0) {
                    isValidFeatureExtEnable = true;
                    break;
                }
            }
        }
    }
    for (const auto& i : needLayers) {
        auto layerIt = findLayer(i);
        if (layerIt == layerProps.end()) {
            RADRAY_ERR_LOG("vk layer {} is not supported", i);
            return false;
        }
    }
    for (const auto& i : needExts) {
        auto extIt = findExt(i);
        if (extIt == extProps.end()) {
            RADRAY_ERR_LOG("vk extension {} is not supported", i);
            return false;
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

    VkAllocationCallbacks* allocCbPtr = nullptr;
#ifdef RADRAY_ENABLE_MIMALLOC
    VkAllocationCallbacks allocCb{};
    allocCb.pUserData = nullptr;
    allocCb.pfnAllocation = [](void* pUserData, size_t size, size_t alignment, VkSystemAllocationScope) noexcept {
        RADRAY_UNUSED(pUserData);
        return mi_aligned_alloc(alignment, size);
    };
    allocCb.pfnReallocation = [](void* pUserData, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope) noexcept {
        RADRAY_UNUSED(pUserData);
        return mi_realloc_aligned(pOriginal, size, alignment);
    };
    allocCb.pfnFree = [](void* pUserData, void* pMemory) noexcept {
        RADRAY_UNUSED(pUserData);
        mi_free(pMemory);
    };
    allocCb.pfnInternalAllocation = nullptr;
    allocCb.pfnInternalFree = nullptr;
    allocCbPtr = &allocCb;
#endif

    radray::vector<VkValidationFeatureEnableEXT> validEnables{};
    VkValidationFeaturesEXT validFeature{};
    validFeature.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validFeature.pNext = nullptr;
    if (isValidFeatureExtEnable) {
        validEnables.emplace_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        if (desc.IsEnableGpuBasedValid) {
            validEnables.emplace_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
            validEnables.emplace_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
        }
    }
    validFeature.enabledValidationFeatureCount = static_cast<uint32_t>(validEnables.size());
    validFeature.pEnabledValidationFeatures = validEnables.data();
    validFeature.disabledValidationFeatureCount = 0;
    validFeature.pDisabledValidationFeatures = nullptr;

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugCreateInfo.pNext = nullptr;
    debugCreateInfo.flags = 0;
    debugCreateInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugCreateInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugCreateInfo.pfnUserCallback = VKDebugUtilsMessengerCallback;
    debugCreateInfo.pUserData = nullptr;

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
    createInfo.pNext = nullptr;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(needExtsCStr.size());
    createInfo.ppEnabledExtensionNames = needExtsCStr.empty() ? nullptr : needExtsCStr.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(needLayersCStr.size());
    createInfo.ppEnabledLayerNames = needLayersCStr.empty() ? nullptr : needLayersCStr.data();
    if (isValidFeatureExtEnable) {
        SetVkStructPtrToLast(&createInfo, &validFeature);
        SetVkStructPtrToLast(&createInfo, &debugCreateInfo);
    }
    VkInstance instance = nullptr;
    if (vkCreateInstance(&createInfo, allocCbPtr, &instance) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateInstance failed");
        return false;
    }
    volkLoadInstance(instance);
    g_instance = std::make_unique<InstanceVulkan>();
    g_instance->_instance = instance;
    g_instance->_allocCb = allocCbPtr ? std::make_optional(*allocCbPtr) : std::nullopt;
    g_instance->_exts = {needExts.begin(), needExts.end()};
    g_instance->_layers = {needLayers.begin(), needLayers.end()};
    return true;
}

void GlobalTerminate() {
    g_instance = nullptr;
    volkFinalize();
}

}  // namespace radray::render::vulkan
