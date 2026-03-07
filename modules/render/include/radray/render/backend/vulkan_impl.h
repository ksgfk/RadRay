#pragma once

#ifdef RADRAY_ENABLE_VULKAN

#include <array>
#include <unordered_map>
#include <unordered_set>

#include <radray/allocator.h>
#include <radray/render/backend/vulkan_helper.h>

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
class SimulateBufferViewVulkan;
class ImageVulkan;
class ImageViewVulkan;
class DescriptorSetLayoutVulkan;
class PipelineLayoutVulkan;
class GraphicsPipelineVulkan;
class ComputePipelineVulkan;
class ShaderModuleVulkan;
class DescriptorSetVulkan;
class DescriptorSetAllocatorVulkan;
class BindingSetVulkan;
class BindlessDescriptorSetVulkan;
class BindlessDescAllocator;
class SamplerVulkan;
class BindlessArrayVulkan;
class AccelerationStructureVulkan;
class AccelerationStructureViewVulkan;
class RayTracingPipelineVulkan;
class CommandEncoderRayTracingVulkan;

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

    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override;

    Nullable<unique_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override;

    Nullable<unique_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<BindingSet>> CreateBindingSet(RootSignature* rootSig) noexcept override;

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

    Nullable<unique_ptr<RenderPassVulkan>> CreateRenderPass(const VkRenderPassCreateInfo& info) noexcept;

    Nullable<unique_ptr<SamplerVulkan>> CreateSamplerVulkan(const SamplerDescriptor& desc) noexcept;

    Nullable<unique_ptr<SamplerVulkan>> CreateSamplerVulkan(const VkSamplerCreateInfo& desc) noexcept;

    Nullable<unique_ptr<BindlessDescriptorSetVulkan>> CreateBindlessDescriptorSetVulkan(VkDescriptorType type, uint32_t capacity) noexcept;

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

    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept override;

    void EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept override;

    Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept override;

    void EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept override;

    Nullable<unique_ptr<RayTracingCommandEncoder>> BeginRayTracingPass() noexcept override;

    void EndRayTracingPass(unique_ptr<RayTracingCommandEncoder> encoder) noexcept override;

    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override;

    void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept override;

    void CopyTextureToBuffer(Buffer* dst, uint64_t dstOffset, Texture* src, SubresourceRange srcRange) noexcept override;

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

    void BindRootSignature(RootSignature* rootSig) noexcept override;

    void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept override;

    void BindBindingSet(BindingSet* set) noexcept override;

    void BindBindlessArray(uint32_t groupIndex, BindlessArray* array) noexcept override;

    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept override;

    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    CommandBufferVulkan* _cmdBuffer;
    unique_ptr<RenderPassVulkan> _pass;
    unique_ptr<FrameBufferVulkan> _framebuffer;
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

    void BindRootSignature(RootSignature* rootSig) noexcept override;

    void BindBindingSet(BindingSet* set) noexcept override;

    void BindBindlessArray(uint32_t groupIndex, BindlessArray* array) noexcept override;

    void BindComputePipelineState(ComputePipelineState* pso) noexcept override;

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept override;

    void SetThreadGroupSize(uint32_t x, uint32_t y, uint32_t z) noexcept override;

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

    void BindRootSignature(RootSignature* rootSig) noexcept override;

    void BindBindingSet(BindingSet* set) noexcept override;

    void BindBindlessArray(uint32_t groupIndex, BindlessArray* array) noexcept override;

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

class RenderPassVulkan final : public RenderBase {
public:
    RenderPassVulkan(
        DeviceVulkan* device,
        VkRenderPass renderPass) noexcept;

    ~RenderPassVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkRenderPass _renderPass;
};

class FrameBufferVulkan final : public RenderBase {
public:
    FrameBufferVulkan(
        DeviceVulkan* device,
        VkFramebuffer framebuffer) noexcept;

    ~FrameBufferVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkFramebuffer _framebuffer;
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
    FenceVulkan(
        DeviceVulkan* device,
        unique_ptr<LegacyFenceVulkan> legacyFence) noexcept;

    ~FenceVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    uint64_t GetCompletedValue() const noexcept override;

