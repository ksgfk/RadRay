#pragma once

#ifdef RADRAY_ENABLE_D3D12

#include <array>
#include <limits>

#include <radray/allocator.h>
#include <radray/hash.h>

#include <radray/render/backend/d3d12_helper.h>
#include <radray/render/common.h>

namespace radray::render::d3d12 {

class DescriptorHeap;
struct DescriptorHeapView;
class CpuDescriptorAllocator;
class GpuDescriptorAllocator;
class DXGIFactoryImpl;
class DeviceD3D12;
class CmdQueueD3D12;
class FenceD3D12;
class CmdListD3D12;
class CmdRenderPassD3D12;
class SwapChainD3D12;
class BufferD3D12;
class QueryPoolD3D12;
class TextureD3D12;
class TextureViewD3D12;
class RenderPassD3D12;
class FramebufferD3D12;
class RootSigD3D12;
class DescriptorPoolD3D12;
class BindingGroupD3D12;
class GraphicsPsoD3D12;
class ComputePsoD3D12;
class AccelerationStructureD3D12;
class RayTracingPsoD3D12;
class SamplerD3D12;

class DescriptorHeap {
public:
    DescriptorHeap(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept;
    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;
    DescriptorHeap(DescriptorHeap&&) noexcept = default;
    DescriptorHeap& operator=(DescriptorHeap&&) noexcept = default;

    ID3D12DescriptorHeap* Get() const noexcept;

    D3D12_DESCRIPTOR_HEAP_TYPE GetHeapType() const noexcept;

    UINT GetLength() const noexcept;

    bool IsShaderVisible() const noexcept;

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu(UINT index) const noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu(UINT index) const noexcept;

    void Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept;

    void Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept;

    void Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept;

    void Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept;

    void Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept;

    void Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept;

    void CopyTo(UINT start, UINT count, DescriptorHeap* dst, UINT dstStart) noexcept;

private:
    ID3D12Device* _device;
    ComPtr<ID3D12DescriptorHeap> _heap;
    D3D12_DESCRIPTOR_HEAP_DESC _desc;
    D3D12_CPU_DESCRIPTOR_HANDLE _cpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE _gpuStart;
    UINT _incrementSize;
};

struct DescriptorHeapView {
    DescriptorHeap* Heap{nullptr};
    UINT Start{0};
    UINT Length{0};

    static constexpr DescriptorHeapView Invalid() noexcept {
        return {nullptr, 0, 0};
    }

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu() const noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu() const noexcept;

    bool IsValid() const noexcept;
};

template <class T>
concept is_desc_heap_view_like = requires(T v) {
    { v.Heap } -> std::convertible_to<DescriptorHeap*>;
    { v.Start } -> std::convertible_to<UINT>;
    { v.Length } -> std::convertible_to<UINT>;
    { T::Invalid() } -> std::same_as<T>;
};

template <class TAllocator, class TAllocation>
requires is_allocator<TAllocator, TAllocation> && is_desc_heap_view_like<TAllocation>
class DescriptorHeapViewRAII {
public:
    DescriptorHeapViewRAII() noexcept
        : _allocator(nullptr),
          _allocation(TAllocation::Invalid()) {}

    DescriptorHeapViewRAII(TAllocator* allocator, const TAllocation& allocation) noexcept
        : _allocator(allocator),
          _allocation(allocation) {}

    DescriptorHeapViewRAII(const DescriptorHeapViewRAII&) = delete;
    DescriptorHeapViewRAII(DescriptorHeapViewRAII&& other) noexcept
        : _allocator(other._allocator),
          _allocation(other._allocation) {
        other._allocation = TAllocation::Invalid();
        other._allocator = nullptr;
    }

    DescriptorHeapViewRAII& operator=(const DescriptorHeapViewRAII&) = delete;
    DescriptorHeapViewRAII& operator=(DescriptorHeapViewRAII&& other) noexcept {
        DescriptorHeapViewRAII tmp{std::move(other)};
        swap(*this, tmp);
        return *this;
    }

    ~DescriptorHeapViewRAII() noexcept { this->Destroy(); }

    bool IsValid() const noexcept {
        return _allocator != nullptr;
    }

