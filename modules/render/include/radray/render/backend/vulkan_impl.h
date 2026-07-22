#pragma once

#ifdef RADRAY_ENABLE_VULKAN

#include <array>

#include <radray/allocator.h>
#include <radray/render/backend/vulkan_helper.h>
#include <radray/render/common.h>

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
class DescriptorSetLayoutVulkan;
class PipelineLayoutVulkan;
class BindingDescriptorPoolVulkan;
class BindingGroupVulkan;
class GraphicsPipelineVulkan;
class ComputePipelineVulkan;
class ShaderModuleVulkan;
class DescriptorSetVulkan;
class DescriptorSetAllocatorVulkan;
class BindlessDescriptorSetVulkan;
class BindlessDescAllocator;
class SamplerVulkan;
class BindlessArrayVulkan;
class AccelerationStructureVulkan;
class AccelerationStructureViewVulkan;
class RayTracingPipelineVulkan;
class CommandEncoderRayTracingVulkan;
class QueryPoolVulkan;

struct QueueIndexInFamily {
    uint32_t Family;
    uint32_t IndexInFamily;
};

struct ExtFeaturesVulkan {
    VkPhysicalDeviceVulkan11Features feature11;
    VkPhysicalDeviceVulkan12Features feature12;
    VkPhysicalDeviceVulkan13Features feature13;
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddress;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipeline;
};

struct ExtPropertiesVulkan {
    std::optional<VkPhysicalDeviceConservativeRasterizationPropertiesEXT> conservativeRasterization;
    std::optional<VkPhysicalDeviceAccelerationStructurePropertiesKHR> accelerationStructure;
    std::optional<VkPhysicalDeviceRayTracingPipelinePropertiesKHR> rayTracingPipeline;
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

    Nullable<unique_ptr<DescriptorPool>> CreateDescriptorPool(const DescriptorPoolDescriptor& desc) noexcept override;

