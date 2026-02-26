#include <radray/render/backend/vulkan_impl.h>

#if RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

#include <algorithm>
#include <bit>
#include <cstring>

#if defined(VK_USE_PLATFORM_METAL_EXT)
namespace radray {
VkSurfaceKHR CreateMacOSMetalSurface(VkInstance instance, void* nsView, const VkAllocationCallbacks* allocator) noexcept;
}  // namespace radray
#endif

namespace radray::render::vulkan {

static Nullable<InstanceVulkanImpl*> g_vkInstance = nullptr;

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
            FormatVkDebugUtilsMessageTypeFlagsEXT(messageType),
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

Nullable<CommandQueue*> DeviceVulkan::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    return _queues[static_cast<std::underlying_type_t<QueueType>>(type)][slot].get();
}

Nullable<unique_ptr<CommandBuffer>> DeviceVulkan::CreateCommandBuffer(CommandQueue* queue_) noexcept {
    auto queue = CastVkObject(queue_);
    return CreateCommandBufferVulkan(queue);
}

Nullable<unique_ptr<Fence>> DeviceVulkan::CreateFence() noexcept {
    return this->CreateLegacyFence(0);
}

Nullable<unique_ptr<Semaphore>> DeviceVulkan::CreateSemaphoreDevice() noexcept {
    return this->CreateLegacySemaphore(0);
}