    void Destroy() noexcept {
        if (IsValid()) {
            _allocator->Destroy(_allocation);
            _allocation = TAllocation::Invalid();
            _allocator = nullptr;
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu() const noexcept {
        DescriptorHeapView view{_allocation.Heap, _allocation.Start, _allocation.Length};
        return view.HandleGpu();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu() const noexcept {
        DescriptorHeapView view{_allocation.Heap, _allocation.Start, _allocation.Length};
        return view.HandleCpu();
    }

    UINT GetStart() const noexcept { return _allocation.Start; }

    UINT GetLength() const noexcept { return _allocation.Length; }

    DescriptorHeap* GetHeap() const noexcept { return _allocation.Heap; }

    template <class T, class U>
    void CopyTo(UINT start, UINT count, DescriptorHeapViewRAII<T, U>& dst, UINT dstStart) noexcept {
        this->GetHeap()->CopyTo(this->GetStart() + start, count, dst.GetHeap(), dst.GetStart() + dstStart);
    }

    friend constexpr void swap(DescriptorHeapViewRAII& lhs, DescriptorHeapViewRAII& rhs) noexcept {
        using std::swap;
        swap(lhs._allocator, rhs._allocator);
        swap(lhs._allocation, rhs._allocation);
    }

private:
    TAllocator* _allocator;
    TAllocation _allocation;
};

class CpuDescriptorAllocator {
public:
    class Page {
    public:
        unique_ptr<DescriptorHeap> Heap;
        ComPtr<D3D12MA::VirtualBlock> Allocator;
        size_t Capacity = 0;
        size_t Used = 0;

        Page(
            unique_ptr<DescriptorHeap> heap,
            ComPtr<D3D12MA::VirtualBlock> allocator,
            size_t capacity) noexcept;
        Page(const Page&) = delete;
        Page(Page&&) noexcept = delete;
        ~Page() noexcept = default;
    };

    struct Allocation {
        DescriptorHeap* Heap{nullptr};
        UINT Start{0};
        UINT Length{0};

        Page* PagePtr = nullptr;
        size_t Offset = 0;
        D3D12MA::VirtualAllocation VirtualAlloc{};

        constexpr static Allocation Invalid() noexcept {
            return {nullptr, 0, 0, nullptr, 0, {}};
        }

        constexpr bool IsValid() const noexcept {
            return Heap != nullptr && Length != 0 && PagePtr != nullptr && VirtualAlloc.AllocHandle != 0;
        }
    };

    static_assert(is_desc_heap_view_like<Allocation>, "CpuDescriptorAllocator::Allocation is not a desc heap view like");

    CpuDescriptorAllocator(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT basicSize,
        UINT keepFreePages) noexcept;

    std::optional<Allocation> Allocate(UINT count) noexcept;

    void Destroy(Allocation allocation) noexcept;

private:
    void TryReleaseFreePages() noexcept;

    ID3D12Device* _device = nullptr;
    vector<unique_ptr<Page>> _pages;
    size_t _hint = 0;
    D3D12_DESCRIPTOR_HEAP_TYPE _type{};
    UINT _basicSize = 0;
    UINT _freePageKeepCount = 1;
};

static_assert(is_allocator<CpuDescriptorAllocator, CpuDescriptorAllocator::Allocation>, "CpuDescriptorAllocator is not an allocator");

class GpuDescriptorAllocator {
public:
    struct Allocation {
        DescriptorHeap* Heap{nullptr};
        UINT Start{0};
        UINT Length{0};

        FirstFitAllocator::Allocation ParentAllocation;

        static constexpr Allocation Invalid() noexcept {
            return {nullptr, 0, 0, FirstFitAllocator::Allocation::Invalid()};
        }
    };

    static_assert(is_desc_heap_view_like<Allocation>, "GpuDescriptorAllocator::Allocation is not a desc heap view like");

    GpuDescriptorAllocator(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT size) noexcept;

    std::optional<GpuDescriptorAllocator::Allocation> Allocate(UINT count) noexcept;

    void Destroy(GpuDescriptorAllocator::Allocation allocation) noexcept;

    ID3D12DescriptorHeap* GetNative() const noexcept { return _heap->Get(); }
    DescriptorHeap* GetHeap() const noexcept { return _heap.get(); }

private:
    ID3D12Device* _device;
    unique_ptr<DescriptorHeap> _heap;
    FirstFitAllocator _allocator;
};

static_assert(is_allocator<GpuDescriptorAllocator, GpuDescriptorAllocator::Allocation>, "GpuDescriptorAllocator is not an allocator");

using CpuDescriptorHeapViewRAII = DescriptorHeapViewRAII<CpuDescriptorAllocator, CpuDescriptorAllocator::Allocation>;
using GpuDescriptorHeapViewRAII = DescriptorHeapViewRAII<GpuDescriptorAllocator, GpuDescriptorAllocator::Allocation>;

class DXGIFactoryImpl final : public DXGIFactory {
public:
    DXGIFactoryImpl(
        ComPtr<IDXGIFactory4> factory,
        const DXGIFactoryDescriptor& desc) noexcept;

    ~DXGIFactoryImpl() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    vector<DXGIAdapterInfo> GetAdapters() const noexcept override;

    std::optional<uint32_t> SelectHighPerformanceAdapter() const noexcept override;

public:
    void DestroyImpl() noexcept;

    ComPtr<IDXGIFactory4> _factory;
    DXGIFactoryDescriptor _desc;
};

class DeviceD3D12 final : public Device {
public:
    DeviceD3D12(
        ComPtr<ID3D12Device> device,
        ComPtr<IDXGIFactory4> dxgiFactory,
        ComPtr<IDXGIAdapter1> dxgiAdapter,
        ComPtr<D3D12MA::Allocator> mainAlloc) noexcept;

    ~DeviceD3D12() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    RenderBackend GetBackend() noexcept override { return RenderBackend::D3D12; }

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
    void DestroyImpl() noexcept;

    Nullable<unique_ptr<FenceD3D12>> CreateFenceD3D12(uint64_t initValue) noexcept;

    Nullable<unique_ptr<RootSigD3D12>> CreateRootSignatureInternal(const PipelineLayoutDescriptor& desc) noexcept;

    void TryDrainValidationMessages();

    ComPtr<ID3D12Device> _device;
    ComPtr<IDXGIFactory4> _dxgiFactory;
    ComPtr<IDXGIAdapter1> _dxgiAdapter;
    ComPtr<D3D12MA::Allocator> _mainAlloc;
    ComPtr<ID3D12CommandSignature> _drawIndirectSignature;
    ComPtr<ID3D12CommandSignature> _drawIndexedIndirectSignature;
    ComPtr<ID3D12CommandSignature> _dispatchIndirectSignature;
    std::array<vector<unique_ptr<CmdQueueD3D12>>, (size_t)QueueType::MAX_COUNT> _queues;
    unique_ptr<CpuDescriptorAllocator> _cpuResAlloc;
    unique_ptr<CpuDescriptorAllocator> _cpuRtvAlloc;
    unique_ptr<CpuDescriptorAllocator> _cpuDsvAlloc;
    unique_ptr<CpuDescriptorAllocator> _cpuSamplerAlloc;
    unique_ptr<GpuDescriptorAllocator> _gpuResHeap;
    unique_ptr<GpuDescriptorAllocator> _gpuSamplerHeap;
    DeviceDetail _detail;
    CD3DX12FeatureSupport _features;
    bool _isAllowTearing = false;
    bool _isDebugLayerEnabled = false;

    DWORD _infoQueueCallbackCookie{0};
    Nullable<RenderLogCallback> _logCallback{};
    Nullable<void*> _logUserData{};
};

class CmdQueueD3D12 final : public CommandQueue {
public:
    CmdQueueD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12CommandQueue> queue,
        D3D12_COMMAND_LIST_TYPE type,
        unique_ptr<FenceD3D12> fence) noexcept;
    ~CmdQueueD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override;

    void Wait() noexcept override;

    QueueType GetQueueType() const noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12CommandQueue> _queue;
    unique_ptr<FenceD3D12> _fence;
    D3D12_COMMAND_LIST_TYPE _type;
};

/**
 * Fence 保存的 _fenceValue 表示下一个可用的值, _fenceValue - 1 表示已提交的的值
 */
class FenceD3D12 final : public Fence {
public:
    FenceD3D12(
        ComPtr<ID3D12Fence> fence,
        Win32Event event) noexcept;
    virtual ~FenceD3D12() noexcept = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    uint64_t GetCompletedValue() const noexcept override;

    uint64_t GetLastSignaledValue() const noexcept override;

    void Wait() noexcept override;

    void Wait(uint64_t value) noexcept override;

public:
    ComPtr<ID3D12Fence> _fence;
    uint64_t _fenceValue{0};
    Win32Event _event{};
};

class CmdListD3D12 final : public CommandBuffer {
public:
    CmdListD3D12(
        DeviceD3D12* _device,
        ComPtr<ID3D12CommandAllocator> cmdAlloc,
        ComPtr<ID3D12GraphicsCommandList> cmdList,
        D3D12_COMMAND_LIST_TYPE type) noexcept;
    ~CmdListD3D12() noexcept override = default;

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

    ComPtr<ID3D12GraphicsCommandList4> QueryCommandList4() noexcept;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12CommandAllocator> _cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> _cmdList;
    D3D12_COMMAND_LIST_TYPE _type;
    vector<unique_ptr<Buffer>> _keepAliveBuffers;
};

class CmdRenderPassD3D12 final : public GraphicsCommandEncoder {
public:
    explicit CmdRenderPassD3D12(CmdListD3D12* cmdList) noexcept;
    ~CmdRenderPassD3D12() noexcept override = default;

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
    CmdListD3D12* _cmdList;
    GraphicsPsoD3D12* _boundPso{nullptr};
    vector<VertexBufferView> _boundVbvs;
    RootSigD3D12* _boundRootSig{nullptr};
};

class CmdComputePassD3D12 final : public ComputeCommandEncoder {
public:
    explicit CmdComputePassD3D12(CmdListD3D12* cmdList) noexcept;
    ~CmdComputePassD3D12() noexcept override = default;

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
    CmdListD3D12* _cmdList;
    RootSigD3D12* _boundRootSig{nullptr};
};

class CmdRayTracingPassD3D12 final : public RayTracingCommandEncoder {
public:
    explicit CmdRayTracingPassD3D12(CmdListD3D12* cmdList) noexcept;
    ~CmdRayTracingPassD3D12() noexcept override = default;

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
    CmdListD3D12* _cmdList;
    RootSigD3D12* _boundRootSig{nullptr};
    RayTracingPsoD3D12* _boundRtPso{nullptr};
};

class SwapChainD3D12 final : public SwapChain {
public:
    SwapChainD3D12(
        DeviceD3D12* device,
        ComPtr<IDXGISwapChain3> swapchain,
        const SwapChainDescriptor& desc) noexcept;
    ~SwapChainD3D12() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    SwapChainAcquireResult AcquireNext(uint64_t timeoutMs) noexcept override;

    SwapChainPresentResult Present(SwapChainFrame&& frame) noexcept override;

    bool Recreate(uint32_t width, uint32_t height, TextureFormat format, PresentMode presentMode) noexcept override;

    uint32_t GetBackBufferCount() const noexcept override;

    SwapChainDescriptor GetDesc() const noexcept override;

public:
    class Frame {
    public:
        unique_ptr<TextureD3D12> image;
    };

    DeviceD3D12* _device;
    CmdQueueD3D12* _presentQueue{nullptr};
    ComPtr<IDXGISwapChain3> _swapchain;
    HANDLE _frameLatencyEvent{nullptr};
    vector<Frame> _frames;
    const void* _nativeHandler{nullptr};
    PresentMode _mode{PresentMode::FIFO};
    bool _hasOutstandingFrame{false};
    uint64_t _outstandingFrameToken{0};
    uint32_t _outstandingBackBufferIndex{std::numeric_limits<uint32_t>::max()};
    TextureFormat _reqFormat{TextureFormat::UNKNOWN};
};

class BufferD3D12 final : public Buffer {
public:
    BufferD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12Resource> buf,
        ComPtr<D3D12MA::Allocation> alloc) noexcept;
    ~BufferD3D12() noexcept override = default;

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
    DeviceD3D12* _device;
    ComPtr<ID3D12Resource> _buf;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_DESC _rawDesc;
    D3D12_GPU_VIRTUAL_ADDRESS _gpuAddr;
    string _name;
    uint64_t _reqSize{0};
    MemoryType _memory{};
    BufferUses _usage{BufferUse::UNKNOWN};
    ResourceHints _hints{};
    void* _mappedData{nullptr};
    bool _mapped{false};
};

class QueryPoolD3D12 final : public QueryPool {
public:
    QueryPoolD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12QueryHeap> heap,
        QueryPoolDescriptor desc) noexcept;
    ~QueryPoolD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    QueryType GetType() const noexcept override;

