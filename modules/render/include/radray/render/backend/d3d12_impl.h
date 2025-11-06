#pragma once

#ifdef RADRAY_ENABLE_D3D12

#include <utility>

#include <radray/allocator.h>

#include <radray/render/backend/d3d12_helper.h>

namespace radray::render::d3d12 {

class DescriptorHeap;
struct DescriptorHeapView;
class CpuDescriptorAllocator;
class GpuDescriptorAllocator;
class DeviceD3D12;
class CmdQueueD3D12;
class FenceD3D12;
class CmdListD3D12;
class CmdRenderPassD3D12;
class SwapChainD3D12;
class BufferD3D12;
class BufferViewD3D12;
class TextureD3D12;
class TextureViewD3D12;
class RootSigD3D12;
class GraphicsPsoD3D12;
class SimulateDescriptorSetLayoutD3D12;
class GpuDescriptorHeapViews;
class SamplerD3D12;

class DescriptorHeap {
public:
    DescriptorHeap(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept;

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
    DescriptorHeap* Heap;
    UINT Start;
    UINT Length;

    static constexpr DescriptorHeapView Invalid() noexcept { return {nullptr, 0, 0}; }

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu() const noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu() const noexcept;

    bool IsValid() const noexcept;
};

template <class TAllocator>
requires is_allocator<TAllocator, DescriptorHeapView>
class DescriptorHeapViewRAII {
public:
    DescriptorHeapViewRAII() noexcept
        : _heapView(DescriptorHeapView::Invalid()),
          _allocator(nullptr) {}
    DescriptorHeapViewRAII(
        DescriptorHeapView heapView,
        TAllocator* allocator) noexcept
        : _heapView(heapView),
          _allocator(allocator) {}
    DescriptorHeapViewRAII(const DescriptorHeapViewRAII&) = delete;
    DescriptorHeapViewRAII(DescriptorHeapViewRAII&& other) noexcept
        : _heapView(other._heapView),
          _allocator(other._allocator) {
        other._heapView = DescriptorHeapView::Invalid();
        other._allocator = nullptr;
    }
    DescriptorHeapViewRAII& operator=(const DescriptorHeapViewRAII&) = delete;
    DescriptorHeapViewRAII& operator=(DescriptorHeapViewRAII&& other) noexcept {
        DescriptorHeapViewRAII tmp{std::move(other)};
        swap(*this, tmp);
        return *this;
    }
    ~DescriptorHeapViewRAII() noexcept { Destroy(); }

    bool IsValid() const noexcept {
        return _heapView.IsValid() && _allocator != nullptr;
    }