Nullable<unique_ptr<SwapChain>> DeviceVulkan::CreateSwapChain(const SwapChainDescriptor& desc_) noexcept {
    SwapChainDescriptor desc = desc_;
    unique_ptr<SurfaceVulkan> surface;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    {
        LPCWSTR instanceAddr = std::bit_cast<LPCWSTR>(&DestroyVulkanInstanceImpl);
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
    VkSurfaceCapabilitiesKHR surfaceProperties;
    if (auto vr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, surface->_surface, &surfaceProperties);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: {}", vr);
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
    VkExtent2D swapchainSize{};
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
        RADRAY_ERR_LOG("vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}", vr);
        return nullptr;
    }
    auto needFormatIter = std::ranges::find_if(supportedFormats, [rawFormat](VkSurfaceFormatKHR i) { return i.format == rawFormat; });
    if (needFormatIter == supportedFormats.end()) {
        RADRAY_ERR_LOG("vk surface format not supported", rawFormat);
        return nullptr;
    }
    const VkSurfaceFormatKHR& needFormat = *needFormatIter;
    vector<VkPresentModeKHR> supportedPresentModes;
    if (auto vr = EnumerateVectorFromVkFunc(supportedPresentModes, vkGetPhysicalDeviceSurfacePresentModesKHR, this->_physicalDevice, surface->_surface);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkGetPhysicalDeviceSurfacePresentModesKHR failed: {}", vr);
        return nullptr;
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
        RADRAY_ERR_LOG("vkCreateSwapchainKHR failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<SwapChainVulkan>(this, presentQueue, std::move(surface), swapchain);
    vector<VkImage> swapchainImages;
    if (auto vr = EnumerateVectorFromVkFunc(swapchainImages, _ftb.vkGetSwapchainImagesKHR, _device, swapchain);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkGetSwapchainImagesKHR failed: {}", vr);
        return nullptr;
    }
    result->_frames.reserve(swapchainImages.size());
    for (VkImage img : swapchainImages) {
        SwapChainVulkan::Frame& f = result->_frames.emplace_back();
        f.image = make_unique<ImageVulkan>(this, img, VK_NULL_HANDLE, VmaAllocationInfo{});
        string name = fmt::format("SwapChain Image {}", result->_frames.size() - 1);
        TextureDescriptor texDesc{
            TextureDimension::Dim2D,
            swapchianCreateInfo.imageExtent.width,
            swapchianCreateInfo.imageExtent.height,
            1,
            1,
            1,
            desc.Format,
            MemoryType::Device,
            TextureUse::RenderTarget,
            ResourceHint::None,
            name};
        f.image->SetExtData(texDesc);
    }
    return result;
}

Nullable<unique_ptr<Buffer>> DeviceVulkan::CreateBuffer(const BufferDescriptor& desc) noexcept {
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
    if (desc.Usage.HasFlag(BufferUse::AccelerationStructure) ||
        desc.Usage.HasFlag(BufferUse::Scratch) ||
        desc.Usage.HasFlag(BufferUse::ShaderTable) ||
        desc.Usage.HasFlag(BufferUse::Vertex) ||
        desc.Usage.HasFlag(BufferUse::Index) ||
        desc.Usage.HasFlag(BufferUse::Resource)) {
        bufInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
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
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    if (desc.Usage.HasFlag(BufferUse::MapRead)) {
        vmaInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
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
    this->SetObjectName(desc.Name, vkBuf);
    auto result = make_unique<BufferVulkan>(this, vkBuf, vmaAlloc, vmaAllocInfo);
    result->_reqSize = bufInfo.size;
    result->_name = desc.Name;
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints;
    return result;
}

Nullable<unique_ptr<BufferView>> DeviceVulkan::CreateBufferView(const BufferViewDescriptor& desc) noexcept {
    if (desc.Target == nullptr) {
        RADRAY_ERR_LOG("BufferViewDescriptor.Target is null");
        return nullptr;
    }
    auto buf = CastVkObject(desc.Target);
    if (!ValidateBufferViewDescriptor(desc, buf->GetDesc())) {
        return nullptr;
    }
    unique_ptr<BufferViewVulkan> texelView;
    if (desc.Usage == BufferViewUsage::TexelReadOnly || desc.Usage == BufferViewUsage::TexelReadWrite) {
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
    auto result = make_unique<SimulateBufferViewVulkan>(this, buf, desc.Range);
    result->_texelView = std::move(texelView);
    return result;
}

Nullable<unique_ptr<Texture>> DeviceVulkan::CreateTexture(const TextureDescriptor& desc) noexcept {
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
    this->SetObjectName(desc.Name, vkImg);
    auto result = make_unique<ImageVulkan>(this, vkImg, vmaAlloc, vmaAllocInfo);
    result->SetExtData(desc);
    return result;
}

Nullable<unique_ptr<TextureView>> DeviceVulkan::CreateTextureView(const TextureViewDescriptor& desc) noexcept {
    if (desc.Target == nullptr) {
        RADRAY_ERR_LOG("TextureViewDescriptor.Target is null");
        return nullptr;
    }
    auto image = CastVkObject(desc.Target);
    if (!ValidateTextureViewDescriptor(desc, image->_mdesc)) {
        return nullptr;
    }
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
        desc.Range.BaseArrayLayer,
        desc.Range.ArrayLayerCount == SubresourceRange::All ? VK_REMAINING_ARRAY_LAYERS : desc.Range.ArrayLayerCount};
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
    if (!ValidateAccelerationStructureViewDescriptor(desc)) {
        return nullptr;
    }
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
    return make_unique<ShaderModuleVulkan>(this, shaderModule);
}

Nullable<unique_ptr<RootSignature>> DeviceVulkan::CreateRootSignature(const RootSignatureDescriptor& desc) noexcept {
    if (!desc.RootDescriptors.empty()) {
        // vk 要模拟 dx D3D12_ROOT_PARAMETER_TYPE_CBV/SRV/UAV 的 root binding 有亿点麻烦, 也没那么简单, 先不做了 (欸嘿
        RADRAY_ERR_LOG("vk unsupported root descriptors in root signature");
        return nullptr;
    }
    vector<VkDescriptorSetLayoutBinding> bindings;
    VkPushConstantRange pushConstant{};
    VkPushConstantRange* pushConstantPtr = nullptr;
    if (desc.Constant.has_value()) {
        pushConstant.stageFlags = MapType(desc.Constant->Stages);
        pushConstant.offset = 0;
        pushConstant.size = desc.Constant->Size;
        pushConstantPtr = &pushConstant;
    }
    vector<unique_ptr<DescriptorSetLayoutVulkan>> layouts;
    vector<VkDescriptorSetLayout> vkDescSetLayouts;
    vector<uint8_t> isBindlessSet;
    unordered_map<uint32_t, const RootSignatureDescriptorSet*> setsByIndex;
    uint32_t maxSetIndex = 0;
    for (const auto& set : desc.DescriptorSets) {
        if (setsByIndex.find(set.SetIndex) != setsByIndex.end()) {
            RADRAY_ERR_LOG("duplicate descriptor set index: {}", set.SetIndex);
            return nullptr;
        }
        setsByIndex.emplace(set.SetIndex, &set);
        maxSetIndex = std::max(maxSetIndex, set.SetIndex);
    }
    for (const auto& ss : desc.StaticSamplers) {
        maxSetIndex = std::max(maxSetIndex, ss.SetIndex);
    }
    if (!setsByIndex.empty() || !desc.StaticSamplers.empty()) {
        vkDescSetLayouts.resize(maxSetIndex + 1, VK_NULL_HANDLE);
        layouts.resize(maxSetIndex + 1);
        isBindlessSet.resize(maxSetIndex + 1, false);
    }

    for (uint32_t setIdx = 0; setIdx < vkDescSetLayouts.size(); setIdx++) {
        auto setIt = setsByIndex.find(setIdx);
        const RootSignatureDescriptorSet* setDesc = setIt == setsByIndex.end() ? nullptr : setIt->second;
        std::span<const RootSignatureSetElement> setElements = setDesc == nullptr ? std::span<const RootSignatureSetElement>{} : setDesc->Elements;

        bool hasBindless = false;
        bool hasRegularElements = false;
        VkDescriptorType bindlessType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        for (const auto& elem : setElements) {
            if (elem.BindlessCapacity.has_value()) {
                VkDescriptorType elemType = MapType(elem.Type);
                if (bindlessType == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
                    bindlessType = elemType;
                } else if (bindlessType != elemType) {
                    RADRAY_ERR_LOG("vk bindless set {} contains mixed descriptor types", setIdx);
                    return nullptr;
                }
                hasBindless = true;
            } else {
                hasRegularElements = true;
            }
        }

        vector<RootSignatureStaticSampler> setStaticSamplers;
        for (const auto& ss : desc.StaticSamplers) {
            if (ss.SetIndex == setIdx) {
                setStaticSamplers.push_back(ss);
            }
        }
        bool hasStaticSamplers = !setStaticSamplers.empty();

        if (hasBindless && (hasRegularElements || hasStaticSamplers)) {
            RADRAY_ERR_LOG(
                "Illegal descriptor set layout: Set {} contains both bindless array and other descriptors. "
                "Bindless arrays must be in their own descriptor set. "
                "This is not supported in HLSL and may cause validation errors.",
                setIdx);
            return nullptr;
        }

        if (hasBindless && !hasRegularElements && !hasStaticSamplers) {
            VkDescriptorSetLayout globalLayout = VK_NULL_HANDLE;
            if (bindlessType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                globalLayout = _bdlsTex2d->GetLayout();
            } else if (bindlessType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                globalLayout = _bdlsBuffer->GetLayout();
            }
            if (globalLayout == VK_NULL_HANDLE) {
                RADRAY_ERR_LOG("vk failed to find global bindless layout for type {}", bindlessType);
                return nullptr;
            }
            vkDescSetLayouts[setIdx] = globalLayout;
            isBindlessSet[setIdx] = true;
            continue;
        }

        RootSignatureDescriptorSet emptySet{};
        emptySet.SetIndex = setIdx;
        auto layout = this->CreateDescriptorSetLayout(setDesc == nullptr ? emptySet : *setDesc, setStaticSamplers);
        if (!layout) {
            return nullptr;
        }
        auto layoutOwned = layout.Release();
        vkDescSetLayouts[setIdx] = layoutOwned->_layout;
        layouts[setIdx] = std::move(layoutOwned);
        isBindlessSet[setIdx] = false;
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
        RADRAY_ERR_LOG("vkCreatePipelineLayout failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<PipelineLayoutVulkan>(this, pipelineLayout);
    result->_descSetLayouts = std::move(layouts);
    result->_isBindlessSet = std::move(isBindlessSet);
    if (pushConstantPtr == nullptr) {
        result->_pushConst = std::nullopt;
    } else {
        result->_pushConst = *pushConstantPtr;
    }
    return result;
}

Nullable<unique_ptr<GraphicsPipelineState>> DeviceVulkan::CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept {
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
            rasterInfo.depthBiasConstantFactor = (float)ds.DepthBias.Constant;
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
        RADRAY_ERR_LOG("vkCreateGraphicsPipelines failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<GraphicsPipelineVulkan>(this, pipeline);
    result->_renderPass = std::move(renderPass);
    return result;
}

Nullable<unique_ptr<ComputePipelineState>> DeviceVulkan::CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept {
    auto rs = CastVkObject(desc.RootSig);
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
    if (!ValidateAccelerationStructureDescriptor(desc)) {
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
    this->SetObjectName(desc.Name, vkBuf);

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
    if (desc.RootSig == nullptr) {
        RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor.RootSig is null");
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

    auto rs = CastVkObject(desc.RootSig);
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
    auto buffer = this->CreateBuffer(
        {totalSize, MemoryType::Upload, BufferUse::ShaderTable | BufferUse::MapWrite, ResourceHint::None, desc.Name});
    if (!buffer.HasValue()) {
        return nullptr;
    }
    return make_unique<ShaderBindingTableVulkan>(this, pipeline, buffer.Release(), desc, recordStride);
}

Nullable<unique_ptr<DescriptorSetLayoutVulkan>> DeviceVulkan::CreateDescriptorSetLayout(const RootSignatureDescriptorSet& desc, std::span<const RootSignatureStaticSampler> staticSamplers) noexcept {
    struct BindingCtx {
        VkDescriptorSetLayoutBinding binding;
        vector<unique_ptr<SamplerVulkan>> staticSamplers;
        vector<VkSampler> tmpSS;
        bool isBindless{false};
    };
    vector<BindingCtx> ctxs;
    bool hasBindless = false;
    // Set elements
    for (const auto& j : desc.Elements) {
        auto& ctx = ctxs.emplace_back();
        ctx.binding.binding = j.Slot;
        ctx.binding.descriptorType = MapType(j.Type);
        if (j.BindlessCapacity.has_value()) {
            ctx.binding.descriptorCount = j.BindlessCapacity.value();
            ctx.isBindless = true;
            hasBindless = true;
        } else {
            ctx.binding.descriptorCount = j.Count;
        }
        ctx.binding.stageFlags = MapType(j.Stages);
    }
    // Static samplers (immutable samplers)
    for (const auto& ss : staticSamplers) {
        auto& ctx = ctxs.emplace_back();
        ctx.binding.binding = ss.Slot;
        ctx.binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        ctx.binding.descriptorCount = 1;
        ctx.binding.stageFlags = MapType(ss.Stages);
        auto samplerOpt = this->CreateSamplerVulkan(ss.Desc);
        if (!samplerOpt.HasValue()) {
            return nullptr;
        }
        auto sampler = samplerOpt.Release();
        ctx.tmpSS.emplace_back(sampler->_sampler);
        ctx.staticSamplers.emplace_back(std::move(sampler));
    }
    vector<VkDescriptorSetLayoutBinding> bindings;
    vector<VkDescriptorBindingFlags> bindingFlags;
    bindings.reserve(ctxs.size());
    bindingFlags.reserve(ctxs.size());
    for (const auto& i : ctxs) {
        auto b = i.binding;
        if (i.tmpSS.empty()) {
            b.pImmutableSamplers = nullptr;
        } else {
            b.pImmutableSamplers = i.tmpSS.data();
        }
        bindings.emplace_back(b);
        // Set UPDATE_AFTER_BIND flag for bindless bindings
        VkDescriptorBindingFlags flags = 0;
        if (i.isBindless) {
            flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        }
        bindingFlags.emplace_back(flags);
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    if (hasBindless) {
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.pNext = nullptr;
        flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
        flagsInfo.pBindingFlags = bindingFlags.data();
        dslci.pNext = &flagsInfo;
        dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    } else {
        dslci.pNext = nullptr;
        dslci.flags = 0;
    }
    dslci.bindingCount = static_cast<uint32_t>(bindings.size());
    dslci.pBindings = bindings.empty() ? nullptr : bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateDescriptorSetLayout(_device, &dslci, this->GetAllocationCallbacks(), &layout);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateDescriptorSetLayout failed: {}", vr);
        return nullptr;
    }
    auto result = make_unique<DescriptorSetLayoutVulkan>(this, layout);
    result->_bindings.reserve(ctxs.size());
    for (auto& ctx : ctxs) {
        result->_bindings.emplace_back(ctx.binding, std::move(ctx.staticSamplers));
    }
    return result;
}

Nullable<unique_ptr<SamplerVulkan>> DeviceVulkan::CreateSamplerVulkan(const SamplerDescriptor& desc) noexcept {
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
    auto samplerOpt = this->CreateSamplerVulkan(createInfo);
    if (!samplerOpt.HasValue()) {
        return nullptr;
    }
    auto sampler = samplerOpt.Release();
    sampler->_mdesc = desc;
    return sampler;
}

Nullable<unique_ptr<SamplerVulkan>> DeviceVulkan::CreateSamplerVulkan(const VkSamplerCreateInfo& desc) noexcept {
    VkSampler sampler = VK_NULL_HANDLE;
    if (auto vr = _ftb.vkCreateSampler(_device, &desc, this->GetAllocationCallbacks(), &sampler);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateSampler failed: {}", vr);
        return nullptr;
    }
    return make_unique<SamplerVulkan>(this, sampler);
}

Nullable<unique_ptr<BindlessDescriptorSetVulkan>> DeviceVulkan::CreateBindlessDescriptorSetVulkan(
    VkDescriptorType type,
    uint32_t capacity) noexcept {
    if (capacity == 0) {
        RADRAY_ERR_LOG("vk bindless descriptor set capacity must be greater than 0");
        return nullptr;
    }
    auto bdls = make_unique<BindlessDescriptorSetVulkan>(this, type, capacity);
    VkDescriptorPoolSize poolSize{type, capacity};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.pNext = nullptr;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (auto vr = _ftb.vkCreateDescriptorPool(_device, &poolInfo, this->GetAllocationCallbacks(), &bdls->_pool);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateDescriptorPool failed: {}", vr);
        return nullptr;
    }
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = type;
    binding.descriptorCount = capacity;
    binding.stageFlags = VK_SHADER_STAGE_ALL;
    binding.pImmutableSamplers = nullptr;
    VkDescriptorBindingFlags bindFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.pNext = nullptr;
    flagsInfo.bindingCount = 1;
    flagsInfo.pBindingFlags = &bindFlags;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (auto vr = _ftb.vkCreateDescriptorSetLayout(_device, &layoutInfo, this->GetAllocationCallbacks(), &bdls->_layout);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateDescriptorSetLayout failed: {}", vr);
        return nullptr;
    }
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.pNext = nullptr;
    allocInfo.descriptorPool = bdls->_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &bdls->_layout;
    if (auto vr = _ftb.vkAllocateDescriptorSets(_device, &allocInfo, &bdls->_set);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkAllocateDescriptorSets failed: {}", vr);
        return nullptr;
    }
    return bdls;
}

Nullable<unique_ptr<CommandBufferVulkan>> DeviceVulkan::CreateCommandBufferVulkan(QueueVulkan* queue) noexcept {
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

Nullable<unique_ptr<DescriptorSet>> DeviceVulkan::CreateDescriptorSet(RootSignature* rootSig_, uint32_t index) noexcept {
    auto rootSig = CastVkObject(rootSig_);
    if (index >= rootSig->_descSetLayouts.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "index", rootSig->_descSetLayouts.size(), index);
        return nullptr;
    }
    if (rootSig->IsBindlessSet(index)) {
        RADRAY_ERR_LOG("cannot create descriptor set for bindless set {}", index);
        return nullptr;
    }
    const auto& layout = rootSig->_descSetLayouts[index];
    if (!layout) {
        RADRAY_ERR_LOG("cannot create descriptor set for missing layout set {}", index);
        return nullptr;
    }
    auto allocOpt = _descSetAlloc->Allocate(layout.get());
    if (!allocOpt.has_value()) {
        return nullptr;
    }
    return make_unique<DescriptorSetVulkan>(this, layout.get(), _descSetAlloc.get(), allocOpt.value());
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
    auto result = make_unique<BindlessArrayVulkan>(this, desc);
    if (desc.SlotType == BindlessSlotType::BufferOnly || desc.SlotType == BindlessSlotType::Multiple) {
        auto allocOpt = _bdlsBuffer->Allocate(desc.Size);
        if (!allocOpt.has_value()) {
            RADRAY_ERR_LOG("vk bindless array failed to allocate buffer descriptors (size={})", desc.Size);
            return nullptr;
        }
        result->_bufferAlloc = allocOpt.value();
    }
    if (desc.SlotType == BindlessSlotType::Texture2DOnly || desc.SlotType == BindlessSlotType::Multiple) {
        auto allocOpt = _bdlsTex2d->Allocate(desc.Size);
        if (!allocOpt.has_value()) {
            RADRAY_ERR_LOG("vk bindless array failed to allocate tex2d descriptors (size={})", desc.Size);
            return nullptr;
        }
        result->_tex2dAlloc = allocOpt.value();
    }
    if (desc.SlotType == BindlessSlotType::Texture3DOnly || desc.SlotType == BindlessSlotType::Multiple) {
        auto allocOpt = _bdlsTex3d->Allocate(desc.Size);
        if (!allocOpt.has_value()) {
            RADRAY_ERR_LOG("vk bindless array failed to allocate tex3d descriptors (size={})", desc.Size);
            return nullptr;
        }
        result->_tex3dAlloc = allocOpt.value();
    }
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
        RADRAY_ERR_LOG("vkCreateFence failed: {}", vr);
        return nullptr;
    }
    auto v = make_unique<FenceVulkan>(this, fence);
    v->_submitted = false;
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
        RADRAY_ERR_LOG("vkCreateSemaphore failed: {}", vr);
        return nullptr;
    }
    auto v = make_unique<SemaphoreVulkan>(this, semaphore);
    v->_signaled = false;
    return v;
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
    return make_unique<BufferViewVulkan>(this, bufferView);
}

Nullable<unique_ptr<RenderPassVulkan>> DeviceVulkan::CreateRenderPass(const VkRenderPassCreateInfo& info) noexcept {
    VkRenderPass pass;
    if (auto vr = _ftb.vkCreateRenderPass(_device, &info, this->GetAllocationCallbacks(), &pass);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateRenderPass failed: {}", vr);
        return nullptr;
    }
    return make_unique<RenderPassVulkan>(this, pass);
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
    _bdlsTex2d.reset();
    _bdlsTex3d.reset();
    _descSetAlloc.reset();
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
        validEnables.emplace_back(VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT);
        validEnables.emplace_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
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
    for (uint32_t apiVersion : apiVersionsToTry) {
        appInfo.apiVersion = apiVersion;
        if (auto vr = vkCreateInstance(&createInfo, allocCbPtr, &instance);
            vr == VK_SUCCESS) {
            break;
        } else {
            lastResult = vr;
            RADRAY_WARN_LOG("vkCreateInstance with api version {}.{}.{} failed: {}", VK_VERSION_MAJOR(apiVersion), VK_VERSION_MINOR(apiVersion), VK_VERSION_PATCH(apiVersion), vr);
        }
    }
    if (instance == VK_NULL_HANDLE) {
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
            if (const auto vr = vkCreateDebugUtilsMessengerEXT(
                    instance,
                    &debugCreateInfo,
                    result->GetAllocationCallbacks(),
                    &result->_debugMessenger);
                vr != VK_SUCCESS) {
                RADRAY_WARN_LOG("vkCreateDebugUtilsMessengerEXT failed: {}", vr);
            }
        } else {
            RADRAY_WARN_LOG("vkCreateDebugUtilsMessengerEXT is null");
        }
    }
    g_vkInstance = result.get();
    return result;
}

void DestroyVulkanInstanceImpl(unique_ptr<InstanceVulkan> instance) noexcept {
    RADRAY_ASSERT(g_vkInstance.Get() == instance.get());
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
        RADRAY_ERR_LOG("vk env not init");
        return nullptr;
    }
    VkInstance instance = g_vkInstance->_instance;
    vector<VkPhysicalDevice> physicalDevices;
    if (EnumerateVectorFromVkFunc(physicalDevices, vkEnumeratePhysicalDevices, instance) != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkEnumeratePhysicalDevices failed");
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
        RADRAY_INFO_LOG("vk find physical device: {}, heap memory: {}MB", deviceProps.deviceName, total / (1024 * 1024));
        physicalDeviceProps.emplace_back(PhyDeviceInfo{phyDevice, deviceProps, memory, i});
    }
    size_t selectPhysicalDeviceIndex = std::numeric_limits<size_t>::max();
    if (desc.PhysicalDeviceIndex.has_value()) {
        uint32_t index = desc.PhysicalDeviceIndex.value();
        if (index >= physicalDevices.size()) {
            RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "PhysicalDeviceIndex", physicalDevices.size(), index);
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
                RADRAY_WARN_LOG("vk unsupported queue type: {}", FormatVkQueueFlags(r.requiredFlag));
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
                RADRAY_ERR_LOG("vk not enough queue family for type: {}", FormatVkQueueFlags(i.requiredFlag));
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
        RADRAY_ERR_LOG("vkEnumerateDeviceExtensionProperties failed: {}", vr);
        return nullptr;
    }
    if (IsValidateExtensions("VK_KHR_portability_subset", deviceExtsAvailable)) {
        needExts.emplace("VK_KHR_portability_subset");
    }
    const bool hasRtExts =
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
    if (IsValidateExtensions(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, deviceExtsAvailable)) {
        extProperties->accelerationStructure = VkPhysicalDeviceAccelerationStructurePropertiesKHR{};
        auto& asProp = extProperties->accelerationStructure.value();
        asProp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        asProp.pNext = nullptr;
        AddToHeadVulkanStruct(deviceProperties2, asProp);
    }
    if (IsValidateExtensions(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, deviceExtsAvailable)) {
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

        // Enable queried features conservatively.
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
    if (VkResult vr = vkCreateDevice(selectPhyDevice.device, &deviceInfo, g_vkInstance->GetAllocationCallbacks(), &device);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateDevice failed: {}", vr);
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
            auto queue = make_unique<QueueVulkan>(deviceR.get(), queuePtr, j, i.rawType);
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
        detail.TextureDataPitchAlignment = (uint32_t)deviceR->_properties.limits.optimalBufferCopyRowPitchAlignment;
        detail.MaxVertexInputBindings = deviceR->_properties.limits.maxVertexInputBindings;
        detail.IsUMA = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
        detail.VramBudget = GetPhysicalDeviceMemoryAllSize(selectPhyDevice.memory, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        const auto& f12 = deviceR->_extFeatures.feature12;
        detail.IsBindlessArraySupported =
            selectPhyDevice.properties.apiVersion >= VK_API_VERSION_1_2 &&
            f12.runtimeDescriptorArray &&
            f12.descriptorBindingVariableDescriptorCount &&
            f12.shaderSampledImageArrayNonUniformIndexing;
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
        auto bdlsBufferOpt = deviceR->CreateBindlessDescriptorSetVulkan(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 262144);
        if (!bdlsBufferOpt) {
            return nullptr;
        }
        deviceR->_bdlsBuffer = make_unique<BindlessDescAllocator>(bdlsBufferOpt.Release());
        auto bdlsTex2dOpt = deviceR->CreateBindlessDescriptorSetVulkan(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 262144);
        if (!bdlsTex2dOpt) {
            return nullptr;
        }
        deviceR->_bdlsTex2d = make_unique<BindlessDescAllocator>(bdlsTex2dOpt.Release());
        auto bdlsTex3dOpt = deviceR->CreateBindlessDescriptorSetVulkan(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 262144);
        if (!bdlsTex3dOpt) {
            return nullptr;
        }
        deviceR->_bdlsTex3d = make_unique<BindlessDescAllocator>(bdlsTex3dOpt.Release());
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
    vector<VkSemaphore> waitSemaphores;
    waitSemaphores.reserve(desc.WaitSemaphores.size());
    vector<VkPipelineStageFlags> waitStages;
    waitStages.resize(desc.WaitSemaphores.size(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (auto i : desc.WaitSemaphores) {
        auto semaphore = CastVkObject(i);
        if (semaphore->_signaled) {
            waitSemaphores.emplace_back(semaphore->_semaphore);
            semaphore->_signaled = false;
        }
    }
    vector<VkCommandBuffer> cmdBufs;
    cmdBufs.reserve(desc.CmdBuffers.size());
    for (auto i : desc.CmdBuffers) {
        auto cmdBuffer = CastVkObject(i);
        cmdBufs.emplace_back(cmdBuffer->_cmdBuffer);
    }
    vector<VkSemaphore> signalSemaphores;
    signalSemaphores.reserve(desc.SignalSemaphores.size());
    for (auto i : desc.SignalSemaphores) {
        auto semaphore = CastVkObject(i);
        if (!semaphore->_signaled) {
            signalSemaphores.emplace_back(semaphore->_semaphore);
            semaphore->_signaled = true;
        }
    }
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = nullptr;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    submitInfo.commandBufferCount = static_cast<uint32_t>(cmdBufs.size());
    submitInfo.pCommandBuffers = cmdBufs.empty() ? nullptr : cmdBufs.data();
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
    submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();

    VkFence signalFence = VK_NULL_HANDLE;
    if (desc.SignalFence.HasValue()) {
        auto fence = CastVkObject(desc.SignalFence.Get());
        signalFence = fence->_fence;
    }

    if (auto vr = _device->_ftb.vkQueueSubmit(_queue, 1, &submitInfo, signalFence);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkQueueSubmit failed: {}", vr);
    }

    if (desc.SignalFence.HasValue()) {
        auto fence = CastVkObject(desc.SignalFence.Get());
        fence->_submitted = true;
    }
}

void QueueVulkan::Wait() noexcept {
    if (auto vr = _device->_ftb.vkQueueWaitIdle(_queue);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vkQueueWaitIdle failed: {}", vr);
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
    _device->_ftb.vkCmdPipelineBarrier(
        _cmdBuffer,
        srcStageMask,
        dstStageMask,
        0,
        0, nullptr,
        static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.data(),
        static_cast<uint32_t>(imageBarriers.size()), imageBarriers.data());
}

Nullable<unique_ptr<GraphicsCommandEncoder>> CommandBufferVulkan::BeginRenderPass(const RenderPassDescriptor& desc) noexcept {
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
        attachDesc.samples = MapSampleCount(imageView->_image->_mdesc.SampleCount);
        attachDesc.loadOp = MapType(i.Load);
        attachDesc.storeOp = MapType(i.Store);
        attachDesc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachDesc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        auto& colorRef = colorRefs.emplace_back();
        colorRef.attachment = static_cast<uint32_t>(attachs.size() - 1);
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        fbs.emplace_back(imageView->_imageView);
        auto& clear = clearValues.emplace_back();
        if (IsUintFormat(imageView->_mdesc.Format)) {
            clear.color.uint32[0] = static_cast<uint32_t>(i.ClearValue.Value[0]);
            clear.color.uint32[1] = static_cast<uint32_t>(i.ClearValue.Value[1]);
            clear.color.uint32[2] = static_cast<uint32_t>(i.ClearValue.Value[2]);
            clear.color.uint32[3] = static_cast<uint32_t>(i.ClearValue.Value[3]);
        } else if (IsSintFormat(imageView->_mdesc.Format)) {
            clear.color.int32[0] = static_cast<int32_t>(i.ClearValue.Value[0]);
            clear.color.int32[1] = static_cast<int32_t>(i.ClearValue.Value[1]);
            clear.color.int32[2] = static_cast<int32_t>(i.ClearValue.Value[2]);
            clear.color.int32[3] = static_cast<int32_t>(i.ClearValue.Value[3]);
        } else {
            clear.color.float32[0] = i.ClearValue.Value[0];
            clear.color.float32[1] = i.ClearValue.Value[1];
            clear.color.float32[2] = i.ClearValue.Value[2];
            clear.color.float32[3] = i.ClearValue.Value[3];
        }
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
        attachDesc.samples = MapSampleCount(imageView->_image->_mdesc.SampleCount);
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
    _device->SetObjectName(desc.Name, passR->_renderPass);
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
        RADRAY_ERR_LOG("vkCreateFramebuffer failed: {}", vr);
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
    const auto& texDesc = dst->_mdesc;
    const uint32_t bpp = GetTextureFormatBytesPerPixel(texDesc.Format);
    if (bpp == 0) {
        RADRAY_ERR_LOG("vk CopyBufferToTexture invalid texture format {}", texDesc.Format);
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
    if (dstRange.BaseMipLevel >= texDesc.MipLevels ||
        dstRange.BaseMipLevel + mipLevels > texDesc.MipLevels) {
        RADRAY_ERR_LOG("vk CopyBufferToTexture mip range out of bounds (base={}, count={}, total={})",
                       dstRange.BaseMipLevel, mipLevels, texDesc.MipLevels);
        return;
    }
    bool is3D = texDesc.Dim == TextureDimension::Dim3D;
    uint32_t arraySize = is3D ? 1u : texDesc.DepthOrArraySize;
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
        uint32_t mipWidth = std::max(texDesc.Width >> mipLevel, 1u);
        uint32_t mipHeight = std::max(texDesc.Height >> mipLevel, 1u);
        uint32_t mipDepth = is3D ? std::max(texDesc.DepthOrArraySize >> mipLevel, 1u) : 1u;
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
            copyInfo.bufferImageHeight = 0;
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
    const auto& texDesc = src->_mdesc;
    const uint32_t bpp = GetTextureFormatBytesPerPixel(texDesc.Format);
    if (bpp == 0) {
        RADRAY_ERR_LOG("vk CopyTextureToBuffer invalid texture format {}", texDesc.Format);
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
    if (srcRange.BaseMipLevel >= texDesc.MipLevels ||
        srcRange.BaseMipLevel + mipLevels > texDesc.MipLevels) {
        RADRAY_ERR_LOG("vk CopyTextureToBuffer mip range out of bounds (base={}, count={}, total={})",
                       srcRange.BaseMipLevel, mipLevels, texDesc.MipLevels);
        return;
    }
    bool is3D = texDesc.Dim == TextureDimension::Dim3D;
    uint32_t arraySize = is3D ? 1u : texDesc.DepthOrArraySize;
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
        uint32_t mipWidth = std::max(texDesc.Width >> mipLevel, 1u);
        uint32_t mipHeight = std::max(texDesc.Height >> mipLevel, 1u);
        uint32_t mipDepth = is3D ? std::max(texDesc.DepthOrArraySize >> mipLevel, 1u) : 1u;
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
            copyInfo.bufferImageHeight = 0;
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

CommandBuffer* SimulateCommandEncoderVulkan::GetCommandBuffer() const noexcept {
    return _cmdBuffer;
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
        RADRAY_ERR_LOG("bind root signature before CommandEncoder::PushConstant");
        return;
    }
    if (!_boundPipeLayout->_pushConst.has_value()) {
        RADRAY_ERR_LOG("VkPipelineLayout has no push constant");
        return;
    }
    const auto& pc = _boundPipeLayout->_pushConst.value();
    if (length > pc.size) {
        RADRAY_ERR_LOG("vk push constant length {} exceeds VkPipelineLayout push constant size {}", length, pc.size);
        return;
    }
    _device->_ftb.vkCmdPushConstants(_cmdBuffer->_cmdBuffer, _boundPipeLayout->_layout, pc.stageFlags, 0, static_cast<uint32_t>(length), data);
}

void SimulateCommandEncoderVulkan::BindRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept {
    RADRAY_UNUSED(slot);
    RADRAY_UNUSED(buffer);
    RADRAY_UNUSED(offset);
    RADRAY_UNUSED(size);
    RADRAY_ERR_LOG("unsupported CommandEncoder::BindRootDescriptor in vk");
}

void SimulateCommandEncoderVulkan::BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("bind root signature before CommandEncoder::BindDescriptorSet");
        return;
    }
    if (slot >= _boundPipeLayout->_descSetLayouts.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _boundPipeLayout->_descSetLayouts.size(), slot);
        return;
    }
    auto descSet = CastVkObject(set);
    _device->_ftb.vkCmdBindDescriptorSets(
        _cmdBuffer->_cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _boundPipeLayout->_layout,
        slot,
        1,
        &descSet->_allocation.Set,
        0, nullptr);
}

void SimulateCommandEncoderVulkan::BindBindlessArray(uint32_t slot, BindlessArray* array) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("bind root signature before CommandEncoder::BindBindlessArray");
        return;
    }
    if (slot >= _boundPipeLayout->_descSetLayouts.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _boundPipeLayout->_descSetLayouts.size(), slot);
        return;
    }
    if (!_boundPipeLayout->IsBindlessSet(slot)) {
        RADRAY_ERR_LOG("set {} is not a bindless set", slot);
        return;
    }
    auto bdlsArray = static_cast<BindlessArrayVulkan*>(array);
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (bdlsArray->_tex2dAlloc.IsValid()) {
        set = bdlsArray->_tex2dAlloc.Set;
    } else if (bdlsArray->_tex3dAlloc.IsValid()) {
        set = bdlsArray->_tex3dAlloc.Set;
    } else if (bdlsArray->_bufferAlloc.IsValid()) {
        set = bdlsArray->_bufferAlloc.Set;
    }
    if (set == VK_NULL_HANDLE) {
        RADRAY_ERR_LOG("bindless array has no valid allocation");
        return;
    }
    _device->_ftb.vkCmdBindDescriptorSets(
        _cmdBuffer->_cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        _boundPipeLayout->_layout,
        slot,
        1,
        &set,
        0, nullptr);
}

void SimulateCommandEncoderVulkan::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
    _device->_ftb.vkCmdDraw(_cmdBuffer->_cmdBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void SimulateCommandEncoderVulkan::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
    _device->_ftb.vkCmdDrawIndexed(_cmdBuffer->_cmdBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

// ======================== SimulateComputeEncoderVulkan ========================

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

void SimulateComputeEncoderVulkan::BindRootSignature(RootSignature* rootSig) noexcept {
    auto layout = CastVkObject(rootSig);
    _boundPipeLayout = layout;
}

void SimulateComputeEncoderVulkan::PushConstant(const void* data, size_t length) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("bind root signature before ComputeCommandEncoder::PushConstant");
        return;
    }
    if (!_boundPipeLayout->_pushConst.has_value()) {
        RADRAY_ERR_LOG("VkPipelineLayout has no push constant");
        return;
    }
    const auto& pc = _boundPipeLayout->_pushConst.value();
    if (length > pc.size) {
        RADRAY_ERR_LOG("vk push constant length {} exceeds VkPipelineLayout push constant size {}", length, pc.size);
        return;
    }
    _device->_ftb.vkCmdPushConstants(_cmdBuffer->_cmdBuffer, _boundPipeLayout->_layout, pc.stageFlags, 0, static_cast<uint32_t>(length), data);
}

void SimulateComputeEncoderVulkan::BindRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept {
    RADRAY_UNUSED(slot);
    RADRAY_UNUSED(buffer);
    RADRAY_UNUSED(offset);
    RADRAY_UNUSED(size);
    RADRAY_ERR_LOG("unsupported ComputeCommandEncoder::BindRootDescriptor in vk");
}

void SimulateComputeEncoderVulkan::BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("bind root signature before ComputeCommandEncoder::BindDescriptorSet");
        return;
    }
    if (slot >= _boundPipeLayout->_descSetLayouts.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _boundPipeLayout->_descSetLayouts.size(), slot);
        return;
    }
    auto descSet = CastVkObject(set);
    _device->_ftb.vkCmdBindDescriptorSets(
        _cmdBuffer->_cmdBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _boundPipeLayout->_layout,
        slot,
        1,
        &descSet->_allocation.Set,
        0, nullptr);
}

void SimulateComputeEncoderVulkan::BindBindlessArray(uint32_t slot, BindlessArray* array) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("bind root signature before ComputeCommandEncoder::BindBindlessArray");
        return;
    }
    if (slot >= _boundPipeLayout->_descSetLayouts.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _boundPipeLayout->_descSetLayouts.size(), slot);
        return;
    }
    if (!_boundPipeLayout->IsBindlessSet(slot)) {
        RADRAY_ERR_LOG("set {} is not a bindless set", slot);
        return;
    }
    auto bdlsArray = static_cast<BindlessArrayVulkan*>(array);
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (bdlsArray->_tex2dAlloc.IsValid()) {
        set = bdlsArray->_tex2dAlloc.Set;
    } else if (bdlsArray->_tex3dAlloc.IsValid()) {
        set = bdlsArray->_tex3dAlloc.Set;
    } else if (bdlsArray->_bufferAlloc.IsValid()) {
        set = bdlsArray->_bufferAlloc.Set;
    }
    if (set == VK_NULL_HANDLE) {
        RADRAY_ERR_LOG("bindless array has no valid allocation");
        return;
    }
    _device->_ftb.vkCmdBindDescriptorSets(
        _cmdBuffer->_cmdBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        _boundPipeLayout->_layout,
        slot,
        1,
        &set,
        0, nullptr);
}

void SimulateComputeEncoderVulkan::BindComputePipelineState(ComputePipelineState* pso) noexcept {
    auto p = CastVkObject(pso);
    _device->_ftb.vkCmdBindPipeline(_cmdBuffer->_cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, p->_pipeline);
}

void SimulateComputeEncoderVulkan::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
    _device->_ftb.vkCmdDispatch(_cmdBuffer->_cmdBuffer, groupCountX, groupCountY, groupCountZ);
}