    uint32_t GetCount() const noexcept override;

    TimestampQueryCalibration GetTimestampCalibration(CommandQueue* queue) const noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12QueryHeap> _heap;
    QueryPoolDescriptor _desc;
};

class TextureD3D12 final : public Texture {
public:
    TextureD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12Resource> tex,
        ComPtr<D3D12MA::Allocation> alloc) noexcept;
    ~TextureD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    TextureDescriptor GetDesc() const noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12Resource> _tex;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_DESC _rawDesc;
    string _name;
    TextureDimension _dimension{TextureDimension::UNKNOWN};
    TextureFormat _format{TextureFormat::UNKNOWN};
    MemoryType _memory{MemoryType::Device};
    TextureUses _usage{TextureUse::UNKNOWN};
    ResourceHints _hints{ResourceHint::None};
};

class TextureViewD3D12 final : public TextureView {
public:
    TextureViewD3D12(
        DeviceD3D12* device,
        TextureD3D12* texture,
        CpuDescriptorHeapViewRAII heapView) noexcept;
    ~TextureViewD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    TextureD3D12* _texture;
    CpuDescriptorHeapViewRAII _heapView;
    TextureViewDescriptor _desc;
    DXGI_FORMAT _rawFormat;
};

class RenderPassD3D12 final : public RenderPass {
public:
    explicit RenderPassD3D12(const RenderPassDescriptor& desc);
    ~RenderPassD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    void SetDebugName(std::string_view name) noexcept override;
    RenderPassDescriptor GetDesc() const noexcept override;

public:
    vector<RenderPassColorAttachmentDescriptor> _colorAttachments;
    std::optional<RenderPassDepthStencilAttachmentDescriptor> _depthStencilAttachment;
    string _name;
    bool _valid{true};
};

class FramebufferD3D12 final : public Framebuffer {
public:
    explicit FramebufferD3D12(const FramebufferDescriptor& desc);
    ~FramebufferD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    void SetDebugName(std::string_view name) noexcept override;
    FramebufferDescriptor GetDesc() const noexcept override;

public:
    RenderPass* _pass{nullptr};
    vector<TextureView*> _colorAttachments;
    TextureView* _depthStencilAttachment{nullptr};
    uint32_t _width{0};
    uint32_t _height{0};
    uint32_t _layers{1};
    string _name;
    bool _valid{true};
};

class Dxil final : public Shader {
public:
    Dxil() noexcept = default;
    template <class TIter>
    requires std::is_same_v<typename std::iterator_traits<TIter>::value_type, byte>
    Dxil(
        TIter begin,
        TIter end,
        ShaderStages stages,
        std::optional<ShaderReflectionDesc> reflection) noexcept
        : _dxil{begin, end},
          _stages(stages),
          _reflection(std::move(reflection)) {}
    ~Dxil() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    ShaderStages GetStages() const noexcept override { return _stages; }

