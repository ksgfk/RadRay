#pragma once

#ifdef RADRAY_ENABLE_METAL

#ifndef __OBJC__
#error "This header can only be included in Objective-C++ files."
#endif
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <radray/render/common.h>
#include <radray/render/backend/metal_impl_cpp.h>
#include <dispatch/dispatch.h>

namespace radray::render::metal {

class DeviceMetal;
class CmdQueueMetal;
class CmdBufferMetal;
class FenceMetal;
class SemaphoreMetal;
class SwapChainMetal;
class BufferMetal;
class TextureMetal;
class ShaderMetal;
class RootSignatureMetal;
class GraphicsPipelineStateMetal;
class ComputePipelineStateMetal;
class GraphicsCmdEncoderMetal;
class ComputeCmdEncoderMetal;
class TextureViewMetal;
class BufferViewMetal;
class SamplerMetal;
class DescriptorSetMetal;
class BindlessArrayMetal;

class DeviceMetal final : public Device {
public:
    ~DeviceMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    RenderBackend GetBackend() noexcept override { return RenderBackend::Metal; }

    DeviceDetail GetDetail() const noexcept override;

    Nullable<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept override;

    Nullable<unique_ptr<Fence>> CreateFence() noexcept override;

    Nullable<unique_ptr<Semaphore>> CreateSemaphoreDevice() noexcept override;

    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override;

    Nullable<unique_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override;

    Nullable<unique_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<DescriptorSet>> CreateDescriptorSet(RootSignature* rootSig, uint32_t index) noexcept override;

    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override;

    Nullable<unique_ptr<BindlessArray>> CreateBindlessArray(const BindlessArrayDescriptor& desc) noexcept override;

public:
    void DestroyImpl() noexcept;

    id<MTLDevice> _device{nil};
    std::array<vector<unique_ptr<CmdQueueMetal>>, (size_t)QueueType::MAX_COUNT> _queues;
    DeviceDetail _detail{};
};

class CmdQueueMetal final : public CommandQueue {
public:
    ~CmdQueueMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override;

    void Wait() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLCommandQueue> _queue{nil};
    id<MTLCommandBuffer> _lastCmdBuffer{nil};
};

class CmdBufferMetal final : public CommandBuffer {
public:
    ~CmdBufferMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Begin() noexcept override;

    void End() noexcept override;

    void ResourceBarrier(std::span<const BarrierBufferDescriptor> buffers, std::span<const BarrierTextureDescriptor> textures) noexcept override;

    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept override;

    void EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept override;

    Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept override;

    void EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept override;

    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override;

    void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    CmdQueueMetal* _queue{nullptr};
    id<MTLCommandBuffer> _cmdBuffer{nil};
};

class FenceMetal final : public Fence {
public:
    ~FenceMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    FenceStatus GetStatus() const noexcept override;

    void Wait() noexcept override;

    void Reset() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLSharedEvent> _event{nil};
    uint64_t _targetValue{0};
    bool _submitted{false};
    dispatch_semaphore_t _semaphore{nullptr};
};

class SemaphoreMetal final : public Semaphore {
public:
    ~SemaphoreMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLEvent> _event{nil};
    uint64_t _value{0};
};

class SwapChainMetal final : public SwapChain {
public:
    ~SwapChainMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Nullable<Texture*> AcquireNext(Nullable<Semaphore*> signalSemaphore, Nullable<Fence*> signalFence) noexcept override;

    void Present(std::span<Semaphore*> waitSemaphores) noexcept override;

    Nullable<Texture*> GetCurrentBackBuffer() const noexcept override;

    uint32_t GetCurrentBackBufferIndex() const noexcept override;

    uint32_t GetBackBufferCount() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    CmdQueueMetal* _presentQueue{nullptr};
    CAMetalLayer* _layer{nil};
    id<CAMetalDrawable> _currentDrawable{nil};
    unique_ptr<TextureMetal> _currentTexture;
    uint32_t _backBufferCount{0};
    uint32_t _currentIndex{0};
};

class BufferMetal final : public Buffer {
public:
    ~BufferMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void* Map(uint64_t offset, uint64_t size) noexcept override;

