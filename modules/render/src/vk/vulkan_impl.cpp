#include <radray/render/backend/vulkan_impl.h>

#if RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

#include <algorithm>
#include <bit>
#include <cstring>
#include <radray/scope_guard.h>
#include <type_traits>

#if defined(_WIN32)
#include <excpt.h>
#endif

#if defined(VK_USE_PLATFORM_METAL_EXT)
namespace radray {
extern VkSurfaceKHR CreateMacOSMetalSurface(VkInstance instance, void* nativeHandler, const VkAllocationCallbacks* allocator) noexcept;
}  // namespace radray
#endif

namespace radray::render::vulkan {

static bool _BindBindingGroupVulkan(
    DeviceVulkan* device,
    CommandBufferVulkan* cmdBuffer,
    PipelineLayoutVulkan*& boundPipeLayout,
    uint32_t groupIndex,
    BindingGroup* group,
    std::span<const uint32_t> dynamicOffsets,
    VkPipelineBindPoint bindPoint) noexcept;

static Nullable<unique_ptr<InstanceVulkanImpl>> g_vkInstance = nullptr;

static VkSwapchainKHR _CreateVkSwapChain(
    DeviceVulkan* device,
    SurfaceVulkan* surface,
    SwapChainDescriptor& desc,
    VkSwapchainKHR oldSwapchain,
    VkExtent2D& swapchainSize,
    VkFormat& rawFormat) noexcept {
    VkSurfaceCapabilitiesKHR surfaceProperties;
    if (auto vr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->_physicalDevice, surface->_surface, &surfaceProperties);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: {}", vr);
        return VK_NULL_HANDLE;
    }
    if (desc.BackBufferCount < surfaceProperties.minImageCount || desc.BackBufferCount > surfaceProperties.maxImageCount) {
        auto newValue = radray::Clamp(desc.BackBufferCount, surfaceProperties.minImageCount, surfaceProperties.maxImageCount);
        RADRAY_WARN_LOG(
            "vk back buffer count {} not in range [{}, {}]. auto clamp to {}",
            desc.BackBufferCount,
            surfaceProperties.minImageCount,
            surfaceProperties.maxImageCount,
            newValue);
        desc.BackBufferCount = newValue;
    }
    if (surfaceProperties.currentExtent.width == 0xFFFFFFFF) {
        swapchainSize.width = desc.Width;
        swapchainSize.height = desc.Height;
    } else {
        swapchainSize = surfaceProperties.currentExtent;
    }
    VkSurfaceTransformFlagBitsKHR preTransform;
    if (surfaceProperties.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
        preTransform = surfaceProperties.currentTransform;
    }
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (surfaceProperties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (surfaceProperties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        composite = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else if (surfaceProperties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
        composite = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    } else if (surfaceProperties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
        composite = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }
    rawFormat = MapType(desc.Format);
    vector<VkSurfaceFormatKHR> supportedFormats;
    if (auto vr = EnumerateVectorFromVkFunc(supportedFormats, vkGetPhysicalDeviceSurfaceFormatsKHR, device->_physicalDevice, surface->_surface);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}", vr);
        return VK_NULL_HANDLE;
    }
    auto needFormatIter = std::ranges::find_if(supportedFormats, [rawFormat](VkSurfaceFormatKHR i) { return i.format == rawFormat; });
    if (needFormatIter == supportedFormats.end()) {
        RADRAY_ERR_LOG("vk surface format not supported", rawFormat);
        return VK_NULL_HANDLE;
    }
    const VkSurfaceFormatKHR& needFormat = *needFormatIter;
    vector<VkPresentModeKHR> supportedPresentModes;
    if (auto vr = EnumerateVectorFromVkFunc(supportedPresentModes, vkGetPhysicalDeviceSurfacePresentModesKHR, device->_physicalDevice, surface->_surface);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkGetPhysicalDeviceSurfacePresentModesKHR failed: {}", vr);
        return VK_NULL_HANDLE;
    }
    VkPresentModeKHR needPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    {
        const VkPresentModeKHR tryList[] = {
            VK_PRESENT_MODE_MAILBOX_KHR,
            VK_PRESENT_MODE_FIFO_RELAXED_KHR,
            VK_PRESENT_MODE_FIFO_KHR};
        vector<VkPresentModeKHR> lookupPresentModes;
        lookupPresentModes.reserve(16);
        lookupPresentModes.push_back(MapType(desc.PresentMode));
        lookupPresentModes.insert(lookupPresentModes.end(), std::begin(tryList), std::end(tryList));
        for (const auto& i : lookupPresentModes) {
            if (std::ranges::find(supportedPresentModes, i) != supportedPresentModes.end()) {
                needPresentMode = i;
                break;
            }
        }
    }
    VkSwapchainCreateInfoKHR swapchianCreateInfo{};
    swapchianCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchianCreateInfo.pNext = nullptr;
    swapchianCreateInfo.flags = 0;
    swapchianCreateInfo.surface = surface->_surface;
    swapchianCreateInfo.minImageCount = desc.BackBufferCount;
    swapchianCreateInfo.imageFormat = needFormat.format;
    swapchianCreateInfo.imageColorSpace = needFormat.colorSpace;
    swapchianCreateInfo.imageExtent = swapchainSize;
    swapchianCreateInfo.imageArrayLayers = 1;
    swapchianCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (surfaceProperties.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        swapchianCreateInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    swapchianCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchianCreateInfo.queueFamilyIndexCount = 0;
    swapchianCreateInfo.pQueueFamilyIndices = nullptr;
    swapchianCreateInfo.preTransform = preTransform;
    swapchianCreateInfo.compositeAlpha = composite;
    swapchianCreateInfo.presentMode = needPresentMode;
    swapchianCreateInfo.clipped = VK_TRUE;
    swapchianCreateInfo.oldSwapchain = oldSwapchain;
    VkSwapchainKHR swapchain{};
    if (auto vr = device->_ftb.vkCreateSwapchainKHR(device->_device, &swapchianCreateInfo, device->GetAllocationCallbacks(), &swapchain);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateSwapchainKHR failed: {}", vr);
        return VK_NULL_HANDLE;
    }
    return swapchain;
}

static bool _RefreshSwapChainImages(SwapChainVulkan* swapChain, VkExtent2D swapchainSize, VkFormat rawFormat, TextureFormat format) noexcept {
    vector<VkImage> swapchainImages;
    if (auto vr = EnumerateVectorFromVkFunc(swapchainImages, swapChain->_device->_ftb.vkGetSwapchainImagesKHR, swapChain->_device->_device, swapChain->_swapchain);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkGetSwapchainImagesKHR failed: {}", vr);
        return false;
    }
    vector<SwapChainVulkan::Frame> frames;
    frames.reserve(swapchainImages.size());
    for (VkImage img : swapchainImages) {
        SwapChainVulkan::Frame& f = frames.emplace_back();
        f.image = make_unique<ImageVulkan>(swapChain->_device, img, VK_NULL_HANDLE, VmaAllocationInfo{});
        auto readyToPresentOpt = swapChain->AcquireSyncObjectFromPool();
        if (!readyToPresentOpt.HasValue()) {
            RADRAY_ERR_LOG("vk swapchain creation failed: cannot allocate present semaphore");
            return false;
        }
        f.readyToPresent = readyToPresentOpt.Release();
        auto name = fmt::format("SwapChain_Image_{}", frames.size() - 1);
        f.image->_name = name;
        f.image->_rawFormat = rawFormat;
        f.image->_dim = TextureDimension::Dim2D;
        f.image->_width = swapchainSize.width;
        f.image->_height = swapchainSize.height;
        f.image->_depthOrArraySize = 1;
        f.image->_mipLevels = 1;
        f.image->_sampleCount = 1;
        f.image->_format = format;
        f.image->_memory = MemoryType::Device;
        f.image->_usage = TextureUse::RenderTarget | TextureUse::Resource | TextureUse::CopySource;
        f.image->_hints = ResourceHint::External;
        f.image->_isSwapchainImage = true;
    }
    swapChain->_frames = std::move(frames);
    return true;
}

static bool _TryCreateVkInstance(
    const VkInstanceCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance,
    VkResult& result,
    uint32_t& exceptionCode) noexcept {
#if defined(_WIN32)
    exceptionCode = 0;
    __try {
        result = vkCreateInstance(createInfo, allocator, instance);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exceptionCode = static_cast<uint32_t>(GetExceptionCode());
        result = VK_ERROR_UNKNOWN;
        if (instance != nullptr) {
            *instance = VK_NULL_HANDLE;
        }
        return false;
    }
#else
    exceptionCode = 0;
    result = vkCreateInstance(createInfo, allocator, instance);
    return true;
#endif
}

static bool _TryCreateVkDebugUtilsMessenger(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDebugUtilsMessengerEXT* messenger,
    VkResult& result,
    uint32_t& exceptionCode) noexcept {
#if defined(_WIN32)
    exceptionCode = 0;
    __try {
        result = vkCreateDebugUtilsMessengerEXT(instance, createInfo, allocator, messenger);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exceptionCode = static_cast<uint32_t>(GetExceptionCode());
        result = VK_ERROR_UNKNOWN;
        if (messenger != nullptr) {
            *messenger = VK_NULL_HANDLE;
        }
        return false;
    }
#else
    exceptionCode = 0;
    result = vkCreateDebugUtilsMessengerEXT(instance, createInfo, allocator, messenger);
    return true;
#endif
}

static VkResult _EnumeratePhysicalDevicesSafe(
    VkInstance instance,
    uint32_t* count,
    VkPhysicalDevice* physicalDevices) noexcept {
#if defined(_WIN32)
    __try {
        return vkEnumeratePhysicalDevices(instance, count, physicalDevices);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return VK_ERROR_UNKNOWN;
    }
#else
    return vkEnumeratePhysicalDevices(instance, count, physicalDevices);
#endif
}

struct PhysicalDeviceInfoVulkan {
    VkPhysicalDevice device;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VulkanPhysicalDeviceInfo publicInfo;
};

static PhysicalDeviceType _MapPhysicalDeviceType(VkPhysicalDeviceType type) noexcept {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return PhysicalDeviceType::IntegratedGpu;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return PhysicalDeviceType::DiscreteGpu;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return PhysicalDeviceType::VirtualGpu;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return PhysicalDeviceType::Cpu;
        default: return PhysicalDeviceType::Other;
    }
}

static int _GetPhysicalDeviceTypeScore(VkPhysicalDeviceType type) noexcept {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
    }
}

static std::optional<vector<PhysicalDeviceInfoVulkan>> _EnumeratePhysicalDeviceInfos(VkInstance instance, bool logDevices) noexcept {
    vector<VkPhysicalDevice> physicalDevices;
    if (EnumerateVectorFromVkFunc(physicalDevices, _EnumeratePhysicalDevicesSafe, instance) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkEnumeratePhysicalDevices failed");
        return std::nullopt;
    }
    if (physicalDevices.empty()) {
        RADRAY_ERR_LOG("vk no physical device found");
        return std::nullopt;
    }

    vector<PhysicalDeviceInfoVulkan> result;
    result.reserve(physicalDevices.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(physicalDevices.size()); ++i) {
        const auto& physicalDevice = physicalDevices[i];
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

        const uint64_t deviceLocalMemory = GetPhysicalDeviceMemoryAllSize(memory, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        if (logDevices) {
            RADRAY_INFO_LOG("vk find physical device: {}, heap memory: {}MB", properties.deviceName, deviceLocalMemory / (1024 * 1024));
        }

        VulkanPhysicalDeviceInfo publicInfo{};
        publicInfo.Index = i;
        publicInfo.Name = properties.deviceName;
        publicInfo.Type = _MapPhysicalDeviceType(properties.deviceType);
        publicInfo.VendorId = properties.vendorID;
        publicInfo.DeviceId = properties.deviceID;
        publicInfo.ApiVersionMajor = VK_VERSION_MAJOR(properties.apiVersion);
        publicInfo.ApiVersionMinor = VK_VERSION_MINOR(properties.apiVersion);
        publicInfo.ApiVersionPatch = VK_VERSION_PATCH(properties.apiVersion);
        publicInfo.DeviceLocalMemoryBytes = deviceLocalMemory;
        publicInfo.IsUma = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

        result.emplace_back(PhysicalDeviceInfoVulkan{physicalDevice, properties, memory, std::move(publicInfo)});
    }
    return result;
}

static std::optional<size_t> _SelectHighPerformancePhysicalDeviceArrayIndex(std::span<const PhysicalDeviceInfoVulkan> physicalDevices) noexcept {
    if (physicalDevices.empty()) {
        return std::nullopt;
    }

    vector<size_t> indices;
    indices.reserve(physicalDevices.size());
    for (size_t i = 0; i < physicalDevices.size(); ++i) {
        indices.emplace_back(i);
    }

    std::stable_sort(indices.begin(), indices.end(), [&physicalDevices](size_t lhsIndex, size_t rhsIndex) noexcept {
        const auto& lhs = physicalDevices[lhsIndex];
        const auto& rhs = physicalDevices[rhsIndex];
        const int lscore = _GetPhysicalDeviceTypeScore(lhs.properties.deviceType);
        const int rscore = _GetPhysicalDeviceTypeScore(rhs.properties.deviceType);
        if (lscore != rscore) {
            return lscore > rscore;
        }
        if (lhs.properties.apiVersion != rhs.properties.apiVersion) {
            return lhs.properties.apiVersion > rhs.properties.apiVersion;
        }
        return lhs.publicInfo.DeviceLocalMemoryBytes > rhs.publicInfo.DeviceLocalMemoryBytes;
    });

    return indices[0];
}

static bool _TryCreateVkDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDevice* device,
    VkResult& result,
    uint32_t& exceptionCode) noexcept {
#if defined(_WIN32)
    exceptionCode = 0;
    __try {
        result = vkCreateDevice(physicalDevice, createInfo, allocator, device);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exceptionCode = static_cast<uint32_t>(GetExceptionCode());
        result = VK_ERROR_UNKNOWN;
        if (device != nullptr) {
            *device = VK_NULL_HANDLE;
        }
        return false;
    }
#else
    exceptionCode = 0;
    result = vkCreateDevice(physicalDevice, createInfo, allocator, device);
    return true;
#endif
}

static VKAPI_ATTR VkBool32 VKAPI_CALL VKDebugUtilsMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) noexcept {
    auto* instance = static_cast<InstanceVulkanImpl*>(pUserData);
    try {
        LogLevel lvl = LogLevel::Info;
        if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            lvl = LogLevel::Err;
        } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            lvl = LogLevel::Warn;
        } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
            lvl = LogLevel::Info;
        } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
            lvl = LogLevel::Debug;
        }
        const auto msg = fmt::format(
            "type={} id={} {}: {}",
            VulkanDebugUtilsMessageTypeFlagsEXT{static_cast<std::underlying_type_t<VkDebugUtilsMessageTypeFlagBitsEXT>>(messageType)},
            pCallbackData == nullptr ? 0 : pCallbackData->messageIdNumber,
            (pCallbackData == nullptr || pCallbackData->pMessageIdName == nullptr) ? "" : pCallbackData->pMessageIdName,
            (pCallbackData == nullptr || pCallbackData->pMessage == nullptr) ? "" : pCallbackData->pMessage);
        if (instance != nullptr && instance->_logCallback != nullptr) {
            instance->_logCallback(lvl, msg, instance->_logUserData);
        } else {
            switch (lvl) {
                case LogLevel::Critical: RADRAY_ERR_LOG("vk Validation Layer\n{}", msg); break;
                case LogLevel::Err: RADRAY_ERR_LOG("vk Validation Layer\n{}", msg); break;
                case LogLevel::Warn: RADRAY_WARN_LOG("vk Validation Layer\n{}", msg); break;
                case LogLevel::Info: RADRAY_INFO_LOG("vk Validation Layer\n{}", msg); break;
                case LogLevel::Debug: RADRAY_DEBUG_LOG("vk Validation Layer\n{}", msg); break;
                case LogLevel::Trace: RADRAY_DEBUG_LOG("vk Validation Layer\n{}", msg); break;
            }
        }
    } catch (...) {
    }
    return VK_FALSE;
}

static VkDescriptorType BufferViewUsageToDescriptorType(BufferViewUsage usage) noexcept {
    switch (usage) {
        case BufferViewUsage::CBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case BufferViewUsage::ReadOnlyStorage: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case BufferViewUsage::ReadWriteStorage: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case BufferViewUsage::TexelReadOnly: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case BufferViewUsage::TexelReadWrite: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    }
    Unreachable();
}

static bool _ResolveBufferBindingRangeSizeVulkan(const BufferBindingDescriptor& desc, uint64_t& rangeSize) noexcept {
    const uint64_t bufferSize = desc.Target->GetDesc().Size;
    if (desc.Range.Offset > bufferSize) {
        RADRAY_ERR_LOG(
            "vk buffer binding offset out of range. offset={}, bufferSize={}",
            desc.Range.Offset,
            bufferSize);
        return false;
    }
    rangeSize = desc.Range.Size == BufferRange::All()
                    ? bufferSize - desc.Range.Offset
                    : desc.Range.Size;
    return true;
}

constexpr uint32_t kBindlessDescriptorCapacityVulkan = 262144;

static std::optional<ResourceBindType> _GetResourceViewBindTypeVulkan(ResourceView* view) noexcept {
    if (view == nullptr) {
        return std::nullopt;
    }
    const auto tag = view->GetTag();
    if (tag.HasFlag(RenderObjectTag::TextureView)) {
        const auto* textureView = static_cast<ImageViewVulkan*>(view);
        switch (textureView->_mdesc.Usage) {
            case TextureViewUsage::Resource: return ResourceBindType::Texture;
            case TextureViewUsage::UnorderedAccess: return ResourceBindType::RWTexture;
            default: return std::nullopt;
        }
    }
    if (tag.HasFlag(RenderObjectTag::AccelerationStructureView)) {
        return ResourceBindType::AccelerationStructure;
    }
    return std::nullopt;
}

