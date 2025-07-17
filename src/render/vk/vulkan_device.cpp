#include "vulkan_device.h"

#include "vulkan_fence.h"
#include "vulkan_shader_module.h"
#include "vulkan_image.h"
#include "vulkan_swapchain.h"

namespace radray::render::vulkan {

class InstanceVulkan {
public:
    InstanceVulkan() = default;
    InstanceVulkan(const InstanceVulkan&) = delete;
    InstanceVulkan& operator=(const InstanceVulkan&) = delete;
    InstanceVulkan(InstanceVulkan&&) = delete;
    InstanceVulkan& operator=(InstanceVulkan&&) = delete;
    ~InstanceVulkan() noexcept {
        if (_instance != nullptr) {
            const VkAllocationCallbacks* allocCbPtr = this->GetAllocationCallbacks();
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
    if (d._alloc != VK_NULL_HANDLE) {
        vmaDestroyAllocator(d._alloc);
        d._alloc = VK_NULL_HANDLE;
    }
    if (d._device != VK_NULL_HANDLE) {
        d.CallVk(&FTbVk::vkDestroyDevice, d.GetAllocationCallbacks());
        d._device = VK_NULL_HANDLE;
    }
    d._physicalDevice = VK_NULL_HANDLE;
    d._instance = VK_NULL_HANDLE;
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

Nullable<shared_ptr<Fence>> DeviceVulkan::CreateFence() noexcept {
    return this->CreateFenceVk(VK_FENCE_CREATE_SIGNALED_BIT);
}

Nullable<shared_ptr<Shader>> DeviceVulkan::CreateShader(
    std::span<const byte> blob,
    ShaderBlobCategory category,
    ShaderStage stage,
    std::string_view entryPoint,
    std::string_view name) noexcept {
    if (category != ShaderBlobCategory::SPIRV) {
        RADRAY_ERR_LOG("vk only support SPIRV");
        return nullptr;
    }
    VkShaderModuleCreateInfo moduleInfo{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        nullptr,
        0,
        blob.size(),
        reinterpret_cast<const uint32_t*>(blob.data())};
    VkShaderModule shaderModule;
    if (auto vr = CallVk(&FTbVk::vkCreateShaderModule, &moduleInfo, this->GetAllocationCallbacks(), &shaderModule);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateShaderModule failed: {}", vr);
        return nullptr;
    }
    return make_shared<ShaderModuleVulkan>(this, shaderModule, string{name}, string{entryPoint}, stage, category);
}

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
    bool enableSync) noexcept {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    {
        HMODULE hInstance;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)(void*)&g_instance,
                &hInstance) == 0) {
            RADRAY_ERR_LOG("vk call win32 GetModuleHandleExW failed. (code={})", GetLastError());
            return nullptr;
        }
        VkWin32SurfaceCreateInfoKHR win32SurfaceInfo{};
        win32SurfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        win32SurfaceInfo.pNext = nullptr;
        win32SurfaceInfo.flags = 0;
        win32SurfaceInfo.hinstance = hInstance;
        win32SurfaceInfo.hwnd = reinterpret_cast<HWND>(const_cast<void*>(nativeWindow));
        if (auto vr = vkCreateWin32SurfaceKHR(_instance, &win32SurfaceInfo, this->GetAllocationCallbacks(), &surface);
            vr != VK_SUCCESS) {
            RADRAY_ERR_LOG("vk call vkCreateWin32SurfaceKHR failed: {}", vr);
            return nullptr;
        }
    }
#else
#error "unsupported platform for Vulkan surface creation"
#endif
    if (surface == nullptr) {
        RADRAY_ERR_LOG("vk create VkSurfaceKHR failed");
        return nullptr;
    }

    auto result = make_shared<SwapChainVulkan>(this, static_cast<QueueVulkan*>(presentQueue));
    result->_surface = surface;