    Nullable<unique_ptr<BindingGroup>> CreateBindingGroup(
        DescriptorPool* pool,
        PipelineLayout* layout,
        uint32_t groupIndex) noexcept override;

    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<AccelerationStructure>> CreateAccelerationStructure(const AccelerationStructureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<AccelerationStructureView>> CreateAccelerationStructureView(const AccelerationStructureViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<RayTracingPipelineState>> CreateRayTracingPipelineState(const RayTracingPipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<ShaderBindingTable>> CreateShaderBindingTable(const ShaderBindingTableDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override;

    Nullable<unique_ptr<BindlessArray>> CreateBindlessArray(const BindlessArrayDescriptor& desc) noexcept override;

public:
    Nullable<unique_ptr<LegacyFenceVulkan>> CreateLegacyFence(VkFenceCreateFlags flags) noexcept;

    Nullable<unique_ptr<LegacySemaphoreVulkan>> CreateLegacySemaphore(VkSemaphoreCreateFlags flags) noexcept;

    Nullable<unique_ptr<TimelineSemaphoreVulkan>> CreateTimelineSemaphore(uint64_t initValue) noexcept;

    Nullable<unique_ptr<BufferViewVulkan>> CreateBufferView(const VkBufferViewCreateInfo& info) noexcept;

    Nullable<unique_ptr<PipelineLayoutVulkan>> CreateRootSignatureInternal(const PipelineLayoutDescriptor& desc) noexcept;

    Nullable<unique_ptr<DescriptorSetVulkan>> CreateDescriptorSetInternal(
        PipelineLayoutVulkan* rootSig,
        uint32_t setIndex,
        DescriptorSetAllocatorVulkan* allocator = nullptr) noexcept;

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
    std::unique_ptr<DescriptorSetAllocatorVulkan> _descSetAlloc;
    std::unique_ptr<BindlessDescAllocator> _bdlsBuffer;
    std::unique_ptr<BindlessDescAllocator> _bdlsBufferTexelRo;
    std::unique_ptr<BindlessDescAllocator> _bdlsBufferTexelRw;
    std::unique_ptr<BindlessDescAllocator> _bdlsTex2d;
    std::unique_ptr<BindlessDescAllocator> _bdlsTex3d;
    DeviceFuncTable _ftb;
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

    Nullable<unique_ptr<RayTracingCommandEncoder>> BeginRayTracingPass() noexcept override;

    void EndRayTracingPass(unique_ptr<RayTracingCommandEncoder> encoder) noexcept override;

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

    void BindBindingGroup(
        uint32_t groupIndex,
        BindingGroup* group,
        std::span<const uint32_t> dynamicOffsets) noexcept override;

    bool SetPushConstants(
        PipelineLayout* layout,
        uint32_t groupIndex,
        uint32_t binding,
        std::span<const byte> data) noexcept override;

    void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept override;

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
    PipelineLayoutVulkan* _boundPipeLayout{nullptr};
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

    void BindBindingGroup(
        uint32_t groupIndex,
        BindingGroup* group,
        std::span<const uint32_t> dynamicOffsets) noexcept override;

    bool SetPushConstants(
        PipelineLayout* layout,
        uint32_t groupIndex,
        uint32_t binding,
        std::span<const byte> data) noexcept override;

    void BindComputePipelineState(ComputePipelineState* pso) noexcept override;

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept override;

    void DispatchIndirect(Buffer* argumentBuffer, uint64_t argumentOffset) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    CommandBufferVulkan* _cmdBuffer;
    PipelineLayoutVulkan* _boundPipeLayout{nullptr};
};

class CommandEncoderRayTracingVulkan final : public RayTracingCommandEncoder {
public:
    CommandEncoderRayTracingVulkan(
        DeviceVulkan* device,
        CommandBufferVulkan* cmdBuffer) noexcept;

    ~CommandEncoderRayTracingVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    CommandBuffer* GetCommandBuffer() const noexcept override;

    void BindBindingGroup(
        uint32_t groupIndex,
        BindingGroup* group,
        std::span<const uint32_t> dynamicOffsets) noexcept override;

    bool SetPushConstants(
        PipelineLayout* layout,
        uint32_t groupIndex,
        uint32_t binding,
        std::span<const byte> data) noexcept override;

    void BuildBottomLevelAS(const BuildBottomLevelASDescriptor& desc) noexcept override;

    void BuildTopLevelAS(const BuildTopLevelASDescriptor& desc) noexcept override;

    void BindRayTracingPipelineState(RayTracingPipelineState* pso) noexcept override;

    void TraceRays(const TraceRaysDescriptor& desc) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device{nullptr};
    CommandBufferVulkan* _cmdBuffer{nullptr};
    PipelineLayoutVulkan* _boundPipeLayout{nullptr};
    RayTracingPipelineVulkan* _boundRtPipeline{nullptr};
    vector<unique_ptr<Buffer>> _keepAliveBuffers{};
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

class DescriptorSetLayoutBindingVulkanContainer {
public:
    DescriptorSetLayoutBindingVulkanContainer() = default;
    DescriptorSetLayoutBindingVulkanContainer(
        const VkDescriptorSetLayoutBinding& binding,
        ResourceBindType bindType,
        vector<unique_ptr<SamplerVulkan>> immutableSamplers,
        VkDescriptorBindingFlags bindingFlags = 0) noexcept;

    uint32_t slot{0};
    ResourceBindType bindType{ResourceBindType::UNKNOWN};
    uint32_t binding;
    VkDescriptorType descriptorType;
    uint32_t descriptorCount;
    VkShaderStageFlags stageFlags;
    VkDescriptorBindingFlags bindingFlags{0};
    vector<unique_ptr<SamplerVulkan>> immutableSamplers;
};

class DescriptorSetLayoutVulkan final : public RenderBase, public IDebugName {
public:
    DescriptorSetLayoutVulkan(
        DeviceVulkan* device,
        VkDescriptorSetLayout layout) noexcept;

    ~DescriptorSetLayoutVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkDescriptorSetLayout _layout;
    vector<DescriptorSetLayoutBindingVulkanContainer> _bindings;
};

class BindlessDescriptorSetVulkan final : public RenderBase {
public:
    BindlessDescriptorSetVulkan(
        DeviceVulkan* device,
        VkDescriptorType type,
        uint32_t capacity) noexcept;
    ~BindlessDescriptorSetVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkDescriptorPool _pool{VK_NULL_HANDLE};
    VkDescriptorSetLayout _layout{VK_NULL_HANDLE};
    VkDescriptorSet _set{VK_NULL_HANDLE};
    VkDescriptorType _type{};
    uint32_t _capacity{};
};

class BindlessDescAllocator {
public:
    struct Allocation {
        FirstFitAllocator::Allocation Range{};
        VkDescriptorSet Set{VK_NULL_HANDLE};
        VkDescriptorType Type{};

        static constexpr Allocation Invalid() noexcept { return Allocation{FirstFitAllocator::Allocation::Invalid()}; }

        constexpr bool IsValid() const noexcept { return Range.Length != 0; }
    };

    explicit BindlessDescAllocator(unique_ptr<BindlessDescriptorSetVulkan> bdls) noexcept;

    std::optional<Allocation> Allocate(uint32_t count) noexcept;

    void Destroy(Allocation allocation) noexcept;

    VkDescriptorSetLayout GetLayout() const noexcept { return _bdls->_layout; }

public:
    unique_ptr<BindlessDescriptorSetVulkan> _bdls;
    FirstFitAllocator _allocator;
};

static_assert(is_allocator<BindlessDescAllocator, BindlessDescAllocator::Allocation>, "BindlessDescAllocator does not satisfy the allocator concept");

class PipelineLayoutVulkan final : public PipelineLayout {
public:
    /**
     * PipelineLayoutVulkan internal record, indexed by ParameterIndex.
     * - Info: 对外暴露的参数信息, 同时作为 FindParameter 返回指针的稳定存储
     * - SetIndex/BindingIndex/DescriptorType: 对应 VkDescriptorSetLayoutBinding 的定位信息
     * - DescriptorWriteOffset: 参数在所属 set 的 resource/sampler 写入追踪数组内的起始下标
     * - PushConstantOffset/PushConstantSize: push constant 参数的字节范围
     * per-set 布局/push constant 列表/bindless set 反查一律扫描本结构派生, 不再用并行数组重复保存.
     * VkPipelineLayout/VkDescriptorSetLayout 为原生数据源, 描述符数量等从中派生.
     */
    struct ParameterBinding {
        ShaderParameterInfo Info{};
        uint32_t ParameterIndex{0};
        uint32_t SetIndex{0};
        uint32_t BindingIndex{0};
        VkDescriptorType DescriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};
        uint32_t DescriptorWriteOffset{0};
        uint32_t PushConstantOffset{0};
        uint32_t PushConstantSize{0};
        BindlessSlotType BindlessSlotType{BindlessSlotType::Multiple};
        bool IsStaticSampler{false};
        bool HasDynamicOffset{false};
    };

    PipelineLayoutVulkan(
        DeviceVulkan* device,
        VkPipelineLayout layout,
        vector<ParameterBinding> parameterBindings,
        uint32_t setLayoutCount) noexcept;

    ~PipelineLayoutVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    vector<ShaderParameterInfo> GetParameters() const noexcept override;

    Nullable<const ShaderParameterInfo*> FindParameter(std::string_view name) const noexcept override;

    std::optional<ShaderBindingLocation> FindBindingLocation(std::string_view name) const noexcept override;

    vector<BindingGroupLayout> GetBindingGroupLayouts() const noexcept override;

    vector<PushConstantRange> GetPushConstantRanges() const noexcept override;

    uint32_t GetParameterCount() const noexcept { return static_cast<uint32_t>(_parameterBindings.size()); }

    uint32_t GetDescriptorSetCount() const noexcept { return _setLayoutCount; }

    std::span<const ParameterBinding> GetParameterBindings() const noexcept { return _parameterBindings; }

    bool HasBindlessSet(uint32_t setIndex) const noexcept;

    Nullable<const ParameterBinding*> FindBindlessSet(uint32_t setIndex) const noexcept;

    Nullable<const ParameterBinding*> FindParameterInfo(uint32_t parameterIndex) const noexcept;

    Nullable<const ParameterBinding*> FindParameterInfo(
        uint32_t setIndex,
        uint32_t bindingIndex) const noexcept;

    vector<const ParameterBinding*> GetDynamicBufferBindings(uint32_t setIndex) const noexcept;

    Nullable<DescriptorSetLayoutVulkan*> GetSetLayout(uint32_t setIndex) const noexcept;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkPipelineLayout _layout;

    // Single internal record array, indexed by ParameterIndex.
    vector<ParameterBinding> _parameterBindings{};
    // Resolved per-set layouts may be explicitly borrowed from another pipeline
    // layout. Owned layouts keep immutable samplers alive.
    vector<DescriptorSetLayoutVulkan*> _setLayouts;
    vector<unique_ptr<DescriptorSetLayoutVulkan>> _ownedLayouts;
    uint32_t _setLayoutCount{0};
};

class GraphicsPipelineVulkan final : public GraphicsPipelineState {
public:
    GraphicsPipelineVulkan(
        DeviceVulkan* device,
        VkPipeline pipeline) noexcept;

    ~GraphicsPipelineVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkPipeline _pipeline;
};

class ComputePipelineVulkan final : public ComputePipelineState {
public:
    ComputePipelineVulkan(
        DeviceVulkan* device,
        VkPipeline pipeline) noexcept;

    ~ComputePipelineVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkPipeline _pipeline;
};

class RayTracingPipelineVulkan final : public RayTracingPipelineState {
public:
    RayTracingPipelineVulkan(
        DeviceVulkan* device,
        VkPipeline pipeline,
        PipelineLayoutVulkan* rootSig) noexcept;

    ~RayTracingPipelineVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    ShaderBindingTableRequirements GetShaderBindingTableRequirements() const noexcept override;

    std::optional<vector<byte>> GetShaderBindingTableHandle(std::string_view shaderName) const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkPipeline _pipeline{VK_NULL_HANDLE};
    PipelineLayoutVulkan* _rootSig{nullptr};
    unordered_map<string, uint32_t> _groupIndices;
    uint32_t _groupCount{0};
};

class ShaderBindingTableVulkan final : public ShaderBindingTable {
public:
    ShaderBindingTableVulkan(
        DeviceVulkan* device,
        RayTracingPipelineVulkan* pipeline,
        unique_ptr<Buffer> buffer,
        const ShaderBindingTableDescriptor& desc,
        uint64_t recordStride) noexcept;
    ~ShaderBindingTableVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    bool Build(std::span<const ShaderBindingTableBuildEntry> entries) noexcept override;

    bool IsBuilt() const noexcept override;

    ShaderBindingTableRegions GetRegions() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    RayTracingPipelineVulkan* _pipeline;
    unique_ptr<Buffer> _buffer;
    ShaderBindingTableDescriptor _desc;
    uint64_t _recordStride{0};
    uint64_t _rayGenOffset{0};
    uint64_t _missOffset{0};
    uint64_t _hitGroupOffset{0};
    uint64_t _callableOffset{0};
    bool _isBuilt{false};
    string _name;
};

class ShaderModuleVulkan final : public Shader {
public:
    ShaderModuleVulkan(
        DeviceVulkan* device,
        VkShaderModule shaderModule,
        ShaderStages stages,
        std::optional<ShaderReflectionDesc> reflection) noexcept;

    ~ShaderModuleVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    ShaderStages GetStages() const noexcept override { return _stages; }

    Nullable<const ShaderReflectionDesc*> GetReflection() const noexcept override {
        return _reflection.has_value() ? Nullable<const ShaderReflectionDesc*>{&_reflection.value()} : Nullable<const ShaderReflectionDesc*>{};
    }

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkShaderModule _shaderModule;
    ShaderStages _stages{ShaderStage::UNKNOWN};
    std::optional<ShaderReflectionDesc> _reflection{};
};

class DescriptorPoolVulkan final : public RenderBase {
public:
    DescriptorPoolVulkan(
        DeviceVulkan* device,
        DescriptorSetAllocatorVulkan* alloc,
        VkDescriptorPool pool,
        uint32_t capacity) noexcept;

    ~DescriptorPoolVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    DescriptorSetAllocatorVulkan* _alloc;
    VkDescriptorPool _pool;
    uint32_t _capacity;
    uint32_t _liveCount{0};
};

class DescriptorSetAllocatorVulkan {
public:
    struct Allocation {
        DescriptorPoolVulkan* Pool;
        VkDescriptorSet Set;

        static constexpr Allocation Invalid() noexcept {
            return Allocation{nullptr, VK_NULL_HANDLE};
        }

        constexpr bool IsValid() const noexcept {
            return Pool != nullptr && Set != VK_NULL_HANDLE;
        }
    };

    DescriptorSetAllocatorVulkan(
        DeviceVulkan* device,
        uint32_t keepFreePages,
        std::optional<vector<VkDescriptorPoolSize>> specPoolSize = std::nullopt,
        uint32_t maxSetsPerPage = 1024,
        uint32_t maxAllocations = std::numeric_limits<uint32_t>::max(),
        uint32_t maxPages = std::numeric_limits<uint32_t>::max(),
        bool strictPoolSizes = false) noexcept;
    ~DescriptorSetAllocatorVulkan() noexcept;

    DescriptorSetAllocatorVulkan(const DescriptorSetAllocatorVulkan&) = delete;
    DescriptorSetAllocatorVulkan(DescriptorSetAllocatorVulkan&&) = delete;
    DescriptorSetAllocatorVulkan& operator=(const DescriptorSetAllocatorVulkan&) = delete;
    DescriptorSetAllocatorVulkan& operator=(DescriptorSetAllocatorVulkan&&) = delete;

    std::optional<Allocation> Allocate(
        DescriptorSetLayoutVulkan* layout,
        std::optional<uint32_t> variableDescriptorCount = std::nullopt) noexcept;

    void Destroy(Allocation allocation) noexcept;

    bool Reset() noexcept;

    uint32_t GetLiveAllocationCount() const noexcept { return _liveAllocationCount; }

public:
    DescriptorPoolVulkan* NewPage(
        DescriptorSetLayoutVulkan* layout,
        std::optional<uint32_t> variableDescriptorCount) noexcept;

    void TryReleaseFreePages() noexcept;

    DeviceVulkan* _device;
    vector<unique_ptr<DescriptorPoolVulkan>> _pages;
    size_t _hintPage{0};
    std::optional<vector<VkDescriptorPoolSize>> _specPoolSize;
    uint32_t _keepFreePages;
    uint32_t _maxSetsPerPage{1024};
    uint32_t _maxAllocations{std::numeric_limits<uint32_t>::max()};
    uint32_t _maxPages{std::numeric_limits<uint32_t>::max()};
    uint32_t _liveAllocationCount{0};
    bool _strictPoolSizes{false};
};

class BindingDescriptorPoolVulkan final : public DescriptorPool {
public:
    BindingDescriptorPoolVulkan(
        DeviceVulkan* device,
        const DescriptorPoolDescriptor& desc,
        unique_ptr<DescriptorSetAllocatorVulkan> allocator) noexcept;
    ~BindingDescriptorPoolVulkan() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    void SetDebugName(std::string_view name) noexcept override;
    bool Reset() noexcept override;
    DescriptorPoolDescriptor GetDesc() const noexcept override { return _desc; }
    uint32_t GetAllocatedBindingGroupCount() const noexcept override;

    DescriptorSetAllocatorVulkan* GetAllocator() const noexcept { return _allocator.get(); }
    bool ReserveGroup(const DescriptorPoolDescriptor& usage) noexcept;
    void ReleaseGroup(const DescriptorPoolDescriptor& usage) noexcept;

public:
    DeviceVulkan* _device{nullptr};
    DescriptorPoolDescriptor _desc{};
    DescriptorPoolDescriptor _used{};
    unique_ptr<DescriptorSetAllocatorVulkan> _allocator{};
    uint32_t _liveGroups{0};
    string _name{};
};

enum class DescriptorWritePayloadVulkan : uint8_t {
    Image,
    Buffer,
    TexelBuffer,
    AccelerationStructure,
};

struct PendingDescriptorWriteVulkan {
    VkDescriptorSet Set{VK_NULL_HANDLE};
    uint32_t Binding{0};
    uint32_t ArrayIndex{0};
    VkDescriptorType Type{VK_DESCRIPTOR_TYPE_MAX_ENUM};
    DescriptorWritePayloadVulkan Payload{DescriptorWritePayloadVulkan::Image};
    VkDescriptorImageInfo ImageInfo{};
    VkDescriptorBufferInfo BufferInfo{};
    VkBufferView TexelBufferView{VK_NULL_HANDLE};
    VkAccelerationStructureKHR AccelerationStructure{VK_NULL_HANDLE};
};

class DescriptorSetVulkan final : public IDebugName {
public:
    DescriptorSetVulkan(
        DeviceVulkan* device,
        PipelineLayoutVulkan* rootSig,
        uint32_t setIndex,
        DescriptorSetLayoutVulkan* layout,
        DescriptorSetAllocatorVulkan* allocator,
        DescriptorSetAllocatorVulkan::Allocation allocation) noexcept;

    ~DescriptorSetVulkan() noexcept;

    bool IsValid() const noexcept;

    void Destroy() noexcept;

    void Reset() noexcept;

    void SetDebugName(std::string_view name) noexcept override;

    PipelineLayoutVulkan* GetRootSignature() const noexcept { return _rootSig; }

    uint32_t GetSetIndex() const noexcept { return _setIndex; }

    bool WriteResource(uint32_t parameterIndex, ResourceView* view, uint32_t arrayIndex) noexcept;

    bool WriteResource(uint32_t parameterIndex, const BufferBindingDescriptor& desc, uint32_t arrayIndex) noexcept;

    bool WriteSampler(uint32_t parameterIndex, Sampler* sampler, uint32_t arrayIndex) noexcept;

    bool SetResource(uint32_t slot, uint32_t arrayIndex, ResourceView* view) noexcept;

    bool SetBufferResource(uint32_t slot, uint32_t arrayIndex, const BufferBindingDescriptor& desc) noexcept;

    bool SetSampler(uint32_t slot, uint32_t arrayIndex, Sampler* sampler) noexcept;

    Nullable<const PipelineLayoutVulkan::ParameterBinding*> FindFirstUnwrittenParameter(
        uint32_t* arrayIndex = nullptr) const noexcept;

    bool IsFullyWritten() const noexcept;
    bool HasAnyWrite() const noexcept;

    void StageWrite(PendingDescriptorWriteVulkan write) noexcept;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    PipelineLayoutVulkan* _rootSig;
    uint32_t _setIndex{0};
    DescriptorSetLayoutVulkan* _layout;
    DescriptorSetAllocatorVulkan* _allocator;
    DescriptorSetAllocatorVulkan::Allocation _allocation;
    vector<uint8_t> _resourceWritten{};
    vector<uint8_t> _samplerWritten{};
    vector<PendingDescriptorWriteVulkan> _pendingWrites{};
    unordered_map<uint64_t, unique_ptr<BufferViewVulkan>> _ownedTexelBufferViews{};
    string _name{};
};

class BindingGroupVulkan final : public BindingGroup {
public:
    BindingGroupVulkan(
        DeviceVulkan* device,
        BindingDescriptorPoolVulkan* pool,
        PipelineLayoutVulkan* layout,
        uint32_t groupIndex,
        unique_ptr<DescriptorSetVulkan> descriptorSet,
        const DescriptorPoolDescriptor& poolUsage) noexcept;
    ~BindingGroupVulkan() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    void Reset() noexcept override;
    void SetDebugName(std::string_view name) noexcept override;

    PipelineLayout* GetPipelineLayout() const noexcept override { return _layout; }
    uint32_t GetGroupIndex() const noexcept override { return _groupIndex; }

    bool SetResource(uint32_t binding, ResourceView* view, uint32_t arrayIndex = 0) noexcept override;
    bool SetResource(uint32_t binding, const BufferBindingDescriptor& desc, uint32_t arrayIndex = 0) noexcept override;
    bool SetSampler(uint32_t binding, Sampler* sampler, uint32_t arrayIndex = 0) noexcept override;
    bool SetBindlessArray(uint32_t binding, BindlessArray* array) noexcept override;
    bool IsFullyWritten() const noexcept override;

    bool FlushDescriptorWrites() noexcept;
    DescriptorSetVulkan* GetDescriptorSet() const noexcept { return _descriptorSet.get(); }
    BindlessArray* GetBindlessArray() const noexcept { return _bindlessArray; }
    const BufferBindingDescriptor* GetDynamicBuffer(uint32_t parameterIndex) const noexcept;

public:
    DeviceVulkan* _device{nullptr};
    BindingDescriptorPoolVulkan* _pool{nullptr};
    PipelineLayoutVulkan* _layout{nullptr};
    uint32_t _groupIndex{0};
    unique_ptr<DescriptorSetVulkan> _descriptorSet{};
    BindlessArray* _bindlessArray{nullptr};
    vector<PendingDescriptorWriteVulkan> _pendingWriteScratch{};
    vector<VkWriteDescriptorSet> _descriptorWriteScratch{};
    vector<VkWriteDescriptorSetAccelerationStructureKHR> _accelerationWriteScratch{};
    vector<std::optional<BufferBindingDescriptor>> _dynamicBuffers{};
    DescriptorPoolDescriptor _poolUsage{};
    string _name{};
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

class BindlessArrayVulkan final : public BindlessArray {
public:
    enum class SlotKind : uint8_t {
        None,
        Buffer,
        Texture2D,
        Texture3D
    };

    struct SlotState {
        SlotKind Kind{SlotKind::None};
        ResourceBindType ResourceType{ResourceBindType::UNKNOWN};
        BufferBindingDescriptor BufferDesc{};
        Nullable<TextureView*> Texture{nullptr};
    };

    struct CachedDescriptorSet {
        DescriptorSetLayoutVulkan* Layout{nullptr};
        DescriptorSetAllocatorVulkan::Allocation Allocation{};
        uint32_t BindingIndex{0};
        VkDescriptorType DescriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};
        bool Dirty{true};
    };

    BindlessArrayVulkan(
        DeviceVulkan* device,
        const BindlessArrayDescriptor& desc) noexcept;

    ~BindlessArrayVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    void SetBuffer(uint32_t slot, const BufferBindingDescriptor& desc) noexcept override;

    void SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    BindlessArrayDescriptor _desc{};
    uint32_t _size;
    BindlessSlotType _slotType;
    vector<SlotState> _slots{};
    vector<CachedDescriptorSet> _cachedSets{};
    string _name;
};

class AccelerationStructureVulkan final : public AccelerationStructure {
public:
    AccelerationStructureVulkan(
        DeviceVulkan* device,
        VkBuffer buffer,
        VmaAllocation allocation,
        VmaAllocationInfo allocInfo,
        VkAccelerationStructureKHR accelerationStructure,
        const AccelerationStructureDescriptor& desc,
        uint64_t asSize) noexcept;

    ~AccelerationStructureVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkBuffer _buffer{VK_NULL_HANDLE};
    VmaAllocation _allocation{VK_NULL_HANDLE};
    VmaAllocationInfo _allocInfo{};
    VkAccelerationStructureKHR _accelerationStructure{VK_NULL_HANDLE};
    VkDeviceAddress _deviceAddress{0};
    AccelerationStructureDescriptor _desc{};
    uint64_t _asSize{0};
    string _name;
};

class AccelerationStructureViewVulkan final : public AccelerationStructureView {
public:
    AccelerationStructureViewVulkan(
        DeviceVulkan* device,
        AccelerationStructureVulkan* target) noexcept;

    ~AccelerationStructureViewVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    AccelerationStructureVulkan* _target;
    AccelerationStructureViewDescriptor _desc;
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
constexpr auto CastVkObject(DescriptorPool* p) noexcept { return static_cast<BindingDescriptorPoolVulkan*>(p); }
constexpr auto CastVkObject(BindingGroup* p) noexcept { return static_cast<BindingGroupVulkan*>(p); }
constexpr auto CastVkObject(GraphicsPipelineState* p) noexcept { return static_cast<GraphicsPipelineVulkan*>(p); }
constexpr auto CastVkObject(ComputePipelineState* p) noexcept { return static_cast<ComputePipelineVulkan*>(p); }
constexpr auto CastVkObject(AccelerationStructure* p) noexcept { return static_cast<AccelerationStructureVulkan*>(p); }
constexpr auto CastVkObject(AccelerationStructureView* p) noexcept { return static_cast<AccelerationStructureViewVulkan*>(p); }
constexpr auto CastVkObject(RayTracingPipelineState* p) noexcept { return static_cast<RayTracingPipelineVulkan*>(p); }
constexpr auto CastVkObject(ShaderBindingTable* p) noexcept { return static_cast<ShaderBindingTableVulkan*>(p); }
constexpr auto CastVkObject(SwapChainSyncObject* p) noexcept { return static_cast<SwapChainSyncObjectVulkan*>(p); }
constexpr auto CastVkObject(QueryPool* p) noexcept { return static_cast<QueryPoolVulkan*>(p); }

struct VulkanBindingParameterInfo {
    string Name{};
    ShaderParameterKind Kind{ShaderParameterKind::UNKNOWN};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    uint32_t SetIndex{0};
    uint32_t BindingIndex{0};
    ResourceBindType ResourceType{ResourceBindType::UNKNOWN};
    uint32_t DescriptorCount{0};
    bool IsReadOnly{true};
    bool IsBindless{false};
    bool IsStaticSampler{false};
    BindlessSlotType BindlessSlotType{BindlessSlotType::Multiple};
    VkDescriptorType DescriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};
    uint32_t Offset{0};
    uint32_t Size{0};
};

struct VulkanMergedPipelineLayout {
    vector<ShaderParameterInfo> Parameters{};
    vector<VulkanBindingParameterInfo> VulkanParameters{};
    uint32_t DescriptorSetCount{0};
};

std::optional<VulkanMergedPipelineLayout> BuildMergedPipelineLayoutVulkan(
    std::span<Shader*> shaders,
    std::span<const BindingGroupLayout> explicitGroups = {}) noexcept;

}  // namespace radray::render::vulkan

#endif