    void Wait() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    std::variant<std::monostate, unique_ptr<TimelineSemaphoreVulkan>, unique_ptr<LegacyFenceVulkan>> _fence;
    uint64_t _fenceValue{0};
    bool _legacyPendingSubmit{false};
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

public:
    unique_ptr<LegacySemaphoreVulkan> _semaphore{nullptr};
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

    AcquireResult AcquireNext() noexcept override;

    void Present(SwapChainSyncObject* waitToPresent) noexcept override;

    Nullable<Texture*> GetCurrentBackBuffer() const noexcept override;

    uint32_t GetCurrentBackBufferIndex() const noexcept override;

    uint32_t GetBackBufferCount() const noexcept override;

public:
    void DestroyImpl() noexcept;

    class Frame {
    public:
        unique_ptr<ImageVulkan> image;
    };

    DeviceVulkan* _device;
    QueueVulkan* _queue;
    unique_ptr<SurfaceVulkan> _surface;
    VkSwapchainKHR _swapchain;
    vector<Frame> _frames;
    vector<unique_ptr<SwapChainSyncObjectVulkan>> _acquireSemaphores;
    vector<unique_ptr<SwapChainSyncObjectVulkan>> _renderFinishSemaphores;
    vector<unique_ptr<LegacyFenceVulkan>> _acquireFences;
    vector<uint8_t> _acquireFenceShouldWait;
    uint32_t _nextSemaphoreSlot{0};
    uint32_t _currentTextureIndex{std::numeric_limits<uint32_t>::max()};
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

    void Unmap(uint64_t offset, uint64_t size) noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    BufferDescriptor GetDesc() const noexcept override;

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

class SimulateBufferViewVulkan final : public BufferView {
public:
    SimulateBufferViewVulkan(
        DeviceVulkan* device,
        BufferVulkan* buffer,
        const BufferViewDescriptor& desc) noexcept;

    ~SimulateBufferViewVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    BufferVulkan* _buffer;
    BufferViewDescriptor _desc;
    unique_ptr<BufferViewVulkan> _texelView;
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
    DescriptorSetLayoutBindingVulkanContainer(const VkDescriptorSetLayoutBinding& binding, ResourceBindType bindType, vector<unique_ptr<SamplerVulkan>> immutableSamplers) noexcept;

    uint32_t slot{0};
    ResourceBindType bindType{ResourceBindType::UNKNOWN};
    uint32_t binding;
    VkDescriptorType descriptorType;
    uint32_t descriptorCount;
    VkShaderStageFlags stageFlags;
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

private:
    unique_ptr<BindlessDescriptorSetVulkan> _bdls;
    FirstFitAllocator _allocator;
};

static_assert(is_allocator<BindlessDescAllocator, BindlessDescAllocator::Allocation>, "BindlessDescAllocator does not satisfy the allocator concept");

class PipelineLayoutVulkan final : public RootSignature {
public:
    struct ParameterBindingInfo {
        BindingParameterKind Kind{BindingParameterKind::UNKNOWN};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t SetIndex{0};
        uint32_t BindingIndex{0};
        uint32_t DescriptorCount{0};
        bool IsReadOnly{true};
        VkDescriptorType DescriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};
        uint32_t DescriptorWriteOffset{0};
        uint32_t PushConstantOffset{0};
        uint32_t PushConstantSize{0};
        uint32_t PushConstantStorageOffset{0};
        ShaderStages Stages{ShaderStage::UNKNOWN};
    };

    PipelineLayoutVulkan(
        DeviceVulkan* device,
        VkPipelineLayout layout) noexcept;

    ~PipelineLayoutVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    const BindingLayout& GetBindingLayout() const noexcept override { return _bindingLayout; }

    Nullable<const ParameterBindingInfo*> FindParameterInfo(BindingParameterId id) const noexcept;

    std::span<const ParameterBindingInfo> GetParameterInfos() const noexcept { return _parameters; }

    uint32_t GetSetLayoutCount() const noexcept { return _setLayoutCount; }

    Nullable<DescriptorSetLayoutVulkan*> GetSetLayout(uint32_t setIndex) const noexcept;

