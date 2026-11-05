#include "external_resource_vulkan.h"

#include <limits>

#include <radray/render/backend/vulkan_impl.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
namespace radray::render::vulkan {
namespace {
ExternalError Fail(std::string_view op, ExternalError error, std::string_view parameters = {}) noexcept {
    return detail::LogExternalError(op, RenderBackend::Vulkan, error, parameters);
}
ExternalError NativeError(VkResult r, std::string_view operation) noexcept { return {fmt::format("{} failed: {}", operation, r), ExternalNativeApi::Vulkan, r}; }
VkExternalMemoryHandleTypeFlagBits MemoryHandleType(ExternalMemoryHandleType type) noexcept {
    switch (type) {
        case ExternalMemoryHandleType::D3D12Resource: return VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        case ExternalMemoryHandleType::OpaqueWin32: return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        default: return static_cast<VkExternalMemoryHandleTypeFlagBits>(0);
    }
}
VkExternalSemaphoreHandleTypeFlagBits FenceHandleType(ExternalFenceHandleType type) noexcept {
    switch (type) {
        case ExternalFenceHandleType::D3D12Fence: return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        case ExternalFenceHandleType::OpaqueWin32: return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        default: return static_cast<VkExternalSemaphoreHandleTypeFlagBits>(0);
    }
}
ExternalVulkanIdentity VulkanIdentity(DeviceVulkan* device) noexcept {
    ExternalVulkanIdentity result;
    const auto& id = device->_extProperties.Vulkan11;
    std::copy_n(id.deviceUUID, result.DeviceUuid.size(), result.DeviceUuid.begin());
    std::copy_n(id.driverUUID, result.DriverUuid.size(), result.DriverUuid.begin());
    return result;
}
std::optional<ExternalError> AllocateOpaque(DeviceVulkan* device, VkMemoryRequirements requirements,
                                            const ExternalAllocationInfo& exported, const ExternalVulkanMemoryInfo& memory,
                                            void* import, VkBuffer buffer, VkImage image,
                                            VmaAllocation* allocation, VmaAllocationInfo* info) noexcept {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(device->_physicalDevice, &properties);
    const auto index = memory.MemoryTypeIndex;
    if (index >= properties.memoryTypeCount || !(requirements.memoryTypeBits & (uint32_t{1} << index)) ||
        !(properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        return ExternalError{"Exported memory type index is incompatible with the importing resource"};
    if (exported.Size < requirements.size) return ExternalError{"Exported allocation size is smaller than the importing resource requirements"};
    requirements.size = exported.Size;
    requirements.memoryTypeBits = uint32_t{1} << index;
    VmaAllocationCreateInfo create{};
    create.usage = VMA_MEMORY_USAGE_UNKNOWN;
    create.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    create.memoryTypeBits = requirements.memoryTypeBits;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, import, image, buffer};
    const auto r = vmaAllocateDedicatedMemory(device->_vma->_vma, &requirements, &create, &dedicated, allocation, info);
    if (r != VK_SUCCESS) return NativeError(r, "vmaAllocateDedicatedMemory (opaque import)");
    return std::nullopt;
}
VkBufferUsageFlags BufferUsage(BufferUses u) noexcept {
    VkBufferUsageFlags result = 0;
    if (u.HasFlag(BufferUse::CopySource)) result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (u.HasFlag(BufferUse::CopyDestination)) result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (u.HasFlag(BufferUse::Index)) result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (u.HasFlag(BufferUse::Vertex)) result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (u.HasFlag(BufferUse::CBuffer)) result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (u.HasFlag(BufferUse::Resource)) result |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (u.HasFlag(BufferUse::UnorderedAccess)) result |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (u.HasFlag(BufferUse::Indirect)) result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return result;
}
ExternalSupport DirectionSupport(bool exportable, bool importable, std::string_view handleType) noexcept {
    ExternalSupport result{exportable, importable, {}};
    if (!exportable || !importable)
        result.Reason.Message = fmt::format("Vulkan does not support {} {} for the requested resource", handleType, !exportable && !importable ? "export or import" : !exportable ? "export"
                                                                                                                                                                                  : "import");
    return result;
}
ExternalSupport MemorySupport(VkExternalMemoryFeatureFlags flags, ExternalMemoryHandleType type) noexcept {
    return DirectionSupport((flags & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0, (flags & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0, detail::ExternalHandleName(type));
}
ExternalResult<unique_ptr<BufferVulkan>> MakeBuffer(DeviceVulkan* device, const BufferDescriptor& desc, uint64_t nativeSize, ExternalMemoryHandleType type, void* next, uint32_t types, Nullable<const ExternalBufferInfo*> opaque = nullptr) noexcept {
    const VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr, VkExternalMemoryHandleTypeFlags(MemoryHandleType(type))};
    VkBufferCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create.pNext = &external;
    create.size = nativeSize;
    create.usage = BufferUsage(desc.Usage);
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    alloc.memoryTypeBits = types;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    if (opaque) {
        auto r = device->_ftb.vkCreateBuffer(device->_device, &create, device->GetAllocationCallbacks(), &buffer);
        if (r != VK_SUCCESS) return NativeError(r, "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        device->_ftb.vkGetBufferMemoryRequirements(device->_device, buffer, &requirements);
        const auto error = AllocateOpaque(device, requirements, opaque.Get()->Allocation, *opaque.Get()->VulkanMemory,
                                          next, buffer, VK_NULL_HANDLE, &allocation, &info);
        if (error) {
            device->_ftb.vkDestroyBuffer(device->_device, buffer, device->GetAllocationCallbacks());
            return *error;
        }
        r = vmaBindBufferMemory(device->_vma->_vma, allocation, buffer);
        if (r != VK_SUCCESS) {
            vmaDestroyBuffer(device->_vma->_vma, buffer, allocation);
            return NativeError(r, "vmaBindBufferMemory");
        }
    } else {
        const auto r = vmaCreateDedicatedBuffer(device->_vma->_vma, &create, &alloc, next, &buffer, &allocation, &info);
        if (r != VK_SUCCESS) return NativeError(r, "vmaCreateDedicatedBuffer");
    }
    auto result = make_unique<BufferVulkan>(device, buffer, allocation, info);
    result->_reqSize = nativeSize;
    result->_reqSizeLogical = desc.Size;
    result->_memory = MemoryType::Device;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints | ResourceHint::Dedicated;
    return result;
}
ExternalResult<unique_ptr<ImageVulkan>> MakeImage(DeviceVulkan* device, const TextureDescriptor& desc, const ExternalTextureDescriptor& options, void* next, uint32_t types, Nullable<const ExternalTextureInfo*> imported = nullptr) noexcept {
    const VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VkExternalMemoryHandleTypeFlags(MemoryHandleType(options.HandleType))};
    auto create = MakeTextureImageCreateInfo(desc, options.AllowFormatReinterpretation);
    create.pNext = &external;
    if (imported && imported->SourceBackend == RenderBackend::Vulkan) {
        create.format = static_cast<VkFormat>(imported->NativeFormat);
        create.flags = imported->NativeFlags;
        create.usage = imported->NativeUsage;
        create.tiling = static_cast<VkImageTiling>(imported->NativeTiling);
    }
    const auto opaque = options.HandleType == ExternalMemoryHandleType::OpaqueWin32 ? imported : nullptr;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    alloc.memoryTypeBits = types;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    if (opaque) {
        auto r = device->_ftb.vkCreateImage(device->_device, &create, device->GetAllocationCallbacks(), &image);
        if (r != VK_SUCCESS) return NativeError(r, "vkCreateImage");
        VkMemoryRequirements requirements{};
        device->_ftb.vkGetImageMemoryRequirements(device->_device, image, &requirements);
        const auto error = AllocateOpaque(device, requirements, opaque.Get()->Allocation, *opaque.Get()->VulkanMemory,
                                          next, VK_NULL_HANDLE, image, &allocation, &info);
        if (error) {
            device->_ftb.vkDestroyImage(device->_device, image, device->GetAllocationCallbacks());
            return *error;
        }
        r = vmaBindImageMemory(device->_vma->_vma, allocation, image);
        if (r != VK_SUCCESS) {
            vmaDestroyImage(device->_vma->_vma, image, allocation);
            return NativeError(r, "vmaBindImageMemory");
        }
    } else {
        const auto r = vmaCreateDedicatedImage(device->_vma->_vma, &create, &alloc, next, &image, &allocation, &info);
        if (r != VK_SUCCESS) return NativeError(r, "vmaCreateDedicatedImage");
    }
    auto result = make_unique<ImageVulkan>(device, image, allocation, info);
    result->_rawFormat = create.format;
    result->_dim = desc.Dim;
    result->_width = desc.Width;
    result->_height = desc.Height;
    result->_depthOrArraySize = desc.DepthOrArraySize;
    result->_mipLevels = desc.MipLevels;
    result->_sampleCount = desc.SampleCount;
    result->_format = desc.Format;
    result->_memory = MemoryType::Device;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints | ResourceHint::Dedicated;
    result->_createFlags = create.flags;
    result->_nativeUsage = create.usage;
    return result;
}
ExternalResult<OwnedExternalHandle> ExportMemory(DeviceVulkan* device, VkDeviceMemory memory, ExternalMemoryHandleType type) noexcept {
    const VkMemoryGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, memory, MemoryHandleType(type)};
    HANDLE handle = nullptr;
    const auto r = device->_ftb.vkGetMemoryWin32HandleKHR(device->_device, &info, &handle);
    if (r != VK_SUCCESS) return NativeError(r, "vkGetMemoryWin32HandleKHR");
    return OwnedExternalHandle{reinterpret_cast<uintptr_t>(handle)};
}
ExternalResult<uint32_t> ImportMemoryTypes(DeviceVulkan* device, BorrowedExternalHandle handle) noexcept {
    VkMemoryWin32HandlePropertiesKHR properties{};
    properties.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    const auto r = device->_ftb.vkGetMemoryWin32HandlePropertiesKHR(device->_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, reinterpret_cast<HANDLE>(handle.Value), &properties);
    if (r != VK_SUCCESS) return NativeError(r, "vkGetMemoryWin32HandlePropertiesKHR");
    if (properties.memoryTypeBits == 0) return ExternalError{"External memory handle has no compatible Vulkan memory types"};
    return properties.memoryTypeBits;
}
template <class Native, class Output, class Input>
ExternalResult<Output> ExportObject(Input* input, std::string_view operation) noexcept {
    auto* object = static_cast<Native*>(input);
    string parameters;
    if constexpr (std::is_same_v<Input, Buffer>)
        parameters = fmt::format("size={} usage={}", object->GetDesc().Size, object->GetDesc().Usage);
    else if constexpr (std::is_same_v<Input, Texture>) {
        const auto d = object->GetDesc();
        parameters = fmt::format("dim={} {}x{} depthOrLayers={} mips={} samples={} format={} usage={} reinterpret={}", d.Dim, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.SampleCount, d.Format, d.Usage, object->_externalInfo->AllowFormatReinterpretation);
    } else
        parameters = fmt::format("lastLocalSignal={}", object->GetLastSignaledValue());
    parameters += fmt::format(" handleType={}", radray::render::detail::ExternalHandleName(object->_externalInfo->HandleType));
    auto handle = object->_externalHandle.Duplicate();
    if (auto* error = std::get_if<ExternalError>(&handle)) return Fail(operation, *error, parameters);
    return Output{*object->_externalInfo, std::get<OwnedExternalHandle>(std::move(handle))};
}
}  // namespace

ExternalResourceVulkan::ExternalResourceVulkan(DeviceVulkan* device) noexcept : _device(device) {}
ExternalResult<ExternalDeviceIdentity> ExternalResourceVulkan::GetDeviceIdentity() const noexcept {
    ExternalDeviceIdentity result;
    const auto& id = _device->_extProperties.Vulkan11;
    if (!id.deviceLUIDValid) return ExternalError{"Vulkan device does not provide a valid LUID"};
    if (id.deviceNodeMask != 1) return ExternalError{fmt::format("External resources require Vulkan node mask 1; device reports {:#x}", id.deviceNodeMask)};
    std::copy_n(id.deviceLUID, result.Luid.size(), result.Luid.begin());
    result.NodeMask = id.deviceNodeMask;
    return result;
}
ExternalSupport ExternalResourceVulkan::QueryBufferSupport(const BufferDescriptor& desc, const ExternalResourceDescriptor& external) const noexcept {
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return {false, false, *error};
    if (!_device->_externalMemoryWin32) return {false, false, {"Vulkan Win32 external memory extension or entry points are unavailable"}};
    if (_device->GetCapabilities().Limits.MaxBufferSize && desc.Size > _device->GetCapabilities().Limits.MaxBufferSize) return {false, false, {"Buffer size exceeds the Vulkan device limit"}};
    const VkPhysicalDeviceExternalBufferInfo input{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, nullptr, 0, BufferUsage(desc.Usage), MemoryHandleType(external.HandleType)};
    VkExternalBufferProperties output{};
    output.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(_device->_physicalDevice, &input, &output);
    return MemorySupport(output.externalMemoryProperties.externalMemoryFeatures, external.HandleType);
}
ExternalSupport ExternalResourceVulkan::QueryTextureSupport(const TextureDescriptor& desc, const ExternalTextureDescriptor& external) const noexcept {
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return {false, false, *error};
    if (!_device->_externalMemoryWin32) return {false, false, {"Vulkan Win32 external memory extension or entry points are unavailable"}};
    const VkPhysicalDeviceExternalImageFormatInfo externalInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, nullptr, MemoryHandleType(external.HandleType)};
    const auto native = MakeTextureImageCreateInfo(desc, external.AllowFormatReinterpretation);
    if ((native.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
        (native.extent.width > _device->_properties.limits.maxFramebufferWidth || native.extent.height > _device->_properties.limits.maxFramebufferHeight))
        return {false, false, {"Texture attachment dimensions exceed the Vulkan framebuffer limits"}};
    const VkPhysicalDeviceImageFormatInfo2 input{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &externalInfo, native.format, native.imageType, native.tiling, native.usage, native.flags};
    VkExternalImageFormatProperties externalProperties{};
    externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 output{};
    output.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    output.pNext = &externalProperties;
    const auto r = vkGetPhysicalDeviceImageFormatProperties2(_device->_physicalDevice, &input, &output);
    if (r != VK_SUCCESS) return {false, false, NativeError(r, fmt::format("vkGetPhysicalDeviceImageFormatProperties2 for {}", detail::ExternalHandleName(external.HandleType)))};
    const auto& p = output.imageFormatProperties;
    if (native.extent.width > p.maxExtent.width || native.extent.height > p.maxExtent.height || native.extent.depth > p.maxExtent.depth ||
        native.mipLevels > p.maxMipLevels || native.arrayLayers > p.maxArrayLayers || !(p.sampleCounts & native.samples)) return {false, false, {"Texture dimensions, mip count, layer count or sample count exceed the Vulkan external image limits"}};
    return MemorySupport(externalProperties.externalMemoryProperties.externalMemoryFeatures, external.HandleType);
}
ExternalSupport ExternalResourceVulkan::QueryFenceSupport(ExternalFenceHandleType type) const noexcept {
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return {false, false, *error};
    if (!_device->_externalSemaphoreWin32) return {false, false, {"Vulkan Win32 external semaphore extension or entry points are unavailable"}};
    const VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    const VkPhysicalDeviceExternalSemaphoreInfo input{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &timeline, FenceHandleType(type)};
    VkExternalSemaphoreProperties output{};
    output.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    vkGetPhysicalDeviceExternalSemaphoreProperties(_device->_physicalDevice, &input, &output);
    return DirectionSupport((output.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0, (output.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0, fmt::format("timeline {}", detail::ExternalHandleName(type)));
}
ExternalResult<unique_ptr<Buffer>> ExternalResourceVulkan::CreateExportableBuffer(const BufferDescriptor& desc, const ExternalResourceDescriptor& external) noexcept {
    const auto params = fmt::format("handleType={} size={} usage={}", radray::render::detail::ExternalHandleName(external.HandleType), desc.Size, desc.Usage);
    const auto support = QueryBufferSupport(desc, external);
    if (!support.CanCreateExportable) return Fail("CreateExportableBuffer", support.Reason, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("CreateExportableBuffer", *error, params);
    uint64_t size = desc.Size;
    if (desc.Usage.HasFlag(BufferUse::CBuffer)) {
        const uint64_t alignment = std::max<uint64_t>(256, _device->GetCapabilities().Detail.CBufferAlignment);
        if (size > std::numeric_limits<uint64_t>::max() - (alignment - 1)) return Fail("CreateExportableBuffer", {"Aligned buffer size would overflow uint64_t"}, params);
        size = (size + alignment - 1) / alignment * alignment;
    }
    VkExportMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VkExternalMemoryHandleTypeFlags(MemoryHandleType(external.HandleType))};
    auto created = MakeBuffer(_device, desc, size, external.HandleType, &allocation, std::numeric_limits<uint32_t>::max());
    if (auto* error = std::get_if<ExternalError>(&created)) return Fail("CreateExportableBuffer", *error, params);
    auto result = std::get<unique_ptr<BufferVulkan>>(std::move(created));
    auto handle = ExportMemory(_device, result->_allocInfo.deviceMemory, external.HandleType);
    if (auto* error = std::get_if<ExternalError>(&handle)) return Fail("CreateExportableBuffer", *error, params);
    result->_externalInfo = ExternalBufferInfo{external.HandleType, std::get<ExternalDeviceIdentity>(identity), RenderBackend::Vulkan, result->GetDesc(), size, 0, BufferUsage(desc.Usage), {result->_allocInfo.size, 0, true}, std::nullopt};
    if (external.HandleType == ExternalMemoryHandleType::OpaqueWin32)
        result->_externalInfo->VulkanMemory = ExternalVulkanMemoryInfo{VulkanIdentity(_device), result->_allocInfo.memoryType};
    result->_externalHandle = std::get<OwnedExternalHandle>(std::move(handle));
    return unique_ptr<Buffer>{std::move(result)};
}
ExternalResult<unique_ptr<Texture>> ExternalResourceVulkan::CreateExportableTexture(const TextureDescriptor& desc, const ExternalTextureDescriptor& external) noexcept {
    const auto params = fmt::format("handleType={} dim={} {}x{} depthOrLayers={} mips={} samples={} format={} usage={} reinterpret={}", radray::render::detail::ExternalHandleName(external.HandleType), desc.Dim, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, desc.Format, desc.Usage, external.AllowFormatReinterpretation);
    const auto support = QueryTextureSupport(desc, external);
    if (!support.CanCreateExportable) return Fail("CreateExportableTexture", support.Reason, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("CreateExportableTexture", *error, params);
    VkExportMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VkExternalMemoryHandleTypeFlags(MemoryHandleType(external.HandleType))};
    auto created = MakeImage(_device, desc, external, &allocation, std::numeric_limits<uint32_t>::max());
    if (auto* error = std::get_if<ExternalError>(&created)) return Fail("CreateExportableTexture", *error, params);
    auto result = std::get<unique_ptr<ImageVulkan>>(std::move(created));
    auto handle = ExportMemory(_device, result->_allocInfo.deviceMemory, external.HandleType);
    if (auto* error = std::get_if<ExternalError>(&handle)) return Fail("CreateExportableTexture", *error, params);
    result->_externalInfo = ExternalTextureInfo{external.HandleType, std::get<ExternalDeviceIdentity>(identity), RenderBackend::Vulkan, result->GetDesc(), uint32_t(result->_rawFormat), result->_createFlags, result->_nativeUsage, uint32_t(VK_IMAGE_TILING_OPTIMAL), {result->_allocInfo.size, 0, true}, std::nullopt};
    if (external.HandleType == ExternalMemoryHandleType::OpaqueWin32)
        result->_externalInfo->VulkanMemory = ExternalVulkanMemoryInfo{VulkanIdentity(_device), result->_allocInfo.memoryType};
    result->_externalInfo->AllowFormatReinterpretation = external.AllowFormatReinterpretation;
    result->_externalHandle = std::get<OwnedExternalHandle>(std::move(handle));
    return unique_ptr<Texture>{std::move(result)};
}
ExternalResult<unique_ptr<Fence>> ExternalResourceVulkan::CreateExportableFence(const ExternalFenceDescriptor& desc) noexcept {
    const auto params = fmt::format("handleType={} initialValue={}", radray::render::detail::ExternalHandleName(desc.HandleType), desc.InitialValue);
    const auto support = QueryFenceSupport(desc.HandleType);
    if (!support.CanCreateExportable) return Fail("CreateExportableFence", support.Reason, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("CreateExportableFence", *error, params);
    const VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr, VkExternalSemaphoreHandleTypeFlags(FenceHandleType(desc.HandleType))};
    const VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &external, VK_SEMAPHORE_TYPE_TIMELINE, desc.InitialValue};
    const VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline, 0};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    auto r = _device->_ftb.vkCreateSemaphore(_device->_device, &create, _device->GetAllocationCallbacks(), &semaphore);
    if (r != VK_SUCCESS) return Fail("CreateExportableFence", NativeError(r, "vkCreateSemaphore"), params);
    auto owned = make_unique<TimelineSemaphoreVulkan>(_device, semaphore);
    const VkSemaphoreGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, semaphore, FenceHandleType(desc.HandleType)};
    HANDLE handle = nullptr;
    r = _device->_ftb.vkGetSemaphoreWin32HandleKHR(_device->_device, &get, &handle);
    if (r != VK_SUCCESS) return Fail("CreateExportableFence", NativeError(r, "vkGetSemaphoreWin32HandleKHR"), params);
    auto result = make_unique<FenceVulkan>(_device, std::move(owned));
    result->_fenceValue = 1;
    result->_externalInfo = ExternalFenceInfo{desc.HandleType, std::get<ExternalDeviceIdentity>(identity), std::nullopt};
    if (desc.HandleType == ExternalFenceHandleType::OpaqueWin32) result->_externalInfo->VulkanIdentity = VulkanIdentity(_device);
    result->_externalHandle = OwnedExternalHandle{reinterpret_cast<uintptr_t>(handle)};
    return unique_ptr<Fence>{std::move(result)};
}
ExternalResult<ExternalBufferExport> ExternalResourceVulkan::ExportBuffer(Buffer* b) noexcept { return ExportObject<BufferVulkan, ExternalBufferExport>(b, "ExportBuffer"); }
ExternalResult<ExternalTextureExport> ExternalResourceVulkan::ExportTexture(Texture* t) noexcept { return ExportObject<ImageVulkan, ExternalTextureExport>(t, "ExportTexture"); }
ExternalResult<ExternalFenceExport> ExternalResourceVulkan::ExportFence(Fence* f) noexcept { return ExportObject<FenceVulkan, ExternalFenceExport>(f, "ExportFence"); }
ExternalResult<unique_ptr<Buffer>> ExternalResourceVulkan::ImportBuffer(BorrowedExternalHandle handle, const ExternalBufferInfo& info) noexcept {
    const auto params = fmt::format("handleType={} size={} nativeSize={} usage={}", radray::render::detail::ExternalHandleName(info.HandleType), info.Descriptor.Size, info.NativeSize, info.Descriptor.Usage);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("ImportBuffer", *error, params);
    if (info.Device != std::get<ExternalDeviceIdentity>(identity)) return Fail("ImportBuffer", {"Exported device LUID or node mask does not match the importing device"}, params);
    if (info.HandleType == ExternalMemoryHandleType::OpaqueWin32 && info.VulkanMemory->Device != VulkanIdentity(_device))
        return Fail("ImportBuffer", {"Exported Vulkan device UUID or driver UUID does not match the importing device"}, params);
    const auto support = QueryBufferSupport(info.Descriptor, {info.HandleType});
    if (!support.CanImport) return Fail("ImportBuffer", support.Reason, params);
    uint32_t types = std::numeric_limits<uint32_t>::max();
    if (info.HandleType == ExternalMemoryHandleType::D3D12Resource) {
        auto queried = ImportMemoryTypes(_device, handle);
        if (auto* error = std::get_if<ExternalError>(&queried)) return Fail("ImportBuffer", *error, params);
        types = std::get<uint32_t>(queried);
    }
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, nullptr, MemoryHandleType(info.HandleType), reinterpret_cast<HANDLE>(handle.Value), nullptr};
    auto created = MakeBuffer(_device, info.Descriptor, info.NativeSize, info.HandleType, &import, types, info.HandleType == ExternalMemoryHandleType::OpaqueWin32 ? &info : nullptr);
    if (auto* error = std::get_if<ExternalError>(&created)) return Fail("ImportBuffer", *error, params);
    auto result = std::get<unique_ptr<BufferVulkan>>(std::move(created));
    result->_externalInfo = info;
    return unique_ptr<Buffer>{std::move(result)};
}
ExternalResult<unique_ptr<Texture>> ExternalResourceVulkan::ImportTexture(BorrowedExternalHandle handle, const ExternalTextureInfo& info) noexcept {
    const auto& desc = info.Descriptor;
    const auto params = fmt::format("handleType={} dim={} {}x{} depthOrLayers={} mips={} samples={} format={} usage={} reinterpret={}", radray::render::detail::ExternalHandleName(info.HandleType), desc.Dim, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, desc.Format, desc.Usage, info.AllowFormatReinterpretation);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("ImportTexture", *error, params);
    if (info.Device != std::get<ExternalDeviceIdentity>(identity)) return Fail("ImportTexture", {"Exported device LUID or node mask does not match the importing device"}, params);
    if (info.HandleType == ExternalMemoryHandleType::OpaqueWin32 && info.VulkanMemory->Device != VulkanIdentity(_device))
        return Fail("ImportTexture", {"Exported Vulkan device UUID or driver UUID does not match the importing device"}, params);
    const auto support = QueryTextureSupport(desc, {info.HandleType, info.AllowFormatReinterpretation});
    if (!support.CanImport) return Fail("ImportTexture", support.Reason, params);
    uint32_t types = std::numeric_limits<uint32_t>::max();
    if (info.HandleType == ExternalMemoryHandleType::D3D12Resource) {
        auto queried = ImportMemoryTypes(_device, handle);
        if (auto* error = std::get_if<ExternalError>(&queried)) return Fail("ImportTexture", *error, params);
        types = std::get<uint32_t>(queried);
    }
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, nullptr, MemoryHandleType(info.HandleType), reinterpret_cast<HANDLE>(handle.Value), nullptr};
    auto created = MakeImage(_device, desc, {info.HandleType, info.AllowFormatReinterpretation}, &import, types, &info);
    if (auto* error = std::get_if<ExternalError>(&created)) return Fail("ImportTexture", *error, params);
    auto result = std::get<unique_ptr<ImageVulkan>>(std::move(created));
    result->_externalInfo = info;
    return unique_ptr<Texture>{std::move(result)};
}
ExternalResult<unique_ptr<Fence>> ExternalResourceVulkan::ImportFence(BorrowedExternalHandle handle, const ExternalFenceInfo& info) noexcept {
    const auto params = fmt::format("handleType={} nodeMask={} handle={:#x}", radray::render::detail::ExternalHandleName(info.HandleType), info.Device.NodeMask, handle.Value);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("ImportFence", *error, params);
    if (info.Device != std::get<ExternalDeviceIdentity>(identity)) return Fail("ImportFence", {"Exported device LUID or node mask does not match the importing device"}, params);
    if (info.HandleType == ExternalFenceHandleType::OpaqueWin32 && *info.VulkanIdentity != VulkanIdentity(_device))
        return Fail("ImportFence", {"Exported Vulkan device UUID or driver UUID does not match the importing device"}, params);
    const auto support = QueryFenceSupport(info.HandleType);
    if (!support.CanImport) return Fail("ImportFence", support.Reason, params);
    const VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    const VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline, 0};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const auto created = _device->_ftb.vkCreateSemaphore(_device->_device, &create, _device->GetAllocationCallbacks(), &semaphore);
    if (created != VK_SUCCESS) return Fail("ImportFence", NativeError(created, "vkCreateSemaphore"), params);
    auto owned = make_unique<TimelineSemaphoreVulkan>(_device, semaphore);
    const VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR, nullptr, owned->_semaphore, 0, FenceHandleType(info.HandleType), reinterpret_cast<HANDLE>(handle.Value), nullptr};
    const auto r = _device->_ftb.vkImportSemaphoreWin32HandleKHR(_device->_device, &import);
    if (r != VK_SUCCESS) return Fail("ImportFence", NativeError(r, "vkImportSemaphoreWin32HandleKHR"), params);
    auto result = make_unique<FenceVulkan>(_device, std::move(owned));
    result->_externalInfo = info;
    return unique_ptr<Fence>{std::move(result)};
}
}  // namespace radray::render::vulkan
#endif
