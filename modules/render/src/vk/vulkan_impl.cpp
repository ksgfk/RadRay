#include "vulkan_impl.h"

#include <bit>
#include <cstring>

namespace radray::render::vulkan {

static auto CastVkObject(CommandQueue* p) noexcept { return static_cast<QueueVulkan*>(p); }
static auto CastVkObject(CommandBuffer* p) noexcept { return static_cast<CommandBufferVulkan*>(p); }
static auto CastVkObject(Fence* p) noexcept { return static_cast<TimelineSemaphoreVulkan*>(p); }
static auto CastVkObject(Buffer* p) noexcept { return static_cast<BufferVulkan*>(p); }
static auto CastVkObject(Texture* p) noexcept { return static_cast<ImageVulkan*>(p); }
static auto CastVkObject(TextureView* p) noexcept { return static_cast<ImageViewVulkan*>(p); }
static auto CastVkObject(Shader* p) noexcept { return static_cast<ShaderModuleVulkan*>(p); }
static auto CastVkObject(RootSignature* p) noexcept { return static_cast<PipelineLayoutVulkan*>(p); }
static auto CastVkObject(GraphicsPipelineState* p) noexcept { return static_cast<GraphicsPipelineVulkan*>(p); }
static auto CastVkObject(DescriptorSetLayout* p) noexcept { return static_cast<DescriptorSetLayoutVulkan*>(p); }
static auto CastVkObject(DescriptorSet* p) noexcept { return static_cast<DescriptorSetVulkanWrapper*>(p); }

static Nullable<InstanceVulkanImpl*> g_vkInstance = nullptr;

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

const VkAllocationCallbacks* InstanceVulkanImpl::GetAllocationCallbacks() const noexcept {
    return _allocCb.has_value() ? &_allocCb.value() : nullptr;
}

void InstanceVulkanImpl::DestroyImpl() noexcept {
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

Nullable<CommandQueue*> DeviceVulkan::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    return _queues[static_cast<std::underlying_type_t<QueueType>>(type)][slot].get();
}

Nullable<shared_ptr<CommandBuffer>> DeviceVulkan::CreateCommandBuffer(CommandQueue* queue_) noexcept {
    auto queue = CastVkObject(queue_);
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.pNext = nullptr;
    poolInfo.flags = 0;
    poolInfo.queueFamilyIndex = queue->_family.Family;
    VkCommandPool pool{VK_NULL_HANDLE};
    if (auto vr = _ftb.vkCreateCommandPool(_device, &poolInfo, this->GetAllocationCallbacks(), &pool);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateCommandPool failed: {}", vr);
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
        RADRAY_ERR_LOG("vk call vkAllocateCommandBuffers failed: {}", vr);
        return nullptr;
    }
    return make_shared<CommandBufferVulkan>(this, queue, std::move(cmdPool), cmdBuf);
}

Nullable<shared_ptr<Fence>> DeviceVulkan::CreateFence(uint64_t initValue) noexcept {
    auto fence = this->CreateTimelineSemaphore(initValue);
    if (!fence.HasValue()) {
        return nullptr;
    }
    return shared_ptr{fence.Release()};
}

Nullable<shared_ptr<SwapChain>> DeviceVulkan::CreateSwapChain(const SwapChainDescriptor& desc_) noexcept {
    SwapChainDescriptor desc = desc_;
    unique_ptr<SurfaceVulkan> surface;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    {
        LPCSTR instanceAddr = std::bit_cast<LPCSTR>(&DestroyVulkanInstanceImpl);
        HMODULE hInstance;
        if (GetModuleHandleEx(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                instanceAddr,
                &hInstance) == 0) {
            RADRAY_ERR_LOG("vk call win32 GetModuleHandleExW failed. (code={})", GetLastError());
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
            RADRAY_ERR_LOG("vk call vkCreateWin32SurfaceKHR failed: {}", vr);
            return nullptr;
        }
        surface = make_unique<SurfaceVulkan>(this, vkSurface);
    }
#else
#error "unsupported platform for Vulkan surface creation"
#endif
    if (surface == nullptr) {
        RADRAY_ERR_LOG("vk create VkSurfaceKHR failed");
        return nullptr;
    }
    auto presentQueue = CastVkObject(desc.PresentQueue);
    VkSurfaceCapabilitiesKHR surfaceProperties;
    if (auto vr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, surface->_surface, &surfaceProperties);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: {}", vr);
        return nullptr;
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
    VkExtent2D swapchainSize;
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
    VkFormat rawFormat = MapType(desc.Format);
    vector<VkSurfaceFormatKHR> supportedFormats;
    if (auto vr = EnumerateVectorFromVkFunc(supportedFormats, vkGetPhysicalDeviceSurfaceFormatsKHR, this->_physicalDevice, surface->_surface);
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
    if (auto vr = EnumerateVectorFromVkFunc(supportedPresentModes, vkGetPhysicalDeviceSurfacePresentModesKHR, this->_physicalDevice, surface->_surface);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkGetPhysicalDeviceSurfacePresentModesKHR failed: {}", vr);
        return nullptr;
    }
    VkPresentModeKHR needPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    {
        const VkPresentModeKHR vsyncOffModes[] = {
            VK_PRESENT_MODE_IMMEDIATE_KHR,
            VK_PRESENT_MODE_MAILBOX_KHR,
            VK_PRESENT_MODE_FIFO_KHR};
        const VkPresentModeKHR vsyncOnModes[] = {
            VK_PRESENT_MODE_FIFO_RELAXED_KHR,
            VK_PRESENT_MODE_FIFO_KHR,
            VK_PRESENT_MODE_IMMEDIATE_KHR,
            VK_PRESENT_MODE_MAILBOX_KHR};
        std::span<const VkPresentModeKHR> lookupPresentModes;
        if (desc.EnableSync) {
            lookupPresentModes = vsyncOnModes;
        } else {
            lookupPresentModes = vsyncOffModes;
        }
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
    swapchianCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchianCreateInfo.queueFamilyIndexCount = 0;
    swapchianCreateInfo.pQueueFamilyIndices = nullptr;
    swapchianCreateInfo.preTransform = preTransform;
    swapchianCreateInfo.compositeAlpha = composite;
    swapchianCreateInfo.presentMode = needPresentMode;
    swapchianCreateInfo.clipped = VK_TRUE;
    swapchianCreateInfo.oldSwapchain = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain{};
    if (auto vr = _ftb.vkCreateSwapchainKHR(_device, &swapchianCreateInfo, this->GetAllocationCallbacks(), &swapchain);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateSwapchainKHR failed: {}", vr);
        return nullptr;
    }
    auto result = make_shared<SwapChainVulkan>(this, presentQueue, std::move(surface), swapchain);
    vector<VkImage> swapchainImages;
    if (auto vr = EnumerateVectorFromVkFunc(swapchainImages, _ftb.vkGetSwapchainImagesKHR, _device, swapchain);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkGetSwapchainImagesKHR failed: {}", vr);
        return nullptr;
    }
    result->_frames.reserve(swapchainImages.size());
    for (VkImage img : swapchainImages) {
        SwapChainVulkan::Frame& f = result->_frames.emplace_back();
        f.image = make_unique<ImageVulkan>(this, img, VK_NULL_HANDLE, VmaAllocationInfo{});
        f.image->_mdesc = TextureDescriptor{
            TextureDimension::Dim2D,
            swapchianCreateInfo.imageExtent.width,
            swapchianCreateInfo.imageExtent.height,
            1,
            1,
            1,
            desc.Format,
            TextureUse::RenderTarget,
            ResourceHint::None,
            {}};
        f.image->_rawInfo = VkImageCreateInfo{
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            nullptr,
            0,
            VK_IMAGE_TYPE_2D,
            swapchianCreateInfo.imageFormat,
            VkExtent3D{
                swapchianCreateInfo.imageExtent.width,
                swapchianCreateInfo.imageExtent.height,
                1},
            1,
            1,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            VK_SHARING_MODE_EXCLUSIVE,
            0,
            nullptr,
            VK_IMAGE_LAYOUT_UNDEFINED};
        f.fence = this->CreateLegacyFence(VK_FENCE_CREATE_SIGNALED_BIT).Unwrap();
        f.imageAvailableSemaphore = this->CreateLegacySemaphore(0).Unwrap();
        f.renderFinishedSemaphore = this->CreateLegacySemaphore(0).Unwrap();
    }
    result->_desc = desc;
    return result;
}

Nullable<shared_ptr<Buffer>> DeviceVulkan::CreateBuffer(const BufferDescriptor& desc) noexcept {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.pNext = nullptr;
    bufInfo.flags = 0;
    bufInfo.size = desc.Size;
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
    VmaAllocationCreateInfo vmaInfo{};
    vmaInfo.flags = 0;
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::MapWrite)) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
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
        RADRAY_ERR_LOG("vk failed to create buffer: {}", vr);
        return nullptr;
    }
    auto result = make_shared<BufferVulkan>(this, vkBuf, vmaAlloc, vmaAllocInfo);
    result->_mdesc = desc;
    result->_rawInfo = bufInfo;
    return result;
}