    uint32_t GetResourceDescriptorCount() const noexcept { return _resourceDescriptorCount; }

    uint32_t GetSamplerDescriptorCount() const noexcept { return _samplerDescriptorCount; }

    uint32_t GetPushConstantStorageSize() const noexcept { return _pushConstantStorageSize; }

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkPipelineLayout _layout;
    vector<unique_ptr<DescriptorSetLayoutVulkan>> _ownedLayouts;
    vector<ParameterBindingInfo> _parameters{};
    uint32_t _setLayoutCount{0};
    uint32_t _resourceDescriptorCount{0};
    uint32_t _samplerDescriptorCount{0};
    uint32_t _pushConstantStorageSize{0};
    BindingLayout _bindingLayout{};
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
    unique_ptr<RenderPassVulkan> _renderPass;
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

    DescriptorSetAllocatorVulkan(DeviceVulkan* device, uint32_t keepFreePages, std::optional<vector<VkDescriptorPoolSize>> specPoolSize = std::nullopt) noexcept;
    ~DescriptorSetAllocatorVulkan() noexcept;

    DescriptorSetAllocatorVulkan(const DescriptorSetAllocatorVulkan&) = delete;
    DescriptorSetAllocatorVulkan(DescriptorSetAllocatorVulkan&&) = delete;
    DescriptorSetAllocatorVulkan& operator=(const DescriptorSetAllocatorVulkan&) = delete;
    DescriptorSetAllocatorVulkan& operator=(DescriptorSetAllocatorVulkan&&) = delete;

    std::optional<Allocation> Allocate(DescriptorSetLayoutVulkan* layout) noexcept;

    void Destroy(Allocation allocation) noexcept;

private:
    DescriptorPoolVulkan* NewPage() noexcept;

    void TryReleaseFreePages() noexcept;

    DeviceVulkan* _device;
    vector<unique_ptr<DescriptorPoolVulkan>> _pages;
    size_t _hintPage{0};
    std::optional<vector<VkDescriptorPoolSize>> _specPoolSize;
    uint32_t _keepFreePages;
};

class DescriptorSetVulkan final : public RenderBase, public IDebugName {
public:
    DescriptorSetVulkan(
        DeviceVulkan* device,
        DescriptorSetLayoutVulkan* layout,
        DescriptorSetAllocatorVulkan* allocator,
        DescriptorSetAllocatorVulkan::Allocation allocation) noexcept;

    ~DescriptorSetVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    bool SetResource(uint32_t slot, uint32_t arrayIndex, ResourceView* view) noexcept;

    bool SetSampler(uint32_t slot, uint32_t arrayIndex, Sampler* sampler) noexcept;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    DescriptorSetLayoutVulkan* _layout;
    DescriptorSetAllocatorVulkan* _allocator;
    DescriptorSetAllocatorVulkan::Allocation _allocation;
};

class BindingSetVulkan final : public BindingSet {
public:
    BindingSetVulkan(
        DeviceVulkan* device,
        PipelineLayoutVulkan* rootSig,
        vector<unique_ptr<DescriptorSetVulkan>> sets) noexcept;
    ~BindingSetVulkan() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    RootSignature* GetRootSignature() const noexcept override { return _rootSig; }

    bool WriteResource(BindingParameterId id, ResourceView* view, uint32_t arrayIndex) noexcept override;

    bool WriteSampler(BindingParameterId id, Sampler* sampler, uint32_t arrayIndex) noexcept override;

    bool WritePushConstant(BindingParameterId id, const void* data, uint32_t size) noexcept override;

    bool IsFullyWritten() const noexcept;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device{nullptr};
    PipelineLayoutVulkan* _rootSig{nullptr};
    vector<unique_ptr<DescriptorSetVulkan>> _sets{};
    vector<uint8_t> _resourceWritten{};
    vector<uint8_t> _samplerWritten{};
    vector<uint8_t> _pushConstantWritten{};
    vector<byte> _pushConstantData{};
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
    BindlessArrayVulkan(
        DeviceVulkan* device,
        const BindlessArrayDescriptor& desc) noexcept;