    Nullable<const ShaderReflectionDesc*> GetReflection() const noexcept override {
        return _reflection.has_value() ? Nullable<const ShaderReflectionDesc*>{&_reflection.value()} : Nullable<const ShaderReflectionDesc*>{};
    }

    D3D12_SHADER_BYTECODE ToByteCode() const noexcept;

public:
    vector<byte> _dxil;
    ShaderStages _stages{ShaderStage::UNKNOWN};
    std::optional<ShaderReflectionDesc> _reflection{};
};

class RootSigD3D12 final : public PipelineLayout {
public:
    /**
     * RootSigD3D12 internal record, indexed by ParameterIndex.
     * - Info: 对外暴露的参数信息, 同时作为 FindParameter 返回指针的稳定存储
     * - RootParameterIndex: 指向 _rootParams; 静态采样器为 max()
     * - RangeIndex: 指向 _ranges[RootParameterIndex] 的对应 range; 非 descriptor table 参数为 max()
     * descriptor 数量/表内偏移/push constant 大小等一律从 _rootParams/_ranges 派生, 不再重复保存.
     * register space 反查 (root parameter 下标/bindless set/descriptor 计数) 一律扫描本结构派生.
     */
    struct ParameterBinding {
        ShaderParameterInfo Info{};
        uint32_t ParameterIndex{0};
        uint32_t RegisterSpace{0};
        uint32_t ShaderRegister{0};
        bool IsStaticSampler{false};
        bool HasDynamicOffset{false};
        BindlessSlotType BindlessSlotType{BindlessSlotType::Multiple};
        uint32_t RootParameterIndex{std::numeric_limits<uint32_t>::max()};
        uint32_t RangeIndex{std::numeric_limits<uint32_t>::max()};
    };

