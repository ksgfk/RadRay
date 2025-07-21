#pragma once

#include "vulkan_common.h"
#include "vulkan_helper.h"

namespace radray::render::vulkan {

class InstanceVulkan;
class DeviceVulkan;

struct QueueIndexInFamily {
    uint32_t Family;
    uint32_t IndexInFamily;
};

class DeviceVulkan final : public Device {
public:
    InstanceVulkan* _instance = nullptr;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkDevice _device = VK_NULL_HANDLE;
    VmaAllocator _alloc = VK_NULL_HANDLE;
    // std::array<vector<unique_ptr<QueueVulkan>>, (size_t)QueueType::MAX_COUNT> _queues;
    DeviceFuncTable _ftb;

public:
    ~DeviceVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::Vulkan; }

    Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    Nullable<shared_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept override;

    Nullable<shared_ptr<Fence>> CreateFence() noexcept override;

    Nullable<shared_ptr<Semaphore>> CreateGpuSemaphore() noexcept override;

    Nullable<shared_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override;

public:
    const VkAllocationCallbacks* GetAllocationCallbacks() const noexcept;

    void DestroyImpl() noexcept;
};

bool GlobalInitVulkan(const VulkanBackendInitDescriptor& desc);

void GlobalTerminateVulkan();

Nullable<shared_ptr<DeviceVulkan>> CreateDeviceVulkan(const VulkanDeviceDescriptor& desc);

}  // namespace radray::render::vulkan
