#include "vulkan_device.h"

namespace radray::render::vulkan {

class InstanceVulkan {
public:
    InstanceVulkan() = default;
    InstanceVulkan(const InstanceVulkan&) = delete;
    InstanceVulkan& operator=(const InstanceVulkan&) = delete;
    InstanceVulkan(InstanceVulkan&&) = delete;
    InstanceVulkan& operator=(InstanceVulkan&&) = delete;
    ~InstanceVulkan() {
        if (_instance != nullptr) {
            const VkAllocationCallbacks* allocCbPtr = GetAllocationCallbacks();
            vkDestroyInstance(_instance, allocCbPtr);
            _instance = nullptr;
        }
    }

    const VkAllocationCallbacks* GetAllocationCallbacks() const noexcept {
        return _allocCb.has_value() ? &_allocCb.value() : nullptr;
    }

public:
    VkInstance _instance;
    std::optional<VkAllocationCallbacks> _allocCb;
    vector<string> _exts;
    vector<string> _layers;
};

static Nullable<unique_ptr<InstanceVulkan>> g_instance;

static void DeviceVulkanDestroy(DeviceVulkan& d) noexcept {
    if (d.IsValid()) {
        d.CallVk(&FTbVk::vkDestroyDevice, g_instance->GetAllocationCallbacks());
        d._device = VK_NULL_HANDLE;
        d._physicalDevice = VK_NULL_HANDLE;
    }
}

DeviceVulkan::~DeviceVulkan() noexcept {
    DeviceVulkanDestroy(*this);
}

bool DeviceVulkan::IsValid() const noexcept {
    return _device != VK_NULL_HANDLE && _physicalDevice != VK_NULL_HANDLE;
}

void DeviceVulkan::Destroy() noexcept {
    DeviceVulkanDestroy(*this);
}

Nullable<CommandQueue> DeviceVulkan::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    if (type >= QueueType::MAX_COUNT) {
        RADRAY_ERR_LOG("vk invalid queue type: {}", (uint32_t)type);
        return nullptr;
    }
    if (slot >= _queues[(size_t)type].size()) {
        RADRAY_ERR_LOG("vk invalid queue slot: {}, max {}", slot, _queues[(size_t)type].size());
        return nullptr;
    }
    return _queues[(size_t)type][slot].get();
}

Nullable<shared_ptr<Fence>> DeviceVulkan::CreateFence() noexcept { return nullptr; }

Nullable<shared_ptr<Shader>> DeviceVulkan::CreateShader(
    std::span<const byte> blob,
    ShaderBlobCategory category,
    ShaderStage stage,
    std::string_view entryPoint,
    std::string_view name) noexcept { return nullptr; }

Nullable<shared_ptr<RootSignature>> DeviceVulkan::CreateRootSignature(const RootSignatureDescriptor& info) noexcept { return nullptr; }

Nullable<shared_ptr<GraphicsPipelineState>> DeviceVulkan::CreateGraphicsPipeline(
    const GraphicsPipelineStateDescriptor& desc) noexcept { return nullptr; }

Nullable<shared_ptr<SwapChain>> DeviceVulkan::CreateSwapChain(
    CommandQueue* presentQueue,
    const void* nativeWindow,
    uint32_t width,
    uint32_t height,
    uint32_t backBufferCount,
    TextureFormat format,
    bool enableSync) noexcept { return nullptr; }

Nullable<shared_ptr<Buffer>> DeviceVulkan::CreateBuffer(
    uint64_t size,
    ResourceType type,
    ResourceUsage usage,
    ResourceStates initState,
    ResourceMemoryTips tips,
    std::string_view name) noexcept { return nullptr; }