void SimulateComputeEncoderVulkan::SetThreadGroupSize(uint32_t x, uint32_t y, uint32_t z) noexcept {
    RADRAY_UNUSED(x);
    RADRAY_UNUSED(y);
    RADRAY_UNUSED(z);
    // Vulkan: thread group size is defined in shader (local_size), no-op here
}

// ======================== CommandEncoderRayTracingVulkan ========================

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

void CommandEncoderRayTracingVulkan::BindRootSignature(RootSignature* rootSig) noexcept {
    _boundPipeLayout = CastVkObject(rootSig);
}

void CommandEncoderRayTracingVulkan::PushConstant(const void* data, size_t length) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("bind root signature before RayTracingCommandEncoder::PushConstant");
        return;
    }
    if (!_boundPipeLayout->_pushConst.has_value()) {
        RADRAY_ERR_LOG("VkPipelineLayout has no push constant");
        return;
    }
    const auto& pc = _boundPipeLayout->_pushConst.value();
    if (length > pc.size) {
        RADRAY_ERR_LOG("vk push constant length {} exceeds VkPipelineLayout push constant size {}", length, pc.size);
        return;
    }
    _device->_ftb.vkCmdPushConstants(_cmdBuffer->_cmdBuffer, _boundPipeLayout->_layout, pc.stageFlags, 0, static_cast<uint32_t>(length), data);
}