    ~BindlessArrayVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    void SetBuffer(uint32_t slot, BufferView* bufView) noexcept override;

    void SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept override;

public:
    void DestroyImpl() noexcept;

    VkDescriptorSet GetSetForDescriptorType(VkDescriptorType type) const noexcept;

    DeviceVulkan* _device;
    BindlessDescAllocator::Allocation _bufferAlloc{};
    BindlessDescAllocator::Allocation _bufferTexelRoAlloc{};
    BindlessDescAllocator::Allocation _bufferTexelRwAlloc{};
    BindlessDescAllocator::Allocation _tex2dAlloc{};
    BindlessDescAllocator::Allocation _tex3dAlloc{};
    uint32_t _size;
    BindlessSlotType _slotType;
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

Nullable<unique_ptr<InstanceVulkanImpl>> CreateVulkanInstanceImpl(const VulkanInstanceDescriptor& desc);

void DestroyVulkanInstanceImpl(unique_ptr<InstanceVulkan> instance) noexcept;

constexpr auto CastVkObject(CommandQueue* p) noexcept { return static_cast<QueueVulkan*>(p); }
constexpr auto CastVkObject(CommandBuffer* p) noexcept { return static_cast<CommandBufferVulkan*>(p); }
constexpr auto CastVkObject(SwapChain* p) noexcept { return static_cast<SwapChainVulkan*>(p); }
constexpr auto CastVkObject(Fence* p) noexcept { return static_cast<FenceVulkan*>(p); }
constexpr auto CastVkObject(Buffer* p) noexcept { return static_cast<BufferVulkan*>(p); }
constexpr auto CastVkObject(Texture* p) noexcept { return static_cast<ImageVulkan*>(p); }
constexpr auto CastVkObject(TextureView* p) noexcept { return static_cast<ImageViewVulkan*>(p); }
constexpr auto CastVkObject(Sampler* p) noexcept { return static_cast<SamplerVulkan*>(p); }
constexpr auto CastVkObject(Shader* p) noexcept { return static_cast<ShaderModuleVulkan*>(p); }
constexpr auto CastVkObject(RootSignature* p) noexcept { return static_cast<PipelineLayoutVulkan*>(p); }
constexpr auto CastVkObject(BindingSet* p) noexcept { return static_cast<BindingSetVulkan*>(p); }
constexpr auto CastVkObject(GraphicsPipelineState* p) noexcept { return static_cast<GraphicsPipelineVulkan*>(p); }
constexpr auto CastVkObject(ComputePipelineState* p) noexcept { return static_cast<ComputePipelineVulkan*>(p); }
constexpr auto CastVkObject(AccelerationStructure* p) noexcept { return static_cast<AccelerationStructureVulkan*>(p); }
constexpr auto CastVkObject(AccelerationStructureView* p) noexcept { return static_cast<AccelerationStructureViewVulkan*>(p); }
constexpr auto CastVkObject(RayTracingPipelineState* p) noexcept { return static_cast<RayTracingPipelineVulkan*>(p); }
constexpr auto CastVkObject(ShaderBindingTable* p) noexcept { return static_cast<ShaderBindingTableVulkan*>(p); }
constexpr auto CastVkObject(SwapChainSyncObject* p) noexcept { return static_cast<SwapChainSyncObjectVulkan*>(p); }

struct VulkanBindingParameterInfo {
    BindingParameterId Id{0};
    BindingParameterKind Kind{BindingParameterKind::UNKNOWN};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    uint32_t SetIndex{0};
    uint32_t BindingIndex{0};
    ResourceBindType ResourceType{ResourceBindType::UNKNOWN};
    uint32_t DescriptorCount{0};
    bool IsReadOnly{true};
    VkDescriptorType DescriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};
    uint32_t Offset{0};
    uint32_t Size{0};
};

struct VulkanMergedBindingLayout {
    BindingLayout Layout{};
    vector<VulkanBindingParameterInfo> Parameters{};
    uint32_t SetLayoutCount{0};
};

std::optional<VulkanMergedBindingLayout> BuildMergedBindingLayoutVulkan(std::span<Shader*> shaders) noexcept;

}  // namespace radray::render::vulkan

#endif