    RootSigD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12RootSignature> rootSig,
        vector<ParameterBinding> parameterBindings,
        vector<D3D12_ROOT_PARAMETER1> rootParams,
        vector<vector<D3D12_DESCRIPTOR_RANGE1>> ranges,
        uint32_t registerSpaceCount) noexcept;
    ~RootSigD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    vector<ShaderParameterInfo> GetParameters() const noexcept override;

    Nullable<const ShaderParameterInfo*> FindParameter(std::string_view name) const noexcept override;

    std::optional<ShaderBindingLocation> FindBindingLocation(std::string_view name) const noexcept override;

    vector<BindingGroupLayout> GetBindingGroupLayouts() const noexcept override;

    vector<PushConstantRange> GetPushConstantRanges() const noexcept override;

    Nullable<const ParameterBinding*> FindParameterBinding(uint32_t parameterIndex) const noexcept;

    Nullable<const ParameterBinding*> FindParameterBinding(
        uint32_t registerSpace,
        uint32_t shaderRegister) const noexcept;

    vector<const ParameterBinding*> GetDynamicBufferBindings(uint32_t registerSpace) const noexcept;

    bool HasBindlessSet(uint32_t registerSpace) const noexcept;

    Nullable<const ParameterBinding*> FindBindlessSet(uint32_t registerSpace) const noexcept;