void CommandEncoderRayTracingVulkan::BindRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept {
    RADRAY_UNUSED(slot);
    RADRAY_UNUSED(buffer);
    RADRAY_UNUSED(offset);
    RADRAY_UNUSED(size);
    RADRAY_ERR_LOG("unsupported RayTracingCommandEncoder::BindRootDescriptor in vk");
}

void CommandEncoderRayTracingVulkan::BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("bind root signature before RayTracingCommandEncoder::BindDescriptorSet");
        return;
    }
    if (slot >= _boundPipeLayout->_descSetLayouts.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _boundPipeLayout->_descSetLayouts.size(), slot);
        return;
    }
    auto descSet = CastVkObject(set);
    _device->_ftb.vkCmdBindDescriptorSets(
        _cmdBuffer->_cmdBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        _boundPipeLayout->_layout,
        slot,
        1,
        &descSet->_allocation.Set,
        0, nullptr);
}

void CommandEncoderRayTracingVulkan::BindBindlessArray(uint32_t slot, BindlessArray* array) noexcept {
    if (!_boundPipeLayout) {
        RADRAY_ERR_LOG("bind root signature before RayTracingCommandEncoder::BindBindlessArray");
        return;
    }
    if (slot >= _boundPipeLayout->_descSetLayouts.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _boundPipeLayout->_descSetLayouts.size(), slot);
        return;
    }
    if (!_boundPipeLayout->IsBindlessSet(slot)) {
        RADRAY_ERR_LOG("set {} is not a bindless set", slot);
        return;
    }
    auto bdlsArray = static_cast<BindlessArrayVulkan*>(array);
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (bdlsArray->_tex2dAlloc.IsValid()) {
        set = bdlsArray->_tex2dAlloc.Set;
    } else if (bdlsArray->_tex3dAlloc.IsValid()) {
        set = bdlsArray->_tex3dAlloc.Set;
    } else if (bdlsArray->_bufferAlloc.IsValid()) {
        set = bdlsArray->_bufferAlloc.Set;
    }
    if (set == VK_NULL_HANDLE) {
        RADRAY_ERR_LOG("bindless array has no valid allocation");
        return;
    }
    _device->_ftb.vkCmdBindDescriptorSets(
        _cmdBuffer->_cmdBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        _boundPipeLayout->_layout,
        slot,
        1,
        &set,
        0, nullptr);
}