// 构建期临时结构: 记录每个静态采样器参数的定位信息与采样器描述, 仅在 CreateRootSignatureInternal 内使用.
struct _StaticSamplerBuildInfoVulkan {
    uint32_t ParameterIndex{0};
    uint32_t SetIndex{0};
    uint32_t BindingIndex{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    SamplerDescriptor Desc{};
};

struct _StaticSamplerSelectionVulkan {
    vector<ShaderParameterInfo> PublicParameters{};
    vector<_StaticSamplerBuildInfoVulkan> StaticSamplers{};
    vector<uint8_t> IsStaticParameter{};
};

static std::optional<_StaticSamplerSelectionVulkan> _SelectStaticSamplersVulkan(
    std::span<const ShaderParameterInfo> parameters,
    std::span<const VulkanBindingParameterInfo> lowering,
    std::span<const StaticSamplerDescriptor> staticSamplers) noexcept {
    if (lowering.size() != parameters.size()) {
        RADRAY_ERR_LOG("internal error: static sampler selection metadata size mismatch");
        return std::nullopt;
    }

    _StaticSamplerSelectionVulkan result{};
    result.IsStaticParameter.resize(parameters.size(), 0);
    result.PublicParameters.reserve(parameters.size());
    result.StaticSamplers.reserve(staticSamplers.size());

    unordered_set<string> usedNames{};
    for (const auto& staticSampler : staticSamplers) {
        if (staticSampler.Name.empty()) {
            RADRAY_ERR_LOG("static sampler declaration must name a shader sampler parameter");
            return std::nullopt;
        }
        if (!usedNames.insert(staticSampler.Name).second) {
            RADRAY_ERR_LOG("duplicate static sampler declaration '{}'", staticSampler.Name);
            return std::nullopt;
        }
        size_t matchedIndex = parameters.size();
        for (size_t i = 0; i < parameters.size(); ++i) {
            const auto& parameter = parameters[i];
            if (parameter.Kind != ShaderParameterKind::Sampler) {
                continue;
            }
            if (parameter.Name == staticSampler.Name) {
                matchedIndex = i;
                break;
            }
        }
        if (matchedIndex == parameters.size()) {
            RADRAY_ERR_LOG("static sampler '{}' does not match any shader sampler parameter", staticSampler.Name);
            return std::nullopt;
        }
        if (result.IsStaticParameter[matchedIndex]) {
            RADRAY_ERR_LOG("duplicate static sampler declaration '{}'", staticSampler.Name);
            return std::nullopt;
        }

        const auto& parameter = parameters[matchedIndex];
        const auto& vkInfo = lowering[matchedIndex];
        if (parameter.Type != ResourceBindType::Sampler || parameter.IsBindless) {
            RADRAY_ERR_LOG(
                "static sampler '{}' must target a non-bindless sampler binding",
                parameter.Name);
            return std::nullopt;
        }
        if (parameter.Count != 1) {
            RADRAY_ERR_LOG(
                "static sampler '{}' does not support sampler arrays (count={})",
                parameter.Name,
                parameter.Count);
            return std::nullopt;
        }
        if (vkInfo.DescriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
            RADRAY_ERR_LOG("vk lowering metadata is unavailable for static sampler '{}'", parameter.Name);
            return std::nullopt;
        }
        result.IsStaticParameter[matchedIndex] = 1;
        result.StaticSamplers.push_back(_StaticSamplerBuildInfoVulkan{
            .ParameterIndex = static_cast<uint32_t>(matchedIndex),
            .SetIndex = vkInfo.SetIndex,
            .BindingIndex = vkInfo.BindingIndex,
            .Stages = parameter.Stages,
            .Desc = staticSampler.Desc,
        });
    }

    for (size_t i = 0; i < parameters.size(); ++i) {
        if (!result.IsStaticParameter[i]) {
            result.PublicParameters.push_back(parameters[i]);
        }
    }
    return result;
}

static unique_ptr<DescriptorSetLayoutVulkan> _CreateDescriptorSetLayoutVulkan(
    DeviceVulkan* device,
    const vector<VkDescriptorSetLayoutBinding>& bindings,
    vector<DescriptorSetLayoutBindingVulkanContainer> bindingContainers,
    std::span<const VkDescriptorBindingFlags> bindingFlags = {},
    VkDescriptorSetLayoutCreateFlags layoutFlags = 0) noexcept {
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = nullptr;
    layoutInfo.flags = layoutFlags;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.empty() ? nullptr : bindings.data();
    if (!bindingFlags.empty()) {
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.pNext = nullptr;
        flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
        flagsInfo.pBindingFlags = bindingFlags.data();
        layoutInfo.pNext = &flagsInfo;
    }
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (auto vr = device->_ftb.vkCreateDescriptorSetLayout(device->_device, &layoutInfo, device->GetAllocationCallbacks(), &layout);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateDescriptorSetLayout failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<DescriptorSetLayoutVulkan>(device, layout);
    result->_bindings = std::move(bindingContainers);
    return result;
}

static bool _IsDescriptorSetLayoutCompatible(
    const DescriptorSetLayoutVulkan& source,
    std::span<const DescriptorSetLayoutBindingVulkanContainer> expected) noexcept {
    if (!source.IsValid() || source._bindings.size() != expected.size()) {
        return false;
    }
    for (const DescriptorSetLayoutBindingVulkanContainer& expectedBinding : expected) {
        const auto sourceBinding = std::ranges::find_if(
            source._bindings,
            [&expectedBinding](const DescriptorSetLayoutBindingVulkanContainer& binding) noexcept {
                return binding.binding == expectedBinding.binding;
            });
        if (sourceBinding == source._bindings.end() ||
            sourceBinding->bindType != expectedBinding.bindType ||
            sourceBinding->descriptorType != expectedBinding.descriptorType ||
            sourceBinding->descriptorCount != expectedBinding.descriptorCount ||
            sourceBinding->stageFlags != expectedBinding.stageFlags ||
            sourceBinding->bindingFlags != expectedBinding.bindingFlags ||
            sourceBinding->immutableSamplers.size() != expectedBinding.immutableSamplers.size()) {
            return false;
        }
        for (size_t i = 0; i < sourceBinding->immutableSamplers.size(); ++i) {
            const auto& sourceSampler = sourceBinding->immutableSamplers[i];
            const auto& expectedSampler = expectedBinding.immutableSamplers[i];
            if (sourceSampler == nullptr || expectedSampler == nullptr ||
                sourceSampler->_mdesc != expectedSampler->_mdesc) {
                return false;
            }
        }
    }
    return true;
}

static bool _BindDescriptorSetVulkan(
    DeviceVulkan* device,
    CommandBufferVulkan* cmdBuffer,
    PipelineLayoutVulkan* boundPipeLayout,
    uint32_t setIndex,
    DescriptorSetVulkan* set,
    VkPipelineBindPoint bindPoint,
    std::span<const uint32_t> dynamicOffsets) noexcept {
    if (boundPipeLayout == nullptr) {
        RADRAY_ERR_LOG("bind shader binding layout before binding shader parameters");
        return false;
    }
    if (set == nullptr) {
        RADRAY_ERR_LOG("descriptor set is null");
        return false;
    }
    if (set == nullptr || !set->IsValid()) {
        RADRAY_ERR_LOG("descriptor set is invalid");
        return false;
    }
    if (set->GetRootSignature() != boundPipeLayout) {
        RADRAY_ERR_LOG("descriptor set belongs to a different root signature");
        return false;
    }
    if (set->GetSetIndex() != setIndex) {
        RADRAY_ERR_LOG(
            "descriptor set index mismatch expected: {}, actual: {}",
            setIndex,
            set->GetSetIndex());
        return false;
    }
    if (!set->IsFullyWritten()) {
        uint32_t missingArrayIndex = 0;
        const auto* missing = set->FindFirstUnwrittenParameter(&missingArrayIndex).Get();
        if (missing != nullptr && missing->Info.Count > 1) {
            RADRAY_ERR_LOG(
                "descriptor set is missing parameter '{}[{}]'",
                missing->Info.Name,
                missingArrayIndex);
        } else if (missing != nullptr) {
            RADRAY_ERR_LOG("descriptor set is missing parameter '{}'", missing->Info.Name);
        } else {
            RADRAY_ERR_LOG("descriptor set is not fully written");
        }
        return false;
    }
    const auto dynamicBindings = boundPipeLayout->GetDynamicBufferBindings(setIndex);
    if (dynamicOffsets.size() != dynamicBindings.size()) {
        RADRAY_ERR_LOG(
            "dynamic offset count mismatch for set {} expected: {}, actual: {}",
            setIndex,
            dynamicBindings.size(),
            dynamicOffsets.size());
        return false;
    }
    device->_ftb.vkCmdBindDescriptorSets(
        cmdBuffer->_cmdBuffer,
        bindPoint,
        boundPipeLayout->_layout,
        setIndex,
        1,
        &set->_allocation.Set,
        static_cast<uint32_t>(dynamicOffsets.size()),
        dynamicOffsets.empty() ? nullptr : dynamicOffsets.data());
    return true;
}

static bool _BindlessArrayMatchesVulkan(
    const PipelineLayoutVulkan::ParameterBinding& bindlessInfo,
    const BindlessArrayVulkan* array) noexcept {
    if (array == nullptr || !array->IsValid()) {
        RADRAY_ERR_LOG("bindless array is invalid");
        return false;
    }
    if (array->_slotType != bindlessInfo.BindlessSlotType) {
        RADRAY_ERR_LOG(
            "bindless array slot type mismatch for set {} expected: {}, actual: {}",
            bindlessInfo.SetIndex,
            static_cast<uint32_t>(bindlessInfo.BindlessSlotType),
            static_cast<uint32_t>(array->_slotType));
        return false;
    }
    for (size_t i = 0; i < array->_slots.size(); ++i) {
        const ResourceBindType slotType = array->_slots[i].ResourceType;
        if (slotType == ResourceBindType::UNKNOWN) {
            continue;
        }
        bool isCompatible = false;
        switch (bindlessInfo.Info.Type) {
            case ResourceBindType::Buffer:
            case ResourceBindType::RWBuffer:
                isCompatible = slotType == ResourceBindType::Buffer || slotType == ResourceBindType::RWBuffer;
                break;
            case ResourceBindType::Texture:
                isCompatible = slotType == ResourceBindType::Texture;
                break;
            default:
                isCompatible = false;
                break;
        }
        if (!isCompatible) {
            RADRAY_ERR_LOG(
                "bindless array slot {} type mismatch for set {} expected: {}, actual: {}",
                i,
                bindlessInfo.SetIndex,
                bindlessInfo.Info.Type,
                slotType);
            return false;
        }
    }
    return true;
}

static bool _UpdateBindlessDescriptorSetVulkan(
    DeviceVulkan* device,
    VkDescriptorSet set,
    uint32_t bindingIndex,
    VkDescriptorType descriptorType,
    uint32_t arrayIndex,
    ResourceView* view) noexcept {
    if (device == nullptr || set == VK_NULL_HANDLE || view == nullptr) {
        return false;
    }
    VkWriteDescriptorSet writeDesc{};
    writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDesc.pNext = nullptr;
    writeDesc.dstSet = set;
    writeDesc.dstBinding = bindingIndex;
    writeDesc.dstArrayElement = arrayIndex;
    writeDesc.descriptorCount = 1;
    writeDesc.descriptorType = descriptorType;

    VkDescriptorImageInfo imgInfo{};
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    const auto tag = view->GetTag();
    if (tag.HasFlag(RenderObjectTag::TextureView)) {
        auto* textureView = static_cast<ImageViewVulkan*>(view);
        if (descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
            textureView->_mdesc.Usage != TextureViewUsage::Resource) {
            RADRAY_ERR_LOG("descriptor type mismatch for bindless texture view");
            return false;
        }
        imgInfo.sampler = VK_NULL_HANDLE;
        imgInfo.imageView = textureView->_imageView;
        imgInfo.imageLayout = TextureViewUsageToLayout(textureView->_mdesc.Usage);
        writeDesc.pImageInfo = &imgInfo;
    } else if (tag.HasFlag(RenderObjectTag::AccelerationStructureView)) {
        auto* asView = static_cast<AccelerationStructureViewVulkan*>(view);
        if (descriptorType != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR ||
            asView->_target == nullptr ||
            asView->_target->_accelerationStructure == VK_NULL_HANDLE) {
            RADRAY_ERR_LOG("invalid bindless acceleration structure descriptor");
            return false;
        }
        asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asInfo.pNext = nullptr;
        asInfo.accelerationStructureCount = 1;
        asInfo.pAccelerationStructures = &asView->_target->_accelerationStructure;
        writeDesc.pNext = &asInfo;
    } else {
        RADRAY_ERR_LOG("unsupported bindless resource view tag {}", tag);
        return false;
    }

    device->_ftb.vkUpdateDescriptorSets(device->_device, 1, &writeDesc, 0, nullptr);
    return true;
}

static bool _SubmitDescriptorWritesVulkan(
    DeviceVulkan* device,
    std::span<const PendingDescriptorWriteVulkan> pendingWrites,
    vector<VkWriteDescriptorSet>& writes,
    vector<VkWriteDescriptorSetAccelerationStructureKHR>& accelerationWrites) noexcept {
    if (device == nullptr || pendingWrites.empty()) {
        return device != nullptr;
    }

    writes.resize(pendingWrites.size());
    accelerationWrites.resize(pendingWrites.size());
    for (size_t i = 0; i < pendingWrites.size(); ++i) {
        const auto& pending = pendingWrites[i];
        if (pending.Set == VK_NULL_HANDLE) {
            RADRAY_ERR_LOG("vk descriptor write has no destination set");
            return false;
        }

        auto& write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.pNext = nullptr;
        write.dstSet = pending.Set;
        write.dstBinding = pending.Binding;
        write.dstArrayElement = pending.ArrayIndex;
        write.descriptorCount = 1;
        write.descriptorType = pending.Type;
        write.pImageInfo = nullptr;
        write.pBufferInfo = nullptr;
        write.pTexelBufferView = nullptr;

        switch (pending.Payload) {
            case DescriptorWritePayloadVulkan::Image:
                write.pImageInfo = &pending.ImageInfo;
                break;
            case DescriptorWritePayloadVulkan::Buffer:
                write.pBufferInfo = &pending.BufferInfo;
                break;
            case DescriptorWritePayloadVulkan::TexelBuffer:
                write.pTexelBufferView = &pending.TexelBufferView;
                break;
            case DescriptorWritePayloadVulkan::AccelerationStructure: {
                auto& accelerationWrite = accelerationWrites[i];
                accelerationWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                accelerationWrite.pNext = nullptr;
                accelerationWrite.accelerationStructureCount = 1;
                accelerationWrite.pAccelerationStructures = &pending.AccelerationStructure;
                write.pNext = &accelerationWrite;
                break;
            }
        }
    }

    device->_ftb.vkUpdateDescriptorSets(
        device->_device,
        static_cast<uint32_t>(writes.size()),
        writes.data(),
        0,
        nullptr);
    return true;
}

static bool _SubmitDescriptorWritesVulkan(
    DeviceVulkan* device,
    std::span<const PendingDescriptorWriteVulkan> pendingWrites) noexcept {
    vector<VkWriteDescriptorSet> writes;
    vector<VkWriteDescriptorSetAccelerationStructureKHR> accelerationWrites;
    return _SubmitDescriptorWritesVulkan(device, pendingWrites, writes, accelerationWrites);
}

static bool _BuildBufferBindingDescriptorVulkan(
    DeviceVulkan* device,
    VkDescriptorSet set,
    uint32_t bindingIndex,
    VkDescriptorType descriptorType,
    uint32_t arrayIndex,
    const BufferBindingDescriptor& desc,
    bool allowTexelBuffer,
    PendingDescriptorWriteVulkan& write,
    unique_ptr<BufferViewVulkan>& ownedTexelView) noexcept {
    if (device == nullptr || set == VK_NULL_HANDLE) {
        return false;
    }
    if (desc.Target == nullptr) {
        RADRAY_ERR_LOG("BufferBindingDescriptor.Target is null");
        return false;
    }
    const auto requiredType = BufferViewUsageToDescriptorType(desc.Usage);
    const bool isDynamicUniform =
        requiredType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    if (requiredType != descriptorType && !isDynamicUniform) {
        RADRAY_ERR_LOG(
            "descriptor type mismatch for buffer binding. expected={}, actual={}",
            requiredType,
            descriptorType);
        return false;
    }
    uint64_t rangeSize = 0;
    if (!_ResolveBufferBindingRangeSizeVulkan(desc, rangeSize)) {
        return false;
    }

    auto* buffer = CastVkObject(desc.Target);
    write = {};
    write.Set = set;
    write.Binding = bindingIndex;
    write.ArrayIndex = arrayIndex;
    write.Type = descriptorType;

    switch (desc.Usage) {
        case BufferViewUsage::CBuffer: {
            const uint64_t align = std::max<uint64_t>(1, device->_detail.CBufferAlignment);
            if (desc.Range.Offset % align != 0) {
                RADRAY_ERR_LOG("vk uniform buffer binding offset must align to CBuffer alignment");
                return false;
            }
            write.Payload = DescriptorWritePayloadVulkan::Buffer;
            write.BufferInfo.buffer = buffer->_buffer;
            write.BufferInfo.offset = desc.Range.Offset;
            write.BufferInfo.range = rangeSize;
            break;
        }
        case BufferViewUsage::ReadOnlyStorage:
        case BufferViewUsage::ReadWriteStorage: {
            if (desc.Stride == 0) {
                RADRAY_ERR_LOG("vk structured buffer binding stride must be non-zero");
                return false;
            }
            if (desc.Range.Offset % desc.Stride != 0 || rangeSize % desc.Stride != 0) {
                RADRAY_ERR_LOG("vk structured buffer binding offset/size must align to stride");
                return false;
            }
            write.Payload = DescriptorWritePayloadVulkan::Buffer;
            write.BufferInfo.buffer = buffer->_buffer;
            write.BufferInfo.offset = desc.Range.Offset;
            write.BufferInfo.range = rangeSize;
            break;
        }
        case BufferViewUsage::TexelReadOnly:
        case BufferViewUsage::TexelReadWrite: {
            if (!allowTexelBuffer) {
                RADRAY_ERR_LOG("vk bindless arrays do not support texel buffer descriptors in this revision");
                return false;
            }
            const auto bpp = GetTextureFormatBytesPerPixel(desc.Format);
            if (bpp == 0) {
                RADRAY_ERR_LOG("vk texel buffer binding format must not be UNKNOWN");
                return false;
            }
            if (desc.Range.Offset % bpp != 0 || rangeSize % bpp != 0) {
                RADRAY_ERR_LOG("vk texel buffer binding offset/size must align to format bytes");
                return false;
            }
            VkBufferViewCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
            info.pNext = nullptr;
            info.flags = 0;
            info.buffer = buffer->_buffer;
            info.format = MapType(desc.Format);
            info.offset = desc.Range.Offset;
            info.range = rangeSize;
            auto texelViewOpt = device->CreateBufferView(info);
            if (!texelViewOpt) {
                return false;
            }
            ownedTexelView = texelViewOpt.Release();
            write.Payload = DescriptorWritePayloadVulkan::TexelBuffer;
            write.TexelBufferView = ownedTexelView->_bufferView;
            break;
        }
    }

    return true;
}

static bool _WriteBufferBindingDescriptorVulkan(
    DeviceVulkan* device,
    VkDescriptorSet set,
    uint32_t bindingIndex,
    VkDescriptorType descriptorType,
    uint32_t arrayIndex,
    const BufferBindingDescriptor& desc,
    unordered_map<uint64_t, unique_ptr<BufferViewVulkan>>* ownedTexelViews) noexcept {
    PendingDescriptorWriteVulkan write{};
    unique_ptr<BufferViewVulkan> ownedTexelView{};
    if (!_BuildBufferBindingDescriptorVulkan(
            device,
            set,
            bindingIndex,
            descriptorType,
            arrayIndex,
            desc,
            ownedTexelViews != nullptr,
            write,
            ownedTexelView) ||
        !_SubmitDescriptorWritesVulkan(device, std::span{&write, 1})) {
        return false;
    }

    if (ownedTexelViews != nullptr) {
        const uint64_t descriptorKey = (static_cast<uint64_t>(bindingIndex) << 32u) | static_cast<uint64_t>(arrayIndex);
        if (ownedTexelView) {
            (*ownedTexelViews)[descriptorKey] = std::move(ownedTexelView);
        } else {
            ownedTexelViews->erase(descriptorKey);
        }
    }
    return true;
}

static std::optional<VkDescriptorSet> _PrepareBindlessDescriptorSetVulkan(
    DeviceVulkan* device,
    BindlessArrayVulkan* array,
    DescriptorSetLayoutVulkan* layout,
    const PipelineLayoutVulkan::ParameterBinding& bindlessInfo) noexcept {
    if (device == nullptr || array == nullptr || layout == nullptr) {
        return std::nullopt;
    }
    auto it = std::find_if(
        array->_cachedSets.begin(),
        array->_cachedSets.end(),
        [&](const BindlessArrayVulkan::CachedDescriptorSet& cached) {
            return cached.Layout == layout;
        });
    if (it == array->_cachedSets.end()) {
        auto allocOpt = device->_descSetAlloc->Allocate(layout, array->_size);
        if (!allocOpt.has_value()) {
            RADRAY_ERR_LOG("failed to allocate vk bindless descriptor set for set {}", bindlessInfo.SetIndex);
            return std::nullopt;
        }
        array->_cachedSets.push_back(BindlessArrayVulkan::CachedDescriptorSet{
            .Layout = layout,
            .Allocation = allocOpt.value(),
            .BindingIndex = bindlessInfo.BindingIndex,
            .DescriptorType = bindlessInfo.DescriptorType,
            .Dirty = true,
        });
        it = std::prev(array->_cachedSets.end());
    }
    if (it->Dirty) {
        for (uint32_t slot = 0; slot < array->_size; ++slot) {
            const auto& slotState = array->_slots[slot];
            switch (slotState.Kind) {
                case BindlessArrayVulkan::SlotKind::None:
                    continue;
                case BindlessArrayVulkan::SlotKind::Buffer:
                    if (!_WriteBufferBindingDescriptorVulkan(
                            device,
                            it->Allocation.Set,
                            it->BindingIndex,
                            it->DescriptorType,
                            slot,
                            slotState.BufferDesc,
                            nullptr)) {
                        return std::nullopt;
                    }
                    break;
                case BindlessArrayVulkan::SlotKind::Texture2D:
                case BindlessArrayVulkan::SlotKind::Texture3D: {
                    auto view = slotState.Texture;
                    if (view == nullptr) {
                        continue;
                    }
                    if (!_UpdateBindlessDescriptorSetVulkan(
                            device,
                            it->Allocation.Set,
                            it->BindingIndex,
                            it->DescriptorType,
                            slot,
                            view.Get())) {
                        return std::nullopt;
                    }
                    break;
                }
            }
        }
        it->Dirty = false;
    }
    return it->Allocation.Set;
}

static bool _BindBindlessArrayVulkan(
    DeviceVulkan* device,
    CommandBufferVulkan* cmdBuffer,
    PipelineLayoutVulkan* boundPipeLayout,
    uint32_t setIndex,
    BindlessArray* array_,
    VkPipelineBindPoint bindPoint) noexcept {
    if (boundPipeLayout == nullptr) {
        RADRAY_ERR_LOG("bind root signature before CommandEncoder::BindBindlessArray");
        return false;
    }
    if (array_ == nullptr) {
        RADRAY_ERR_LOG("bindless array is null");
        return false;
    }
    auto* array = static_cast<BindlessArrayVulkan*>(array_);
    if (array == nullptr || !array->IsValid()) {
        RADRAY_ERR_LOG("bindless array is invalid");
        return false;
    }
    auto bindlessInfoOpt = boundPipeLayout->FindBindlessSet(setIndex);
    if (!bindlessInfoOpt.HasValue() || bindlessInfoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("set {} is not declared as a bindless set", setIndex);
        return false;
    }
    const auto* bindlessInfo = bindlessInfoOpt.Get();
    if (!_BindlessArrayMatchesVulkan(*bindlessInfo, array)) {
        return false;
    }
    auto setLayoutOpt = boundPipeLayout->GetSetLayout(setIndex);
    if (!setLayoutOpt.HasValue() || setLayoutOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("vk bindless set layout {} is unavailable", setIndex);
        return false;
    }
    auto descriptorSetOpt = _PrepareBindlessDescriptorSetVulkan(device, array, setLayoutOpt.Get(), *bindlessInfo);
    if (!descriptorSetOpt.has_value()) {
        return false;
    }
    const auto descriptorSet = descriptorSetOpt.value();
    device->_ftb.vkCmdBindDescriptorSets(
        cmdBuffer->_cmdBuffer,
        bindPoint,
        boundPipeLayout->_layout,
        setIndex,
        1,
        &descriptorSet,
        0,
        nullptr);
    return true;
}

static bool _PushConstantsVulkan(
    DeviceVulkan* device,
    CommandBufferVulkan* cmdBuffer,
    PipelineLayoutVulkan* boundPipeLayout,
    uint32_t groupIndex,
    uint32_t bindingIndex,
    std::span<const byte> data) noexcept {
    if (boundPipeLayout == nullptr) {
        RADRAY_ERR_LOG("bind root signature before CommandEncoder::PushConstants");
        return false;
    }
    if (data.empty()) {
        RADRAY_ERR_LOG("push constant data is null");
        return false;
    }
    const PipelineLayoutVulkan::ParameterBinding* info = nullptr;
    for (const auto& candidate : boundPipeLayout->GetParameterBindings()) {
        if (candidate.Info.Kind == ShaderParameterKind::Constant &&
            candidate.SetIndex == groupIndex && candidate.BindingIndex == bindingIndex) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr) {
        RADRAY_ERR_LOG("push constant range at group {} binding {} is unavailable", groupIndex, bindingIndex);
        return false;
    }
    if (data.size() != info->PushConstantSize) {
        RADRAY_ERR_LOG(
            "push constant size mismatch at group {} binding {} expected: {}, actual: {}",
            groupIndex,
            bindingIndex,
            info->PushConstantSize,
            data.size());
        return false;
    }
    device->_ftb.vkCmdPushConstants(
        cmdBuffer->_cmdBuffer,
        boundPipeLayout->_layout,
        MapType(info->Info.Stages),
        info->PushConstantOffset,
        info->PushConstantSize,
        data.data());
    return true;
}

InstanceVulkanImpl::InstanceVulkanImpl(
    VkInstance instance,
    std::optional<VkAllocationCallbacks> allocCb,
    vector<string> exts,
    vector<string> layers) noexcept
    : _instance(instance),
      _allocCb(std::move(allocCb)),
      _exts(std::move(exts)),
      _layers(std::move(layers)) {}

InstanceVulkanImpl::~InstanceVulkanImpl() noexcept {
    this->DestroyImpl();
}

bool InstanceVulkanImpl::IsValid() const noexcept { return _instance != nullptr; }

void InstanceVulkanImpl::Destroy() noexcept { this->DestroyImpl(); }

vector<VulkanPhysicalDeviceInfo> InstanceVulkanImpl::GetPhysicalDevices() const noexcept {
    if (_instance == VK_NULL_HANDLE) {
        RADRAY_ERR_LOG("vk env not init");
        return {};
    }

    auto physicalDevices = _EnumeratePhysicalDeviceInfos(_instance, false);
    if (!physicalDevices.has_value()) {
        return {};
    }

    vector<VulkanPhysicalDeviceInfo> result;
    result.reserve(physicalDevices->size());
    for (const auto& physicalDevice : physicalDevices.value()) {
        result.emplace_back(physicalDevice.publicInfo);
    }
    return result;
}

std::optional<uint32_t> InstanceVulkanImpl::SelectHighPerformancePhysicalDevice() const noexcept {
    if (_instance == VK_NULL_HANDLE) {
        RADRAY_ERR_LOG("vk env not init");
        return std::nullopt;
    }

    auto physicalDevices = _EnumeratePhysicalDeviceInfos(_instance, false);
    if (!physicalDevices.has_value()) {
        return std::nullopt;
    }
    auto selectedIndex = _SelectHighPerformancePhysicalDeviceArrayIndex(physicalDevices.value());
    if (!selectedIndex.has_value()) {
        return std::nullopt;
    }
    return physicalDevices.value()[selectedIndex.value()].publicInfo.Index;
}

const VkAllocationCallbacks* InstanceVulkanImpl::GetAllocationCallbacks() const noexcept {
    return _allocCb.has_value() ? &_allocCb.value() : nullptr;
}

void InstanceVulkanImpl::DestroyImpl() noexcept {
    if (_debugMessenger != VK_NULL_HANDLE) {
        if (vkDestroyDebugUtilsMessengerEXT != nullptr) {
            vkDestroyDebugUtilsMessengerEXT(_instance, _debugMessenger, this->GetAllocationCallbacks());
        }
        _debugMessenger = VK_NULL_HANDLE;
    }
    _logCallback = nullptr;
    _logUserData = nullptr;
    if (_instance != nullptr) {
        const VkAllocationCallbacks* allocCbPtr = this->GetAllocationCallbacks();
        vkDestroyInstance(_instance, allocCbPtr);
        _instance = nullptr;
    }
}

VMA::VMA(VmaAllocator vma) noexcept
    : _vma(vma) {}

VMA::~VMA() noexcept {
    this->DestroyImpl();
}

bool VMA::IsValid() const noexcept {
    return _vma != VK_NULL_HANDLE;
}

void VMA::Destroy() noexcept {
    this->DestroyImpl();
}

void VMA::DestroyImpl() noexcept {
    if (_vma != VK_NULL_HANDLE) {
        vmaDestroyAllocator(_vma);
        _vma = VK_NULL_HANDLE;
    }
}

DeviceVulkan::DeviceVulkan(
    InstanceVulkanImpl* instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device) noexcept
    : _instance(instance),
      _physicalDevice(physicalDevice),
      _device(device) {}

DeviceVulkan::~DeviceVulkan() noexcept {
    this->DestroyImpl();
}

bool DeviceVulkan::IsValid() const noexcept {
    return _device != VK_NULL_HANDLE && _vma != nullptr;
}

void DeviceVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

DeviceDetail DeviceVulkan::GetDetail() const noexcept {
    return _detail;
}

bool DeviceVulkan::InitializeNativeGraphicsPipelineCache(
    std::span<const byte> initialData) noexcept {
    if (_device == VK_NULL_HANDLE) {
        return false;
    }
    if (_pipelineCache != VK_NULL_HANDLE) {
        _ftb.vkDestroyPipelineCache(_device, _pipelineCache, GetAllocationCallbacks());
        _pipelineCache = VK_NULL_HANDLE;
    }

    bool compatible = initialData.empty();
    if (initialData.size() >= sizeof(VkPipelineCacheHeaderVersionOne)) {
        VkPipelineCacheHeaderVersionOne header{};
        std::memcpy(&header, initialData.data(), sizeof(header));
        compatible = header.headerSize >= sizeof(VkPipelineCacheHeaderVersionOne) &&
                     header.headerSize <= initialData.size() &&
                     header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                     header.vendorID == _properties.vendorID &&
                     header.deviceID == _properties.deviceID &&
                     std::memcmp(
                         header.pipelineCacheUUID,
                         _properties.pipelineCacheUUID,
                         VK_UUID_SIZE) == 0;
    }
    if (!compatible) {
        RADRAY_WARN_LOG("Vulkan pipeline cache blob is incompatible; creating an empty cache");
        initialData = {};
    }

    const auto createCache = [&](std::span<const byte> data) noexcept {
        VkPipelineCacheCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        createInfo.initialDataSize = data.size();
        createInfo.pInitialData = data.empty() ? nullptr : data.data();
        return _ftb.vkCreatePipelineCache(
            _device,
            &createInfo,
            GetAllocationCallbacks(),
            &_pipelineCache);
    };
    VkResult result = createCache(initialData);
    if (result != VK_SUCCESS && !initialData.empty()) {
        RADRAY_WARN_LOG("vkCreatePipelineCache rejected cached data: {}; retrying empty", result);
        _pipelineCache = VK_NULL_HANDLE;
        result = createCache({});
    }
    if (result != VK_SUCCESS) {
        RADRAY_WARN_LOG("vkCreatePipelineCache failed: {}", result);
        _pipelineCache = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

std::optional<vector<byte>> DeviceVulkan::SerializeNativeGraphicsPipelineCache() noexcept {
    if (_device == VK_NULL_HANDLE || _pipelineCache == VK_NULL_HANDLE) {
        return vector<byte>{};
    }
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        size_t size = 0;
        VkResult result = _ftb.vkGetPipelineCacheData(_device, _pipelineCache, &size, nullptr);
        if (result != VK_SUCCESS) {
            RADRAY_WARN_LOG("vkGetPipelineCacheData(size) failed: {}", result);
            return std::nullopt;
        }
        vector<byte> data(size);
        result = _ftb.vkGetPipelineCacheData(
            _device,
            _pipelineCache,
            &size,
            data.empty() ? nullptr : data.data());
        if (result == VK_SUCCESS) {
            data.resize(size);
            return data;
        }
        if (result != VK_INCOMPLETE) {
            RADRAY_WARN_LOG("vkGetPipelineCacheData failed: {}", result);
            return std::nullopt;
        }
    }
    RADRAY_WARN_LOG("vkGetPipelineCacheData remained incomplete after retries");
    return std::nullopt;
}

Nullable<CommandQueue*> DeviceVulkan::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    auto index = static_cast<std::underlying_type_t<QueueType>>(type);
    if (index >= static_cast<std::underlying_type_t<QueueType>>(QueueType::MAX_COUNT)) {
        return nullptr;
    }
    auto& queues = _queues[index];
    if (slot >= queues.size()) {
        return nullptr;
    }
    return queues[slot].get();
}

Nullable<unique_ptr<CommandBuffer>> DeviceVulkan::CreateCommandBuffer(CommandQueue* queue_) noexcept {
    auto queue = CastVkObject(queue_);
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.pNext = nullptr;
    poolInfo.flags = 0;
    poolInfo.queueFamilyIndex = queue->_family.Family;
    VkCommandPool pool{VK_NULL_HANDLE};
    if (auto vr = _ftb.vkCreateCommandPool(_device, &poolInfo, this->GetAllocationCallbacks(), &pool);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateCommandPool failed: {}", vr);
        return nullptr;
    }
    auto cmdPool = make_unique<CommandPoolVulkan>(this, pool);
    VkCommandBufferAllocateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bufferInfo.pNext = nullptr;
    bufferInfo.commandPool = cmdPool->_cmdPool;
    bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferInfo.commandBufferCount = 1;
    VkCommandBuffer cmdBuf{VK_NULL_HANDLE};
    if (auto vr = _ftb.vkAllocateCommandBuffers(_device, &bufferInfo, &cmdBuf);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkAllocateCommandBuffers failed: {}", vr);
        return nullptr;
    }
    return make_unique<CommandBufferVulkan>(this, queue, std::move(cmdPool), cmdBuf);
}

Nullable<unique_ptr<Fence>> DeviceVulkan::CreateFence() noexcept {
    unique_ptr<FenceVulkan> result;
    if (_extFeatures.feature12.timelineSemaphore) {
        auto timeline = this->CreateTimelineSemaphore(0);
        if (timeline.HasValue()) {
            result = make_unique<FenceVulkan>(this, timeline.Release());
        }
    } else {
        RADRAY_ABORT("vulkan unsupport timelineSemaphore");
    }
    return result;
}

Nullable<unique_ptr<QueryPool>> DeviceVulkan::CreateQueryPool(const QueryPoolDescriptor& desc) noexcept {
    if (desc.Count == 0) {
        RADRAY_ERR_LOG("vk QueryPoolDescriptor Count must be greater than 0");
        return nullptr;
    }
    if (desc.Type != QueryType::Timestamp) {
        RADRAY_ERR_LOG("vk query type is not supported: {}", static_cast<int32_t>(desc.Type));
        return nullptr;
    }

    VkQueryPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    createInfo.queryCount = desc.Count;
    createInfo.pipelineStatistics = 0;

    VkQueryPool pool{VK_NULL_HANDLE};
    if (auto vr = _ftb.vkCreateQueryPool(_device, &createInfo, this->GetAllocationCallbacks(), &pool);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateQueryPool failed: {}", vr);
        return nullptr;
    }

    auto result = make_unique<QueryPoolVulkan>(this, pool, desc);
    if (!desc.DebugName.empty()) {
        result->SetDebugName(desc.DebugName);
    }
    return result;
}

Nullable<unique_ptr<SwapChain>> DeviceVulkan::CreateSwapChain(const SwapChainDescriptor& desc_) noexcept {
    SwapChainDescriptor desc = desc_;
    unique_ptr<SurfaceVulkan> surface;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    {
        LPCWSTR instanceAddr = std::bit_cast<LPCWSTR>(&ShutdownVulkanEnvImpl);
        HMODULE hInstance;
        if (::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                instanceAddr,
                &hInstance) == 0) {
            RADRAY_ERR_LOG("GetModuleHandleExW failed: {}", GetLastError());
            return nullptr;
        }
        HWND hwnd = std::bit_cast<HWND>(desc.NativeHandler);
        VkWin32SurfaceCreateInfoKHR win32SurfaceInfo{};
        win32SurfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        win32SurfaceInfo.pNext = nullptr;
        win32SurfaceInfo.flags = 0;
        win32SurfaceInfo.hinstance = hInstance;
        win32SurfaceInfo.hwnd = hwnd;
        VkSurfaceKHR vkSurface;
        if (auto vr = vkCreateWin32SurfaceKHR(_instance->_instance, &win32SurfaceInfo, this->GetAllocationCallbacks(), &vkSurface);
            vr != VK_SUCCESS) {
            RADRAY_ERR_LOG("vkCreateWin32SurfaceKHR failed: {}", vr);
            return nullptr;
        }
        surface = make_unique<SurfaceVulkan>(this, vkSurface);
    }
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    {
        VkSurfaceKHR vkSurface = ::radray::CreateMacOSMetalSurface(_instance->_instance, std::bit_cast<void*>(desc.NativeHandler), this->GetAllocationCallbacks());
        if (vkSurface == VK_NULL_HANDLE) {
            RADRAY_ERR_LOG("vkCreateMetalSurfaceEXT failed");
            return nullptr;
        }
        surface = make_unique<SurfaceVulkan>(this, vkSurface);
    }
#else
// TODO: other platform surface creation
#endif
    if (surface == nullptr) {
        RADRAY_ERR_LOG("vk cannot create VkSurfaceKHR");
        return nullptr;
    }
    auto presentQueue = CastVkObject(desc.PresentQueue);
    VkExtent2D swapchainSize{};
    VkFormat rawFormat = VK_FORMAT_UNDEFINED;
    VkSwapchainKHR swapchain = _CreateVkSwapChain(this, surface.get(), desc, VK_NULL_HANDLE, swapchainSize, rawFormat);
    if (swapchain == VK_NULL_HANDLE) {
        return nullptr;
    }
    auto result = make_unique<SwapChainVulkan>(this, presentQueue, std::move(surface), swapchain, desc);
    if (!_RefreshSwapChainImages(result.get(), swapchainSize, rawFormat, desc.Format)) {
        return nullptr;
    }
    return result;
}

Nullable<unique_ptr<Buffer>> DeviceVulkan::CreateBuffer(const BufferDescriptor& desc) noexcept {
    const bool wantsMapRead = desc.Usage.HasFlag(BufferUse::MapRead);
    const bool wantsMapWrite = desc.Usage.HasFlag(BufferUse::MapWrite);
    const bool wantsPersistentMap = desc.Hints.HasFlag(ResourceHint::PersistentMap);
    if (wantsMapRead && wantsMapWrite) {
        RADRAY_ERR_LOG("vk buffer cannot be both map-read and map-write");
        return nullptr;
    }
    if (wantsMapRead && desc.Memory != MemoryType::ReadBack) {
        RADRAY_ERR_LOG("vk map-read buffer must use readback memory");
        return nullptr;
    }
    if (wantsMapWrite && desc.Memory != MemoryType::Upload) {
        RADRAY_ERR_LOG("vk map-write buffer must use upload memory");
        return nullptr;
    }
    if (wantsPersistentMap && !wantsMapRead && !wantsMapWrite) {
        RADRAY_ERR_LOG("vk persistent-map buffer must declare map-read or map-write usage");
        return nullptr;
    }
    const bool rtSupported = _detail.IsRayTracingSupported;
    if (!rtSupported &&
        (desc.Usage.HasFlag(BufferUse::AccelerationStructure) ||
         desc.Usage.HasFlag(BufferUse::Scratch) ||
         desc.Usage.HasFlag(BufferUse::ShaderTable))) {
        RADRAY_ERR_LOG("vk ray tracing buffer usage requested but ray tracing is not supported");
        return nullptr;
    }

    const uint64_t logicalSize = desc.Size;
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.pNext = nullptr;
    bufInfo.flags = 0;
    uint64_t allocSize = desc.Size;
    if (desc.Usage.HasFlag(BufferUse::CBuffer)) {
        const uint64_t align = std::max<uint64_t>(1, _detail.CBufferAlignment);
        allocSize = Align(allocSize, align);
    }
    bufInfo.size = allocSize;
    bufInfo.usage = 0;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bufInfo.queueFamilyIndexCount = 0;
    bufInfo.pQueueFamilyIndices = nullptr;
    if (desc.Usage.HasFlag(BufferUse::MapRead) || desc.Usage.HasFlag(BufferUse::CopySource)) {
        bufInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::MapWrite) || desc.Usage.HasFlag(BufferUse::CopyDestination)) {
        bufInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::Index)) {
        bufInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::Vertex)) {
        bufInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::CBuffer)) {
        bufInfo.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::Resource)) {
        bufInfo.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::UnorderedAccess)) {
        bufInfo.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::Indirect)) {
        bufInfo.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (rtSupported) {
        if (desc.Usage.HasFlag(BufferUse::Vertex) ||
            desc.Usage.HasFlag(BufferUse::Index) ||
            desc.Usage.HasFlag(BufferUse::Resource) ||
            desc.Usage.HasFlag(BufferUse::Scratch)) {
            bufInfo.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        if (desc.Usage.HasFlag(BufferUse::AccelerationStructure)) {
            bufInfo.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
        }
        if (desc.Usage.HasFlag(BufferUse::ShaderTable)) {
            bufInfo.usage |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
        }
    }
    if (_extFeatures.bufferDeviceAddress.bufferDeviceAddress) {
        if (desc.Usage.HasFlag(BufferUse::AccelerationStructure) ||
            desc.Usage.HasFlag(BufferUse::Scratch) ||
            desc.Usage.HasFlag(BufferUse::ShaderTable) ||
            desc.Usage.HasFlag(BufferUse::Vertex) ||
            desc.Usage.HasFlag(BufferUse::Index) ||
            desc.Usage.HasFlag(BufferUse::Resource)) {
            bufInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
    }
    if (desc.Usage.HasFlag(BufferUse::Scratch)) {
        bufInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    VmaAllocationCreateInfo vmaInfo{};
    vmaInfo.flags = 0;
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::MapWrite)) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::MapRead)) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    if (wantsPersistentMap) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    vmaInfo.usage = MapType(desc.Memory);
    vmaInfo.requiredFlags = 0;
    vmaInfo.preferredFlags = 0;
    vmaInfo.memoryTypeBits = 0;
    vmaInfo.pool = VK_NULL_HANDLE;
    vmaInfo.pUserData = nullptr;
    vmaInfo.priority = 0;
    VkBuffer vkBuf = VK_NULL_HANDLE;
    VmaAllocation vmaAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo vmaAllocInfo{};
    if (auto vr = vmaCreateBuffer(_vma->_vma, &bufInfo, &vmaInfo, &vkBuf, &vmaAlloc, &vmaAllocInfo);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vmaCreateBuffer failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<BufferVulkan>(this, vkBuf, vmaAlloc, vmaAllocInfo);
    result->_reqSize = bufInfo.size;
    result->_reqSizeLogical = logicalSize;
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints;
    VkMemoryPropertyFlags memoryFlags{};
    vmaGetMemoryTypeProperties(_vma->_vma, vmaAllocInfo.memoryType, &memoryFlags);
    result->_hostCoherent = (memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return result;
}

void DeviceVulkan::FlushMappedRanges(std::span<const MappedBufferRange> mappedRanges) noexcept {
    struct FlushRange {
        VmaAllocation Allocation{VK_NULL_HANDLE};
        VkDeviceSize Offset{0};
        VkDeviceSize Size{0};
    };

    vector<FlushRange> ranges;
    ranges.reserve(mappedRanges.size());
    const uint64_t atomSize = std::max<uint64_t>(1, _properties.limits.nonCoherentAtomSize);

    for (const MappedBufferRange& mappedRange : mappedRanges) {
        if (mappedRange.Target == nullptr) {
            RADRAY_ABORT("Vulkan mapped flush range has a null target");
        }
        if (mappedRange.Target->GetDevice() != this) {
            RADRAY_ABORT("cannot flush a Vulkan mapped range across devices");
        }
        const BufferDescriptor desc = mappedRange.Target->GetDesc();
        if (!desc.Usage.HasFlag(BufferUse::MapWrite)) {
            RADRAY_ABORT("Vulkan mapped flush requires MapWrite usage");
        }
        const uint64_t offset = mappedRange.Range.Offset;
        if (offset > desc.Size) {
            RADRAY_ABORT("Vulkan mapped flush offset is outside the buffer");
        }
        const uint64_t size = mappedRange.Range.Size == BufferRange::All()
                                  ? desc.Size - offset
                                  : mappedRange.Range.Size;
        if (size > desc.Size - offset) {
            RADRAY_ABORT("Vulkan mapped flush range is outside the buffer");
        }
        auto* buffer = CastVkObject(mappedRange.Target);
        if (!buffer->IsValid() || buffer->_allocation == VK_NULL_HANDLE) {
            RADRAY_ABORT("cannot flush an invalid Vulkan buffer allocation");
        }
        if (size == 0) {
            continue;
        }
        if (buffer->_hostCoherent) {
            continue;
        }
        const uint64_t begin = offset & ~(atomSize - 1);
        const uint64_t end = offset + size;
        const uint64_t allocationSize = buffer->_allocInfo.size;
        const uint64_t alignedEnd = std::min(
            allocationSize,
            end > std::numeric_limits<uint64_t>::max() - (atomSize - 1)
                ? allocationSize
                : Align(end, atomSize));
        ranges.push_back(FlushRange{
            .Allocation = buffer->_allocation,
            .Offset = begin,
            .Size = alignedEnd - begin});
    }

    std::sort(ranges.begin(), ranges.end(), [](const FlushRange& a, const FlushRange& b) noexcept {
        if (a.Allocation != b.Allocation) {
            return std::less<VmaAllocation>{}(a.Allocation, b.Allocation);
        }
        return a.Offset < b.Offset;
    });

    vector<FlushRange> merged;
    merged.reserve(ranges.size());
    for (const FlushRange& range : ranges) {
        if (!merged.empty()) {
            FlushRange& last = merged.back();
            const uint64_t lastEnd = last.Offset + last.Size;
            if (last.Allocation == range.Allocation && range.Offset <= lastEnd) {
                last.Size = std::max(lastEnd, range.Offset + range.Size) - last.Offset;
                continue;
            }
        }
        merged.push_back(range);
    }

    vector<VmaAllocation> allocations;
    vector<VkDeviceSize> offsets;
    vector<VkDeviceSize> sizes;
    allocations.reserve(merged.size());
    offsets.reserve(merged.size());
    sizes.reserve(merged.size());
    for (const FlushRange& range : merged) {
        allocations.push_back(range.Allocation);
        offsets.push_back(range.Offset);
        sizes.push_back(range.Size);
    }
    if (!allocations.empty()) {
        if (auto vr = vmaFlushAllocations(
                _vma->_vma,
                static_cast<uint32_t>(allocations.size()),
                allocations.data(),
                offsets.data(),
                sizes.data());
            vr != VK_SUCCESS) {
            RADRAY_ABORT("vmaFlushAllocations failed: {}", vr);
        }
    }
}

Nullable<unique_ptr<Texture>> DeviceVulkan::CreateTexture(const TextureDescriptor& desc) noexcept {
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.pNext = nullptr;
    imgInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    imgInfo.imageType = MapType(desc.Dim);
    imgInfo.format = MapType(desc.Format);
    imgInfo.extent.width = static_cast<uint32_t>(desc.Width);
    imgInfo.extent.height = static_cast<uint32_t>(desc.Height);
    // 仅 3D 纹理把 DepthOrArraySize 当作 extent.depth；2D/2DArray/Cube 等映射到 VK_IMAGE_TYPE_2D，
    // 其 extent.depth 必须为 1，层数只通过 arrayLayers 表达（否则触发 VUID-VkImageCreateInfo-extent-02254）。
    if (desc.Dim == TextureDimension::Dim3D) {
        imgInfo.extent.depth = static_cast<uint32_t>(desc.DepthOrArraySize);
    } else {
        imgInfo.extent.depth = 1;
    }
    imgInfo.mipLevels = desc.MipLevels;
    if (desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim3D) {
        imgInfo.arrayLayers = 1;
    } else {
        imgInfo.arrayLayers = desc.DepthOrArraySize;
    }
    imgInfo.samples = MapSampleCount(desc.SampleCount);
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = 0;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.queueFamilyIndexCount = 0;
    imgInfo.pQueueFamilyIndices = nullptr;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.Dim == TextureDimension::Cube || desc.Dim == TextureDimension::CubeArray) {
        imgInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::CopySource)) {
        imgInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::CopyDestination)) {
        imgInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::Resource)) {
        imgInfo.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::RenderTarget)) {
        imgInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::DepthStencilRead) || desc.Usage.HasFlag(TextureUse::DepthStencilWrite)) {
        imgInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::UnorderedAccess)) {
        imgInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    VmaAllocationCreateInfo vmaInfo{};
    vmaInfo.flags = 0;
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    vmaInfo.usage = MapType(desc.Memory);
    vmaInfo.requiredFlags = 0;
    vmaInfo.preferredFlags = 0;
    vmaInfo.memoryTypeBits = 0;
    vmaInfo.pool = VK_NULL_HANDLE;
    vmaInfo.pUserData = nullptr;
    vmaInfo.priority = 0;
    VkImage vkImg = VK_NULL_HANDLE;
    VmaAllocation vmaAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo vmaAllocInfo{};
    if (auto vr = vmaCreateImage(_vma->_vma, &imgInfo, &vmaInfo, &vkImg, &vmaAlloc, &vmaAllocInfo);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vmaCreateImage failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<ImageVulkan>(this, vkImg, vmaAlloc, vmaAllocInfo);
    result->_rawFormat = imgInfo.format;
    result->_dim = desc.Dim;
    result->_width = desc.Width;
    result->_height = desc.Height;
    result->_depthOrArraySize = desc.DepthOrArraySize;
    result->_mipLevels = desc.MipLevels;
    result->_sampleCount = desc.SampleCount;
    result->_format = desc.Format;
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints;
    return result;
}

static std::optional<SubresourceRange> _ResolveTextureViewArrayRangeVulkan(
    TextureDimension dim,
    SubresourceRange range,
    uint32_t targetArrayLayerCount) noexcept {
    auto resolveRemainingLayers = [&]() noexcept {
        if (range.ArrayLayerCount != SubresourceRange::All) {
            return true;
        }
        if (range.BaseArrayLayer >= targetArrayLayerCount) {
            RADRAY_ERR_LOG(
                "vk texture view base array layer {} has no remaining layers in target with {} layers",
                range.BaseArrayLayer,
                targetArrayLayerCount);
            return false;
        }
        range.ArrayLayerCount = targetArrayLayerCount - range.BaseArrayLayer;
        return true;
    };

    switch (dim) {
        case TextureDimension::Dim1D:
        case TextureDimension::Dim2D:
        case TextureDimension::Dim3D:
            if (range.BaseArrayLayer != 0 ||
                (range.ArrayLayerCount != 1 && range.ArrayLayerCount != SubresourceRange::All)) {
                RADRAY_ERR_LOG(
                    "vk {} texture view requires array range [0, 1] or all",
                    dim);
                return std::nullopt;
            }
            range.BaseArrayLayer = 0;
            range.ArrayLayerCount = 1;
            return range;
        case TextureDimension::Dim1DArray:
        case TextureDimension::Dim2DArray:
            if (!resolveRemainingLayers()) {
                return std::nullopt;
            }
            return range;
        case TextureDimension::Cube:
            if (!resolveRemainingLayers()) {
                return std::nullopt;
            }
            if (range.BaseArrayLayer != 0 || range.ArrayLayerCount != 6) {
                RADRAY_ERR_LOG("vk cube texture view requires array range [0, 6]");
                return std::nullopt;
            }
            return range;
        case TextureDimension::CubeArray:
            if (!resolveRemainingLayers()) {
                return std::nullopt;
            }
            if ((range.BaseArrayLayer % 6) != 0 ||
                range.ArrayLayerCount == 0 || (range.ArrayLayerCount % 6) != 0) {
                RADRAY_ERR_LOG(
                    "vk cube array texture view base layer and layer count must be multiples of 6");
                return std::nullopt;
            }
            return range;
        case TextureDimension::UNKNOWN:
            return range;
    }
    return range;
}

Nullable<unique_ptr<TextureView>> DeviceVulkan::CreateTextureView(const TextureViewDescriptor& desc) noexcept {
    auto image = CastVkObject(desc.Target);
    switch (desc.Usage) {
        case TextureViewUsage::Resource:
        case TextureViewUsage::RenderTarget:
        case TextureViewUsage::DepthRead:
        case TextureViewUsage::DepthWrite:
        case TextureViewUsage::UnorderedAccess:
            break;
        case TextureViewUsage::UNKNOWN:
        default:
            RADRAY_ERR_LOG("vk invalid texture view usage: {}", desc.Usage);
            return nullptr;
    }
    const uint32_t targetArrayLayerCount =
        image->_dim == TextureDimension::Dim1D || image->_dim == TextureDimension::Dim3D
            ? 1
            : image->_depthOrArraySize;
    auto rangeOpt = _ResolveTextureViewArrayRangeVulkan(
        desc.Dim, desc.Range, targetArrayLayerCount);
    if (!rangeOpt.has_value()) {
        return nullptr;
    }
    const SubresourceRange range = rangeOpt.value();
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.image = image->_image;
    createInfo.viewType = MapViewType(desc.Dim);
    createInfo.format = MapType(desc.Format);
    createInfo.components = VkComponentMapping{
        VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_A};
    createInfo.subresourceRange = {
        ImageFormatToAspectFlags(createInfo.format),
        desc.Range.BaseMipLevel,
        desc.Range.MipLevelCount == SubresourceRange::All ? VK_REMAINING_MIP_LEVELS : desc.Range.MipLevelCount,
        range.BaseArrayLayer,
        range.ArrayLayerCount};
    VkImageView imageView = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateImageView(_device, &createInfo, this->GetAllocationCallbacks(), &imageView);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateImageView failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<ImageViewVulkan>(this, image, imageView);
    result->_mdesc = desc;
    result->_rawFormat = createInfo.format;
    return result;
}

Nullable<unique_ptr<AccelerationStructureView>> DeviceVulkan::CreateAccelerationStructureView(const AccelerationStructureViewDescriptor& desc) noexcept {
    auto target = CastVkObject(desc.Target);
    if (!target->IsValid()) {
        RADRAY_ERR_LOG("vk acceleration structure view target is invalid");
        return nullptr;
    }
    auto result = make_unique<AccelerationStructureViewVulkan>(this, target);
    result->_desc = desc;
    return result;
}

Nullable<unique_ptr<Shader>> DeviceVulkan::CreateShader(const ShaderDescriptor& desc) noexcept {
    static_assert(sizeof(uint32_t) == (sizeof(byte) * 4), "byte size mismatch");
    if (desc.Category != ShaderBlobCategory::SPIRV) {
        RADRAY_ERR_LOG("vk only supported SPIR-V shader blobs");
        return nullptr;
    }
    if (desc.Reflection.has_value() && !std::holds_alternative<SpirvShaderDesc>(desc.Reflection.value())) {
        RADRAY_ERR_LOG("vk shader only accepts spirv reflection metadata");
        return nullptr;
    }
    if (desc.Source.size() % 4 != 0) {
        RADRAY_ERR_LOG("vk SPIR-V code byte size must be a multiple of 4: {}", desc.Source.size());
        return nullptr;
    }
    size_t realSize = desc.Source.size();
    const uint32_t* code = std::bit_cast<const uint32_t*>(desc.Source.data());
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.codeSize = realSize;
    createInfo.pCode = code;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateShaderModule(_device, &createInfo, this->GetAllocationCallbacks(), &shaderModule);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateShaderModule failed: {}", vr);
        return nullptr;
    }
    return make_unique<ShaderModuleVulkan>(this, shaderModule, desc.Stages, desc.Reflection);
}