    uint32_t GetDescriptorCount(const ParameterBinding& binding) const noexcept;

    uint32_t GetDescriptorHeapOffset(const ParameterBinding& binding) const noexcept;

    uint32_t GetPushConstantSize(const ParameterBinding& binding) const noexcept;

    uint32_t GetDescriptorSetResourceCount(uint32_t registerSpace) const noexcept;

    uint32_t GetDescriptorSetSamplerCount(uint32_t registerSpace) const noexcept;

    std::optional<uint32_t> FindDescriptorTableRootParameter(uint32_t registerSpace, ShaderParameterKind kind) const noexcept;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12RootSignature> _rootSig;
    // Single internal record array, indexed by ParameterIndex.
    vector<ParameterBinding> _parameterBindings{};
    // 创建 ID3D12RootSignature 时使用的 D3D12 结构, 作为 descriptor 数量/偏移/push constant 大小的唯一数据源.
    // _ranges 必须指针稳定 (构造后不再修改), _rootParams 中 descriptor table 的 pDescriptorRanges 指向它.
    vector<D3D12_ROOT_PARAMETER1> _rootParams{};
    vector<vector<D3D12_DESCRIPTOR_RANGE1>> _ranges{};
    uint32_t _registerSpaceCount{0};
};

class GraphicsPsoD3D12 final : public GraphicsPipelineState {
public:
    GraphicsPsoD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12PipelineState> pso,
        vector<uint64_t> arrayStrides,
        D3D12_PRIMITIVE_TOPOLOGY topo) noexcept;
    ~GraphicsPsoD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12PipelineState> _pso;
    vector<uint64_t> _arrayStrides;
    D3D12_PRIMITIVE_TOPOLOGY _topo;
};

class ComputePsoD3D12 final : public ComputePipelineState {
public:
    ComputePsoD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12PipelineState> pso) noexcept;
    ~ComputePsoD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12PipelineState> _pso;
};

class AccelerationStructureD3D12 final : public AccelerationStructure {
public:
    AccelerationStructureD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12Resource> buffer,
        ComPtr<D3D12MA::Allocation> alloc,
        const AccelerationStructureDescriptor& desc,
        uint64_t asSize) noexcept;
    ~AccelerationStructureD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12Resource> _buffer;
    ComPtr<D3D12MA::Allocation> _alloc;
    AccelerationStructureDescriptor _desc;
    uint64_t _asSize{0};
    D3D12_GPU_VIRTUAL_ADDRESS _gpuAddr{0};
    string _name;
};

class AccelerationStructureViewD3D12 final : public AccelerationStructureView {
public:
    AccelerationStructureViewD3D12(
        DeviceD3D12* device,
        AccelerationStructureD3D12* target,
        CpuDescriptorHeapViewRAII heapView) noexcept;
    ~AccelerationStructureViewD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    AccelerationStructureD3D12* _target;
    CpuDescriptorHeapViewRAII _heapView;
    AccelerationStructureViewDescriptor _desc;
};

class RayTracingPsoD3D12 final : public RayTracingPipelineState {
public:
    RayTracingPsoD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12StateObject> stateObject,
        ComPtr<ID3D12StateObjectProperties> stateProps,
        RootSigD3D12* rootSig) noexcept;
    ~RayTracingPsoD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    ShaderBindingTableRequirements GetShaderBindingTableRequirements() const noexcept override;

    std::optional<vector<byte>> GetShaderBindingTableHandle(std::string_view shaderName) const noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12StateObject> _stateObject;
    ComPtr<ID3D12StateObjectProperties> _stateProps;
    RootSigD3D12* _rootSig{nullptr};
    unordered_map<string, std::array<byte, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES>> _shaderIdentifiers;
};