void CommandEncoderRayTracingVulkan::BuildBottomLevelAS(const BuildBottomLevelASDescriptor& desc) noexcept {
    if (!ValidateBuildBottomLevelASDescriptor(desc)) {
        return;
    }
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
    if (!ValidateBuildTopLevelASDescriptor(desc)) {
        return;
    }
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
    auto getBufferAddress = [&](BufferVulkan* b) -> VkDeviceAddress {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = b->_buffer;
        return _device->_ftb.vkGetBufferDeviceAddress(_device->_device, &info);
    };
    const uint64_t instanceBytes = sizeof(VkAccelerationStructureInstanceKHR) * desc.Instances.size();
    auto instBufOpt = _device->CreateBuffer({
        Align(instanceBytes, 16ull),
        MemoryType::Upload,
        BufferUse::Scratch | BufferUse::MapWrite,
        ResourceHint::None,
        "vk_tlas_instances"});
    if (!instBufOpt.HasValue()) {
        return;
    }
    auto instBuf = instBufOpt.Release();
    auto* mapped = static_cast<VkAccelerationStructureInstanceKHR*>(instBuf->Map(0, instanceBytes));
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
    instBuf->Unmap(0, instanceBytes);
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
    if (!ValidateTraceRaysDescriptor(resolved, _device->GetDetail())) {
        return;
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

FenceStatus FenceVulkan::GetStatus() const noexcept {
    if (_submitted) {
        VkResult vr = _device->_ftb.vkGetFenceStatus(_device->_device, _fence);
        return vr == VK_SUCCESS ? FenceStatus::Complete : FenceStatus::Incomplete;
    } else {
        return FenceStatus::NotSubmitted;
    }
}

void FenceVulkan::Wait() noexcept {
    if (_submitted) {
        if (auto vr = _device->_ftb.vkWaitForFences(_device->_device, 1, &_fence, VK_TRUE, UINT64_MAX);
            vr != VK_SUCCESS) {
            RADRAY_ABORT("vkWaitForFences failed: {}", vr);
        }
        _device->_ftb.vkResetFences(_device->_device, 1, &_fence);
        _submitted = false;
    }
}

void FenceVulkan::Reset() noexcept {
    _device->_ftb.vkResetFences(_device->_device, 1, &_fence);
    _submitted = false;
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

Nullable<Texture*> SwapChainVulkan::AcquireNext(Nullable<Semaphore*> signalSemaphore, Nullable<Fence*> signalFence) noexcept {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (signalSemaphore.HasValue()) {
        auto sem = CastVkObject(signalSemaphore.Get());
        semaphore = sem->_semaphore;
    }
    VkFence fence = VK_NULL_HANDLE;
    if (signalFence.HasValue()) {
        auto f = CastVkObject(signalFence.Get());
        fence = f->_fence;
    }
    _currentTextureIndex = std::numeric_limits<uint32_t>::max();
    auto vr = _device->_ftb.vkAcquireNextImageKHR(
        _device->_device,
        _swapchain,
        UINT64_MAX,
        semaphore,
        fence,
        &_currentTextureIndex);
    if (vr == VK_SUCCESS) {
        if (signalFence.HasValue()) {
            auto fenceObj = CastVkObject(signalFence.Get());
            fenceObj->_submitted = true;
        }
        if (signalSemaphore.HasValue()) {
            auto semObj = CastVkObject(signalSemaphore.Get());
            semObj->_signaled = true;
        }
        Frame& imageFrame = _frames[_currentTextureIndex];
        return imageFrame.image.get();
    } else if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_TIMEOUT || vr == VK_NOT_READY) {  // 窗口大小改变时可能返回 VK_ERROR_OUT_OF_DATE_KHR
        if (signalFence.HasValue()) {
            auto fenceObj = CastVkObject(signalFence.Get());
            _device->_ftb.vkResetFences(_device->_device, 1, &fenceObj->_fence);
            fenceObj->_submitted = false;
        }
        if (signalSemaphore.HasValue()) {
            auto semObj = CastVkObject(signalSemaphore.Get());
            semObj->_signaled = false;
        }
        RADRAY_WARN_LOG("vkAcquireNextImageKHR failed: {}", vr);
        return nullptr;
    } else {
        RADRAY_ERR_LOG("vkAcquireNextImageKHR failed: {}", vr);
        return nullptr;
    }
}

void SwapChainVulkan::Present(std::span<Semaphore*> waitSemaphores) noexcept {
    vector<VkSemaphore> waitSems;
    waitSems.reserve(waitSemaphores.size());
    for (auto sem : waitSemaphores) {
        auto semaphore = CastVkObject(sem);
        if (semaphore->_signaled) {
            waitSems.emplace_back(semaphore->_semaphore);
            semaphore->_signaled = false;
        }
    }
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.waitSemaphoreCount = waitSems.empty() ? 0 : static_cast<uint32_t>(waitSems.size());
    presentInfo.pWaitSemaphores = waitSems.empty() ? nullptr : waitSems.data();
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
            RADRAY_ABORT("vmaMapMemory failed: {}", vr);
        }
        mappedData = static_cast<byte*>(mappedData) + offset;
    }
    return mappedData;
}

