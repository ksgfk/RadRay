#pragma once

#include <array>

#include <radray/render/common.h>

#include "vulkan_helper.h"

namespace radray::render::vulkan {

using DeviceFuncTable = VolkDeviceTable;

class InstanceVulkan;
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

    Nullable<shared_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override;

    Nullable<shared_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept override;

    Nullable<shared_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override;

    Nullable<shared_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override;

    Nullable<shared_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override;

    Nullable<shared_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept override;

    Nullable<shared_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override;

public:
    Nullable<unique_ptr<FenceVulkan>> CreateLegacyFence(VkFenceCreateFlags flags) noexcept;

    Nullable<unique_ptr<SemaphoreVulkan>> CreateLegacySemaphore(VkSemaphoreCreateFlags flags) noexcept;

    Nullable<unique_ptr<TimelineSemaphoreVulkan>> CreateTimelineSemaphore(uint64_t initValue) noexcept;

    Nullable<unique_ptr<BufferViewVulkan>> CreateBufferView(const VkBufferViewCreateInfo& info) noexcept;

    Nullable<unique_ptr<DescriptorSetLayoutVulkan>> CreateDescriptorSetLayout(const VkDescriptorSetLayoutCreateInfo& info) noexcept;

    Nullable<unique_ptr<RenderPassVulkan>> CreateRenderPass(const VkRenderPassCreateInfo& info) noexcept;

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
    VkPhysicalDeviceProperties _properties;
    ExtPropertiesVulkan _extProperties;
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

    Nullable<unique_ptr<CommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept override;

    void EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept override;

    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override;

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

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    CommandBufferVulkan* _cmdBuffer;
    unique_ptr<RenderPassVulkan> _pass;
    unique_ptr<FrameBufferVulkan> _framebuffer;
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

    void CopyFromHost(std::span<byte> data, uint64_t offset) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkBuffer _buffer;
    VmaAllocation _allocation;
    VmaAllocationInfo _allocInfo;
    BufferDescriptor _mdesc;
    VkBufferCreateInfo _rawInfo;
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
    shared_ptr<BufferViewVulkan> _texelView;
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
    TextureDescriptor _mdesc;
    VkImageCreateInfo _rawInfo;
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

class DescriptorSetLayoutVulkan final : public RenderBase {
public:
    DescriptorSetLayoutVulkan(
        DeviceVulkan* device,
        VkDescriptorSetLayout layout) noexcept;

    ~DescriptorSetLayoutVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceVulkan* _device;
    VkDescriptorSetLayout _layout;
    vector<RootSignatureBinding> _rootBindings;
    vector<RootSignatureSetElement> _bindingElements;
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

bool GlobalInitVulkan(const VulkanBackendInitDescriptor& desc);

void GlobalTerminateVulkan();

Nullable<shared_ptr<DeviceVulkan>> CreateDeviceVulkan(const VulkanDeviceDescriptor& desc);

}  // namespace radray::render::vulkan