class ShaderBindingTableD3D12 final : public ShaderBindingTable {
public:
    ShaderBindingTableD3D12(
        DeviceD3D12* device,
        RayTracingPsoD3D12* pipeline,
        unique_ptr<Buffer> buffer,
        const ShaderBindingTableDescriptor& desc,
        uint64_t recordStride) noexcept;
    ~ShaderBindingTableD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    bool Build(std::span<const ShaderBindingTableBuildEntry> entries) noexcept override;

    bool IsBuilt() const noexcept override;

    ShaderBindingTableRegions GetRegions() const noexcept override;

public:
    DeviceD3D12* _device;
    RayTracingPsoD3D12* _pipeline;
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

struct PendingDescriptorCopyD3D12 {
    D3D12_CPU_DESCRIPTOR_HANDLE Source{};
    D3D12_CPU_DESCRIPTOR_HANDLE Destination{};
    D3D12_DESCRIPTOR_HEAP_TYPE Type{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
};

struct DescriptorSetSlotD3D12 {
    GpuDescriptorHeapViewRAII ResHeapView;
    GpuDescriptorHeapViewRAII SamplerHeapView;
    vector<uint8_t> ResourceWritten;
    vector<uint8_t> SamplerWritten;

    bool HasStorage() const noexcept { return ResHeapView.IsValid() || SamplerHeapView.IsValid(); }
};

class DescriptorPoolD3D12 final : public DescriptorPool {
public:
    explicit DescriptorPoolD3D12(const DescriptorPoolDescriptor& desc) noexcept;
    ~DescriptorPoolD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    void SetDebugName(std::string_view name) noexcept override;
    bool Reset() noexcept override;
    DescriptorPoolDescriptor GetDesc() const noexcept override { return _desc; }
    uint32_t GetAllocatedBindingGroupCount() const noexcept override { return 0; }

public:
    bool _valid{true};
    DescriptorPoolDescriptor _desc{};
    string _name{};
};

class BindingGroupD3D12 final : public BindingGroup {
public:
    BindingGroupD3D12(
        DeviceD3D12* device,
        RootSigD3D12* layout,
        uint32_t groupIndex,
        DescriptorSetSlotD3D12 slot,
        uint32_t parameterCount,
        uint32_t resourceDescriptorCount,
        uint32_t samplerDescriptorCount) noexcept;
    ~BindingGroupD3D12() noexcept override;

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
    void FlushDescriptorCopies() noexcept;
    const BufferBindingDescriptor* GetDynamicBuffer(uint32_t parameterIndex) const noexcept;

    DescriptorSetSlotD3D12& GetSlot() noexcept { return _slot; }
    const DescriptorSetSlotD3D12& GetSlot() const noexcept { return _slot; }
    BindlessArray* GetBindlessArray() const noexcept { return _bindlessArray; }

public:
    void StageDescriptorCopy(PendingDescriptorCopyD3D12 copy) noexcept;

    DeviceD3D12* _device{nullptr};
    RootSigD3D12* _layout{nullptr};
    uint32_t _groupIndex{0};
    DescriptorSetSlotD3D12 _slot{};
    vector<std::optional<BufferBindingDescriptor>> _dynamicBuffers{};
    BindlessArray* _bindlessArray{nullptr};
    vector<PendingDescriptorCopyD3D12> _pendingDescriptorCopies{};
    vector<D3D12_CPU_DESCRIPTOR_HANDLE> _descriptorCopySources{};
    vector<D3D12_CPU_DESCRIPTOR_HANDLE> _descriptorCopyDestinations{};
    uint32_t _resourceDescriptorCount{0};
    uint32_t _samplerDescriptorCount{0};
    string _name{};
};

class SamplerD3D12 final : public Sampler {
public:
    SamplerD3D12(
        DeviceD3D12* device,
        CpuDescriptorHeapViewRAII heapView) noexcept;
    ~SamplerD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    CpuDescriptorHeapViewRAII _samplerView;
    string _name;
};

class BindlessArrayD3D12 final : public BindlessArray {
public:
    BindlessArrayD3D12(
        DeviceD3D12* device,
        const BindlessArrayDescriptor& desc,
        GpuDescriptorHeapViewRAII resHeap,
        GpuDescriptorHeapViewRAII samplerHeap) noexcept;
    ~BindlessArrayD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    void SetBuffer(uint32_t slot, const BufferBindingDescriptor& desc) noexcept override;

    void SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept override;

public:
    enum class SlotKind : uint8_t {
        None,
        Buffer,
        Texture2D,
        Texture3D
    };