    void Unmap(uint64_t offset, uint64_t size) noexcept override;

    BufferDescriptor GetDesc() const noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLBuffer> _buffer{nil};
    BufferDescriptor _desc{};
};

class TextureMetal final : public Texture {
public:
    ~TextureMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLTexture> _texture{nil};
    TextureDescriptor _desc{};
    bool _isExternalOwned{false};
};

class ShaderMetal final : public Shader {
public:
    ~ShaderMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLLibrary> _library{nil};
};

struct RootSignatureSetLayoutMetal {
    vector<RootSignatureSetElement> Elements;
    bool IsBindless{false};
};

class RootSignatureMetal final : public RootSignature {
public:
    ~RootSignatureMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<RootSignatureSetLayoutMetal> _setLayouts;
    vector<RootSignatureStaticSampler> _staticSamplers;
    std::optional<RootSignatureConstant> _pushConstant;
};

class GraphicsPipelineStateMetal final : public GraphicsPipelineState {
public:
    ~GraphicsPipelineStateMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLRenderPipelineState> _pipelineState{nil};
    id<MTLDepthStencilState> _depthStencilState{nil};
    MTLPrimitiveType _primitiveType{MTLPrimitiveTypeTriangle};
    MTLCullMode _cullMode{MTLCullModeNone};
    MTLWinding _winding{MTLWindingClockwise};
    MTLTriangleFillMode _fillMode{MTLTriangleFillModeFill};
    float _depthBiasConstant{0};
    float _depthBiasSlopeScale{0};
    float _depthBiasClamp{0};
};

class ComputePipelineStateMetal final : public ComputePipelineState {
public:
    ~ComputePipelineStateMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLComputePipelineState> _pipelineState{nil};
    MTLSize _threadGroupSize{1, 1, 1};
};

class GraphicsCmdEncoderMetal final : public GraphicsCommandEncoder {
public:
    ~GraphicsCmdEncoderMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    CommandBuffer* GetCommandBuffer() const noexcept override;

    void BindRootSignature(RootSignature* rootSig) noexcept override;

    void PushConstant(const void* data, size_t length) noexcept override;

    void BindRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept override;

    void BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept override;

    void BindBindlessArray(uint32_t slot, BindlessArray* array) noexcept override;

    void SetViewport(Viewport vp) noexcept override;

    void SetScissor(Rect rect) noexcept override;

    void BindVertexBuffer(std::span<const VertexBufferView> vbv) noexcept override;

    void BindIndexBuffer(IndexBufferView ibv) noexcept override;

    void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept override;

    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept override;

    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept override;

public:
    void DestroyImpl() noexcept;

    CmdBufferMetal* _cmdBuffer{nullptr};
    id<MTLRenderCommandEncoder> _encoder{nil};
    RootSignatureMetal* _boundRootSig{nullptr};
    MTLPrimitiveType _primitiveType{MTLPrimitiveTypeTriangle};
    id<MTLBuffer> _indexBuffer{nil};
    MTLIndexType _indexType{MTLIndexTypeUInt32};
    uint32_t _indexBufferOffset{0};
};

class ComputeCmdEncoderMetal final : public ComputeCommandEncoder {
public:
    ~ComputeCmdEncoderMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    CommandBuffer* GetCommandBuffer() const noexcept override;

    void BindRootSignature(RootSignature* rootSig) noexcept override;

    void PushConstant(const void* data, size_t length) noexcept override;

    void BindRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept override;

    void BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept override;

    void BindBindlessArray(uint32_t slot, BindlessArray* array) noexcept override;

    void BindComputePipelineState(ComputePipelineState* pso) noexcept override;

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept override;

