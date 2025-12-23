#pragma once

#ifdef RADRAY_ENABLE_VULKAN

#include <array>

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
class FenceVulkan;
class SemaphoreVulkan;
class TimelineSemaphoreVulkan;
class SurfaceVulkan;
class SwapChainVulkan;
class BufferVulkan;
class BufferViewVulkan;
class SimulateBufferViewVulkan;
class ImageVulkan;
class ImageViewVulkan;
class DescriptorSetLayoutVulkan;
class PipelineLayoutVulkan;
class GraphicsPipelineVulkan;
class ShaderModuleVulkan;
class DescriptorSetVulkan;
class DescriptorSetAllocatorVulkan;
class SamplerVulkan;

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

    Nullable<unique_ptr<Fence>> CreateFence(uint64_t initValue) noexcept override;

    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override;

    Nullable<unique_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override;

    Nullable<unique_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<DescriptorSet>> CreateDescriptorSet(RootSignature* rootSig, uint32_t index) noexcept override;

    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override;

public:
    Nullable<unique_ptr<FenceVulkan>> CreateLegacyFence(VkFenceCreateFlags flags) noexcept;

    Nullable<unique_ptr<SemaphoreVulkan>> CreateLegacySemaphore(VkSemaphoreCreateFlags flags) noexcept;
    Nullable<unique_ptr<TimelineSemaphoreVulkan>> CreateTimelineSemaphore(uint64_t initValue) noexcept;

    Nullable<unique_ptr<BufferViewVulkan>> CreateBufferView(const VkBufferViewCreateInfo& info) noexcept;

    Nullable<unique_ptr<RenderPassVulkan>> CreateRenderPass(const VkRenderPassCreateInfo& info) noexcept;

    Nullable<unique_ptr<DescriptorSetLayoutVulkan>> CreateDescriptorSetLayout(const RootSignatureDescriptorSet& desc) noexcept;

    Nullable<unique_ptr<SamplerVulkan>> CreateSamplerVulkan(const SamplerDescriptor& desc) noexcept;
    Nullable<unique_ptr<SamplerVulkan>> CreateSamplerVulkan(const VkSamplerCreateInfo& desc) noexcept;

    Nullable<unique_ptr<CommandBufferVulkan>> CreateCommandBufferVulkan(QueueVulkan* queue) noexcept;

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

    void ResourceBarrier(std::span<const BarrierBufferDescriptor> buffers, std::span<const BarrierTextureDescriptor> textures) noexcept override;

    Nullable<unique_ptr<CommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept override;

    void EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept override;

    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override;

    void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    QueueVulkan* _queue;
    unique_ptr<CommandPoolVulkan> _cmdPool;
    VkCommandBuffer _cmdBuffer;
    vector<unique_ptr<CommandEncoder>> _endedEncoders;
};

class SimulateCommandEncoderVulkan final : public CommandEncoder {
public:
    SimulateCommandEncoderVulkan(
        DeviceVulkan* device,
        CommandBufferVulkan* cmdBuffer) noexcept;

    ~SimulateCommandEncoderVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetViewport(Viewport vp) noexcept override;

    void SetScissor(Rect rect) noexcept override;

    void BindVertexBuffer(std::span<const VertexBufferView> vbv) noexcept override;

    void BindIndexBuffer(IndexBufferView ibv) noexcept override;

    void BindRootSignature(RootSignature* rootSig) noexcept override;

    void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept override;

    void PushConstant(const void* data, size_t length) noexcept override;

    void BindRootDescriptor(uint32_t slot, ResourceView* view) noexcept override;

    void BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept override;

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

    Nullable<Texture*> AcquireNext() noexcept override;

    void Present() noexcept override;

    Nullable<Texture*> GetCurrentBackBuffer() const noexcept override;

    uint32_t GetCurrentBackBufferIndex() const noexcept override;

    uint32_t GetBackBufferCount() const noexcept override;

    SwapChainDescriptor GetDesc() const noexcept override;

public:
    void DestroyImpl() noexcept;

    class Frame {
    public:
        unique_ptr<ImageVulkan> image;
        shared_ptr<FenceVulkan> fence;
        shared_ptr<SemaphoreVulkan> imageAvailableSemaphore;
        shared_ptr<SemaphoreVulkan> renderFinishedSemaphore;
        unique_ptr<CommandBufferVulkan> internalCmdBuffer;
    };

    DeviceVulkan* _device;
    QueueVulkan* _queue;
    unique_ptr<SurfaceVulkan> _surface;
    VkSwapchainKHR _swapchain;
    vector<Frame> _frames;
    uint32_t _currentTextureIndex{0};
    uint32_t _currentFrameIndex{0};
    SwapChainDescriptor _desc;
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

    BufferDescriptor GetDesc() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkBuffer _buffer;
    VmaAllocation _allocation;
    VmaAllocationInfo _allocInfo;
    string _name;
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
        BufferRange range) noexcept;

    ~SimulateBufferViewVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    BufferVulkan* _buffer;
    BufferRange _range;
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

    void DangerousDestroy() noexcept;