Nullable<unique_ptr<PipelineLayoutVulkan>> DeviceVulkan::CreateRootSignatureInternal(const PipelineLayoutDescriptor& desc) noexcept {
    auto mergedOpt = BuildMergedPipelineLayoutVulkan(
        desc.Shaders, desc.BindingGroupLayouts);
    if (!mergedOpt.has_value()) {
        return nullptr;
    }
    auto merged = std::move(mergedOpt.value());
    const auto allParameters = std::span<const ShaderParameterInfo>{merged.Parameters};
    if (merged.VulkanParameters.size() != allParameters.size()) {
        RADRAY_ERR_LOG("internal error: merged parameter metadata size mismatch");
        return nullptr;
    }
    vector<PushConstantBinding> pushConstantBindings{
        desc.PushConstantBindings.begin(), desc.PushConstantBindings.end()};
    std::ranges::sort(
        pushConstantBindings,
        [](const PushConstantBinding& lhs, const PushConstantBinding& rhs) noexcept {
            return std::tie(lhs.Group, lhs.Binding) < std::tie(rhs.Group, rhs.Binding);
        });
    if (std::ranges::adjacent_find(pushConstantBindings) != pushConstantBindings.end()) {
        RADRAY_ERR_LOG("pipeline layout contains duplicate push constant bindings");
        return nullptr;
    }
    const size_t reflectedPushConstantCount = static_cast<size_t>(std::ranges::count_if(
        allParameters,
        [](const ShaderParameterInfo& parameter) noexcept {
            return parameter.Kind == ShaderParameterKind::Constant;
        }));
    if (reflectedPushConstantCount != pushConstantBindings.size()) {
        RADRAY_ERR_LOG(
            "pipeline layout declares {} push constant bindings, but SPIR-V reflection contains {} ranges",
            pushConstantBindings.size(),
            reflectedPushConstantCount);
        return nullptr;
    }
    auto staticSamplerSelectionOpt = _SelectStaticSamplersVulkan(
        allParameters,
        merged.VulkanParameters,
        desc.StaticSamplers);
    if (!staticSamplerSelectionOpt.has_value()) {
        return nullptr;
    }
    auto staticSamplerSelection = std::move(staticSamplerSelectionOpt.value());
    const auto parameters = allParameters;

    // Parameter bindings keep the merged reflection order as an internal index.
    vector<PipelineLayoutVulkan::ParameterBinding> parameterBindings(parameters.size());
    // 创建 VkDescriptorSetLayout 所需的原生数据 (仅构建期), 逐 set 收集.
    vector<vector<VkDescriptorSetLayoutBinding>> rawBindingsBySet(merged.DescriptorSetCount);
    vector<vector<DescriptorSetLayoutBindingVulkanContainer>> bindingContainersBySet(merged.DescriptorSetCount);
    vector<vector<VkDescriptorBindingFlags>> bindingFlagsBySet(merged.DescriptorSetCount);
    vector<vector<VkSampler>> immutableSamplerHandles{};
    vector<VkPushConstantRange> pushRanges{};
    pushRanges.reserve(parameters.size());
    vector<uint32_t> resourceCountsBySet(merged.DescriptorSetCount, 0);
    vector<uint32_t> samplerCountsBySet(merged.DescriptorSetCount, 0);

    auto isDynamicBuffer = [&](uint32_t setIndex, uint32_t bindingIndex) noexcept {
        const bool explicitlyDynamic = std::ranges::any_of(
            desc.BindingGroupLayouts,
            [&](const BindingGroupLayout& group) noexcept {
                return group.GroupIndex == setIndex && std::ranges::any_of(
                                                           group.Entries,
                                                           [&](const BindingGroupLayoutEntry& entry) noexcept {
                                                               return entry.Binding == bindingIndex && entry.HasDynamicOffset;
                                                           });
            });
        return explicitlyDynamic || std::ranges::any_of(
                                        desc.DynamicBufferBindings,
                                        [&](const DynamicBufferBinding& dynamicBinding) noexcept {
                                            return dynamicBinding.Group == setIndex && dynamicBinding.Binding == bindingIndex;
                                        });
    };

    size_t pushConstantIndex = 0;
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto& parameter = parameters[i];
        const auto& vkInfo = merged.VulkanParameters[i];
        auto& binding = parameterBindings[i];
        binding.Info = parameter;
        binding.ParameterIndex = static_cast<uint32_t>(i);
        binding.SetIndex = vkInfo.SetIndex;
        binding.BindingIndex = vkInfo.BindingIndex;
        if (parameter.Kind == ShaderParameterKind::Constant) {
            const PushConstantBinding& declared = pushConstantBindings[pushConstantIndex++];
            binding.SetIndex = declared.Group;
            binding.BindingIndex = declared.Binding;
            if (vkInfo.Size == 0 || (vkInfo.Offset % 4) != 0 || (vkInfo.Size % 4) != 0) {
                RADRAY_ERR_LOG("vk push constant '{}' must be 4-byte aligned and non-empty", parameter.Name);
                return nullptr;
            }
            if (vkInfo.Offset + vkInfo.Size > _properties.limits.maxPushConstantsSize) {
                RADRAY_ERR_LOG(
                    "vk push constant '{}' exceeds device limit {}",
                    parameter.Name,
                    _properties.limits.maxPushConstantsSize);
                return nullptr;
            }
            pushRanges.push_back(VkPushConstantRange{
                .stageFlags = MapType(parameter.Stages),
                .offset = vkInfo.Offset,
                .size = vkInfo.Size,
            });
            binding.PushConstantOffset = vkInfo.Offset;
            binding.PushConstantSize = vkInfo.Size;
            continue;
        }

        if (vkInfo.DescriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            RADRAY_ERR_LOG("vk lowering metadata is unavailable for '{}'", parameter.Name);
            return nullptr;
        }
        binding.DescriptorType = vkInfo.DescriptorType;
        binding.HasDynamicOffset = isDynamicBuffer(vkInfo.SetIndex, vkInfo.BindingIndex);
        if (binding.HasDynamicOffset) {
            if (parameter.Type != ResourceBindType::CBuffer ||
                vkInfo.DescriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                parameter.Count != 1) {
                RADRAY_ERR_LOG(
                    "vk dynamic binding set={} binding={} must be a single cbuffer",
                    vkInfo.SetIndex,
                    vkInfo.BindingIndex);
                return nullptr;
            }
            binding.DescriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        }
        binding.BindlessSlotType = vkInfo.BindlessSlotType;
        if (staticSamplerSelection.IsStaticParameter[i]) {
            auto staticSamplerIt = std::find_if(
                staticSamplerSelection.StaticSamplers.begin(),
                staticSamplerSelection.StaticSamplers.end(),
                [&](const _StaticSamplerBuildInfoVulkan& staticSampler) {
                    return staticSampler.ParameterIndex == i;
                });
            if (staticSamplerIt == staticSamplerSelection.StaticSamplers.end()) {
                RADRAY_ERR_LOG("internal error: static sampler metadata is unavailable for '{}'", parameter.Name);
                return nullptr;
            }
            auto samplerOpt = this->CreateSampler(staticSamplerIt->Desc);
            if (!samplerOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create immutable sampler for '{}'", parameter.Name);
                return nullptr;
            }
            vector<unique_ptr<SamplerVulkan>> immutableSamplers{};
            auto samplerBase = samplerOpt.Release();
            immutableSamplers.emplace_back(StaticCastUniquePtr<SamplerVulkan>(std::move(samplerBase)));
            immutableSamplerHandles.push_back(vector<VkSampler>{immutableSamplers[0]->_sampler});

            binding.IsStaticSampler = true;
            binding.DescriptorWriteOffset = 0;

            VkDescriptorSetLayoutBinding rawBinding{};
            rawBinding.binding = vkInfo.BindingIndex;
            rawBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            rawBinding.descriptorCount = 1;
            rawBinding.stageFlags = MapType(staticSamplerIt->Stages);
            rawBinding.pImmutableSamplers = immutableSamplerHandles.back().data();
            rawBindingsBySet[vkInfo.SetIndex].push_back(rawBinding);
            bindingContainersBySet[vkInfo.SetIndex].emplace_back(rawBinding, parameter.Type, std::move(immutableSamplers));
            bindingFlagsBySet[vkInfo.SetIndex].push_back(0);
            continue;
        }
        VkDescriptorSetLayoutBinding rawBinding{};
        rawBinding.binding = vkInfo.BindingIndex;
        rawBinding.descriptorType = binding.DescriptorType;
        rawBinding.descriptorCount = parameter.IsBindless ? kBindlessDescriptorCapacityVulkan : parameter.Count;
        rawBinding.stageFlags = MapType(parameter.Stages);
        rawBinding.pImmutableSamplers = nullptr;
        VkDescriptorBindingFlags bindingFlags = 0;
        if (parameter.IsBindless) {
            bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                           VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        }
        rawBindingsBySet[vkInfo.SetIndex].push_back(rawBinding);
        bindingContainersBySet[vkInfo.SetIndex].emplace_back(
            rawBinding,
            parameter.Type,
            vector<unique_ptr<SamplerVulkan>>{},
            bindingFlags);
        bindingFlagsBySet[vkInfo.SetIndex].push_back(bindingFlags);
        if (parameter.IsBindless) {
            binding.DescriptorWriteOffset = 0;
        } else if (parameter.Kind == ShaderParameterKind::Sampler) {
            binding.DescriptorWriteOffset = samplerCountsBySet[vkInfo.SetIndex];
            samplerCountsBySet[vkInfo.SetIndex] += parameter.Count;
        } else {
            binding.DescriptorWriteOffset = resourceCountsBySet[vkInfo.SetIndex];
            resourceCountsBySet[vkInfo.SetIndex] += parameter.Count;
        }
    }

    vector<DescriptorSetLayoutVulkan*> reusedLayouts(merged.DescriptorSetCount, nullptr);
    for (const BindingGroupLayoutReuse& reuse : desc.BindingGroupLayoutReuses) {
        if (reuse.Group >= merged.DescriptorSetCount || reuse.Source == nullptr) {
            RADRAY_ERR_LOG("vk binding group layout reuse target is invalid: group={}", reuse.Group);
            return nullptr;
        }
        if (reusedLayouts[reuse.Group] != nullptr) {
            RADRAY_ERR_LOG("vk binding group layout reuse target is duplicated: group={}", reuse.Group);
            return nullptr;
        }
        auto* source = CastVkObject(reuse.Source);
        if (source == nullptr || source->_device != this || !source->IsValid()) {
            RADRAY_ERR_LOG("vk binding group layout reuse source is invalid: group={}", reuse.Group);
            return nullptr;
        }
        auto sourceLayout = source->GetSetLayout(reuse.SourceGroup);
        if (!sourceLayout.HasValue() || sourceLayout.Get() == nullptr || !sourceLayout.Get()->IsValid()) {
            RADRAY_ERR_LOG(
                "vk binding group layout reuse source group is invalid: sourceGroup={}",
                reuse.SourceGroup);
            return nullptr;
        }
        reusedLayouts[reuse.Group] = sourceLayout.Get();
    }

    vector<unique_ptr<DescriptorSetLayoutVulkan>> ownedLayouts(merged.DescriptorSetCount);
    vector<DescriptorSetLayoutVulkan*> resolvedLayouts(merged.DescriptorSetCount, nullptr);
    vector<VkDescriptorSetLayout> setLayouts{};
    setLayouts.reserve(merged.DescriptorSetCount);
    for (uint32_t setIndex = 0; setIndex < merged.DescriptorSetCount; ++setIndex) {
        if (reusedLayouts[setIndex] != nullptr) {
            if (!_IsDescriptorSetLayoutCompatible(
                    *reusedLayouts[setIndex], bindingContainersBySet[setIndex])) {
                RADRAY_ERR_LOG(
                    "vk binding group layout reuse is incompatible: group={}",
                    setIndex);
                return nullptr;
            }
            resolvedLayouts[setIndex] = reusedLayouts[setIndex];
            setLayouts.push_back(reusedLayouts[setIndex]->_layout);
            continue;
        }
        auto layout = _CreateDescriptorSetLayoutVulkan(
            this,
            rawBindingsBySet[setIndex],
            std::move(bindingContainersBySet[setIndex]),
            bindingFlagsBySet[setIndex]);
        if (!layout) {
            return nullptr;
        }
        setLayouts.push_back(layout->_layout);
        resolvedLayouts[setIndex] = layout.get();
        ownedLayouts[setIndex] = std::move(layout);
    }

    VkPipelineLayoutCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    createInfo.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
    createInfo.pushConstantRangeCount = static_cast<uint32_t>(pushRanges.size());
    createInfo.pPushConstantRanges = pushRanges.empty() ? nullptr : pushRanges.data();
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreatePipelineLayout(_device, &createInfo, this->GetAllocationCallbacks(), &layout);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreatePipelineLayout failed: {}", vr);
        return nullptr;
    }

    auto result = make_unique<PipelineLayoutVulkan>(
        this,
        layout,
        std::move(parameterBindings),
        merged.DescriptorSetCount);
    result->_setLayouts = std::move(resolvedLayouts);
    result->_ownedLayouts = std::move(ownedLayouts);
    return result;
}

Nullable<unique_ptr<DescriptorSetVulkan>> DeviceVulkan::CreateDescriptorSetInternal(
    PipelineLayoutVulkan* layout,
    uint32_t setIndex,
    DescriptorSetAllocatorVulkan* allocator) noexcept {
    if (layout == nullptr) {
        RADRAY_ERR_LOG("root signature is null");
        return nullptr;
    }
    if (layout == nullptr || !layout->IsValid()) {
        RADRAY_ERR_LOG("root signature is invalid");
        return nullptr;
    }
    if (layout->HasBindlessSet(setIndex)) {
        RADRAY_ERR_LOG("descriptor set {} is declared as a bindless set", setIndex);
        return nullptr;
    }
    if (setIndex >= layout->GetDescriptorSetCount()) {
        RADRAY_ERR_LOG("descriptor set {} is out of range", setIndex);
        return nullptr;
    }
    auto setLayout = layout->GetSetLayout(setIndex);
    if (!setLayout.HasValue() || setLayout.Get() == nullptr) {
        RADRAY_ERR_LOG("internal error: vk set layout {} is unavailable", setIndex);
        return nullptr;
    }
    allocator = allocator != nullptr ? allocator : _descSetAlloc.get();
    auto allocOpt = allocator->Allocate(setLayout.Get());
    if (!allocOpt.has_value()) {
        RADRAY_ERR_LOG("failed to allocate vk descriptor set for set {}", setIndex);
        return nullptr;
    }

    uint32_t resourceDescriptorCount = 0;
    uint32_t samplerDescriptorCount = 0;
    for (const auto& binding : layout->GetParameterBindings()) {
        // 只统计该 set 下的非静态采样器/非 bindless/非 push constant 参数.
        if (binding.IsStaticSampler || binding.Info.IsBindless ||
            binding.Info.Kind == ShaderParameterKind::Constant ||
            binding.SetIndex != setIndex) {
            continue;
        }
        const uint32_t count = binding.DescriptorWriteOffset + binding.Info.Count;
        if (binding.Info.Kind == ShaderParameterKind::Sampler) {
            samplerDescriptorCount = std::max(samplerDescriptorCount, count);
        } else {
            resourceDescriptorCount = std::max(resourceDescriptorCount, count);
        }
    }

    auto result = make_unique<DescriptorSetVulkan>(this, layout, setIndex, setLayout.Get(), allocator, allocOpt.value());
    result->_resourceWritten.resize(resourceDescriptorCount, 0);
    result->_samplerWritten.resize(samplerDescriptorCount, 0);
    return result;
}

Nullable<unique_ptr<PipelineLayout>> DeviceVulkan::CreatePipelineLayout(const PipelineLayoutDescriptor& desc) noexcept {
    auto layout = CreateRootSignatureInternal(desc);
    if (!layout.HasValue()) {
        return nullptr;
    }
    return unique_ptr<PipelineLayout>{layout.Release()};
}

static DescriptorPoolDescriptor _GetBindingGroupPoolUsage(
    PipelineLayout* layout,
    uint32_t groupIndex) noexcept {
    DescriptorPoolDescriptor usage{};
    for (const BindingGroupLayout& group : layout->GetBindingGroupLayouts()) {
        if (group.GroupIndex != groupIndex) {
            continue;
        }
        for (const BindingGroupLayoutEntry& entry : group.Entries) {
            if (entry.IsStaticSampler || entry.Parameter.IsBindless) {
                continue;
            }
            const uint32_t count = entry.Parameter.Count;
            switch (entry.Parameter.Type) {
                case ResourceBindType::CBuffer:
                    if (entry.HasDynamicOffset) {
                        usage.MaxDynamicUniformBuffers += count;
                    } else {
                        usage.MaxUniformBuffers += count;
                    }
                    break;
                case ResourceBindType::Buffer:
                case ResourceBindType::RWBuffer:
                    usage.MaxStorageBuffers += count;
                    break;
                case ResourceBindType::TexelBuffer:
                    usage.MaxReadOnlyTexelBuffers += count;
                    break;
                case ResourceBindType::RWTexelBuffer:
                    usage.MaxReadWriteTexelBuffers += count;
                    break;
                case ResourceBindType::Texture:
                    usage.MaxSampledTextures += count;
                    break;
                case ResourceBindType::RWTexture:
                    usage.MaxStorageTextures += count;
                    break;
                case ResourceBindType::Sampler:
                    usage.MaxSamplers += count;
                    break;
                case ResourceBindType::AccelerationStructure:
                    usage.MaxAccelerationStructures += count;
                    break;
                case ResourceBindType::UNKNOWN:
                    break;
            }
        }
        break;
    }
    return usage;
}

Nullable<unique_ptr<DescriptorPool>> DeviceVulkan::CreateDescriptorPool(
    const DescriptorPoolDescriptor& desc) noexcept {
    if (desc.MaxBindingGroups == 0) {
        RADRAY_ERR_LOG("vk descriptor pool MaxBindingGroups must be greater than zero");
        return nullptr;
    }
    vector<VkDescriptorPoolSize> sizes{};
    const auto add = [&sizes](VkDescriptorType type, uint32_t count) {
        if (count > 0) {
            sizes.push_back(VkDescriptorPoolSize{type, count});
        }
    };
    add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, desc.MaxSampledTextures);
    add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, desc.MaxStorageTextures);
    add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, desc.MaxUniformBuffers);
    add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, desc.MaxDynamicUniformBuffers);
    add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, desc.MaxStorageBuffers);
    add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, desc.MaxReadOnlyTexelBuffers);
    add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, desc.MaxReadWriteTexelBuffers);
    add(VK_DESCRIPTOR_TYPE_SAMPLER, desc.MaxSamplers);
    if (_detail.IsRayTracingSupported) {
        add(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, desc.MaxAccelerationStructures);
    }
    auto allocator = make_unique<DescriptorSetAllocatorVulkan>(
        this,
        1,
        std::move(sizes),
        desc.MaxBindingGroups,
        desc.MaxBindingGroups,
        1,
        true);
    return make_unique<BindingDescriptorPoolVulkan>(this, desc, std::move(allocator));
}

Nullable<unique_ptr<BindingGroup>> DeviceVulkan::CreateBindingGroup(
    DescriptorPool* pool_,
    PipelineLayout* layout_,
    uint32_t groupIndex) noexcept {
    auto* pool = CastVkObject(pool_);
    auto* layout = CastVkObject(layout_);
    if (pool == nullptr || !pool->IsValid()) {
        RADRAY_ERR_LOG("vk descriptor pool is invalid");
        return nullptr;
    }
    if (layout == nullptr || !layout->IsValid()) {
        RADRAY_ERR_LOG("vk binding group layout is invalid");
        return nullptr;
    }
    if (groupIndex >= layout->GetDescriptorSetCount()) {
        RADRAY_ERR_LOG(
            "vk binding group index out of range expected: {}, actual: {}",
            layout->GetDescriptorSetCount(),
            groupIndex);
        return nullptr;
    }
    const DescriptorPoolDescriptor poolUsage = _GetBindingGroupPoolUsage(layout, groupIndex);
    if (!pool->ReserveGroup(poolUsage)) {
        return nullptr;
    }

    unique_ptr<DescriptorSetVulkan> descriptorSet{};
    if (!layout->HasBindlessSet(groupIndex)) {
        auto setLayout = layout->GetSetLayout(groupIndex);
        if (!setLayout.HasValue() || setLayout.Get() == nullptr || setLayout.Get()->_bindings.empty()) {
            RADRAY_ERR_LOG("vk binding group {} has no descriptor bindings", groupIndex);
            pool->ReleaseGroup(poolUsage);
            return nullptr;
        }
        auto setOpt = CreateDescriptorSetInternal(layout, groupIndex, pool->GetAllocator());
        if (!setOpt.HasValue()) {
            pool->ReleaseGroup(poolUsage);
            return nullptr;
        }
        descriptorSet = setOpt.Release();
    }

    return make_unique<BindingGroupVulkan>(
        this,
        pool,
        layout,
        groupIndex,
        std::move(descriptorSet),
        poolUsage);
}

Nullable<unique_ptr<GraphicsPipelineState>> DeviceVulkan::CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept {
    if (desc.PipelineLayout == nullptr) {
        RADRAY_ERR_LOG("GraphicsPipelineStateDescriptor.PipelineLayout is null");
        return nullptr;
    }
    if (desc.Primitive.StripIndexFormat.has_value() &&
        desc.Primitive.Topology != PrimitiveTopology::LineStrip &&
        desc.Primitive.Topology != PrimitiveTopology::TriangleStrip) {
        RADRAY_ERR_LOG("StripIndexFormat is only valid for LineStrip or TriangleStrip topology");
        return nullptr;
    }
    if (desc.Primitive.Poly == PolygonMode::Point && !_feature.fillModeNonSolid) {
        RADRAY_ERR_LOG("vk point polygon mode requires fillModeNonSolid feature");
        return nullptr;
    }
    vector<VkPipelineShaderStageCreateInfo> shaderStages;
    vector<string> entryPointsOwned;
    {
        struct ShaderWithStage {
            std::optional<ShaderEntry> shader;
            VkShaderStageFlagBits stage;
        };
        ShaderWithStage ss[] = {
            {desc.VS, VK_SHADER_STAGE_VERTEX_BIT},
            {desc.PS, VK_SHADER_STAGE_FRAGMENT_BIT}};
        size_t cnt = 0;
        for (const auto& i : ss) {
            if (i.shader.has_value()) cnt++;
        }
        shaderStages.reserve(cnt);
        entryPointsOwned.reserve(cnt);
        for (const auto& i : ss) {
            if (!i.shader.has_value()) continue;
            const auto& src = i.shader.value();
            if (src.EntryPoint.empty()) {
                RADRAY_ERR_LOG("vk graphics shader entry point is empty");
                return nullptr;
            }
            auto shaderVulkan = CastVkObject(src.Target);
            entryPointsOwned.emplace_back(src.EntryPoint);
            auto& stage = shaderStages.emplace_back();
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pNext = nullptr;
            stage.flags = 0;
            stage.stage = i.stage;
            stage.module = shaderVulkan->_shaderModule;
            stage.pName = entryPointsOwned.back().c_str();
            stage.pSpecializationInfo = nullptr;
        }
    }
    vector<VkVertexInputBindingDescription> vertexInputBindings;
    vector<VkVertexInputAttributeDescription> vertexInputAttributes;
    {
        vertexInputBindings.reserve(desc.VertexLayouts.size());
        for (size_t i = 0; i < desc.VertexLayouts.size(); i++) {
            const auto& vbl = desc.VertexLayouts[i];
            auto& bindingDesc = vertexInputBindings.emplace_back();
            bindingDesc.binding = static_cast<uint32_t>(i);
            bindingDesc.stride = static_cast<uint32_t>(vbl.ArrayStride);
            bindingDesc.inputRate = MapType(vbl.StepMode);
            for (auto&& j : vbl.Elements) {
                auto& attributeDesc = vertexInputAttributes.emplace_back();
                attributeDesc.location = static_cast<uint32_t>(j.Location);
                attributeDesc.binding = bindingDesc.binding;
                attributeDesc.format = MapType(j.Format);
                attributeDesc.offset = static_cast<uint32_t>(j.Offset);
            }
        }
    }
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.pNext = nullptr;
    vertexInputInfo.flags = 0;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size());
    vertexInputInfo.pVertexBindingDescriptions = vertexInputBindings.empty() ? nullptr : vertexInputBindings.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
    vertexInputInfo.pVertexAttributeDescriptions = vertexInputAttributes.empty() ? nullptr : vertexInputAttributes.data();
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.pNext = nullptr;
    inputAssemblyInfo.flags = 0;
    inputAssemblyInfo.topology = MapType(desc.Primitive.Topology);
    inputAssemblyInfo.primitiveRestartEnable = desc.Primitive.StripIndexFormat.has_value() ? VK_TRUE : VK_FALSE;
    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.pNext = nullptr;
    viewportInfo.flags = 0;
    viewportInfo.viewportCount = 1;
    viewportInfo.pViewports = nullptr;
    viewportInfo.scissorCount = 1;
    viewportInfo.pScissors = nullptr;
    VkPipelineRasterizationStateCreateInfo rasterInfo{};
    rasterInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterInfo.pNext = nullptr;
    rasterInfo.flags = 0;
    rasterInfo.depthClampEnable = desc.Primitive.UnclippedDepth;
    rasterInfo.rasterizerDiscardEnable = VK_FALSE;
    rasterInfo.polygonMode = MapType(desc.Primitive.Poly);
    rasterInfo.cullMode = MapType(desc.Primitive.Cull);
    rasterInfo.frontFace = MapType(desc.Primitive.FaceClockwise);
    rasterInfo.depthBiasEnable = VK_FALSE;
    rasterInfo.depthBiasConstantFactor = 0.0f;
    rasterInfo.depthBiasClamp = 0.0f;
    rasterInfo.depthBiasSlopeFactor = 0.0f;
    rasterInfo.lineWidth = 1.0f;
    VkPipelineRasterizationConservativeStateCreateInfoEXT rasterConservativeInfo{};
    if (desc.Primitive.Conservative && _extProperties.conservativeRasterization.has_value()) {
        rasterConservativeInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT;
        rasterConservativeInfo.pNext = nullptr;
        rasterConservativeInfo.flags = 0;
        rasterConservativeInfo.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
        rasterConservativeInfo.extraPrimitiveOverestimationSize = 0.0f;
        AddToHeadVulkanStruct(rasterInfo, rasterConservativeInfo);
    }
    VkPipelineDepthStencilStateCreateInfo depthStencilInfo{};
    depthStencilInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilInfo.pNext = nullptr;
    depthStencilInfo.flags = 0;
    depthStencilInfo.depthTestEnable = VK_FALSE;
    depthStencilInfo.stencilTestEnable = VK_FALSE;
    if (desc.DepthStencil.has_value()) {
        const auto& ds = desc.DepthStencil.value();
        if (ds.DepthBias.Constant != 0.0f || ds.DepthBias.SlopScale != 0.0f) {
            rasterInfo.depthBiasEnable = VK_TRUE;
            rasterInfo.depthBiasConstantFactor = (float)ds.DepthBias.Constant;
            rasterInfo.depthBiasClamp = ds.DepthBias.Clamp;
            rasterInfo.depthBiasSlopeFactor = ds.DepthBias.SlopScale;
        }
        const bool hardwareDepthEnable = ds.DepthTestEnable || ds.DepthWriteEnable;
        depthStencilInfo.depthTestEnable = hardwareDepthEnable ? VK_TRUE : VK_FALSE;
        depthStencilInfo.depthWriteEnable = ds.DepthWriteEnable ? VK_TRUE : VK_FALSE;
        depthStencilInfo.depthCompareOp = ds.DepthTestEnable ? MapType(ds.DepthCompare) : VK_COMPARE_OP_ALWAYS;
        depthStencilInfo.depthBoundsTestEnable = VK_FALSE;
        depthStencilInfo.minDepthBounds = 0.0f;
        depthStencilInfo.maxDepthBounds = 0.0f;
        if (ds.Stencil.has_value()) {
            const auto& stencil = ds.Stencil.value();
            depthStencilInfo.stencilTestEnable = VK_TRUE;
            depthStencilInfo.front = MapType(stencil.Front, stencil.ReadMask, stencil.WriteMask);
            depthStencilInfo.back = MapType(stencil.Back, stencil.ReadMask, stencil.WriteMask);
        }
    }
    VkSampleMask sampleMask[2] = {
        static_cast<VkSampleMask>(desc.MultiSample.Mask & 0xFFFFFFFF),
        static_cast<VkSampleMask>((desc.MultiSample.Mask >> 32) & 0xFFFFFFFF)};
    VkPipelineMultisampleStateCreateInfo multisampleInfo{};
    multisampleInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleInfo.pNext = nullptr;
    multisampleInfo.flags = 0;
    multisampleInfo.rasterizationSamples = MapSampleCount(desc.MultiSample.Count);
    multisampleInfo.sampleShadingEnable = VK_FALSE;
    multisampleInfo.minSampleShading = 0.0f;
    multisampleInfo.pSampleMask = sampleMask;
    multisampleInfo.alphaToCoverageEnable = desc.MultiSample.AlphaToCoverageEnable ? VK_TRUE : VK_FALSE;
    multisampleInfo.alphaToOneEnable = VK_FALSE;
    vector<VkPipelineColorBlendAttachmentState> blendAttachments;
    for (const auto& target : desc.ColorTargets) {
        auto& blend = blendAttachments.emplace_back();
        blend.blendEnable = target.Blend.has_value() ? VK_TRUE : VK_FALSE;
        blend.colorWriteMask = MapType(target.WriteMask);
        if (target.Blend.has_value()) {
            const auto& b = target.Blend.value();
            auto [colorOp, colorSrc, colorDst] = MapType(b.Color);
            auto [alphaOp, alphaSrc, alphaDst] = MapType(b.Alpha);
            blend.colorBlendOp = colorOp;
            blend.srcColorBlendFactor = colorSrc;
            blend.dstColorBlendFactor = colorDst;
            blend.alphaBlendOp = alphaOp;
            blend.srcAlphaBlendFactor = alphaSrc;
            blend.dstAlphaBlendFactor = alphaDst;
        }
    }
    VkPipelineColorBlendStateCreateInfo blendInfo{};
    blendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendInfo.pNext = nullptr;
    blendInfo.flags = 0;
    blendInfo.logicOpEnable = VK_FALSE;
    blendInfo.logicOp = VK_LOGIC_OP_COPY;
    blendInfo.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    blendInfo.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();
    blendInfo.blendConstants[0] = 0.0f;
    blendInfo.blendConstants[1] = 0.0f;
    blendInfo.blendConstants[2] = 0.0f;
    blendInfo.blendConstants[3] = 0.0f;
    vector<VkDynamicState> dynStates;
    dynStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    dynStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
    dynStates.emplace_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
    dynStates.emplace_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
    VkPipelineDynamicStateCreateInfo dynStateInfo{};
    dynStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynStateInfo.pNext = nullptr;
    dynStateInfo.flags = 0;
    dynStateInfo.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynStateInfo.pDynamicStates = dynStates.empty() ? nullptr : dynStates.data();
    if (desc.CompatibleRenderPass == nullptr ||
        !IsGraphicsPipelineCompatibleWithRenderPass(desc, *desc.CompatibleRenderPass)) {
        RADRAY_ERR_LOG("vk graphics pipeline requires a compatible explicit render pass");
        return nullptr;
    }
    auto* renderPass = CastVkObject(desc.CompatibleRenderPass);
    auto rs = CastVkObject(desc.PipelineLayout);
    VkGraphicsPipelineCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    createInfo.pStages = shaderStages.empty() ? nullptr : shaderStages.data();
    createInfo.pVertexInputState = &vertexInputInfo;
    createInfo.pInputAssemblyState = &inputAssemblyInfo;
    createInfo.pTessellationState = nullptr;
    createInfo.pViewportState = &viewportInfo;
    createInfo.pRasterizationState = &rasterInfo;
    createInfo.pMultisampleState = &multisampleInfo;
    createInfo.pDepthStencilState = &depthStencilInfo;
    createInfo.pColorBlendState = &blendInfo;
    createInfo.pDynamicState = &dynStateInfo;
    createInfo.layout = rs->_layout;
    createInfo.renderPass = renderPass->_renderPass;
    createInfo.subpass = 0;
    createInfo.basePipelineHandle = VK_NULL_HANDLE;
    createInfo.basePipelineIndex = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateGraphicsPipelines(_device, _pipelineCache, 1, &createInfo, this->GetAllocationCallbacks(), &pipeline);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateGraphicsPipelines failed: {}", vr);
        return nullptr;
    }
    return make_unique<GraphicsPipelineVulkan>(this, pipeline);
}

Nullable<unique_ptr<ComputePipelineState>> DeviceVulkan::CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept {
    if (desc.PipelineLayout == nullptr) {
        RADRAY_ERR_LOG("ComputePipelineStateDescriptor.PipelineLayout is null");
        return nullptr;
    }
    auto rs = CastVkObject(desc.PipelineLayout);
    auto cs = CastVkObject(desc.CS.Target);
    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = cs->_shaderModule;
    string entryPoint{desc.CS.EntryPoint};
    stageInfo.pName = entryPoint.c_str();
    VkComputePipelineCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    createInfo.stage = stageInfo;
    createInfo.layout = rs->_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &createInfo, this->GetAllocationCallbacks(), &pipeline);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateComputePipelines failed: {}", vr);
        return nullptr;
    }
    return make_unique<ComputePipelineVulkan>(this, pipeline);
}

Nullable<unique_ptr<AccelerationStructure>> DeviceVulkan::CreateAccelerationStructure(const AccelerationStructureDescriptor& desc) noexcept {
    if (!this->GetDetail().IsRayTracingSupported) {
        RADRAY_ERR_LOG("vk ray tracing acceleration structure is not supported by this device");
        return nullptr;
    }
    uint64_t estimatedSize = 0;
    if (desc.Type == AccelerationStructureType::BottomLevel) {
        estimatedSize = std::max<uint64_t>(1ull << 20, uint64_t(std::max(1u, desc.MaxGeometryCount)) * (2ull << 20));
    } else {
        estimatedSize = std::max<uint64_t>(1ull << 20, uint64_t(std::max(1u, desc.MaxInstanceCount)) * 4096ull);
    }
    uint64_t asAlignment = std::max<uint64_t>(256, this->GetDetail().AccelerationStructureAlignment);
    estimatedSize = Align(estimatedSize, asAlignment);

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = estimatedSize;
    bufInfo.usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkBuffer vkBuf = VK_NULL_HANDLE;
    VmaAllocation vmaAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo vmaAllocInfo{};
    if (auto vr = vmaCreateBuffer(_vma->_vma, &bufInfo, &allocInfo, &vkBuf, &vmaAlloc, &vmaAllocInfo);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vmaCreateBuffer (AS) failed: {}", vr);
        return nullptr;
    }
    VkAccelerationStructureCreateInfoKHR asInfo{};
    asInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asInfo.buffer = vkBuf;
    asInfo.offset = 0;
    asInfo.size = estimatedSize;
    asInfo.type = desc.Type == AccelerationStructureType::BottomLevel
                      ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
                      : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateAccelerationStructureKHR(_device, &asInfo, this->GetAllocationCallbacks(), &as);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateAccelerationStructureKHR failed: {}", vr);
        vmaDestroyBuffer(_vma->_vma, vkBuf, vmaAlloc);
        return nullptr;
    }

    auto result = make_unique<AccelerationStructureVulkan>(this, vkBuf, vmaAlloc, vmaAllocInfo, as, desc, estimatedSize);
    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = as;
    result->_deviceAddress = _ftb.vkGetAccelerationStructureDeviceAddressKHR(_device, &addrInfo);
    if (result->_deviceAddress == 0) {
        RADRAY_ERR_LOG("vkGetAccelerationStructureDeviceAddressKHR returned 0");
        return nullptr;
    }
    return result;
}

Nullable<unique_ptr<RayTracingPipelineState>> DeviceVulkan::CreateRayTracingPipelineState(const RayTracingPipelineStateDescriptor& desc) noexcept {
    if (!this->GetDetail().IsRayTracingSupported) {
        RADRAY_ERR_LOG("vk ray tracing pipeline state is not supported by this device");
        return nullptr;
    }
    if (desc.PipelineLayout == nullptr) {
        RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor.PipelineLayout is null");
        return nullptr;
    }
    if (desc.ShaderEntries.empty()) {
        RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor.ShaderEntries is empty");
        return nullptr;
    }
    if (desc.MaxRecursionDepth == 0 || desc.MaxRecursionDepth > this->GetDetail().MaxRayRecursionDepth) {
        RADRAY_ERR_LOG("invalid MaxRecursionDepth {} (device max={})", desc.MaxRecursionDepth, this->GetDetail().MaxRayRecursionDepth);
        return nullptr;
    }

    auto rs = CastVkObject(desc.PipelineLayout);
    vector<string> entryNames{};
    entryNames.reserve(desc.ShaderEntries.size());
    vector<VkPipelineShaderStageCreateInfo> stages{};
    stages.reserve(desc.ShaderEntries.size());
    unordered_map<string, uint32_t> stageIndexByName{};
    unordered_map<string, ShaderStage> stageKindByName{};

    auto toVkRtStage = [](ShaderStage s) -> VkShaderStageFlagBits {
        switch (s) {
            case ShaderStage::RayGen: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            case ShaderStage::Miss: return VK_SHADER_STAGE_MISS_BIT_KHR;
            case ShaderStage::ClosestHit: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            case ShaderStage::AnyHit: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            case ShaderStage::Intersection: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            case ShaderStage::Callable: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
            default: return VK_SHADER_STAGE_ALL;
        }
    };
    for (const auto& entry : desc.ShaderEntries) {
        if (entry.Target == nullptr) {
            RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor contains null shader entry target");
            return nullptr;
        }
        auto vkStage = toVkRtStage(entry.Stage);
        if (vkStage == VK_SHADER_STAGE_ALL) {
            RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor shader entry has non-RT stage {}", entry.Stage);
            return nullptr;
        }
        string ep{entry.EntryPoint};
        if (ep.empty()) {
            RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor shader entry has empty entry point");
            return nullptr;
        }
        if (stageIndexByName.contains(ep)) {
            RADRAY_ERR_LOG("duplicated RT shader entry '{}'", ep);
            return nullptr;
        }
        entryNames.push_back(ep);
        auto sm = CastVkObject(entry.Target);
        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = vkStage;
        stageInfo.module = sm->_shaderModule;
        stageInfo.pName = entryNames.back().c_str();
        stageInfo.pSpecializationInfo = nullptr;
        stageIndexByName.emplace(ep, static_cast<uint32_t>(stages.size()));
        stageKindByName.emplace(ep, entry.Stage);
        stages.push_back(stageInfo);
    }

    vector<VkRayTracingShaderGroupCreateInfoKHR> groups{};
    groups.reserve(desc.ShaderEntries.size() + desc.HitGroups.size());
    unordered_map<string, uint32_t> groupIndices{};
    for (const auto& entry : desc.ShaderEntries) {
        if (entry.Stage == ShaderStage::ClosestHit ||
            entry.Stage == ShaderStage::AnyHit ||
            entry.Stage == ShaderStage::Intersection) {
            continue;
        }
        VkRayTracingShaderGroupCreateInfoKHR group{};
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        group.generalShader = stageIndexByName[string(entry.EntryPoint)];
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
        groupIndices.emplace(string(entry.EntryPoint), static_cast<uint32_t>(groups.size()));
        groups.push_back(group);
    }
    for (const auto& hg : desc.HitGroups) {
        string groupName{hg.Name};
        if (groupName.empty()) {
            RADRAY_ERR_LOG("RayTracingHitGroupDescriptor.Name is empty");
            return nullptr;
        }
        VkRayTracingShaderGroupCreateInfoKHR group{};
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
        bool hasIntersection = false;
        if (hg.ClosestHit.has_value()) {
            auto it = stageIndexByName.find(string(hg.ClosestHit->EntryPoint));
            if (it == stageIndexByName.end()) {
                RADRAY_ERR_LOG("hit group '{}' references missing ClosestHit '{}'", groupName, hg.ClosestHit->EntryPoint);
                return nullptr;
            }
            group.closestHitShader = it->second;
        }
        if (hg.AnyHit.has_value()) {
            auto it = stageIndexByName.find(string(hg.AnyHit->EntryPoint));
            if (it == stageIndexByName.end()) {
                RADRAY_ERR_LOG("hit group '{}' references missing AnyHit '{}'", groupName, hg.AnyHit->EntryPoint);
                return nullptr;
            }
            group.anyHitShader = it->second;
        }
        if (hg.Intersection.has_value()) {
            auto it = stageIndexByName.find(string(hg.Intersection->EntryPoint));
            if (it == stageIndexByName.end()) {
                RADRAY_ERR_LOG("hit group '{}' references missing Intersection '{}'", groupName, hg.Intersection->EntryPoint);
                return nullptr;
            }
            group.intersectionShader = it->second;
            hasIntersection = true;
        }
        group.type = hasIntersection ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
                                     : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groupIndices.emplace(groupName, static_cast<uint32_t>(groups.size()));
        groups.push_back(group);
    }

    VkRayTracingPipelineCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    createInfo.stageCount = static_cast<uint32_t>(stages.size());
    createInfo.pStages = stages.data();
    createInfo.groupCount = static_cast<uint32_t>(groups.size());
    createInfo.pGroups = groups.data();
    createInfo.maxPipelineRayRecursionDepth = desc.MaxRecursionDepth;
    createInfo.layout = rs->_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateRayTracingPipelinesKHR(_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &createInfo, this->GetAllocationCallbacks(), &pipeline);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateRayTracingPipelinesKHR failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<RayTracingPipelineVulkan>(this, pipeline, rs);
    result->_groupIndices = std::move(groupIndices);
    result->_groupCount = static_cast<uint32_t>(groups.size());
    return result;
}

Nullable<unique_ptr<ShaderBindingTable>> DeviceVulkan::CreateShaderBindingTable(const ShaderBindingTableDescriptor& desc) noexcept {
    if (!this->GetDetail().IsRayTracingSupported) {
        RADRAY_ERR_LOG("vk shader binding table is not supported by this device");
        return nullptr;
    }
    if (desc.Pipeline == nullptr) {
        RADRAY_ERR_LOG("ShaderBindingTableDescriptor.Pipeline is null");
        return nullptr;
    }
    if (desc.RayGenCount != 1) {
        RADRAY_ERR_LOG("ShaderBindingTableDescriptor.RayGenCount must be exactly 1");
        return nullptr;
    }
    auto* pipeline = CastVkObject(desc.Pipeline);
    auto req = pipeline->GetShaderBindingTableRequirements();
    if (req.HandleSize == 0 || req.BaseAlignment == 0) {
        RADRAY_ERR_LOG("invalid Vulkan ray tracing pipeline SBT requirements");
        return nullptr;
    }
    uint64_t recordSize = static_cast<uint64_t>(req.HandleSize) + static_cast<uint64_t>(desc.MaxLocalDataSize);
    uint64_t recordStride = Align(recordSize, static_cast<uint64_t>(req.BaseAlignment));
    uint64_t totalRecords = static_cast<uint64_t>(desc.RayGenCount) +
                            static_cast<uint64_t>(desc.MissCount) +
                            static_cast<uint64_t>(desc.HitGroupCount) +
                            static_cast<uint64_t>(desc.CallableCount);
    if (totalRecords == 0) {
        RADRAY_ERR_LOG("ShaderBindingTableDescriptor has no records");
        return nullptr;
    }
    uint64_t totalSize = recordStride * totalRecords;
    auto buffer = this->CreateBuffer(BufferDescriptor{
        .Size = totalSize,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::ShaderTable | BufferUse::MapWrite,
        .Hints = ResourceHint::None});
    if (!buffer.HasValue()) {
        return nullptr;
    }
    return make_unique<ShaderBindingTableVulkan>(this, pipeline, buffer.Release(), desc, recordStride);
}