    DeviceD3D12* _device;
    BindlessArrayDescriptor _desc{};
    GpuDescriptorHeapViewRAII _resHeap;
    GpuDescriptorHeapViewRAII _samplerHeap;
    vector<SlotKind> _slotKinds{};
    vector<ResourceBindType> _slotResourceTypes{};
    uint32_t _size;
    BindlessSlotType _slotType{BindlessSlotType::Multiple};
    string _name;
};

Nullable<shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc);

Nullable<unique_ptr<DXGIFactory>> CreateDXGIFactory(const DXGIFactoryDescriptor& desc);

constexpr auto CastD3D12Object(Device* v) noexcept { return static_cast<DeviceD3D12*>(v); }
constexpr auto CastD3D12Object(CommandQueue* v) noexcept { return static_cast<CmdQueueD3D12*>(v); }
constexpr auto CastD3D12Object(Buffer* v) noexcept { return static_cast<BufferD3D12*>(v); }
constexpr auto CastD3D12Object(Texture* v) noexcept { return static_cast<TextureD3D12*>(v); }
constexpr auto CastD3D12Object(RenderPass* v) noexcept { return static_cast<RenderPassD3D12*>(v); }
constexpr auto CastD3D12Object(Framebuffer* v) noexcept { return static_cast<FramebufferD3D12*>(v); }
constexpr auto CastD3D12Object(Fence* v) noexcept { return static_cast<FenceD3D12*>(v); }
constexpr auto CastD3D12Object(CommandBuffer* v) noexcept { return static_cast<CmdListD3D12*>(v); }
constexpr auto CastD3D12Object(PipelineLayout* v) noexcept { return static_cast<RootSigD3D12*>(v); }
constexpr auto CastD3D12Object(Shader* v) noexcept { return static_cast<Dxil*>(v); }
constexpr auto CastD3D12Object(Sampler* v) noexcept { return static_cast<SamplerD3D12*>(v); }
constexpr auto CastD3D12Object(TextureView* v) noexcept { return static_cast<TextureViewD3D12*>(v); }
constexpr auto CastD3D12Object(GraphicsPipelineState* v) noexcept { return static_cast<GraphicsPsoD3D12*>(v); }
constexpr auto CastD3D12Object(ComputePipelineState* v) noexcept { return static_cast<ComputePsoD3D12*>(v); }
constexpr auto CastD3D12Object(AccelerationStructure* v) noexcept { return static_cast<AccelerationStructureD3D12*>(v); }
constexpr auto CastD3D12Object(AccelerationStructureView* v) noexcept { return static_cast<AccelerationStructureViewD3D12*>(v); }
constexpr auto CastD3D12Object(RayTracingPipelineState* v) noexcept { return static_cast<RayTracingPsoD3D12*>(v); }
constexpr auto CastD3D12Object(ShaderBindingTable* v) noexcept { return static_cast<ShaderBindingTableD3D12*>(v); }
constexpr auto CastD3D12Object(DescriptorPool* v) noexcept { return static_cast<DescriptorPoolD3D12*>(v); }
constexpr auto CastD3D12Object(BindingGroup* v) noexcept { return static_cast<BindingGroupD3D12*>(v); }
constexpr auto CastD3D12Object(QueryPool* v) noexcept { return static_cast<QueryPoolD3D12*>(v); }
constexpr auto CastD3D12Object(DXGIFactory* v) noexcept { return static_cast<DXGIFactoryImpl*>(v); }

struct D3D12BindingParameterInfo {
    string Name{};
    ShaderParameterKind Kind{ShaderParameterKind::UNKNOWN};
    bool IsAvailable{false};
    uint32_t BindingIndex{0};
    uint32_t ShaderRegister{0};
    uint32_t RegisterSpace{0};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t Count{0};
    bool IsReadOnly{true};
    bool IsBindless{false};
    bool IsStaticSampler{false};
    BindlessSlotType BindlessSlotType{BindlessSlotType::Multiple};
    uint32_t PushConstantOffset{0};
    uint32_t PushConstantSize{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

struct D3D12MergedPipelineLayout {
    vector<ShaderParameterInfo> Parameters{};
    vector<D3D12BindingParameterInfo> D3D12Parameters{};
    uint32_t RegisterSpaceCount{0};
};

std::optional<D3D12MergedPipelineLayout> BuildMergedPipelineLayoutD3D12(
    std::span<Shader*> shaders,
    std::span<const PushConstantBinding> pushConstants = {},
    std::span<const BindingGroupLayout> explicitGroups = {}) noexcept;

}  // namespace radray::render::d3d12

#endif
