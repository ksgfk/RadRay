#pragma once

#ifdef RADRAY_ENABLE_D3D12

#include <array>
#include <atomic>

#include <radray/allocator.h>

#include <radray/render/backend/d3d12_helper.h>
#include <radray/render/rhi.h>
#include <radray/render/sampler_cache.h>

namespace radray::render::d3d12 {

class DescriptorHeap;
struct DescriptorHeapView;
class CpuDescriptorAllocator;
class DXGIFactoryImpl;
class DeviceD3D12;
class CmdQueueD3D12;
class FenceD3D12;
class CmdListD3D12;
class CommandAllocatorD3D12;
class CmdRenderPassD3D12;
class SwapChainD3D12;
class BufferD3D12;
class QueryPoolD3D12;
class TextureD3D12;
class TextureViewD3D12;
class RootSigD3D12;
class BindingGroupLayoutD3D12;
class ShaderParameterSetD3D12;
class GraphicsPsoD3D12;
class ComputePsoD3D12;
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

using CpuDescriptorHeapViewRAII = DescriptorHeapViewRAII<CpuDescriptorAllocator, CpuDescriptorAllocator::Allocation>;

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

using GpuDescriptorHeapViewRAII = DescriptorHeapViewRAII<GpuDescriptorAllocator, GpuDescriptorAllocator::Allocation>;

class DXGIFactoryImpl final : public DXGIFactory {
public:
    DXGIFactoryImpl(
        ComPtr<IDXGIFactory4> factory,
        const DXGIFactoryDescriptor& desc,
        bool validationEnabled) noexcept;

    ~DXGIFactoryImpl() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    vector<DXGIAdapterInfo> GetAdapters() const noexcept override;

    std::optional<uint32_t> SelectHighPerformanceAdapter() const noexcept override;
    bool IsValidationEnabled() const noexcept override { return _validationEnabled; }

public:
    void DestroyImpl() noexcept;

    ComPtr<IDXGIFactory4> _factory;
    DXGIFactoryDescriptor _desc;
    bool _validationEnabled{false};
};

class DeviceD3D12 final : public Device {
public:
    DeviceD3D12(
        ComPtr<ID3D12Device> device,
        ComPtr<IDXGIFactory4> dxgiFactory,
        ComPtr<IDXGIAdapter1> dxgiAdapter,
        ComPtr<D3D12MA::Allocator> mainAlloc,
        const DXGIFactoryDescriptor& diagnostics) noexcept;

    ~DeviceD3D12() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    RenderBackend GetBackend() noexcept override { return RenderBackend::D3D12; }

    DeviceDetail GetDetail() const noexcept override;

    const RenderDeviceCapabilities& GetCapabilities() const noexcept override { return _capabilities; }

    TextureSupport QueryTextureSupport(const TextureSupportQuery& query) const noexcept override;

    Nullable<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    Nullable<unique_ptr<CommandAllocator>> CreateCommandAllocator(CommandQueue* queue) noexcept override;

    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandAllocator* allocator) noexcept override;

    Nullable<unique_ptr<Fence>> CreateFence() noexcept override;

    Nullable<unique_ptr<QueryPool>> CreateQueryPool(const QueryPoolDescriptor& desc) noexcept override;

    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override;

    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override;

    Nullable<unique_ptr<BindingGroupLayout>> CreateBindingGroupLayout(const BindingGroupLayoutDescriptor& desc) noexcept override;

    Nullable<unique_ptr<PipelineLayout>> CreatePipelineLayout(const PipelineLayoutDescriptor& desc) noexcept override;

    Nullable<unique_ptr<ShaderParameterSet>> CreateShaderParameterSet(const ShaderParameterSetDescriptor& desc) noexcept override;

    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept override;

    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override;

    Nullable<Sampler*> GetOrCreateSampler(const SamplerDescriptor& desc) noexcept override;

public:
    void DestroyImpl() noexcept;

    Nullable<unique_ptr<FenceD3D12>> CreateFenceD3D12(uint64_t initValue) noexcept;

    bool CreateQueues(const array<uint32_t, static_cast<size_t>(QueueType::MAX_COUNT)>& counts) noexcept;