    void SetThreadGroupSize(uint32_t x, uint32_t y, uint32_t z) noexcept override;

public:
    void DestroyImpl() noexcept;

    CmdBufferMetal* _cmdBuffer{nullptr};
    id<MTLComputeCommandEncoder> _encoder{nil};
    RootSignatureMetal* _boundRootSig{nullptr};
    MTLSize _threadGroupSize{1, 1, 1};
};

class TextureViewMetal final : public TextureView {
public:
    ~TextureViewMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    TextureMetal* _texture{nullptr};
    id<MTLTexture> _textureView{nil};
    TextureViewDescriptor _desc{};
};

class BufferViewMetal final : public BufferView {
public:
    ~BufferViewMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    BufferMetal* _buffer{nullptr};
    BufferViewDescriptor _desc{};
};

class SamplerMetal final : public Sampler {
public:
    ~SamplerMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLSamplerState> _sampler{nil};
};

struct DescriptorBindingMetal {
    uint32_t Slot{0};
    uint32_t Index{0};
    ResourceView* View{nullptr};
};

class DescriptorSetMetal final : public DescriptorSet {
public:
    ~DescriptorSetMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetResource(uint32_t slot, uint32_t index, ResourceView* view) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    RootSignatureMetal* _rootSig{nullptr};
    uint32_t _setIndex{0};
    vector<DescriptorBindingMetal> _bindings;
};

class BindlessArrayMetal final : public BindlessArray {
public:
    ~BindlessArrayMetal() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetBuffer(uint32_t slot, BufferView* bufView) noexcept override;

    void SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept override;

public:
    void DestroyImpl() noexcept;

    DeviceMetal* _device{nullptr};
    id<MTLBuffer> _argumentBuffer{nil};
    uint32_t _size{0};
    BindlessSlotType _slotType{BindlessSlotType::Multiple};
};

constexpr auto CastMtlObject(Device* p) noexcept { return static_cast<DeviceMetal*>(p); }
constexpr auto CastMtlObject(CommandQueue* p) noexcept { return static_cast<CmdQueueMetal*>(p); }
constexpr auto CastMtlObject(CommandBuffer* p) noexcept { return static_cast<CmdBufferMetal*>(p); }
constexpr auto CastMtlObject(Fence* p) noexcept { return static_cast<FenceMetal*>(p); }
constexpr auto CastMtlObject(Semaphore* p) noexcept { return static_cast<SemaphoreMetal*>(p); }
constexpr auto CastMtlObject(SwapChain* p) noexcept { return static_cast<SwapChainMetal*>(p); }
constexpr auto CastMtlObject(Buffer* p) noexcept { return static_cast<BufferMetal*>(p); }
constexpr auto CastMtlObject(Texture* p) noexcept { return static_cast<TextureMetal*>(p); }
constexpr auto CastMtlObject(Shader* p) noexcept { return static_cast<ShaderMetal*>(p); }
constexpr auto CastMtlObject(RootSignature* p) noexcept { return static_cast<RootSignatureMetal*>(p); }
constexpr auto CastMtlObject(GraphicsPipelineState* p) noexcept { return static_cast<GraphicsPipelineStateMetal*>(p); }
constexpr auto CastMtlObject(ComputePipelineState* p) noexcept { return static_cast<ComputePipelineStateMetal*>(p); }
constexpr auto CastMtlObject(TextureView* p) noexcept { return static_cast<TextureViewMetal*>(p); }
constexpr auto CastMtlObject(BufferView* p) noexcept { return static_cast<BufferViewMetal*>(p); }
constexpr auto CastMtlObject(Sampler* p) noexcept { return static_cast<SamplerMetal*>(p); }
constexpr auto CastMtlObject(DescriptorSet* p) noexcept { return static_cast<DescriptorSetMetal*>(p); }
constexpr auto CastMtlObject(BindlessArray* p) noexcept { return static_cast<BindlessArrayMetal*>(p); }

}  // namespace radray::render::metal

#endif  // RADRAY_ENABLE_METAL