    void Destroy() noexcept {
        if (IsValid()) {
            _allocator->Destroy(_heapView);
            _heapView = DescriptorHeapView::Invalid();
            _allocator = nullptr;
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu() const noexcept { return _heapView.HandleGpu(); }

    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu() const noexcept { return _heapView.HandleCpu(); }

    UINT GetStart() const noexcept { return _heapView.Start; }

    UINT GetLength() const noexcept { return _heapView.Length; }

    DescriptorHeap* GetHeap() const noexcept { return _heapView.Heap; }

    template <class T>
    void CopyTo(UINT start, UINT count, DescriptorHeapViewRAII<T>& dst, UINT dstStart) noexcept {
        this->GetHeap()->CopyTo(this->GetStart() + start, count, dst.GetHeap(), dst.GetStart() + dstStart);
    }

    friend constexpr void swap(DescriptorHeapViewRAII& lhs, DescriptorHeapViewRAII& rhs) noexcept {
        using std::swap;
        swap(lhs._heapView, rhs._heapView);
        swap(lhs._allocator, rhs._allocator);
    }

private:
    DescriptorHeapView _heapView;
    TAllocator* _allocator;
};

class CpuDescriptorAllocatorImpl final : public BlockAllocator<BuddyAllocator, DescriptorHeap, CpuDescriptorAllocatorImpl> {
public:
    CpuDescriptorAllocatorImpl(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT basicSize) noexcept;

    ~CpuDescriptorAllocatorImpl() noexcept override = default;

    unique_ptr<DescriptorHeap> CreateHeap(size_t size) noexcept;

    BuddyAllocator CreateSubAllocator(size_t size) noexcept;

private:
    ID3D12Device* _device;
    D3D12_DESCRIPTOR_HEAP_TYPE _type;
};

class CpuDescriptorAllocator {
public:
    CpuDescriptorAllocator(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT basicSize) noexcept;

    std::optional<DescriptorHeapView> Allocate(UINT count) noexcept;

    void Destroy(DescriptorHeapView view) noexcept;

private:
    CpuDescriptorAllocatorImpl _impl;
};

static_assert(is_allocator<CpuDescriptorAllocator, DescriptorHeapView>, "CpuDescriptorAllocator is not an allocator");

class GpuDescriptorAllocator {
public:
    GpuDescriptorAllocator(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT size) noexcept;

    std::optional<DescriptorHeapView> Allocate(UINT count) noexcept;

    void Destroy(DescriptorHeapView view) noexcept;

    ID3D12DescriptorHeap* GetNative() const noexcept { return _heap->Get(); }
    DescriptorHeap* GetHeap() const noexcept { return _heap.get(); }

private:
    ID3D12Device* _device;
    unique_ptr<DescriptorHeap> _heap;
    FreeListAllocator _allocator;
};

static_assert(is_allocator<GpuDescriptorAllocator, DescriptorHeapView>, "GpuDescriptorAllocator is not an allocator");

using CpuDescriptorHeapViewRAII = DescriptorHeapViewRAII<CpuDescriptorAllocator>;
using GpuDescriptorHeapViewRAII = DescriptorHeapViewRAII<GpuDescriptorAllocator>;

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

    Nullable<shared_ptr<DescriptorSetLayout>> CreateDescriptorSetLayout(const RootSignatureBindingSet& desc) noexcept override;

    Nullable<shared_ptr<DescriptorSet>> CreateDescriptorSet(DescriptorSetLayout* layout) noexcept override;

    Nullable<shared_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override;

public:
    void DestroyImpl() noexcept;

    D3D12_RESOURCE_DESC MapTextureDesc(const TextureDescriptor& desc) noexcept;

    ComPtr<ID3D12Device> _device;
    ComPtr<IDXGIFactory4> _dxgiFactory;
    ComPtr<IDXGIAdapter1> _dxgiAdapter;
    ComPtr<D3D12MA::Allocator> _mainAlloc;
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
};

class CmdQueueD3D12 final : public CommandQueue {
public:
    CmdQueueD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12CommandQueue> queue,
        D3D12_COMMAND_LIST_TYPE type,
        shared_ptr<FenceD3D12> fence) noexcept;
    ~CmdQueueD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override;

    void Wait() noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12CommandQueue> _queue;
    shared_ptr<FenceD3D12> _fence;
    D3D12_COMMAND_LIST_TYPE _type;
};

class FenceD3D12 final : public Fence {
public:
    FenceD3D12(
        ComPtr<ID3D12Fence> fence,
        Win32Event event) noexcept;
    ~FenceD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    uint64_t GetCompletedValue() const noexcept override;

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

    void Begin() noexcept override;

    void End() noexcept override;

    void ResourceBarrier(std::span<BarrierBufferDescriptor> buffers, std::span<BarrierTextureDescriptor> textures) noexcept override;

    Nullable<unique_ptr<CommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept override;

    void EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept override;

    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override;

    void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12CommandAllocator> _cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> _cmdList;
    D3D12_COMMAND_LIST_TYPE _type;
};

class CmdRenderPassD3D12 final : public CommandEncoder {
public:
    explicit CmdRenderPassD3D12(CmdListD3D12* cmdList) noexcept;
    ~CmdRenderPassD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetViewport(Viewport vp) noexcept override;

    void SetScissor(Rect rect) noexcept override;

    void BindVertexBuffer(std::span<VertexBufferView> vbv) noexcept override;

    void BindIndexBuffer(IndexBufferView ibv) noexcept override;

    void BindRootSignature(RootSignature* rootSig) noexcept override;

    void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept override;

    void PushConstant(const void* data, size_t length) noexcept override;

    void BindRootDescriptor(uint32_t slot, ResourceView* view) noexcept override;

