#pragma once

#ifdef RADRAY_ENABLE_VULKAN

#include <array>

#include <radray/allocator.h>
#include <radray/render/backend/vulkan_helper.h>
#include <radray/render/rhi.h>

namespace radray::render::vulkan {

using DeviceFuncTable = VolkDeviceTable;

class InstanceVulkanImpl;
class VMA;
class DeviceVulkan;
class QueueVulkan;
class CommandPoolVulkan;
class CommandBufferVulkan;
class SimulateCommandEncoderVulkan;
class RenderPassVulkan;
class FrameBufferVulkan;
class LegacyFenceVulkan;
class LegacySemaphoreVulkan;
class FenceVulkan;
class TimelineSemaphoreVulkan;
class SurfaceVulkan;
class SwapChainVulkan;
class SwapChainSyncObjectVulkan;
class BufferVulkan;
class BufferViewVulkan;
class ImageVulkan;
class ImageViewVulkan;
class PipelineLayoutVulkan;
class ShaderParameterSetVulkan;
class GraphicsPipelineVulkan;
class ComputePipelineVulkan;
class ShaderModuleVulkan;
class SamplerVulkan;
class QueryPoolVulkan;

struct QueueIndexInFamily {
    uint32_t Family;
    uint32_t IndexInFamily;
};

struct ExtFeaturesVulkan {
    VkPhysicalDeviceVulkan11Features feature11;
    VkPhysicalDeviceVulkan12Features feature12;
    VkPhysicalDeviceVulkan13Features feature13;
};

struct ExtPropertiesVulkan {
    std::optional<VkPhysicalDeviceConservativeRasterizationPropertiesEXT> conservativeRasterization;
};

class InstanceVulkanImpl final : public InstanceVulkan {
public:
    InstanceVulkanImpl(
        VkInstance instance,
        std::optional<VkAllocationCallbacks> allocCb,
        vector<string> exts,
        vector<string> layers) noexcept;

    ~InstanceVulkanImpl() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    vector<VulkanPhysicalDeviceInfo> GetPhysicalDevices() const noexcept override;

    std::optional<uint32_t> SelectHighPerformancePhysicalDevice() const noexcept override;

    const VkAllocationCallbacks* GetAllocationCallbacks() const noexcept;

public:
    void DestroyImpl() noexcept;

    VkInstance _instance;
    std::optional<VkAllocationCallbacks> _allocCb;
    vector<string> _exts;
    vector<string> _layers;

    RenderLogCallback _logCallback{nullptr};
    void* _logUserData{nullptr};
    VkDebugUtilsMessengerEXT _debugMessenger{VK_NULL_HANDLE};
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

struct DescriptorSetLayoutCacheKeyVulkan {
    struct BindingKey {
        uint32_t Binding;
        VkDescriptorType Type;
        uint32_t Count;
        VkShaderStageFlags Stages;
        vector<VkSampler> ImmutableSamplers;

        friend bool operator==(const BindingKey&, const BindingKey&) noexcept = default;
    };

    vector<BindingKey> Bindings;

    friend bool operator==(
        const DescriptorSetLayoutCacheKeyVulkan&,
        const DescriptorSetLayoutCacheKeyVulkan&) noexcept = default;
};

}  // namespace radray::render::vulkan

namespace std {

template <>
struct hash<radray::render::vulkan::DescriptorSetLayoutCacheKeyVulkan> {
    size_t operator()(
        const radray::render::vulkan::DescriptorSetLayoutCacheKeyVulkan& key) const noexcept;
};

}  // namespace std

namespace radray::render::vulkan {

// Device-lifetime cache for descriptor set layouts.
class DescriptorSetLayoutCacheVulkan final {
public:
    explicit DescriptorSetLayoutCacheVulkan(DeviceVulkan* device) noexcept;

    ~DescriptorSetLayoutCacheVulkan() noexcept;