void BufferVulkan::Unmap(uint64_t offset, uint64_t size) noexcept {
    RADRAY_UNUSED(offset);
    RADRAY_UNUSED(size);
    if (!_allocInfo.pMappedData) {
        vmaUnmapMemory(_device->_vma->_vma, _allocation);
    }
}

BufferDescriptor BufferVulkan::GetDesc() const noexcept {
    return BufferDescriptor{
        _allocInfo.size,
        _memory,
        _usage,
        _hints,
        _name};
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

void ImageVulkan::SetExtData(const TextureDescriptor& desc) noexcept {
    _mdesc = desc;
    _name = desc.Name;
    _mdesc.Name = _name;
    _rawFormat = MapType(_mdesc.Format);
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

DescriptorSetLayoutBindingVulkanContainer::DescriptorSetLayoutBindingVulkanContainer(
    const VkDescriptorSetLayoutBinding& binding,
    vector<unique_ptr<SamplerVulkan>> immutableSamplers) noexcept
    : binding(binding.binding),
      descriptorType(binding.descriptorType),
      descriptorCount(binding.descriptorCount),
      stageFlags(binding.stageFlags),
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

bool PipelineLayoutVulkan::IsBindlessSet(uint32_t index) const noexcept {
    return index < _isBindlessSet.size() && _isBindlessSet[index];
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

bool ShaderBindingTableVulkan::Build(std::span<const ShaderBindingTableBuildEntry> entries) noexcept {
    if (!this->IsValid()) {
        return false;
    }
    auto req = _pipeline->GetShaderBindingTableRequirements();
    if (req.HandleSize == 0 || _recordStride < req.HandleSize) {
        RADRAY_ERR_LOG("invalid SBT record stride/handle size");
        return false;
    }
    uint64_t totalSize = _buffer->GetDesc().Size;
    auto* mapped = static_cast<byte*>(_buffer->Map(0, totalSize));
    if (mapped == nullptr) {
        RADRAY_ERR_LOG("failed to map SBT buffer");
        return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(totalSize));
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
            _buffer->Unmap(0, totalSize);
            return false;
        }
        auto handle = _pipeline->GetShaderBindingTableHandle(entry.ShaderName);
        if (!handle.has_value()) {
            RADRAY_ERR_LOG("cannot find shader handle '{}'", entry.ShaderName);
            _buffer->Unmap(0, totalSize);
            return false;
        }
        if (entry.LocalData.size() > _recordStride - req.HandleSize) {
            RADRAY_ERR_LOG("local data too large for SBT record '{}'", entry.ShaderName);
            _buffer->Unmap(0, totalSize);
            return false;
        }
        uint64_t dstOffset = baseOffset + _recordStride * entry.RecordIndex;
        auto* dst = mapped + dstOffset;
        std::memcpy(dst, handle->data(), req.HandleSize);
        if (!entry.LocalData.empty()) {
            std::memcpy(dst + req.HandleSize, entry.LocalData.data(), entry.LocalData.size());
        }
    }
    _buffer->Unmap(0, totalSize);
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
      _asSize(asSize),
      _name(desc.Name) {}

AccelerationStructureVulkan::~AccelerationStructureVulkan() noexcept {
    this->DestroyImpl();
}

bool AccelerationStructureVulkan::IsValid() const noexcept {
    return _buffer != VK_NULL_HANDLE && _allocation != VK_NULL_HANDLE && _accelerationStructure != VK_NULL_HANDLE;
}

void AccelerationStructureVulkan::Destroy() noexcept {
    this->DestroyImpl();
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

void AccelerationStructureViewVulkan::DestroyImpl() noexcept {
    _target = nullptr;
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
    DescriptorSetLayoutVulkan* layout,
    DescriptorSetAllocatorVulkan* allocator,
    DescriptorSetAllocatorVulkan::Allocation allocation) noexcept
    : _device(device),
      _layout(layout),
      _allocator(allocator),
      _allocation(allocation) {}

DescriptorSetVulkan::~DescriptorSetVulkan() noexcept {
    this->DestroyImpl();
}

bool DescriptorSetVulkan::IsValid() const noexcept {
    return _allocation.IsValid();
}

void DescriptorSetVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void DescriptorSetVulkan::DestroyImpl() noexcept {
    if (_allocation.IsValid()) {
        _allocator->Destroy(_allocation);
        _allocation = DescriptorSetAllocatorVulkan::Allocation::Invalid();
    }
}

void DescriptorSetVulkan::SetResource(uint32_t slot, uint32_t index, ResourceView* view) noexcept {
    if (!_layout || !_allocation.IsValid()) {
        RADRAY_ERR_LOG("vk invalid descriptor set");
        return;
    }
    if (slot >= _layout->_bindings.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _layout->_bindings.size(), slot);
        return;
    }
    const auto& e = _layout->_bindings[slot];
    if (index >= e.descriptorCount) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "index", e.descriptorCount, index);
        return;
    }
    auto tag = view->GetTag();
    VkWriteDescriptorSet writeDesc{};
    writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDesc.pNext = nullptr;
    writeDesc.dstSet = _allocation.Set;
    writeDesc.dstBinding = e.binding;
    writeDesc.dstArrayElement = index;
    writeDesc.descriptorCount = 1;
    writeDesc.descriptorType = e.descriptorType;
    writeDesc.pBufferInfo = nullptr;
    writeDesc.pImageInfo = nullptr;
    writeDesc.pTexelBufferView = nullptr;
    VkDescriptorBufferInfo bufInfo{};
    VkDescriptorImageInfo imgInfo{};
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
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
        imgInfo.imageLayout = TextureViewUsageToLayout(tv->_mdesc.Usage);
        writeDesc.pImageInfo = &imgInfo;
    } else if (tag.HasFlag(RenderObjectTag::AccelerationStructureView)) {
        auto asView = static_cast<AccelerationStructureViewVulkan*>(view);
        if (asView->_target == nullptr || asView->_target->_accelerationStructure == VK_NULL_HANDLE) {
            RADRAY_ERR_LOG("vk invalid acceleration structure view");
            return;
        }
        asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asInfo.pNext = nullptr;
        asInfo.accelerationStructureCount = 1;
        asInfo.pAccelerationStructures = &asView->_target->_accelerationStructure;
        writeDesc.pNext = &asInfo;
    } else {
        RADRAY_ERR_LOG("vk unsupported resource view: {}", tag);
        return;
    }
    _device->_ftb.vkUpdateDescriptorSets(
        _device->_device,
        1,
        &writeDesc,
        0,
        nullptr);
}