public:
    void DestroyImpl() noexcept;

    void SetExtData(const TextureDescriptor& desc) noexcept;

    DeviceVulkan* _device;
    VkImage _image;
    VmaAllocation _allocation;
    VmaAllocationInfo _allocInfo;
    TextureDescriptor _mdesc;
    string _name;
    VkFormat _rawFormat;
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
    DescriptorSetLayoutBindingVulkanContainer(const VkDescriptorSetLayoutBinding& binding, vector<unique_ptr<SamplerVulkan>> immutableSamplers) noexcept;

    uint32_t binding;
    VkDescriptorType descriptorType;
    uint32_t descriptorCount;
    VkShaderStageFlags stageFlags;
    vector<unique_ptr<SamplerVulkan>> immutableSamplers;
};

class DescriptorSetLayoutVulkan final : public RenderBase {
public:
    DescriptorSetLayoutVulkan(
        DeviceVulkan* device,
        VkDescriptorSetLayout layout) noexcept;

    ~DescriptorSetLayoutVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkDescriptorSetLayout _layout;
    vector<DescriptorSetLayoutBindingVulkanContainer> _bindings;
};

class PipelineLayoutVulkan final : public RootSignature {
public:
    PipelineLayoutVulkan(
        DeviceVulkan* device,
        VkPipelineLayout layout) noexcept;

    ~PipelineLayoutVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkPipelineLayout _layout;
    vector<unique_ptr<DescriptorSetLayoutVulkan>> _descSetLayouts;
    std::optional<VkPushConstantRange> _pushConst;
};

class GraphicsPipelineVulkan final : public GraphicsPipelineState {
public:
    GraphicsPipelineVulkan(
        DeviceVulkan* device,
        VkPipeline pipeline) noexcept;

    ~GraphicsPipelineVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkPipeline _pipeline;
    unique_ptr<RenderPassVulkan> _renderPass;
};

class ShaderModuleVulkan final : public Shader {
public:
    ShaderModuleVulkan(
        DeviceVulkan* device,
        VkShaderModule shaderModule) noexcept;

    ~ShaderModuleVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkShaderModule _shaderModule;
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

    DescriptorSetAllocatorVulkan(DeviceVulkan* device, uint32_t keepFreePages) noexcept;
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
    uint32_t _keepFreePages;
};

class DescriptorSetVulkan final : public DescriptorSet {
public:
    DescriptorSetVulkan(
        DeviceVulkan* device,
        DescriptorSetLayoutVulkan* layout,
        DescriptorSetAllocatorVulkan* allocator,
        DescriptorSetAllocatorVulkan::Allocation allocation) noexcept;

    ~DescriptorSetVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetResource(uint32_t slot, uint32_t index, ResourceView* view) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    DescriptorSetLayoutVulkan* _layout;
    DescriptorSetAllocatorVulkan* _allocator;
    DescriptorSetAllocatorVulkan::Allocation _allocation;
};

class SamplerVulkan final : public Sampler {
public:
    SamplerVulkan(
        DeviceVulkan* device,
        VkSampler sampler) noexcept;
    ~SamplerVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkSampler _sampler;
    SamplerDescriptor _mdesc;
};

Nullable<shared_ptr<DeviceVulkan>> CreateDeviceVulkan(const VulkanDeviceDescriptor& desc);

Nullable<unique_ptr<InstanceVulkanImpl>> CreateVulkanInstanceImpl(const VulkanInstanceDescriptor& desc);

void DestroyVulkanInstanceImpl(unique_ptr<InstanceVulkan> instance) noexcept;

constexpr auto CastVkObject(CommandQueue* p) noexcept { return static_cast<QueueVulkan*>(p); }
constexpr auto CastVkObject(CommandBuffer* p) noexcept { return static_cast<CommandBufferVulkan*>(p); }
constexpr auto CastVkObject(Fence* p) noexcept { return static_cast<TimelineSemaphoreVulkan*>(p); }
constexpr auto CastVkObject(Buffer* p) noexcept { return static_cast<BufferVulkan*>(p); }
constexpr auto CastVkObject(Texture* p) noexcept { return static_cast<ImageVulkan*>(p); }
constexpr auto CastVkObject(TextureView* p) noexcept { return static_cast<ImageViewVulkan*>(p); }
constexpr auto CastVkObject(Shader* p) noexcept { return static_cast<ShaderModuleVulkan*>(p); }
constexpr auto CastVkObject(RootSignature* p) noexcept { return static_cast<PipelineLayoutVulkan*>(p); }
constexpr auto CastVkObject(GraphicsPipelineState* p) noexcept { return static_cast<GraphicsPipelineVulkan*>(p); }
constexpr auto CastVkObject(DescriptorSet* p) noexcept { return static_cast<DescriptorSetVulkan*>(p); }

}  // namespace radray::render::vulkan

#endif