    VkDescriptorSetLayout GetOrCreate(
        std::span<const VkDescriptorSetLayoutBinding> bindings) noexcept;

    void Destroy() noexcept;

private:
    using Key = DescriptorSetLayoutCacheKeyVulkan;
    using BindingKey = Key::BindingKey;

    DeviceVulkan* _device;
    unordered_map<Key, VkDescriptorSetLayout> _layouts;
};

class DescriptorSetAllocatorVulkan final {
public:
    struct Page;

    struct Request {
        VkDescriptorSetLayout Layout{VK_NULL_HANDLE};
        std::span<const VkDescriptorPoolSize> DescriptorCounts;
    };

    struct Allocation {
        Page* PagePtr{nullptr};
        VkDescriptorPool Pool{VK_NULL_HANDLE};
        VkDescriptorSet Set{VK_NULL_HANDLE};

        static constexpr Allocation Invalid() noexcept { return {}; }

        constexpr bool IsValid() const noexcept {
            return PagePtr != nullptr &&
                   Pool != VK_NULL_HANDLE &&
                   Set != VK_NULL_HANDLE;
        }
    };

    explicit DescriptorSetAllocatorVulkan(DeviceVulkan* device) noexcept;

    ~DescriptorSetAllocatorVulkan() noexcept;

    std::optional<Allocation> Allocate(Request request) noexcept;

    void Destroy(Allocation allocation) noexcept;

    void Clear() noexcept;

private:
    DeviceVulkan* _device;
    vector<unique_ptr<Page>> _pages;
};

static_assert(
    is_allocator<
        DescriptorSetAllocatorVulkan,
        DescriptorSetAllocatorVulkan::Allocation,
        DescriptorSetAllocatorVulkan::Request>,
    "DescriptorSetAllocatorVulkan is not an allocator");

class DeviceVulkan final : public Device {
public:
    DeviceVulkan(
        InstanceVulkanImpl* instance,
        VkPhysicalDevice physicalDevice,
        VkDevice device) noexcept;

    ~DeviceVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    RenderBackend GetBackend() noexcept override { return RenderBackend::Vulkan; }

    DeviceDetail GetDetail() const noexcept override;

    Nullable<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept override;

    Nullable<unique_ptr<Fence>> CreateFence() noexcept override;

    Nullable<unique_ptr<QueryPool>> CreateQueryPool(const QueryPoolDescriptor& desc) noexcept override;

    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override;