    void BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept override;

    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept override;

    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept override;

public:
    CmdListD3D12* _cmdList;
    GraphicsPsoD3D12* _boundPso{nullptr};
    vector<VertexBufferView> _boundVbvs;
    RootSigD3D12* _boundRootSig{nullptr};
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

    Nullable<Texture*> AcquireNext() noexcept override;

    void Present() noexcept override;

    Nullable<Texture*> GetCurrentBackBuffer() const noexcept override;

    uint32_t GetCurrentBackBufferIndex() const noexcept override;

    uint32_t GetBackBufferCount() const noexcept override;

    SwapChainDescriptor GetDesc() const noexcept override;

public:
    class Frame {
    public:
        unique_ptr<TextureD3D12> image;
        Win32Event event;
    };

    DeviceD3D12* _device;
    ComPtr<IDXGISwapChain3> _swapchain;
    ComPtr<ID3D12Fence> _fence;
    uint64_t _fenceValue;
    vector<Frame> _frames;
    SwapChainDescriptor _desc;
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

    void Unmap(uint64_t offset, uint64_t size) noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12Resource> _buf;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_DESC _rawDesc;
    D3D12_GPU_VIRTUAL_ADDRESS _gpuAddr;
    BufferDescriptor _desc;
    string _name;
};

class BufferViewD3D12 final : public BufferView {
public:
    BufferViewD3D12(
        DeviceD3D12* device,
        BufferD3D12* buffer,
        CpuDescriptorHeapViewRAII heapView) noexcept;
    ~BufferViewD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    DeviceD3D12* _device;
    BufferD3D12* _buffer;
    CpuDescriptorHeapViewRAII _heapView;
    BufferViewDescriptor _desc;
    DXGI_FORMAT _rawFormat;
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

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12Resource> _tex;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_DESC _rawDesc;
    TextureDescriptor _desc;
    string _name;
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
    Dxil(TIter begin, TIter end) noexcept : _dxil{begin, end} {}
    ~Dxil() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    D3D12_SHADER_BYTECODE ToByteCode() const noexcept;

public:
    vector<byte> _dxil;
};

class RootSigD3D12 final : public RootSignature {
public:
    RootSigD3D12(
        DeviceD3D12* device,
        ComPtr<ID3D12RootSignature> rootSig) noexcept;
    ~RootSigD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12RootSignature> _rootSig;
    vector<RootSignatureSetElementContainer> _bindDescriptors;
    vector<RootSignatureBinding> _rootDescriptors;
    std::optional<RootSignatureConstant> _rootConstant;
    UINT _rootConstStart;
    UINT _rootDescStart;
    UINT _bindDescStart;
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

public:
    DeviceD3D12* _device;
    ComPtr<ID3D12PipelineState> _pso;
    vector<uint64_t> _arrayStrides;
    D3D12_PRIMITIVE_TOPOLOGY _topo;
};

class SimulateDescriptorSetLayoutD3D12 final : public DescriptorSetLayout {
public:
    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    vector<RootSignatureSetElementContainer> _elems;
};

class GpuDescriptorHeapViews final : public DescriptorSet {
public:
    GpuDescriptorHeapViews(
        DeviceD3D12* device,
        GpuDescriptorHeapViewRAII resHeapView,
        GpuDescriptorHeapViewRAII samplerHeapView) noexcept;
    ~GpuDescriptorHeapViews() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void SetResource(uint32_t slot, uint32_t index, ResourceView* view) noexcept override;

public:
    DeviceD3D12* _device;
    GpuDescriptorHeapViewRAII _resHeapView;
    GpuDescriptorHeapViewRAII _samplerHeapView;
    vector<RootSignatureSetElementContainer> _elems;
    vector<uint32_t> _elemToHeapOffset;
};

class SamplerD3D12 final : public Sampler {
public:
    SamplerD3D12(
        DeviceD3D12* device,
        CpuDescriptorHeapViewRAII heapView) noexcept;
    ~SamplerD3D12() noexcept override = default;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    DeviceD3D12* _device;
    CpuDescriptorHeapViewRAII _samplerView;
};

Nullable<shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc);

}  // namespace radray::render::d3d12

#endif