Nullable<shared_ptr<BufferView>> DeviceVulkan::CreateBufferView(const BufferViewDescriptor& desc) noexcept {
    auto buf = CastVkObject(desc.Target);
    shared_ptr<BufferViewVulkan> texelView;
    if (buf->_mdesc.Usage.HasFlag(BufferUse::Resource) || buf->_mdesc.Usage.HasFlag(BufferUse::UnorderedAccess)) {
        VkBufferViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        info.pNext = nullptr;
        info.flags = 0;
        info.buffer = buf->_buffer;
        info.format = MapType(desc.Format);
        info.offset = desc.Range.Offset;
        info.range = desc.Range.Size;
        auto bv = this->CreateBufferView(info);
        if (!bv->IsValid()) {
            return nullptr;
        }
        texelView = bv.Release();
    }
    auto result = make_shared<SimulateBufferViewVulkan>(this, buf, desc.Range);
    result->_texelView = std::move(texelView);
    return result;
}

Nullable<shared_ptr<Texture>> DeviceVulkan::CreateTexture(const TextureDescriptor& desc) noexcept {
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
    imgInfo.samples = MapSampleCount(desc.SampleCount);
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = 0;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.queueFamilyIndexCount = 0;
    imgInfo.pQueueFamilyIndices = nullptr;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.Dim == TextureDimension::Dim2D && desc.DepthOrArraySize % 6 == 0 && desc.SampleCount == 1 && desc.Width == desc.Height) {
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
    if (auto vr = vmaCreateImage(_vma->_vma, &imgInfo, &vmaInfo, &vkImg, &vmaAlloc, &vmaAllocInfo);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk failed to create image: {}", vr);
        return nullptr;
    }
    auto result = make_shared<ImageVulkan>(this, vkImg, vmaAlloc, vmaAllocInfo);
    result->_mdesc = desc;
    result->_rawInfo = imgInfo;
    return result;
}

Nullable<shared_ptr<TextureView>> DeviceVulkan::CreateTextureView(const TextureViewDescriptor& desc) noexcept {
    auto image = CastVkObject(desc.Target);
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.image = image->_image;
    createInfo.viewType = MapType(desc.Dim);
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
        desc.Range.BaseArrayLayer,
        desc.Range.ArrayLayerCount == SubresourceRange::All ? VK_REMAINING_ARRAY_LAYERS : desc.Range.ArrayLayerCount};
    VkImageView imageView = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateImageView(_device, &createInfo, this->GetAllocationCallbacks(), &imageView);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateImageView failed: {}", vr);
        return nullptr;
    }
    auto result = make_shared<ImageViewVulkan>(this, image, imageView);
    result->_mdesc = desc;
    result->_rawFormat = createInfo.format;
    return result;
}

Nullable<shared_ptr<Shader>> DeviceVulkan::CreateShader(const ShaderDescriptor& desc) noexcept {
    static_assert(sizeof(uint32_t) == (sizeof(byte) * 4), "byte size mismatch");
    if (desc.Category != ShaderBlobCategory::SPIRV) {
        RADRAY_ERR_LOG("vk only support SPIR-V");
        return nullptr;
    }
    if (desc.Source.size() % 4 != 0) {
        RADRAY_ERR_LOG("vk SPIR-V code byte size must be a multiple of 4");
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
        RADRAY_ERR_LOG("vk call vkCreateShaderModule failed: {}", vr);
        return nullptr;
    }
    return make_shared<ShaderModuleVulkan>(this, shaderModule);
}

Nullable<shared_ptr<RootSignature>> DeviceVulkan::CreateRootSignature(const RootSignatureDescriptor& desc) noexcept {
    vector<VkDescriptorSetLayoutBinding> bindings;
    unique_ptr<DescriptorSetLayoutVulkan> rootSetLayout;
    if (!desc.RootBindings.empty()) {
        for (const auto& i : desc.RootBindings) {
            auto& binding = bindings.emplace_back();
            binding.binding = i.Slot;
            binding.descriptorType = MapType(i.Type);
            binding.descriptorCount = 1;
            binding.stageFlags = MapType(i.Stages);
            binding.pImmutableSamplers = nullptr;
        }
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.pNext = nullptr;
        dslci.flags = 0;
        dslci.bindingCount = static_cast<uint32_t>(bindings.size());
        dslci.pBindings = bindings.empty() ? nullptr : bindings.data();
        auto descSetLayoutVk = this->CreateDescriptorSetLayout(dslci);
        if (!descSetLayoutVk.HasValue()) {
            return nullptr;
        }
        auto descSetLayoutV = descSetLayoutVk.Release();
        descSetLayoutV->_rootBindings = {desc.RootBindings.begin(), desc.RootBindings.end()};
        rootSetLayout = std::move(descSetLayoutV);
    }
    unique_ptr<DescriptorSetVulkan> rootSet;
    if (rootSetLayout != nullptr) {
        rootSet = _poolAlloc->Allocate(rootSetLayout->_layout);
    }
    VkPushConstantRange pushConstant{};
    VkPushConstantRange* pushConstantPtr = nullptr;
    if (desc.Constant.has_value()) {
        pushConstant.stageFlags = MapType(desc.Constant->Stages);
        pushConstant.offset = 0;
        pushConstant.size = desc.Constant->Size;
        pushConstantPtr = &pushConstant;
    }
    vector<VkDescriptorSetLayout> vkDescSetLayouts;
    vkDescSetLayouts.reserve((rootSetLayout == nullptr ? 0 : 1) + desc.BindingSets.size());
    uint32_t rootSetStart = static_cast<uint32_t>(vkDescSetLayouts.size());
    if (rootSetLayout != nullptr) {
        vkDescSetLayouts.emplace_back(rootSetLayout->_layout);
    }
    uint32_t descSetsStart = static_cast<uint32_t>(vkDescSetLayouts.size());
    for (auto i : desc.BindingSets) {
        auto layout = CastVkObject(i);
        vkDescSetLayouts.emplace_back(layout->_layout);
    }
    VkPipelineLayoutCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.setLayoutCount = static_cast<uint32_t>(vkDescSetLayouts.size());
    createInfo.pSetLayouts = vkDescSetLayouts.empty() ? nullptr : vkDescSetLayouts.data();
    createInfo.pushConstantRangeCount = pushConstantPtr == nullptr ? 0 : 1;
    createInfo.pPushConstantRanges = pushConstantPtr;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreatePipelineLayout(_device, &createInfo, this->GetAllocationCallbacks(), &pipelineLayout);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreatePipelineLayout failed: {}", vr);
        return nullptr;
    }
    auto result = make_shared<PipelineLayoutVulkan>(this, pipelineLayout);
    result->_rootSetLayout = std::move(rootSetLayout);
    result->_rootSet = std::move(rootSet);
    result->_sets.reserve(desc.BindingSets.size());
    for (auto i : desc.BindingSets) {
        auto layout = CastVkObject(i);
        result->_sets.emplace_back(layout);
    }
    if (pushConstantPtr == nullptr) {
        result->_pushConst = std::nullopt;
    } else {
        result->_pushConst = *pushConstantPtr;
    }
    result->_rootSetStart = rootSetStart;
    result->_setsStart = descSetsStart;
    return result;
}

Nullable<shared_ptr<GraphicsPipelineState>> DeviceVulkan::CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept {
    vector<VkPipelineShaderStageCreateInfo> shaderStages;
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
        for (const auto& i : ss) {
            if (!i.shader.has_value()) continue;
            const auto& src = i.shader.value();
            auto shaderVulkan = CastVkObject(src.Target);
            auto& stage = shaderStages.emplace_back();
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pNext = nullptr;
            stage.flags = 0;
            stage.stage = i.stage;
            stage.module = shaderVulkan->_shaderModule;
            stage.pName = src.EntryPoint.data();
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
            rasterInfo.depthBiasConstantFactor = ds.DepthBias.Constant;
            rasterInfo.depthBiasClamp = ds.DepthBias.Clamp;
            rasterInfo.depthBiasSlopeFactor = ds.DepthBias.SlopScale;
        }
        depthStencilInfo.depthTestEnable = VK_TRUE;
        depthStencilInfo.depthWriteEnable = ds.DepthWriteEnable ? VK_TRUE : VK_FALSE;
        depthStencilInfo.depthCompareOp = MapType(ds.DepthCompare);
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
    unique_ptr<RenderPassVulkan> renderPass;
    {
        vector<VkAttachmentDescription> attachs;
        vector<VkAttachmentReference> colorRefs;
        VkAttachmentReference depthRef{};
        for (const auto& i : desc.ColorTargets) {
            auto& attach = attachs.emplace_back();
            attach.flags = 0;
            attach.format = MapType(i.Format);
            attach.samples = MapSampleCount(desc.MultiSample.Count);
            attach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            attach.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            auto& colorRef = colorRefs.emplace_back();
            colorRef.attachment = static_cast<uint32_t>(attachs.size() - 1);
            colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        if (desc.DepthStencil.has_value()) {
            const auto& ds = desc.DepthStencil.value();
            auto& attach = attachs.emplace_back();
            attach.flags = 0;
            attach.format = MapType(ds.Format);
            attach.samples = MapSampleCount(desc.MultiSample.Count);
            attach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (ds.Stencil.has_value()) {
                attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            } else {
                attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            }
            depthRef.attachment = static_cast<uint32_t>(attachs.size() - 1);
            if (desc.DepthStencil->Stencil.has_value()) {
                attach.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                attach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            } else {
                attach.initialLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                attach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            }
        }
        VkSubpassDescription subpassDesc{};
        subpassDesc.flags = 0;
        subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDesc.inputAttachmentCount = 0;
        subpassDesc.pInputAttachments = nullptr;
        subpassDesc.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
        subpassDesc.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
        subpassDesc.pResolveAttachments = nullptr;
        subpassDesc.pDepthStencilAttachment = desc.DepthStencil.has_value() ? &depthRef : nullptr;
        subpassDesc.preserveAttachmentCount = 0;
        subpassDesc.pPreserveAttachments = nullptr;
        VkRenderPassCreateInfo passInfo{};
        passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        passInfo.pNext = nullptr;
        passInfo.flags = 0;
        passInfo.attachmentCount = static_cast<uint32_t>(attachs.size());
        passInfo.pAttachments = attachs.empty() ? nullptr : attachs.data();
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpassDesc;
        passInfo.dependencyCount = 0;
        passInfo.pDependencies = nullptr;
        auto renderPassOpt = this->CreateRenderPass(passInfo);
        if (!renderPassOpt.HasValue()) {
            return nullptr;
        }
        renderPass = renderPassOpt.Release();
    }
    auto rs = CastVkObject(desc.RootSig);
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
    if (auto vr = _ftb.vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &createInfo, this->GetAllocationCallbacks(), &pipeline);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateGraphicsPipelines failed: {}", vr);
        return nullptr;
    }
    auto result = make_shared<GraphicsPipelineVulkan>(this, pipeline);
    result->_renderPass = std::move(renderPass);
    return result;
}