    void FlushMappedRanges(std::span<const MappedBufferRange> ranges) noexcept override;

    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<RenderPass>> CreateRenderPass(const RenderPassDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Framebuffer>> CreateFramebuffer(const FramebufferDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override;

    Nullable<unique_ptr<PipelineLayout>> CreatePipelineLayout(const PipelineLayoutDescriptor& desc) noexcept override;

    Nullable<unique_ptr<ShaderParameterSet>> CreateShaderParameterSet(const ShaderParameterSetDescriptor& desc) noexcept override;

    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override;

    Nullable<Sampler*> GetOrCreateSampler(const SamplerDescriptor& desc) noexcept override;

public:
    Nullable<unique_ptr<LegacyFenceVulkan>> CreateLegacyFence(VkFenceCreateFlags flags) noexcept;

    Nullable<unique_ptr<LegacySemaphoreVulkan>> CreateLegacySemaphore(VkSemaphoreCreateFlags flags) noexcept;

    Nullable<unique_ptr<TimelineSemaphoreVulkan>> CreateTimelineSemaphore(uint64_t initValue) noexcept;

    Nullable<unique_ptr<BufferViewVulkan>> CreateBufferView(const VkBufferViewCreateInfo& info) noexcept;

    Nullable<unique_ptr<PipelineLayoutVulkan>> CreatePipelineLayoutInternal(const PipelineLayoutDescriptor& desc) noexcept;

    Nullable<unique_ptr<SamplerVulkan>> CreateSamplerInternal(const SamplerDescriptor& desc) noexcept;

    const VkAllocationCallbacks* GetAllocationCallbacks() const noexcept;

    void SetObjectName(std::string_view name, VkObjectType type, void* vkObject) const noexcept;
    template <class T>
    void SetObjectName(std::string_view name, T vkObject) const noexcept {
        SetObjectName(name, VulkanObjectTrait<T>::type, vkObject);
    }

    void DestroyImpl() noexcept;

    InstanceVulkanImpl* _instance;
    VkPhysicalDevice _physicalDevice;
    VkDevice _device;
    std::unique_ptr<VMA> _vma;
    std::array<vector<unique_ptr<QueueVulkan>>, (size_t)QueueType::MAX_COUNT> _queues;
    DeviceFuncTable _ftb;
    DescriptorSetLayoutCacheVulkan _descriptorSetLayoutCache;
    DescriptorSetAllocatorVulkan _descriptorSetAllocator;
    SamplerCache _samplerCache;
    VkPhysicalDeviceFeatures _feature;
    ExtFeaturesVulkan _extFeatures;
    VkPhysicalDeviceProperties _properties;
    ExtPropertiesVulkan _extProperties;
    DeviceDetail _detail;
};

class QueueVulkan final : public CommandQueue {
public:
    QueueVulkan(
        DeviceVulkan* device,
        VkQueue queue,
        QueueIndexInFamily family,
        QueueType type,
        VkQueueFlags queueFlags) noexcept;

    ~QueueVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override;

    void Wait() noexcept override;

    QueueType GetQueueType() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkQueue _queue;
    QueueIndexInFamily _family;
    QueueType _type;
    VkQueueFlags _queueFlags{0};
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

    void SetDebugName(std::string_view name) noexcept override;

    void Begin() noexcept override;

    void End() noexcept override;

    void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept override;

    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept override;

    void EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept override;

    Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept override;

    void EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept override;

    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override;

    void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept override;

    void CopyTextureToBuffer(Buffer* dst, uint64_t dstOffset, Texture* src, SubresourceRange srcRange) noexcept override;

    void CopyTextureToTexture(const TextureCopyDescriptor& desc) noexcept override;

    void ResolveTexture(const TextureResolveDescriptor& desc) noexcept override;

    void ResetQueryPool(QueryPool* pool, uint32_t firstIndex, uint32_t count) noexcept override;

    void WriteTimestamp(const QueryTimestampDescriptor& desc) noexcept override;

    void ResolveQueryData(const QueryResolveDescriptor& desc) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    QueueVulkan* _queue;
    unique_ptr<CommandPoolVulkan> _cmdPool;
    VkCommandBuffer _cmdBuffer;
    vector<unique_ptr<CommandEncoder>> _endedEncoders;
};

class SimulateCommandEncoderVulkan final : public GraphicsCommandEncoder {
public:
    SimulateCommandEncoderVulkan(
        DeviceVulkan* device,
        CommandBufferVulkan* cmdBuffer) noexcept;

    ~SimulateCommandEncoderVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    CommandBuffer* GetCommandBuffer() const noexcept override;

    void SetViewport(Viewport vp) noexcept override;

    void SetScissor(Rect rect) noexcept override;

    void BindVertexBuffer(std::span<const VertexBufferView> vbv) noexcept override;

    void BindIndexBuffer(IndexBufferView ibv) noexcept override;

    void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept override;

    void BindShaderParameterSet(
        uint32_t groupIndex,
        ShaderParameterSet* set,
        std::span<const ShaderParameterDynamicOffset> dynamicOffsets) noexcept override;

    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept override;

    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept override;

    void DrawIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount) noexcept override;

    void DrawIndexedIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    CommandBufferVulkan* _cmdBuffer;
    FrameBufferVulkan* _framebuffer{nullptr};
    GraphicsPipelineVulkan* _boundPso{nullptr};
    PipelineLayoutVulkan* _boundLayout{nullptr};
};

class SimulateComputeEncoderVulkan final : public ComputeCommandEncoder {
public:
    SimulateComputeEncoderVulkan(
        DeviceVulkan* device,
        CommandBufferVulkan* cmdBuffer) noexcept;

