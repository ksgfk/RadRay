#pragma once

#include <array>

#include "vulkan_common.h"
#include "vulkan_helper.h"

namespace radray::render::vulkan {

class InstanceVulkan;
class VMA;
class DeviceVulkan;
class QueueVulkan;
class CommandPoolVulkan;
class CommandBufferVulkan;
class FenceVulkan;
class SemaphoreVulkan;
class TimelineSemaphoreVulkan;
class SurfaceVulkan;
class SwapChainVulkan;
class BufferVulkan;
class ImageVulkan;

struct QueueIndexInFamily {
    uint32_t Family;
    uint32_t IndexInFamily;
};

struct ExtFeaturesVulkan {
    VkPhysicalDeviceVulkan11Features feature11;
    VkPhysicalDeviceVulkan12Features feature12;
    VkPhysicalDeviceVulkan13Features feature13;
};

class InstanceVulkan final : public RenderBase {
public:
    InstanceVulkan(
        VkInstance instance,
        std::optional<VkAllocationCallbacks> allocCb,
        vector<string> exts,
        vector<string> layers) noexcept;

    ~InstanceVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    const VkAllocationCallbacks* GetAllocationCallbacks() const noexcept;

public:
    void DestroyImpl() noexcept;

    VkInstance _instance;
    std::optional<VkAllocationCallbacks> _allocCb;
    vector<string> _exts;
    vector<string> _layers;
};

class VMA final : public RenderBase {
public:
    explicit VMA(VmaAllocator alloc) noexcept;

    ~VMA() noexcept override;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    VmaAllocator _vma;
};

class DeviceVulkan final : public Device {
public:
    DeviceVulkan(
        InstanceVulkan* instance,
        VkPhysicalDevice physicalDevice,
        VkDevice device) noexcept;

    ~DeviceVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::Vulkan; }

    Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    Nullable<shared_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept override;

    Nullable<shared_ptr<Fence>> CreateFence(uint64_t initValue) noexcept override;

    Nullable<shared_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override;

    Nullable<shared_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override;

    Nullable<shared_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override;

public:
    Nullable<shared_ptr<FenceVulkan>> CreateLegacyFence(VkFenceCreateFlags flags) noexcept;

    Nullable<shared_ptr<SemaphoreVulkan>> CreateLegacySemaphore(VkSemaphoreCreateFlags flags) noexcept;

    Nullable<shared_ptr<TimelineSemaphoreVulkan>> CreateTimelineSemaphore(uint64_t initValue) noexcept;

    const VkAllocationCallbacks* GetAllocationCallbacks() const noexcept;

    void DestroyImpl() noexcept;

    InstanceVulkan* _instance;
    VkPhysicalDevice _physicalDevice;
    VkDevice _device;
    std::unique_ptr<VMA> _vma;
    std::array<vector<unique_ptr<QueueVulkan>>, (size_t)QueueType::MAX_COUNT> _queues;
    DeviceFuncTable _ftb;
    VkPhysicalDeviceFeatures _feature;
    ExtFeaturesVulkan _extFeatures;
};

class QueueVulkan final : public CommandQueue {
public:
    QueueVulkan(
        DeviceVulkan* device,
        VkQueue queue,
        QueueIndexInFamily family,
        QueueType type) noexcept;

    ~QueueVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override;

    void Wait() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkQueue _queue;
    QueueIndexInFamily _family;
    QueueType _type;
    struct {
        shared_ptr<FenceVulkan> fence;
        shared_ptr<SemaphoreVulkan> imageAvailableSemaphore;
        shared_ptr<SemaphoreVulkan> renderFinishedSemaphore;
    } _swapchainSync;
};

class CommandPoolVulkan final : public RenderBase {
public:
    CommandPoolVulkan(
        DeviceVulkan* device,
        VkCommandPool cmdPool) noexcept;

    ~CommandPoolVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Reset() const noexcept;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkCommandPool _cmdPool;
};

class CommandBufferVulkan final : public CommandBuffer {
public:
    CommandBufferVulkan(
        DeviceVulkan* device,
        QueueVulkan* queue,
        unique_ptr<CommandPoolVulkan> cmdPool,
        VkCommandBuffer cmdBuffer) noexcept;