Nullable<shared_ptr<DescriptorSetLayout>> DeviceVulkan::CreateDescriptorSetLayout(const RootSignatureBindingSet& desc) noexcept {
    vector<VkDescriptorSetLayoutBinding> bindings;
    vector<shared_ptr<SamplerVulkan>> staticSamplers;
    vector<vector<VkSampler>> tmpSS;
    for (const auto& j : desc.Elements) {
        auto& binding = bindings.emplace_back();
        binding.binding = j.Slot;
        binding.descriptorType = MapType(j.Type);
        binding.descriptorCount = j.Count;
        binding.stageFlags = MapType(j.Stages);
        if (j.StaticSamplers.empty()) {
            binding.pImmutableSamplers = nullptr;
        } else {
            if (j.StaticSamplers.size() != j.Count) {
                RADRAY_ERR_LOG("vk static sampler count mismatch {} need {}", j.StaticSamplers.size(), j.Count);
                return nullptr;
            }
            vector<VkSampler> sss;
            sss.reserve(j.StaticSamplers.size());
            for (size_t t = 0; t < j.StaticSamplers.size(); t++) {
                const SamplerDescriptor& ss = j.StaticSamplers[t];
                auto samplerOpt = this->CreateSampler(ss);
                if (!samplerOpt.HasValue()) {
                    return nullptr;
                }
                auto sampler = std::static_pointer_cast<SamplerVulkan>(samplerOpt.Release());
                sss.emplace_back(sampler->_sampler);
                staticSamplers.emplace_back(std::move(sampler));
            }
            const auto& tsss = tmpSS.emplace_back(std::move(sss));
            binding.pImmutableSamplers = tsss.data();
        }
    }
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.pNext = nullptr;
    dslci.flags = 0;
    dslci.bindingCount = static_cast<uint32_t>(bindings.size());
    dslci.pBindings = bindings.empty() ? nullptr : bindings.data();
    auto descSetLayoutVk = this->CreateDescriptorSetLayout(dslci);
    if (!descSetLayoutVk.HasValue()) {
        return nullptr;
    }
    auto result = descSetLayoutVk.Release();
    result->_bindingElements = {desc.Elements.begin(), desc.Elements.end()};
    staticSamplers.shrink_to_fit();
    result->_immutableSamplers = std::move(staticSamplers);
    return shared_ptr{std::move(result)};
}

Nullable<shared_ptr<DescriptorSet>> DeviceVulkan::CreateDescriptorSet(DescriptorSetLayout* layout_) noexcept {
    auto layout = CastVkObject(layout_);
    auto descSet = _poolAlloc->Allocate(layout->_layout);
    auto result = std::make_shared<DescriptorSetVulkanWrapper>(this, std::move(descSet));
    result->_bindingElements = layout->_bindingElements;
    return result;
}

Nullable<shared_ptr<Sampler>> DeviceVulkan::CreateSampler(const SamplerDescriptor& desc) noexcept {
    VkSamplerCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.magFilter = MapTypeFilter(desc.MagFilter);
    createInfo.minFilter = MapTypeFilter(desc.MigFilter);
    createInfo.mipmapMode = MapTypeMipmapMode(desc.MipmapFilter);
    createInfo.addressModeU = MapType(desc.AddressS);
    createInfo.addressModeV = MapType(desc.AddressT);
    createInfo.addressModeW = MapType(desc.AddressR);
    createInfo.mipLodBias = 0;
    if (desc.AnisotropyClamp > 1.0f) {
        createInfo.anisotropyEnable = VK_TRUE;
        createInfo.maxAnisotropy = desc.AnisotropyClamp;
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
        RADRAY_ERR_LOG("vk call vkCreateSampler failed: {}", vr);
        return nullptr;
    }
    auto result = make_shared<SamplerVulkan>(this, sampler);
    result->_mdesc = desc;
    return result;
}

Nullable<unique_ptr<FenceVulkan>> DeviceVulkan::CreateLegacyFence(VkFenceCreateFlags flags) noexcept {
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = nullptr;
    fenceInfo.flags = flags;
    VkFence fence = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateFence(_device, &fenceInfo, this->GetAllocationCallbacks(), &fence);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk create fence failed: {}", vr);
        return nullptr;
    }
    auto v = make_unique<FenceVulkan>(this, fence);
    return v;
}

Nullable<unique_ptr<SemaphoreVulkan>> DeviceVulkan::CreateLegacySemaphore(VkSemaphoreCreateFlags flags) noexcept {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = nullptr;
    info.flags = flags;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateSemaphore(_device, &info, this->GetAllocationCallbacks(), &semaphore);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateSemaphore failed {}", vr);
        return nullptr;
    }
    return make_unique<SemaphoreVulkan>(this, semaphore);
}

Nullable<unique_ptr<TimelineSemaphoreVulkan>> DeviceVulkan::CreateTimelineSemaphore(uint64_t initValue) noexcept {
    if (!_extFeatures.feature12.timelineSemaphore) {
        RADRAY_ERR_LOG("vk timeline semaphore not supported");
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
        RADRAY_ERR_LOG("vk call vkCreateSemaphore failed {}", vr);
        return nullptr;
    }
    return make_unique<TimelineSemaphoreVulkan>(this, semaphore);
}

Nullable<unique_ptr<BufferViewVulkan>> DeviceVulkan::CreateBufferView(const VkBufferViewCreateInfo& info) noexcept {
    VkBufferView bufferView = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateBufferView(_device, &info, this->GetAllocationCallbacks(), &bufferView);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateBufferView failed: {}", vr);
        return nullptr;
    }
    return make_unique<BufferViewVulkan>(this, bufferView);
}

Nullable<unique_ptr<DescriptorSetLayoutVulkan>> DeviceVulkan::CreateDescriptorSetLayout(const VkDescriptorSetLayoutCreateInfo& info) noexcept {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateDescriptorSetLayout(_device, &info, this->GetAllocationCallbacks(), &layout);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateDescriptorSetLayout failed: {}", vr);
        return nullptr;
    }
    return make_unique<DescriptorSetLayoutVulkan>(this, layout);
}

Nullable<unique_ptr<RenderPassVulkan>> DeviceVulkan::CreateRenderPass(const VkRenderPassCreateInfo& info) noexcept {
    VkRenderPass pass;
    if (auto vr = _ftb.vkCreateRenderPass(_device, &info, this->GetAllocationCallbacks(), &pass);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateRenderPass failed: {}", vr);
        return nullptr;
    }
    return make_unique<RenderPassVulkan>(this, pass);
}

Nullable<unique_ptr<DescriptorPoolVulkan>> DeviceVulkan::CreateDescriptorPool() noexcept {
    vector<VkDescriptorPoolSize> poolSizes{};
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1024});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024});
    poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1024});
    VkDescriptorPoolInlineUniformBlockCreateInfo inlineInfo{};
    VkDescriptorPoolInlineUniformBlockCreateInfo* pInlineInfo = nullptr;
    if (_extFeatures.feature13.inlineUniformBlock) {
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT, 1024});
        pInlineInfo = &inlineInfo;
        inlineInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO;
        inlineInfo.pNext = nullptr;
        inlineInfo.maxInlineUniformBlockBindings = 1024;
    }
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.pNext = nullptr;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = 1024;
    info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    info.pPoolSizes = poolSizes.data();
    if (pInlineInfo != nullptr) {
        info.pNext = pInlineInfo;
    }
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateDescriptorPool(_device, &info, this->GetAllocationCallbacks(), &pool);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateDescriptorPool failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<DescriptorPoolVulkan>(this, pool);
    result->_sizes = std::move(poolSizes);
    result->_maxSets = info.maxSets;
    return result;
}

const VkAllocationCallbacks* DeviceVulkan::GetAllocationCallbacks() const noexcept {
    return _instance->GetAllocationCallbacks();
}

void DeviceVulkan::DestroyImpl() noexcept {
    _poolAlloc.reset();
    _vma.reset();
    for (auto&& i : _queues) {
        i.clear();
    }
    if (_device != VK_NULL_HANDLE) {
        _ftb.vkDestroyDevice(_device, this->GetAllocationCallbacks());
        _device = VK_NULL_HANDLE;
    }
    _physicalDevice = VK_NULL_HANDLE;
    _instance = nullptr;
}