DescriptorSetAllocatorVulkan::DescriptorSetAllocatorVulkan(
    DeviceVulkan* device,
    uint32_t keepFreePages,
    std::optional<vector<VkDescriptorPoolSize>> specPoolSize) noexcept
    : _device(device),
      _specPoolSize(std::move(specPoolSize)),
      _keepFreePages(keepFreePages) {}

DescriptorSetAllocatorVulkan::~DescriptorSetAllocatorVulkan() noexcept = default;

std::optional<DescriptorSetAllocatorVulkan::Allocation> DescriptorSetAllocatorVulkan::Allocate(DescriptorSetLayoutVulkan* layout) noexcept {
    if (_pages.empty()) {
        if (!NewPage()) {
            return std::nullopt;
        }
    }
    auto tryAllocFrom = [&](size_t pageIndex) -> VkDescriptorSet {
        auto* page = _pages[pageIndex].get();
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.pNext = nullptr;
        allocInfo.descriptorPool = page->_pool;
        allocInfo.descriptorSetCount = 1;
        VkDescriptorSetLayout l = layout->_layout;
        allocInfo.pSetLayouts = &l;
        VkDescriptorSet set = VK_NULL_HANDLE;
        auto vr = _device->_ftb.vkAllocateDescriptorSets(_device->_device, &allocInfo, &set);
        if (vr == VK_SUCCESS) {
            page->_liveCount += 1;
            _hintPage = pageIndex;
            return set;
        }
        if (vr == VK_ERROR_OUT_OF_POOL_MEMORY || vr == VK_ERROR_FRAGMENTED_POOL) {
            return VK_NULL_HANDLE;
        }
        RADRAY_ERR_LOG("vkAllocateDescriptorSets failed: {}", vr);
        return VK_NULL_HANDLE;
    };
    const size_t start = _hintPage < _pages.size() ? _hintPage : 0;
    for (size_t i = 0; i < _pages.size(); i++) {
        size_t idx = (start + i) % _pages.size();
        VkDescriptorSet set = tryAllocFrom(idx);
        if (set != VK_NULL_HANDLE) {
            return std::make_optional(Allocation{_pages[idx].get(), set});
        }
    }
    auto* newPage = NewPage();
    if (!newPage) {
        return std::nullopt;
    }
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.pNext = nullptr;
    allocInfo.descriptorPool = newPage->_pool;
    allocInfo.descriptorSetCount = 1;
    VkDescriptorSetLayout l = layout->_layout;
    allocInfo.pSetLayouts = &l;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (auto vr = _device->_ftb.vkAllocateDescriptorSets(_device->_device, &allocInfo, &set);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkAllocateDescriptorSets failed: {}", vr);
        return std::nullopt;
    }
    newPage->_liveCount += 1;
    _hintPage = _pages.size() - 1;
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
    if (allocation.Pool->_liveCount == 0) {
        TryReleaseFreePages();
    }
}