    ~CommandBufferVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Begin() noexcept override;

    void End() noexcept override;

    void ResourceBarrier(std::span<BarrierBufferDescriptor> buffers, std::span<BarrierTextureDescriptor> textures) noexcept override;

    unique_ptr<CommandEncoder> BeginRenderPass(const RenderPassDescriptor& desc) noexcept override;

    void EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    QueueVulkan* _queue;
    unique_ptr<CommandPoolVulkan> _cmdPool;
    VkCommandBuffer _cmdBuffer;
};

class FenceVulkan final : public RenderBase {
public:
    FenceVulkan(
        DeviceVulkan* device,
        VkFence fence) noexcept;

    ~FenceVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkFence _fence;
};

class SemaphoreVulkan final : public RenderBase {
public:
    SemaphoreVulkan(
        DeviceVulkan* device,
        VkSemaphore semaphore) noexcept;

    ~SemaphoreVulkan() noexcept;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkSemaphore _semaphore;
};

class TimelineSemaphoreVulkan final : public Fence {
public:
    TimelineSemaphoreVulkan(
        DeviceVulkan* device,
        VkSemaphore semaphore) noexcept;

    ~TimelineSemaphoreVulkan() noexcept;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    uint64_t GetCompletedValue() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkSemaphore _semaphore;
};

class SurfaceVulkan final : public RenderBase {
public:
    SurfaceVulkan(
        DeviceVulkan* device,
        VkSurfaceKHR surface) noexcept;

    ~SurfaceVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkSurfaceKHR _surface;
};

class SwapChainVulkan final : public SwapChain {
public:
    SwapChainVulkan(
        DeviceVulkan* device,
        QueueVulkan* queue,
        unique_ptr<SurfaceVulkan> surface,
        VkSwapchainKHR swapchain) noexcept;

    ~SwapChainVulkan() noexcept;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Nullable<Texture> AcquireNext() noexcept override;

    void Present() noexcept override;

    Nullable<Texture> GetCurrentBackBuffer() const noexcept override;

    uint32_t GetCurrentBackBufferIndex() const noexcept override;

    uint32_t GetBackBufferCount() const noexcept override;

public:
    void DestroyImpl() noexcept;

    class Frame {
    public:
        unique_ptr<ImageVulkan> image;
        shared_ptr<FenceVulkan> fence;
        shared_ptr<SemaphoreVulkan> imageAvailableSemaphore;
        shared_ptr<SemaphoreVulkan> renderFinishedSemaphore;
        shared_ptr<CommandBufferVulkan> internalCmdBuffer;
    };

    DeviceVulkan* _device;
    QueueVulkan* _queue;
    unique_ptr<SurfaceVulkan> _surface;
    VkSwapchainKHR _swapchain;
    vector<Frame> _frames;
    uint32_t _currentTextureIndex{0};
    uint32_t _currentFrameIndex{0};
};

class BufferVulkan final : public Buffer {
public:
    BufferVulkan(
        DeviceVulkan* device,
        VkBuffer buffer,
        VmaAllocation allocation,
        VmaAllocationInfo allocInfo) noexcept;

    ~BufferVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkBuffer _buffer;
    VmaAllocation _allocation;
    VmaAllocationInfo _allocInfo;
};

class ImageVulkan final : public Texture {
public:
    ImageVulkan(
        DeviceVulkan* device,
        VkImage image,
        VmaAllocation allocation,
        VmaAllocationInfo allocInfo) noexcept;

    ~ImageVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void DangerousDestroy() noexcept;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkImage _image;
    VmaAllocation _allocation;
    VmaAllocationInfo _allocInfo;
    VkFormat _rawFormat{VK_FORMAT_UNDEFINED};
};

bool GlobalInitVulkan(const VulkanBackendInitDescriptor& desc);

void GlobalTerminateVulkan();

Nullable<shared_ptr<DeviceVulkan>> CreateDeviceVulkan(const VulkanDeviceDescriptor& desc);

}  // namespace radray::render::vulkan