    VkSurfaceCapabilitiesKHR surfaceProperties;
    if (auto vr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, surface, &surfaceProperties);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: {}", vr);
        return nullptr;
    }
    if (backBufferCount < surfaceProperties.minImageCount || backBufferCount > surfaceProperties.maxImageCount) {
        RADRAY_ERR_LOG("vk back buffer count {} not in range [{}, {}]", backBufferCount, surfaceProperties.minImageCount, surfaceProperties.maxImageCount);
        return nullptr;
    }
    VkExtent2D swapchainSize;
    if (surfaceProperties.currentExtent.width == 0xFFFFFFFF) {
        swapchainSize.width = width;
        swapchainSize.height = height;
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
    VkFormat rawFormat = MapType(format);
    vector<VkSurfaceFormatKHR> supportedFormats;
    if (auto vr = GetVector(supportedFormats, vkGetPhysicalDeviceSurfaceFormatsKHR, this->_physicalDevice, surface);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}", vr);
        return nullptr;
    }
    auto needFormatIter = std::ranges::find_if(supportedFormats, [rawFormat](VkSurfaceFormatKHR i) { return i.format == rawFormat; });
    if (needFormatIter == supportedFormats.end()) {
        RADRAY_ERR_LOG("vk surface format {} not supported", rawFormat);
        return nullptr;
    }
    const VkSurfaceFormatKHR& needFormat = *needFormatIter;
    vector<VkPresentModeKHR> supportedPresentModes;
    if (auto vr = GetVector(supportedPresentModes, vkGetPhysicalDeviceSurfacePresentModesKHR, this->_physicalDevice, surface);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkGetPhysicalDeviceSurfacePresentModesKHR failed: {}", vr);
        return nullptr;
    }
    VkPresentModeKHR needPresentMode = enableSync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (std::ranges::find(supportedPresentModes, needPresentMode) == supportedPresentModes.end()) {
        RADRAY_ERR_LOG("vk present mode {} not supported", needPresentMode);
        return nullptr;
    }
    VkSwapchainCreateInfoKHR swapchianCreateInfo{};
    swapchianCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchianCreateInfo.pNext = nullptr;
    swapchianCreateInfo.flags = 0;
    swapchianCreateInfo.surface = surface;
    swapchianCreateInfo.minImageCount = backBufferCount;
    swapchianCreateInfo.imageFormat = needFormat.format;
    swapchianCreateInfo.imageColorSpace = needFormat.colorSpace;
    swapchianCreateInfo.imageExtent = swapchainSize;
    swapchianCreateInfo.imageArrayLayers = 1;
    swapchianCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchianCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchianCreateInfo.queueFamilyIndexCount = 0;
    swapchianCreateInfo.pQueueFamilyIndices = nullptr;
    swapchianCreateInfo.preTransform = preTransform;
    swapchianCreateInfo.compositeAlpha = composite;
    swapchianCreateInfo.presentMode = needPresentMode;
    swapchianCreateInfo.clipped = VK_TRUE;
    swapchianCreateInfo.oldSwapchain = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain{};
    if (auto vr = this->CallVk(&FTbVk::vkCreateSwapchainKHR, &swapchianCreateInfo, this->GetAllocationCallbacks(), &swapchain);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateSwapchainKHR failed: {}", vr);
        return nullptr;
    }
    result->_swapchain = swapchain;
    vector<VkImage> swapchainImages;
    if (auto vr = GetVector(swapchainImages, _vtb.vkGetSwapchainImagesKHR, _device, swapchain);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkGetSwapchainImagesKHR failed: {}", vr);
        return nullptr;
    }
    vector<SwapChainFrame> frames;
    frames.reserve(swapchainImages.size());
    for (VkImage img : swapchainImages) {
        auto color = make_shared<ImageVulkan>(this, img, VK_NULL_HANDLE, VmaAllocationInfo{}, ImageVulkanDescriptor{});
        SwapChainFrame frame{};
        frame._color = std::move(color);
        frame._acquireSemaphore = nullptr;
        frame._releaseSemaphore = this->CreateSemaphoreVk(0).Unwrap();
        frame._submitFence = this->CreateFenceVk(VK_FENCE_CREATE_SIGNALED_BIT).Unwrap();
        frames.emplace_back(std::move(frame));
    }
    result->_frames = std::move(frames);
    return result;
}

Nullable<shared_ptr<Buffer>> DeviceVulkan::CreateBuffer(
    uint64_t size,
    ResourceType type,
    ResourceMemoryUsage usage,
    ResourceStates initState,
    ResourceHints tips,
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
    ResourceHints tips,
    std::string_view) noexcept {
    return nullptr;
}