    ~SimulateComputeEncoderVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    CommandBuffer* GetCommandBuffer() const noexcept override;

    void BindComputePipelineState(ComputePipelineState* pso) noexcept override;

    void BindShaderParameterSet(
        uint32_t groupIndex,
        ShaderParameterSet* set,
        std::span<const ShaderParameterDynamicOffset> dynamicOffsets) noexcept override;

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept override;

    void DispatchIndirect(Buffer* argumentBuffer, uint64_t argumentOffset) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    CommandBufferVulkan* _cmdBuffer;
    ComputePipelineVulkan* _boundPso{nullptr};
    PipelineLayoutVulkan* _boundLayout{nullptr};
};

class RenderPassVulkan final : public RenderPass {
public:
    RenderPassVulkan(
        DeviceVulkan* device,
        VkRenderPass renderPass,
        const RenderPassDescriptor& desc);

    ~RenderPassVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    RenderPassDescriptor GetDesc() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkRenderPass _renderPass;
    vector<RenderPassColorAttachmentDescriptor> _colorAttachments;
    std::optional<RenderPassDepthStencilAttachmentDescriptor> _depthStencilAttachment;
};

class FrameBufferVulkan final : public Framebuffer {
public:
    FrameBufferVulkan(
        DeviceVulkan* device,
        VkFramebuffer framebuffer,
        const FramebufferDescriptor& desc);

    ~FrameBufferVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    FramebufferDescriptor GetDesc() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkFramebuffer _framebuffer;
    RenderPass* _pass{nullptr};
    vector<TextureView*> _colorAttachments;
    TextureView* _depthStencilAttachment{nullptr};
    uint32_t _width{0};
    uint32_t _height{0};
    uint32_t _layers{1};
};

class LegacyFenceVulkan final : public RenderBase {
public:
    LegacyFenceVulkan(
        DeviceVulkan* device,
        VkFence fence) noexcept;

    ~LegacyFenceVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Wait() noexcept;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkFence _fence;
};

class LegacySemaphoreVulkan final : public RenderBase {
public:
    LegacySemaphoreVulkan(
        DeviceVulkan* device,
        VkSemaphore semaphore) noexcept;

    ~LegacySemaphoreVulkan() noexcept;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkSemaphore _semaphore;
};

class FenceVulkan final : public Fence {
public:
    FenceVulkan(
        DeviceVulkan* device,
        unique_ptr<TimelineSemaphoreVulkan> timelineSemaphore) noexcept;

    ~FenceVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    uint64_t GetCompletedValue() const noexcept override;

    uint64_t GetLastSignaledValue() const noexcept override;

    void Wait() noexcept override;

    void Wait(uint64_t value) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    unique_ptr<TimelineSemaphoreVulkan> _fence;
    uint64_t _fenceValue{0};
};

class TimelineSemaphoreVulkan final : public RenderBase {
public:
    TimelineSemaphoreVulkan(
        DeviceVulkan* device,
        VkSemaphore semaphore) noexcept;

    ~TimelineSemaphoreVulkan() noexcept;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    uint64_t GetCompletedValue() const noexcept;

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

class SwapChainSyncObjectVulkan final : public SwapChainSyncObject {
public:
    explicit SwapChainSyncObjectVulkan(unique_ptr<LegacySemaphoreVulkan> semaphore) noexcept;

    ~SwapChainSyncObjectVulkan() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    bool IsPendingQueueUseComplete() const noexcept;

    void ClearPendingQueueUse() noexcept;