    void TryDrainValidationMessages() noexcept;

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
    SamplerCache _samplerCache;
    RenderDeviceCapabilities _capabilities;
    vector<std::pair<TextureSupportQuery, TextureSupport>> _textureSupportCache;
    CD3DX12FeatureSupport _features;
    bool _isAllowTearing = false;
    bool _isDebugLayerEnabled = false;

    struct ValidationMessages;
    unique_ptr<ValidationMessages> _validationMessages;
    std::optional<DWORD> _infoQueueCallbackCookie;
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
    ~CmdQueueD3D12() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override;

    void Wait() noexcept override;

    QueueType GetQueueType() const noexcept override;

    TimestampClockCalibrationResult GetTimestampClockCalibration() const noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12CommandQueue> _queue;
    unique_ptr<FenceD3D12> _fence;
    D3D12_COMMAND_LIST_TYPE _type;
#ifdef RADRAY_ENABLE_PROFILER
    // Profiler GPU timestamp context for this queue; null when profiling is disabled or unsupported.
    void* _profilerContext{nullptr};
#endif
};

/**
 * Fence 保存的 _fenceValue 表示下一个可用的值, _fenceValue - 1 表示已提交的的值
 */
class FenceD3D12 final : public Fence {
public:
    FenceD3D12(
        DeviceD3D12* device,
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

    bool WaitFor(uint64_t value, uint64_t timeoutMs) noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12Fence> _fence;
    mutable std::atomic<uint64_t> _drainedCompletedValue{0};
    uint64_t _fenceValue{0};
    Win32Event _event{};
};

class CommandAllocatorD3D12 final : public CommandAllocator {
public:
    CommandAllocatorD3D12(DeviceD3D12* device, CmdQueueD3D12* queue, ComPtr<ID3D12CommandAllocator> allocator) noexcept;
    ~CommandAllocatorD3D12() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    void Reset() noexcept override;
    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    CmdQueueD3D12* _queue;
    ComPtr<ID3D12CommandAllocator> _native;
};

class CmdListD3D12 final : public CommandBuffer {
public:
    CmdListD3D12(
        DeviceD3D12* _device,
        CmdQueueD3D12* queue,
        CommandAllocatorD3D12* allocator,
        ComPtr<ID3D12GraphicsCommandList> cmdList,
        D3D12_COMMAND_LIST_TYPE type) noexcept;
    ~CmdListD3D12() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

    void Begin() noexcept override;

    void End() noexcept override;

    void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept override;

    void PushDebugGroup(std::string_view name) noexcept override;

    void PopDebugGroup() noexcept override;

    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept override;

    void EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept override;

    Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept override;

    void EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept override;

    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override;

    void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept override;

    bool CopyBufferToTextureRegion(const BufferToTextureCopyDescriptor& desc) noexcept override;

    void CopyTextureToBuffer(Buffer* dst, uint64_t dstOffset, Texture* src, SubresourceRange srcRange) noexcept override;

    void CopyTextureToTexture(const TextureCopyDescriptor& desc) noexcept override;

    void ResolveTexture(const TextureResolveDescriptor& desc) noexcept override;

    void ResetQueryPool(QueryPool* pool, uint32_t firstIndex, uint32_t count) noexcept override;

    void WriteTimestamp(const QueryTimestampDescriptor& desc) noexcept override;

    void ResolveQueryData(const QueryResolveDescriptor& desc) noexcept override;

public:
    DeviceD3D12* _device;
#ifdef RADRAY_ENABLE_PROFILER
    CmdQueueD3D12* _queue;
#endif
    CommandAllocatorD3D12* _allocator;
    ComPtr<ID3D12GraphicsCommandList> _cmdList;
    ComPtr<ID3D12GraphicsCommandList4> _renderPassList;
    D3D12_COMMAND_LIST_TYPE _type;
#ifdef RADRAY_ENABLE_PROFILER
    // Profiler GPU zones opened by PushDebugGroup. D3D12 forbids query resolution inside a render pass, so
    // zones open only outside one, and pops issued while a render pass is recording are deferred to
    // EndRenderPass; a merged raster group therefore reports as a single zone named after its first pass.
    struct ProfilerZoneStack;
    unique_ptr<ProfilerZoneStack> _profilerZones;
    bool _inRenderPass{false};
    uint32_t _deferredZonePops{0};
    uint32_t _suppressedZonePushes{0};
#endif
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