Nullable<unique_ptr<Sampler>> DeviceVulkan::CreateSampler(const SamplerDescriptor& desc) noexcept {
    VkSamplerCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.magFilter = MapTypeFilter(desc.MagFilter);
    createInfo.minFilter = MapTypeFilter(desc.MinFilter);
    createInfo.mipmapMode = MapTypeMipmapMode(desc.MipmapFilter);
    createInfo.addressModeU = MapType(desc.AddressS);
    createInfo.addressModeV = MapType(desc.AddressT);
    createInfo.addressModeW = MapType(desc.AddressR);
    createInfo.mipLodBias = 0;
    if (desc.AnisotropyClamp > 1.0f) {
        createInfo.anisotropyEnable = VK_TRUE;
        createInfo.maxAnisotropy = (float)desc.AnisotropyClamp;
    } else {
        createInfo.anisotropyEnable = VK_FALSE;
        createInfo.maxAnisotropy = 1.0f;
    }
    createInfo.compareEnable = desc.Compare.has_value() ? VK_TRUE : VK_FALSE;
    createInfo.compareOp = desc.Compare.has_value() ? MapType(desc.Compare.value()) : VK_COMPARE_OP_NEVER;
    createInfo.minLod = desc.LodMin;
    createInfo.maxLod = desc.LodMax;
    createInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    createInfo.unnormalizedCoordinates = VK_FALSE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateSampler(_device, &createInfo, this->GetAllocationCallbacks(), &sampler);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateSampler failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<SamplerVulkan>(this, sampler);
    result->_mdesc = desc;
    return result;
}

Nullable<unique_ptr<BindlessArray>> DeviceVulkan::CreateBindlessArray(const BindlessArrayDescriptor& desc) noexcept {
    if (!this->GetDetail().IsBindlessArraySupported) {
        RADRAY_ERR_LOG("vk bindless array is not supported by this device");
        return nullptr;
    }
    if (desc.Size == 0) {
        RADRAY_ERR_LOG("vk bindless array size must be greater than 0");
        return nullptr;
    }
    if (desc.Size > kBindlessDescriptorCapacityVulkan) {
        RADRAY_ERR_LOG(
            "vk bindless array size {} exceeds supported capacity {}",
            desc.Size,
            kBindlessDescriptorCapacityVulkan);
        return nullptr;
    }
    if (desc.SlotType != BindlessSlotType::BufferOnly &&
        desc.SlotType != BindlessSlotType::Texture2DOnly) {
        RADRAY_ERR_LOG(
            "vk bindless array slot type {} is not supported by the shader-derived path",
            static_cast<uint32_t>(desc.SlotType));
        return nullptr;
    }
    return make_unique<BindlessArrayVulkan>(this, desc);
}

Nullable<unique_ptr<LegacyFenceVulkan>> DeviceVulkan::CreateLegacyFence(VkFenceCreateFlags flags) noexcept {
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = nullptr;
    fenceInfo.flags = flags;
    VkFence fence = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateFence(_device, &fenceInfo, this->GetAllocationCallbacks(), &fence);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateFence failed: {}", vr);
        return nullptr;
    }
    return make_unique<LegacyFenceVulkan>(this, fence);
}

Nullable<unique_ptr<LegacySemaphoreVulkan>> DeviceVulkan::CreateLegacySemaphore(VkSemaphoreCreateFlags flags) noexcept {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = nullptr;
    info.flags = flags;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateSemaphore(_device, &info, this->GetAllocationCallbacks(), &semaphore);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateSemaphore failed: {}", vr);
        return nullptr;
    }
    return make_unique<LegacySemaphoreVulkan>(this, semaphore);
}

Nullable<unique_ptr<TimelineSemaphoreVulkan>> DeviceVulkan::CreateTimelineSemaphore(uint64_t initValue) noexcept {
    if (!_extFeatures.feature12.timelineSemaphore) {
        RADRAY_ERR_LOG("vk feature timeline semaphore not supported");
        return nullptr;
    }
    VkSemaphoreTypeCreateInfo timelineCreateInfo{};
    timelineCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineCreateInfo.pNext = nullptr;
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = initValue;
    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    createInfo.pNext = &timelineCreateInfo;
    createInfo.flags = 0;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateSemaphore(_device, &createInfo, this->GetAllocationCallbacks(), &semaphore);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateSemaphore failed: {}", vr);
        return nullptr;
    }
    return make_unique<TimelineSemaphoreVulkan>(this, semaphore);
}

Nullable<unique_ptr<BufferViewVulkan>> DeviceVulkan::CreateBufferView(const VkBufferViewCreateInfo& info) noexcept {
    VkBufferView bufferView = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateBufferView(_device, &info, this->GetAllocationCallbacks(), &bufferView);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateBufferView failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<BufferViewVulkan>(this, bufferView);
    result->_rawInfo = info;
    return result;
}