Nullable<unique_ptr<InstanceVulkanImpl>> CreateVulkanInstanceImpl(const VulkanInstanceDescriptor& desc) {
    if (g_vkInstance.HasValue()) {
        RADRAY_ERR_LOG("vk has active VkInstance");
        return nullptr;
    }
    if (volkInitialize() != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk volk init fail");
        return nullptr;
    }
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceVersion failed");
        return nullptr;
    }
    RADRAY_INFO_LOG("vk instance version: {}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    vector<VkExtensionProperties> extProps;
    if (EnumerateVectorFromVkFunc(extProps, vkEnumerateInstanceExtensionProperties, nullptr) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceExtensionProperties failed");
        return nullptr;
    }
    vector<VkLayerProperties> layerProps;
    if (EnumerateVectorFromVkFunc(layerProps, vkEnumerateInstanceLayerProperties) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateInstanceLayerProperties failed");
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
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    needExts.emplace(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
    if (desc.IsEnableDebugLayer) {
        const auto requireExt = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        const char* requireExts[] = {requireExt};
        if (IsValidateExtensions(requireExts, extProps)) {
            needExts.emplace(requireExt);
        } else {
            RADRAY_WARN_LOG("vk extension {} is not supported", requireExt);
        }
        const auto validName = "VK_LAYER_KHRONOS_validation";
        const char* requireLayer[] = {validName};
        if (IsValidateLayers(requireLayer, layerProps)) {
            needLayers.emplace(validName);
            isValidFeatureExtEnable = true;
        } else {
            RADRAY_WARN_LOG("vk layer {} is not supported", validName);
        }
    }
    for (const auto& i : needLayers) {
        const char* require[] = {i.c_str()};
        if (!IsValidateLayers(require, layerProps)) {
            RADRAY_ERR_LOG("vk layer {} is not supported", i);
            return nullptr;
        }
    }
    for (const auto& i : needExts) {
        const char* require[] = {i.c_str()};
        if (!IsValidateExtensions(require, extProps)) {
            RADRAY_ERR_LOG("vk extension {} is not supported", i);
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
        validEnables.emplace_back(VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT);
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
    appInfo.pApplicationName = desc.AppName.data();
    appInfo.applicationVersion = desc.AppVersion;
    appInfo.pEngineName = desc.EngineName.data();
    appInfo.engineVersion = desc.EngineVersion;
    appInfo.apiVersion = version;
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
    uint32_t apiVersionsToTry[] = {
        VK_API_VERSION_1_3,
        VK_API_VERSION_1_2,
        VK_API_VERSION_1_1,
        VK_API_VERSION_1_0};
    VkInstance instance = VK_NULL_HANDLE;
    for (uint32_t apiVersion : apiVersionsToTry) {
        appInfo.apiVersion = apiVersion;
        if (auto vr = vkCreateInstance(&createInfo, allocCbPtr, &instance);
            vr == VK_SUCCESS) {
            break;
        }
    }
    if (instance == VK_NULL_HANDLE) {
        RADRAY_ERR_LOG("vk call vkCreateInstance failed");
        return nullptr;
    }
    volkLoadInstanceOnly(instance);
    auto result = make_unique<InstanceVulkanImpl>(
        instance,
        allocCbPtr ? std::make_optional(*allocCbPtr) : std::nullopt,
        vector<string>{needExts.begin(), needExts.end()},
        vector<string>{needLayers.begin(), needLayers.end()});
    g_vkInstance = result.get();
    return result;
}

void DestroyVulkanInstanceImpl(unique_ptr<InstanceVulkan> instance) noexcept {
    g_vkInstance = nullptr;
    instance.reset();
    volkFinalize();
}

Nullable<shared_ptr<DeviceVulkan>> CreateDeviceVulkan(const VulkanDeviceDescriptor& desc) {
    struct PhyDeviceInfo {
        VkPhysicalDevice device;
        VkPhysicalDeviceProperties properties;
        VkPhysicalDeviceMemoryProperties memory;
        size_t index;
    };

    if (!g_vkInstance.HasValue()) {
        RADRAY_ERR_LOG("vk not init");
        return nullptr;
    }
    VkInstance instance = g_vkInstance->_instance;
    vector<VkPhysicalDevice> physicalDevices;
    if (EnumerateVectorFromVkFunc(physicalDevices, vkEnumeratePhysicalDevices, instance) != VK_SUCCESS) {
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
                auto typeScore = [](VkPhysicalDeviceType t) noexcept {
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

    auto extProperties = make_unique<ExtPropertiesVulkan>();
    unordered_set<string> needExts;
    needExts.emplace(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    needExts.emplace(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    vector<VkExtensionProperties> deviceExtsAvailable;
    if (auto vr = EnumerateVectorFromVkFunc(deviceExtsAvailable, vkEnumerateDeviceExtensionProperties, selectPhyDevice.device, nullptr);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkEnumerateDeviceExtensionProperties failed");
        return nullptr;
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
    for (const auto& ext : needExts) {
        const char* exts[] = {ext.c_str()};
        if (!IsValidateExtensions(exts, deviceExtsAvailable)) {
            RADRAY_ERR_LOG("vk device extension {} not supported", ext);
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

        vkGetPhysicalDeviceFeatures2(selectPhyDevice.device, &deviceFeatures2);
        deviceInfo.pNext = &deviceFeatures2;
        deviceInfo.pEnabledFeatures = nullptr;
    }

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(selectPhyDevice.device, &deviceInfo, g_vkInstance->GetAllocationCallbacks(), &device) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk create device fail");
        return nullptr;
    }
    auto deviceR = make_shared<DeviceVulkan>(
        g_vkInstance.Ptr,
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
        RADRAY_ERR_LOG("vk call vmaImportVulkanFunctionsFromVolk failed: {}", vr);
        return nullptr;
    }
    VmaAllocator vma;
    if (auto vr = vmaCreateAllocator(&vmaCreateInfo, &vma);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vmaCreateAllocator failed: {}", vr);
        return nullptr;
    }
    deviceR->_vma = make_unique<VMA>(vma);
    for (const auto& i : queueRequests) {
        for (const auto& j : i.queueIndices) {
            VkQueue queuePtr = VK_NULL_HANDLE;
            deviceR->_ftb.vkGetDeviceQueue(deviceR->_device, j.Family, j.IndexInFamily, &queuePtr);
            if (queuePtr == VK_NULL_HANDLE) {
                RADRAY_ERR_LOG("vk get queue for family {} index {} failed", j.Family, j.IndexInFamily);
                return nullptr;
            }
            auto queue = make_unique<QueueVulkan>(deviceR.get(), queuePtr, j, i.rawType);
            deviceR->_queues[(size_t)i.rawType].emplace_back(std::move(queue));
        }
    }
    deviceR->_poolAlloc = make_unique<DescPoolAllocator>(deviceR.get());
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
        detail.CBufferAlignment = (uint32_t)deviceR->_properties.limits.minUniformBufferOffsetAlignment;
        detail.UploadTextureAlignment = (uint32_t)deviceR->_properties.limits.optimalBufferCopyOffsetAlignment;
        detail.UploadTextureRowAlignment = (uint32_t)deviceR->_properties.limits.optimalBufferCopyRowPitchAlignment;
        detail.MapAlignment = deviceR->_properties.limits.nonCoherentAtomSize;
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
    {
        RADRAY_INFO_LOG("Timeline Semaphore: {}", deviceR->_extFeatures.feature12.timelineSemaphore ? true : false);
    }
    {
        RADRAY_INFO_LOG("Conservative Rasterization: {}", deviceR->_extProperties.conservativeRasterization.has_value() ? true : false);
    }
    RADRAY_INFO_LOG("=============================");
    return deviceR;
}

QueueVulkan::QueueVulkan(
    DeviceVulkan* device,
    VkQueue queue,
    QueueIndexInFamily family,
    QueueType type) noexcept
    : _device(device),
      _queue(queue),
      _family(family),
      _type(type) {}

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
    if (desc.CmdBuffers.size() >= std::numeric_limits<uint32_t>::max()) [[unlikely]] {
        RADRAY_ABORT("vk cmd buffers count: {}, max: {}", desc.CmdBuffers.size(), std::numeric_limits<uint32_t>::max());
        return;
    }
    if (desc.WaitFences.size() >= std::numeric_limits<uint32_t>::max()) [[unlikely]] {
        RADRAY_ABORT("vk wait fences count: {}, max: {}", desc.WaitFences.size(), std::numeric_limits<uint32_t>::max());
        return;
    }
    if (desc.SignalFences.size() >= std::numeric_limits<uint32_t>::max()) [[unlikely]] {
        RADRAY_ABORT("vk signal fences count: {}, max: {}", desc.SignalFences.size(), std::numeric_limits<uint32_t>::max());
        return;
    }
    if (desc.WaitFences.size() != desc.WaitFenceValues.size()) [[unlikely]] {
        RADRAY_ABORT("vk wait fences count: {}, wait values count: {}", desc.WaitFences.size(), desc.WaitFenceValues.size());
        return;
    }
    if (desc.SignalFences.size() != desc.SignalFenceValues.size()) [[unlikely]] {
        RADRAY_ABORT("vk signal fences count: {}, signal values count: {}", desc.SignalFences.size(), desc.SignalFenceValues.size());
        return;
    }
    vector<VkCommandBuffer> cmdBufs;
    cmdBufs.reserve(desc.CmdBuffers.size());
    for (auto i : desc.CmdBuffers) {
        auto cmdBuffer = CastVkObject(i);
        cmdBufs.emplace_back(cmdBuffer->_cmdBuffer);
    }
    vector<VkSemaphore> waitSemaphores;
    vector<uint64_t> waitValues;
    vector<VkPipelineStageFlags> waitStages;
    size_t waitCount = desc.WaitFences.size();
    size_t waitCapacity = desc.WaitFences.size() + (_swapchainSync.imageAvailableSemaphore ? 1 : 0);
    waitSemaphores.reserve(waitCapacity);
    waitValues.reserve(waitCapacity);
    waitStages.reserve(waitCapacity);
    if (_swapchainSync.imageAvailableSemaphore) {
        waitSemaphores.emplace_back(_swapchainSync.imageAvailableSemaphore->_semaphore);
        waitValues.emplace_back(0);
        waitStages.emplace_back(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        _swapchainSync.imageAvailableSemaphore = nullptr;
    }
    for (size_t i = 0; i < waitCount; i++) {
        auto timelineSemaphore = CastVkObject(desc.WaitFences[i]);
        auto fenceValue = desc.WaitFenceValues[i];
        waitSemaphores.emplace_back(timelineSemaphore->_semaphore);
        waitValues.emplace_back(fenceValue);
        waitStages.emplace_back(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vector<VkSemaphore> signalSemaphores;
    vector<uint64_t> signalValues;
    size_t signalCount = desc.SignalFences.size();
    size_t signalCapacity = desc.SignalFences.size() + (_swapchainSync.renderFinishedSemaphore ? 1 : 0);
    signalSemaphores.reserve(signalCapacity);
    signalValues.reserve(signalCapacity);
    if (_swapchainSync.renderFinishedSemaphore) {
        signalSemaphores.emplace_back(_swapchainSync.renderFinishedSemaphore->_semaphore);
        signalValues.emplace_back(0);
        _swapchainSync.renderFinishedSemaphore = nullptr;
    }
    for (size_t i = 0; i < signalCount; i++) {
        auto timelineSemaphore = CastVkObject(desc.SignalFences[i]);
        auto fenceValue = desc.SignalFenceValues[i];
        signalSemaphores.emplace_back(timelineSemaphore->_semaphore);
        signalValues.emplace_back(fenceValue);
    }
    VkTimelineSemaphoreSubmitInfo timelineSubmitInfo{};
    timelineSubmitInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineSubmitInfo.pNext = nullptr;
    timelineSubmitInfo.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
    timelineSubmitInfo.pWaitSemaphoreValues = waitValues.empty() ? nullptr : waitValues.data();
    timelineSubmitInfo.signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size());
    timelineSubmitInfo.pSignalSemaphoreValues = signalValues.empty() ? nullptr : signalValues.data();
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = &timelineSubmitInfo;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    submitInfo.commandBufferCount = static_cast<uint32_t>(cmdBufs.size());
    submitInfo.pCommandBuffers = cmdBufs.empty() ? nullptr : cmdBufs.data();
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
    submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();
    VkFence signalFence = VK_NULL_HANDLE;
    if (_swapchainSync.fence) {
        signalFence = _swapchainSync.fence->_fence;
        _swapchainSync.fence = nullptr;
    }
    if (auto vr = _device->_ftb.vkQueueSubmit(_queue, 1, &submitInfo, signalFence);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vk call vkQueueSubmit failed: {}", vr);
        return;
    }
}

void QueueVulkan::Wait() noexcept {
    if (auto vr = _device->_ftb.vkQueueWaitIdle(_queue);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vk call vkQueueWaitIdle failed: {}", vr);
    }
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
        RADRAY_ABORT("vk call vkResetCommandPool failed: {}", vr);
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

void CommandBufferVulkan::DestroyImpl() noexcept {
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
        RADRAY_ABORT("vk call vkBeginCommandBuffer failed: {}", vr);
    }
}

void CommandBufferVulkan::End() noexcept {
    if (auto vr = _device->_ftb.vkEndCommandBuffer(_cmdBuffer);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vk call vkEndCommandBuffer failed: {}", vr);
    }
}

void CommandBufferVulkan::ResourceBarrier(std::span<BarrierBufferDescriptor> buffers, std::span<BarrierTextureDescriptor> textures) noexcept {
    VkPipelineStageFlags srcStageMask = 0;
    VkPipelineStageFlags dstStageMask = 0;
    vector<VkBufferMemoryBarrier> bufferBarriers;
    bufferBarriers.reserve(buffers.size());
    for (const auto& i : buffers) {
        auto buf = CastVkObject(i.Target);
        auto& bufBarrier = bufferBarriers.emplace_back();
        bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bufBarrier.pNext = nullptr;
        bufBarrier.srcAccessMask = BufferUseToAccessFlags(i.Before);
        bufBarrier.dstAccessMask = BufferUseToAccessFlags(i.After);
        if (i.OtherQueue.HasValue()) {
            auto otherQ = CastVkObject(i.OtherQueue.Value());
            if (i.IsFromOrToOtherQueue) {
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
        bufBarrier.size = buf->_rawInfo.size;

        auto srcStage = BufferUseToPipelineStageFlags(i.Before);
        auto dstStage = BufferUseToPipelineStageFlags(i.After);
        srcStageMask |= srcStage;
        dstStageMask |= dstStage;
    }
    vector<VkImageMemoryBarrier> imageBarriers;
    imageBarriers.reserve(textures.size());
    for (const auto& i : textures) {
        auto tex = CastVkObject(i.Target);
        auto& imgBarrier = imageBarriers.emplace_back();
        imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imgBarrier.pNext = nullptr;
        imgBarrier.srcAccessMask = TextureUseToAccessFlags(i.Before);
        imgBarrier.dstAccessMask = TextureUseToAccessFlags(i.After);
        imgBarrier.oldLayout = TextureUseToLayout(i.Before);
        imgBarrier.newLayout = TextureUseToLayout(i.After);
        if (i.OtherQueue.HasValue()) {
            auto otherQ = CastVkObject(i.OtherQueue.Value());
            if (i.IsFromOrToOtherQueue) {
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
        imgBarrier.subresourceRange.aspectMask = ImageFormatToAspectFlags(tex->_rawInfo.format);
        imgBarrier.subresourceRange.baseMipLevel = i.IsSubresourceBarrier ? i.Range.BaseMipLevel : 0;
        imgBarrier.subresourceRange.levelCount = i.IsSubresourceBarrier ? i.Range.MipLevelCount : VK_REMAINING_MIP_LEVELS;
        imgBarrier.subresourceRange.baseArrayLayer = i.IsSubresourceBarrier ? i.Range.BaseArrayLayer : 0;
        imgBarrier.subresourceRange.layerCount = i.IsSubresourceBarrier ? i.Range.ArrayLayerCount : VK_REMAINING_ARRAY_LAYERS;

        auto srcStage = TextureUseToPipelineStageFlags(i.Before, true);
        auto dstStage = TextureUseToPipelineStageFlags(i.After, false);
        srcStageMask |= srcStage;
        dstStageMask |= dstStage;
    }
    _device->_ftb.vkCmdPipelineBarrier(
        _cmdBuffer,
        srcStageMask,
        dstStageMask,
        0,
        0, nullptr,
        static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.data(),
        static_cast<uint32_t>(imageBarriers.size()), imageBarriers.data());
}

Nullable<unique_ptr<CommandEncoder>> CommandBufferVulkan::BeginRenderPass(const RenderPassDescriptor& desc) noexcept {
    vector<VkAttachmentDescription> attachs;
    vector<VkAttachmentReference> colorRefs;
    vector<VkImageView> fbs;
    vector<VkClearValue> clearValues;
    VkAttachmentReference depthRef{};
    attachs.reserve(desc.ColorAttachments.size() + (desc.DepthStencilAttachment.has_value() ? 1 : 0));
    colorRefs.reserve(desc.ColorAttachments.size());
    fbs.reserve(desc.ColorAttachments.size() + (desc.DepthStencilAttachment.has_value() ? 1 : 0));
    clearValues.reserve(desc.ColorAttachments.size() + (desc.DepthStencilAttachment.has_value() ? 1 : 0));
    uint32_t width = std::numeric_limits<uint32_t>::max(), height = std::numeric_limits<uint32_t>::max();
    for (const auto& i : desc.ColorAttachments) {
        auto imageView = CastVkObject(i.Target);
        auto& attachDesc = attachs.emplace_back();
        attachDesc.flags = 0;
        attachDesc.format = imageView->_rawFormat;
        attachDesc.samples = imageView->_image->_rawInfo.samples;
        attachDesc.loadOp = MapType(i.Load);
        attachDesc.storeOp = MapType(i.Store);
        attachDesc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachDesc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        auto& colorRef = colorRefs.emplace_back();
        colorRef.attachment = static_cast<uint32_t>(attachs.size() - 1);
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        fbs.emplace_back(imageView->_imageView);
        auto& clear = clearValues.emplace_back();
        clear.color.float32[0] = i.ClearValue.R;
        clear.color.float32[1] = i.ClearValue.G;
        clear.color.float32[2] = i.ClearValue.B;
        clear.color.float32[3] = i.ClearValue.A;
        if (width == std::numeric_limits<uint32_t>::max()) {
            width = imageView->_image->_mdesc.Width;
        } else {
            if (width != imageView->_image->_mdesc.Width) {
                RADRAY_ERR_LOG("vk render pass color attachment width mismatch, expected: {}, got: {}", width, imageView->_image->_mdesc.Width);
                return nullptr;
            }
        }
        if (height == std::numeric_limits<uint32_t>::max()) {
            height = imageView->_image->_mdesc.Height;
        } else if (height != imageView->_image->_mdesc.Height) {
            RADRAY_ERR_LOG("vk render pass color attachment height mismatch, expected: {}, got: {}", height, imageView->_image->_mdesc.Height);
            return nullptr;
        }
    }
    if (desc.DepthStencilAttachment.has_value()) {
        auto& i = desc.DepthStencilAttachment.value();
        auto imageView = CastVkObject(i.Target);
        auto& attachDesc = attachs.emplace_back();
        attachDesc.flags = 0;
        attachDesc.format = imageView->_rawFormat;
        attachDesc.samples = imageView->_image->_rawInfo.samples;
        attachDesc.loadOp = MapType(i.DepthLoad);
        attachDesc.storeOp = MapType(i.DepthStore);
        attachDesc.stencilLoadOp = MapType(i.StencilLoad);
        attachDesc.stencilStoreOp = MapType(i.StencilStore);
        attachDesc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachDesc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRef = {
            static_cast<uint32_t>(attachs.size() - 1),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        fbs.emplace_back(imageView->_imageView);
        auto& clear = clearValues.emplace_back();
        clear.depthStencil.depth = i.ClearValue.Depth;
        clear.depthStencil.stencil = i.ClearValue.Stencil;
        if (width == std::numeric_limits<uint32_t>::max()) {
            width = imageView->_image->_mdesc.Width;
        } else {
            if (width != imageView->_image->_mdesc.Width) {
                RADRAY_ERR_LOG("vk render pass color attachment width mismatch, expected: {}, got: {}", width, imageView->_image->_mdesc.Width);
                return nullptr;
            }
        }
        if (height == std::numeric_limits<uint32_t>::max()) {
            height = imageView->_image->_mdesc.Height;
        } else if (height != imageView->_image->_mdesc.Height) {
            RADRAY_ERR_LOG("vk render pass color attachment height mismatch, expected: {}, got: {}", height, imageView->_image->_mdesc.Height);
            return nullptr;
        }
    }
    VkSubpassDescription subpassDesc{};
    subpassDesc.flags = 0;
    subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDesc.inputAttachmentCount = 0;
    subpassDesc.pInputAttachments = nullptr;
    subpassDesc.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
    subpassDesc.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
    subpassDesc.pResolveAttachments = nullptr;
    subpassDesc.pDepthStencilAttachment = desc.DepthStencilAttachment.has_value() ? &depthRef : nullptr;
    subpassDesc.preserveAttachmentCount = 0;
    subpassDesc.pPreserveAttachments = nullptr;
    VkRenderPassCreateInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    passInfo.pNext = nullptr;
    passInfo.flags = 0;
    passInfo.attachmentCount = static_cast<uint32_t>(attachs.size());
    passInfo.pAttachments = attachs.empty() ? nullptr : attachs.data();
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpassDesc;
    passInfo.dependencyCount = 0;
    passInfo.pDependencies = nullptr;
    auto passOpt = _device->CreateRenderPass(passInfo);
    if (!passOpt.HasValue()) {
        return nullptr;
    }
    auto passR = passOpt.Release();
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.pNext = nullptr;
    fbInfo.flags = 0;
    fbInfo.renderPass = passR->_renderPass;
    fbInfo.attachmentCount = static_cast<uint32_t>(fbs.size());
    fbInfo.pAttachments = fbs.empty() ? nullptr : fbs.data();
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    VkFramebuffer framebuffer;
    if (auto vr = _device->_ftb.vkCreateFramebuffer(_device->_device, &fbInfo, _device->GetAllocationCallbacks(), &framebuffer);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateFramebuffer failed: {}", vr);
        return nullptr;
    }
    auto fbR = make_unique<FrameBufferVulkan>(_device, framebuffer);
    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.pNext = nullptr;
    beginInfo.renderPass = passR->_renderPass;
    beginInfo.framebuffer = framebuffer;
    beginInfo.renderArea = {{0, 0}, {fbInfo.width, fbInfo.height}};
    beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    beginInfo.pClearValues = clearValues.size() == 0 ? nullptr : clearValues.data();
    _device->_ftb.vkCmdBeginRenderPass(_cmdBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    auto encoder = make_unique<SimulateCommandEncoderVulkan>(_device, this);
    encoder->_pass = std::move(passR);
    encoder->_framebuffer = std::move(fbR);
    return encoder;
}

void CommandBufferVulkan::EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept {
    _device->_ftb.vkCmdEndRenderPass(_cmdBuffer);
    _endedEncoders.emplace_back(std::move(encoder));
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
    const uint64_t width = std::max(1u, dst->_mdesc.Width >> dstRange.BaseMipLevel);
    const uint64_t height = std::max(1u, dst->_mdesc.Height >> dstRange.BaseMipLevel);
    const uint64_t depth = std::max(1u, dst->_mdesc.DepthOrArraySize >> dstRange.BaseMipLevel);
    VkBufferImageCopy copyInfo{};
    copyInfo.bufferOffset = srcOffset;
    copyInfo.bufferRowLength = 0;
    copyInfo.bufferImageHeight = 0;
    copyInfo.imageSubresource.aspectMask = ImageFormatToAspectFlags(dst->_rawInfo.format);
    copyInfo.imageSubresource.mipLevel = dstRange.BaseMipLevel;
    copyInfo.imageSubresource.baseArrayLayer = dstRange.BaseArrayLayer;
    copyInfo.imageSubresource.layerCount = dstRange.ArrayLayerCount;
    copyInfo.imageOffset = {0, 0, 0};
    copyInfo.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth)};
    _device->_ftb.vkCmdCopyBufferToImage(_cmdBuffer, src->_buffer, dst->_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyInfo);
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
    return _pass != nullptr && _framebuffer != nullptr;
}

void SimulateCommandEncoderVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void SimulateCommandEncoderVulkan::DestroyImpl() noexcept {
    _framebuffer.reset();
    _pass.reset();
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

void SimulateCommandEncoderVulkan::BindVertexBuffer(std::span<VertexBufferView> vbv) noexcept {
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
    _device->_ftb.vkCmdBindIndexBuffer(_cmdBuffer->_cmdBuffer, buffer->_buffer, ibv.Offset, indexType);
}

void SimulateCommandEncoderVulkan::BindRootSignature(RootSignature* rootSig) noexcept {
    auto layout = CastVkObject(rootSig);
    _boundPipeLayout = layout;
}

void SimulateCommandEncoderVulkan::BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept {
    auto p = CastVkObject(pso);
    _device->_ftb.vkCmdBindPipeline(_cmdBuffer->_cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, p->_pipeline);
}

void SimulateCommandEncoderVulkan::PushConstant(const void* data, size_t length) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("vk push constant without pipelinelayout");
        return;
    }
    if (!_boundPipeLayout->_pushConst.has_value()) {
        RADRAY_ERR_LOG("vk push constant but pipelinelayout has no push constant");
        return;
    }
    const auto& pc = _boundPipeLayout->_pushConst.value();
    if (length > pc.size) {
        RADRAY_ERR_LOG("vk push constant length {} exceeds pipelinelayout push constant size {}", length, pc.size);
        return;
    }
    _device->_ftb.vkCmdPushConstants(_cmdBuffer->_cmdBuffer, _boundPipeLayout->_layout, pc.stageFlags, 0, static_cast<uint32_t>(length), data);
}

void SimulateCommandEncoderVulkan::BindRootDescriptor(uint32_t slot, ResourceView* view) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("vk BindRootDescriptor without pipelinelayout");
        return;
    }
    if (!_boundPipeLayout->_rootSetLayout || !_boundPipeLayout->_rootSet) {
        RADRAY_ERR_LOG("vk BindRootDescriptor but pipelinelayout has no root descriptor");
        return;
    }
    DescriptorSetLayoutVulkan* rootSetLayout = _boundPipeLayout->_rootSetLayout.get();
    DescriptorSetVulkan* rootSet = _boundPipeLayout->_rootSet.get();
    if (slot >= rootSetLayout->_rootBindings.size()) {
        RADRAY_ERR_LOG("vk BindRootDescriptor slot {} exceeds pipelinelayout root descriptor count {}", slot, rootSetLayout->_rootBindings.size());
        return;
    }
    _device->_ftb.vkCmdBindDescriptorSets(
        _cmdBuffer->_cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _boundPipeLayout->_layout,
        _boundPipeLayout->_rootSetStart,
        1,
        &rootSet->_set,
        0, nullptr);
    const auto& rb = rootSetLayout->_rootBindings[slot];
    VkWriteDescriptorSet writeDesc{};
    writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDesc.pNext = nullptr;
    writeDesc.dstSet = rootSet->_set;
    writeDesc.dstBinding = rb.Slot;
    writeDesc.dstArrayElement = 0;
    writeDesc.descriptorCount = 1;
    writeDesc.descriptorType = MapType(rb.Type);
    auto tag = view->GetTag();
    VkDescriptorBufferInfo bufInfo{};
    if (tag == RenderObjectTag::BufferView) {
        auto bv = static_cast<SimulateBufferViewVulkan*>(view);
        bufInfo.buffer = bv->_buffer->_buffer;
        bufInfo.offset = bv->_range.Offset;
        bufInfo.range = bv->_range.Size;
        writeDesc.pBufferInfo = &bufInfo;
    } else if (tag == RenderObjectTag::TextureView) {
        // TODO: support image view as root descriptor
        RADRAY_ERR_LOG("vk cannot bind texture as root descriptor");
        return;
    } else {
        RADRAY_ERR_LOG("vk cannot BindRootDescriptor, unsupported tag {}", static_cast<RenderObjectTag>(tag.value()));
        return;
    }
    _device->_ftb.vkUpdateDescriptorSets(
        _device->_device,
        1,
        &writeDesc,
        0,
        nullptr);
}

void SimulateCommandEncoderVulkan::BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("vk BindDescriptorSet without pipelinelayout");
        return;
    }
    if (slot >= _boundPipeLayout->_sets.size()) {
        RADRAY_ERR_LOG("vk BindDescriptorSet slot {} exceeds pipelinelayout descriptor set count {}", slot, _boundPipeLayout->_sets.size());
        return;
    }
    auto descSet = CastVkObject(set);
    _device->_ftb.vkCmdBindDescriptorSets(
        _cmdBuffer->_cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _boundPipeLayout->_layout,
        _boundPipeLayout->_setsStart + slot,
        1,
        &descSet->_set->_set,
        0, nullptr);
}

void SimulateCommandEncoderVulkan::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
    _device->_ftb.vkCmdDraw(_cmdBuffer->_cmdBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void SimulateCommandEncoderVulkan::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
    _device->_ftb.vkCmdDrawIndexed(_cmdBuffer->_cmdBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

RenderPassVulkan::RenderPassVulkan(
    DeviceVulkan* device,
    VkRenderPass renderPass) noexcept
    : _device(device),
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

void RenderPassVulkan::DestroyImpl() noexcept {
    if (_renderPass != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyRenderPass(_device->_device, _renderPass, _device->GetAllocationCallbacks());
        _renderPass = VK_NULL_HANDLE;
    }
}

FrameBufferVulkan::FrameBufferVulkan(
    DeviceVulkan* device,
    VkFramebuffer framebuffer) noexcept
    : _device(device),
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

void FrameBufferVulkan::DestroyImpl() noexcept {
    if (_framebuffer != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyFramebuffer(_device->_device, _framebuffer, _device->GetAllocationCallbacks());
        _framebuffer = VK_NULL_HANDLE;
    }
}

FenceVulkan::FenceVulkan(
    DeviceVulkan* device,
    VkFence fence) noexcept
    : _device(device),
      _fence(fence) {}

FenceVulkan::~FenceVulkan() noexcept {
    this->DestroyImpl();
}

bool FenceVulkan::IsValid() const noexcept {
    return _fence != VK_NULL_HANDLE;
}

void FenceVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void FenceVulkan::DestroyImpl() noexcept {
    if (_fence != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyFence(_device->_device, _fence, _device->GetAllocationCallbacks());
        _fence = VK_NULL_HANDLE;
    }
}

SemaphoreVulkan::SemaphoreVulkan(
    DeviceVulkan* device,
    VkSemaphore semaphore) noexcept
    : _device(device),
      _semaphore(semaphore) {}

SemaphoreVulkan::~SemaphoreVulkan() noexcept {
    this->DestroyImpl();
}

bool SemaphoreVulkan::IsValid() const noexcept {
    return _semaphore != VK_NULL_HANDLE;
}

void SemaphoreVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void SemaphoreVulkan::DestroyImpl() noexcept {
    if (_semaphore != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroySemaphore(_device->_device, _semaphore, _device->GetAllocationCallbacks());
        _semaphore = VK_NULL_HANDLE;
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
        RADRAY_ABORT("vk call vkGetSemaphoreCounterValue failed: {}", vr);
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

SwapChainVulkan::SwapChainVulkan(
    DeviceVulkan* device,
    QueueVulkan* queue,
    unique_ptr<SurfaceVulkan> surface,
    VkSwapchainKHR swapchain) noexcept
    : _device(device),
      _queue(queue),
      _surface(std::move(surface)),
      _swapchain(swapchain) {}

SwapChainVulkan::~SwapChainVulkan() noexcept {
    this->DestroyImpl();
}

bool SwapChainVulkan::IsValid() const noexcept {
    return _surface != nullptr && _swapchain != VK_NULL_HANDLE;
}

void SwapChainVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void SwapChainVulkan::DestroyImpl() noexcept {
    for (auto& i : _frames) {
        i.image->DangerousDestroy();
    }
    _frames.clear();
    if (_swapchain != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroySwapchainKHR(_device->_device, _swapchain, _device->GetAllocationCallbacks());
        _swapchain = VK_NULL_HANDLE;
    }
    _surface.reset();
}

Nullable<Texture*> SwapChainVulkan::AcquireNext() noexcept {
    Frame& frameData = _frames[_currentFrameIndex];
    _device->_ftb.vkWaitForFences(_device->_device, 1, &frameData.fence->_fence, VK_TRUE, UINT64_MAX);
    _device->_ftb.vkResetFences(_device->_device, 1, &frameData.fence->_fence);
    _currentTextureIndex = std::numeric_limits<uint32_t>::max();
    // 窗口大小改变时可能返回 VK_ERROR_OUT_OF_DATE_KHR
    if (auto vr = _device->_ftb.vkAcquireNextImageKHR(
            _device->_device,
            _swapchain,
            UINT64_MAX,
            frameData.imageAvailableSemaphore->_semaphore,  // signal rt available
            VK_NULL_HANDLE,
            &_currentTextureIndex);
        vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
            RADRAY_DEBUG_LOG("vk call vkAcquireNextImageKHR return VK_ERROR_OUT_OF_DATE_KHR");
        } else {
            RADRAY_ERR_LOG("vk call vkAcquireNextImageKHR failed: {}", vr);
        }
        return nullptr;
    }
    _queue->_swapchainSync.fence = frameData.fence;
    _queue->_swapchainSync.imageAvailableSemaphore = frameData.imageAvailableSemaphore;
    _queue->_swapchainSync.renderFinishedSemaphore = frameData.renderFinishedSemaphore;
    return _frames[_currentTextureIndex].image.get();
}

void SwapChainVulkan::Present() noexcept {
    Frame& frameData = _frames[_currentFrameIndex];
    _currentFrameIndex = (_currentFrameIndex + 1) % _frames.size();
    // 如果直到 present, queue 还没有提交过命令
    // 在提交前需要将当前 back buffer 转换到 PRESENT 状态
    if (_queue->_swapchainSync.fence != nullptr) {
        if (frameData.internalCmdBuffer == nullptr) {
            frameData.internalCmdBuffer = std::static_pointer_cast<CommandBufferVulkan>(_device->CreateCommandBuffer(_queue).Unwrap());
        }
        auto cmdBuffer = frameData.internalCmdBuffer.get();
        cmdBuffer->Begin();
        VkImageMemoryBarrier imgBarrier{};
        imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imgBarrier.pNext = nullptr;
        imgBarrier.srcAccessMask = 0;
        imgBarrier.dstAccessMask = 0;
        imgBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarrier.image = frameData.image->_image;
        imgBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgBarrier.subresourceRange.baseMipLevel = 0;
        imgBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        imgBarrier.subresourceRange.baseArrayLayer = 0;
        imgBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        _device->_ftb.vkCmdPipelineBarrier(
            cmdBuffer->_cmdBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &imgBarrier);
        cmdBuffer->End();
        VkPipelineStageFlags waitStage{VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = nullptr;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &_queue->_swapchainSync.imageAvailableSemaphore->_semaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuffer->_cmdBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &_queue->_swapchainSync.renderFinishedSemaphore->_semaphore;         // signal cmd finish
        _device->_ftb.vkQueueSubmit(_queue->_queue, 1, &submitInfo, _queue->_swapchainSync.fence->_fence);  // signal host that cmd finish
        _queue->_swapchainSync.fence = nullptr;
        _queue->_swapchainSync.imageAvailableSemaphore = nullptr;
        _queue->_swapchainSync.renderFinishedSemaphore = nullptr;
    }
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frameData.renderFinishedSemaphore->_semaphore;  // wait cmd finish to present
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.pImageIndices = &_currentTextureIndex;
    presentInfo.pResults = nullptr;
    _device->_ftb.vkQueuePresentKHR(_queue->_queue, &presentInfo);
}

Nullable<Texture*> SwapChainVulkan::GetCurrentBackBuffer() const noexcept {
    if (_currentTextureIndex >= _frames.size()) {
        return nullptr;
    }
    return _frames[_currentTextureIndex].image.get();
}

uint32_t SwapChainVulkan::GetCurrentBackBufferIndex() const noexcept {
    return _currentTextureIndex;
}

uint32_t SwapChainVulkan::GetBackBufferCount() const noexcept {
    return static_cast<uint32_t>(_frames.size());
}

SwapChainDescriptor SwapChainVulkan::GetDesc() const noexcept {
    return _desc;
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
    RADRAY_UNUSED(size);
    void* mappedData = nullptr;
    if (_allocInfo.pMappedData) {
        mappedData = static_cast<byte*>(_allocInfo.pMappedData) + offset;
    } else {
        if (auto vr = vmaMapMemory(_device->_vma->_vma, _allocation, &mappedData);
            vr != VK_SUCCESS) {
            RADRAY_ABORT("vk call vmaMapMemory failed: {}", vr);
        }
        mappedData = static_cast<byte*>(mappedData) + offset;
    }
    return mappedData;
}

void BufferVulkan::Unmap(uint64_t offset, uint64_t size) noexcept {
    RADRAY_UNUSED(offset);
    RADRAY_UNUSED(size);
    if (_allocInfo.pMappedData && !_mdesc.Usage.HasFlag(BufferUse::MapWrite)) {
        vmaUnmapMemory(_device->_vma->_vma, _allocation);
    }
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

SimulateBufferViewVulkan::SimulateBufferViewVulkan(
    DeviceVulkan* device,
    BufferVulkan* buffer,
    BufferRange range) noexcept
    : _device(device),
      _buffer(buffer),
      _range(range) {}

SimulateBufferViewVulkan::~SimulateBufferViewVulkan() noexcept {
    this->DestroyImpl();
}

bool SimulateBufferViewVulkan::IsValid() const noexcept {
    return _buffer != nullptr;
}

void SimulateBufferViewVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void SimulateBufferViewVulkan::DestroyImpl() noexcept {
    _texelView.reset();
    _buffer = nullptr;
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

void ImageVulkan::DangerousDestroy() noexcept {
    _image = VK_NULL_HANDLE;
    _allocation = VK_NULL_HANDLE;
}

void ImageVulkan::DestroyImpl() noexcept {
    if (_image != VK_NULL_HANDLE) {
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

void ImageViewVulkan::DestroyImpl() noexcept {
    if (_imageView != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyImageView(_device->_device, _imageView, _device->GetAllocationCallbacks());
        _imageView = VK_NULL_HANDLE;
    }
}

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

void DescriptorSetLayoutVulkan::DestroyImpl() noexcept {
    if (_layout != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyDescriptorSetLayout(_device->_device, _layout, _device->GetAllocationCallbacks());
        _layout = VK_NULL_HANDLE;
    }
}

PipelineLayoutVulkan::PipelineLayoutVulkan(
    DeviceVulkan* device,
    VkPipelineLayout layout) noexcept
    : _device(device),
      _layout(layout) {}

PipelineLayoutVulkan::~PipelineLayoutVulkan() noexcept {
    this->DestroyImpl();
}

bool PipelineLayoutVulkan::IsValid() const noexcept {
    return _layout != VK_NULL_HANDLE;
}

void PipelineLayoutVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void PipelineLayoutVulkan::DestroyImpl() noexcept {
    if (_layout != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyPipelineLayout(_device->_device, _layout, _device->GetAllocationCallbacks());
        _layout = VK_NULL_HANDLE;
    }
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

void GraphicsPipelineVulkan::DestroyImpl() noexcept {
    if (_pipeline != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroyPipeline(_device->_device, _pipeline, _device->GetAllocationCallbacks());
        _pipeline = VK_NULL_HANDLE;
    }
}

ShaderModuleVulkan::ShaderModuleVulkan(
    DeviceVulkan* device,
    VkShaderModule shaderModule) noexcept
    : _device(device),
      _shaderModule(shaderModule) {}

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
}

DescriptorPoolVulkan::DescriptorPoolVulkan(
    DeviceVulkan* device,
    VkDescriptorPool pool) noexcept
    : _device(device),
      _pool(pool) {}

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
    DescriptorPoolVulkan* pool,
    VkDescriptorSet set) noexcept
    : _device(device),
      _pool(pool),
      _set(set) {}

DescriptorSetVulkan::~DescriptorSetVulkan() noexcept {
    this->DestroyImpl();
}

bool DescriptorSetVulkan::IsValid() const noexcept {
    return _set != VK_NULL_HANDLE;
}

void DescriptorSetVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void DescriptorSetVulkan::DestroyImpl() noexcept {
    if (_set != VK_NULL_HANDLE) {
        _device->_ftb.vkFreeDescriptorSets(_device->_device, _pool->_pool, 1, &_set);
        _set = VK_NULL_HANDLE;
    }
}

DescriptorSetVulkanWrapper::DescriptorSetVulkanWrapper(
    DeviceVulkan* device,
    unique_ptr<DescriptorSetVulkan> set) noexcept
    : _device(device),
      _set(std::move(set)) {}

bool DescriptorSetVulkanWrapper::IsValid() const noexcept {
    return _set != nullptr && _set->IsValid();
}

void DescriptorSetVulkanWrapper::Destroy() noexcept {
    _set.reset();
}

void DescriptorSetVulkanWrapper::SetResource(uint32_t index, ResourceView* view) noexcept {
    if (index >= _bindingElements.size()) {
        RADRAY_ERR_LOG("vk DescriptorSetVulkanWrapper::SetResource index out of range {} of {}", index, _bindingElements.size());
        return;
    }
    auto tag = view->GetTag();
    const auto& e = _bindingElements[index];
    VkWriteDescriptorSet writeDesc{};
    writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDesc.pNext = nullptr;
    writeDesc.dstSet = _set->_set;
    writeDesc.dstBinding = e.Slot;
    writeDesc.dstArrayElement = 0;
    writeDesc.descriptorCount = 1;
    writeDesc.descriptorType = MapType(e.Type);
    writeDesc.pBufferInfo = nullptr;
    writeDesc.pImageInfo = nullptr;
    writeDesc.pTexelBufferView = nullptr;
    VkDescriptorBufferInfo bufInfo{};
    VkDescriptorImageInfo imgInfo{};
    if (tag.HasFlag(RenderObjectTag::BufferView)) {
        auto bv = static_cast<SimulateBufferViewVulkan*>(view);
        if (bv->_texelView) {
            writeDesc.pTexelBufferView = &bv->_texelView->_bufferView;
        } else {
            bufInfo.buffer = bv->_buffer->_buffer;
            bufInfo.offset = bv->_range.Offset;
            bufInfo.range = bv->_range.Size;
            writeDesc.pBufferInfo = &bufInfo;
        }
    } else if (tag.HasFlag(RenderObjectTag::TextureView)) {
        auto tv = static_cast<ImageViewVulkan*>(view);
        imgInfo.imageView = tv->_imageView;
        imgInfo.imageLayout = TextureUseToLayout(tv->_mdesc.Usage);
    } else {
        RADRAY_ERR_LOG("vk DescriptorSetVulkanWrapper::SetResource unsupported tag {}", static_cast<RenderObjectTag>(tag.value()));
        return;
    }
    _device->_ftb.vkUpdateDescriptorSets(
        _device->_device,
        1,
        &writeDesc,
        0,
        nullptr);
}

DescPoolAllocator::DescPoolAllocator(DeviceVulkan* device)
    : _device(device) {}

vector<unique_ptr<DescriptorSetVulkan>> DescPoolAllocator::Allocate(std::span<VkDescriptorSetLayout> layouts) {
    auto mapToResult = [this](std::span<VkDescriptorSet> handle, DescriptorPoolVulkan* pool) {
        vector<unique_ptr<DescriptorSetVulkan>> result;
        result.reserve(handle.size());
        for (auto i : handle) {
            auto set = make_unique<DescriptorSetVulkan>(_device, pool, i);
            result.emplace_back(std::move(set));
        }
        return result;
    };
    auto tryAlloc = [this, layouts, &mapToResult](DescriptorPoolVulkan* pool) -> std::optional<vector<unique_ptr<DescriptorSetVulkan>>> {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.pNext = nullptr;
        allocInfo.descriptorPool = pool->_pool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocInfo.pSetLayouts = layouts.data();
        vector<VkDescriptorSet> sets;
        sets.resize(allocInfo.descriptorSetCount, VK_NULL_HANDLE);
        if (auto vr = _device->_ftb.vkAllocateDescriptorSets(_device->_device, &allocInfo, sets.data());
            vr == VK_SUCCESS) {
            return mapToResult(sets, pool);
        } else {
            return std::nullopt;
        }
    };
    {
        auto pool = GetMaybeEmptyPool();
        auto tryRes = tryAlloc(pool);
        if (tryRes.has_value()) {
            auto result = std::move(tryRes.value());
            return result;
        }
    }
    for (size_t i = 0; i < _pools.size() - 1; i++) {
        auto pool = _pools[i].get();
        auto tryRes = tryAlloc(pool);
        if (tryRes.has_value()) {
            auto result = std::move(tryRes.value());
            return result;
        }
    }
    {
        auto pool = NewPoolToBack();
        auto tryRes = tryAlloc(pool);
        if (!tryRes.has_value()) {
            RADRAY_ABORT("vk alloc desc set fail");
            return {};
        }
        auto result = std::move(tryRes.value());
        return result;
    }
}

unique_ptr<DescriptorSetVulkan> DescPoolAllocator::Allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetLayout t[] = {layout};
    auto r = this->Allocate(t);
    if (r.size() != 1) {
        RADRAY_ABORT("vk alloc desc set fail");
        return nullptr;
    }
    return std::move(r[0]);
}

DescriptorPoolVulkan* DescPoolAllocator::NewPoolToBack() {
    auto pool = _device->CreateDescriptorPool().Unwrap();
    return _pools.emplace_back(std::move(pool)).get();
}

DescriptorPoolVulkan* DescPoolAllocator::GetMaybeEmptyPool() {
    if (!_pools.empty()) {
        return _pools.back().get();
    }
    return NewPoolToBack();
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

void SamplerVulkan::DestroyImpl() noexcept {
    if (_sampler != VK_NULL_HANDLE) {
        _device->_ftb.vkDestroySampler(_device->_device, _sampler, _device->GetAllocationCallbacks());
        _sampler = VK_NULL_HANDLE;
    }
}

}  // namespace radray::render::vulkan