    void BindVertexBuffers(std::span<const VertexBufferBinding> bindings) noexcept override;

    void BindIndexBuffer(IndexBufferView ibv) noexcept override;

    void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept override;

    void BindShaderParameterSet(uint32_t groupIndex, ShaderParameterSet* set, std::span<const ShaderParameterDynamicOffset> dynamicOffsets) noexcept override;

    bool SetPushConstants(uint32_t pushId, uint32_t byteOffset, std::span<const byte> data) noexcept override;

    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept override;

    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept override;

    void DrawIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount) noexcept override;

    void DrawIndexedIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount) noexcept override;

public:
    CmdListD3D12* _cmdList;
    GraphicsPsoD3D12* _boundPso{nullptr};
    RootSigD3D12* _boundRs{nullptr};
};

class CmdComputePassD3D12 final : public ComputeCommandEncoder {
public:
    explicit CmdComputePassD3D12(CmdListD3D12* cmdList) noexcept;
    ~CmdComputePassD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    CommandBuffer* GetCommandBuffer() const noexcept override;

    void BindComputePipelineState(ComputePipelineState* pso) noexcept override;

    void BindShaderParameterSet(uint32_t groupIndex, ShaderParameterSet* set, std::span<const ShaderParameterDynamicOffset> dynamicOffsets) noexcept override;

    bool SetPushConstants(uint32_t pushId, uint32_t byteOffset, std::span<const byte> data) noexcept override;

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept override;

    void DispatchIndirect(Buffer* argumentBuffer, uint64_t argumentOffset) noexcept override;

public:
    CmdListD3D12* _cmdList;
    ComputePsoD3D12* _boundPso{nullptr};
    RootSigD3D12* _boundRs{nullptr};
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

    SwapChainPresentResult Present(SwapChainFrame frame) noexcept override;

    SwapChainPresentResult DiscardAcquiredFrame(SwapChainFrame frame) noexcept override;

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
    uint64_t _outstandingFrameToken{0};
    bool _requiresRecreateAfterDiscard{false};
    bool _frameLatencyReady{false};
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
};

class QueryPoolD3D12 final : public QueryPool {
public:
    QueryPoolD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12QueryHeap> heap,
        QueryType type,
        uint32_t count) noexcept;
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
    QueryType _type;
    uint32_t _count;
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

    Device* GetDevice() const noexcept override { return _device; }

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

    TextureViewDescriptor GetDesc() const noexcept override { return _desc; }

public:
    DeviceD3D12* _device;
    TextureD3D12* _texture;
    CpuDescriptorHeapViewRAII _heapView;
    TextureViewDescriptor _desc;
    DXGI_FORMAT _rawFormat;
};

class Dxil final : public Shader {
public:
    Dxil() noexcept = default;
    template <class TIter>
    requires std::is_same_v<typename std::iterator_traits<TIter>::value_type, byte>
    Dxil(
        TIter begin,
        TIter end,
        ShaderStages stages) noexcept
        : _dxil{begin, end},
          _stages(stages) {}
    ~Dxil() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    ShaderStages GetStages() const noexcept override { return _stages; }

    D3D12_SHADER_BYTECODE ToByteCode() const noexcept;

public:
    vector<byte> _dxil;
    ShaderStages _stages{ShaderStage::UNKNOWN};
};