    void WaitPendingQueueUse() noexcept;

public:
    unique_ptr<LegacySemaphoreVulkan> _semaphore{nullptr};
    FenceVulkan* _pendingQueueFence{nullptr};
    uint64_t _pendingQueueValue{0};
};

/**
 * Vulkan swapchain semaphore ownership follows the Khronos recommended split:
 *
 * - acquire semaphore ("waitToDraw") is indexed by the in-flight frame. It is
 *   passed to vkAcquireNextImageKHR and then consumed by a queue submit wait.
 *   Before reusing it for another acquire, the consuming submit must be proven
 *   complete. This implementation records the submit timeline fence/value in
 *   SwapChainSyncObjectVulkan and waits or skips the object in the recycle pool
 *   until that token is complete, satisfying
 *   VUID-vkAcquireNextImageKHR-semaphore-01779.
 *
 * - present wait semaphore ("readyToPresent") is indexed by swapchain image.
 *   vkQueuePresentKHR gives no fence in core Vulkan, so a submit fence does not
 *   prove presentation has stopped using this semaphore. Reacquiring the same
 *   image proves the previous presentation for that image is complete, so the
 *   semaphore lives in Frame::readyToPresent.
 */
class SwapChainVulkan final : public SwapChain {
public:
    SwapChainVulkan(
        DeviceVulkan* device,
        QueueVulkan* queue,
        unique_ptr<SurfaceVulkan> surface,
        VkSwapchainKHR swapchain,
        const SwapChainDescriptor& desc) noexcept;

    ~SwapChainVulkan() noexcept;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    SwapChainAcquireResult AcquireNext(uint64_t timeoutMs) noexcept override;

    SwapChainPresentResult Present(SwapChainFrame&& frame) noexcept override;

    bool Recreate(uint32_t width, uint32_t height, TextureFormat format, PresentMode presentMode) noexcept override;

    uint32_t GetBackBufferCount() const noexcept override;

    SwapChainDescriptor GetDesc() const noexcept override;

public:
    void DestroyImpl() noexcept;
    Nullable<unique_ptr<SwapChainSyncObjectVulkan>> AcquireSyncObjectFromPool() noexcept;
    void RecycleSyncObject(unique_ptr<SwapChainSyncObjectVulkan> syncObject) noexcept;

    class Frame {
    public:
        unique_ptr<ImageVulkan> image;
        unique_ptr<SwapChainSyncObjectVulkan> acquireSyncObject;
        unique_ptr<SwapChainSyncObjectVulkan> readyToPresent;
    };

    class OutstandingAcquire {
    public:
        uint32_t imageIndex{std::numeric_limits<uint32_t>::max()};
        SwapChainSyncObjectVulkan* waitToDraw{nullptr};
        SwapChainSyncObjectVulkan* readyToPresent{nullptr};

        bool IsValid() const noexcept {
            return imageIndex != std::numeric_limits<uint32_t>::max() &&
                   waitToDraw != nullptr &&
                   readyToPresent != nullptr;
        }

        void Reset() noexcept {
            imageIndex = std::numeric_limits<uint32_t>::max();
            waitToDraw = nullptr;
            readyToPresent = nullptr;
        }
    };

    DeviceVulkan* _device;
    QueueVulkan* _queue;
    unique_ptr<SurfaceVulkan> _surface;
    VkSwapchainKHR _swapchain;
    const void* _nativeHandler;
    vector<Frame> _frames;
    vector<unique_ptr<SwapChainSyncObjectVulkan>> _recycledSyncObjects;
    OutstandingAcquire _outstandingAcquire{};
    uint64_t _outstandingFrameToken{0};
    uint32_t _width{0};
    uint32_t _height{0};
    TextureFormat _reqFormat{TextureFormat::UNKNOWN};
    PresentMode _mode{PresentMode::FIFO};
};

class QueryPoolVulkan final : public QueryPool {
public:
    QueryPoolVulkan(
        DeviceVulkan* device,
        VkQueryPool pool,
        QueryPoolDescriptor desc) noexcept;
    ~QueryPoolVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    QueryType GetType() const noexcept override;