Nullable<unique_ptr<RenderPass>> DeviceVulkan::CreateRenderPass(const RenderPassDescriptor& desc) noexcept {
    if (desc.ColorAttachments.empty() && !desc.DepthStencilAttachment.has_value()) {
        RADRAY_ERR_LOG("vk render pass must have at least one attachment");
        return nullptr;
    }

    vector<VkAttachmentDescription> attachments;
    vector<VkAttachmentReference> colorReferences;
    attachments.reserve(desc.ColorAttachments.size() + (desc.DepthStencilAttachment.has_value() ? 1u : 0u));
    colorReferences.reserve(desc.ColorAttachments.size());
    for (const RenderPassColorAttachmentDescriptor& color : desc.ColorAttachments) {
        if (color.Format == TextureFormat::UNKNOWN || color.SampleCount == 0) {
            RADRAY_ERR_LOG("vk render pass has invalid color attachment format or sample count");
            return nullptr;
        }
        attachments.push_back(VkAttachmentDescription{
            .flags = 0,
            .format = MapType(color.Format),
            .samples = MapSampleCount(color.SampleCount),
            .loadOp = MapType(color.Load),
            .storeOp = MapType(color.Store),
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        colorReferences.push_back(VkAttachmentReference{
            .attachment = static_cast<uint32_t>(attachments.size() - 1),
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    }

    VkAttachmentReference depthReference{};
    if (desc.DepthStencilAttachment.has_value()) {
        const RenderPassDepthStencilAttachmentDescriptor& depth = desc.DepthStencilAttachment.value();
        if (!IsDepthStencilFormat(depth.Format) || depth.SampleCount == 0) {
            RADRAY_ERR_LOG("vk render pass has invalid depth attachment format or sample count");
            return nullptr;
        }
        const bool hasStencil = depth.Format == TextureFormat::D24_UNORM_S8_UINT ||
                                depth.Format == TextureFormat::D32_FLOAT_S8_UINT;
        const VkImageLayout layout = hasStencil
                                         ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                         : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        attachments.push_back(VkAttachmentDescription{
            .flags = 0,
            .format = MapType(depth.Format),
            .samples = MapSampleCount(depth.SampleCount),
            .loadOp = MapType(depth.DepthLoad),
            .storeOp = MapType(depth.DepthStore),
            .stencilLoadOp = hasStencil ? MapType(depth.StencilLoad) : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = hasStencil ? MapType(depth.StencilStore) : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = layout,
            .finalLayout = layout});
        depthReference = VkAttachmentReference{
            .attachment = static_cast<uint32_t>(attachments.size() - 1),
            .layout = layout};
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
    subpass.pColorAttachments = colorReferences.empty() ? nullptr : colorReferences.data();
    subpass.pDepthStencilAttachment = desc.DepthStencilAttachment.has_value() ? &depthReference : nullptr;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;

    VkRenderPass pass = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateRenderPass(_device, &info, this->GetAllocationCallbacks(), &pass);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateRenderPass failed: {}", vr);
        return nullptr;
    }
    return make_unique<RenderPassVulkan>(this, pass, desc);
}

Nullable<unique_ptr<Framebuffer>> DeviceVulkan::CreateFramebuffer(const FramebufferDescriptor& desc) noexcept {
    if (desc.Pass == nullptr || desc.Width == 0 || desc.Height == 0 || desc.Layers == 0) {
        RADRAY_ERR_LOG("vk framebuffer has invalid pass or dimensions");
        return nullptr;
    }
    const RenderPassDescriptor passDesc = desc.Pass->GetDesc();
    if (desc.ColorAttachments.size() != passDesc.ColorAttachments.size() ||
        (desc.DepthStencilAttachment != nullptr) != passDesc.DepthStencilAttachment.has_value()) {
        RADRAY_ERR_LOG("vk framebuffer attachment count does not match render pass");
        return nullptr;
    }

    vector<VkImageView> attachments;
    attachments.reserve(desc.ColorAttachments.size() + (desc.DepthStencilAttachment != nullptr ? 1u : 0u));
    for (size_t i = 0; i < desc.ColorAttachments.size(); ++i) {
        auto* view = CastVkObject(desc.ColorAttachments[i]);
        if (view == nullptr || !view->IsValid() || view->_mdesc.Format != passDesc.ColorAttachments[i].Format ||
            view->_image->_sampleCount != passDesc.ColorAttachments[i].SampleCount ||
            view->_image->_width < desc.Width || view->_image->_height < desc.Height) {
            RADRAY_ERR_LOG("vk framebuffer color attachment {} is incompatible", i);
            return nullptr;
        }
        attachments.push_back(view->_imageView);
    }
    if (desc.DepthStencilAttachment != nullptr) {
        auto* view = CastVkObject(desc.DepthStencilAttachment);
        const auto& depth = passDesc.DepthStencilAttachment.value();
        if (!view->IsValid() || view->_mdesc.Format != depth.Format ||
            view->_image->_sampleCount != depth.SampleCount ||
            view->_image->_width < desc.Width || view->_image->_height < desc.Height) {
            RADRAY_ERR_LOG("vk framebuffer depth attachment is incompatible");
            return nullptr;
        }
        attachments.push_back(view->_imageView);
    }

    auto* pass = CastVkObject(desc.Pass);
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass->_renderPass;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.width = desc.Width;
    info.height = desc.Height;
    info.layers = desc.Layers;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateFramebuffer(_device, &info, GetAllocationCallbacks(), &framebuffer);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateFramebuffer failed: {}", vr);
        return nullptr;
    }
    return make_unique<FrameBufferVulkan>(this, framebuffer, desc);
}

const VkAllocationCallbacks* DeviceVulkan::GetAllocationCallbacks() const noexcept {
    return _instance->GetAllocationCallbacks();
}

void DeviceVulkan::SetObjectName(std::string_view name, VkObjectType type, void* vkObject) const noexcept {
    bool hasDebugUtils = false;
    for (const string& ext : _instance->_exts) {
        if (ext == VK_EXT_DEBUG_UTILS_EXTENSION_NAME) {
            hasDebugUtils = true;
            break;
        }
    }
    if (!hasDebugUtils) {
        return;
    }
    if (vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }
    string cpyName{name};
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext = nullptr;
    info.objectType = type;
    info.objectHandle = std::bit_cast<uint64_t>(vkObject);
    info.pObjectName = cpyName.c_str();
    vkSetDebugUtilsObjectNameEXT(_device, &info);
}

void DeviceVulkan::DestroyImpl() noexcept {
    _bdlsBuffer.reset();
    _bdlsBufferTexelRo.reset();
    _bdlsBufferTexelRw.reset();
    _bdlsTex2d.reset();
    _bdlsTex3d.reset();
    _descSetAlloc.reset();
    _vma.reset();
    for (auto&& i : _queues) {
        i.clear();
    }
    if (_device != VK_NULL_HANDLE && _pipelineCache != VK_NULL_HANDLE) {
        _ftb.vkDestroyPipelineCache(_device, _pipelineCache, GetAllocationCallbacks());
        _pipelineCache = VK_NULL_HANDLE;
    }
    if (_device != VK_NULL_HANDLE) {
        _ftb.vkDestroyDevice(_device, this->GetAllocationCallbacks());
        _device = VK_NULL_HANDLE;
    }
    _physicalDevice = VK_NULL_HANDLE;
    _instance = nullptr;
}

Nullable<InstanceVulkanImpl*> InitVulkanEnvImpl(const VulkanInstanceDescriptor& desc) {
    if (g_vkInstance.HasValue()) {
        RADRAY_ERR_LOG("vk has actived VkInstance");
        return nullptr;
    }
    if (volkInitialize() != VK_SUCCESS) {
        RADRAY_ERR_LOG("volkInitialize failed");
        return nullptr;
    }
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkEnumerateInstanceVersion failed");
        return nullptr;
    }
    RADRAY_INFO_LOG("vk instance version: {}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    vector<VkExtensionProperties> extProps;
    if (EnumerateVectorFromVkFunc(extProps, vkEnumerateInstanceExtensionProperties, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkEnumerateInstanceExtensionProperties failed");
        return nullptr;
    }
    vector<VkLayerProperties> layerProps;
    if (EnumerateVectorFromVkFunc(layerProps, vkEnumerateInstanceLayerProperties) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkEnumerateInstanceLayerProperties failed");
        return nullptr;
    }
    for (auto& i : extProps) {
        RADRAY_DEBUG_LOG("vk instance extension: {} version: {}", i.extensionName, i.specVersion);
    }
    for (auto& i : layerProps) {
        RADRAY_DEBUG_LOG("vk instance layer: {} version: {}", i.layerName, i.specVersion);
    }

    unordered_set<string> needExts;
    unordered_set<string> needLayers;
    bool isValidFeatureExtEnable = false;
    needExts.emplace(VK_KHR_SURFACE_EXTENSION_NAME);
    needExts.emplace(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    needExts.emplace(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    needExts.emplace(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    needExts.emplace(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
    needExts.emplace(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    needExts.emplace(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
    if (desc.IsEnableDebugLayer) {
        const auto requireExt = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        const char* requireExts[] = {requireExt};
        if (IsValidateExtensions(requireExts, extProps)) {
            needExts.emplace(requireExt);
        } else {
            RADRAY_WARN_LOG("vk unsupported ext: {}", requireExt);
        }
        const auto validName = "VK_LAYER_KHRONOS_validation";
        const char* requireLayer[] = {validName};
        if (IsValidateLayers(requireLayer, layerProps)) {
            needLayers.emplace(validName);
            isValidFeatureExtEnable = true;
        } else {
            RADRAY_WARN_LOG("vk unsupported layer: {}", validName);
        }
    }
    for (const auto& i : needLayers) {
        const char* require[] = {i.c_str()};
        if (!IsValidateLayers(require, layerProps)) {
            RADRAY_ERR_LOG("vk unsupported layer: {}", i);
            return nullptr;
        }
    }
    for (const auto& i : needExts) {
        const char* require[] = {i.c_str()};
        if (!IsValidateExtensions(require, extProps)) {
            RADRAY_ERR_LOG("vk unsupported ext: {}", i);
            return nullptr;
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
    const bool hasDebugUtilsExt = needExts.contains(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkAllocationCallbacks* allocCbPtr = nullptr;
#if RADRAY_ENABLE_MIMALLOC
    VkAllocationCallbacks allocCb{
        nullptr,
        [](void* pUserData, size_t size, size_t alignment, VkSystemAllocationScope allocationScope) -> void* {
            RADRAY_UNUSED(pUserData);
            RADRAY_UNUSED(allocationScope);
            return mi_malloc_aligned(size, alignment);
        },
        [](void* pUserData, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope allocationScope) -> void* {
            RADRAY_UNUSED(pUserData);
            RADRAY_UNUSED(allocationScope);
            return mi_realloc_aligned(pOriginal, size, alignment);
        },
        [](void* pUserData, void* pMemory) { RADRAY_UNUSED(pUserData); mi_free(pMemory); },
        nullptr,
        nullptr};
    allocCbPtr = &allocCb;
#endif

    vector<VkValidationFeatureEnableEXT> validEnables{};
    VkValidationFeaturesEXT validFeature{};
    validFeature.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validFeature.pNext = nullptr;
    if (isValidFeatureExtEnable) {
        // Extra validation features are unstable on some Windows driver/SDK setups
        // and can crash before device creation. Keep the validation layer enabled,
        // but avoid opting into these non-essential feature toggles by default.
        if (desc.IsEnableGpuBasedValid) {  // vk 1.4.321.0 开这两个校验层内存会持续增长 :) :) :)
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
    appInfo.pApplicationName = desc.AppName.data();
    appInfo.applicationVersion = desc.AppVersion;
    appInfo.pEngineName = desc.EngineName.data();
    appInfo.engineVersion = desc.EngineVersion;
    appInfo.apiVersion = version;
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
#if defined(VK_USE_PLATFORM_METAL_EXT)
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(needExtsCStr.size());
    createInfo.ppEnabledExtensionNames = needExtsCStr.empty() ? nullptr : needExtsCStr.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(needLayersCStr.size());
    createInfo.ppEnabledLayerNames = needLayersCStr.empty() ? nullptr : needLayersCStr.data();
    if (isValidFeatureExtEnable) {
        createInfo.pNext = &validFeature;
        validFeature.pNext = hasDebugUtilsExt ? &debugCreateInfo : nullptr;
    } else if (hasDebugUtilsExt) {
        createInfo.pNext = &debugCreateInfo;
    }
    uint32_t apiVersionsToTry[] = {
        VK_API_VERSION_1_3,
        VK_API_VERSION_1_2,
        VK_API_VERSION_1_1,
        VK_API_VERSION_1_0};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult lastResult = VK_ERROR_UNKNOWN;
    uint32_t lastExceptionCode = 0;
    for (uint32_t apiVersion : apiVersionsToTry) {
        appInfo.apiVersion = apiVersion;
        VkResult vr = VK_ERROR_UNKNOWN;
        uint32_t exceptionCode = 0;
        if (!_TryCreateVkInstance(&createInfo, allocCbPtr, &instance, vr, exceptionCode)) {
            lastExceptionCode = exceptionCode;
            RADRAY_WARN_LOG(
                "vkCreateInstance with api version {}.{}.{} raised SEH 0x{:08X}",
                VK_VERSION_MAJOR(apiVersion),
                VK_VERSION_MINOR(apiVersion),
                VK_VERSION_PATCH(apiVersion),
                exceptionCode);
            continue;
        }
        if (vr == VK_SUCCESS) {
            break;
        } else {
            lastResult = vr;
            RADRAY_WARN_LOG("vkCreateInstance with api version {}.{}.{} failed: {}", VK_VERSION_MAJOR(apiVersion), VK_VERSION_MINOR(apiVersion), VK_VERSION_PATCH(apiVersion), vr);
        }
    }
    if (instance == VK_NULL_HANDLE) {
        if (lastExceptionCode != 0) {
            RADRAY_ERR_LOG("vkCreateInstance raised structured exception 0x{:08X}", lastExceptionCode);
        }
        RADRAY_ERR_LOG("vkCreateInstance failed: {}", lastResult);
        return nullptr;
    }
    volkLoadInstanceOnly(instance);
    auto result = make_unique<InstanceVulkanImpl>(
        instance,
        allocCbPtr ? std::make_optional(*allocCbPtr) : std::nullopt,
        vector<string>{needExts.begin(), needExts.end()},
        vector<string>{needLayers.begin(), needLayers.end()});
    result->_logCallback = desc.LogCallback;
    result->_logUserData = desc.LogUserData;
    if (hasDebugUtilsExt) {
        debugCreateInfo.pUserData = result.get();
        if (vkCreateDebugUtilsMessengerEXT != nullptr) {
            VkResult vr = VK_ERROR_UNKNOWN;
            uint32_t exceptionCode = 0;
            if (!_TryCreateVkDebugUtilsMessenger(
                    instance,
                    &debugCreateInfo,
                    result->GetAllocationCallbacks(),
                    &result->_debugMessenger,
                    vr,
                    exceptionCode)) {
                RADRAY_WARN_LOG("vkCreateDebugUtilsMessengerEXT raised SEH 0x{:08X}", exceptionCode);
            } else if (vr != VK_SUCCESS) {
                RADRAY_WARN_LOG("vkCreateDebugUtilsMessengerEXT failed: {}", vr);
            }
        } else {
            RADRAY_WARN_LOG("vkCreateDebugUtilsMessengerEXT is null");
        }
    }
    g_vkInstance = std::move(result);
    return g_vkInstance.Get();
}

void ShutdownVulkanEnvImpl() noexcept {
    g_vkInstance = nullptr;
    volkFinalize();
}

Nullable<shared_ptr<DeviceVulkan>> CreateDeviceVulkan(const VulkanDeviceDescriptor& desc) {
    if (!g_vkInstance.HasValue()) {
        RADRAY_ERR_LOG("vk env not init");
        return nullptr;
    }

    auto physicalDevices = _EnumeratePhysicalDeviceInfos(g_vkInstance->_instance, true);
    if (!physicalDevices.has_value()) {
        return nullptr;
    }

    size_t selectPhysicalDeviceIndex = std::numeric_limits<size_t>::max();
    if (desc.PhysicalDeviceIndex.has_value()) {
        uint32_t index = desc.PhysicalDeviceIndex.value();
        if (index >= physicalDevices->size()) {
            RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "PhysicalDeviceIndex", physicalDevices->size(), index);
            return nullptr;
        }
        selectPhysicalDeviceIndex = index;
    } else {
        auto selectedIndex = _SelectHighPerformancePhysicalDeviceArrayIndex(physicalDevices.value());
        if (!selectedIndex.has_value()) {
            RADRAY_ERR_LOG("vk no physical device found");
            return nullptr;
        }
        selectPhysicalDeviceIndex = selectedIndex.value();
    }

    const auto& selectPhyDevice = physicalDevices.value()[selectPhysicalDeviceIndex];
    RADRAY_INFO_LOG("vk select physical device: {}", selectPhyDevice.properties.deviceName);

    struct QueueRequest {
        QueueType rawType;
        VkQueueFlags requiredFlag;
        uint32_t requiredCount;
        vector<QueueIndexInFamily> queueIndices;
    };
    vector<VkDeviceQueueCreateInfo> queueInfos;
    vector<float> queuePriorities;
    vector<QueueRequest> queueRequests;
    struct QueueFamilyUsage {
        uint32_t familyIndex;
        VkQueueFlags flags;
        uint32_t maxCount;
        uint32_t usedCount;
    };
    vector<VkQueueFamilyProperties> queueFamilyProps;
    {
        EnumerateVectorFromVkFunc(queueFamilyProps, vkGetPhysicalDeviceQueueFamilyProperties, selectPhyDevice.device);
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
                RADRAY_WARN_LOG("vk unsupported queue type: {}", VulkanQueueFlags{static_cast<std::underlying_type_t<VkQueueFlagBits>>(r.requiredFlag)});
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
                RADRAY_ERR_LOG("vk not enough queue family for type: {}", VulkanQueueFlags{static_cast<std::underlying_type_t<VkQueueFlagBits>>(i.requiredFlag)});
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

    auto extProperties = make_unique<ExtPropertiesVulkan>();
    unordered_set<string> needExts;
    needExts.emplace(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    vector<VkExtensionProperties> deviceExtsAvailable;
    if (auto vr = EnumerateVectorFromVkFunc(deviceExtsAvailable, vkEnumerateDeviceExtensionProperties, selectPhyDevice.device, nullptr);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkEnumerateDeviceExtensionProperties failed: {}", vr);
        return nullptr;
    }
    if (IsValidateExtensions("VK_KHR_portability_subset", deviceExtsAvailable)) {
        needExts.emplace("VK_KHR_portability_subset");
    }
    if (IsValidateExtensions(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, deviceExtsAvailable)) {
        needExts.emplace(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    }
    const bool hasRtExts =
        desc.EnableRayTracing &&
        IsValidateExtensions(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, deviceExtsAvailable) &&
        IsValidateExtensions(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, deviceExtsAvailable) &&
        IsValidateExtensions(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, deviceExtsAvailable) &&
        IsValidateExtensions(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, deviceExtsAvailable);
    if (hasRtExts) {
        needExts.emplace(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        needExts.emplace(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        needExts.emplace(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        needExts.emplace(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
    VkPhysicalDeviceProperties2 deviceProperties2{};
    deviceProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    deviceProperties2.pNext = nullptr;
    if (IsValidateExtensions(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME, deviceExtsAvailable)) {
        needExts.emplace(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
        extProperties->conservativeRasterization = VkPhysicalDeviceConservativeRasterizationPropertiesEXT{};
        auto& cr = extProperties->conservativeRasterization.value();
        cr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT;
        cr.pNext = nullptr;
        AddToHeadVulkanStruct(deviceProperties2, cr);
    }
    if (hasRtExts) {
        extProperties->accelerationStructure = VkPhysicalDeviceAccelerationStructurePropertiesKHR{};
        auto& asProp = extProperties->accelerationStructure.value();
        asProp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        asProp.pNext = nullptr;
        AddToHeadVulkanStruct(deviceProperties2, asProp);
    }
    if (hasRtExts) {
        extProperties->rayTracingPipeline = VkPhysicalDeviceRayTracingPipelinePropertiesKHR{};
        auto& rtProp = extProperties->rayTracingPipeline.value();
        rtProp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        rtProp.pNext = nullptr;
        AddToHeadVulkanStruct(deviceProperties2, rtProp);
    }
    for (const auto& ext : needExts) {
        const char* exts[] = {ext.c_str()};
        if (!IsValidateExtensions(exts, deviceExtsAvailable)) {
            RADRAY_ERR_LOG("vk device extension not supported: {}", ext);
            return nullptr;
        }
    }
    if (selectPhyDevice.properties.apiVersion >= VK_API_VERSION_1_1 && vkGetPhysicalDeviceProperties2) {
        vkGetPhysicalDeviceProperties2(selectPhyDevice.device, &deviceProperties2);
    }

    vector<const char*> deviceExts{};
    deviceExts.reserve(needExts.size());
    for (const auto& ext : needExts) {
        deviceExts.emplace_back(ext.c_str());
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
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
    deviceInfo.pEnabledFeatures = &deviceFeatures;
    auto extFeatures = make_unique<ExtFeaturesVulkan>();
    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures2.pNext = nullptr;
    if (selectPhyDevice.properties.apiVersion >= VK_API_VERSION_1_1 && vkGetPhysicalDeviceFeatures2) {
        extFeatures->feature11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        AddToHeadVulkanStruct(deviceFeatures2, extFeatures->feature11);

        extFeatures->feature12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        extFeatures->feature12.pNext = nullptr;
        if (selectPhyDevice.properties.apiVersion >= VK_API_VERSION_1_2) {
            AddToHeadVulkanStruct(deviceFeatures2, extFeatures->feature12);
        }

        extFeatures->feature13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        extFeatures->feature13.pNext = nullptr;
        if (selectPhyDevice.properties.apiVersion >= VK_API_VERSION_1_3) {
            AddToHeadVulkanStruct(deviceFeatures2, extFeatures->feature13);
        }

        const bool useCore12Bda = selectPhyDevice.properties.apiVersion >= VK_API_VERSION_1_2;
        extFeatures->bufferDeviceAddress = VkPhysicalDeviceBufferDeviceAddressFeatures{};
        extFeatures->bufferDeviceAddress.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        extFeatures->bufferDeviceAddress.pNext = nullptr;
        if (!useCore12Bda) {
            AddToHeadVulkanStruct(deviceFeatures2, extFeatures->bufferDeviceAddress);
        }

        extFeatures->accelerationStructure = VkPhysicalDeviceAccelerationStructureFeaturesKHR{};
        extFeatures->rayTracingPipeline = VkPhysicalDeviceRayTracingPipelineFeaturesKHR{};
        if (hasRtExts) {
            extFeatures->accelerationStructure.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            extFeatures->accelerationStructure.pNext = nullptr;
            AddToHeadVulkanStruct(deviceFeatures2, extFeatures->accelerationStructure);

            extFeatures->rayTracingPipeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
            extFeatures->rayTracingPipeline.pNext = nullptr;
            AddToHeadVulkanStruct(deviceFeatures2, extFeatures->rayTracingPipeline);
        }

        vkGetPhysicalDeviceFeatures2(selectPhyDevice.device, &deviceFeatures2);

        if (useCore12Bda) {
            extFeatures->feature12.bufferDeviceAddress = extFeatures->feature12.bufferDeviceAddress ? VK_TRUE : VK_FALSE;
            extFeatures->bufferDeviceAddress.bufferDeviceAddress = extFeatures->feature12.bufferDeviceAddress;
        } else {
            extFeatures->bufferDeviceAddress.bufferDeviceAddress = extFeatures->bufferDeviceAddress.bufferDeviceAddress ? VK_TRUE : VK_FALSE;
        }
        if (hasRtExts) {
            extFeatures->accelerationStructure.accelerationStructure = extFeatures->accelerationStructure.accelerationStructure ? VK_TRUE : VK_FALSE;
            extFeatures->rayTracingPipeline.rayTracingPipeline = extFeatures->rayTracingPipeline.rayTracingPipeline ? VK_TRUE : VK_FALSE;
        }
        deviceInfo.pNext = &deviceFeatures2;
        deviceInfo.pEnabledFeatures = nullptr;
    }

    VkDevice device = VK_NULL_HANDLE;
    VkResult deviceCreateResult = VK_ERROR_UNKNOWN;
    uint32_t exceptionCode = 0;
    if (!_TryCreateVkDevice(
            selectPhyDevice.device,
            &deviceInfo,
            g_vkInstance->GetAllocationCallbacks(),
            &device,
            deviceCreateResult,
            exceptionCode)) {
        RADRAY_ERR_LOG("vkCreateDevice raised structured exception 0x{:08X}", exceptionCode);
        return nullptr;
    }
    if (deviceCreateResult != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateDevice failed: {}", deviceCreateResult);
        return nullptr;
    }
    auto deviceR = make_shared<DeviceVulkan>(
        g_vkInstance.Get(),
        selectPhyDevice.device,
        device);
    volkLoadDeviceTable(&deviceR->_ftb, deviceR->_device);
    VmaVulkanFunctions vmaFunctions{};
    VmaAllocatorCreateInfo vmaCreateInfo{};
    vmaCreateInfo.flags = 0;
    vmaCreateInfo.physicalDevice = deviceR->_physicalDevice;
    vmaCreateInfo.device = deviceR->_device;
    vmaCreateInfo.preferredLargeHeapBlockSize = 0;
    vmaCreateInfo.pAllocationCallbacks = deviceR->GetAllocationCallbacks();
    vmaCreateInfo.pDeviceMemoryCallbacks = nullptr;
    vmaCreateInfo.pHeapSizeLimit = nullptr;
    vmaCreateInfo.pVulkanFunctions = &vmaFunctions;
    vmaCreateInfo.instance = deviceR->_instance->_instance;
    vmaCreateInfo.vulkanApiVersion = selectPhyDevice.properties.apiVersion;
#if VMA_EXTERNAL_MEMORY
    vmaCreateInfo.pTypeExternalMemoryHandleTypes = nullptr;
#endif
    if (auto vr = vmaImportVulkanFunctionsFromVolk(&vmaCreateInfo, &vmaFunctions);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vmaImportVulkanFunctionsFromVolk failed: {}", vr);
        return nullptr;
    }
    if (extFeatures->bufferDeviceAddress.bufferDeviceAddress) {
        vmaCreateInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }
    VmaAllocator vma;
    if (auto vr = vmaCreateAllocator(&vmaCreateInfo, &vma);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vmaCreateAllocator failed: {}", vr);
        return nullptr;
    }
    deviceR->_vma = make_unique<VMA>(vma);
    for (const auto& i : queueRequests) {
        for (const auto& j : i.queueIndices) {
            VkQueue queuePtr = VK_NULL_HANDLE;
            deviceR->_ftb.vkGetDeviceQueue(deviceR->_device, j.Family, j.IndexInFamily, &queuePtr);
            if (queuePtr == VK_NULL_HANDLE) {
                RADRAY_ERR_LOG("vkGetDeviceQueue failed: {} {}", j.Family, j.IndexInFamily);
                return nullptr;
            }
            auto queue = make_unique<QueueVulkan>(
                deviceR.get(),
                queuePtr,
                j,
                i.rawType,
                queueFamilyProps[j.Family].queueFlags);
            deviceR->_queues[(size_t)i.rawType].emplace_back(std::move(queue));
        }
    }
    deviceR->_descSetAlloc = make_unique<DescriptorSetAllocatorVulkan>(deviceR.get(), 1);
    if (deviceInfo.pEnabledFeatures) {
        deviceR->_feature = *deviceInfo.pEnabledFeatures;
    } else {
        deviceR->_feature = deviceFeatures2.features;
    }
    deviceR->_extFeatures = *extFeatures;
    deviceR->_properties = selectPhyDevice.properties;
    deviceR->_extProperties = *extProperties;
    {
        DeviceDetail& detail = deviceR->_detail;
        const auto& props = selectPhyDevice.properties;
        detail.GpuName = props.deviceName;
        detail.CBufferAlignment = (uint32_t)deviceR->_properties.limits.minUniformBufferOffsetAlignment;
        detail.BufferCopyOffsetAlignment = 1;
        detail.TextureDataPitchAlignment = (uint32_t)deviceR->_properties.limits.optimalBufferCopyRowPitchAlignment;
        detail.TextureDataPlacementAlignment = deviceR->_properties.limits.optimalBufferCopyOffsetAlignment;
        detail.MaxVertexInputBindings = deviceR->_properties.limits.maxVertexInputBindings;
        detail.IsUMA = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
        detail.VramBudget = GetPhysicalDeviceMemoryAllSize(selectPhyDevice.memory, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        const auto& f12 = deviceR->_extFeatures.feature12;
        detail.IsBindlessArraySupported =
            selectPhyDevice.properties.apiVersion >= VK_API_VERSION_1_2 &&
            f12.runtimeDescriptorArray &&
            f12.descriptorBindingVariableDescriptorCount &&
            f12.descriptorBindingPartiallyBound &&
            f12.shaderSampledImageArrayNonUniformIndexing;
        detail.IsLayeredRenderingFromVertexShaderSupported =
            selectPhyDevice.properties.apiVersion >= VK_API_VERSION_1_2 &&
            f12.shaderOutputLayer;
        detail.IsRayTracingSupported =
            extFeatures->bufferDeviceAddress.bufferDeviceAddress &&
            extFeatures->accelerationStructure.accelerationStructure &&
            extFeatures->rayTracingPipeline.rayTracingPipeline;
        if (detail.IsRayTracingSupported && extProperties->rayTracingPipeline.has_value() && extProperties->accelerationStructure.has_value()) {
            const auto& rtProp = extProperties->rayTracingPipeline.value();
            const auto& asProp = extProperties->accelerationStructure.value();
            detail.MaxRayRecursionDepth = rtProp.maxRayRecursionDepth;
            detail.ShaderTableAlignment = rtProp.shaderGroupBaseAlignment;
            detail.AccelerationStructureAlignment = static_cast<uint32_t>(asProp.minAccelerationStructureScratchOffsetAlignment);
            detail.AccelerationStructureScratchAlignment = static_cast<uint32_t>(asProp.minAccelerationStructureScratchOffsetAlignment);
        }
    }
    if (deviceR->_detail.IsBindlessArraySupported) {
        // auto bdlsBufferOpt = deviceR->CreateBindlessDescriptorSetVulkan(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 262144);
        // if (!bdlsBufferOpt) {
        //     return nullptr;
        // }
        // deviceR->_bdlsBuffer = make_unique<BindlessDescAllocator>(bdlsBufferOpt.Release());
        // auto bdlsBufferTexelRoOpt = deviceR->CreateBindlessDescriptorSetVulkan(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 262144);
        // if (!bdlsBufferTexelRoOpt) {
        //     return nullptr;
        // }
        // deviceR->_bdlsBufferTexelRo = make_unique<BindlessDescAllocator>(bdlsBufferTexelRoOpt.Release());
        // auto bdlsBufferTexelRwOpt = deviceR->CreateBindlessDescriptorSetVulkan(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 262144);
        // if (!bdlsBufferTexelRwOpt) {
        //     return nullptr;
        // }
        // deviceR->_bdlsBufferTexelRw = make_unique<BindlessDescAllocator>(bdlsBufferTexelRwOpt.Release());
        // auto bdlsTex2dOpt = deviceR->CreateBindlessDescriptorSetVulkan(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 262144);
        // if (!bdlsTex2dOpt) {
        //     return nullptr;
        // }
        // deviceR->_bdlsTex2d = make_unique<BindlessDescAllocator>(bdlsTex2dOpt.Release());
        // auto bdlsTex3dOpt = deviceR->CreateBindlessDescriptorSetVulkan(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 262144);
        // if (!bdlsTex3dOpt) {
        //     return nullptr;
        // }
        // deviceR->_bdlsTex3d = make_unique<BindlessDescAllocator>(bdlsTex3dOpt.Release());
    }
    RADRAY_INFO_LOG("========== Feature ==========");
    {
        auto apiVersion = selectPhyDevice.properties.apiVersion;
        RADRAY_INFO_LOG("Vulkan API Version: {}.{}.{}", VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));
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
                verStr = fmt::format("{}.{}.{}.{}", major, minor, patch, build);
                break;
            }
            case 0x8086:  // Intel
            case 0x1002:  // AMD
            default: {
                uint32_t variant = VK_API_VERSION_VARIANT(driverVersion);
                uint32_t major = VK_API_VERSION_MAJOR(driverVersion);
                uint32_t minor = VK_API_VERSION_MINOR(driverVersion);
                uint32_t patch = VK_API_VERSION_PATCH(driverVersion);
                verStr = fmt::format("{}.{}.{}.{}", variant, major, minor, patch);
                break;
            }
        }
        RADRAY_INFO_LOG("Driver Version: {}", verStr);
    }
    RADRAY_INFO_LOG("Physical Device Type: {}", selectPhyDevice.properties.deviceType);
    RADRAY_INFO_LOG("Queue:");
    for (size_t i = 0; i < deviceR->_queues.size(); ++i) {
        const auto& queues = deviceR->_queues[i];
        QueueType type = static_cast<QueueType>(i);
        if (queues.size() > 0) {
            RADRAY_INFO_LOG("\t{}: {}", type, queues.size());
        }
    }
    RADRAY_INFO_LOG("Timeline Semaphore: {}", deviceR->_extFeatures.feature12.timelineSemaphore ? true : false);
    RADRAY_INFO_LOG("Conservative Rasterization: {}", deviceR->_extProperties.conservativeRasterization.has_value() ? true : false);
    RADRAY_INFO_LOG("Bindless Array: {}", deviceR->_detail.IsBindlessArraySupported);
    RADRAY_INFO_LOG(
        "Layered Rendering From Vertex Shader: {}",
        deviceR->_detail.IsLayeredRenderingFromVertexShaderSupported);
    RADRAY_INFO_LOG("Ray Tracing: {}", deviceR->_detail.IsRayTracingSupported);
    RADRAY_INFO_LOG("=============================");
    return deviceR;
}

QueueVulkan::QueueVulkan(
    DeviceVulkan* device,
    VkQueue queue,
    QueueIndexInFamily family,
    QueueType type,
    VkQueueFlags queueFlags) noexcept
    : _device(device),
      _queue(queue),
      _family(family),
      _type(type),
      _queueFlags(queueFlags) {}

QueueVulkan::~QueueVulkan() noexcept {
    this->DestroyImpl();
}

bool QueueVulkan::IsValid() const noexcept {
    return _queue != VK_NULL_HANDLE;
}

void QueueVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void QueueVulkan::Submit(const CommandQueueSubmitDescriptor& desc) noexcept {
    vector<VkCommandBuffer> cmdBufs;
    cmdBufs.reserve(desc.CmdBuffers.size());
    for (auto i : desc.CmdBuffers) {
        auto cmdBuffer = CastVkObject(i);
        cmdBufs.emplace_back(cmdBuffer->_cmdBuffer);
    }

    vector<VkSemaphore> waitSemaphores;
    vector<VkSemaphore> signalSemaphores;
    vector<uint64_t> waitValues;
    vector<uint64_t> signalValues;
    vector<VkPipelineStageFlags> waitStages;
    bool useTimelineSubmit = false;
    FenceVulkan* submitCompletionFence = nullptr;
    uint64_t submitCompletionValue = 0;

    for (auto* syncObj : desc.WaitToExecute) {
        auto* waitSync = CastVkObject(syncObj);
        if (waitSync == nullptr || !waitSync->IsValid()) {
            RADRAY_ABORT("invalid swapchain wait sync object");
        }
        waitSemaphores.emplace_back(waitSync->_semaphore->_semaphore);
        VkPipelineStageFlags waitStageMask = 0;
        if ((_queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            waitStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        if ((_queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            waitStageMask |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if (waitStageMask == 0) {
            waitStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        waitStages.emplace_back(waitStageMask);
        waitValues.emplace_back(0);
    }

    for (size_t i = 0; i < desc.WaitFences.size(); ++i) {
        auto* fenceObj = CastVkObject(desc.WaitFences[i]);
        uint64_t waitValue = desc.WaitValues[i];
        useTimelineSubmit = true;
        waitSemaphores.emplace_back(fenceObj->_fence->_semaphore);
        waitValues.emplace_back(waitValue);
        waitStages.emplace_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }

    for (auto* syncObj : desc.ReadyToPresent) {
        auto* signalSync = CastVkObject(syncObj);
        if (signalSync == nullptr || !signalSync->IsValid()) {
            RADRAY_ABORT("invalid swapchain signal sync object");
        }
        signalSemaphores.emplace_back(signalSync->_semaphore->_semaphore);
        signalValues.emplace_back(0);
    }

    VkFence submitFence = VK_NULL_HANDLE;
    for (size_t i = 0; i < desc.SignalFences.size(); ++i) {
        auto* fenceObj = CastVkObject(desc.SignalFences[i]);
        uint64_t signalValue = desc.SignalValues[i];
        useTimelineSubmit = true;
        signalSemaphores.emplace_back(fenceObj->_fence->_semaphore);
        signalValues.emplace_back(signalValue);
        submitCompletionFence = fenceObj;
        submitCompletionValue = signalValue;
        if (signalValue + 1 > fenceObj->_fenceValue) {
            fenceObj->_fenceValue = signalValue + 1;
        }
    }

    if (!desc.WaitToExecute.empty() && submitCompletionFence == nullptr) {
        RADRAY_ABORT("vk queue submit with swapchain wait sync objects must signal a fence");
    }

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    if (useTimelineSubmit) {
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.pNext = nullptr;
        timelineInfo.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
        timelineInfo.pWaitSemaphoreValues = waitValues.empty() ? nullptr : waitValues.data();
        timelineInfo.signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size());
        timelineInfo.pSignalSemaphoreValues = signalValues.empty() ? nullptr : signalValues.data();
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = useTimelineSubmit ? &timelineInfo : nullptr;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    submitInfo.commandBufferCount = static_cast<uint32_t>(cmdBufs.size());
    submitInfo.pCommandBuffers = cmdBufs.empty() ? nullptr : cmdBufs.data();
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
    submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();

    if (auto vr = _device->_ftb.vkQueueSubmit(_queue, 1, &submitInfo, submitFence);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkQueueSubmit failed: {}", vr);
    }

    if (submitCompletionFence != nullptr) {
        for (auto* syncObj : desc.WaitToExecute) {
            auto* waitSync = CastVkObject(syncObj);
            waitSync->_pendingQueueFence = submitCompletionFence;
            waitSync->_pendingQueueValue = submitCompletionValue;
        }
    }
}

void QueueVulkan::Wait() noexcept {
    if (auto vr = _device->_ftb.vkQueueWaitIdle(_queue);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkQueueWaitIdle failed: {}", vr);
    }
}

QueueType QueueVulkan::GetQueueType() const noexcept {
    return _type;
}

void QueueVulkan::DestroyImpl() noexcept {
    if (_queue != VK_NULL_HANDLE) {
        _queue = VK_NULL_HANDLE;
    }
}

CommandPoolVulkan::CommandPoolVulkan(
    DeviceVulkan* device,
    VkCommandPool cmdPool) noexcept
    : _device(device),
      _cmdPool(cmdPool) {}

CommandPoolVulkan::~CommandPoolVulkan() noexcept {
    this->DestroyImpl();
}

bool CommandPoolVulkan::IsValid() const noexcept {
    return _cmdPool != VK_NULL_HANDLE;
}

void CommandPoolVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void CommandPoolVulkan::Reset() const noexcept {
    if (auto vr = _device->_ftb.vkResetCommandPool(_device->_device, _cmdPool, 0);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkResetCommandPool failed: {}", vr);
    }
}

void CommandPoolVulkan::DestroyImpl() noexcept {
    if (_cmdPool != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyCommandPool(_device->_device, _cmdPool, _device->GetAllocationCallbacks());
        _cmdPool = VK_NULL_HANDLE;
    }
}

CommandBufferVulkan::CommandBufferVulkan(
    DeviceVulkan* device,
    QueueVulkan* queue,
    unique_ptr<CommandPoolVulkan> cmdPool,
    VkCommandBuffer cmdBuffer) noexcept
    : _device(device),
      _queue(queue),
      _cmdPool(std::move(cmdPool)),
      _cmdBuffer(cmdBuffer) {}

CommandBufferVulkan::~CommandBufferVulkan() noexcept {
    this->DestroyImpl();
}

bool CommandBufferVulkan::IsValid() const noexcept {
    return _cmdPool != nullptr && _cmdBuffer != VK_NULL_HANDLE;
}

void CommandBufferVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void CommandBufferVulkan::SetDebugName(std::string_view name) noexcept {
    auto listName = fmt::format("CmdBuffer_{}", name);
    auto allocName = fmt::format("CmdPool_{}", name);
    _device->SetObjectName(listName, _cmdBuffer);
    _device->SetObjectName(allocName, _cmdPool->_cmdPool);
}

void CommandBufferVulkan::DestroyImpl() noexcept {
    _endedEncoders.clear();
    if (_cmdBuffer != VK_NULL_HANDLE) {
        _device->_ftb.vkFreeCommandBuffers(_device->_device, _cmdPool->_cmdPool, 1, &_cmdBuffer);
        _cmdBuffer = VK_NULL_HANDLE;
    }
    _cmdPool.reset();
}

void CommandBufferVulkan::Begin() noexcept {
    _endedEncoders.clear();
    _cmdPool->Reset();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.pNext = nullptr;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = nullptr;
    if (auto vr = _device->_ftb.vkBeginCommandBuffer(_cmdBuffer, &beginInfo);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkBeginCommandBuffer failed: {}", vr);
    }
}

void CommandBufferVulkan::End() noexcept {
    if (auto vr = _device->_ftb.vkEndCommandBuffer(_cmdBuffer);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkEndCommandBuffer failed: {}", vr);
    }
}

void CommandBufferVulkan::ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept {
    VkPipelineStageFlags srcStageMask = 0;
    VkPipelineStageFlags dstStageMask = 0;
    vector<VkBufferMemoryBarrier> bufferBarriers;
    vector<VkImageMemoryBarrier> imageBarriers;
    bufferBarriers.reserve(barriers.size());
    imageBarriers.reserve(barriers.size());
    for (const auto& v : barriers) {
        if (const auto* bb = std::get_if<BarrierBufferDescriptor>(&v)) {
            auto buf = CastVkObject(bb->Target);
            auto& bufBarrier = bufferBarriers.emplace_back();
            bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bufBarrier.pNext = nullptr;
            bufBarrier.srcAccessMask = BufferStateToAccessFlags(bb->Before);
            bufBarrier.dstAccessMask = BufferStateToAccessFlags(bb->After);
            if (bb->OtherQueue.HasValue()) {
                if (!_device->_extFeatures.feature12.timelineSemaphore) {
                    RADRAY_ABORT("cross-queue sync requires timeline semaphore support on Vulkan backend");
                }
                auto otherQ = CastVkObject(bb->OtherQueue.Get());
                if (bb->IsFromOrToOtherQueue) {
                    bufBarrier.srcQueueFamilyIndex = otherQ->_family.Family;
                    bufBarrier.dstQueueFamilyIndex = _queue->_family.Family;
                } else {
                    bufBarrier.srcQueueFamilyIndex = _queue->_family.Family;
                    bufBarrier.dstQueueFamilyIndex = otherQ->_family.Family;
                }
            } else {
                bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            }
            bufBarrier.buffer = buf->_buffer;
            bufBarrier.offset = 0;
            bufBarrier.size = buf->_reqSize;

            auto srcStage = BufferStateToPipelineStageFlags(bb->Before);
            auto dstStage = BufferStateToPipelineStageFlags(bb->After);
            srcStageMask |= srcStage;
            dstStageMask |= dstStage;
        } else if (const auto* tb = std::get_if<BarrierTextureDescriptor>(&v)) {
            auto tex = CastVkObject(tb->Target);
            auto& imgBarrier = imageBarriers.emplace_back();
            imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            imgBarrier.pNext = nullptr;
            imgBarrier.srcAccessMask = TextureStateToAccessFlags(tb->Before);
            imgBarrier.dstAccessMask = TextureStateToAccessFlags(tb->After);
            imgBarrier.oldLayout = TextureStateToLayout(tb->Before);
            imgBarrier.newLayout = TextureStateToLayout(tb->After);
            if (tb->OtherQueue.HasValue()) {
                if (!_device->_extFeatures.feature12.timelineSemaphore) {
                    RADRAY_ABORT("cross-queue sync requires timeline semaphore support on Vulkan backend");
                }
                auto otherQ = CastVkObject(tb->OtherQueue.Get());
                if (tb->IsFromOrToOtherQueue) {
                    imgBarrier.srcQueueFamilyIndex = otherQ->_family.Family;
                    imgBarrier.dstQueueFamilyIndex = _queue->_family.Family;
                } else {
                    imgBarrier.srcQueueFamilyIndex = _queue->_family.Family;
                    imgBarrier.dstQueueFamilyIndex = otherQ->_family.Family;
                }
            } else {
                imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            }
            imgBarrier.image = tex->_image;
            imgBarrier.subresourceRange.aspectMask = ImageFormatToAspectFlags(tex->_rawFormat);
            imgBarrier.subresourceRange.baseMipLevel = tb->IsSubresourceBarrier ? tb->Range.BaseMipLevel : 0;
            imgBarrier.subresourceRange.levelCount = tb->IsSubresourceBarrier ? tb->Range.MipLevelCount : VK_REMAINING_MIP_LEVELS;
            imgBarrier.subresourceRange.baseArrayLayer = tb->IsSubresourceBarrier ? tb->Range.BaseArrayLayer : 0;
            imgBarrier.subresourceRange.layerCount = tb->IsSubresourceBarrier ? tb->Range.ArrayLayerCount : VK_REMAINING_ARRAY_LAYERS;

            auto srcStage = TextureStateToPipelineStageFlags(tb->Before, true);
            auto dstStage = TextureStateToPipelineStageFlags(tb->After, false);
            // 如果是 swapchain image 从 undefined 转换, 增加 src stage 确保之前的写入操作都能被正确同步
            if (tex->_isSwapchainImage && tb->Before == TextureState::Undefined) {
                srcStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            }
            srcStageMask |= srcStage;
            dstStageMask |= dstStage;
        } else {
            const auto* ab = std::get_if<BarrierAccelerationStructureDescriptor>(&v);
            RADRAY_ASSERT(ab != nullptr);
            auto as = CastVkObject(ab->Target);
            auto& asBarrier = bufferBarriers.emplace_back();
            asBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            asBarrier.pNext = nullptr;
            asBarrier.srcAccessMask = BufferStateToAccessFlags(ab->Before) | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            asBarrier.dstAccessMask = BufferStateToAccessFlags(ab->After);
            asBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            asBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            asBarrier.buffer = as->_buffer;
            asBarrier.offset = 0;
            asBarrier.size = as->_asSize;
            srcStageMask |= BufferStateToPipelineStageFlags(ab->Before) | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            dstStageMask |= BufferStateToPipelineStageFlags(ab->After);
        }
    }
    if (bufferBarriers.size() > 0 || imageBarriers.size() > 0) {
        _device->_ftb.vkCmdPipelineBarrier(
            _cmdBuffer,
            srcStageMask,
            dstStageMask,
            0,
            0, nullptr,
            static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.size() == 0 ? nullptr : bufferBarriers.data(),
            static_cast<uint32_t>(imageBarriers.size()), imageBarriers.size() == 0 ? nullptr : imageBarriers.data());
    }
}

Nullable<unique_ptr<GraphicsCommandEncoder>> CommandBufferVulkan::BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept {
    if (desc.Pass == nullptr || desc.Target == nullptr) {
        RADRAY_ERR_LOG("vk BeginRenderPass requires an explicit render pass and framebuffer");
        return nullptr;
    }
    auto* pass = CastVkObject(desc.Pass);
    auto* framebuffer = CastVkObject(desc.Target);
    const FramebufferDescriptor framebufferDesc = desc.Target->GetDesc();
    const RenderPassDescriptor passDesc = desc.Pass->GetDesc();
    if (framebufferDesc.Pass != desc.Pass || desc.ColorClearValues.size() != passDesc.ColorAttachments.size() ||
        desc.DepthStencilClearValue.has_value() != passDesc.DepthStencilAttachment.has_value()) {
        RADRAY_ERR_LOG("vk BeginRenderPass descriptor does not match the framebuffer/render pass");
        return nullptr;
    }

    vector<VkClearValue> clearValues;
    clearValues.reserve(desc.ColorClearValues.size() + (desc.DepthStencilClearValue.has_value() ? 1u : 0u));
    for (size_t index = 0; index < desc.ColorClearValues.size(); ++index) {
        const ColorClearValue& value = desc.ColorClearValues[index];
        auto& clear = clearValues.emplace_back();
        if (IsUintFormat(passDesc.ColorAttachments[index].Format)) {
            for (uint32_t component = 0; component < 4; ++component) {
                clear.color.uint32[component] = static_cast<uint32_t>(value.Value[component]);
            }
        } else if (IsSintFormat(passDesc.ColorAttachments[index].Format)) {
            for (uint32_t component = 0; component < 4; ++component) {
                clear.color.int32[component] = static_cast<int32_t>(value.Value[component]);
            }
        } else {
            for (uint32_t component = 0; component < 4; ++component) {
                clear.color.float32[component] = value.Value[component];
            }
        }
    }
    if (desc.DepthStencilClearValue.has_value()) {
        auto& clear = clearValues.emplace_back();
        clear.depthStencil.depth = desc.DepthStencilClearValue->Depth;
        clear.depthStencil.stencil = desc.DepthStencilClearValue->Stencil;
    }
    if (!desc.Name.empty()) {
        pass->SetDebugName(desc.Name);
        framebuffer->SetDebugName(desc.Name);
    }

    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = pass->_renderPass;
    beginInfo.framebuffer = framebuffer->_framebuffer;
    beginInfo.renderArea = {{0, 0}, {framebufferDesc.Width, framebufferDesc.Height}};
    beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    beginInfo.pClearValues = clearValues.empty() ? nullptr : clearValues.data();
    _device->_ftb.vkCmdBeginRenderPass(_cmdBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    auto encoder = make_unique<SimulateCommandEncoderVulkan>(_device, this);
    encoder->_framebuffer = framebuffer;
    return encoder;
}

void CommandBufferVulkan::EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept {
    _device->_ftb.vkCmdEndRenderPass(_cmdBuffer);
    _endedEncoders.emplace_back(std::move(encoder));
}

Nullable<unique_ptr<ComputeCommandEncoder>> CommandBufferVulkan::BeginComputePass() noexcept {
    return make_unique<SimulateComputeEncoderVulkan>(_device, this);
}

void CommandBufferVulkan::EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept {
    _endedEncoders.emplace_back(std::move(encoder));
}

Nullable<unique_ptr<RayTracingCommandEncoder>> CommandBufferVulkan::BeginRayTracingPass() noexcept {
    if (!_device->GetDetail().IsRayTracingSupported) {
        RADRAY_ERR_LOG("vk ray tracing command encoder is not supported by this device");
        return nullptr;
    }
    return make_unique<CommandEncoderRayTracingVulkan>(_device, this);
}

void CommandBufferVulkan::EndRayTracingPass(unique_ptr<RayTracingCommandEncoder> encoder) noexcept {
    if (encoder != nullptr) {
        _endedEncoders.emplace_back(std::move(encoder));
    }
}

void CommandBufferVulkan::CopyBufferToBuffer(Buffer* dst_, uint64_t dstOffset, Buffer* src_, uint64_t srcOffset, uint64_t size) noexcept {
    auto dst = CastVkObject(dst_);
    auto src = CastVkObject(src_);
    VkBufferCopy copyInfo{};
    copyInfo.srcOffset = srcOffset;
    copyInfo.dstOffset = dstOffset;
    copyInfo.size = size;
    _device->_ftb.vkCmdCopyBuffer(_cmdBuffer, src->_buffer, dst->_buffer, 1, &copyInfo);
}

void CommandBufferVulkan::CopyBufferToTexture(Texture* dst_, SubresourceRange dstRange, Buffer* src_, uint64_t srcOffset) noexcept {
    auto dst = CastVkObject(dst_);
    auto src = CastVkObject(src_);
    const uint32_t bpp = GetTextureFormatBytesPerPixel(dst->_format);
    if (bpp == 0) {
        RADRAY_ERR_LOG("vk CopyBufferToTexture invalid texture format {}", dst->_format);
        return;
    }
    if (dstRange.MipLevelCount == SubresourceRange::All || dstRange.ArrayLayerCount == SubresourceRange::All) {
        RADRAY_ERR_LOG("vk CopyBufferToTexture requires explicit SubresourceRange count");
        return;
    }
    uint32_t mipLevels = dstRange.MipLevelCount;
    uint32_t layerCount = dstRange.ArrayLayerCount;
    if (mipLevels == 0 || layerCount == 0) {
        RADRAY_ERR_LOG("vk CopyBufferToTexture invalid SubresourceRange count (mipLevels={}, layerCount={})", mipLevels, layerCount);
        return;
    }
    if (dstRange.BaseMipLevel >= dst->_mipLevels ||
        dstRange.BaseMipLevel + mipLevels > dst->_mipLevels) {
        RADRAY_ERR_LOG("vk CopyBufferToTexture mip range out of bounds (base={}, count={}, total={})",
                       dstRange.BaseMipLevel, mipLevels, dst->_mipLevels);
        return;
    }
    bool is3D = dst->_dim == TextureDimension::Dim3D;
    uint32_t arraySize = is3D ? 1u : dst->_depthOrArraySize;
    if (dstRange.BaseArrayLayer >= arraySize ||
        dstRange.BaseArrayLayer + layerCount > arraySize) {
        RADRAY_ERR_LOG("vk CopyBufferToTexture array range out of bounds (base={}, count={}, total={})",
                       dstRange.BaseArrayLayer, layerCount, arraySize);
        return;
    }
    const uint64_t rowPitchAlignment = std::max<uint64_t>(1, _device->_detail.TextureDataPitchAlignment);
    VkImageAspectFlags aspectMask = ImageFormatToAspectFlags(dst->_rawFormat);
    uint64_t bufferOffset = srcOffset;
    for (uint32_t mip = 0; mip < mipLevels; mip++) {
        uint32_t mipLevel = dstRange.BaseMipLevel + mip;
        uint32_t mipWidth = std::max(dst->_width >> mipLevel, 1u);
        uint32_t mipHeight = std::max(dst->_height >> mipLevel, 1u);
        uint32_t mipDepth = is3D ? std::max(dst->_depthOrArraySize >> mipLevel, 1u) : 1u;
        uint64_t tightBytesPerRow = static_cast<uint64_t>(mipWidth) * bpp;
        uint64_t alignedBytesPerRow = Align(tightBytesPerRow, rowPitchAlignment);
        if (alignedBytesPerRow % bpp != 0) {
            RADRAY_ERR_LOG(
                "vk CopyBufferToTexture row pitch cannot be represented in texels (alignedBytesPerRow={}, bpp={})", alignedBytesPerRow, bpp);
            return;
        }
        uint32_t bufferRowLength = static_cast<uint32_t>(alignedBytesPerRow / bpp);
        uint64_t bytesPerImage = alignedBytesPerRow * mipHeight;
        for (uint32_t layer = 0; layer < layerCount; layer++) {
            uint32_t arrayLayer = dstRange.BaseArrayLayer + layer;
            VkBufferImageCopy copyInfo{};
            copyInfo.bufferOffset = bufferOffset;
            copyInfo.bufferRowLength = bufferRowLength;
            copyInfo.bufferImageHeight = mipHeight;
            copyInfo.imageSubresource.aspectMask = aspectMask;
            copyInfo.imageSubresource.mipLevel = mipLevel;
            copyInfo.imageSubresource.baseArrayLayer = arrayLayer;
            copyInfo.imageSubresource.layerCount = 1;
            copyInfo.imageOffset = {0, 0, 0};
            copyInfo.imageExtent = {mipWidth, mipHeight, mipDepth};
            _device->_ftb.vkCmdCopyBufferToImage(_cmdBuffer, src->_buffer, dst->_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyInfo);
            bufferOffset += bytesPerImage * mipDepth;
        }
    }
}

void CommandBufferVulkan::CopyTextureToBuffer(Buffer* dst_, uint64_t dstOffset, Texture* src_, SubresourceRange srcRange) noexcept {
    auto dst = CastVkObject(dst_);
    auto src = CastVkObject(src_);
    const uint32_t bpp = GetTextureFormatBytesPerPixel(src->_format);
    if (bpp == 0) {
        RADRAY_ERR_LOG("vk CopyTextureToBuffer invalid texture format {}", src->_format);
        return;
    }
    if (srcRange.MipLevelCount == SubresourceRange::All || srcRange.ArrayLayerCount == SubresourceRange::All) {
        RADRAY_ERR_LOG("vk CopyTextureToBuffer requires explicit SubresourceRange count");
        return;
    }
    uint32_t mipLevels = srcRange.MipLevelCount;
    uint32_t layerCount = srcRange.ArrayLayerCount;
    if (mipLevels == 0 || layerCount == 0) {
        RADRAY_ERR_LOG("vk CopyTextureToBuffer invalid SubresourceRange count (mipLevels={}, layerCount={})", mipLevels, layerCount);
        return;
    }
    if (srcRange.BaseMipLevel >= src->_mipLevels ||
        srcRange.BaseMipLevel + mipLevels > src->_mipLevels) {
        RADRAY_ERR_LOG("vk CopyTextureToBuffer mip range out of bounds (base={}, count={}, total={})",
                       srcRange.BaseMipLevel, mipLevels, src->_mipLevels);
        return;
    }
    bool is3D = src->_dim == TextureDimension::Dim3D;
    uint32_t arraySize = is3D ? 1u : src->_depthOrArraySize;
    if (srcRange.BaseArrayLayer >= arraySize ||
        srcRange.BaseArrayLayer + layerCount > arraySize) {
        RADRAY_ERR_LOG("vk CopyTextureToBuffer array range out of bounds (base={}, count={}, total={})",
                       srcRange.BaseArrayLayer, layerCount, arraySize);
        return;
    }
    const uint64_t rowPitchAlignment = std::max<uint64_t>(1, _device->_detail.TextureDataPitchAlignment);
    VkImageAspectFlags aspectMask = ImageFormatToAspectFlags(src->_rawFormat);
    uint64_t bufferOffset = dstOffset;
    for (uint32_t mip = 0; mip < mipLevels; mip++) {
        uint32_t mipLevel = srcRange.BaseMipLevel + mip;
        uint32_t mipWidth = std::max(src->_width >> mipLevel, 1u);
        uint32_t mipHeight = std::max(src->_height >> mipLevel, 1u);
        uint32_t mipDepth = is3D ? std::max(src->_depthOrArraySize >> mipLevel, 1u) : 1u;
        uint64_t tightBytesPerRow = static_cast<uint64_t>(mipWidth) * bpp;
        uint64_t alignedBytesPerRow = Align(tightBytesPerRow, rowPitchAlignment);
        if (alignedBytesPerRow % bpp != 0) {
            RADRAY_ERR_LOG(
                "vk CopyTextureToBuffer row pitch cannot be represented in texels (alignedBytesPerRow={}, bpp={})", alignedBytesPerRow, bpp);
            return;
        }
        uint32_t bufferRowLength = static_cast<uint32_t>(alignedBytesPerRow / bpp);
        uint64_t bytesPerImage = alignedBytesPerRow * mipHeight;
        for (uint32_t layer = 0; layer < layerCount; layer++) {
            uint32_t arrayLayer = srcRange.BaseArrayLayer + layer;
            VkBufferImageCopy copyInfo{};
            copyInfo.bufferOffset = bufferOffset;
            copyInfo.bufferRowLength = bufferRowLength;
            copyInfo.bufferImageHeight = mipHeight;
            copyInfo.imageSubresource.aspectMask = aspectMask;
            copyInfo.imageSubresource.mipLevel = mipLevel;
            copyInfo.imageSubresource.baseArrayLayer = arrayLayer;
            copyInfo.imageSubresource.layerCount = 1;
            copyInfo.imageOffset = {0, 0, 0};
            copyInfo.imageExtent = {mipWidth, mipHeight, mipDepth};
            _device->_ftb.vkCmdCopyImageToBuffer(_cmdBuffer, src->_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->_buffer, 1, &copyInfo);
            bufferOffset += bytesPerImage * mipDepth;
        }
    }
}

void CommandBufferVulkan::CopyTextureToTexture(const TextureCopyDescriptor& desc) noexcept {
    if (desc.Source == nullptr || desc.Destination == nullptr) {
        RADRAY_ERR_LOG("vk CopyTextureToTexture source or destination is null");
        return;
    }
    auto* src = CastVkObject(desc.Source);
    auto* dst = CastVkObject(desc.Destination);
    if (!src->IsValid() || !dst->IsValid() ||
        !src->_usage.HasFlag(TextureUse::CopySource) ||
        !dst->_usage.HasFlag(TextureUse::CopyDestination) ||
        src->_format != dst->_format || src->_dim != dst->_dim ||
        src->_sampleCount != dst->_sampleCount || IsDepthStencilFormat(src->_format)) {
        RADRAY_ERR_LOG("vk CopyTextureToTexture requires valid, matching non-depth textures with copy usage");
        return;
    }
    if (desc.Width == 0 || desc.Height == 0 || desc.Depth == 0 || desc.ArrayLayerCount == 0 ||
        desc.SourceMipLevel >= src->_mipLevels || desc.DestinationMipLevel >= dst->_mipLevels) {
        RADRAY_ERR_LOG("vk CopyTextureToTexture invalid extent, layer count, or mip level");
        return;
    }

    const bool is3D = src->_dim == TextureDimension::Dim3D;
    const bool is1D = src->_dim == TextureDimension::Dim1D || src->_dim == TextureDimension::Dim1DArray;
    const uint32_t srcWidth = std::max(src->_width >> desc.SourceMipLevel, 1u);
    const uint32_t srcHeight = is1D ? 1 : std::max(src->_height >> desc.SourceMipLevel, 1u);
    const uint32_t srcDepth = is3D ? std::max(src->_depthOrArraySize >> desc.SourceMipLevel, 1u) : 1u;
    const uint32_t dstWidth = std::max(dst->_width >> desc.DestinationMipLevel, 1u);
    const uint32_t dstHeight = is1D ? 1 : std::max(dst->_height >> desc.DestinationMipLevel, 1u);
    const uint32_t dstDepth = is3D ? std::max(dst->_depthOrArraySize >> desc.DestinationMipLevel, 1u) : 1u;
    if (desc.SourceX > srcWidth || desc.Width > srcWidth - desc.SourceX ||
        desc.SourceY > srcHeight || desc.Height > srcHeight - desc.SourceY ||
        desc.SourceZ > srcDepth || desc.Depth > srcDepth - desc.SourceZ ||
        desc.DestinationX > dstWidth || desc.Width > dstWidth - desc.DestinationX ||
        desc.DestinationY > dstHeight || desc.Height > dstHeight - desc.DestinationY ||
        desc.DestinationZ > dstDepth || desc.Depth > dstDepth - desc.DestinationZ) {
        RADRAY_ERR_LOG("vk CopyTextureToTexture region is out of bounds");
        return;
    }

    const uint32_t srcArraySize = is3D ? 1u : src->_depthOrArraySize;
    const uint32_t dstArraySize = is3D ? 1u : dst->_depthOrArraySize;
    if (is3D) {
        if (desc.SourceArrayLayer != 0 || desc.DestinationArrayLayer != 0 || desc.ArrayLayerCount != 1) {
            RADRAY_ERR_LOG("vk CopyTextureToTexture 3D textures require array layer zero and one layer");
            return;
        }
    } else if (desc.SourceZ != 0 || desc.DestinationZ != 0 || desc.Depth != 1 ||
               desc.SourceArrayLayer >= srcArraySize || desc.ArrayLayerCount > srcArraySize - desc.SourceArrayLayer ||
               desc.DestinationArrayLayer >= dstArraySize || desc.ArrayLayerCount > dstArraySize - desc.DestinationArrayLayer) {
        RADRAY_ERR_LOG("vk CopyTextureToTexture invalid array-layer or depth range");
        return;
    }
    if (is1D && (desc.SourceY != 0 || desc.DestinationY != 0 || desc.Height != 1)) {
        RADRAY_ERR_LOG("vk CopyTextureToTexture 1D textures require a one-texel Y extent");
        return;
    }
    if (src->_sampleCount > 1 &&
        (desc.SourceX != 0 || desc.SourceY != 0 || desc.SourceZ != 0 ||
         desc.DestinationX != 0 || desc.DestinationY != 0 || desc.DestinationZ != 0 ||
         desc.Width != srcWidth || desc.Height != srcHeight || desc.Depth != srcDepth ||
         desc.Width != dstWidth || desc.Height != dstHeight || desc.Depth != dstDepth)) {
        RADRAY_ERR_LOG("vk CopyTextureToTexture multisampled copies must cover whole subresources");
        return;
    }

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = desc.SourceMipLevel;
    region.srcSubresource.baseArrayLayer = desc.SourceArrayLayer;
    region.srcSubresource.layerCount = is3D ? 1u : desc.ArrayLayerCount;
    region.srcOffset = {
        static_cast<int32_t>(desc.SourceX),
        static_cast<int32_t>(desc.SourceY),
        static_cast<int32_t>(desc.SourceZ)};
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.mipLevel = desc.DestinationMipLevel;
    region.dstSubresource.baseArrayLayer = desc.DestinationArrayLayer;
    region.dstSubresource.layerCount = is3D ? 1u : desc.ArrayLayerCount;
    region.dstOffset = {
        static_cast<int32_t>(desc.DestinationX),
        static_cast<int32_t>(desc.DestinationY),
        static_cast<int32_t>(desc.DestinationZ)};
    region.extent = {desc.Width, desc.Height, desc.Depth};
    _device->_ftb.vkCmdCopyImage(
        _cmdBuffer,
        src->_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst->_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region);
}

void CommandBufferVulkan::ResolveTexture(const TextureResolveDescriptor& desc) noexcept {
    if (desc.Source == nullptr || desc.Destination == nullptr) {
        RADRAY_ERR_LOG("vk ResolveTexture source or destination is null");
        return;
    }
    auto* src = CastVkObject(desc.Source);
    auto* dst = CastVkObject(desc.Destination);
    const bool sourceIs2D = src->_dim == TextureDimension::Dim2D ||
                            src->_dim == TextureDimension::Dim2DArray ||
                            src->_dim == TextureDimension::Cube ||
                            src->_dim == TextureDimension::CubeArray;
    const bool destinationIs2D = dst->_dim == TextureDimension::Dim2D ||
                                 dst->_dim == TextureDimension::Dim2DArray ||
                                 dst->_dim == TextureDimension::Cube ||
                                 dst->_dim == TextureDimension::CubeArray;
    if (!src->IsValid() || !dst->IsValid() ||
        !src->_usage.HasFlag(TextureUse::CopySource) ||
        !dst->_usage.HasFlag(TextureUse::CopyDestination) ||
        !sourceIs2D || !destinationIs2D ||
        src->_format != dst->_format || IsDepthStencilFormat(src->_format) ||
        src->_sampleCount <= 1 || dst->_sampleCount != 1 ||
        desc.SourceMipLevel >= src->_mipLevels || desc.DestinationMipLevel >= dst->_mipLevels ||
        desc.ArrayLayerCount == 0) {
        RADRAY_ERR_LOG("vk ResolveTexture requires matching 2D color textures with copy usage, multisampled source, and single-sampled destination");
        return;
    }
    const uint32_t srcWidth = std::max(src->_width >> desc.SourceMipLevel, 1u);
    const uint32_t srcHeight = std::max(src->_height >> desc.SourceMipLevel, 1u);
    const uint32_t dstWidth = std::max(dst->_width >> desc.DestinationMipLevel, 1u);
    const uint32_t dstHeight = std::max(dst->_height >> desc.DestinationMipLevel, 1u);
    if (srcWidth != dstWidth || srcHeight != dstHeight ||
        desc.SourceArrayLayer >= src->_depthOrArraySize || desc.ArrayLayerCount > src->_depthOrArraySize - desc.SourceArrayLayer ||
        desc.DestinationArrayLayer >= dst->_depthOrArraySize || desc.ArrayLayerCount > dst->_depthOrArraySize - desc.DestinationArrayLayer) {
        RADRAY_ERR_LOG("vk ResolveTexture subresource extents or array ranges do not match");
        return;
    }

    VkImageResolve region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = desc.SourceMipLevel;
    region.srcSubresource.baseArrayLayer = desc.SourceArrayLayer;
    region.srcSubresource.layerCount = desc.ArrayLayerCount;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.mipLevel = desc.DestinationMipLevel;
    region.dstSubresource.baseArrayLayer = desc.DestinationArrayLayer;
    region.dstSubresource.layerCount = desc.ArrayLayerCount;
    region.extent = {srcWidth, srcHeight, 1};
    _device->_ftb.vkCmdResolveImage(
        _cmdBuffer,
        src->_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst->_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region);
}

void CommandBufferVulkan::ResetQueryPool(QueryPool* pool_, uint32_t firstIndex, uint32_t count) noexcept {
    auto pool = CastVkObject(pool_);
    if (pool == nullptr || !pool->IsValid() || count == 0 || firstIndex + count > pool->_desc.Count) {
        RADRAY_ERR_LOG("vk ResetQueryPool invalid range (first={}, count={})", firstIndex, count);
        return;
    }
    _device->_ftb.vkCmdResetQueryPool(_cmdBuffer, pool->_pool, firstIndex, count);
}

void CommandBufferVulkan::WriteTimestamp(const QueryTimestampDescriptor& desc) noexcept {
    auto pool = CastVkObject(desc.Pool);
    if (pool == nullptr || !pool->IsValid() || desc.Index >= pool->_desc.Count) {
        RADRAY_ERR_LOG("vk WriteTimestamp invalid query index {}", desc.Index);
        return;
    }
    if (pool->_desc.Type != QueryType::Timestamp) {
        RADRAY_ERR_LOG("vk WriteTimestamp requires a timestamp query pool");
        return;
    }

    VkPipelineStageFlagBits stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    switch (desc.Stage) {
        case QueryPipelineStage::Top:
            stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
        case QueryPipelineStage::Bottom:
            stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            break;
        case QueryPipelineStage::Graphics:
            stage = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
            break;
        case QueryPipelineStage::Compute:
            stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            break;
        case QueryPipelineStage::RayTracing:
            if (!_device->GetDetail().IsRayTracingSupported) {
                RADRAY_ERR_LOG("vk ray tracing timestamp stage requested but ray tracing is not supported");
                return;
            }
            stage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
            break;
    }
    _device->_ftb.vkCmdWriteTimestamp(_cmdBuffer, stage, pool->_pool, desc.Index);
}

void CommandBufferVulkan::ResolveQueryData(const QueryResolveDescriptor& desc) noexcept {
    auto pool = CastVkObject(desc.Pool);
    auto dst = CastVkObject(desc.Destination);
    if (pool == nullptr || !pool->IsValid() || dst == nullptr || !dst->IsValid() ||
        desc.Count == 0 || desc.FirstIndex + desc.Count > pool->_desc.Count) {
        RADRAY_ERR_LOG("vk ResolveQueryData invalid descriptor");
        return;
    }
    if (pool->_desc.Type != QueryType::Timestamp) {
        RADRAY_ERR_LOG("vk ResolveQueryData requires a timestamp query pool");
        return;
    }
    if ((desc.DestinationOffset % alignof(uint64_t)) != 0) {
        RADRAY_ERR_LOG("vk ResolveQueryData destination offset must be 8-byte aligned");
        return;
    }
    const BufferDescriptor dstDesc = dst->GetDesc();
    const uint64_t requiredBytes = desc.DestinationOffset + sizeof(uint64_t) * static_cast<uint64_t>(desc.Count);
    if (dstDesc.Size < requiredBytes) {
        RADRAY_ERR_LOG("vk ResolveQueryData destination buffer is too small (required={}, size={})", requiredBytes, dstDesc.Size);
        return;
    }

    _device->_ftb.vkCmdCopyQueryPoolResults(
        _cmdBuffer,
        pool->_pool,
        desc.FirstIndex,
        desc.Count,
        dst->_buffer,
        desc.DestinationOffset,
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}

SimulateCommandEncoderVulkan::SimulateCommandEncoderVulkan(
    DeviceVulkan* device,
    CommandBufferVulkan* cmdBuffer) noexcept
    : _device(device),
      _cmdBuffer(cmdBuffer) {}

SimulateCommandEncoderVulkan::~SimulateCommandEncoderVulkan() noexcept {
    this->DestroyImpl();
}

bool SimulateCommandEncoderVulkan::IsValid() const noexcept {
    return _device != nullptr && _cmdBuffer != nullptr && _framebuffer != nullptr;
}

void SimulateCommandEncoderVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

CommandBuffer* SimulateCommandEncoderVulkan::GetCommandBuffer() const noexcept {
    return _cmdBuffer;
}

void SimulateCommandEncoderVulkan::DestroyImpl() noexcept {
    _framebuffer = nullptr;
    _boundPso = nullptr;
    _boundPipeLayout = nullptr;
}

void SimulateCommandEncoderVulkan::SetViewport(Viewport vp) noexcept {
    VkViewport v{
        vp.X,
        vp.Y,
        vp.Width,
        vp.Height,
        vp.MinDepth,
        vp.MaxDepth};
    _device->_ftb.vkCmdSetViewport(_cmdBuffer->_cmdBuffer, 0, 1, &v);
}

void SimulateCommandEncoderVulkan::SetScissor(Rect rect) noexcept {
    VkRect2D s{
        {rect.X,
         rect.Y},
        {rect.Width,
         rect.Height}};
    _device->_ftb.vkCmdSetScissor(_cmdBuffer->_cmdBuffer, 0, 1, &s);
}

void SimulateCommandEncoderVulkan::BindVertexBuffer(std::span<const VertexBufferView> vbv) noexcept {
    vector<VkBuffer> buffers;
    vector<VkDeviceSize> offsets;
    for (const auto& view : vbv) {
        auto buffer = CastVkObject(view.Target);
        buffers.push_back(buffer->_buffer);
        offsets.push_back(view.Offset);
    }
    _device->_ftb.vkCmdBindVertexBuffers(_cmdBuffer->_cmdBuffer, 0, static_cast<uint32_t>(buffers.size()), buffers.data(), offsets.data());
}

void SimulateCommandEncoderVulkan::BindIndexBuffer(IndexBufferView ibv) noexcept {
    auto buffer = CastVkObject(ibv.Target);
    auto indexType = MapIndexType(ibv.Stride);
    if (indexType == VK_INDEX_TYPE_MAX_ENUM) {
        RADRAY_ERR_LOG("vk index buffer stride must be 2 or 4 bytes, got {}", ibv.Stride);
        return;
    }
    _device->_ftb.vkCmdBindIndexBuffer(_cmdBuffer->_cmdBuffer, buffer->_buffer, ibv.Offset, indexType);
}

void SimulateCommandEncoderVulkan::BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept {
    auto p = CastVkObject(pso);
    if (_boundPso == p) {
        return;
    }
    _device->_ftb.vkCmdBindPipeline(_cmdBuffer->_cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, p->_pipeline);
    _boundPso = p;
}

void SimulateCommandEncoderVulkan::BindBindingGroup(
    uint32_t groupIndex,
    BindingGroup* group,
    std::span<const uint32_t> dynamicOffsets) noexcept {
    _BindBindingGroupVulkan(
        _device, _cmdBuffer, _boundPipeLayout, groupIndex, group, dynamicOffsets,
        VK_PIPELINE_BIND_POINT_GRAPHICS);
}

bool SimulateCommandEncoderVulkan::SetPushConstants(
    PipelineLayout* layout,
    uint32_t groupIndex,
    uint32_t binding,
    std::span<const byte> data) noexcept {
    _boundPipeLayout = CastVkObject(layout);
    return _PushConstantsVulkan(
        _device, _cmdBuffer, _boundPipeLayout, groupIndex, binding, data);
}

void SimulateCommandEncoderVulkan::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
    _device->_ftb.vkCmdDraw(_cmdBuffer->_cmdBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void SimulateCommandEncoderVulkan::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
    _device->_ftb.vkCmdDrawIndexed(_cmdBuffer->_cmdBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

static Nullable<BufferVulkan*> _ValidateIndirectBufferVulkan(
    Buffer* argumentBuffer,
    uint64_t argumentOffset,
    uint32_t commandCount,
    uint32_t commandStride) noexcept {
    if (argumentBuffer == nullptr || commandCount == 0 || argumentOffset % 4 != 0) {
        RADRAY_ERR_LOG("vk indirect command has a null buffer, zero count, or unaligned offset");
        return nullptr;
    }
    auto* buffer = CastVkObject(argumentBuffer);
    const uint64_t requiredSize = static_cast<uint64_t>(commandCount) * commandStride;
    if (!buffer->IsValid() || !buffer->_usage.HasFlag(BufferUse::Indirect) ||
        argumentOffset > buffer->_reqSizeLogical || requiredSize > buffer->_reqSizeLogical - argumentOffset) {
        RADRAY_ERR_LOG("vk indirect argument buffer is invalid, lacks BufferUse::Indirect, or is out of bounds");
        return nullptr;
    }
    return buffer;
}

void SimulateCommandEncoderVulkan::DrawIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount) noexcept {
    auto buffer = _ValidateIndirectBufferVulkan(
        argumentBuffer, argumentOffset, drawCount, sizeof(DrawIndirectArguments));
    if (!buffer) {
        return;
    }
    const uint32_t maxBatchSize = _device->_feature.multiDrawIndirect
                                      ? std::max(_device->_properties.limits.maxDrawIndirectCount, 1u)
                                      : 1u;
    for (uint32_t firstDraw = 0; firstDraw < drawCount;) {
        const uint32_t batchSize = std::min(drawCount - firstDraw, maxBatchSize);
        _device->_ftb.vkCmdDrawIndirect(
            _cmdBuffer->_cmdBuffer,
            buffer.Get()->_buffer,
            argumentOffset + static_cast<uint64_t>(firstDraw) * sizeof(DrawIndirectArguments),
            batchSize,
            sizeof(DrawIndirectArguments));
        firstDraw += batchSize;
    }
}

void SimulateCommandEncoderVulkan::DrawIndexedIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount) noexcept {
    auto buffer = _ValidateIndirectBufferVulkan(
        argumentBuffer, argumentOffset, drawCount, sizeof(DrawIndexedIndirectArguments));
    if (!buffer) {
        return;
    }
    const uint32_t maxBatchSize = _device->_feature.multiDrawIndirect
                                      ? std::max(_device->_properties.limits.maxDrawIndirectCount, 1u)
                                      : 1u;
    for (uint32_t firstDraw = 0; firstDraw < drawCount;) {
        const uint32_t batchSize = std::min(drawCount - firstDraw, maxBatchSize);
        _device->_ftb.vkCmdDrawIndexedIndirect(
            _cmdBuffer->_cmdBuffer,
            buffer.Get()->_buffer,
            argumentOffset + static_cast<uint64_t>(firstDraw) * sizeof(DrawIndexedIndirectArguments),
            batchSize,
            sizeof(DrawIndexedIndirectArguments));
        firstDraw += batchSize;
    }
}

SimulateComputeEncoderVulkan::SimulateComputeEncoderVulkan(
    DeviceVulkan* device,
    CommandBufferVulkan* cmdBuffer) noexcept
    : _device(device),
      _cmdBuffer(cmdBuffer) {}

SimulateComputeEncoderVulkan::~SimulateComputeEncoderVulkan() noexcept {
    DestroyImpl();
}

bool SimulateComputeEncoderVulkan::IsValid() const noexcept {
    return _device != nullptr;
}

void SimulateComputeEncoderVulkan::Destroy() noexcept {
    DestroyImpl();
}

void SimulateComputeEncoderVulkan::DestroyImpl() noexcept {
    _device = nullptr;
    _cmdBuffer = nullptr;
    _boundPipeLayout = nullptr;
}

CommandBuffer* SimulateComputeEncoderVulkan::GetCommandBuffer() const noexcept {
    return _cmdBuffer;
}

void SimulateComputeEncoderVulkan::BindBindingGroup(
    uint32_t groupIndex,
    BindingGroup* group,
    std::span<const uint32_t> dynamicOffsets) noexcept {
    _BindBindingGroupVulkan(
        _device, _cmdBuffer, _boundPipeLayout, groupIndex, group, dynamicOffsets,
        VK_PIPELINE_BIND_POINT_COMPUTE);
}

bool SimulateComputeEncoderVulkan::SetPushConstants(
    PipelineLayout* layout,
    uint32_t groupIndex,
    uint32_t binding,
    std::span<const byte> data) noexcept {
    _boundPipeLayout = CastVkObject(layout);
    return _PushConstantsVulkan(
        _device, _cmdBuffer, _boundPipeLayout, groupIndex, binding, data);
}

void SimulateComputeEncoderVulkan::BindComputePipelineState(ComputePipelineState* pso) noexcept {
    auto p = CastVkObject(pso);
    _device->_ftb.vkCmdBindPipeline(_cmdBuffer->_cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, p->_pipeline);
}

void SimulateComputeEncoderVulkan::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
    _device->_ftb.vkCmdDispatch(_cmdBuffer->_cmdBuffer, groupCountX, groupCountY, groupCountZ);
}

void SimulateComputeEncoderVulkan::DispatchIndirect(Buffer* argumentBuffer, uint64_t argumentOffset) noexcept {
    auto buffer = _ValidateIndirectBufferVulkan(
        argumentBuffer, argumentOffset, 1, sizeof(DispatchIndirectArguments));
    if (!buffer) {
        return;
    }
    _device->_ftb.vkCmdDispatchIndirect(
        _cmdBuffer->_cmdBuffer,
        buffer.Get()->_buffer,
        argumentOffset);
}

CommandEncoderRayTracingVulkan::CommandEncoderRayTracingVulkan(
    DeviceVulkan* device,
    CommandBufferVulkan* cmdBuffer) noexcept
    : _device(device),
      _cmdBuffer(cmdBuffer) {}

CommandEncoderRayTracingVulkan::~CommandEncoderRayTracingVulkan() noexcept {
    this->DestroyImpl();
}

bool CommandEncoderRayTracingVulkan::IsValid() const noexcept {
    return _device != nullptr;
}

void CommandEncoderRayTracingVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void CommandEncoderRayTracingVulkan::DestroyImpl() noexcept {
    _keepAliveBuffers.clear();
    _boundRtPipeline = nullptr;
    _boundPipeLayout = nullptr;
    _cmdBuffer = nullptr;
    _device = nullptr;
}

CommandBuffer* CommandEncoderRayTracingVulkan::GetCommandBuffer() const noexcept {
    return _cmdBuffer;
}

void CommandEncoderRayTracingVulkan::BindBindingGroup(
    uint32_t groupIndex,
    BindingGroup* group,
    std::span<const uint32_t> dynamicOffsets) noexcept {
    _BindBindingGroupVulkan(
        _device, _cmdBuffer, _boundPipeLayout, groupIndex, group, dynamicOffsets,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
}

bool CommandEncoderRayTracingVulkan::SetPushConstants(
    PipelineLayout* layout,
    uint32_t groupIndex,
    uint32_t binding,
    std::span<const byte> data) noexcept {
    _boundPipeLayout = CastVkObject(layout);
    return _PushConstantsVulkan(
        _device, _cmdBuffer, _boundPipeLayout, groupIndex, binding, data);
}

void CommandEncoderRayTracingVulkan::BuildBottomLevelAS(const BuildBottomLevelASDescriptor& desc) noexcept {
    auto target = CastVkObject(desc.Target);
    auto scratch = CastVkObject(desc.ScratchBuffer);
    if (target->_desc.Type != AccelerationStructureType::BottomLevel) {
        RADRAY_ERR_LOG("BuildBottomLevelAS target type mismatch");
        return;
    }
    if (desc.Mode == AccelerationStructureBuildMode::Update &&
        !target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowUpdate)) {
        RADRAY_ERR_LOG("BuildBottomLevelAS update requested without AllowUpdate flag");
        return;
    }
    auto getBufferAddress = [&](BufferVulkan* b) -> VkDeviceAddress {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = b->_buffer;
        return _device->_ftb.vkGetBufferDeviceAddress(_device->_device, &info);
    };
    vector<VkAccelerationStructureGeometryKHR> geometries{};
    vector<VkAccelerationStructureBuildRangeInfoKHR> ranges{};
    vector<uint32_t> primitiveCounts{};
    geometries.reserve(desc.Geometries.size());
    ranges.reserve(desc.Geometries.size());
    primitiveCounts.reserve(desc.Geometries.size());
    for (const auto& geom : desc.Geometries) {
        VkAccelerationStructureGeometryKHR g{};
        g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g.flags = geom.Opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        if (const auto* tri = std::get_if<RayTracingTrianglesDescriptor>(&geom.Geometry)) {
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            g.geometry.triangles.vertexFormat = MapType(tri->VertexFmt);
            g.geometry.triangles.vertexData.deviceAddress = getBufferAddress(CastVkObject(tri->VertexBuffer)) + tri->VertexOffset;
            g.geometry.triangles.vertexStride = tri->VertexStride;
            g.geometry.triangles.maxVertex = tri->VertexCount == 0 ? 0 : (tri->VertexCount - 1);
            if (tri->IndexBuffer != nullptr) {
                g.geometry.triangles.indexType = tri->IndexFmt == IndexFormat::UINT16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
                g.geometry.triangles.indexData.deviceAddress = getBufferAddress(CastVkObject(tri->IndexBuffer)) + tri->IndexOffset;
                range.primitiveCount = tri->IndexCount / 3;
            } else {
                g.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
                range.primitiveCount = tri->VertexCount / 3;
            }
            if (tri->TransformBuffer != nullptr) {
                g.geometry.triangles.transformData.deviceAddress = getBufferAddress(CastVkObject(tri->TransformBuffer)) + tri->TransformOffset;
            }
        } else {
            const auto* aabb = std::get_if<RayTracingAABBsDescriptor>(&geom.Geometry);
            RADRAY_ASSERT(aabb != nullptr);
            g.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
            g.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
            g.geometry.aabbs.data.deviceAddress = getBufferAddress(CastVkObject(aabb->Target)) + aabb->Offset;
            g.geometry.aabbs.stride = aabb->Stride;
            range.primitiveCount = aabb->Count;
        }
        range.primitiveOffset = 0;
        range.firstVertex = 0;
        range.transformOffset = 0;
        primitiveCounts.push_back(range.primitiveCount);
        geometries.push_back(g);
        ranges.push_back(range);
    }

    VkBuildAccelerationStructureFlagsKHR buildFlags = 0;
    if (target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::PreferFastTrace)) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    if (target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::PreferFastBuild)) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    }
    if (target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowUpdate)) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    if (target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowCompaction)) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = buildFlags;
    buildInfo.mode = desc.Mode == AccelerationStructureBuildMode::Update
                         ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                         : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure = desc.Mode == AccelerationStructureBuildMode::Update ? target->_accelerationStructure : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure = target->_accelerationStructure;
    buildInfo.geometryCount = static_cast<uint32_t>(geometries.size());
    buildInfo.pGeometries = geometries.data();

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    _device->_ftb.vkGetAccelerationStructureBuildSizesKHR(
        _device->_device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo,
        primitiveCounts.data(),
        &sizeInfo);
    if (target->_asSize < sizeInfo.accelerationStructureSize) {
        RADRAY_ERR_LOG("BLAS target AS buffer too small: need={}, actual={}", sizeInfo.accelerationStructureSize, target->_asSize);
        return;
    }
    if (desc.ScratchSize < sizeInfo.buildScratchSize) {
        RADRAY_ERR_LOG("BLAS scratch buffer too small: need={}, actual={}", sizeInfo.buildScratchSize, desc.ScratchSize);
        return;
    }
    buildInfo.scratchData.deviceAddress = getBufferAddress(scratch) + desc.ScratchOffset;
    const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = ranges.data();
    _device->_ftb.vkCmdBuildAccelerationStructuresKHR(_cmdBuffer->_cmdBuffer, 1, &buildInfo, &rangePtr);
}

void CommandEncoderRayTracingVulkan::BuildTopLevelAS(const BuildTopLevelASDescriptor& desc) noexcept {
    auto target = CastVkObject(desc.Target);
    auto scratch = CastVkObject(desc.ScratchBuffer);
    if (target->_desc.Type != AccelerationStructureType::TopLevel) {
        RADRAY_ERR_LOG("BuildTopLevelAS target type mismatch");
        return;
    }
    if (desc.Mode == AccelerationStructureBuildMode::Update &&
        !target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowUpdate)) {
        RADRAY_ERR_LOG("BuildTopLevelAS update requested without AllowUpdate flag");
        return;
    }
    for (const RayTracingInstanceDescriptor& instance : desc.Instances) {
        if (instance.Blas == nullptr) {
            RADRAY_ERR_LOG("BuildTopLevelAS instance has a null BLAS");
            return;
        }
        auto* blas = CastVkObject(instance.Blas);
        if (!blas->IsValid() || blas->_device != _device ||
            blas->_desc.Type != AccelerationStructureType::BottomLevel) {
            RADRAY_ERR_LOG("BuildTopLevelAS instance has an invalid BLAS");
            return;
        }
    }
    auto getBufferAddress = [&](BufferVulkan* b) -> VkDeviceAddress {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = b->_buffer;
        return _device->_ftb.vkGetBufferDeviceAddress(_device->_device, &info);
    };
    const uint64_t instanceBytes = sizeof(VkAccelerationStructureInstanceKHR) * desc.Instances.size();
    auto instBufOpt = _device->CreateBuffer(BufferDescriptor{
        .Size = Align(instanceBytes, 16ull),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::Scratch | BufferUse::MapWrite,
        .Hints = ResourceHint::None});
    if (!instBufOpt.HasValue()) {
        return;
    }
    auto instBuf = instBufOpt.Release();
    instBuf->SetDebugName("vk_tlas_instances");
    auto instBufRaw = CastVkObject(instBuf.get());

    VkBuildAccelerationStructureFlagsKHR buildFlags = 0;
    if (target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::PreferFastTrace)) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    if (target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::PreferFastBuild)) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    }
    if (target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowUpdate)) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    if (target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowCompaction)) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    }

    VkAccelerationStructureGeometryInstancesDataKHR instData{};
    instData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instData.arrayOfPointers = VK_FALSE;
    instData.data.deviceAddress = getBufferAddress(instBufRaw);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instData;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = buildFlags;
    buildInfo.mode = desc.Mode == AccelerationStructureBuildMode::Update
                         ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                         : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure = desc.Mode == AccelerationStructureBuildMode::Update ? target->_accelerationStructure : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure = target->_accelerationStructure;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    uint32_t primitiveCount = static_cast<uint32_t>(desc.Instances.size());
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    _device->_ftb.vkGetAccelerationStructureBuildSizesKHR(
        _device->_device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo,
        &primitiveCount,
        &sizeInfo);
    if (target->_asSize < sizeInfo.accelerationStructureSize) {
        RADRAY_ERR_LOG("TLAS target AS buffer too small: need={}, actual={}", sizeInfo.accelerationStructureSize, target->_asSize);
        return;
    }
    if (desc.ScratchSize < sizeInfo.buildScratchSize) {
        RADRAY_ERR_LOG("TLAS scratch buffer too small: need={}, actual={}", sizeInfo.buildScratchSize, desc.ScratchSize);
        return;
    }

    auto* mapped = static_cast<VkAccelerationStructureInstanceKHR*>(instBuf->Map(0, instanceBytes));
    if (mapped == nullptr) {
        RADRAY_ERR_LOG("failed to map TLAS instance buffer");
        return;
    }
    auto unmapGuard = MakeScopeGuard([&]() noexcept { instBuf->Unmap(); });
    for (size_t i = 0; i < desc.Instances.size(); i++) {
        const auto& src = desc.Instances[i];
        auto& dst = mapped[i];
        std::memset(&dst, 0, sizeof(dst));
        dst.transform.matrix[0][0] = src.Transform(0, 0);
        dst.transform.matrix[0][1] = src.Transform(0, 1);
        dst.transform.matrix[0][2] = src.Transform(0, 2);
        dst.transform.matrix[0][3] = src.Transform(0, 3);
        dst.transform.matrix[1][0] = src.Transform(1, 0);
        dst.transform.matrix[1][1] = src.Transform(1, 1);
        dst.transform.matrix[1][2] = src.Transform(1, 2);
        dst.transform.matrix[1][3] = src.Transform(1, 3);
        dst.transform.matrix[2][0] = src.Transform(2, 0);
        dst.transform.matrix[2][1] = src.Transform(2, 1);
        dst.transform.matrix[2][2] = src.Transform(2, 2);
        dst.transform.matrix[2][3] = src.Transform(2, 3);
        dst.instanceCustomIndex = src.InstanceID;
        dst.mask = src.InstanceMask;
        dst.instanceShaderBindingTableRecordOffset = src.InstanceContributionToHitGroupIndex;
        dst.flags = 0;
        if (src.ForceOpaque) dst.flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        if (src.ForceNoOpaque) dst.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        dst.accelerationStructureReference = CastVkObject(src.Blas)->_deviceAddress;
    }
    instBuf->FlushMappedRange(BufferRange{.Offset = 0, .Size = instanceBytes});
    instBuf->Unmap();
    unmapGuard.Dismiss();
    buildInfo.scratchData.deviceAddress = getBufferAddress(scratch) + desc.ScratchOffset;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &range;
    _device->_ftb.vkCmdBuildAccelerationStructuresKHR(_cmdBuffer->_cmdBuffer, 1, &buildInfo, &rangePtr);
    _keepAliveBuffers.emplace_back(std::move(instBuf));
}

void CommandEncoderRayTracingVulkan::BindRayTracingPipelineState(RayTracingPipelineState* pso) noexcept {
    _boundRtPipeline = CastVkObject(pso);
}

void CommandEncoderRayTracingVulkan::TraceRays(const TraceRaysDescriptor& desc) noexcept {
    if (_boundRtPipeline == nullptr) {
        RADRAY_ERR_LOG("bind ray tracing pipeline state before TraceRays");
        return;
    }
    TraceRaysDescriptor resolved = desc;
    if (desc.Sbt != nullptr) {
        auto regions = desc.Sbt->GetRegions();
        resolved.RayGen = regions.RayGen;
        resolved.Miss = regions.Miss;
        resolved.HitGroup = regions.HitGroup;
        resolved.Callable = regions.Callable;
    }
    auto toRegion = [&](const ShaderBindingTableRegion& r) {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = CastVkObject(r.Target)->_buffer;
        VkDeviceAddress base = _device->_ftb.vkGetBufferDeviceAddress(_device->_device, &info);
        VkStridedDeviceAddressRegionKHR out{};
        out.deviceAddress = base + r.Offset;
        out.size = r.Size;
        out.stride = r.Stride;
        return out;
    };
    VkStridedDeviceAddressRegionKHR rayGen = toRegion(resolved.RayGen);
    VkStridedDeviceAddressRegionKHR miss = toRegion(resolved.Miss);
    VkStridedDeviceAddressRegionKHR hit = toRegion(resolved.HitGroup);
    VkStridedDeviceAddressRegionKHR callable{};
    if (resolved.Callable.has_value()) {
        callable = toRegion(resolved.Callable.value());
    }
    _device->_ftb.vkCmdBindPipeline(_cmdBuffer->_cmdBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _boundRtPipeline->_pipeline);
    _device->_ftb.vkCmdTraceRaysKHR(
        _cmdBuffer->_cmdBuffer,
        &rayGen,
        &miss,
        &hit,
        &callable,
        resolved.Width,
        resolved.Height,
        resolved.Depth);
}

RenderPassVulkan::RenderPassVulkan(
    DeviceVulkan* device,
    VkRenderPass renderPass,
    const RenderPassDescriptor& desc)
    : RenderPass(desc),
      _device(device),
      _renderPass(renderPass) {}

RenderPassVulkan::~RenderPassVulkan() noexcept {
    this->DestroyImpl();
}

bool RenderPassVulkan::IsValid() const noexcept {
    return _renderPass != VK_NULL_HANDLE;
}

void RenderPassVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void RenderPassVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(name, _renderPass);
}

void RenderPassVulkan::DestroyImpl() noexcept {
    if (_renderPass != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyRenderPass(_device->_device, _renderPass, _device->GetAllocationCallbacks());
        _renderPass = VK_NULL_HANDLE;
    }
}

FrameBufferVulkan::FrameBufferVulkan(
    DeviceVulkan* device,
    VkFramebuffer framebuffer,
    const FramebufferDescriptor& desc)
    : Framebuffer(desc),
      _device(device),
      _framebuffer(framebuffer) {}

FrameBufferVulkan::~FrameBufferVulkan() noexcept {
    this->DestroyImpl();
}

bool FrameBufferVulkan::IsValid() const noexcept {
    return _framebuffer != VK_NULL_HANDLE;
}

void FrameBufferVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void FrameBufferVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(name, _framebuffer);
}

void FrameBufferVulkan::DestroyImpl() noexcept {
    if (_framebuffer != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyFramebuffer(_device->_device, _framebuffer, _device->GetAllocationCallbacks());
        _framebuffer = VK_NULL_HANDLE;
    }
}

LegacyFenceVulkan::LegacyFenceVulkan(
    DeviceVulkan* device,
    VkFence fence) noexcept
    : _device(device),
      _fence(fence) {}

LegacyFenceVulkan::~LegacyFenceVulkan() noexcept {
    this->DestroyImpl();
}

bool LegacyFenceVulkan::IsValid() const noexcept {
    return _fence != VK_NULL_HANDLE;
}

void LegacyFenceVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void LegacyFenceVulkan::Wait() noexcept {
    if (auto vr = _device->_ftb.vkWaitForFences(_device->_device, 1, &_fence, VK_TRUE, UINT64_MAX);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkWaitForFences failed: {}", vr);
    }
    _device->_ftb.vkResetFences(_device->_device, 1, &_fence);
}

void LegacyFenceVulkan::DestroyImpl() noexcept {
    if (_fence != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyFence(_device->_device, _fence, _device->GetAllocationCallbacks());
        _fence = VK_NULL_HANDLE;
    }
}

LegacySemaphoreVulkan::LegacySemaphoreVulkan(
    DeviceVulkan* device,
    VkSemaphore semaphore) noexcept
    : _device(device),
      _semaphore(semaphore) {}

LegacySemaphoreVulkan::~LegacySemaphoreVulkan() noexcept {
    this->DestroyImpl();
}

bool LegacySemaphoreVulkan::IsValid() const noexcept {
    return _semaphore != VK_NULL_HANDLE;
}

void LegacySemaphoreVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void LegacySemaphoreVulkan::DestroyImpl() noexcept {
    if (_semaphore != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroySemaphore(_device->_device, _semaphore, _device->GetAllocationCallbacks());
        _semaphore = VK_NULL_HANDLE;
    }
}

FenceVulkan::FenceVulkan(
    DeviceVulkan* device,
    unique_ptr<TimelineSemaphoreVulkan> timelineSemaphore) noexcept
    : _device(device),
      _fence(std::move(timelineSemaphore)),
      _fenceValue(1) {}

FenceVulkan::~FenceVulkan() noexcept {
    this->DestroyImpl();
}

void FenceVulkan::DestroyImpl() noexcept {
    _fence.reset();
    _fenceValue = 0;
}

bool FenceVulkan::IsValid() const noexcept {
    return _fence != nullptr && _fence->IsValid();
}

void FenceVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void FenceVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(fmt::format("FenceTimeline_{}", name), _fence->_semaphore);
}

uint64_t FenceVulkan::GetCompletedValue() const noexcept {
    return _fence->GetCompletedValue();
}

uint64_t FenceVulkan::GetLastSignaledValue() const noexcept {
    return _fenceValue - 1;
}

void FenceVulkan::Wait() noexcept {
    uint64_t completedValue = _fence->GetCompletedValue();
    uint64_t signaledValue = _fenceValue - 1;
    if (completedValue < signaledValue) {
        VkSemaphore waitSemaphore = _fence->_semaphore;
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.pNext = nullptr;
        waitInfo.flags = 0;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &waitSemaphore;
        waitInfo.pValues = &signaledValue;
        if (auto vr = _device->_ftb.vkWaitSemaphores(_device->_device, &waitInfo, UINT64_MAX);
            vr != VK_SUCCESS) {
            RADRAY_ABORT("vkWaitSemaphores failed: {}", vr);
        }
    }
}

void FenceVulkan::Wait(uint64_t value) noexcept {
    uint64_t completedValue = _fence->GetCompletedValue();
    if (completedValue < value) {
        VkSemaphore waitSemaphore = _fence->_semaphore;
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.pNext = nullptr;
        waitInfo.flags = 0;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &waitSemaphore;
        waitInfo.pValues = &value;
        if (auto vr = _device->_ftb.vkWaitSemaphores(_device->_device, &waitInfo, UINT64_MAX);
            vr != VK_SUCCESS) {
            RADRAY_ABORT("vkWaitSemaphores failed: {}", vr);
        }
    }
}

TimelineSemaphoreVulkan::TimelineSemaphoreVulkan(
    DeviceVulkan* device,
    VkSemaphore semaphore) noexcept
    : _device(device),
      _semaphore(semaphore) {}

TimelineSemaphoreVulkan::~TimelineSemaphoreVulkan() noexcept {
    this->DestroyImpl();
}

bool TimelineSemaphoreVulkan::IsValid() const noexcept {
    return _semaphore != VK_NULL_HANDLE;
}

void TimelineSemaphoreVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void TimelineSemaphoreVulkan::DestroyImpl() noexcept {
    if (_semaphore != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroySemaphore(_device->_device, _semaphore, _device->GetAllocationCallbacks());
        _semaphore = VK_NULL_HANDLE;
    }
}

uint64_t TimelineSemaphoreVulkan::GetCompletedValue() const noexcept {
    uint64_t result;
    if (auto vr = _device->_ftb.vkGetSemaphoreCounterValue(_device->_device, _semaphore, &result);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkGetSemaphoreCounterValue failed: {}", vr);
    }
    return result;
}

SurfaceVulkan::SurfaceVulkan(
    DeviceVulkan* device,
    VkSurfaceKHR surface) noexcept
    : _device(device),
      _surface(surface) {}

SurfaceVulkan::~SurfaceVulkan() noexcept {
    this->DestroyImpl();
}

bool SurfaceVulkan::IsValid() const noexcept {
    return _surface != VK_NULL_HANDLE;
}

void SurfaceVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void SurfaceVulkan::DestroyImpl() noexcept {
    if (_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(_device->_instance->_instance, _surface, _device->GetAllocationCallbacks());
        _surface = VK_NULL_HANDLE;
    }
}

SwapChainSyncObjectVulkan::SwapChainSyncObjectVulkan(unique_ptr<LegacySemaphoreVulkan> semaphore) noexcept
    : _semaphore(std::move(semaphore)) {}

bool SwapChainSyncObjectVulkan::IsValid() const noexcept {
    return _semaphore != nullptr && _semaphore->IsValid();
}

void SwapChainSyncObjectVulkan::Destroy() noexcept {
    this->ClearPendingQueueUse();
    _semaphore.reset();
}

bool SwapChainSyncObjectVulkan::IsPendingQueueUseComplete() const noexcept {
    return _pendingQueueFence == nullptr ||
           _pendingQueueFence->GetCompletedValue() >= _pendingQueueValue;
}

void SwapChainSyncObjectVulkan::ClearPendingQueueUse() noexcept {
    _pendingQueueFence = nullptr;
    _pendingQueueValue = 0;
}

void SwapChainSyncObjectVulkan::WaitPendingQueueUse() noexcept {
    if (_pendingQueueFence != nullptr) {
        _pendingQueueFence->Wait(_pendingQueueValue);
        this->ClearPendingQueueUse();
    }
}

SwapChainVulkan::SwapChainVulkan(
    DeviceVulkan* device,
    QueueVulkan* queue,
    unique_ptr<SurfaceVulkan> surface,
    VkSwapchainKHR swapchain,
    const SwapChainDescriptor& desc) noexcept
    : _device(device),
      _queue(queue),
      _surface(std::move(surface)),
      _swapchain(swapchain),
      _nativeHandler(desc.NativeHandler),
      _width(desc.Width),
      _height(desc.Height),
      _reqFormat(desc.Format),
      _mode(desc.PresentMode) {}

SwapChainVulkan::~SwapChainVulkan() noexcept {
    this->DestroyImpl();
}

bool SwapChainVulkan::IsValid() const noexcept {
    return _surface != nullptr && _swapchain != VK_NULL_HANDLE;
}

void SwapChainVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

Nullable<unique_ptr<SwapChainSyncObjectVulkan>> SwapChainVulkan::AcquireSyncObjectFromPool() noexcept {
    for (auto it = _recycledSyncObjects.begin(); it != _recycledSyncObjects.end(); ++it) {
        if ((*it)->IsPendingQueueUseComplete()) {
            auto result = std::move(*it);
            result->ClearPendingQueueUse();
            _recycledSyncObjects.erase(it);
            return result;
        }
    }
    if (!_recycledSyncObjects.empty()) {
        auto result = std::move(_recycledSyncObjects.front());
        _recycledSyncObjects.erase(_recycledSyncObjects.begin());
        result->WaitPendingQueueUse();
        return result;
    }
    auto semaphoreOpt = _device->CreateLegacySemaphore(0);
    if (!semaphoreOpt.HasValue()) {
        return nullptr;
    }
    return make_unique<SwapChainSyncObjectVulkan>(semaphoreOpt.Release());
}

void SwapChainVulkan::RecycleSyncObject(unique_ptr<SwapChainSyncObjectVulkan> syncObject) noexcept {
    if (syncObject != nullptr) {
        _recycledSyncObjects.emplace_back(std::move(syncObject));
    }
}

void SwapChainVulkan::DestroyImpl() noexcept {
    RADRAY_ASSERT(!_outstandingAcquire.IsValid());
    _outstandingAcquire.Reset();
    _recycledSyncObjects.clear();
    _frames.clear();
    if (_swapchain != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroySwapchainKHR(_device->_device, _swapchain, _device->GetAllocationCallbacks());
        _swapchain = VK_NULL_HANDLE;
    }
    _surface.reset();
}

SwapChainAcquireResult SwapChainVulkan::AcquireNext(uint64_t timeoutMs) noexcept {
    SwapChainAcquireResult result{};
    RADRAY_ASSERT(!_outstandingAcquire.IsValid());
    if (_outstandingAcquire.IsValid()) {
        RADRAY_ERR_LOG("vkAcquireNextImageKHR called before Present");
        result.Status = SwapChainStatus::Error;
        result.NativeStatusCode = -1;
        return result;
    }

    auto waitToDrawOpt = this->AcquireSyncObjectFromPool();
    if (!waitToDrawOpt.HasValue()) {
        RADRAY_ERR_LOG("vk acquire failed: cannot allocate wait semaphore");
        result.Status = SwapChainStatus::Error;
        return result;
    }

    auto waitToDraw = waitToDrawOpt.Release();

    uint64_t nanoTimeout;
    if (timeoutMs == std::numeric_limits<uint64_t>::max()) {
        nanoTimeout = std::numeric_limits<uint64_t>::max();
    } else if (timeoutMs > std::numeric_limits<uint64_t>::max() / (1000ull * 1000ull)) {
        nanoTimeout = std::numeric_limits<uint64_t>::max();
    } else {
        nanoTimeout = timeoutMs * 1000ull * 1000ull;
    }
    uint32_t imageIndex = std::numeric_limits<uint32_t>::max();
    auto vr = _device->_ftb.vkAcquireNextImageKHR(
        _device->_device,
        _swapchain,
        nanoTimeout,
        waitToDraw->_semaphore->_semaphore,
        VK_NULL_HANDLE,
        &imageIndex);
    if (vr == VK_SUCCESS || vr == VK_SUBOPTIMAL_KHR) {
        Frame& imageFrame = _frames[imageIndex];
        if (imageFrame.readyToPresent == nullptr) {
            RADRAY_ERR_LOG("vk acquire failed: missing per-image present semaphore");
            this->RecycleSyncObject(std::move(waitToDraw));
            result.Status = SwapChainStatus::Error;
            result.NativeStatusCode = -1;
            return result;
        }
        if (imageFrame.acquireSyncObject != nullptr) {
            this->RecycleSyncObject(std::move(imageFrame.acquireSyncObject));
        }
        imageFrame.acquireSyncObject = std::move(waitToDraw);
        _outstandingAcquire.imageIndex = imageIndex;
        _outstandingAcquire.waitToDraw = imageFrame.acquireSyncObject.get();
        _outstandingAcquire.readyToPresent = imageFrame.readyToPresent.get();
        ++_outstandingFrameToken;
        SwapChainFrame frame = MakeFrame(
            this,
            _outstandingFrameToken,
            imageFrame.image.get(),
            imageIndex,
            _outstandingAcquire.waitToDraw,
            _outstandingAcquire.readyToPresent);
        result.Status = SwapChainStatus::Success;
        result.NativeStatusCode = vr;
        result.Frame = std::move(frame);
        return result;
    }
    if (vr == VK_TIMEOUT || vr == VK_NOT_READY) {
        this->RecycleSyncObject(std::move(waitToDraw));
        result.Status = SwapChainStatus::RetryLater;
        result.NativeStatusCode = vr;
        return result;
    }
    if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
        this->RecycleSyncObject(std::move(waitToDraw));
        result.Status = SwapChainStatus::RequireRecreate;
        result.NativeStatusCode = vr;
        RADRAY_WARN_LOG("vkAcquireNextImageKHR failed: {}", vr);
        return result;
    }

    this->RecycleSyncObject(std::move(waitToDraw));
    result.NativeStatusCode = vr;
    result.Status = SwapChainStatus::Error;
    RADRAY_ERR_LOG("vkAcquireNextImageKHR failed: {}", vr);
    return result;
}

SwapChainPresentResult SwapChainVulkan::Present(SwapChainFrame&& frame) noexcept {
    SwapChainPresentResult result{};
    RADRAY_ASSERT(frame.IsValid());
    RADRAY_ASSERT(ValidateFrame(frame, this, _outstandingFrameToken));
    RADRAY_ASSERT(_outstandingAcquire.IsValid());
    if (!ValidateFrame(frame, this, _outstandingFrameToken) || !_outstandingAcquire.IsValid()) {
        RADRAY_ERR_LOG("vkQueuePresentKHR skipped: invalid or foreign SwapChainFrame");
        InvalidateFrame(frame);
        result.Status = SwapChainStatus::Error;
        result.NativeStatusCode = -1;
        return result;
    }

    auto* readyToPresentSync = CastVkObject(static_cast<SwapChainSyncObject*>(_outstandingAcquire.readyToPresent));
    const VkSemaphore waitSem = readyToPresentSync->_semaphore->_semaphore;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &waitSem;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.pImageIndices = &_outstandingAcquire.imageIndex;
    presentInfo.pResults = nullptr;
    auto presentResult = _device->_ftb.vkQueuePresentKHR(_queue->_queue, &presentInfo);
    _outstandingAcquire.Reset();
    InvalidateFrame(frame);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        RADRAY_WARN_LOG("vkQueuePresentKHR: {}", presentResult);
        result.Status = SwapChainStatus::RequireRecreate;
    } else if (presentResult != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkQueuePresentKHR failed: {}", presentResult);
        result.Status = SwapChainStatus::Error;
    } else {
        result.Status = SwapChainStatus::Success;
    }
    result.NativeStatusCode = presentResult;
    return result;
}

bool SwapChainVulkan::Recreate(uint32_t width, uint32_t height, TextureFormat format, PresentMode presentMode) noexcept {
    if (_outstandingAcquire.IsValid()) {
        RADRAY_ABORT("vkCreateSwapchainKHR skipped: outstanding SwapChainFrame must be presented first");
    }

    SwapChainDescriptor desc = this->GetDesc();
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.PresentMode = presentMode;

    VkExtent2D swapchainSize{};
    VkFormat rawFormat = VK_FORMAT_UNDEFINED;
    VkSwapchainKHR oldSwapchain = _swapchain;

    VkSwapchainKHR newSwapchain = _CreateVkSwapChain(_device, _surface.get(), desc, oldSwapchain, swapchainSize, rawFormat);
    if (newSwapchain == VK_NULL_HANDLE) {
        RADRAY_ERR_LOG("vkCreateSwapchainKHR failed during swapchain recreate");
        return false;
    }

    _swapchain = newSwapchain;
    if (!_RefreshSwapChainImages(this, swapchainSize, rawFormat, desc.Format)) {
        _frames.clear();
        _swapchain = oldSwapchain;
        _device->_ftb.vkDestroySwapchainKHR(_device->_device, newSwapchain, _device->GetAllocationCallbacks());
        return false;
    }

    if (oldSwapchain != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroySwapchainKHR(_device->_device, oldSwapchain, _device->GetAllocationCallbacks());
    }

    _width = desc.Width;
    _height = desc.Height;
    _reqFormat = desc.Format;
    _mode = desc.PresentMode;
    return true;
}

uint32_t SwapChainVulkan::GetBackBufferCount() const noexcept {
    return static_cast<uint32_t>(_frames.size());
}

SwapChainDescriptor SwapChainVulkan::GetDesc() const noexcept {
    SwapChainDescriptor desc{};
    desc.PresentQueue = _queue;
    desc.NativeHandler = _nativeHandler;
    desc.Width = _width;
    desc.Height = _height;
    desc.BackBufferCount = static_cast<uint32_t>(_frames.size());
    desc.Format = _reqFormat;
    desc.PresentMode = _mode;
    return desc;
}

QueryPoolVulkan::QueryPoolVulkan(
    DeviceVulkan* device,
    VkQueryPool pool,
    QueryPoolDescriptor desc) noexcept
    : _device(device),
      _pool(pool),
      _desc(std::move(desc)) {}

QueryPoolVulkan::~QueryPoolVulkan() noexcept {
    this->DestroyImpl();
}

bool QueryPoolVulkan::IsValid() const noexcept {
    return _pool != VK_NULL_HANDLE;
}

void QueryPoolVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void QueryPoolVulkan::DestroyImpl() noexcept {
    if (_pool != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyQueryPool(_device->_device, _pool, _device->GetAllocationCallbacks());
        _pool = VK_NULL_HANDLE;
    }
}

void QueryPoolVulkan::SetDebugName(std::string_view name) noexcept {
    _desc.DebugName = string{name};
    _device->SetObjectName(name, _pool);
}

QueryType QueryPoolVulkan::GetType() const noexcept {
    return _desc.Type;
}

uint32_t QueryPoolVulkan::GetCount() const noexcept {
    return _desc.Count;
}

TimestampQueryCalibration QueryPoolVulkan::GetTimestampCalibration(CommandQueue* queue_) const noexcept {
    auto queue = CastVkObject(queue_);
    if (queue == nullptr || queue->_device != _device) {
        return {};
    }
    const double periodNs = static_cast<double>(_device->_properties.limits.timestampPeriod);
    if (periodNs <= 0.0) {
        return {};
    }
    return TimestampQueryCalibration{
        .FrequencyHz = static_cast<uint64_t>(1'000'000'000.0 / periodNs),
        .TickPeriodNs = periodNs};
}

BufferVulkan::BufferVulkan(
    DeviceVulkan* device,
    VkBuffer buffer,
    VmaAllocation allocation,
    VmaAllocationInfo allocInfo) noexcept
    : _device(device),
      _buffer(buffer),
      _allocation(allocation),
      _allocInfo(allocInfo) {}

BufferVulkan::~BufferVulkan() noexcept {
    this->DestroyImpl();
}

bool BufferVulkan::IsValid() const noexcept {
    return _buffer != VK_NULL_HANDLE;
}

void BufferVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void BufferVulkan::DestroyImpl() noexcept {
    if (_buffer != VK_NULL_HANDLE) {
        if (_allocation == VK_NULL_HANDLE) {
            _device->_ftb.vkDestroyBuffer(_device->_device, _buffer, _device->GetAllocationCallbacks());
            _buffer = VK_NULL_HANDLE;
        } else {
            vmaDestroyBuffer(_device->_vma->_vma, _buffer, _allocation);
            _buffer = VK_NULL_HANDLE;
            _allocation = VK_NULL_HANDLE;
        }
    }
}

void* BufferVulkan::Map(uint64_t offset, uint64_t size) noexcept {
    if (!_usage.HasFlag(BufferUse::MapRead) && !_usage.HasFlag(BufferUse::MapWrite)) {
        RADRAY_ABORT("cannot map a Vulkan buffer without MapRead or MapWrite usage");
    }
    if (offset > _reqSizeLogical || size > _reqSizeLogical - offset) {
        RADRAY_ABORT("Vulkan buffer map range is out of bounds");
    }
    void* mappedData = nullptr;
    if (_hints.HasFlag(ResourceHint::PersistentMap)) {
        if (_allocInfo.pMappedData == nullptr) {
            RADRAY_ABORT("persistent-map Vulkan buffer has no mapped allocation");
        }
        mappedData = static_cast<byte*>(_allocInfo.pMappedData) + offset;
    } else {
        if (auto vr = vmaMapMemory(_device->_vma->_vma, _allocation, &mappedData);
            vr != VK_SUCCESS) {
            RADRAY_ABORT("vmaMapMemory failed: {}", vr);
        }
        mappedData = static_cast<byte*>(mappedData) + offset;
    }
    return mappedData;
}

void BufferVulkan::Unmap() noexcept {
    if (!_hints.HasFlag(ResourceHint::PersistentMap)) {
        vmaUnmapMemory(_device->_vma->_vma, _allocation);
    }
}

void BufferVulkan::FlushMappedRange(BufferRange range) noexcept {
    const uint64_t offset = range.Offset;
    if (!_usage.HasFlag(BufferUse::MapWrite) || offset > _reqSizeLogical) {
        RADRAY_ABORT("invalid Vulkan mapped flush range");
    }
    const uint64_t size = range.Size == BufferRange::All()
                              ? _reqSizeLogical - offset
                              : range.Size;
    if (size > _reqSizeLogical - offset) {
        RADRAY_ABORT("invalid Vulkan mapped flush range");
    }
    if (size == 0) {
        return;
    }
    if (_allocation != VK_NULL_HANDLE && !_hostCoherent) {
        if (auto vr = vmaFlushAllocation(_device->_vma->_vma, _allocation, offset, size);
            vr != VK_SUCCESS) {
            RADRAY_ABORT("vmaFlushAllocation failed: {}", vr);
        }
    }
}

void BufferVulkan::InvalidateMappedRange(BufferRange range) noexcept {
    const uint64_t offset = range.Offset;
    if (!_usage.HasFlag(BufferUse::MapRead) || offset > _reqSizeLogical) {
        RADRAY_ABORT("invalid Vulkan mapped invalidate range");
    }
    const uint64_t size = range.Size == BufferRange::All()
                              ? _reqSizeLogical - offset
                              : range.Size;
    if (size > _reqSizeLogical - offset) {
        RADRAY_ABORT("invalid Vulkan mapped invalidate range");
    }
    if (size == 0) {
        return;
    }
    if (_allocation != VK_NULL_HANDLE && !_hostCoherent) {
        if (auto vr = vmaInvalidateAllocation(_device->_vma->_vma, _allocation, offset, size);
            vr != VK_SUCCESS) {
            RADRAY_ABORT("vmaInvalidateAllocation failed: {}", vr);
        }
    }
}

void BufferVulkan::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    _device->SetObjectName(name, _buffer);
}

BufferDescriptor BufferVulkan::GetDesc() const noexcept {
    return BufferDescriptor{
        .Size = _reqSizeLogical,
        .Memory = _memory,
        .Usage = _usage,
        .Hints = _hints};
}

BufferViewVulkan::BufferViewVulkan(
    DeviceVulkan* device,
    VkBufferView view) noexcept
    : _device(device),
      _bufferView(view) {}

BufferViewVulkan::~BufferViewVulkan() noexcept {
    this->DestroyImpl();
}

bool BufferViewVulkan::IsValid() const noexcept {
    return _bufferView != VK_NULL_HANDLE;
}

void BufferViewVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void BufferViewVulkan::DestroyImpl() noexcept {
    if (_bufferView != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyBufferView(_device->_device, _bufferView, _device->GetAllocationCallbacks());
        _bufferView = VK_NULL_HANDLE;
    }
}

ImageVulkan::ImageVulkan(
    DeviceVulkan* device,
    VkImage image,
    VmaAllocation allocation,
    VmaAllocationInfo allocInfo) noexcept
    : _device(device),
      _image(image),
      _allocation(allocation),
      _allocInfo(allocInfo) {}

ImageVulkan::~ImageVulkan() noexcept {
    this->DestroyImpl();
}

bool ImageVulkan::IsValid() const noexcept {
    return _image != VK_NULL_HANDLE;
}

void ImageVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void ImageVulkan::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    _device->SetObjectName(name, _image);
}

TextureDescriptor ImageVulkan::GetDesc() const noexcept {
    return TextureDescriptor{
        _dim,
        _width,
        _height,
        _depthOrArraySize,
        _mipLevels,
        _sampleCount,
        _format,
        _memory,
        _usage,
        _hints};
}

void ImageVulkan::DestroyImpl() noexcept {
    if (_image != VK_NULL_HANDLE) {
        if (_hints.HasFlag(ResourceHint::External)) {
            _image = VK_NULL_HANDLE;
        } else {
            if (_allocation == VK_NULL_HANDLE) {
                _device->_ftb.vkDestroyImage(_device->_device, _image, _device->GetAllocationCallbacks());
                _image = VK_NULL_HANDLE;
            } else {
                vmaDestroyImage(_device->_vma->_vma, _image, _allocation);
                _image = VK_NULL_HANDLE;
                _allocation = VK_NULL_HANDLE;
            }
        }
    }
}

ImageViewVulkan::ImageViewVulkan(
    DeviceVulkan* device,
    ImageVulkan* image,
    VkImageView view) noexcept
    : _device(device),
      _image(image),
      _imageView(view) {}

ImageViewVulkan::~ImageViewVulkan() noexcept {
    this->DestroyImpl();
}

bool ImageViewVulkan::IsValid() const noexcept {
    return _imageView != VK_NULL_HANDLE;
}

void ImageViewVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void ImageViewVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(name, _imageView);
}

void ImageViewVulkan::DestroyImpl() noexcept {
    if (_imageView != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyImageView(_device->_device, _imageView, _device->GetAllocationCallbacks());
        _imageView = VK_NULL_HANDLE;
    }
}

DescriptorSetLayoutBindingVulkanContainer::DescriptorSetLayoutBindingVulkanContainer(
    const VkDescriptorSetLayoutBinding& binding,
    ResourceBindType bindType,
    vector<unique_ptr<SamplerVulkan>> immutableSamplers,
    VkDescriptorBindingFlags bindingFlags) noexcept
    : slot(binding.binding),
      bindType(bindType),
      binding(binding.binding),
      descriptorType(binding.descriptorType),
      descriptorCount(binding.descriptorCount),
      stageFlags(binding.stageFlags),
      bindingFlags(bindingFlags),
      immutableSamplers(std::move(immutableSamplers)) {}

DescriptorSetLayoutVulkan::DescriptorSetLayoutVulkan(
    DeviceVulkan* device,
    VkDescriptorSetLayout layout) noexcept
    : _device(device),
      _layout(layout) {}

DescriptorSetLayoutVulkan::~DescriptorSetLayoutVulkan() noexcept {
    this->DestroyImpl();
}

bool DescriptorSetLayoutVulkan::IsValid() const noexcept {
    return _layout != VK_NULL_HANDLE;
}

void DescriptorSetLayoutVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void DescriptorSetLayoutVulkan::SetDebugName(std::string_view name) noexcept {
    if (_layout != VK_NULL_HANDLE) {
        _device->SetObjectName(name, _layout);
    }
}

void DescriptorSetLayoutVulkan::DestroyImpl() noexcept {
    if (_layout != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyDescriptorSetLayout(_device->_device, _layout, _device->GetAllocationCallbacks());
        _layout = VK_NULL_HANDLE;
    }
}

BindlessDescriptorSetVulkan::BindlessDescriptorSetVulkan(
    DeviceVulkan* device,
    VkDescriptorType type,
    uint32_t capacity) noexcept
    : _device(device),
      _type(type),
      _capacity(capacity) {}

BindlessDescriptorSetVulkan::~BindlessDescriptorSetVulkan() noexcept {
    DestroyImpl();
}

bool BindlessDescriptorSetVulkan::IsValid() const noexcept {
    return _device != nullptr && _pool != VK_NULL_HANDLE && _layout != VK_NULL_HANDLE && _set != VK_NULL_HANDLE;
}

void BindlessDescriptorSetVulkan::Destroy() noexcept {
    DestroyImpl();
}

void BindlessDescriptorSetVulkan::DestroyImpl() noexcept {
    _set = VK_NULL_HANDLE;
    if (_pool != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyDescriptorPool(_device->_device, _pool, _device->GetAllocationCallbacks());
        _pool = VK_NULL_HANDLE;
    }
    if (_layout != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyDescriptorSetLayout(_device->_device, _layout, _device->GetAllocationCallbacks());
        _layout = VK_NULL_HANDLE;
    }
}

BindlessDescAllocator::BindlessDescAllocator(
    unique_ptr<BindlessDescriptorSetVulkan> bdls) noexcept
    : _bdls(std::move(bdls)),
      _allocator(_bdls->_capacity) {}

std::optional<BindlessDescAllocator::Allocation> BindlessDescAllocator::Allocate(uint32_t count) noexcept {
    auto alloc = _allocator.Allocate(static_cast<size_t>(count));
    if (!alloc.has_value()) {
        return std::nullopt;
    }
    return std::make_optional(Allocation{alloc.value(), _bdls->_set, _bdls->_type});
}

void BindlessDescAllocator::Destroy(Allocation allocation) noexcept {
    if (!allocation.IsValid()) {
        return;
    }
    _allocator.Destroy(allocation.Range);
}

PipelineLayoutVulkan::PipelineLayoutVulkan(
    DeviceVulkan* device,
    VkPipelineLayout layout,
    vector<ParameterBinding> parameterBindings,
    uint32_t setLayoutCount) noexcept
    : _device(device),
      _layout(layout),
      _parameterBindings(std::move(parameterBindings)),
      _setLayoutCount(setLayoutCount) {}

PipelineLayoutVulkan::~PipelineLayoutVulkan() noexcept {
    this->DestroyImpl();
}

bool PipelineLayoutVulkan::IsValid() const noexcept {
    return _layout != VK_NULL_HANDLE;
}

void PipelineLayoutVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void PipelineLayoutVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(name, _layout);
    for (size_t i = 0; i < _ownedLayouts.size(); i++) {
        auto* layout = _ownedLayouts[i].get();
        if (layout) {
            _device->SetObjectName(fmt::format("{}_SetLayout_{}", name, i), layout->_layout);
        }
    }
}

void PipelineLayoutVulkan::DestroyImpl() noexcept {
    _setLayouts.clear();
    if (_layout != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyPipelineLayout(_device->_device, _layout, _device->GetAllocationCallbacks());
        _layout = VK_NULL_HANDLE;
    }
    _ownedLayouts.clear();
    _parameterBindings.clear();
    _setLayoutCount = 0;
}

vector<ShaderParameterInfo> PipelineLayoutVulkan::GetParameters() const noexcept {
    // 排除静态采样器 (不作为可绑定的公共参数暴露), 其余从 _parameterBindings 的 Info 派生.
    vector<ShaderParameterInfo> result{};
    result.reserve(_parameterBindings.size());
    for (const auto& binding : _parameterBindings) {
        if (binding.IsStaticSampler) {
            continue;
        }
        result.push_back(binding.Info);
    }
    return result;
}

Nullable<const ShaderParameterInfo*> PipelineLayoutVulkan::FindParameter(std::string_view name) const noexcept {
    for (const auto& binding : _parameterBindings) {
        if (!binding.IsStaticSampler && binding.Info.Name == name) {
            return &binding.Info;
        }
    }
    return nullptr;
}

Nullable<const PipelineLayoutVulkan::ParameterBinding*> PipelineLayoutVulkan::FindParameterInfo(
    uint32_t parameterIndex) const noexcept {
    if (parameterIndex >= _parameterBindings.size()) {
        return nullptr;
    }
    return &_parameterBindings[parameterIndex];
}

std::optional<ShaderBindingLocation> PipelineLayoutVulkan::FindBindingLocation(
    std::string_view name) const noexcept {
    for (const auto& binding : _parameterBindings) {
        if (!binding.IsStaticSampler && binding.Info.Kind != ShaderParameterKind::Constant &&
            binding.Info.Name == name) {
            return ShaderBindingLocation{
                .Group = binding.SetIndex,
                .Binding = binding.BindingIndex};
        }
    }
    return std::nullopt;
}

vector<BindingGroupLayout> PipelineLayoutVulkan::GetBindingGroupLayouts() const noexcept {
    vector<BindingGroupLayout> result(_setLayoutCount);
    for (uint32_t group = 0; group < _setLayoutCount; ++group) {
        result[group].GroupIndex = group;
    }
    for (const auto& binding : _parameterBindings) {
        if (binding.Info.Kind == ShaderParameterKind::Constant || binding.SetIndex >= result.size()) {
            continue;
        }
        result[binding.SetIndex].Entries.push_back(BindingGroupLayoutEntry{
            .Parameter = binding.Info,
            .Binding = binding.BindingIndex,
            .HasDynamicOffset = binding.HasDynamicOffset,
            .IsStaticSampler = binding.IsStaticSampler});
    }
    return result;
}

vector<PushConstantRange> PipelineLayoutVulkan::GetPushConstantRanges() const noexcept {
    vector<PushConstantRange> result{};
    for (const auto& binding : _parameterBindings) {
        if (binding.Info.Kind != ShaderParameterKind::Constant) {
            continue;
        }
        result.push_back(PushConstantRange{
            .Name = binding.Info.Name,
            .Group = binding.SetIndex,
            .Binding = binding.BindingIndex,
            .Stages = binding.Info.Stages,
            .Offset = binding.PushConstantOffset,
            .Size = binding.PushConstantSize});
    }
    return result;
}

Nullable<const PipelineLayoutVulkan::ParameterBinding*> PipelineLayoutVulkan::FindParameterInfo(
    uint32_t setIndex,
    uint32_t bindingIndex) const noexcept {
    for (const auto& binding : _parameterBindings) {
        if (binding.Info.Kind != ShaderParameterKind::Constant &&
            binding.SetIndex == setIndex && binding.BindingIndex == bindingIndex) {
            return &binding;
        }
    }
    return nullptr;
}

vector<const PipelineLayoutVulkan::ParameterBinding*> PipelineLayoutVulkan::GetDynamicBufferBindings(
    uint32_t setIndex) const noexcept {
    vector<const ParameterBinding*> result{};
    for (const auto& binding : _parameterBindings) {
        if (binding.SetIndex == setIndex && binding.HasDynamicOffset) {
            result.push_back(&binding);
        }
    }
    std::ranges::sort(result, {}, [](const ParameterBinding* binding) noexcept {
        return binding->BindingIndex;
    });
    return result;
}

bool PipelineLayoutVulkan::HasBindlessSet(uint32_t setIndex) const noexcept {
    return FindBindlessSet(setIndex).HasValue();
}

Nullable<const PipelineLayoutVulkan::ParameterBinding*> PipelineLayoutVulkan::FindBindlessSet(uint32_t setIndex) const noexcept {
    // 扫描 _parameterBindings 找到该 set 的 bindless 参数.
    for (const auto& binding : _parameterBindings) {
        if (binding.Info.IsBindless && binding.SetIndex == setIndex) {
            return &binding;
        }
    }
    return nullptr;
}

Nullable<DescriptorSetLayoutVulkan*> PipelineLayoutVulkan::GetSetLayout(uint32_t setIndex) const noexcept {
    if (setIndex >= _setLayouts.size()) {
        return nullptr;
    }
    if (_setLayouts[setIndex] == nullptr) {
        return nullptr;
    }
    return _setLayouts[setIndex];
}

GraphicsPipelineVulkan::GraphicsPipelineVulkan(
    DeviceVulkan* device,
    VkPipeline pipeline) noexcept
    : _device(device),
      _pipeline(pipeline) {}

GraphicsPipelineVulkan::~GraphicsPipelineVulkan() noexcept {
    this->DestroyImpl();
}

bool GraphicsPipelineVulkan::IsValid() const noexcept {
    return _pipeline != VK_NULL_HANDLE;
}

void GraphicsPipelineVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void GraphicsPipelineVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(name, _pipeline);
}

void GraphicsPipelineVulkan::DestroyImpl() noexcept {
    if (_pipeline != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyPipeline(_device->_device, _pipeline, _device->GetAllocationCallbacks());
        _pipeline = VK_NULL_HANDLE;
    }
}

ComputePipelineVulkan::ComputePipelineVulkan(
    DeviceVulkan* device,
    VkPipeline pipeline) noexcept
    : _device(device),
      _pipeline(pipeline) {}

ComputePipelineVulkan::~ComputePipelineVulkan() noexcept {
    this->DestroyImpl();
}

bool ComputePipelineVulkan::IsValid() const noexcept {
    return _pipeline != VK_NULL_HANDLE;
}

void ComputePipelineVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void ComputePipelineVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(name, _pipeline);
}

void ComputePipelineVulkan::DestroyImpl() noexcept {
    if (_pipeline != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyPipeline(_device->_device, _pipeline, _device->GetAllocationCallbacks());
        _pipeline = VK_NULL_HANDLE;
    }
}

RayTracingPipelineVulkan::RayTracingPipelineVulkan(
    DeviceVulkan* device,
    VkPipeline pipeline,
    PipelineLayoutVulkan* rootSig) noexcept
    : _device(device),
      _pipeline(pipeline),
      _rootSig(rootSig) {}

RayTracingPipelineVulkan::~RayTracingPipelineVulkan() noexcept {
    this->DestroyImpl();
}

bool RayTracingPipelineVulkan::IsValid() const noexcept {
    return _pipeline != VK_NULL_HANDLE;
}

void RayTracingPipelineVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void RayTracingPipelineVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(name, _pipeline);
}

void RayTracingPipelineVulkan::DestroyImpl() noexcept {
    if (_pipeline != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyPipeline(_device->_device, _pipeline, _device->GetAllocationCallbacks());
        _pipeline = VK_NULL_HANDLE;
    }
    _groupIndices.clear();
    _groupCount = 0;
    _rootSig = nullptr;
}

ShaderBindingTableRequirements RayTracingPipelineVulkan::GetShaderBindingTableRequirements() const noexcept {
    ShaderBindingTableRequirements req{};
    if (_device == nullptr || !_device->_extProperties.rayTracingPipeline.has_value()) {
        return req;
    }
    const auto& prop = _device->_extProperties.rayTracingPipeline.value();
    req.HandleSize = prop.shaderGroupHandleSize;
    req.HandleAlignment = prop.shaderGroupHandleAlignment;
    req.BaseAlignment = prop.shaderGroupBaseAlignment;
    return req;
}

std::optional<vector<byte>> RayTracingPipelineVulkan::GetShaderBindingTableHandle(std::string_view shaderName) const noexcept {
    auto it = _groupIndices.find(string(shaderName));
    if (it == _groupIndices.end()) {
        return std::nullopt;
    }
    auto req = this->GetShaderBindingTableRequirements();
    if (req.HandleSize == 0) {
        return std::nullopt;
    }
    vector<byte> handle(req.HandleSize);
    auto vr = _device->_ftb.vkGetRayTracingShaderGroupHandlesKHR(
        _device->_device,
        _pipeline,
        it->second,
        1,
        handle.size(),
        handle.data());
    if (vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkGetRayTracingShaderGroupHandlesKHR failed: {}", vr);
        return std::nullopt;
    }
    return handle;
}

ShaderBindingTableVulkan::ShaderBindingTableVulkan(
    DeviceVulkan* device,
    RayTracingPipelineVulkan* pipeline,
    unique_ptr<Buffer> buffer,
    const ShaderBindingTableDescriptor& desc,
    uint64_t recordStride) noexcept
    : _device(device),
      _pipeline(pipeline),
      _buffer(std::move(buffer)),
      _desc(desc),
      _recordStride(recordStride) {
    _rayGenOffset = 0;
    _missOffset = _recordStride * _desc.RayGenCount;
    _hitGroupOffset = _missOffset + _recordStride * _desc.MissCount;
    _callableOffset = _hitGroupOffset + _recordStride * _desc.HitGroupCount;
}

ShaderBindingTableVulkan::~ShaderBindingTableVulkan() noexcept {
    this->DestroyImpl();
}

bool ShaderBindingTableVulkan::IsValid() const noexcept {
    return _device != nullptr && _pipeline != nullptr && _buffer != nullptr;
}

void ShaderBindingTableVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void ShaderBindingTableVulkan::DestroyImpl() noexcept {
    _buffer.reset();
    _pipeline = nullptr;
    _device = nullptr;
    _isBuilt = false;
}

void ShaderBindingTableVulkan::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    if (_buffer) {
        _buffer->SetDebugName(_name);
    }
}

bool ShaderBindingTableVulkan::Build(std::span<const ShaderBindingTableBuildEntry> entries) noexcept {
    if (!this->IsValid()) {
        return false;
    }
    auto req = _pipeline->GetShaderBindingTableRequirements();
    if (req.HandleSize == 0 || _recordStride < req.HandleSize) {
        RADRAY_ERR_LOG("invalid SBT record stride/handle size");
        return false;
    }
    struct ResolvedEntry {
        uint64_t Offset{0};
        vector<byte> Handle;
        std::span<const byte> LocalData;
    };
    vector<ResolvedEntry> resolvedEntries;
    resolvedEntries.reserve(entries.size());
    for (const auto& entry : entries) {
        uint32_t count = 0;
        uint64_t baseOffset = 0;
        switch (entry.Type) {
            case ShaderBindingTableEntryType::RayGen:
                count = _desc.RayGenCount;
                baseOffset = _rayGenOffset;
                break;
            case ShaderBindingTableEntryType::Miss:
                count = _desc.MissCount;
                baseOffset = _missOffset;
                break;
            case ShaderBindingTableEntryType::HitGroup:
                count = _desc.HitGroupCount;
                baseOffset = _hitGroupOffset;
                break;
            case ShaderBindingTableEntryType::Callable:
                count = _desc.CallableCount;
                baseOffset = _callableOffset;
                break;
        }
        if (entry.RecordIndex >= count) {
            RADRAY_ERR_LOG("SBT record index out of range: type={}, index={}, count={}", static_cast<uint32_t>(entry.Type), entry.RecordIndex, count);
            return false;
        }
        auto handle = _pipeline->GetShaderBindingTableHandle(entry.ShaderName);
        if (!handle.has_value()) {
            RADRAY_ERR_LOG("cannot find shader handle '{}'", entry.ShaderName);
            return false;
        }
        if (entry.LocalData.size() > _recordStride - req.HandleSize) {
            RADRAY_ERR_LOG("local data too large for SBT record '{}'", entry.ShaderName);
            return false;
        }
        resolvedEntries.push_back(ResolvedEntry{
            .Offset = baseOffset + _recordStride * entry.RecordIndex,
            .Handle = std::move(handle.value()),
            .LocalData = entry.LocalData});
    }

    const uint64_t totalSize = _buffer->GetDesc().Size;
    auto* mapped = static_cast<byte*>(_buffer->Map(0, totalSize));
    if (mapped == nullptr) {
        RADRAY_ERR_LOG("failed to map SBT buffer");
        return false;
    }
    auto unmapGuard = MakeScopeGuard([&]() noexcept { _buffer->Unmap(); });
    std::memset(mapped, 0, static_cast<size_t>(totalSize));
    for (const ResolvedEntry& entry : resolvedEntries) {
        auto* dst = mapped + entry.Offset;
        std::memcpy(dst, entry.Handle.data(), req.HandleSize);
        if (!entry.LocalData.empty()) {
            std::memcpy(dst + req.HandleSize, entry.LocalData.data(), entry.LocalData.size());
        }
    }
    _buffer->FlushMappedRange(BufferRange{.Offset = 0, .Size = totalSize});
    _buffer->Unmap();
    unmapGuard.Dismiss();
    _isBuilt = true;
    return true;
}

bool ShaderBindingTableVulkan::IsBuilt() const noexcept {
    return _isBuilt;
}

ShaderBindingTableRegions ShaderBindingTableVulkan::GetRegions() const noexcept {
    ShaderBindingTableRegions regions{};
    regions.RayGen = {_buffer.get(), _rayGenOffset, _recordStride * _desc.RayGenCount, _recordStride};
    regions.Miss = {_buffer.get(), _missOffset, _recordStride * _desc.MissCount, _recordStride};
    regions.HitGroup = {_buffer.get(), _hitGroupOffset, _recordStride * _desc.HitGroupCount, _recordStride};
    if (_desc.CallableCount > 0) {
        regions.Callable = ShaderBindingTableRegion{_buffer.get(), _callableOffset, _recordStride * _desc.CallableCount, _recordStride};
    }
    return regions;
}

AccelerationStructureVulkan::AccelerationStructureVulkan(
    DeviceVulkan* device,
    VkBuffer buffer,
    VmaAllocation allocation,
    VmaAllocationInfo allocInfo,
    VkAccelerationStructureKHR accelerationStructure,
    const AccelerationStructureDescriptor& desc,
    uint64_t asSize) noexcept
    : _device(device),
      _buffer(buffer),
      _allocation(allocation),
      _allocInfo(allocInfo),
      _accelerationStructure(accelerationStructure),
      _desc(desc),
      _asSize(asSize) {}

AccelerationStructureVulkan::~AccelerationStructureVulkan() noexcept {
    this->DestroyImpl();
}

bool AccelerationStructureVulkan::IsValid() const noexcept {
    return _buffer != VK_NULL_HANDLE && _allocation != VK_NULL_HANDLE && _accelerationStructure != VK_NULL_HANDLE;
}

void AccelerationStructureVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void AccelerationStructureVulkan::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    _device->SetObjectName(name, _buffer);
}

void AccelerationStructureVulkan::DestroyImpl() noexcept {
    if (_accelerationStructure != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyAccelerationStructureKHR(_device->_device, _accelerationStructure, _device->GetAllocationCallbacks());
        _accelerationStructure = VK_NULL_HANDLE;
    }
    if (_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(_device->_vma->_vma, _buffer, _allocation);
        _buffer = VK_NULL_HANDLE;
        _allocation = VK_NULL_HANDLE;
        _allocInfo = VmaAllocationInfo{};
    }
    _deviceAddress = 0;
}

AccelerationStructureViewVulkan::AccelerationStructureViewVulkan(
    DeviceVulkan* device,
    AccelerationStructureVulkan* target) noexcept
    : _device(device),
      _target(target) {}

AccelerationStructureViewVulkan::~AccelerationStructureViewVulkan() noexcept {
    this->DestroyImpl();
}

bool AccelerationStructureViewVulkan::IsValid() const noexcept {
    return _target != nullptr && _target->IsValid();
}

void AccelerationStructureViewVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void AccelerationStructureViewVulkan::SetDebugName(std::string_view name) noexcept {
    RADRAY_UNUSED(name);
}

void AccelerationStructureViewVulkan::DestroyImpl() noexcept {
    _target = nullptr;
}

ShaderModuleVulkan::ShaderModuleVulkan(
    DeviceVulkan* device,
    VkShaderModule shaderModule,
    ShaderStages stages,
    std::optional<ShaderReflectionDesc> reflection) noexcept
    : _device(device),
      _shaderModule(shaderModule),
      _stages(stages),
      _reflection(std::move(reflection)) {}

ShaderModuleVulkan::~ShaderModuleVulkan() noexcept {
    this->DestroyImpl();
}

bool ShaderModuleVulkan::IsValid() const noexcept {
    return _shaderModule != VK_NULL_HANDLE;
}

void ShaderModuleVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void ShaderModuleVulkan::DestroyImpl() noexcept {
    if (_shaderModule != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyShaderModule(_device->_device, _shaderModule, _device->GetAllocationCallbacks());
        _shaderModule = VK_NULL_HANDLE;
    }
    _stages = ShaderStage::UNKNOWN;
    _reflection.reset();
}

DescriptorPoolVulkan::DescriptorPoolVulkan(
    DeviceVulkan* device,
    DescriptorSetAllocatorVulkan* alloc,
    VkDescriptorPool pool,
    uint32_t capacity) noexcept
    : _device(device),
      _alloc(alloc),
      _pool(pool),
      _capacity(capacity) {}

DescriptorPoolVulkan::~DescriptorPoolVulkan() noexcept {
    this->DestroyImpl();
}

bool DescriptorPoolVulkan::IsValid() const noexcept {
    return _pool != VK_NULL_HANDLE;
}

void DescriptorPoolVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void DescriptorPoolVulkan::DestroyImpl() noexcept {
    if (_pool != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyDescriptorPool(_device->_device, _pool, _device->GetAllocationCallbacks());
        _pool = VK_NULL_HANDLE;
    }
}

DescriptorSetVulkan::DescriptorSetVulkan(
    DeviceVulkan* device,
    PipelineLayoutVulkan* rootSig,
    uint32_t setIndex,
    DescriptorSetLayoutVulkan* layout,
    DescriptorSetAllocatorVulkan* allocator,
    DescriptorSetAllocatorVulkan::Allocation allocation) noexcept
    : _device(device),
      _rootSig(rootSig),
      _setIndex(setIndex),
      _layout(layout),
      _allocator(allocator),
      _allocation(allocation) {}

DescriptorSetVulkan::~DescriptorSetVulkan() noexcept {
    this->DestroyImpl();
}

bool DescriptorSetVulkan::IsValid() const noexcept {
    return _device != nullptr &&
           _rootSig != nullptr &&
           _rootSig->IsValid() &&
           _layout != nullptr &&
           _allocation.IsValid() &&
           _setIndex < _rootSig->GetDescriptorSetCount();
}

void DescriptorSetVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void DescriptorSetVulkan::Reset() noexcept {
    std::ranges::fill(_resourceWritten, uint8_t{0});
    std::ranges::fill(_samplerWritten, uint8_t{0});
    _pendingWrites.clear();
    _ownedTexelBufferViews.clear();
}

void DescriptorSetVulkan::SetDebugName(std::string_view name) noexcept {
    _name = string{name};
    if (_allocation.IsValid()) {
        _device->SetObjectName(name, _allocation.Set);
    }
}

void DescriptorSetVulkan::DestroyImpl() noexcept {
    if (_allocation.IsValid()) {
        _allocator->Destroy(_allocation);
        _allocation = DescriptorSetAllocatorVulkan::Allocation::Invalid();
    }
    _ownedTexelBufferViews.clear();
    _resourceWritten.clear();
    _samplerWritten.clear();
    _pendingWrites.clear();
    _setIndex = 0;
    _rootSig = nullptr;
    _layout = nullptr;
    _allocator = nullptr;
    _device = nullptr;
    _name.clear();
}

bool DescriptorSetVulkan::WriteResource(uint32_t parameterIndex, ResourceView* view, uint32_t arrayIndex) noexcept {
    if (!this->IsValid()) {
        RADRAY_ERR_LOG("descriptor set is invalid");
        return false;
    }
    if (view == nullptr) {
        RADRAY_ERR_LOG("resource view is null");
        return false;
    }
    auto infoOpt = _rootSig->FindParameterInfo(parameterIndex);
    if (!infoOpt.HasValue() || infoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("binding parameter index {} is out of range", parameterIndex);
        return false;
    }
    const auto* info = infoOpt.Get();
    if (info->Info.Kind != ShaderParameterKind::Resource) {
        RADRAY_ERR_LOG("binding parameter index {} is not a resource parameter", parameterIndex);
        return false;
    }
    if (info->SetIndex != _setIndex) {
        RADRAY_ERR_LOG(
            "binding parameter id {} belongs to descriptor set {}, not {}",
            parameterIndex,
            info->SetIndex,
            _setIndex);
        return false;
    }
    if (arrayIndex >= info->Info.Count) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", info->Info.Count, arrayIndex);
        return false;
    }
    auto bindType = _GetResourceViewBindTypeVulkan(view);
    if (!bindType.has_value() || bindType.value() != info->Info.Type) {
        RADRAY_ERR_LOG(
            "resource type mismatch for binding parameter id {} expected: {}, actual: {}",
            parameterIndex,
            info->Info.Type,
            bindType.has_value() ? bindType.value() : ResourceBindType::UNKNOWN);
        return false;
    }
    if (!SetResource(info->BindingIndex, arrayIndex, view)) {
        return false;
    }
    const uint32_t writtenIndex = info->DescriptorWriteOffset + arrayIndex;
    if (writtenIndex >= _resourceWritten.size()) {
        RADRAY_ERR_LOG("internal error: resource written-state index {} is out of range", writtenIndex);
        return false;
    }
    _resourceWritten[writtenIndex] = 1;
    return true;
}

bool DescriptorSetVulkan::WriteResource(
    uint32_t parameterIndex,
    const BufferBindingDescriptor& desc,
    uint32_t arrayIndex) noexcept {
    if (!this->IsValid()) {
        RADRAY_ERR_LOG("descriptor set is invalid");
        return false;
    }
    auto infoOpt = _rootSig->FindParameterInfo(parameterIndex);
    if (!infoOpt.HasValue() || infoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("binding parameter index {} is out of range", parameterIndex);
        return false;
    }
    const auto* info = infoOpt.Get();
    if (info->Info.Kind != ShaderParameterKind::Resource) {
        RADRAY_ERR_LOG("binding parameter index {} is not a resource parameter", parameterIndex);
        return false;
    }
    if (info->SetIndex != _setIndex) {
        RADRAY_ERR_LOG(
            "binding parameter id {} belongs to descriptor set {}, not {}",
            parameterIndex,
            info->SetIndex,
            _setIndex);
        return false;
    }
    if (arrayIndex >= info->Info.Count) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", info->Info.Count, arrayIndex);
        return false;
    }
    const auto bindType = BufferViewUsageToResourceBindType(desc.Usage);
    if (bindType != info->Info.Type) {
        RADRAY_ERR_LOG(
            "buffer binding type mismatch for binding parameter id {} expected: {}, actual: {}",
            parameterIndex,
            info->Info.Type,
            bindType);
        return false;
    }
    if (!SetBufferResource(info->BindingIndex, arrayIndex, desc)) {
        return false;
    }
    const uint32_t writtenIndex = info->DescriptorWriteOffset + arrayIndex;
    if (writtenIndex >= _resourceWritten.size()) {
        RADRAY_ERR_LOG("internal error: resource written-state index {} is out of range", writtenIndex);
        return false;
    }
    _resourceWritten[writtenIndex] = 1;
    return true;
}

bool DescriptorSetVulkan::WriteSampler(uint32_t parameterIndex, Sampler* sampler, uint32_t arrayIndex) noexcept {
    if (!this->IsValid()) {
        RADRAY_ERR_LOG("descriptor set is invalid");
        return false;
    }
    if (sampler == nullptr) {
        RADRAY_ERR_LOG("sampler is null");
        return false;
    }
    auto infoOpt = _rootSig->FindParameterInfo(parameterIndex);
    if (!infoOpt.HasValue() || infoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("binding parameter index {} is out of range", parameterIndex);
        return false;
    }
    const auto* info = infoOpt.Get();
    if (info->IsStaticSampler) {
        RADRAY_ERR_LOG("binding parameter index {} is a static sampler and cannot be written", parameterIndex);
        return false;
    }
    if (info->Info.Kind != ShaderParameterKind::Sampler) {
        RADRAY_ERR_LOG("binding parameter index {} is not a sampler parameter", parameterIndex);
        return false;
    }
    if (info->SetIndex != _setIndex) {
        RADRAY_ERR_LOG(
            "binding parameter id {} belongs to descriptor set {}, not {}",
            parameterIndex,
            info->SetIndex,
            _setIndex);
        return false;
    }
    if (arrayIndex >= info->Info.Count) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", info->Info.Count, arrayIndex);
        return false;
    }
    if (!SetSampler(info->BindingIndex, arrayIndex, sampler)) {
        return false;
    }
    const uint32_t writtenIndex = info->DescriptorWriteOffset + arrayIndex;
    if (writtenIndex >= _samplerWritten.size()) {
        RADRAY_ERR_LOG("internal error: sampler written-state index {} is out of range", writtenIndex);
        return false;
    }
    _samplerWritten[writtenIndex] = 1;
    return true;
}