DescriptorPoolVulkan* DescriptorSetAllocatorVulkan::NewPage() noexcept {
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
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1024});
        poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1024});
        if (_device->_extFeatures.feature13.inlineUniformBlock) {
            poolSizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT, 1024});
            pInlineInfo = &inlineInfo;
            inlineInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO;
            inlineInfo.pNext = nullptr;
            inlineInfo.maxInlineUniformBlockBindings = 1024;
        }
    }
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.pNext = pInlineInfo;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = 1024;
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
      _size(desc.Size),
      _slotType(desc.SlotType),
      _name(desc.Name) {}

BindlessArrayVulkan::~BindlessArrayVulkan() noexcept {
    this->DestroyImpl();
}

bool BindlessArrayVulkan::IsValid() const noexcept {
    return _device != nullptr && (_bufferAlloc.IsValid() || _tex2dAlloc.IsValid() || _tex3dAlloc.IsValid());
}

void BindlessArrayVulkan::Destroy() noexcept {
    this->DestroyImpl();
}

void BindlessArrayVulkan::DestroyImpl() noexcept {
    if (_device == nullptr) {
        return;
    }
    if (_bufferAlloc.IsValid()) {
        _device->_bdlsBuffer->Destroy(_bufferAlloc);
        _bufferAlloc = BindlessDescAllocator::Allocation::Invalid();
    }
    if (_tex2dAlloc.IsValid()) {
        _device->_bdlsTex2d->Destroy(_tex2dAlloc);
        _tex2dAlloc = BindlessDescAllocator::Allocation::Invalid();
    }
    if (_tex3dAlloc.IsValid()) {
        _device->_bdlsTex3d->Destroy(_tex3dAlloc);
        _tex3dAlloc = BindlessDescAllocator::Allocation::Invalid();
    }
}

void BindlessArrayVulkan::SetBuffer(uint32_t slot, BufferView* bufView) noexcept {
    if (_slotType != BindlessSlotType::BufferOnly && _slotType != BindlessSlotType::Multiple) {
        RADRAY_ERR_LOG("vk bindless array does not support buffer slots");
        return;
    }
    if (slot >= _size) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _size, slot);
        return;
    }
    if (!bufView) {
        RADRAY_ERR_LOG("vk bindless array buffer view is null");
        return;
    }
    auto view = static_cast<SimulateBufferViewVulkan*>(bufView);
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = view->_buffer->_buffer;
    bufInfo.offset = view->_range.Offset;
    bufInfo.range = view->_range.Size;
    VkWriteDescriptorSet writeDesc{};
    writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDesc.pNext = nullptr;
    writeDesc.dstSet = _bufferAlloc.Set;
    writeDesc.dstBinding = 0;
    writeDesc.dstArrayElement = static_cast<uint32_t>(_bufferAlloc.Range.Start + slot);
    writeDesc.descriptorCount = 1;
    writeDesc.descriptorType = _bufferAlloc.Type;
    writeDesc.pBufferInfo = &bufInfo;
    writeDesc.pImageInfo = nullptr;
    writeDesc.pTexelBufferView = nullptr;
    _device->_ftb.vkUpdateDescriptorSets(
        _device->_device,
        1,
        &writeDesc,
        0,
        nullptr);
}

void BindlessArrayVulkan::SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept {
    RADRAY_UNUSED(sampler);
    if (_slotType == BindlessSlotType::BufferOnly) {
        RADRAY_ERR_LOG("vk bindless array does not support texture slots");
        return;
    }
    if (slot >= _size) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _size, slot);
        return;
    }
    auto view = CastVkObject(texView);
    auto dim = view->_mdesc.Dim;
    if (dim != TextureDimension::Dim2D && dim != TextureDimension::Dim3D) {
        RADRAY_ERR_LOG("vk bindless array only support texture 2D/3D");
        return;
    }
    BindlessDescAllocator::Allocation* alloc = nullptr;
    if (dim == TextureDimension::Dim2D) {
        if (_slotType == BindlessSlotType::Texture3DOnly) {
            RADRAY_ERR_LOG("vk bindless array does not support texture slots");
            return;
        }
        alloc = &_tex2dAlloc;
    } else {
        if (_slotType == BindlessSlotType::Texture2DOnly) {
            RADRAY_ERR_LOG("vk bindless array does not support texture slots");
            return;
        }
        alloc = &_tex3dAlloc;
    }
    if (!alloc->IsValid()) {
        RADRAY_ERR_LOG("vk bindless array texture allocator is invalid");
        return;
    }
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = VK_NULL_HANDLE;
    imgInfo.imageView = view->_imageView;
    imgInfo.imageLayout = TextureViewUsageToLayout(view->_mdesc.Usage);
    VkWriteDescriptorSet writeDesc{};
    writeDesc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDesc.pNext = nullptr;
    writeDesc.dstSet = alloc->Set;
    writeDesc.dstBinding = 0;
    writeDesc.dstArrayElement = static_cast<uint32_t>(alloc->Range.Start + slot);
    writeDesc.descriptorCount = 1;
    writeDesc.descriptorType = alloc->Type;
    writeDesc.pBufferInfo = nullptr;
    writeDesc.pImageInfo = &imgInfo;
    writeDesc.pTexelBufferView = nullptr;
    _device->_ftb.vkUpdateDescriptorSets(
        _device->_device,
        1,
        &writeDesc,
        0,
        nullptr);
}

}  // namespace radray::render::vulkan