// Group-local descriptor storage is independent of pipeline root parameter indices.
struct ShaderParameterSetLayoutEntryD3D12 {
    uint32_t Binding{0};
    BindingKind LogicalKind{BindingKind::CBuffer};
    D3D12BufferPlacement Placement{D3D12BufferPlacement::Table};
    uint32_t Count{0};
    bool Immutable{false};
    uint32_t TableIndex{0};
    uint32_t TableOffset{0};
    uint32_t RootIndex{0};
    bool IsRootDescriptor() const noexcept { return Placement == D3D12BufferPlacement::RootDescriptor; }
};
struct DescriptorTableBindingD3D12 {
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    D3D12_SHADER_VISIBILITY Visibility{D3D12_SHADER_VISIBILITY_ALL};
    uint32_t DescriptorCount{0};
    vector<D3D12_DESCRIPTOR_RANGE1> Ranges;
};
class BindingGroupLayoutD3D12 final : public BindingGroupLayout {
public:
    ~BindingGroupLayoutD3D12() noexcept override;
    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    void SetDebugName(std::string_view name) noexcept override;
    BindingGroupLayoutDescriptor GetDesc() const noexcept override;
    DeviceD3D12* _device{nullptr};
    vector<BindingDescriptor> _bindings;
    vector<ShaderParameterSetLayoutEntryD3D12> _entries;
    vector<DescriptorTableBindingD3D12> _tables;
    vector<uint32_t> _rootOrder;
};
struct ShaderParameterGroupLayoutD3D12 {
    uint32_t GroupSlot{0};
    BindingGroupLayoutD3D12* Layout{nullptr};
    vector<uint32_t> TableRoots;
    vector<uint32_t> BufferRoots;
};

struct DescriptorDirtyRangeD3D12 {
    uint32_t Start{0};
    uint32_t Count{0};
    friend bool operator==(const DescriptorDirtyRangeD3D12&, const DescriptorDirtyRangeD3D12&) noexcept = default;
};

class DescriptorDirtyRangesD3D12 {
public:
    // The caller supplies a nonempty range contained in its table allocation.
    void Add(uint32_t start, uint32_t count = 1) noexcept;
    std::optional<DescriptorDirtyRangeD3D12> GetCopySpan(std::span<const uint8_t> initialized) const noexcept;
    vector<DescriptorDirtyRangeD3D12> Ranges;
};

struct DescriptorTableStorageD3D12 {
    CpuDescriptorHeapViewRAII Mirror;
    GpuDescriptorHeapViewRAII Descriptors;
    vector<uint8_t> InitializedSlots;
    DescriptorDirtyRangesD3D12 Dirty;
};

struct PushConstantBindingD3D12 {
    uint32_t PushId{0};
    uint32_t RootParameterIndex{0};
};
class RootSigD3D12 final : public PipelineLayout {
public:
    RootSigD3D12() noexcept = default;
    ID3D12RootSignature* GetNative() const noexcept;
    ~RootSigD3D12() noexcept override;
    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    void SetDebugName(std::string_view name) noexcept override;
    PipelineLayoutDescriptor GetDesc() const noexcept override;
    Nullable<const ShaderParameterGroupLayoutD3D12*> FindParameterGroup(uint32_t groupSlot) const noexcept;
    DeviceD3D12* _device{nullptr};
    vector<PipelineLayoutGroupDescriptor> _groups;
    vector<PushConstantDescriptor> _pushes;
    vector<ShaderParameterGroupLayoutD3D12> _parameterGroups;
    vector<PushConstantBindingD3D12> _pushConstantBindings;
    ComPtr<ID3D12RootSignature> _rootSignature;
};

class ShaderParameterSetD3D12 final : public ShaderParameterSet {
public:
    ShaderParameterSetD3D12() noexcept = default;
    ~ShaderParameterSetD3D12() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    bool Set(uint32_t bindingId, uint32_t arrayElement, ShaderParameterValue value) noexcept override;

    bool FlushWrites() noexcept override;

    void ResetBindingsForReuse() noexcept override;

public:
    DeviceD3D12* _device{nullptr};
    BindingGroupLayoutD3D12* _layout{nullptr};
    // root descriptor 的 GPU 地址，与所属组的 _rootOrder 一一对应：
    // 第 i 项对应 _entries[_rootOrder[i]]，不是原生 root parameter 下标。
    // Set 写入 buffer GPU 地址 + BufferRange.Offset；Bind 时再加本次 dynamic offset。
    // 创建时为 0，使用前必须 Set；descriptor table 的参数不存放在这里。
    vector<D3D12_GPU_VIRTUAL_ADDRESS> _rootBindings;
    vector<DescriptorTableStorageD3D12> _tables;
    // 以下三个数组是 FlushWrites 稀疏批量复制的临时参数，长度相同，第 i 项对应同一段 dirty range。
    // 每处理一个需要稀疏复制的 table 时重新填充，保留容量复用；不是持久的 dirty 状态。
    // 源区间起点：该 table 的 CPU mirror 中，每段 dirty range 第一个 descriptor 的 CPU handle。
    vector<D3D12_CPU_DESCRIPTOR_HANDLE> _copySources;
    // 目标区间起点：该 table 的 shader-visible 区间中对应位置的 CPU handle。
    // CopyDescriptors 的源和目标均使用 CPU handle；命令绑定 table 时才使用 GPU handle。
    vector<D3D12_CPU_DESCRIPTOR_HANDLE> _copyDestinations;
    // 每段复制的 descriptor 个数，同时用作源区间和目标区间长度；单位不是字节。
    vector<UINT> _copyCounts;
};