bool DescriptorSetVulkan::SetResource(uint32_t slot, uint32_t arrayIndex, ResourceView* view) noexcept {
    if (!_layout || !_allocation.IsValid()) {
        RADRAY_ERR_LOG("vk invalid descriptor set");
        return false;
    }
    if (view == nullptr) {
        RADRAY_ERR_LOG("vk resource view is null");
        return false;
    }
    auto tag = view->GetTag();
    ResourceBindType requiredBindType = ResourceBindType::UNKNOWN;
    if (tag.HasFlag(RenderObjectTag::TextureView)) {
        auto tv = static_cast<ImageViewVulkan*>(view);
        switch (tv->_mdesc.Usage) {
            case TextureViewUsage::Resource:
                requiredBindType = ResourceBindType::Texture;
                break;
            case TextureViewUsage::UnorderedAccess:
                requiredBindType = ResourceBindType::RWTexture;
                break;
            default:
                RADRAY_ERR_LOG("vk texture view usage {} cannot be bound as a resource", tv->_mdesc.Usage);
                return false;
        }
    } else if (tag.HasFlag(RenderObjectTag::AccelerationStructureView)) {
        requiredBindType = ResourceBindType::AccelerationStructure;
    } else {
        RADRAY_ERR_LOG("vk unsupported resource view: {}", tag);
        return false;
    }
    const DescriptorSetLayoutBindingVulkanContainer* binding = nullptr;
    for (const auto& e : _layout->_bindings) {
        if (e.slot == slot && e.bindType == requiredBindType) {
            binding = &e;
            break;
        }
    }
    if (binding == nullptr) {
        RADRAY_ERR_LOG("vk no matching descriptor binding for slot={} type={}", slot, requiredBindType);
        return false;
    }
    if (arrayIndex >= binding->descriptorCount) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", binding->descriptorCount, arrayIndex);
        return false;
    }
    PendingDescriptorWriteVulkan write{};
    write.Set = _allocation.Set;
    write.Binding = binding->binding;
    write.ArrayIndex = arrayIndex;
    write.Type = binding->descriptorType;
    if (tag.HasFlag(RenderObjectTag::TextureView)) {
        auto tv = static_cast<ImageViewVulkan*>(view);
        if ((tv->_mdesc.Usage == TextureViewUsage::UnorderedAccess && write.Type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
            (tv->_mdesc.Usage == TextureViewUsage::Resource && write.Type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
            RADRAY_ERR_LOG("descriptor type mismatch for texture view usage");
            return false;
        }
        write.Payload = DescriptorWritePayloadVulkan::Image;
        write.ImageInfo.sampler = VK_NULL_HANDLE;
        write.ImageInfo.imageView = tv->_imageView;
        write.ImageInfo.imageLayout = TextureViewUsageToLayout(tv->_mdesc.Usage);
    } else if (tag.HasFlag(RenderObjectTag::AccelerationStructureView)) {
        auto asView = static_cast<AccelerationStructureViewVulkan*>(view);
        if (asView->_target == nullptr || asView->_target->_accelerationStructure == VK_NULL_HANDLE) {
            RADRAY_ERR_LOG("vk invalid acceleration structure view");
            return false;
        }
        write.Payload = DescriptorWritePayloadVulkan::AccelerationStructure;
        write.AccelerationStructure = asView->_target->_accelerationStructure;
    }
    StageWrite(std::move(write));
    return true;
}

bool DescriptorSetVulkan::SetBufferResource(uint32_t slot, uint32_t arrayIndex, const BufferBindingDescriptor& desc) noexcept {
    if (!_layout || !_allocation.IsValid()) {
        RADRAY_ERR_LOG("vk invalid descriptor set");
        return false;
    }
    const auto bindType = BufferViewUsageToResourceBindType(desc.Usage);
    const DescriptorSetLayoutBindingVulkanContainer* binding = nullptr;
    for (const auto& e : _layout->_bindings) {
        if (e.slot == slot && e.bindType == bindType) {
            binding = &e;
            break;
        }
    }
    if (binding == nullptr) {
        RADRAY_ERR_LOG("vk no matching descriptor binding for slot={} type={}", slot, bindType);
        return false;
    }
    if (arrayIndex >= binding->descriptorCount) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", binding->descriptorCount, arrayIndex);
        return false;
    }
    PendingDescriptorWriteVulkan write{};
    unique_ptr<BufferViewVulkan> ownedTexelView{};
    if (!_BuildBufferBindingDescriptorVulkan(
            _device,
            _allocation.Set,
            binding->binding,
            binding->descriptorType,
            arrayIndex,
            desc,
            true,
            write,
            ownedTexelView)) {
        return false;
    }
    StageWrite(std::move(write));

    const uint64_t descriptorKey = (static_cast<uint64_t>(binding->binding) << 32u) | static_cast<uint64_t>(arrayIndex);
    if (ownedTexelView) {
        _ownedTexelBufferViews[descriptorKey] = std::move(ownedTexelView);
    } else {
        _ownedTexelBufferViews.erase(descriptorKey);
    }
    return true;
}