Nullable<shared_ptr<Texture>> DeviceVulkan::CreateTexture(const TextureCreateDescriptor& desc) noexcept {
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.pNext = nullptr;
    imgInfo.flags = 0;
    imgInfo.imageType = MapType(desc.Dim);
    imgInfo.format = MapType(desc.Format);
    imgInfo.extent.width = static_cast<uint32_t>(desc.Width);
    imgInfo.extent.height = static_cast<uint32_t>(desc.Height);
    if (desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim2D) {
        imgInfo.extent.depth = 1;
    } else {
        imgInfo.extent.depth = static_cast<uint32_t>(desc.DepthOrArraySize);
    }
    imgInfo.mipLevels = desc.MipLevels;
    if (desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim3D) {
        imgInfo.arrayLayers = 1;
    } else {
        imgInfo.arrayLayers = desc.DepthOrArraySize;
    }
    imgInfo.samples = ([](uint32_t sampleCount) noexcept {
        switch (sampleCount) {
            case 1: return VK_SAMPLE_COUNT_1_BIT;
            case 2: return VK_SAMPLE_COUNT_2_BIT;
            case 4: return VK_SAMPLE_COUNT_4_BIT;
            case 8: return VK_SAMPLE_COUNT_8_BIT;
            case 16: return VK_SAMPLE_COUNT_16_BIT;
            case 32: return VK_SAMPLE_COUNT_32_BIT;
            default:
                RADRAY_ERR_LOG("vk unsupported sample count: {}", sampleCount);
                return VK_SAMPLE_COUNT_1_BIT;
        }
    })(desc.SampleCount);
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = 0;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.queueFamilyIndexCount = 0;
    imgInfo.pQueueFamilyIndices = nullptr;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.Dim == TextureDimension::Dim1D && desc.DepthOrArraySize % 6 == 0 && desc.SampleCount == 1 && desc.Width == desc.Height) {
        imgInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::CopySource)) {
        imgInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::CopyDestination)) {
        imgInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::Resource)) {
        imgInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::RenderTarget)) {
        imgInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::DepthStencilRead) || desc.Usage.HasFlag(TextureUse::DepthStencilWrite)) {
        imgInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (desc.Usage.HasFlag(TextureUse::StorageRead) || desc.Usage.HasFlag(TextureUse::StorageWrite) || desc.Usage.HasFlag(TextureUse::StorageRW)) {
        imgInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    VmaAllocationCreateInfo vmaInfo{};
    vmaInfo.flags = 0;
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    vmaInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaInfo.requiredFlags = 0;
    vmaInfo.preferredFlags = 0;
    vmaInfo.memoryTypeBits = 0;
    vmaInfo.pool = VK_NULL_HANDLE;
    vmaInfo.pUserData = nullptr;
    vmaInfo.priority = 0;

    VkImage vkImg = VK_NULL_HANDLE;
    VmaAllocation vmaAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo vmaAllocInfo{};
    if (auto vr = vmaCreateImage(_alloc, &imgInfo, &vmaInfo, &vkImg, &vmaAlloc, &vmaAllocInfo);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk failed to create image: {}", vr);
        return nullptr;
    }
    auto imgDesc = ImageVulkanDescriptor::FromRaw(desc);
    imgDesc._rawFormat = imgInfo.format;
    return make_shared<ImageVulkan>(this, vkImg, vmaAlloc, vmaAllocInfo, imgDesc);
}

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
    TextureViewDimension dim,
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

const VkAllocationCallbacks* DeviceVulkan::GetAllocationCallbacks() const noexcept {
    return g_instance->GetAllocationCallbacks();
}

Nullable<shared_ptr<SemaphoreVulkan>> DeviceVulkan::CreateSemaphoreVk(VkSemaphoreCreateFlags flags) noexcept {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = nullptr;
    info.flags = flags;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (auto vr = this->CallVk(&FTbVk::vkCreateSemaphore, &info, this->GetAllocationCallbacks(), &semaphore);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateSemaphore failed {}", vr);
        return nullptr;
    }
    return make_shared<SemaphoreVulkan>(this, semaphore);
}

Nullable<shared_ptr<FenceVulkan>> DeviceVulkan::CreateFenceVk(VkFenceCreateFlags flags) noexcept {
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = nullptr;
    fenceInfo.flags = flags;
    VkFence fence = VK_NULL_HANDLE;
    if (auto vr = CallVk(&FTbVk::vkCreateFence, &fenceInfo, this->GetAllocationCallbacks(), &fence);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk create fence failed: {}", vr);
        return nullptr;
    }
    return make_shared<FenceVulkan>(this, fence);
}

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

    unordered_set<string> needExts;
    needExts.emplace(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    vector<VkExtensionProperties> deviceExtsAvailable;
    if (GetVector(deviceExtsAvailable, vkEnumerateDeviceExtensionProperties, selectPhyDevice.device, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateDeviceExtensionProperties failed");
        return nullptr;
    }
    for (const auto& ext : needExts) {
        const char* exts[] = {ext.c_str()};
        if (!IsValidateExtensions(exts, deviceExtsAvailable)) {
            RADRAY_ERR_LOG("vk device extension {} not supported", ext);
            return nullptr;
        }
    }

    vector<const char*> deviceExts{};
    deviceExts.reserve(needExts.size());
    for (const auto& ext : needExts) {
        deviceExts.emplace_back(ext.c_str());
    }

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
    auto deviceR = make_shared<DeviceVulkan>();
    deviceR->_instance = g_instance->_instance;
    deviceR->_physicalDevice = selectPhyDevice.device;
    deviceR->_device = device;
    volkLoadDeviceTable(&deviceR->_vtb, deviceR->_device);
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
    vmaCreateInfo.instance = deviceR->_instance;
    vmaCreateInfo.vulkanApiVersion = selectPhyDevice.properties.apiVersion;
#if VMA_EXTERNAL_MEMORY
    vmaCreateInfo.pTypeExternalMemoryHandleTypes = nullptr;
#endif
    if (auto vr = vmaImportVulkanFunctionsFromVolk(&vmaCreateInfo, &vmaFunctions);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vmaImportVulkanFunctionsFromVolk failed: {}", vr);
        return nullptr;
    }
    if (auto vr = vmaCreateAllocator(&vmaCreateInfo, &deviceR->_alloc);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vmaCreateAllocator failed: {}", vr);
        return nullptr;
    }

    RADRAY_INFO_LOG("========== Feature ==========");
    for (const auto& i : queueRequests) {
        for (const auto& j : i.queueIndices) {
            VkQueue queuePtr = VK_NULL_HANDLE;
            deviceR->CallVk(&FTbVk::vkGetDeviceQueue, j.Family, j.IndexInFamily, &queuePtr);
            if (queuePtr == VK_NULL_HANDLE) {
                RADRAY_ERR_LOG("vk get queue for family {} index {} failed", j.Family, j.IndexInFamily);
                return nullptr;
            }
            auto queue = make_unique<QueueVulkan>(deviceR.get(), queuePtr, j, i.rawType);
            deviceR->_queues[(size_t)i.rawType].emplace_back(std::move(queue));
        }
    }
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
        const auto requireExt = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        const char* requireExts[] = {requireExt};
        if (!IsValidateExtensions(requireExts, extProps)) {
            RADRAY_WARN_LOG("vk extension {} is not supported", requireExt);
        } else {
            needExts.emplace(requireExt);
        }
        const auto validName = "VK_LAYER_KHRONOS_validation";
        const char* requireLayer[] = {validName};
        if (!IsValidateLayers(requireLayer, layerProps)) {
            RADRAY_WARN_LOG("vk layer {} is not supported", validName);
        } else {
            needLayers.emplace(validName);
            isValidFeatureExtEnable = true;
        }
    }
    for (const auto& i : needLayers) {
        const char* require[] = {i.c_str()};
        if (!IsValidateLayers(require, layerProps)) {
            RADRAY_ERR_LOG("vk layer {} is not supported", i);
            return false;
        }
    }
    for (const auto& i : needExts) {
        const char* require[] = {i.c_str()};
        if (!IsValidateExtensions(require, extProps)) {
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
        createInfo.pNext = &validFeature;
        validFeature.pNext = &debugCreateInfo;
    }
    VkInstance instance = VK_NULL_HANDLE;
    if (auto vr = vkCreateInstance(&createInfo, allocCbPtr, &instance);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateInstance failed: {}", vr);
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
