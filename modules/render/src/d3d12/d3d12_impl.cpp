#include "d3d12_impl.h"

#include <bit>
#include <cstring>

namespace radray::render::d3d12 {

static CmdQueueD3D12* CastD3D12Object(CommandQueue* v) noexcept { return static_cast<CmdQueueD3D12*>(v); }
static BufferD3D12* CastD3D12Object(Buffer* v) noexcept { return static_cast<BufferD3D12*>(v); }
static TextureD3D12* CastD3D12Object(Texture* v) noexcept { return static_cast<TextureD3D12*>(v); }

DescriptorHeap::DescriptorHeap(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept
    : _device(device) {
    if (FAILED(_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(_heap.GetAddressOf())))) {
        RADRAY_ABORT("d3d12 create DescriptorHeap failed");
    }
    _desc = _heap->GetDesc();
    _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    _gpuStart = IsShaderVisible() ? _heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{0};
    _incrementSize = _device->GetDescriptorHandleIncrementSize(_desc.Type);
    RADRAY_DEBUG_LOG(
        "D3D12 create DescriptorHeap. Type={}, IsShaderVisible={}, IncrementSize={}, Length={}, all={}(bytes)",
        _desc.Type,
        IsShaderVisible(),
        _incrementSize,
        _desc.NumDescriptors,
        UINT64(_desc.NumDescriptors) * _incrementSize);
}

ID3D12DescriptorHeap* DescriptorHeap::Get() const noexcept {
    return _heap.Get();
}

D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeap::GetHeapType() const noexcept {
    return _desc.Type;
}

UINT DescriptorHeap::GetLength() const noexcept {
    return _desc.NumDescriptors;
}

bool DescriptorHeap::IsShaderVisible() const noexcept {
    return (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleGpu(UINT index) const noexcept {
    return {_gpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleCpu(UINT index) const noexcept {
    return {_cpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateUnorderedAccessView(resource, nullptr, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateShaderResourceView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateConstantBufferView(&desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateRenderTargetView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateDepthStencilView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept {
    _device->CreateSampler(&desc, HandleCpu(index));
}

void DescriptorHeap::CopyTo(UINT start, UINT count, DescriptorHeap* dst, UINT dstStart) noexcept {
    _device->CopyDescriptorsSimple(
        count,
        dst->HandleCpu(dstStart),
        HandleCpu(start),
        _desc.Type);
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapView::HandleGpu() const noexcept {
    return Heap->HandleGpu(Start);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapView::HandleCpu() const noexcept {
    return Heap->HandleCpu(Start);
}

bool DescriptorHeapView::IsValid() const noexcept {
    return Heap != nullptr;
}

CpuDescriptorAllocatorImpl::CpuDescriptorAllocatorImpl(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT basicSize) noexcept
    : BlockAllocator(basicSize, 1),
      _device(device),
      _type(type) {}

unique_ptr<DescriptorHeap> CpuDescriptorAllocatorImpl::CreateHeap(size_t size) noexcept {
    return make_unique<DescriptorHeap>(
        _device,
        D3D12_DESCRIPTOR_HEAP_DESC{
            _type,
            static_cast<UINT>(size),
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
            0});
}

BuddyAllocator CpuDescriptorAllocatorImpl::CreateSubAllocator(size_t size) noexcept { return BuddyAllocator{size}; }

CpuDescriptorAllocator::CpuDescriptorAllocator(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT basicSize) noexcept
    : _impl(device, type, basicSize) {}

std::optional<DescriptorHeapView> CpuDescriptorAllocator::Allocate(UINT count) noexcept {
    auto allocation = _impl.Allocate(count);
    if (!allocation.has_value()) {
        return std::nullopt;
    }
    auto v = allocation.value();
    return std::make_optional(DescriptorHeapView{
        v.Heap,
        static_cast<UINT>(v.Start),
        static_cast<UINT>(v.Length)});
}

void CpuDescriptorAllocator::Destroy(DescriptorHeapView view) noexcept {
    _impl.Destroy({view.Heap, view.Start, view.Length});
}

GpuDescriptorAllocator::GpuDescriptorAllocator(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT size) noexcept
    : _device(device),
      _allocator(size) {
    _heap = make_unique<DescriptorHeap>(
        _device,
        D3D12_DESCRIPTOR_HEAP_DESC{
            type,
            static_cast<UINT>(size),
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            0});
}

std::optional<DescriptorHeapView> GpuDescriptorAllocator::Allocate(UINT count) noexcept {
    auto allocation = _allocator.Allocate(count);
    if (!allocation.has_value()) {
        return std::nullopt;
    }
    auto v = allocation.value();
    return std::make_optional(DescriptorHeapView{
        _heap.get(),
        static_cast<UINT>(v),
        count});
}

void GpuDescriptorAllocator::Destroy(DescriptorHeapView view) noexcept {
    _allocator.Destroy(view.Start);
}

DeviceD3D12::DeviceD3D12(
    ComPtr<ID3D12Device> device,
    ComPtr<IDXGIFactory4> dxgiFactory,
    ComPtr<IDXGIAdapter1> dxgiAdapter,
    ComPtr<D3D12MA::Allocator> mainAlloc) noexcept
    : _device(std::move(device)),
      _dxgiFactory(std::move(dxgiFactory)),
      _dxgiAdapter(std::move(dxgiAdapter)),
      _mainAlloc(std::move(mainAlloc)) {
    _cpuResAlloc = make_unique<CpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 512);
    _cpuRtvAlloc = make_unique<CpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 128);
    _cpuDsvAlloc = make_unique<CpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 128);
    _cpuSamplerAlloc = make_unique<CpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 64);
    _gpuResHeap = make_unique<GpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 16);
    _gpuSamplerHeap = make_unique<GpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1 << 8);
    _features.Init(_device.Get());
}

DeviceD3D12::~DeviceD3D12() noexcept {
    DestroyImpl();
}

bool DeviceD3D12::IsValid() const noexcept {
    return _device != nullptr && _dxgiAdapter != nullptr && _dxgiFactory != nullptr;
}

void DeviceD3D12::Destroy() noexcept {
    DestroyImpl();
}

void DeviceD3D12::DestroyImpl() noexcept {
    _cpuResAlloc = nullptr;
    _cpuRtvAlloc = nullptr;
    _cpuDsvAlloc = nullptr;
    _gpuResHeap = nullptr;
    _gpuSamplerHeap = nullptr;
    _mainAlloc = nullptr;
    _device = nullptr;
    _dxgiAdapter = nullptr;
    _dxgiFactory = nullptr;
}

Nullable<shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc) {
    uint32_t dxgiFactoryFlags = 0;
    if (desc.IsEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            if (desc.IsEnableGpuBasedValid) {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debugController.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(true);
                } else {
                    RADRAY_WARN_LOG("d3d12 get ID3D12Debug1 fail. cannot enable gpu based validation");
                }
            }
        } else {
            RADRAY_WARN_LOG("d3d12 find ID3D12Debug fail");
        }
    }
    ComPtr<IDXGIFactory4> dxgiFactory;
    if (HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 create IDXGIFactory4 fail, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (desc.AdapterIndex.has_value()) {
        uint32_t index = desc.AdapterIndex.value();
        if (HRESULT hr = dxgiFactory->EnumAdapters1(index, adapter.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("d3d12 get IDXGIAdapter1 at index {} fail, reason={} (code:{})", index, GetErrorName(hr), hr);
            return nullptr;
        }
    } else {
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(dxgiFactory->QueryInterface(IID_PPV_ARGS(factory6.GetAddressOf())))) {
            ComPtr<IDXGIAdapter1> temp;
            for (
                auto adapterIndex = 0u;
                factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.ReleaseAndGetAddressOf())) != DXGI_ERROR_NOT_FOUND;
                adapterIndex++) {
                DXGI_ADAPTER_DESC1 adapDesc;
                temp->GetDesc1(&adapDesc);
                if ((adapDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    wstring s{adapDesc.Description};
                    RADRAY_INFO_LOG("D3D12 find device: {}", ToMultiByte(s).value());
                }
            }
            for (
                auto adapterIndex = 0u;
                factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
                adapterIndex++) {
                DXGI_ADAPTER_DESC1 adapDesc;
                temp->GetDesc1(&adapDesc);
                if ((adapDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    if (SUCCEEDED(D3D12CreateDevice(temp.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                        break;
                    }
                }
                temp = nullptr;
            }
            adapter = temp;
        } else {
            if (dxgiFactory->EnumAdapters1(0, adapter.GetAddressOf())) {
                DXGI_ADAPTER_DESC1 adapDesc;
                adapter->GetDesc1(&adapDesc);
                if ((adapDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                    FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    adapter = nullptr;
                }
            }
        }
    }
    if (adapter == nullptr) {
        RADRAY_ERR_LOG("d3d12 get IDXGIAdapter1 fail");
        return nullptr;
    }
    ComPtr<ID3D12Device> device;
    if (HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 create ID3D12Device fail, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    {
        DXGI_ADAPTER_DESC1 adapDesc{};
        adapter->GetDesc1(&adapDesc);
        wstring s{adapDesc.Description};
        RADRAY_INFO_LOG("select device: {}", ToMultiByte(s).value());
    }
    ComPtr<D3D12MA::Allocator> alloc;
    {
        D3D12MA::ALLOCATOR_DESC allocDesc{};
        allocDesc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        allocDesc.pDevice = device.Get();
        allocDesc.pAdapter = adapter.Get();
#ifdef RADRAY_ENABLE_MIMALLOC
        D3D12MA::ALLOCATION_CALLBACKS allocationCallbacks{};
        allocationCallbacks.pAllocate = [](size_t Size, size_t Alignment, void* pPrivateData) {
            RADRAY_UNUSED(pPrivateData);
            return mi_malloc_aligned(Size, Alignment);
        };
        allocationCallbacks.pFree = [](void* pMemory, void* pPrivateData) {
            RADRAY_UNUSED(pPrivateData);
            mi_free(pMemory);
        };
        allocDesc.pAllocationCallbacks = &allocationCallbacks;
#endif
        allocDesc.Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED;
        if (HRESULT hr = D3D12MA::CreateAllocator(&allocDesc, alloc.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("d3d12 create D3D12MA::Allocator fail, reason={} (code:{})", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    auto result = make_shared<DeviceD3D12>(device, dxgiFactory, adapter, alloc);
    RADRAY_INFO_LOG("========== Feature ==========");
    {
        LARGE_INTEGER l;
        HRESULT hr = adapter->CheckInterfaceSupport(IID_IDXGIDevice, &l);
        if (SUCCEEDED(hr)) {
            const int64_t mask = 0xFFFF;
            auto quad = l.QuadPart;
            auto ver = radray::format(
                "{}.{}.{}.{}",
                quad >> 48,
                (quad >> 32) & mask,
                (quad >> 16) & mask,
                quad & mask);
            RADRAY_INFO_LOG("Driver Version: {}", ver);
        } else {
            RADRAY_WARN_LOG("get driver version failed");
        }
    }
    {
        BOOL allowTearing = FALSE;
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(dxgiFactory.As(&factory6))) {
            if (HRESULT hr = factory6->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &allowTearing,
                    sizeof(allowTearing));
                FAILED(hr)) {
                RADRAY_DEBUG_LOG("query IDXGIFactory6 feature DXGI_FEATURE_PRESENT_ALLOW_TEARING failed, reason={} (code={})", GetErrorName(hr), hr);
            }
        }
        RADRAY_INFO_LOG("Allow Tearing: {}", static_cast<bool>(allowTearing));
        result->_isAllowTearing = allowTearing;
    }
    const CD3DX12FeatureSupport& fs = result->_features;
    if (SUCCEEDED(fs.GetStatus())) {
        RADRAY_INFO_LOG("Feature Level: {}", fs.MaxSupportedFeatureLevel());
        RADRAY_INFO_LOG("Shader Model: {}", fs.HighestShaderModel());
        RADRAY_INFO_LOG("TBR: {}", static_cast<bool>(fs.TileBasedRenderer()));
        RADRAY_INFO_LOG("UMA: {}", static_cast<bool>(fs.UMA()));
    } else {
        RADRAY_WARN_LOG("check d3d12 feature failed");
    }
    RADRAY_INFO_LOG("=============================");
    return result;
}

Nullable<CommandQueue> DeviceD3D12::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    uint32_t index = static_cast<size_t>(type);
    RADRAY_ASSERT(index >= 0 && index < 3);
    auto& queues = _queues[index];
    if (queues.size() <= slot) {
        queues.reserve(slot + 1);
        for (size_t i = queues.size(); i <= slot; i++) {
            queues.emplace_back(unique_ptr<CmdQueueD3D12>{nullptr});
        }
    }
    unique_ptr<CmdQueueD3D12>& q = queues[slot];
    if (q == nullptr) {
        Nullable<shared_ptr<Fence>> fence = CreateFence(0);
        if (!fence) {
            return nullptr;
        }
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = MapType(type);
        if (HRESULT hr = _device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf()));
            SUCCEEDED(hr)) {
            shared_ptr<FenceD3D12> f = std::static_pointer_cast<FenceD3D12>(fence.Ptr);
            auto ins = make_unique<CmdQueueD3D12>(this, std::move(queue), desc.Type, std::move(f));
            string debugName = radray::format("Queue-{}-{}", type, slot);
            SetObjectName(debugName, ins->_queue.Get());
            q = std::move(ins);
        } else {
            RADRAY_ERR_LOG("d3d12 create ID3D12CommandQueue fail, reason={} (code:{})", GetErrorName(hr), hr);
        }
    }
    return q->IsValid() ? q.get() : nullptr;
}

Nullable<shared_ptr<CommandBuffer>> DeviceD3D12::CreateCommandBuffer(CommandQueue* queue_) noexcept {
    auto queue = CastD3D12Object(queue_);
    ComPtr<ID3D12CommandAllocator> alloc;
    if (HRESULT hr = _device->CreateCommandAllocator(queue->_type, IID_PPV_ARGS(alloc.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create ID3D12CommandAllocator, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr = _device->CreateCommandList(0, queue->_type, alloc.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        SUCCEEDED(hr)) {
        if (FAILED(list->Close())) {
            RADRAY_ERR_LOG("cannot close ID3D12GraphicsCommandList");
            return nullptr;
        }
        return make_shared<CmdListD3D12>(
            this,
            std::move(alloc),
            std::move(list),
            queue->_type);
    } else {
        RADRAY_ERR_LOG("cannot create ID3D12GraphicsCommandList, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
}

Nullable<shared_ptr<Fence>> DeviceD3D12::CreateFence(uint64_t initValue) noexcept {
    ComPtr<ID3D12Fence> fence;
    if (HRESULT hr = _device->CreateFence(initValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 create ID3D12Fence fail, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    std::optional<Win32Event> e = MakeWin32Event();
    if (!e.has_value()) {
        return nullptr;
    }
    auto result = make_shared<FenceD3D12>(std::move(fence), std::move(e.value()));
    result->_fenceValue = initValue;
    return result;
}

Nullable<shared_ptr<SwapChain>> DeviceD3D12::CreateSwapChain(const SwapChainDescriptor& desc) noexcept {
    // https://learn.microsoft.com/zh-cn/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = desc.Width;
    scDesc.Height = desc.Height;
    scDesc.Format = MapType(desc.Format);
    if (scDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
        scDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        scDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        scDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
        RADRAY_ERR_LOG("d3d12 IDXGISwapChain do not support format {}", desc.Format);
        return nullptr;
    }
    scDesc.Stereo = false;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = desc.BackBufferCount;
    if (scDesc.BufferCount < 2 || scDesc.BufferCount > 16) {
        RADRAY_ERR_LOG("d3d12 IDXGISwapChain BufferCount must >= 2 and <= 16, cannot be {}", desc.BackBufferCount);
        return nullptr;
    }
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = 0;
    scDesc.Flags |= _isAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    auto queue = CastD3D12Object(desc.PresentQueue);
    HWND hwnd = std::bit_cast<HWND>(desc.NativeHandler);
    ComPtr<IDXGISwapChain1> temp;
    if (HRESULT hr = _dxgiFactory->CreateSwapChainForHwnd(queue->_queue.Get(), hwnd, &scDesc, nullptr, nullptr, temp.GetAddressOf());
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 create IDXGISwapChain1 for HWND fail, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    if (HRESULT hr = _dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // 阻止 Alt + Enter 进全屏
        FAILED(hr)) {
        RADRAY_WARN_LOG("d3d12 make window association DXGI_MWA_NO_ALT_ENTER fail, reason={} (code:{})", GetErrorName(hr), hr);
    }
    ComPtr<IDXGISwapChain3> swapchain;
    if (HRESULT hr = temp->QueryInterface(IID_PPV_ARGS(swapchain.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 query IDXGISwapChain3 fail, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    auto result = make_shared<SwapChainD3D12>(this, swapchain, desc);
    result->_frames.reserve(scDesc.BufferCount);
    if (HRESULT hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(result->_fence.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 create ID3D12Fence fail, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    result->_fenceValue = 0;
    for (size_t i = 0; i < scDesc.BufferCount; i++) {
        auto& frame = result->_frames.emplace_back();
        ComPtr<ID3D12Resource> rt;
        if (HRESULT hr = swapchain->GetBuffer((UINT)i, IID_PPV_ARGS(rt.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("d3d12 get back buffer in IDXGISwapChain1 fail, reason={} (code:{})", GetErrorName(hr), hr);
            return nullptr;
        }
        frame.image = make_unique<TextureD3D12>(this, std::move(rt), ComPtr<D3D12MA::Allocation>{});
        frame.event = MakeWin32Event().value();
        SetEvent(frame.event.Get());
    }
    return result;
}

Nullable<shared_ptr<Buffer>> DeviceD3D12::CreateBuffer(const BufferDescriptor& desc_) noexcept {
    BufferDescriptor desc = desc_;
    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // Alignment must be 64KB (D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) or 0, which is effectively 64KB.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc
    resDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    // D3D12 要求 cbuffer 是 256 字节对齐
    // https://github.com/d3dcoder/d3d12book/blob/master/Common/d3dUtil.h#L99
    resDesc.Width = desc.Usage.HasFlag(BufferUse::CBuffer) ? Align(desc.Size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) : desc.Size;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.SampleDesc.Quality = 0;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (desc.Usage.HasFlag(BufferUse::UnorderedAccess)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (desc.Memory == MemoryType::ReadBack) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    UINT64 paddedSize;
    _device->GetCopyableFootprints(&resDesc, 0, 1, 0, nullptr, nullptr, nullptr, &paddedSize);
    if (paddedSize != UINT64_MAX) {
        desc.Size = paddedSize;
        resDesc.Width = paddedSize;
    }
    D3D12_RESOURCE_STATES rawInitState;
    switch (desc.Memory) {
        case MemoryType::Device: rawInitState = D3D12_RESOURCE_STATE_COMMON; break;
        case MemoryType::Upload: rawInitState = D3D12_RESOURCE_STATE_GENERIC_READ; break;
        case MemoryType::ReadBack: rawInitState = D3D12_RESOURCE_STATE_COPY_DEST; break;
    }
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = MapType(desc.Memory);
    allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        allocDesc.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocDesc.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }
    ComPtr<ID3D12Resource> buffer;
    ComPtr<D3D12MA::Allocation> allocRes;
    if (allocDesc.HeapType != D3D12_HEAP_TYPE_DEFAULT && (resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_CUSTOM;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        heapProps.VisibleNodeMask = 0;
        heapProps.CreationNodeMask = 0;
        if (rawInitState == D3D12_RESOURCE_STATE_GENERIC_READ) {
            rawInitState = D3D12_RESOURCE_STATE_COMMON;
        }
        if (HRESULT hr = _device->CreateCommittedResource(
                &heapProps,
                allocDesc.ExtraHeapFlags,
                &resDesc,
                rawInitState,
                nullptr,
                IID_PPV_ARGS(buffer.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("d3d12 cannot create buffer, reason={} (code:{})", GetErrorName(hr), hr);
            return nullptr;
        }
    } else {
        if (HRESULT hr = _mainAlloc->CreateResource(
                &allocDesc,
                &resDesc,
                rawInitState,
                nullptr,
                allocRes.GetAddressOf(),
                IID_PPV_ARGS(buffer.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("d3d12 cannot create buffer, reason={} (code:{})", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    SetObjectName(desc.Name, buffer.Get(), allocRes.Get());
    auto result = make_shared<BufferD3D12>(this, std::move(buffer), std::move(allocRes));
    result->_desc = desc;
    return result;
}

Nullable<shared_ptr<BufferView>> DeviceD3D12::CreateBufferView(const BufferViewDescriptor& desc) noexcept {
    auto buf = CastD3D12Object(desc.Target);
    CpuDescriptorAllocator* heap = _cpuResAlloc.get();
    DescriptorHeapView heapView;
    {
        auto heapViewOpt = heap->Allocate(1);
        if (!heapViewOpt.has_value()) {
            RADRAY_ERR_LOG("d3d12 allocate buffer view fail");
            return nullptr;
        }
        heapView = heapViewOpt.value();
    }
    auto guard = radray::MakeScopeGuard([=]() noexcept { heap->Destroy(heapView); });
    DXGI_FORMAT dxgiFormat;
    if (desc.Usage == BufferUse::CBuffer) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = buf->_gpuAddr + desc.Range.Offset;
        cbvDesc.SizeInBytes = desc.Range.Size;
        heapView.Heap->Create(cbvDesc, heapView.Start);
        dxgiFormat = DXGI_FORMAT_UNKNOWN;
    } else if (desc.Usage == BufferUse::Resource) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = MapType(desc.Format);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = desc.Range.Offset;
        srvDesc.Buffer.NumElements = static_cast<UINT>(desc.Range.Size / desc.Stride);
        srvDesc.Buffer.StructureByteStride = desc.Stride;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        heapView.Heap->Create(buf->_buf.Get(), srvDesc, heapView.Start);
        dxgiFormat = srvDesc.Format;
    } else if (desc.Usage == BufferUse::UnorderedAccess) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = MapType(desc.Format);
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = desc.Range.Offset;
        uavDesc.Buffer.NumElements = static_cast<UINT>(desc.Range.Size / desc.Stride);
        uavDesc.Buffer.StructureByteStride = desc.Stride;
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        heapView.Heap->Create(buf->_buf.Get(), uavDesc, heapView.Start);
        dxgiFormat = uavDesc.Format;
    } else {
        RADRAY_ERR_LOG("d3d12 cannot create buffer view");
        return nullptr;
    }
    auto result = make_shared<BufferViewD3D12>(this, buf, heapView, heap);
    guard.Dismiss();
    result->_desc = desc;
    result->_rawFormat = dxgiFormat;
    return result;
}

Nullable<shared_ptr<Texture>> DeviceD3D12::CreateTexture(const TextureDescriptor& desc_) noexcept {
    TextureDescriptor desc = desc_;
    DXGI_FORMAT rawFormat = MapType(desc.Format);
    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = MapType(desc.Dim);
    resDesc.Alignment = desc.SampleCount > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT : 0;
    resDesc.Width = desc.Width;
    resDesc.Height = desc.Height;
    resDesc.DepthOrArraySize = static_cast<UINT16>(desc.DepthOrArraySize);
    resDesc.MipLevels = static_cast<UINT16>(desc.MipLevels);
    resDesc.Format = FormatToTypeless(rawFormat);
    resDesc.SampleDesc.Count = desc.SampleCount ? desc.SampleCount : 1;
    resDesc.SampleDesc.Quality = 0;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (desc.Usage.HasFlag(TextureUse::UnorderedAccess)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (desc.Usage.HasFlag(TextureUse::RenderTarget)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (desc.Usage.HasFlag(TextureUse::DepthStencilRead) || desc.Usage.HasFlag(TextureUse::DepthStencilWrite)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    D3D12_RESOURCE_STATES startState = D3D12_RESOURCE_STATE_COMMON;
    // D3D12_CLEAR_VALUE clear{};
    // clear.Format = rawFormat;
    // if (auto ccv = std::get_if<ColorClearValue>(&clearValue)) {
    //     clear.Color[0] = ccv->R;
    //     clear.Color[1] = ccv->G;
    //     clear.Color[2] = ccv->B;
    //     clear.Color[3] = ccv->A;
    // } else if (auto dcv = std::get_if<DepthStencilClearValue>(&clearValue)) {
    //     clear.DepthStencil.Depth = dcv->Depth;
    //     clear.DepthStencil.Stencil = (UINT8)dcv->Stencil;
    // }
    const D3D12_CLEAR_VALUE* clearPtr = nullptr;
    // if ((resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || (resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
    //     clearPtr = &clear;
    // }
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        allocDesc.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocDesc.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }
    ComPtr<ID3D12Resource> texture;
    ComPtr<D3D12MA::Allocation> allocRes;
    if (HRESULT hr = _mainAlloc->CreateResource(
            &allocDesc,
            &resDesc,
            startState,
            clearPtr,
            allocRes.GetAddressOf(),
            IID_PPV_ARGS(texture.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 cannot create texture, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    SetObjectName(desc.Name, texture.Get(), allocRes.Get());
    auto result = make_shared<TextureD3D12>(this, std::move(texture), std::move(allocRes));
    result->_desc = desc;
    return result;
}

Nullable<shared_ptr<TextureView>> DeviceD3D12::CreateTextureView(const TextureViewDescriptor& desc) noexcept {
    // https://learn.microsoft.com/zh-cn/windows/win32/direct3d12/subresources
    // 三种 slice: mip 横向, array 纵向, plane 看起来更像是通道
    auto tex = CastD3D12Object(desc.Target);
    CpuDescriptorAllocator* heap = nullptr;
    DescriptorHeapView heapView = DescriptorHeapView::Invalid();
    auto guard = radray::MakeScopeGuard([&]() noexcept {
        if (heap != nullptr && heapView.IsValid()) {
            heap->Destroy(heapView);
        }
    });
    DXGI_FORMAT dxgiFormat;
    if (desc.Usage == TextureUse::Resource) {
        heap = _cpuResAlloc.get();
        {
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("d3d12 cannot allocate texture view, unknown error");
                return nullptr;
            }
            heapView = heapViewOpt.value();
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = MapShaderResourceType(desc.Format);
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (desc.Dim) {
            case TextureViewDimension::Dim1D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvDesc.Texture1D.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.Texture1D.MipLevels = desc.MipLevelCount.value_or(static_cast<UINT>(-1));
                break;
            case TextureViewDimension::Dim1DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvDesc.Texture1DArray.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.Texture1DArray.MipLevels = desc.MipLevelCount.value_or(static_cast<UINT>(-1));
                srvDesc.Texture1DArray.FirstArraySlice = desc.BaseArrayLayer;
                srvDesc.Texture1DArray.ArraySize = desc.ArrayLayerCount.value_or(1);
                break;
            case TextureViewDimension::Dim2D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.Texture2D.MipLevels = desc.MipLevelCount.value_or(static_cast<UINT>(-1));
                srvDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim2DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.Texture2DArray.MipLevels = desc.MipLevelCount.value_or(static_cast<UINT>(-1));
                srvDesc.Texture2DArray.FirstArraySlice = desc.BaseArrayLayer;
                srvDesc.Texture2DArray.ArraySize = desc.ArrayLayerCount.value_or(1);
                srvDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim3D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.Texture3D.MipLevels = desc.MipLevelCount.value_or(static_cast<UINT>(-1));
                break;
            case TextureViewDimension::Cube:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.TextureCube.MipLevels = desc.MipLevelCount.value_or(static_cast<UINT>(-1));
                break;
            case TextureViewDimension::CubeArray:
                // https://learn.microsoft.com/zh-cn/windows/win32/api/d3d12/ns-d3d12-d3d12_texcube_array_srv
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvDesc.TextureCubeArray.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.TextureCubeArray.MipLevels = desc.MipLevelCount.value_or(static_cast<UINT>(-1));
                srvDesc.TextureCubeArray.First2DArrayFace = desc.BaseArrayLayer;
                srvDesc.TextureCubeArray.NumCubes = desc.ArrayLayerCount.value_or(1);
                break;
            default:
                RADRAY_ERR_LOG("d3d12 cannot create texture view");
                return nullptr;
        }
        heapView.Heap->Create(tex->_tex.Get(), srvDesc, heapView.Start);
        dxgiFormat = srvDesc.Format;
    } else if (desc.Usage == TextureUse::RenderTarget) {
        heap = _cpuRtvAlloc.get();
        {
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("d3d12 cannot allocate texture view");
                return nullptr;
            }
            heapView = heapViewOpt.value();
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = MapType(desc.Format);
        switch (desc.Dim) {
            case TextureViewDimension::Dim1D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                rtvDesc.Texture1D.MipSlice = desc.BaseMipLevel;
                break;
            case TextureViewDimension::Dim1DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                rtvDesc.Texture1DArray.MipSlice = desc.BaseMipLevel;
                rtvDesc.Texture1DArray.FirstArraySlice = desc.BaseArrayLayer;
                rtvDesc.Texture1DArray.ArraySize = desc.ArrayLayerCount.value_or(1);
                break;
            case TextureViewDimension::Dim2D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = desc.BaseMipLevel;
                rtvDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim2DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = desc.BaseMipLevel;
                rtvDesc.Texture2DArray.FirstArraySlice = desc.BaseArrayLayer;
                rtvDesc.Texture2DArray.ArraySize = desc.ArrayLayerCount.value_or(1);
                rtvDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim3D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtvDesc.Texture3D.MipSlice = desc.BaseMipLevel;
                rtvDesc.Texture3D.FirstWSlice = desc.BaseArrayLayer;
                rtvDesc.Texture3D.WSize = desc.ArrayLayerCount.value_or(1);
            default:
                RADRAY_ERR_LOG("d3d12 cannot create texture view");
                return nullptr;
        }
        heapView.Heap->Create(tex->_tex.Get(), rtvDesc, heapView.Start);
        dxgiFormat = rtvDesc.Format;
    } else if (desc.Usage == TextureUse::DepthStencilRead || desc.Usage == TextureUse::DepthStencilWrite) {
        heap = _cpuDsvAlloc.get();
        {
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("d3d12 cannot allocate texture view");
                return nullptr;
            }
            heapView = heapViewOpt.value();
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = MapType(desc.Format);
        switch (desc.Dim) {
            case TextureViewDimension::Dim1D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                dsvDesc.Texture1D.MipSlice = desc.BaseMipLevel;
                break;
            case TextureViewDimension::Dim1DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                dsvDesc.Texture1DArray.MipSlice = desc.BaseMipLevel;
                dsvDesc.Texture1DArray.FirstArraySlice = desc.BaseArrayLayer;
                dsvDesc.Texture1DArray.ArraySize = desc.ArrayLayerCount.value_or(1);
                break;
            case TextureViewDimension::Dim2D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsvDesc.Texture2D.MipSlice = desc.BaseMipLevel;
                break;
            case TextureViewDimension::Dim2DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.MipSlice = desc.BaseMipLevel;
                dsvDesc.Texture2DArray.FirstArraySlice = desc.BaseArrayLayer;
                dsvDesc.Texture2DArray.ArraySize = desc.ArrayLayerCount.value_or(1);
                break;
            default:
                RADRAY_ERR_LOG("d3d12 cannot create texture view");
                return nullptr;
        }
        heapView.Heap->Create(tex->_tex.Get(), dsvDesc, heapView.Start);
        dxgiFormat = dsvDesc.Format;
    } else if (desc.Usage == TextureUse::UnorderedAccess) {
        heap = _cpuResAlloc.get();
        {
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("d3d12 cannot allocate texture view, unknown error");
                return nullptr;
            }
            heapView = heapViewOpt.value();
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = MapShaderResourceType(desc.Format);
        switch (desc.Dim) {
            case TextureViewDimension::Dim1D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uavDesc.Texture1D.MipSlice = desc.BaseMipLevel;
                break;
            case TextureViewDimension::Dim1DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                uavDesc.Texture1DArray.MipSlice = desc.BaseMipLevel;
                uavDesc.Texture1DArray.FirstArraySlice = desc.BaseArrayLayer;
                uavDesc.Texture1DArray.ArraySize = desc.ArrayLayerCount.value_or(1);
                break;
            case TextureViewDimension::Dim2D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = desc.BaseMipLevel;
                uavDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim2DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = desc.BaseMipLevel;
                uavDesc.Texture2DArray.FirstArraySlice = desc.BaseArrayLayer;
                uavDesc.Texture2DArray.ArraySize = desc.ArrayLayerCount.value_or(1);
                uavDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim3D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavDesc.Texture3D.MipSlice = desc.BaseMipLevel;
                uavDesc.Texture3D.FirstWSlice = desc.BaseArrayLayer;
                uavDesc.Texture3D.WSize = desc.ArrayLayerCount.value_or(1);
                break;
            default:
                RADRAY_ERR_LOG("d3d12 cannot create texture view");
                return nullptr;
        }
        heapView.Heap->Create(tex->_tex.Get(), uavDesc, heapView.Start);
        dxgiFormat = uavDesc.Format;
    } else {
        RADRAY_ERR_LOG("d3d12 cannot create texture view");
        return nullptr;
    }
    auto result = make_shared<TextureViewD3D12>(this, tex, heapView, heap);
    guard.Dismiss();
    result->_desc = desc;
    result->_rawFormat = dxgiFormat;
    return result;
}

Nullable<shared_ptr<Shader>> DeviceD3D12::CreateShader(const ShaderDescriptor& desc) noexcept {
    if (desc.Category != ShaderBlobCategory::DXIL) {
        RADRAY_ERR_LOG("d3d12 only support DXIL");
        return nullptr;
    }
    return make_shared<Dxil>(desc.Source.begin(), desc.Source.end());
}

Nullable<shared_ptr<RootSignature>> DeviceD3D12::CreateRootSignature(const RootSignatureDescriptor& desc) noexcept {
    return nullptr;
}

Nullable<shared_ptr<GraphicsPipelineState>> DeviceD3D12::CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept {
    return nullptr;
}

CmdQueueD3D12::CmdQueueD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12CommandQueue> queue,
    D3D12_COMMAND_LIST_TYPE type,
    shared_ptr<FenceD3D12> fence) noexcept
    : _device(device),
      _queue(std::move(queue)),
      _fence(std::move(fence)),
      _type(type) {}

bool CmdQueueD3D12::IsValid() const noexcept {
    return _queue != nullptr;
}

void CmdQueueD3D12::Destroy() noexcept {
    _fence = nullptr;
    _queue = nullptr;
}

void CmdQueueD3D12::Submit(const CommandQueueSubmitDescriptor& desc) noexcept {
}

void CmdQueueD3D12::Wait() noexcept {
}

FenceD3D12::FenceD3D12(
    ComPtr<ID3D12Fence> fence,
    Win32Event event) noexcept
    : _fence(std::move(fence)),
      _event(std::move(event)) {}

bool FenceD3D12::IsValid() const noexcept {
    return _fence != nullptr;
}

void FenceD3D12::Destroy() noexcept {
    _fence = nullptr;
    _event.Destroy();
}

uint64_t FenceD3D12::GetCompletedValue() const noexcept {
    return _fence->GetCompletedValue();
}

CmdListD3D12::CmdListD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12CommandAllocator> cmdAlloc,
    ComPtr<ID3D12GraphicsCommandList> cmdList,
    D3D12_COMMAND_LIST_TYPE type) noexcept
    : _device(device),
      _cmdAlloc(std::move(cmdAlloc)),
      _cmdList(std::move(cmdList)),
      _type(type) {}

bool CmdListD3D12::IsValid() const noexcept {
    return _cmdAlloc != nullptr && _cmdList != nullptr;
}

void CmdListD3D12::Destroy() noexcept {
    _cmdAlloc = nullptr;
    _cmdList = nullptr;
}

void CmdListD3D12::Begin() noexcept {
}

void CmdListD3D12::End() noexcept {
}

void CmdListD3D12::ResourceBarrier(std::span<BarrierBufferDescriptor> buffers, std::span<BarrierTextureDescriptor> textures) noexcept {
}

Nullable<unique_ptr<CommandEncoder>> CmdListD3D12::BeginRenderPass(const RenderPassDescriptor& desc) noexcept {
    return nullptr;
}

void CmdListD3D12::EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept {
}

void CmdListD3D12::CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept {
}

bool CmdRenderPassD3D12::IsValid() const noexcept {
    return false;
}

void CmdRenderPassD3D12::Destroy() noexcept {
}

void CmdRenderPassD3D12::SetViewport(Viewport vp) noexcept {
}

void CmdRenderPassD3D12::SetScissor(Rect rect) noexcept {
}

void CmdRenderPassD3D12::BindVertexBuffer(std::span<VertexBufferView> vbv) noexcept {
}

void CmdRenderPassD3D12::BindIndexBuffer(IndexBufferView ibv) noexcept {
}

void CmdRenderPassD3D12::BindRootSignature(RootSignature* rootSig) noexcept {
}

void CmdRenderPassD3D12::BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept {
}

void CmdRenderPassD3D12::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
}

void CmdRenderPassD3D12::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
}

SwapChainD3D12::SwapChainD3D12(
    DeviceD3D12* device,
    ComPtr<IDXGISwapChain3> swapchain,
    const SwapChainDescriptor& desc) noexcept
    : _device(device),
      _swapchain(std::move(swapchain)),
      _desc(desc) {}

SwapChainD3D12::~SwapChainD3D12() noexcept {
    _frames.clear();
    _swapchain = nullptr;
}

bool SwapChainD3D12::IsValid() const noexcept {
    return _swapchain != nullptr;
}

void SwapChainD3D12::Destroy() noexcept {
    _frames.clear();
    _swapchain = nullptr;
}

Nullable<Texture> SwapChainD3D12::AcquireNext() noexcept {
    auto curr = _swapchain->GetCurrentBackBufferIndex();
    auto& frame = _frames[curr];
    WaitForSingleObject(frame.event.Get(), INFINITE);
    ResetEvent(frame.event.Get());
    return frame.image.get();
}

void SwapChainD3D12::Present() noexcept {
    UINT curr = _swapchain->GetCurrentBackBufferIndex();
    auto& frame = _frames[curr];
    _fence->SetEventOnCompletion(_fenceValue, frame.event.Get());
    UINT syncInterval = _desc.EnableSync ? 1 : 0;
    UINT presentFlags = (!_desc.EnableSync && _device->_isAllowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    if (HRESULT hr = _swapchain->Present(syncInterval, presentFlags);
        FAILED(hr)) {
        RADRAY_ABORT("d3d12 IDXGISwapChain3 present fail, reason={} (code:{})", GetErrorName(hr), hr);
    }
    _fenceValue++;
    auto queue = CastD3D12Object(_desc.PresentQueue);
    queue->_queue->Signal(_fence.Get(), _fenceValue);
}

Nullable<Texture> SwapChainD3D12::GetCurrentBackBuffer() const noexcept {
    UINT curr = _swapchain->GetCurrentBackBufferIndex();
    return _frames[curr].image.get();
}

uint32_t SwapChainD3D12::GetCurrentBackBufferIndex() const noexcept {
    return _swapchain->GetCurrentBackBufferIndex();
}

uint32_t SwapChainD3D12::GetBackBufferCount() const noexcept {
    return static_cast<uint32_t>(_frames.size());
}

BufferD3D12::BufferD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Resource> buf,
    ComPtr<D3D12MA::Allocation> alloc) noexcept
    : _device(device),
      _buf(std::move(buf)),
      _alloc(std::move(alloc)) {
    _rawDesc = _buf->GetDesc();
    _gpuAddr = _buf->GetGPUVirtualAddress();
}

bool BufferD3D12::IsValid() const noexcept {
    return _buf != nullptr;
}

void BufferD3D12::Destroy() noexcept {
    _buf = nullptr;
    _alloc = nullptr;
}

void BufferD3D12::CopyFromHost(std::span<byte> data, uint64_t offset) noexcept {
    D3D12_RANGE range{offset, offset + data.size()};
    void* ptr = nullptr;
    if (HRESULT hr = _buf->Map(0, &range, &ptr);
        FAILED(hr)) {
        RADRAY_ABORT("cannot map buffer, reason={} (code:{})", GetErrorName(hr), hr);
    }
    std::memcpy(ptr, data.data(), data.size());
    _buf->Unmap(0, &range);
}

BufferViewD3D12::BufferViewD3D12(
    DeviceD3D12* device,
    BufferD3D12* buffer,
    DescriptorHeapView heapView,
    CpuDescriptorAllocator* heapAlloc) noexcept
    : _device(device),
      _buffer(buffer),
      _heapView(heapView),
      _heapAlloc(heapAlloc) {}

BufferViewD3D12::~BufferViewD3D12() noexcept {
    DestroyImpl();
}

bool BufferViewD3D12::IsValid() const noexcept {
    return _heapView.IsValid() && _heapAlloc != nullptr;
}

void BufferViewD3D12::Destroy() noexcept {
    DestroyImpl();
}

void BufferViewD3D12::DestroyImpl() noexcept {
    if (_heapView.IsValid() && _heapAlloc != nullptr) {
        _heapAlloc->Destroy(_heapView);
        _heapAlloc = nullptr;
        _heapView = DescriptorHeapView::Invalid();
    }
}

TextureD3D12::TextureD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Resource> tex,
    ComPtr<D3D12MA::Allocation> alloc) noexcept
    : _device(device),
      _tex(std::move(tex)),
      _alloc(std::move(alloc)) {
    _rawDesc = _tex->GetDesc();
    _gpuAddr = _tex->GetGPUVirtualAddress();
}

bool TextureD3D12::IsValid() const noexcept {
    return _tex != nullptr;
}

void TextureD3D12::Destroy() noexcept {
    _tex = nullptr;
    _alloc = nullptr;
}

TextureViewD3D12::TextureViewD3D12(
    DeviceD3D12* device,
    TextureD3D12* texture,
    DescriptorHeapView heapView,
    CpuDescriptorAllocator* heapAlloc) noexcept
    : _device(device),
      _texture(texture),
      _heapView(heapView),
      _heapAlloc(heapAlloc) {}

TextureViewD3D12::~TextureViewD3D12() noexcept {
    DestroyImpl();
}

bool TextureViewD3D12::IsValid() const noexcept {
    return _heapView.IsValid() && _heapAlloc != nullptr;
}

void TextureViewD3D12::Destroy() noexcept {
    DestroyImpl();
}

void TextureViewD3D12::DestroyImpl() noexcept {
    if (_heapView.IsValid() && _heapAlloc != nullptr) {
        _heapAlloc->Destroy(_heapView);
        _heapAlloc = nullptr;
        _heapView = DescriptorHeapView::Invalid();
    }
}

bool Dxil::IsValid() const noexcept {
    return !_dxil.empty();
}

void Dxil::Destroy() noexcept {
    _dxil.clear();
    _dxil.shrink_to_fit();
}

bool RootSigD3D12::IsValid() const noexcept {
    return false;
}

void RootSigD3D12::Destroy() noexcept {
}

bool GraphicsPsoD3D12::IsValid() const noexcept {
    return false;
}

void GraphicsPsoD3D12::Destroy() noexcept {
}

}  // namespace radray::render::d3d12