bool DescriptorSetVulkan::SetSampler(uint32_t slot, uint32_t arrayIndex, Sampler* sampler) noexcept {
    if (!_layout || !_allocation.IsValid()) {
        RADRAY_ERR_LOG("vk invalid descriptor set");
        return false;
    }
    if (sampler == nullptr) {
        RADRAY_ERR_LOG("vk sampler is null");
        return false;
    }
    const DescriptorSetLayoutBindingVulkanContainer* binding = nullptr;
    for (const auto& e : _layout->_bindings) {
        if (e.slot == slot && e.bindType == ResourceBindType::Sampler) {
            binding = &e;
            break;
        }
    }
    if (binding == nullptr) {
        RADRAY_ERR_LOG("vk no matching sampler binding for slot {}", slot);
        return false;
    }
    if (arrayIndex >= binding->descriptorCount) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", binding->descriptorCount, arrayIndex);
        return false;
    }
    auto* sam = CastVkObject(sampler);
    PendingDescriptorWriteVulkan write{};
    write.Set = _allocation.Set;
    write.Binding = binding->binding;
    write.ArrayIndex = arrayIndex;
    write.Type = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.Payload = DescriptorWritePayloadVulkan::Image;
    write.ImageInfo.sampler = sam->_sampler;
    write.ImageInfo.imageView = VK_NULL_HANDLE;
    write.ImageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    StageWrite(std::move(write));
    return true;
}