Nullable<shared_ptr<Texture>> DeviceVulkan::CreateTexture(
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

Nullable<shared_ptr<ResourceView>> DeviceVulkan::CreateBufferView(
    Buffer* buffer,
    ResourceType type,
    TextureFormat format,
    uint64_t offset,
    uint32_t count,
    uint32_t stride) noexcept { return nullptr; }

Nullable<shared_ptr<ResourceView>> DeviceVulkan::CreateTextureView(
    Texture* texture,
    ResourceType type,
    TextureFormat format,
    TextureDimension dim,
    uint32_t baseArrayLayer,
    uint32_t arrayLayerCount,
    uint32_t baseMipLevel,
    uint32_t mipLevelCount) noexcept { return nullptr; }

Nullable<shared_ptr<DescriptorSet>> DeviceVulkan::CreateDescriptorSet(const DescriptorSetElementInfo& info) noexcept { return nullptr; }

Nullable<shared_ptr<Sampler>> DeviceVulkan::CreateSampler(const SamplerDescriptor& desc) noexcept { return nullptr; }

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
        RADRAY_ERR_LOG("vk Validation Layer {}: {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName, pCallbackData->pMessage);
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        RADRAY_WARN_LOG("vk Validation Layer {}: {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName, pCallbackData->pMessage);
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        RADRAY_INFO_LOG("vk Validation Layer {}: {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName, pCallbackData->pMessage);
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        RADRAY_DEBUG_LOG("vk Validation Layer {}: {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName, pCallbackData->pMessage);
    } else if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
        RADRAY_INFO_LOG("vk Validation Layer {}: {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName, pCallbackData->pMessage);
    }
    return VK_FALSE;
}

Nullable<shared_ptr<DeviceVulkan>> CreateDevice(const VulkanDeviceDescriptor& desc) {
    struct PhyDeviceInfo {
        VkPhysicalDevice device;
        VkPhysicalDeviceProperties properties;
        VkPhysicalDeviceMemoryProperties memory;
        size_t index;
    };

    if (!g_instance.HasValue()) {
        RADRAY_ERR_LOG("vk not init");
        return nullptr;
    }
    VkInstance instance = g_instance->_instance;
    vector<VkPhysicalDevice> physicalDevices;
    if (GetVector(physicalDevices, vkEnumeratePhysicalDevices, instance) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumeratePhysicalDevices failed");
        return nullptr;
    }
    if (physicalDevices.size() == 0) {
        RADRAY_ERR_LOG("vk no physical device found");
        return nullptr;
    }
    vector<PhyDeviceInfo> physicalDeviceProps{};
    for (size_t i = 0; i < physicalDevices.size(); ++i) {
        const auto& phyDevice = physicalDevices[i];
        VkPhysicalDeviceProperties deviceProps;
        vkGetPhysicalDeviceProperties(phyDevice, &deviceProps);
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(phyDevice, &memory);
        uint64_t total = GetPhysicalDeviceMemoryAllSize(memory, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        RADRAY_INFO_LOG("vk find device: {}, memory: {}MB", deviceProps.deviceName, total / (1024 * 1024));
        physicalDeviceProps.emplace_back(PhyDeviceInfo{phyDevice, deviceProps, memory, i});
    }
    size_t selectPhysicalDeviceIndex = std::numeric_limits<size_t>::max();
    if (desc.PhysicalDeviceIndex.has_value()) {
        uint32_t index = desc.PhysicalDeviceIndex.value();
        if (index >= physicalDevices.size()) {
            RADRAY_ERR_LOG("vk PhysicalDeviceIndex {} out of range, max {}", index, physicalDevices.size());
            return nullptr;
        }
        selectPhysicalDeviceIndex = index;
    } else {
        auto cpy = physicalDeviceProps;
        std::stable_sort(cpy.begin(), cpy.end(), [](const PhyDeviceInfo& lhs, const PhyDeviceInfo& rhs) noexcept {
            if (lhs.properties.deviceType != rhs.properties.deviceType) {
                static auto typeScore = [](VkPhysicalDeviceType t) noexcept {
                    switch (t) {
                        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
                        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
                        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
                        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
                        default: return 0;
                    }
                };
                int lscore = typeScore(lhs.properties.deviceType);
                int rscore = typeScore(rhs.properties.deviceType);
                if (lscore != rscore) {
                    return lscore > rscore;
                }
            }
            if (lhs.properties.apiVersion != rhs.properties.apiVersion) {
                return lhs.properties.apiVersion > rhs.properties.apiVersion;
            }
            auto l = GetPhysicalDeviceMemoryAllSize(lhs.memory, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
            auto r = GetPhysicalDeviceMemoryAllSize(rhs.memory, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
            return l > r;
        });
        selectPhysicalDeviceIndex = cpy[0].index;
    }

    const auto& selectPhyDevice = physicalDeviceProps[selectPhysicalDeviceIndex];
    RADRAY_INFO_LOG("vk select device: {}", selectPhyDevice.properties.deviceName);

    struct QueueRequest {
        QueueType rawType;
        VkQueueFlags requiredFlag;
        uint32_t requiredCount;
        vector<QueueIndexInFamily> queueIndices;
    };
    vector<VkDeviceQueueCreateInfo> queueInfos;
    vector<float> queuePriorities;
    vector<QueueRequest> queueRequests;
    {
        struct QueueFamilyUsage {
            uint32_t familyIndex;
            VkQueueFlags flags;
            uint32_t maxCount;
            uint32_t usedCount;
        };
        vector<VkQueueFamilyProperties> queueFamilyProps;
        GetVector(queueFamilyProps, vkGetPhysicalDeviceQueueFamilyProperties, selectPhyDevice.device);
        if (queueFamilyProps.empty()) {
            RADRAY_ERR_LOG("vk no queue family found");
            return nullptr;
        }
        vector<QueueFamilyUsage> families;
        for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilyProps.size()); ++i) {
            families.push_back({i, queueFamilyProps[i].queueFlags, queueFamilyProps[i].queueCount, 0});
        }
        for (const auto& i : desc.Queues) {
            auto& r = queueRequests.emplace_back();
            r.rawType = i.Type;
            r.requiredFlag = MapType(i.Type);
            if (r.requiredFlag) {
                r.requiredCount = i.Count;
            } else {
                r.requiredCount = 0;
                RADRAY_WARN_LOG("vk queue type {} not supported", FormatVkQueueFlags(r.requiredFlag));
            }
        }
        for (auto& req : queueRequests) {
            for (auto& fam : families) {
                while ((fam.flags & req.requiredFlag) && fam.usedCount < fam.maxCount && req.queueIndices.size() < req.requiredCount) {
                    req.queueIndices.emplace_back(QueueIndexInFamily{fam.familyIndex, fam.usedCount});
                    fam.usedCount++;
                    break;
                }
            }
        }
        for (const auto& i : queueRequests) {
            if (i.queueIndices.size() < i.requiredCount) {
                RADRAY_ERR_LOG("vk not enough queue family for type {}, need {}, got {}", FormatVkQueueFlags(i.requiredFlag), i.requiredCount, i.queueIndices.size());
                return nullptr;
            }
        }
        unordered_map<uint32_t, uint32_t> familyQueueCounts;
        for (const auto& i : queueRequests) {
            for (const auto& j : i.queueIndices) {
                if (familyQueueCounts.contains(j.Family)) {
                    familyQueueCounts[j.Family]++;
                } else {
                    familyQueueCounts[j.Family] = 1;
                }
            }
        }
        for (const auto& [family, count] : familyQueueCounts) {
            queuePriorities.resize(queuePriorities.size() + count, 1.0f);
            VkDeviceQueueCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            info.pNext = nullptr;
            info.flags = 0;
            info.queueFamilyIndex = family;
            info.queueCount = count;
            info.pQueuePriorities = queuePriorities.data() + (queuePriorities.size() - count);
            queueInfos.push_back(info);
        }
    }

    vector<const char*> deviceExts;
    deviceExts.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = nullptr;
    deviceInfo.flags = 0;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledLayerCount = 0;
    deviceInfo.ppEnabledLayerNames = nullptr;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
    deviceInfo.ppEnabledExtensionNames = deviceExts.data();
    deviceInfo.pEnabledFeatures = nullptr;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(selectPhyDevice.device, &deviceInfo, g_instance->GetAllocationCallbacks(), &device) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk create device fail");
        return nullptr;
    }
    RADRAY_INFO_LOG("========== Feature ==========");
    auto deviceR = make_shared<DeviceVulkan>();
    deviceR->_physicalDevice = selectPhyDevice.device;
    deviceR->_device = device;
    volkLoadDeviceTable(&deviceR->_vtb, device);
    for (const auto& i : queueRequests) {
        for (const auto& j : i.queueIndices) {
            VkQueue queuePtr = VK_NULL_HANDLE;
            deviceR->CallVk(&FTbVk::vkGetDeviceQueue, j.Family, j.IndexInFamily, &queuePtr);
            if (queuePtr == VK_NULL_HANDLE) {
                RADRAY_ERR_LOG("vk get queue for family {} index {} failed", j.Family, j.IndexInFamily);
                return nullptr;
            }
            auto queue = make_unique<QueueVulkan>(deviceR.get(), queuePtr, j);
            deviceR->_queues[(size_t)i.rawType].emplace_back(std::move(queue));
        }
    }
    {
        const auto& props = selectPhyDevice.properties;
        uint32_t driverVersion = props.driverVersion;
        uint32_t vendorID = props.vendorID;
        string verStr;
        // https://pcisig.com/membership/member-companies
        switch (vendorID) {
            case 0x10DE: {  // NVIDIA
                uint32_t major = (driverVersion >> 22) & 0x3ff;
                uint32_t minor = (driverVersion >> 14) & 0x0ff;
                uint32_t patch = (driverVersion >> 6) & 0x0ff;
                uint32_t build = driverVersion & 0x3f;
                verStr = radray::format("{}.{}.{}.{}", major, minor, patch, build);
                break;
            }
            case 0x8086:  // Intel
            case 0x1002:  // AMD
            default: {
                uint32_t variant = VK_API_VERSION_VARIANT(driverVersion);
                uint32_t major = VK_API_VERSION_MAJOR(driverVersion);
                uint32_t minor = VK_API_VERSION_MINOR(driverVersion);
                uint32_t patch = VK_API_VERSION_PATCH(driverVersion);
                verStr = radray::format("{}.{}.{}.{}", variant, major, minor, patch);
                break;
            }
        }
        RADRAY_INFO_LOG("Driver Version: {}", verStr);
    }
    {
        RADRAY_INFO_LOG("Physical Device Type: {}", selectPhyDevice.properties.deviceType);
    }
    RADRAY_INFO_LOG("=============================");
    return deviceR;
}

bool GlobalInitVulkan(std::span<BackendInitDescriptor> _desc) {
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
    vector<VkExtensionProperties> extProps;
    if (GetVector(extProps, vkEnumerateInstanceExtensionProperties, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceExtensionProperties failed");
        return false;
    }
    vector<VkLayerProperties> layerProps;
    if (GetVector(layerProps, vkEnumerateInstanceLayerProperties) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceLayerProperties failed");
        return false;
    }

    auto findLayer = [&](std::string_view name) noexcept {
        return std::find_if(layerProps.begin(), layerProps.end(), [&](const VkLayerProperties& i) noexcept { return i.layerName == name; });
    };
    auto findExt = [&](std::string_view name) noexcept {
        return std::find_if(extProps.begin(), extProps.end(), [&](const VkExtensionProperties& i) noexcept { return i.extensionName == name; });
    };

    unordered_set<string> needExts;
    unordered_set<string> needLayers;
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
        if (findExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == extProps.end()) {
            RADRAY_WARN_LOG("vk extension {} is not supported", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            needExts.emplace(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        const auto validName = "VK_LAYER_KHRONOS_validation";
        if (findLayer(validName) == layerProps.end()) {
            RADRAY_WARN_LOG("vk layer {} is not supported", validName);
        } else {
            needLayers.emplace(validName);
            vector<VkExtensionProperties> dbgExtProps;
            GetVector(dbgExtProps, vkEnumerateInstanceExtensionProperties, validName);
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

    vector<const char*> needExtsCStr;
    vector<const char*> needLayersCStr;
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

    vector<VkValidationFeatureEnableEXT> validEnables{};
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
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&createInfo, allocCbPtr, &instance) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateInstance failed");
        return false;
    }
    volkLoadInstance(instance);
    g_instance = make_unique<InstanceVulkan>();
    g_instance->_instance = instance;
    g_instance->_allocCb = allocCbPtr ? std::make_optional(*allocCbPtr) : std::nullopt;
    g_instance->_exts = {needExts.begin(), needExts.end()};
    g_instance->_layers = {needLayers.begin(), needLayers.end()};
    return true;
}

void GlobalTerminateVulkan() {
    g_instance = nullptr;
    volkFinalize();
}

}  // namespace radray::render::vulkan