class GraphicsPsoD3D12 final : public GraphicsPipelineState {
public:
    GraphicsPsoD3D12(
        DeviceD3D12* device,
        RootSigD3D12* layout,
        ComPtr<ID3D12PipelineState> pso,
        std::array<std::optional<uint32_t>, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexStrides,
        D3D12_PRIMITIVE_TOPOLOGY topo) noexcept;
    ~GraphicsPsoD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    RootSigD3D12* _layout;
    ComPtr<ID3D12PipelineState> _pso;
    std::array<std::optional<uint32_t>, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> _vertexStrides{};
    D3D12_PRIMITIVE_TOPOLOGY _topo;
};

class ComputePsoD3D12 final : public ComputePipelineState {
public:
    ComputePsoD3D12(
        DeviceD3D12* device,
        RootSigD3D12* layout,
        ComPtr<ID3D12PipelineState> pso) noexcept;
    ~ComputePsoD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetDebugName(std::string_view name) noexcept override;

public:
    DeviceD3D12* _device;
    RootSigD3D12* _layout;
    ComPtr<ID3D12PipelineState> _pso;
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

Nullable<shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc);

Nullable<unique_ptr<DXGIFactory>> CreateDXGIFactory(const DXGIFactoryDescriptor& desc);

constexpr auto CastD3D12Object(Device* v) noexcept { return static_cast<DeviceD3D12*>(v); }
constexpr auto CastD3D12Object(CommandQueue* v) noexcept { return static_cast<CmdQueueD3D12*>(v); }
constexpr auto CastD3D12Object(Buffer* v) noexcept { return static_cast<BufferD3D12*>(v); }
constexpr auto CastD3D12Object(Texture* v) noexcept { return static_cast<TextureD3D12*>(v); }
constexpr auto CastD3D12Object(Fence* v) noexcept { return static_cast<FenceD3D12*>(v); }
constexpr auto CastD3D12Object(CommandAllocator* v) noexcept { return static_cast<CommandAllocatorD3D12*>(v); }
constexpr auto CastD3D12Object(CommandBuffer* v) noexcept { return static_cast<CmdListD3D12*>(v); }
constexpr auto CastD3D12Object(BindingGroupLayout* v) noexcept { return static_cast<BindingGroupLayoutD3D12*>(v); }
constexpr auto CastD3D12Object(PipelineLayout* v) noexcept { return static_cast<RootSigD3D12*>(v); }
constexpr auto CastD3D12Object(ShaderParameterSet* v) noexcept { return static_cast<ShaderParameterSetD3D12*>(v); }
constexpr auto CastD3D12Object(Shader* v) noexcept { return static_cast<Dxil*>(v); }
constexpr auto CastD3D12Object(Sampler* v) noexcept { return static_cast<SamplerD3D12*>(v); }
constexpr auto CastD3D12Object(TextureView* v) noexcept { return static_cast<TextureViewD3D12*>(v); }
constexpr auto CastD3D12Object(GraphicsPipelineState* v) noexcept { return static_cast<GraphicsPsoD3D12*>(v); }
constexpr auto CastD3D12Object(ComputePipelineState* v) noexcept { return static_cast<ComputePsoD3D12*>(v); }
constexpr auto CastD3D12Object(QueryPool* v) noexcept { return static_cast<QueryPoolD3D12*>(v); }
constexpr auto CastD3D12Object(DXGIFactory* v) noexcept { return static_cast<DXGIFactoryImpl*>(v); }

}  // namespace radray::render::d3d12

#endif