Nullable<const PipelineLayoutVulkan::ParameterBinding*> DescriptorSetVulkan::FindFirstUnwrittenParameter(
    uint32_t* arrayIndex) const noexcept {
    if (!this->IsValid()) {
        return nullptr;
    }
    // 遍历该 set 下的非静态采样器/非 bindless/非 push constant 参数, 找到第一个未写槽位.
    for (const auto& binding : _rootSig->GetParameterBindings()) {
        if (binding.IsStaticSampler || binding.Info.IsBindless ||
            binding.Info.Kind == ShaderParameterKind::Constant ||
            binding.SetIndex != _setIndex) {
            continue;
        }
        const auto& written = binding.Info.Kind == ShaderParameterKind::Sampler ? _samplerWritten : _resourceWritten;
        for (uint32_t j = 0; j < binding.Info.Count; ++j) {
            const uint32_t index = binding.DescriptorWriteOffset + j;
            if (index >= written.size() || !written[index]) {
                if (arrayIndex != nullptr) {
                    *arrayIndex = j;
                }
                return &binding;
            }
        }
    }
    return nullptr;
}

bool DescriptorSetVulkan::IsFullyWritten() const noexcept {
    return this->IsValid() && !this->FindFirstUnwrittenParameter().HasValue();
}

bool DescriptorSetVulkan::HasAnyWrite() const noexcept {
    return std::ranges::any_of(_resourceWritten, [](uint8_t v) noexcept { return v != 0; }) ||
           std::ranges::any_of(_samplerWritten, [](uint8_t v) noexcept { return v != 0; });
}

void DescriptorSetVulkan::StageWrite(PendingDescriptorWriteVulkan write) noexcept {
    const auto existing = std::ranges::find_if(
        _pendingWrites,
        [&](const PendingDescriptorWriteVulkan& pending) noexcept {
            return pending.Binding == write.Binding && pending.ArrayIndex == write.ArrayIndex;
        });
    if (existing != _pendingWrites.end()) {
        *existing = std::move(write);
    } else {
        _pendingWrites.push_back(std::move(write));
    }
}

BindingGroupVulkan::BindingGroupVulkan(
    DeviceVulkan* device,
    BindingDescriptorPoolVulkan* pool,
    PipelineLayoutVulkan* layout,
    uint32_t groupIndex,
    unique_ptr<DescriptorSetVulkan> descriptorSet,
    const DescriptorPoolDescriptor& poolUsage) noexcept
    : _device(device),
      _pool(pool),
      _layout(layout),
      _groupIndex(groupIndex),
      _descriptorSet(std::move(descriptorSet)),
      _dynamicBuffers(layout == nullptr ? 0 : layout->GetParameterCount()),
      _poolUsage(poolUsage) {}

BindingGroupVulkan::~BindingGroupVulkan() noexcept {
    Destroy();
}

bool BindingGroupVulkan::IsValid() const noexcept {
    if (_device == nullptr || _layout == nullptr || !_layout->IsValid()) {
        return false;
    }
    return _layout->HasBindlessSet(_groupIndex) ||
           (_descriptorSet != nullptr && _descriptorSet->IsValid());
}

void BindingGroupVulkan::Destroy() noexcept {
    _descriptorSet.reset();
    if (_pool != nullptr) {
        _pool->ReleaseGroup(_poolUsage);
        _pool = nullptr;
    }
    _dynamicBuffers.clear();
    _bindlessArray = nullptr;
    _layout = nullptr;
    _device = nullptr;
    _poolUsage = {};
    _name.clear();
}

void BindingGroupVulkan::Reset() noexcept {
    if (_descriptorSet != nullptr) {
        _descriptorSet->Reset();
    }
    std::ranges::fill(_dynamicBuffers, std::nullopt);
    _bindlessArray = nullptr;
}

void BindingGroupVulkan::SetDebugName(std::string_view name) noexcept {
    _name = string{name};
    if (_descriptorSet != nullptr) {
        _descriptorSet->SetDebugName(name);
    }
}

bool BindingGroupVulkan::SetResource(uint32_t binding, ResourceView* view, uint32_t arrayIndex) noexcept {
    auto info = _layout != nullptr ? _layout->FindParameterInfo(_groupIndex, binding)
                                   : Nullable<const PipelineLayoutVulkan::ParameterBinding*>{};
    if (!info.HasValue() || info.Get() == nullptr || _descriptorSet == nullptr) {
        RADRAY_ERR_LOG("vk binding group resource binding {} is unavailable", binding);
        return false;
    }
    return _descriptorSet->WriteResource(info.Get()->ParameterIndex, view, arrayIndex);
}

bool BindingGroupVulkan::SetResource(
    uint32_t binding,
    const BufferBindingDescriptor& desc,
    uint32_t arrayIndex) noexcept {
    auto info = _layout != nullptr ? _layout->FindParameterInfo(_groupIndex, binding)
                                   : Nullable<const PipelineLayoutVulkan::ParameterBinding*>{};
    if (!info.HasValue() || info.Get() == nullptr || _descriptorSet == nullptr) {
        RADRAY_ERR_LOG("vk binding group buffer binding {} is unavailable", binding);
        return false;
    }
    const auto* parameter = info.Get();
    if (!_descriptorSet->WriteResource(parameter->ParameterIndex, desc, arrayIndex)) {
        return false;
    }
    if (parameter->HasDynamicOffset) {
        if (arrayIndex != 0 || parameter->ParameterIndex >= _dynamicBuffers.size()) {
            RADRAY_ERR_LOG("vk dynamic cbuffer bindings do not support arrays");
            return false;
        }
        _dynamicBuffers[parameter->ParameterIndex] = desc;
    }
    return true;
}

static bool _BindBindingGroupVulkan(
    DeviceVulkan* device,
    CommandBufferVulkan* cmdBuffer,
    PipelineLayoutVulkan*& boundPipeLayout,
    uint32_t groupIndex,
    BindingGroup* group_,
    std::span<const uint32_t> dynamicOffsets,
    VkPipelineBindPoint bindPoint) noexcept {
    auto* group = CastVkObject(group_);
    if (group == nullptr || !group->IsValid()) {
        RADRAY_ERR_LOG("vk binding group is invalid");
        return false;
    }
    auto* layout = CastVkObject(group->GetPipelineLayout());
    if (layout == nullptr || group->GetGroupIndex() != groupIndex) {
        RADRAY_ERR_LOG("vk binding group index/layout mismatch");
        return false;
    }
    if (boundPipeLayout != nullptr && boundPipeLayout != layout) {
        RADRAY_ERR_LOG("vk binding group belongs to a different pipeline layout");
        return false;
    }
    boundPipeLayout = layout;

    const auto dynamicBindings = layout->GetDynamicBufferBindings(groupIndex);
    if (dynamicOffsets.size() != dynamicBindings.size()) {
        RADRAY_ERR_LOG(
            "vk dynamic offset count mismatch for group {} expected: {}, actual: {}",
            groupIndex,
            dynamicBindings.size(),
            dynamicOffsets.size());
        return false;
    }
    const uint64_t alignment = std::max<uint64_t>(1, device->_detail.CBufferAlignment);
    for (size_t i = 0; i < dynamicBindings.size(); ++i) {
        const auto* desc = group->GetDynamicBuffer(dynamicBindings[i]->ParameterIndex);
        if (desc == nullptr || desc->Target == nullptr) {
            RADRAY_ERR_LOG("vk dynamic cbuffer binding {} is unwritten", dynamicBindings[i]->BindingIndex);
            return false;
        }
        const uint64_t offset = dynamicOffsets[i];
        if (offset % alignment != 0) {
            RADRAY_ERR_LOG("vk dynamic cbuffer offset {} is not aligned to {}", offset, alignment);
            return false;
        }
        const auto bufferSize = desc->Target->GetDesc().Size;
        if (desc->Range.Offset > bufferSize) {
            RADRAY_ERR_LOG("vk dynamic cbuffer base offset exceeds buffer size");
            return false;
        }
        const uint64_t rangeSize = desc->Range.Size == BufferRange::All()
                                       ? bufferSize - desc->Range.Offset
                                       : desc->Range.Size;
        if (offset > bufferSize - desc->Range.Offset ||
            rangeSize > bufferSize - desc->Range.Offset - offset) {
            RADRAY_ERR_LOG("vk dynamic cbuffer offset/range exceeds buffer size");
            return false;
        }
    }

    if (!group->FlushDescriptorWrites()) {
        return false;
    }
    if (layout->HasBindlessSet(groupIndex)) {
        return _BindBindlessArrayVulkan(
            device,
            cmdBuffer,
            layout,
            groupIndex,
            group->GetBindlessArray(),
            bindPoint);
    }
    return _BindDescriptorSetVulkan(
        device,
        cmdBuffer,
        layout,
        groupIndex,
        group->GetDescriptorSet(),
        bindPoint,
        dynamicOffsets);
}

bool BindingGroupVulkan::SetSampler(uint32_t binding, Sampler* sampler, uint32_t arrayIndex) noexcept {
    auto info = _layout != nullptr ? _layout->FindParameterInfo(_groupIndex, binding)
                                   : Nullable<const PipelineLayoutVulkan::ParameterBinding*>{};
    if (!info.HasValue() || info.Get() == nullptr || _descriptorSet == nullptr) {
        RADRAY_ERR_LOG("vk binding group sampler binding {} is unavailable", binding);
        return false;
    }
    return _descriptorSet->WriteSampler(info.Get()->ParameterIndex, sampler, arrayIndex);
}

bool BindingGroupVulkan::SetBindlessArray(uint32_t binding, BindlessArray* array) noexcept {
    auto info = _layout != nullptr ? _layout->FindParameterInfo(_groupIndex, binding)
                                   : Nullable<const PipelineLayoutVulkan::ParameterBinding*>{};
    if (!info.HasValue() || info.Get() == nullptr || !info.Get()->Info.IsBindless) {
        RADRAY_ERR_LOG("vk binding group binding {} is not bindless", binding);
        return false;
    }
    _bindlessArray = array;
    return true;
}

bool BindingGroupVulkan::IsFullyWritten() const noexcept {
    return _descriptorSet != nullptr && _descriptorSet->IsFullyWritten();
}

bool BindingGroupVulkan::FlushDescriptorWrites() noexcept {
    if (_descriptorSet == nullptr || _descriptorSet->_pendingWrites.empty()) {
        return true;
    }
    _pendingWriteScratch.assign(
        _descriptorSet->_pendingWrites.begin(),
        _descriptorSet->_pendingWrites.end());
    if (!_SubmitDescriptorWritesVulkan(
            _device,
            _pendingWriteScratch,
            _descriptorWriteScratch,
            _accelerationWriteScratch)) {
        return false;
    }
    _descriptorSet->_pendingWrites.clear();
    return true;
}

const BufferBindingDescriptor* BindingGroupVulkan::GetDynamicBuffer(uint32_t parameterIndex) const noexcept {
    if (parameterIndex >= _dynamicBuffers.size() || !_dynamicBuffers[parameterIndex].has_value()) {
        return nullptr;
    }
    return &_dynamicBuffers[parameterIndex].value();
}

static void _AccumulateDescriptorPoolSizeVulkan(
    vector<VkDescriptorPoolSize>& poolSizes,
    VkDescriptorType type,
    uint32_t descriptorCount) noexcept {
    if (descriptorCount == 0) {
        return;
    }
    auto it = std::find_if(poolSizes.begin(), poolSizes.end(), [type](const VkDescriptorPoolSize& poolSize) {
        return poolSize.type == type;
    });
    if (it == poolSizes.end()) {
        poolSizes.push_back(VkDescriptorPoolSize{type, descriptorCount});
        return;
    }
    it->descriptorCount += descriptorCount;
}

static void _EnsureDescriptorPoolSizeVulkan(
    vector<VkDescriptorPoolSize>& poolSizes,
    VkDescriptorType type,
    uint32_t descriptorCount) noexcept {
    if (descriptorCount == 0) {
        return;
    }
    auto it = std::find_if(poolSizes.begin(), poolSizes.end(), [type](const VkDescriptorPoolSize& poolSize) {
        return poolSize.type == type;
    });
    if (it == poolSizes.end()) {
        poolSizes.push_back(VkDescriptorPoolSize{type, descriptorCount});
        return;
    }
    it->descriptorCount = std::max(it->descriptorCount, descriptorCount);
}

static void _ExpandDescriptorPoolSizesForLayoutVulkan(
    vector<VkDescriptorPoolSize>& poolSizes,
    DescriptorSetLayoutVulkan* layout,
    std::optional<uint32_t> variableDescriptorCount) noexcept {
    if (layout == nullptr) {
        return;
    }
    vector<VkDescriptorPoolSize> requiredSizes{};
    for (const auto& binding : layout->_bindings) {
        uint32_t descriptorCount = binding.descriptorCount;
        if ((binding.bindingFlags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0 &&
            variableDescriptorCount.has_value()) {
            descriptorCount = variableDescriptorCount.value();
        }
        _AccumulateDescriptorPoolSizeVulkan(requiredSizes, binding.descriptorType, descriptorCount);
    }
    for (const auto& requiredSize : requiredSizes) {
        _EnsureDescriptorPoolSizeVulkan(poolSizes, requiredSize.type, requiredSize.descriptorCount);
    }
}

DescriptorSetAllocatorVulkan::DescriptorSetAllocatorVulkan(
    DeviceVulkan* device,
    uint32_t keepFreePages,
    std::optional<vector<VkDescriptorPoolSize>> specPoolSize,
    uint32_t maxSetsPerPage,
    uint32_t maxAllocations,
    uint32_t maxPages,
    bool strictPoolSizes) noexcept
    : _device(device),
      _specPoolSize(std::move(specPoolSize)),
      _keepFreePages(keepFreePages),
      _maxSetsPerPage(maxSetsPerPage),
      _maxAllocations(maxAllocations),
      _maxPages(maxPages),
      _strictPoolSizes(strictPoolSizes) {}

DescriptorSetAllocatorVulkan::~DescriptorSetAllocatorVulkan() noexcept = default;

std::optional<DescriptorSetAllocatorVulkan::Allocation> DescriptorSetAllocatorVulkan::Allocate(
    DescriptorSetLayoutVulkan* layout,
    std::optional<uint32_t> variableDescriptorCount) noexcept {
    if (_liveAllocationCount >= _maxAllocations) {
        RADRAY_ERR_LOG("vk descriptor pool binding-group capacity exhausted ({})", _maxAllocations);
        return std::nullopt;
    }
    if (layout == nullptr || !layout->IsValid()) {
        RADRAY_ERR_LOG("vk descriptor set layout is invalid");
        return std::nullopt;
    }
    if (variableDescriptorCount.has_value()) {
        bool foundVariableBinding = false;
        for (const auto& binding : layout->_bindings) {
            if ((binding.bindingFlags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) == 0) {
                continue;
            }
            foundVariableBinding = true;
            if (variableDescriptorCount.value() > binding.descriptorCount) {
                RADRAY_ERR_LOG(
                    "vk variable descriptor count exceeds layout capacity. requested={}, capacity={}",
                    variableDescriptorCount.value(),
                    binding.descriptorCount);
                return std::nullopt;
            }
        }
        if (!foundVariableBinding) {
            RADRAY_ERR_LOG("vk variable descriptor count was provided for a non-variable descriptor set layout");
            return std::nullopt;
        }
    }
    if (_pages.empty()) {
        if (!NewPage(layout, variableDescriptorCount)) {
            return std::nullopt;
        }
    }
    auto tryAllocFrom = [&](size_t pageIndex, bool logOutOfPool) -> VkDescriptorSet {
        auto* page = _pages[pageIndex].get();
        VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
        uint32_t variableCount = 0;
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.pNext = nullptr;
        allocInfo.descriptorPool = page->_pool;
        allocInfo.descriptorSetCount = 1;
        VkDescriptorSetLayout l = layout->_layout;
        allocInfo.pSetLayouts = &l;
        if (variableDescriptorCount.has_value()) {
            variableCount = variableDescriptorCount.value();
            variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
            variableInfo.pNext = nullptr;
            variableInfo.descriptorSetCount = 1;
            variableInfo.pDescriptorCounts = &variableCount;
            allocInfo.pNext = &variableInfo;
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        auto vr = _device->_ftb.vkAllocateDescriptorSets(_device->_device, &allocInfo, &set);
        if (vr == VK_SUCCESS) {
            page->_liveCount += 1;
            _liveAllocationCount += 1;
            _hintPage = pageIndex;
            return set;
        }
        if (vr == VK_ERROR_OUT_OF_POOL_MEMORY || vr == VK_ERROR_FRAGMENTED_POOL) {
            if (logOutOfPool) {
                RADRAY_ERR_LOG("vkAllocateDescriptorSets failed: {}", vr);
            }
            return VK_NULL_HANDLE;
        }
        RADRAY_ERR_LOG("vkAllocateDescriptorSets failed: {}", vr);
        return VK_NULL_HANDLE;
    };
    const size_t start = _hintPage < _pages.size() ? _hintPage : 0;
    for (size_t i = 0; i < _pages.size(); i++) {
        size_t idx = (start + i) % _pages.size();
        VkDescriptorSet set = tryAllocFrom(idx, false);
        if (set != VK_NULL_HANDLE) {
            return std::make_optional(Allocation{_pages[idx].get(), set});
        }
    }
    auto* newPage = NewPage(layout, variableDescriptorCount);
    if (!newPage) {
        return std::nullopt;
    }
    const size_t newPageIndex = _pages.size() - 1;
    VkDescriptorSet set = tryAllocFrom(newPageIndex, true);
    if (set == VK_NULL_HANDLE) {
        return std::nullopt;
    }
    return std::make_optional(Allocation{newPage, set});
}

void DescriptorSetAllocatorVulkan::Destroy(Allocation allocation) noexcept {
    RADRAY_ASSERT(allocation.Pool->_alloc == this);
    RADRAY_ASSERT(allocation.Set != VK_NULL_HANDLE);
    if (auto vr = _device->_ftb.vkFreeDescriptorSets(_device->_device, allocation.Pool->_pool, 1, &allocation.Set);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkFreeDescriptorSets failed: {}", vr);
        return;
    }
    allocation.Pool->_liveCount -= 1;
    _liveAllocationCount -= 1;
    if (allocation.Pool->_liveCount == 0) {
        TryReleaseFreePages();
    }
}

bool DescriptorSetAllocatorVulkan::Reset() noexcept {
    if (_liveAllocationCount != 0) {
        RADRAY_ERR_LOG(
            "cannot reset vk descriptor pool while {} binding groups are alive",
            _liveAllocationCount);
        return false;
    }
    _pages.clear();
    _hintPage = 0;
    return true;
}

DescriptorPoolVulkan* DescriptorSetAllocatorVulkan::NewPage(
    DescriptorSetLayoutVulkan* layout,
    std::optional<uint32_t> variableDescriptorCount) noexcept {
    if (_pages.size() >= _maxPages) {
        return nullptr;
    }
    VkDescriptorPoolInlineUniformBlockCreateInfo inlineInfo{};
    VkDescriptorPoolInlineUniformBlockCreateInfo* pInlineInfo = nullptr;
    vector<VkDescriptorPoolSize> poolSizes{};
    if (_specPoolSize.has_value()) {
        poolSizes = _specPoolSize.value();
    } else {
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1024});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1024});
        if (_device->_detail.IsRayTracingSupported) {
            poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1024});
        }
        if (_device->_extFeatures.feature13.inlineUniformBlock) {
            poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT, 1024});
            pInlineInfo = &inlineInfo;
            inlineInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO;
            inlineInfo.pNext = nullptr;
            inlineInfo.maxInlineUniformBlockBindings = 1024;
        }
    }
    if (!_strictPoolSizes) {
        _ExpandDescriptorPoolSizesForLayoutVulkan(poolSizes, layout, variableDescriptorCount);
    }
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.pNext = pInlineInfo;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = _maxSetsPerPage;
    info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    info.pPoolSizes = poolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (auto vr = _device->_ftb.vkCreateDescriptorPool(_device->_device, &info, _device->GetAllocationCallbacks(), &pool);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateDescriptorPool failed: {}", vr);
        return nullptr;
    }
    auto page = make_unique<DescriptorPoolVulkan>(_device, this, pool, info.maxSets);
    return _pages.emplace_back(std::move(page)).get();
}

BindingDescriptorPoolVulkan::BindingDescriptorPoolVulkan(
    DeviceVulkan* device,
    const DescriptorPoolDescriptor& desc,
    unique_ptr<DescriptorSetAllocatorVulkan> allocator) noexcept
    : _device(device), _desc(desc), _allocator(std::move(allocator)) {}

BindingDescriptorPoolVulkan::~BindingDescriptorPoolVulkan() noexcept {
    Destroy();
}

bool BindingDescriptorPoolVulkan::IsValid() const noexcept {
    return _device != nullptr && _allocator != nullptr;
}

void BindingDescriptorPoolVulkan::Destroy() noexcept {
    if (_liveGroups != 0) {
        RADRAY_ERR_LOG(
            "cannot destroy vk descriptor pool while {} binding groups are alive",
            _liveGroups);
        return;
    }
    _allocator.reset();
    _device = nullptr;
    _name.clear();
}

void BindingDescriptorPoolVulkan::SetDebugName(std::string_view name) noexcept {
    _name = string{name};
}

bool BindingDescriptorPoolVulkan::Reset() noexcept {
    if (_liveGroups != 0 || _allocator == nullptr) {
        RADRAY_ERR_LOG(
            "cannot reset vk descriptor pool while {} binding groups are alive",
            _liveGroups);
        return false;
    }
    if (!_allocator->Reset()) {
        return false;
    }
    _used = {};
    return true;
}

uint32_t BindingDescriptorPoolVulkan::GetAllocatedBindingGroupCount() const noexcept {
    return _liveGroups;
}

bool BindingDescriptorPoolVulkan::ReserveGroup(const DescriptorPoolDescriptor& usage) noexcept {
    const auto fits = [](uint32_t used, uint32_t requested, uint32_t capacity) noexcept {
        return requested <= capacity - std::min(used, capacity);
    };
    if (!IsValid() || _liveGroups >= _desc.MaxBindingGroups ||
        !fits(_used.MaxSampledTextures, usage.MaxSampledTextures, _desc.MaxSampledTextures) ||
        !fits(_used.MaxStorageTextures, usage.MaxStorageTextures, _desc.MaxStorageTextures) ||
        !fits(_used.MaxUniformBuffers, usage.MaxUniformBuffers, _desc.MaxUniformBuffers) ||
        !fits(_used.MaxDynamicUniformBuffers, usage.MaxDynamicUniformBuffers, _desc.MaxDynamicUniformBuffers) ||
        !fits(_used.MaxStorageBuffers, usage.MaxStorageBuffers, _desc.MaxStorageBuffers) ||
        !fits(_used.MaxReadOnlyTexelBuffers, usage.MaxReadOnlyTexelBuffers, _desc.MaxReadOnlyTexelBuffers) ||
        !fits(_used.MaxReadWriteTexelBuffers, usage.MaxReadWriteTexelBuffers, _desc.MaxReadWriteTexelBuffers) ||
        !fits(_used.MaxSamplers, usage.MaxSamplers, _desc.MaxSamplers) ||
        !fits(_used.MaxAccelerationStructures, usage.MaxAccelerationStructures, _desc.MaxAccelerationStructures)) {
        RADRAY_ERR_LOG("vk descriptor pool capacity exhausted");
        return false;
    }
    ++_liveGroups;
    _used.MaxSampledTextures += usage.MaxSampledTextures;
    _used.MaxStorageTextures += usage.MaxStorageTextures;
    _used.MaxUniformBuffers += usage.MaxUniformBuffers;
    _used.MaxDynamicUniformBuffers += usage.MaxDynamicUniformBuffers;
    _used.MaxStorageBuffers += usage.MaxStorageBuffers;
    _used.MaxReadOnlyTexelBuffers += usage.MaxReadOnlyTexelBuffers;
    _used.MaxReadWriteTexelBuffers += usage.MaxReadWriteTexelBuffers;
    _used.MaxSamplers += usage.MaxSamplers;
    _used.MaxAccelerationStructures += usage.MaxAccelerationStructures;
    return true;
}

void BindingDescriptorPoolVulkan::ReleaseGroup(const DescriptorPoolDescriptor& usage) noexcept {
    RADRAY_ASSERT(
        _liveGroups > 0 &&
        _used.MaxSampledTextures >= usage.MaxSampledTextures &&
        _used.MaxStorageTextures >= usage.MaxStorageTextures &&
        _used.MaxUniformBuffers >= usage.MaxUniformBuffers &&
        _used.MaxDynamicUniformBuffers >= usage.MaxDynamicUniformBuffers &&
        _used.MaxStorageBuffers >= usage.MaxStorageBuffers &&
        _used.MaxReadOnlyTexelBuffers >= usage.MaxReadOnlyTexelBuffers &&
        _used.MaxReadWriteTexelBuffers >= usage.MaxReadWriteTexelBuffers &&
        _used.MaxSamplers >= usage.MaxSamplers &&
        _used.MaxAccelerationStructures >= usage.MaxAccelerationStructures);
    --_liveGroups;
    _used.MaxSampledTextures -= usage.MaxSampledTextures;
    _used.MaxStorageTextures -= usage.MaxStorageTextures;
    _used.MaxUniformBuffers -= usage.MaxUniformBuffers;
    _used.MaxDynamicUniformBuffers -= usage.MaxDynamicUniformBuffers;
    _used.MaxStorageBuffers -= usage.MaxStorageBuffers;
    _used.MaxReadOnlyTexelBuffers -= usage.MaxReadOnlyTexelBuffers;
    _used.MaxReadWriteTexelBuffers -= usage.MaxReadWriteTexelBuffers;
    _used.MaxSamplers -= usage.MaxSamplers;
    _used.MaxAccelerationStructures -= usage.MaxAccelerationStructures;
}

void DescriptorSetAllocatorVulkan::TryReleaseFreePages() noexcept {
    if (_pages.size() <= 1) {
        return;
    }
    size_t idleCount = 0;
    for (const auto& pagePtr : _pages) {
        const auto* page = pagePtr.get();
        if (page->_liveCount == 0) {
            idleCount += 1;
        }
    }
    if (idleCount <= _keepFreePages) {
        return;
    }
    for (size_t i = _pages.size(); i-- > 0 && idleCount > _keepFreePages;) {
        auto* page = _pages[i].get();
        if (page->_liveCount != 0) {
            continue;
        }
        if (_pages.size() <= 1) {
            break;
        }
        const size_t last = _pages.size() - 1;
        if (i != last) {
            std::swap(_pages[i], _pages[last]);
        }
        _pages.pop_back();
        idleCount -= 1;
        if (_hintPage >= _pages.size()) {
            _hintPage = 0;
        }
    }
}

SamplerVulkan::SamplerVulkan(
    DeviceVulkan* device,
    VkSampler sampler) noexcept
    : _device(device),
      _sampler(sampler) {}

SamplerVulkan::~SamplerVulkan() noexcept {
    this->DestroyImpl();
}

bool SamplerVulkan::IsValid() const noexcept {
    return _sampler != VK_NULL_HANDLE;
}

void SamplerVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void SamplerVulkan::SetDebugName(std::string_view name) noexcept {
    _device->SetObjectName(name, _sampler);
}

void SamplerVulkan::DestroyImpl() noexcept {
    if (_sampler != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroySampler(_device->_device, _sampler, _device->GetAllocationCallbacks());
        _sampler = VK_NULL_HANDLE;
    }
}

BindlessArrayVulkan::BindlessArrayVulkan(
    DeviceVulkan* device,
    const BindlessArrayDescriptor& desc) noexcept
    : _device(device),
      _desc(desc),
      _size(desc.Size),
      _slotType(desc.SlotType),
      _slots(desc.Size) {}

BindlessArrayVulkan::~BindlessArrayVulkan() noexcept {
    this->DestroyImpl();
}

bool BindlessArrayVulkan::IsValid() const noexcept {
    return _device != nullptr && _size > 0;
}

void BindlessArrayVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void BindlessArrayVulkan::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
}

void BindlessArrayVulkan::DestroyImpl() noexcept {
    if (_device != nullptr) {
        for (auto& cached : _cachedSets) {
            if (cached.Allocation.IsValid()) {
                _device->_descSetAlloc->Destroy(cached.Allocation);
                cached.Allocation = DescriptorSetAllocatorVulkan::Allocation::Invalid();
            }
        }
    }
    _cachedSets.clear();
    _slots.clear();
    _desc = {};
    _size = 0;
    _slotType = BindlessSlotType::Multiple;
    _name.clear();
    _device = nullptr;
}

void BindlessArrayVulkan::SetBuffer(uint32_t slot, const BufferBindingDescriptor& desc) noexcept {
    if (_slotType != BindlessSlotType::BufferOnly) {
        RADRAY_ERR_LOG("vk bindless array does not support buffer slots");
        return;
    }
    if (slot >= _size) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _size, slot);
        return;
    }
    const auto bindType = BufferViewUsageToResourceBindType(desc.Usage);
    if (bindType != ResourceBindType::Buffer &&
        bindType != ResourceBindType::RWBuffer) {
        RADRAY_ERR_LOG(
            "vk bindless array does not support buffer binding type {}",
            bindType);
        return;
    }
    if (desc.Target == nullptr) {
        RADRAY_ERR_LOG("vk bindless array buffer target is null");
        return;
    }
    uint64_t rangeSize = 0;
    if (!_ResolveBufferBindingRangeSizeVulkan(desc, rangeSize)) {
        return;
    }
    if (desc.Stride == 0) {
        RADRAY_ERR_LOG("vk bindless array structured buffer stride must be non-zero");
        return;
    }
    if (desc.Range.Offset % desc.Stride != 0 || rangeSize % desc.Stride != 0) {
        RADRAY_ERR_LOG("vk bindless array structured buffer offset/size must align to stride");
        return;
    }
    auto& slotState = _slots[slot];
    slotState.Kind = SlotKind::Buffer;
    slotState.ResourceType = bindType;
    slotState.BufferDesc = desc;
    slotState.Texture = nullptr;
    for (auto& cached : _cachedSets) {
        cached.Dirty = true;
    }
}

void BindlessArrayVulkan::SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept {
    RADRAY_UNUSED(sampler);
    if (_slotType != BindlessSlotType::Texture2DOnly) {
        RADRAY_ERR_LOG("vk bindless array does not support texture slots");
        return;
    }
    if (slot >= _size) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _size, slot);
        return;
    }
    if (texView == nullptr) {
        RADRAY_ERR_LOG("vk bindless array texture view is null");
        return;
    }
    auto view = CastVkObject(texView);
    auto dim = view->_mdesc.Dim;
    if (dim != TextureDimension::Dim2D) {
        RADRAY_ERR_LOG("vk bindless array only supports texture 2D");
        return;
    }
    auto bindType = _GetResourceViewBindTypeVulkan(texView);
    if (!bindType.has_value() || bindType.value() != ResourceBindType::Texture) {
        RADRAY_ERR_LOG(
            "vk bindless array does not support texture view type {}",
            bindType.has_value() ? bindType.value() : ResourceBindType::UNKNOWN);
        return;
    }
    auto& slotState = _slots[slot];
    slotState.Kind = SlotKind::Texture2D;
    slotState.ResourceType = bindType.value();
    slotState.BufferDesc = {};
    slotState.Texture = texView;
    for (auto& cached : _cachedSets) {
        cached.Dirty = true;
    }
}

}  // namespace radray::render::vulkan