    uint32_t GetCount() const noexcept override;

    TimestampQueryCalibration GetTimestampCalibration(CommandQueue* queue) const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkQueryPool _pool{VK_NULL_HANDLE};
    QueryPoolDescriptor _desc;
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

    void* Map(uint64_t offset, uint64_t size) noexcept override;

    void Unmap() noexcept override;

    void FlushMappedRange(BufferRange range) noexcept override;

    void InvalidateMappedRange(BufferRange range) noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    BufferDescriptor GetDesc() const noexcept override;

    Device* GetDevice() const noexcept override { return _device; }

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkBuffer _buffer;
    VmaAllocation _allocation;
    VmaAllocationInfo _allocInfo;
    VkDeviceSize _reqSize;
    string _name;
    uint64_t _reqSizeLogical{0};
    MemoryType _memory{};
    BufferUses _usage{BufferUse::UNKNOWN};
    ResourceHints _hints{};
    bool _hostCoherent{false};
};

class BufferViewVulkan final : public RenderBase {
public:
    BufferViewVulkan(
        DeviceVulkan* device,
        VkBufferView view) noexcept;

    ~BufferViewVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkBufferView _bufferView;
    VkBufferViewCreateInfo _rawInfo;
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

    void SetDebugName(std::string_view name) noexcept override;

    TextureDescriptor GetDesc() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkImage _image;
    VmaAllocation _allocation;
    VmaAllocationInfo _allocInfo;
    string _name;
    VkFormat _rawFormat;
    TextureDimension _dim{TextureDimension::UNKNOWN};
    uint32_t _width{0};
    uint32_t _height{0};
    uint32_t _depthOrArraySize{0};
    uint32_t _mipLevels{0};
    uint32_t _sampleCount{0};
    TextureFormat _format{TextureFormat::UNKNOWN};
    MemoryType _memory{MemoryType::Device};
    TextureUses _usage{TextureUse::UNKNOWN};
    ResourceHints _hints{ResourceHint::None};
    bool _isSwapchainImage{false};
};

class ImageViewVulkan final : public TextureView {
public:
    ImageViewVulkan(
        DeviceVulkan* device,
        ImageVulkan* image,
        VkImageView view) noexcept;

    ~ImageViewVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    ImageVulkan* _image;
    VkImageView _imageView;
    TextureViewDescriptor _mdesc;
    VkFormat _rawFormat{VK_FORMAT_UNDEFINED};
};

class PipelineLayoutVulkan final : public PipelineLayout {
public:
    explicit PipelineLayoutVulkan(DeviceVulkan* device) noexcept;

    ~PipelineLayoutVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    void RebindNativePointers() noexcept;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkPipelineLayoutCreateInfo _desc{};
    VkPipelineLayout _layout{VK_NULL_HANDLE};
    vector<VkDescriptorSetLayout> _setLayouts;
    vector<vector<ShaderParameterSetLayoutEntryDescriptor>> _parameterSetLayouts;
    std::optional<VkPushConstantRange> _pushConstantRange;
};

class ShaderParameterSetVulkan final : public ShaderParameterSet {
public:
    ShaderParameterSetVulkan() noexcept = default;
    ~ShaderParameterSetVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    bool Set(
        uint32_t binding,
        uint32_t arrayElement,
        ShaderParameterValue value) noexcept override;

    bool FlushWrites() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device{nullptr};
    PipelineLayoutVulkan* _layout{nullptr};
    uint32_t _groupIndex{0};
    DescriptorSetAllocatorVulkan::Allocation _allocation;
    vector<size_t> _bindingValueOffsets;
    vector<std::optional<ShaderParameterValue>> _values;
    vector<uint8_t> _dirty;
    vector<unique_ptr<BufferViewVulkan>> _texelBufferViews;
};

class GraphicsPipelineVulkan final : public GraphicsPipelineState {
public:
    GraphicsPipelineVulkan(
        DeviceVulkan* device,
        PipelineLayoutVulkan* layout,
        VkPipeline pipeline) noexcept;

    ~GraphicsPipelineVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    PipelineLayoutVulkan* _layout;
    VkPipeline _pipeline;
};

class ComputePipelineVulkan final : public ComputePipelineState {
public:
    ComputePipelineVulkan(
        DeviceVulkan* device,
        PipelineLayoutVulkan* layout,
        VkPipeline pipeline) noexcept;

    ~ComputePipelineVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    PipelineLayoutVulkan* _layout;
    VkPipeline _pipeline;
};

class ShaderModuleVulkan final : public Shader {
public:
    ShaderModuleVulkan(
        DeviceVulkan* device,
        VkShaderModule shaderModule,
        ShaderStages stages) noexcept;

    ~ShaderModuleVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    ShaderStages GetStages() const noexcept override { return _stages; }

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkShaderModule _shaderModule;
    ShaderStages _stages{ShaderStage::UNKNOWN};
};

class SamplerVulkan final : public Sampler {
public:
    SamplerVulkan(
        DeviceVulkan* device,
        VkSampler sampler) noexcept;
    ~SamplerVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkSampler _sampler;
    SamplerDescriptor _mdesc;
};

Nullable<shared_ptr<DeviceVulkan>> CreateDeviceVulkan(const VulkanDeviceDescriptor& desc);

Nullable<InstanceVulkanImpl*> InitVulkanEnvImpl(const VulkanInstanceDescriptor& desc);

void ShutdownVulkanEnvImpl() noexcept;

constexpr auto CastVkObject(CommandQueue* p) noexcept { return static_cast<QueueVulkan*>(p); }
constexpr auto CastVkObject(CommandBuffer* p) noexcept { return static_cast<CommandBufferVulkan*>(p); }
constexpr auto CastVkObject(SwapChain* p) noexcept { return static_cast<SwapChainVulkan*>(p); }
constexpr auto CastVkObject(Fence* p) noexcept { return static_cast<FenceVulkan*>(p); }
constexpr auto CastVkObject(Buffer* p) noexcept { return static_cast<BufferVulkan*>(p); }
constexpr auto CastVkObject(Texture* p) noexcept { return static_cast<ImageVulkan*>(p); }
constexpr auto CastVkObject(TextureView* p) noexcept { return static_cast<ImageViewVulkan*>(p); }
constexpr auto CastVkObject(RenderPass* p) noexcept { return static_cast<RenderPassVulkan*>(p); }
constexpr auto CastVkObject(Framebuffer* p) noexcept { return static_cast<FrameBufferVulkan*>(p); }
constexpr auto CastVkObject(Sampler* p) noexcept { return static_cast<SamplerVulkan*>(p); }
constexpr auto CastVkObject(Shader* p) noexcept { return static_cast<ShaderModuleVulkan*>(p); }
constexpr auto CastVkObject(PipelineLayout* p) noexcept { return static_cast<PipelineLayoutVulkan*>(p); }
constexpr auto CastVkObject(ShaderParameterSet* p) noexcept { return static_cast<ShaderParameterSetVulkan*>(p); }
constexpr auto CastVkObject(GraphicsPipelineState* p) noexcept { return static_cast<GraphicsPipelineVulkan*>(p); }
constexpr auto CastVkObject(ComputePipelineState* p) noexcept { return static_cast<ComputePipelineVulkan*>(p); }
constexpr auto CastVkObject(SwapChainSyncObject* p) noexcept { return static_cast<SwapChainSyncObjectVulkan*>(p); }
constexpr auto CastVkObject(QueryPool* p) noexcept { return static_cast<QueryPoolVulkan*>(p); }

}  // namespace radray::render::vulkan

#endif
