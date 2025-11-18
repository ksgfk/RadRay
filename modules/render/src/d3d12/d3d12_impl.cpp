#include <radray/render/backend/d3d12_impl.h>

#include <bit>
#include <cstring>
#include <algorithm>
#include <ranges>

#include <radray/errors.h>

namespace radray::render::d3d12 {

DescriptorHeap::DescriptorHeap(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept
    : _device(device) {
    if (HRESULT hr = _device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(_heap.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ABORT("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateDescriptorHeap", GetErrorName(hr), hr);
    }
    _desc = _heap->GetDesc();
    _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    _gpuStart = IsShaderVisible() ? _heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{0};
    _incrementSize = _device->GetDescriptorHandleIncrementSize(_desc.Type);
    RADRAY_DEBUG_LOG(
        "{} create DescriptorHeap. Type={}, IsShaderVisible={}, IncrementSize={}, Length={}, all={}(bytes)", Errors::D3D12,
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
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            if (desc.IsEnableGpuBasedValid) {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debugController.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(true);
                } else {
                    RADRAY_WARN_LOG("{} {}::{} {}", Errors::D3D12, "ID3D12Debug", "As<ID3D12Debug1>", "cannot enable gpu based validation");
                }
            }
        } else {
            RADRAY_WARN_LOG("{} {} {}", Errors::D3D12, "D3D12GetDebugInterface", "cannot enable gpu based validation");
        }
    }
    ComPtr<IDXGIFactory4> dxgiFactory;
    if (HRESULT hr = ::CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, "CreateDXGIFactory2", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (desc.AdapterIndex.has_value()) {
        uint32_t index = desc.AdapterIndex.value();
        if (HRESULT hr = dxgiFactory->EnumAdapters1(index, adapter.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("{} {}({}={}) {} {}", Errors::D3D12, "EnumAdapters1", "index", index, GetErrorName(hr), hr);
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
                    RADRAY_INFO_LOG("{} find device: {}", Errors::D3D12, ToMultiByte(s).value());
                }
            }
            for (
                auto adapterIndex = 0u;
                factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
                adapterIndex++) {
                DXGI_ADAPTER_DESC1 adapDesc;
                temp->GetDesc1(&adapDesc);
                if ((adapDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    if (SUCCEEDED(::D3D12CreateDevice(temp.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
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
                    FAILED(::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    adapter = nullptr;
                }
            }
        }
    }
    if (adapter == nullptr) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, "IDXGIAdapter1", "cannot get adapter");
        return nullptr;
    }
    ComPtr<ID3D12Device> device;
    if (HRESULT hr = ::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, "D3D12CreateDevice", GetErrorName(hr), hr);
        return nullptr;
    }
    {
        DXGI_ADAPTER_DESC1 adapDesc{};
        adapter->GetDesc1(&adapDesc);
        wstring s{adapDesc.Description};
        RADRAY_INFO_LOG("{} select device: {}", Errors::D3D12, ToMultiByte(s).value());
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
            RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, "D3D12MA::CreateAllocator", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    auto result = make_shared<DeviceD3D12>(device, dxgiFactory, adapter, alloc);
    {
        DeviceDetail& detail = result->_detail;
        detail.CBufferAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        detail.UploadTextureAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        detail.UploadTextureRowAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        detail.MapAlignment = 1;
    }
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
            RADRAY_WARN_LOG("{} {}::{} {} {}", Errors::D3D12, "IDXGIAdapter", "CheckInterfaceSupport", GetErrorName(hr), hr);
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
                RADRAY_WARN_LOG("{} {}::{} {} {}", Errors::D3D12, "IDXGIFactory6", "CheckFeatureSupport", GetErrorName(hr), hr);
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
        RADRAY_WARN_LOG("{} {}::{}", Errors::D3D12, "CD3DX12FeatureSupport", "GetStatus");
    }
    RADRAY_INFO_LOG("=============================");
    return result;
}

DeviceDetail DeviceD3D12::GetDetail() const noexcept {
    return _detail;
}

Nullable<CommandQueue*> DeviceD3D12::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
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
        if (fence == nullptr) {
            return nullptr;
        }
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = MapType(type);
        if (HRESULT hr = _device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf()));
            SUCCEEDED(hr)) {
            shared_ptr<FenceD3D12> f = std::static_pointer_cast<FenceD3D12>(fence.Release());
            auto ins = make_unique<CmdQueueD3D12>(this, std::move(queue), desc.Type, std::move(f));
            string debugName = radray::format("Queue-{}-{}", type, slot);
            SetObjectName(debugName, ins->_queue.Get());
            q = std::move(ins);
        } else {
            RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateCommandQueue", GetErrorName(hr), hr);
        }
    }
    return q->IsValid() ? q.get() : nullptr;
}

Nullable<shared_ptr<CommandBuffer>> DeviceD3D12::CreateCommandBuffer(CommandQueue* queue_) noexcept {
    auto queue = CastD3D12Object(queue_);
    ComPtr<ID3D12CommandAllocator> alloc;
    if (HRESULT hr = _device->CreateCommandAllocator(queue->_type, IID_PPV_ARGS(alloc.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateCommandAllocator", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr = _device->CreateCommandList(0, queue->_type, alloc.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        SUCCEEDED(hr)) {
        if (FAILED(list->Close())) {
            RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12GraphicsCommandList", "Close", GetErrorName(hr), hr);
            return nullptr;
        }
        return make_shared<CmdListD3D12>(
            this,
            std::move(alloc),
            std::move(list),
            queue->_type);
    } else {
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateCommandList", GetErrorName(hr), hr);
        return nullptr;
    }
}

Nullable<shared_ptr<Fence>> DeviceD3D12::CreateFence(uint64_t initValue) noexcept {
    ComPtr<ID3D12Fence> fence;
    if (HRESULT hr = _device->CreateFence(initValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateFence", GetErrorName(hr), hr);
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
        RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, "IDXGISwapChain", "format not supported", desc.Format);
        return nullptr;
    }
    scDesc.Stereo = false;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = desc.BackBufferCount;
    if (scDesc.BufferCount < 2 || scDesc.BufferCount > 16) {
        RADRAY_ERR_LOG("{} IDXGISwapChain BufferCount must >= 2 and <= 16 {}", Errors::D3D12, desc.BackBufferCount);
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
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "IDXGIFactory", "CreateSwapChainForHwnd", GetErrorName(hr), hr);
        return nullptr;
    }
    if (HRESULT hr = _dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // 阻止 Alt + Enter 进全屏
        FAILED(hr)) {
        RADRAY_WARN_LOG("{} {}::{} {} {}", Errors::D3D12, "IDXGIFactory", "MakeWindowAssociation", GetErrorName(hr), hr);
    }
    ComPtr<IDXGISwapChain3> swapchain;
    if (HRESULT hr = temp->QueryInterface(IID_PPV_ARGS(swapchain.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "IDXGISwapChain1", "QueryInterface", GetErrorName(hr), hr);
        return nullptr;
    }
    auto result = make_shared<SwapChainD3D12>(this, swapchain, desc);
    result->_frames.reserve(scDesc.BufferCount);
    if (HRESULT hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(result->_fence.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateFence", GetErrorName(hr), hr);
        return nullptr;
    }
    result->_fenceValue = 0;
    for (size_t i = 0; i < scDesc.BufferCount; i++) {
        auto& frame = result->_frames.emplace_back();
        ComPtr<ID3D12Resource> rt;
        if (HRESULT hr = swapchain->GetBuffer((UINT)i, IID_PPV_ARGS(rt.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "IDXGISwapChain1", "GetBuffer", GetErrorName(hr), hr);
            return nullptr;
        }
        frame.image = make_unique<TextureD3D12>(this, std::move(rt), ComPtr<D3D12MA::Allocation>{});
        frame.event = MakeWin32Event().value();
        ::SetEvent(frame.event.Get());
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
            RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateCommittedResource", GetErrorName(hr), hr);
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
            RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "D3D12MA::Allocator", "CreateResource", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    SetObjectName(desc.Name, buffer.Get(), allocRes.Get());
    auto result = make_shared<BufferD3D12>(this, std::move(buffer), std::move(allocRes));
    result->_desc = desc;
    result->_name = desc.Name;
    result->_desc.Name = result->_name;
    return result;
}

Nullable<shared_ptr<BufferView>> DeviceD3D12::CreateBufferView(const BufferViewDescriptor& desc) noexcept {
    auto buf = CastD3D12Object(desc.Target);
    CpuDescriptorAllocator* heap = _cpuResAlloc.get();
    CpuDescriptorHeapViewRAII heapView{};
    {
        auto heapViewOpt = heap->Allocate(1);
        if (!heapViewOpt.has_value()) {
            RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "CpuDescriptorAllocator", "Allocate", Errors::OutOfMemory);
            return nullptr;
        }
        heapView = {heapViewOpt.value(), heap};
    }
    DXGI_FORMAT dxgiFormat;
    if (desc.Usage == BufferUse::CBuffer) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = buf->_gpuAddr + desc.Range.Offset;
        cbvDesc.SizeInBytes = desc.Range.Size;
        heapView.GetHeap()->Create(cbvDesc, heapView.GetStart());
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
        heapView.GetHeap()->Create(buf->_buf.Get(), srvDesc, heapView.GetStart());
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
        heapView.GetHeap()->Create(buf->_buf.Get(), uavDesc, heapView.GetStart());
        dxgiFormat = uavDesc.Format;
    } else {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid buffer view usage");
        return nullptr;
    }
    auto result = make_shared<BufferViewD3D12>(this, buf, std::move(heapView));
    result->_desc = desc;
    result->_rawFormat = dxgiFormat;
    return result;
}

Nullable<shared_ptr<Texture>> DeviceD3D12::CreateTexture(const TextureDescriptor& desc_) noexcept {
    TextureDescriptor desc = desc_;
    D3D12_RESOURCE_DESC resDesc = MapTextureDesc(desc);
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
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "D3D12MA::Allocator", "CreateResource", GetErrorName(hr), hr);
        return nullptr;
    }
    SetObjectName(desc.Name, texture.Get(), allocRes.Get());
    auto result = make_shared<TextureD3D12>(this, std::move(texture), std::move(allocRes));
    result->_desc = desc;
    result->_name = desc.Name;
    result->_desc.Name = result->_name;
    return result;
}

Nullable<shared_ptr<TextureView>> DeviceD3D12::CreateTextureView(const TextureViewDescriptor& desc) noexcept {
    // https://learn.microsoft.com/zh-cn/windows/win32/direct3d12/subresources
    // 三种 slice: mip 横向, array 纵向, plane 看起来更像是通道
    auto tex = CastD3D12Object(desc.Target);
    CpuDescriptorHeapViewRAII heapView{};
    DXGI_FORMAT dxgiFormat;
    if (desc.Usage == TextureUse::Resource) {
        {
            auto heap = _cpuResAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "CpuDescriptorAllocator", "Allocate", Errors::OutOfMemory);
                return nullptr;
            }
            heapView = {heapViewOpt.value(), heap};
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = MapShaderResourceType(desc.Format);
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (desc.Dim) {
            case TextureViewDimension::Dim1D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvDesc.Texture1D.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture1D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureViewDimension::Dim1DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvDesc.Texture1DArray.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture1DArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.Texture1DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                srvDesc.Texture1DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            case TextureViewDimension::Dim2D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture2D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim2DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture2DArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.Texture2DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                srvDesc.Texture2DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                srvDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim3D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture3D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureViewDimension::Cube:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.TextureCube.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureViewDimension::CubeArray:
                // https://learn.microsoft.com/zh-cn/windows/win32/api/d3d12/ns-d3d12-d3d12_texcube_array_srv
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvDesc.TextureCubeArray.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.TextureCubeArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.TextureCubeArray.First2DArrayFace = desc.Range.BaseArrayLayer;
                srvDesc.TextureCubeArray.NumCubes = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid texture view dimension");
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), srvDesc, heapView.GetStart());
        dxgiFormat = srvDesc.Format;
    } else if (desc.Usage == TextureUse::RenderTarget) {
        {
            auto heap = _cpuRtvAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "CpuDescriptorAllocator", "Allocate", Errors::OutOfMemory);
                return nullptr;
            }
            heapView = {heapViewOpt.value(), heap};
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = MapType(desc.Format);
        switch (desc.Dim) {
            case TextureViewDimension::Dim1D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                rtvDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureViewDimension::Dim1DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                rtvDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture1DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                rtvDesc.Texture1DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            case TextureViewDimension::Dim2D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim2DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture2DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                rtvDesc.Texture2DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                rtvDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim3D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtvDesc.Texture3D.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture3D.FirstWSlice = desc.Range.BaseArrayLayer;
                rtvDesc.Texture3D.WSize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
            default:
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid texture view dimension");
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), rtvDesc, heapView.GetStart());
        dxgiFormat = rtvDesc.Format;
    } else if (desc.Usage.HasFlag(TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite)) {
        {
            auto heap = _cpuDsvAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "CpuDescriptorAllocator", "Allocate", Errors::OutOfMemory);
                return nullptr;
            }
            heapView = {heapViewOpt.value(), heap};
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = MapType(desc.Format);
        switch (desc.Dim) {
            case TextureViewDimension::Dim1D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                dsvDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureViewDimension::Dim1DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                dsvDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                dsvDesc.Texture1DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                dsvDesc.Texture1DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            case TextureViewDimension::Dim2D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsvDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureViewDimension::Dim2DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                dsvDesc.Texture2DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                dsvDesc.Texture2DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid texture view dimension");
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), dsvDesc, heapView.GetStart());
        dxgiFormat = dsvDesc.Format;
    } else if (desc.Usage == TextureUse::UnorderedAccess) {
        {
            auto heap = _cpuResAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "CpuDescriptorAllocator", "Allocate", Errors::OutOfMemory);
                return nullptr;
            }
            heapView = {heapViewOpt.value(), heap};
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = MapShaderResourceType(desc.Format);
        switch (desc.Dim) {
            case TextureViewDimension::Dim1D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uavDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureViewDimension::Dim1DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                uavDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture1DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                uavDesc.Texture1DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            case TextureViewDimension::Dim2D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim2DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture2DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                uavDesc.Texture2DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                uavDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureViewDimension::Dim3D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavDesc.Texture3D.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture3D.FirstWSlice = desc.Range.BaseArrayLayer;
                uavDesc.Texture3D.WSize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid texture view dimension");
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), uavDesc, heapView.GetStart());
        dxgiFormat = uavDesc.Format;
    } else {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid texture view usage");
        return nullptr;
    }
    auto result = make_shared<TextureViewD3D12>(this, tex, std::move(heapView));
    result->_desc = desc;
    result->_rawFormat = dxgiFormat;
    return result;
}

Nullable<shared_ptr<Shader>> DeviceD3D12::CreateShader(const ShaderDescriptor& desc) noexcept {
    if (desc.Category != ShaderBlobCategory::DXIL) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "only support DXIL shader blobs");
        return nullptr;
    }
    return make_shared<Dxil>(desc.Source.begin(), desc.Source.end());
}

Nullable<shared_ptr<RootSignature>> DeviceD3D12::CreateRootSignature(const RootSignatureDescriptor& desc) noexcept {
    vector<D3D12_ROOT_PARAMETER1> rootParmas{};
    vector<D3D12_DESCRIPTOR_RANGE1> descRanges{};
    ShaderStages allStages = ShaderStage::UNKNOWN;
    // size_t rootConstStart = rootParmas.size();
    if (desc.Constant.has_value()) {
        const RootSignatureConstant& rootConst = desc.Constant.value();
        D3D12_ROOT_PARAMETER1& rp = rootParmas.emplace_back();
        CD3DX12_ROOT_PARAMETER1::InitAsConstants(
            rp,
            rootConst.Size / 4,
            rootConst.Slot,
            rootConst.Space,
            MapShaderStages(rootConst.Stages));
        allStages |= rootConst.Stages;
    }
    // size_t rootDescStart = rootParmas.size();
    for (const RootSignatureBinding& rootDesc : desc.RootBindings) {
        D3D12_ROOT_PARAMETER1& rp = rootParmas.emplace_back();
        switch (rootDesc.Type) {
            case ResourceBindType::CBuffer: {
                CD3DX12_ROOT_PARAMETER1::InitAsConstantBufferView(
                    rp,
                    rootDesc.Slot,
                    rootDesc.Space,
                    D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
                    MapShaderStages(rootDesc.Stages));
                break;
            }
            case ResourceBindType::Buffer:
            case ResourceBindType::Texture: {
                CD3DX12_ROOT_PARAMETER1::InitAsShaderResourceView(
                    rp,
                    rootDesc.Slot,
                    0,
                    D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
                    MapShaderStages(rootDesc.Stages));
                break;
            }
            case ResourceBindType::RWBuffer:
            case ResourceBindType::RWTexture: {
                CD3DX12_ROOT_PARAMETER1::InitAsUnorderedAccessView(
                    rp,
                    rootDesc.Slot,
                    0,
                    D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
                    MapShaderStages(rootDesc.Stages));
                break;
            }
            default: {
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid root binding type");
                return nullptr;
            }
        }
        allStages |= rootDesc.Stages;
    }
    // 我们约定, hlsl 中定义的单个资源独占一个 range
    // 资源数组占一个 range
    for (auto i : desc.BindingSets) {
        auto descSet = CastD3D12Object(i);
        for (const RootSignatureSetElementContainer& c : descSet->_elems) {
            const RootSignatureSetElement& e = c._elem;
            switch (e.Type) {
                case ResourceBindType::CBuffer:
                case ResourceBindType::Buffer:
                case ResourceBindType::RWBuffer:
                case ResourceBindType::Texture:
                case ResourceBindType::RWTexture:
                    descRanges.emplace_back();
                    allStages |= e.Stages;
                    break;
                case ResourceBindType::Sampler: {
                    if (e.StaticSamplers.empty()) {
                        descRanges.emplace_back();
                        allStages |= e.Stages;
                    } else {
                        if (e.StaticSamplers.size() != e.Count) {
                            RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid static sampler count");
                            return nullptr;
                        }
                    }
                    break;
                }
                default: {
                    RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid root binding type");
                    return nullptr;
                }
            }
        }
    }
    // size_t bindStart = rootParmas.size();
    size_t offset = 0;
    vector<RootSignatureSetElement> bindDescs{};
    vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers{};
    for (auto i : desc.BindingSets) {
        auto descSet = CastD3D12Object(i);
        for (const RootSignatureSetElementContainer& c : descSet->_elems) {
            const RootSignatureSetElement& e = c._elem;
            switch (e.Type) {
                case ResourceBindType::Sampler: {
                    if (e.StaticSamplers.empty()) {
                        D3D12_DESCRIPTOR_RANGE1& range = descRanges[offset];
                        offset++;
                        CD3DX12_DESCRIPTOR_RANGE1::Init(
                            range,
                            D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
                            e.Count,
                            e.Slot,
                            e.Space);
                        D3D12_ROOT_PARAMETER1& rp = rootParmas.emplace_back();
                        CD3DX12_ROOT_PARAMETER1::InitAsDescriptorTable(
                            rp,
                            1,
                            &range,
                            MapShaderStages(e.Stages));
                        bindDescs.emplace_back(e);
                    } else {
                        if (e.StaticSamplers.size() != e.Count) {
                            RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid static sampler count");
                            return nullptr;
                        }
                        for (size_t t = 0; t < e.StaticSamplers.size(); t++) {
                            const SamplerDescriptor& ss = e.StaticSamplers[t];
                            D3D12_STATIC_SAMPLER_DESC& ssDesc = staticSamplers.emplace_back();
                            ssDesc.Filter = MapType(ss.MigFilter, ss.MagFilter, ss.MipmapFilter, ss.Compare.has_value(), ss.AnisotropyClamp);
                            ssDesc.AddressU = MapType(ss.AddressS);
                            ssDesc.AddressV = MapType(ss.AddressT);
                            ssDesc.AddressW = MapType(ss.AddressR);
                            ssDesc.MipLODBias = 0;
                            ssDesc.MaxAnisotropy = ss.AnisotropyClamp;
                            ssDesc.ComparisonFunc = ss.Compare.has_value() ? MapType(ss.Compare.value()) : D3D12_COMPARISON_FUNC_NEVER;
                            ssDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
                            ssDesc.MinLOD = ss.LodMin;
                            ssDesc.MaxLOD = ss.LodMax;
                            ssDesc.ShaderRegister = e.Slot + t;
                            ssDesc.RegisterSpace = e.Space;
                            ssDesc.ShaderVisibility = MapShaderStages(e.Stages);
                        }
                    }
                    break;
                }
                case ResourceBindType::Texture:
                case ResourceBindType::Buffer: {
                    D3D12_DESCRIPTOR_RANGE1& range = descRanges[offset];
                    offset++;
                    CD3DX12_DESCRIPTOR_RANGE1::Init(
                        range,
                        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                        e.Count,
                        e.Slot,
                        e.Space);
                    D3D12_ROOT_PARAMETER1& rp = rootParmas.emplace_back();
                    CD3DX12_ROOT_PARAMETER1::InitAsDescriptorTable(
                        rp,
                        1,
                        &range,
                        MapShaderStages(e.Stages));
                    bindDescs.emplace_back(e);
                    break;
                }
                case ResourceBindType::CBuffer: {
                    D3D12_DESCRIPTOR_RANGE1& range = descRanges[offset];
                    offset++;
                    CD3DX12_DESCRIPTOR_RANGE1::Init(
                        range,
                        D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                        e.Count,
                        e.Slot,
                        e.Space);
                    D3D12_ROOT_PARAMETER1& rp = rootParmas.emplace_back();
                    CD3DX12_ROOT_PARAMETER1::InitAsDescriptorTable(
                        rp,
                        1,
                        &range,
                        MapShaderStages(e.Stages));
                    bindDescs.emplace_back(e);
                    break;
                }
                case ResourceBindType::RWTexture:
                case ResourceBindType::RWBuffer: {
                    D3D12_DESCRIPTOR_RANGE1& range = descRanges[offset];
                    offset++;
                    CD3DX12_DESCRIPTOR_RANGE1::Init(
                        range,
                        D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                        e.Count,
                        e.Slot,
                        e.Space);
                    D3D12_ROOT_PARAMETER1& rp = rootParmas.emplace_back();
                    CD3DX12_ROOT_PARAMETER1::InitAsDescriptorTable(
                        rp,
                        1,
                        &range,
                        MapShaderStages(e.Stages));
                    bindDescs.emplace_back(e);
                    break;
                }
                default:
                    break;
            }
        }
    }
    bindDescs.shrink_to_fit();
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionDesc{};
    versionDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionDesc.Desc_1_1 = D3D12_ROOT_SIGNATURE_DESC1{};
    D3D12_ROOT_SIGNATURE_DESC1& rsDesc = versionDesc.Desc_1_1;
    rsDesc.NumParameters = static_cast<UINT>(rootParmas.size());
    rsDesc.pParameters = rootParmas.size() > 0 ? rootParmas.data() : nullptr;
    rsDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rsDesc.pStaticSamplers = staticSamplers.size() > 0 ? staticSamplers.data() : nullptr;
    D3D12_ROOT_SIGNATURE_FLAGS flag =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS;
    if (!allStages.HasFlag(ShaderStage::Vertex)) {
        flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;
    }
    if (!allStages.HasFlag(ShaderStage::Pixel)) {
        flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
    }
    rsDesc.Flags = flag;
    ComPtr<ID3DBlob> rootSigBlob, errorBlob;
    if (HRESULT hr = ::D3DX12SerializeVersionedRootSignature(
            &versionDesc,
            D3D_ROOT_SIGNATURE_VERSION_1_1,
            rootSigBlob.GetAddressOf(),
            errorBlob.GetAddressOf());
        FAILED(hr)) {
        std::string_view reason;
        if (errorBlob) {
            const char* const errInfoBegin = std::bit_cast<const char*>(errorBlob->GetBufferPointer());
            auto errInfoSize = static_cast<size_t>(errorBlob->GetBufferSize());
            reason = std::string_view{errInfoBegin, errInfoSize};
        } else {
            reason = GetErrorName(hr);
        }
        RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, "D3DX12SerializeVersionedRootSignature", reason, hr);
        return nullptr;
    }
    ComPtr<ID3D12RootSignature> rootSig;
    if (HRESULT hr = _device->CreateRootSignature(
            0,
            rootSigBlob->GetBufferPointer(),
            rootSigBlob->GetBufferSize(),
            IID_PPV_ARGS(rootSig.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateRootSignature", GetErrorName(hr), hr);
        return nullptr;
    }
    auto result = make_shared<RootSigD3D12>(this, std::move(rootSig));
    result->_desc = VersionedRootSignatureDescContainer{versionDesc};
    return result;
}

Nullable<shared_ptr<GraphicsPipelineState>> DeviceD3D12::CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept {
    auto [topoClass, topo] = MapType(desc.Primitive.Topology);
    vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    vector<uint64_t> arrayStrides(desc.VertexLayouts.size(), 0);
    for (size_t index = 0; index < desc.VertexLayouts.size(); index++) {
        const VertexBufferLayout& i = desc.VertexLayouts[index];
        arrayStrides[index] = i.ArrayStride;
        D3D12_INPUT_CLASSIFICATION inputClass = MapType(i.StepMode);
        for (const VertexElement& j : i.Elements) {
            auto& ied = inputElements.emplace_back(D3D12_INPUT_ELEMENT_DESC{});
            ied.SemanticName = j.Semantic.data();
            ied.SemanticIndex = j.SemanticIndex;
            ied.Format = MapType(j.Format);
            ied.InputSlot = (UINT)index;
            ied.AlignedByteOffset = (UINT)j.Offset;
            ied.InputSlotClass = inputClass;
            ied.InstanceDataStepRate = inputClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? 1 : 0;
        }
    }
    DepthBiasState depBias;
    if (desc.DepthStencil.has_value()) {
        depBias = desc.DepthStencil.value().DepthBias;
    } else {
        depBias = DepthBiasState{0, 0, 0};
    }
    D3D12_RASTERIZER_DESC rawRaster{};
    if (auto fillMode = MapType(desc.Primitive.Poly);
        fillMode.has_value()) {
        rawRaster.FillMode = fillMode.value();
    } else {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid primitive polygon mode");
        return nullptr;
    }
    rawRaster.CullMode = MapType(desc.Primitive.Cull);
    rawRaster.FrontCounterClockwise = desc.Primitive.FaceClockwise == FrontFace::CCW;
    rawRaster.DepthBias = depBias.Constant;
    rawRaster.DepthBiasClamp = depBias.Clamp;
    rawRaster.SlopeScaledDepthBias = depBias.SlopScale;
    rawRaster.DepthClipEnable = !desc.Primitive.UnclippedDepth;
    rawRaster.MultisampleEnable = desc.MultiSample.Count > 1;
    rawRaster.AntialiasedLineEnable = false;
    rawRaster.ForcedSampleCount = 0;
    rawRaster.ConservativeRaster = desc.Primitive.Conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    D3D12_BLEND_DESC rawBlend{};
    rawBlend.AlphaToCoverageEnable = desc.MultiSample.AlphaToCoverageEnable;
    rawBlend.IndependentBlendEnable = false;
    for (size_t i = 0; i < ArrayLength(rawBlend.RenderTarget); i++) {
        D3D12_RENDER_TARGET_BLEND_DESC& rtb = rawBlend.RenderTarget[i];
        if (i < desc.ColorTargets.size()) {
            const ColorTargetState& ct = desc.ColorTargets[i];
            rtb.BlendEnable = ct.Blend.has_value();
            if (ct.Blend.has_value()) {
                const auto& ctBlend = ct.Blend.value();
                rtb.SrcBlend = MapBlendColor(ctBlend.Color.Src);
                rtb.DestBlend = MapBlendColor(ctBlend.Color.Dst);
                rtb.BlendOp = MapType(ctBlend.Color.Op);
                rtb.SrcBlendAlpha = MapBlendAlpha(ctBlend.Alpha.Src);
                rtb.DestBlendAlpha = MapBlendAlpha(ctBlend.Alpha.Dst);
                rtb.BlendOpAlpha = MapType(ctBlend.Alpha.Op);
            }
            if (auto writeMask = MapColorWrites(ct.WriteMask);
                writeMask.has_value()) {
                rtb.RenderTargetWriteMask = (UINT8)writeMask.value();
            } else {
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid color target write mask");
                return nullptr;
            }
        } else {
            rtb.BlendEnable = false;
            rtb.LogicOpEnable = false;
            rtb.LogicOp = D3D12_LOGIC_OP_CLEAR;
            rtb.RenderTargetWriteMask = 0;
        }
    }
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    if (desc.DepthStencil.has_value()) {
        const auto& ds = desc.DepthStencil.value();
        dsDesc.DepthEnable = true;
        dsDesc.DepthWriteMask = ds.DepthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc = MapType(ds.DepthCompare);
        dsDesc.StencilEnable = ds.Stencil.has_value();
        if (ds.Stencil.has_value()) {
            const auto& s = ds.Stencil.value();
            auto ToDsd = [](StencilFaceState v) noexcept {
                D3D12_DEPTH_STENCILOP_DESC result{};
                result.StencilFailOp = MapType(v.FailOp);
                result.StencilDepthFailOp = MapType(v.DepthFailOp);
                result.StencilPassOp = MapType(v.PassOp);
                result.StencilFunc = MapType(v.Compare);
                return result;
            };
            dsDesc.StencilReadMask = (UINT8)s.ReadMask;
            dsDesc.StencilWriteMask = (UINT8)s.WriteMask;
            dsDesc.FrontFace = ToDsd(s.Front);
            dsDesc.BackFace = ToDsd(s.Back);
        }
    } else {
        dsDesc.DepthEnable = false;
        dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        dsDesc.StencilEnable = false;
        dsDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        dsDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        dsDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        dsDesc.BackFace = dsDesc.FrontFace;
    }
    DXGI_SAMPLE_DESC sampleDesc{desc.MultiSample.Count, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC rawPsoDesc{};
    rawPsoDesc.pRootSignature = CastD3D12Object(desc.RootSig)->_rootSig.Get();
    rawPsoDesc.VS = desc.VS ? CastD3D12Object(desc.VS->Target)->ToByteCode() : D3D12_SHADER_BYTECODE{};
    rawPsoDesc.PS = desc.PS ? CastD3D12Object(desc.PS->Target)->ToByteCode() : D3D12_SHADER_BYTECODE{};
    rawPsoDesc.DS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.HS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.GS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.StreamOutput = D3D12_STREAM_OUTPUT_DESC{};
    rawPsoDesc.BlendState = rawBlend;
    rawPsoDesc.SampleMask = (UINT)desc.MultiSample.Mask;
    rawPsoDesc.RasterizerState = rawRaster;
    rawPsoDesc.DepthStencilState = dsDesc;
    rawPsoDesc.InputLayout = {inputElements.data(), static_cast<uint32_t>(inputElements.size())};
    rawPsoDesc.IBStripCutValue = desc.Primitive.StripIndexFormat.has_value() ? MapType(desc.Primitive.StripIndexFormat.value()) : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    rawPsoDesc.PrimitiveTopologyType = topoClass;
    rawPsoDesc.NumRenderTargets = std::min(static_cast<uint32_t>(desc.ColorTargets.size()), (uint32_t)D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
    for (size_t i = 0; i < rawPsoDesc.NumRenderTargets; i++) {
        rawPsoDesc.RTVFormats[i] = i < desc.ColorTargets.size() ? MapType(desc.ColorTargets[i].Format) : DXGI_FORMAT_UNKNOWN;
    }
    rawPsoDesc.DSVFormat = desc.DepthStencil.has_value() ? MapType(desc.DepthStencil.value().Format) : DXGI_FORMAT_UNKNOWN;
    rawPsoDesc.SampleDesc = sampleDesc;
    rawPsoDesc.NodeMask = 0;
    rawPsoDesc.CachedPSO = D3D12_CACHED_PIPELINE_STATE{};
    rawPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ComPtr<ID3D12PipelineState> pso;
    if (HRESULT hr = _device->CreateGraphicsPipelineState(&rawPsoDesc, IID_PPV_ARGS(pso.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12Device", "CreateGraphicsPipelineState", GetErrorName(hr), hr);
        return nullptr;
    }
    return make_shared<GraphicsPsoD3D12>(this, std::move(pso), std::move(arrayStrides), topo);
}

Nullable<shared_ptr<DescriptorSetLayout>> DeviceD3D12::CreateDescriptorSetLayout(const RootSignatureBindingSet& desc) noexcept {
    auto result = make_shared<SimulateDescriptorSetLayoutD3D12>();
    result->_elems = RootSignatureSetElementContainer::FromView(desc.Elements);
    return result;
}

Nullable<shared_ptr<DescriptorSet>> DeviceD3D12::CreateDescriptorSet(DescriptorSetLayout* layout_) noexcept {
    auto layout = CastD3D12Object(layout_);
    UINT resCount = 0, samplerCount = 0;
    vector<uint32_t> offset;
    offset.reserve(layout->_elems.size());
    for (const RootSignatureSetElementContainer& c : layout->_elems) {
        const RootSignatureSetElement& e = c._elem;
        switch (e.Type) {
            case ResourceBindType::CBuffer:
            case ResourceBindType::Buffer:
            case ResourceBindType::Texture:
            case ResourceBindType::RWBuffer:
            case ResourceBindType::RWTexture:
                offset.push_back(resCount);
                resCount += e.Count;
                break;
            case ResourceBindType::Sampler:
                if (e.StaticSamplers.empty()) {
                    offset.push_back(samplerCount);
                    samplerCount += e.Count;
                }
                break;
            default:
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "invalid root binding type");
                return nullptr;
        }
    }
    GpuDescriptorHeapViewRAII resHeapView{};
    if (resCount > 0) {
        auto gpuResHeapAllocationOpt = _gpuResHeap->Allocate(resCount);
        if (!gpuResHeapAllocationOpt.has_value()) {
            RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "GpuDescriptorAllocator", "Allocate", Errors::OutOfMemory);
            return nullptr;
        }
        resHeapView = {gpuResHeapAllocationOpt.value(), _gpuResHeap.get()};
    }
    GpuDescriptorHeapViewRAII samplerHeapView{};
    if (samplerCount > 0) {
        auto gpuSamplerHeapAllocationOpt = _gpuSamplerHeap->Allocate(samplerCount);
        if (!gpuSamplerHeapAllocationOpt.has_value()) {
            RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "GpuDescriptorAllocator", "Allocate", Errors::OutOfMemory);
            return nullptr;
        }
        samplerHeapView = {gpuSamplerHeapAllocationOpt.value(), _gpuSamplerHeap.get()};
    }
    auto result = make_shared<GpuDescriptorHeapViews>(this, std::move(resHeapView), std::move(samplerHeapView));
    result->_elems = layout->_elems;
    result->_elemToHeapOffset = std::move(offset);
    return result;
}

Nullable<shared_ptr<Sampler>> DeviceD3D12::CreateSampler(const SamplerDescriptor& desc) noexcept {
    D3D12_SAMPLER_DESC rawDesc{};
    rawDesc.Filter = MapType(desc.MigFilter, desc.MagFilter, desc.MipmapFilter, desc.Compare.has_value(), desc.AnisotropyClamp);
    rawDesc.AddressU = MapType(desc.AddressS);
    rawDesc.AddressV = MapType(desc.AddressT);
    rawDesc.AddressW = MapType(desc.AddressR);
    rawDesc.MipLODBias = 0;
    rawDesc.MaxAnisotropy = desc.AnisotropyClamp;
    rawDesc.ComparisonFunc = desc.Compare.has_value() ? MapType(desc.Compare.value()) : D3D12_COMPARISON_FUNC_NEVER;
    rawDesc.BorderColor[0] = 0;
    rawDesc.BorderColor[1] = 0;
    rawDesc.BorderColor[2] = 0;
    rawDesc.BorderColor[3] = 0;
    rawDesc.MinLOD = desc.LodMin;
    rawDesc.MaxLOD = desc.LodMax;
    auto alloc = this->_cpuSamplerAlloc.get();
    CpuDescriptorHeapViewRAII heapView{};
    {
        auto opt = alloc->Allocate(1);
        if (!opt.has_value()) {
            RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "CpuDescriptorAllocator", "Allocate", Errors::OutOfMemory);
            return nullptr;
        }
        heapView = {opt.value(), alloc};
    }
    heapView.GetHeap()->Create(rawDesc, heapView.GetStart());
    return make_shared<SamplerD3D12>(this, std::move(heapView));
}

D3D12_RESOURCE_DESC DeviceD3D12::MapTextureDesc(const TextureDescriptor& desc) noexcept {
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
    return resDesc;
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
    for (size_t i = 0; i < desc.WaitFences.size(); i++) {
        auto f = CastD3D12Object(desc.WaitFences[i]);
        _queue->Wait(f->_fence.Get(), desc.WaitFenceValues[i]);
    }
    vector<ID3D12CommandList*> submits;
    submits.reserve(desc.CmdBuffers.size());
    for (auto& i : desc.CmdBuffers) {
        auto cmdList = CastD3D12Object(i);
        submits.push_back(cmdList->_cmdList.Get());
    }
    if (!submits.empty()) {
        _queue->ExecuteCommandLists(static_cast<UINT>(submits.size()), submits.data());
    }
    for (size_t i = 0; i < desc.SignalFences.size(); i++) {
        auto f = CastD3D12Object(desc.SignalFences[i]);
        _queue->Signal(f->_fence.Get(), desc.SignalFenceValues[i]);
    }
    _fence->_fenceValue++;
    _queue->Signal(_fence->_fence.Get(), _fence->_fenceValue);
}

void CmdQueueD3D12::Wait() noexcept {
    _fence->_fenceValue++;
    _queue->Signal(_fence->_fence.Get(), _fence->_fenceValue);
    _fence->_fence->SetEventOnCompletion(_fence->_fenceValue, _fence->_event.Get());
    ::WaitForSingleObject(_fence->_event.Get(), INFINITE);
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
    if (HRESULT hr = _cmdAlloc->Reset();
        FAILED(hr)) {
        RADRAY_ABORT("{} {}::{} {} {}", Errors::D3D12, "ID3D12CommandAllocator", "Reset", GetErrorName(hr), hr);
    }
    if (HRESULT hr = _cmdList->Reset(_cmdAlloc.Get(), nullptr);
        FAILED(hr)) {
        RADRAY_ABORT("{} {}::{} {} {}", Errors::D3D12, "ID3D12GraphicsCommandList", "Reset", GetErrorName(hr), hr);
    }
    ID3D12DescriptorHeap* heaps[] = {_device->_gpuResHeap->GetNative(), _device->_gpuSamplerHeap->GetNative()};
    if (_type != D3D12_COMMAND_LIST_TYPE_COPY) {
        _cmdList->SetDescriptorHeaps((UINT)radray::ArrayLength(heaps), heaps);
    }
}

void CmdListD3D12::End() noexcept {
    _cmdList->Close();
}

void CmdListD3D12::ResourceBarrier(std::span<BarrierBufferDescriptor> buffers, std::span<BarrierTextureDescriptor> textures) noexcept {
    vector<D3D12_RESOURCE_BARRIER> rawBarriers;
    rawBarriers.reserve(buffers.size() + textures.size());
    for (const BarrierBufferDescriptor& bb : buffers) {
        auto buf = CastD3D12Object(bb.Target);
        D3D12_RESOURCE_BARRIER raw{};
        if (bb.Before == BufferUse::UnorderedAccess && bb.After == BufferUse::UnorderedAccess) {
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.UAV.pResource = buf->_buf.Get();
        } else {
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.Transition.pResource = buf->_buf.Get();
            raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            raw.Transition.StateBefore = MapType(bb.Before);
            raw.Transition.StateAfter = MapType(bb.After);
            if (raw.Transition.StateBefore == raw.Transition.StateAfter) {
                continue;
            }
        }
        rawBarriers.push_back(raw);
    }
    for (const BarrierTextureDescriptor& tb : textures) {
        auto tex = CastD3D12Object(tb.Target);
        D3D12_RESOURCE_BARRIER raw{};
        if (tb.Before == TextureUse::UnorderedAccess && tb.After == TextureUse::UnorderedAccess) {
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.UAV.pResource = tex->_tex.Get();
        } else {
            if (tb.Before == tb.After) {
                continue;
            }
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.Transition.pResource = tex->_tex.Get();
            if (tb.IsSubresourceBarrier) {
                raw.Transition.Subresource = D3D12CalcSubresource(
                    tb.Range.BaseMipLevel,
                    tb.Range.BaseArrayLayer,
                    0,
                    tex->_desc.MipLevels,
                    tex->_desc.DepthOrArraySize);
            } else {
                raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            raw.Transition.StateBefore = MapType(tb.Before);
            raw.Transition.StateAfter = MapType(tb.After);
            // D3D12 COMMON 和 PRESENT flag 完全一致
            if (raw.Transition.StateBefore == D3D12_RESOURCE_STATE_COMMON && raw.Transition.StateAfter == D3D12_RESOURCE_STATE_COMMON) {
                if (tb.Before == TextureUse::Present || tb.After == TextureUse::Present) {
                    continue;
                }
            }
            if (raw.Transition.StateBefore == raw.Transition.StateAfter) {
                continue;
            }
        }
        rawBarriers.push_back(raw);
    }
    if (!rawBarriers.empty()) {
        _cmdList->ResourceBarrier(static_cast<UINT>(rawBarriers.size()), rawBarriers.data());
    }
}

Nullable<unique_ptr<CommandEncoder>> CmdListD3D12::BeginRenderPass(const RenderPassDescriptor& desc) noexcept {
    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    if (HRESULT hr = _cmdList->QueryInterface(IID_PPV_ARGS(cmdList4.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {} {}", Errors::D3D12, "ID3D12GraphicsCommandList", "QueryInterface", GetErrorName(hr), hr);
        return nullptr;
    }
    vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> rtDescs;
    rtDescs.reserve(desc.ColorAttachments.size());
    for (const ColorAttachment& color : desc.ColorAttachments) {
        auto v = CastD3D12Object(color.Target);
        D3D12_CLEAR_VALUE clearColor{};
        clearColor.Format = v->_rawFormat;
        clearColor.Color[0] = color.ClearValue.R;
        clearColor.Color[1] = color.ClearValue.G;
        clearColor.Color[2] = color.ClearValue.B;
        clearColor.Color[3] = color.ClearValue.A;
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE beginningAccess = MapType(color.Load);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE endingAccess = MapType(color.Store);
        auto& rtDesc = rtDescs.emplace_back(D3D12_RENDER_PASS_RENDER_TARGET_DESC{});
        rtDesc.cpuDescriptor = v->_heapView.HandleCpu();
        rtDesc.BeginningAccess.Type = beginningAccess;
        rtDesc.BeginningAccess.Clear.ClearValue = clearColor;
        rtDesc.EndingAccess.Type = endingAccess;
    }
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDsDesc = nullptr;
    if (desc.DepthStencilAttachment.has_value()) {
        const DepthStencilAttachment& depthStencil = desc.DepthStencilAttachment.value();
        auto v = CastD3D12Object(depthStencil.Target);
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE depthBeginningAccess = MapType(depthStencil.DepthLoad);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE depthEndingAccess = MapType(depthStencil.DepthStore);
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE stencilBeginningAccess = MapType(depthStencil.StencilLoad);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE stencilEndingAccess = MapType(depthStencil.StencilStore);
        if (!IsStencilFormatDXGI(v->_rawFormat)) {
            stencilBeginningAccess = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
            stencilEndingAccess = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
        }
        D3D12_CLEAR_VALUE clear{};
        clear.Format = v->_rawFormat;
        clear.DepthStencil.Depth = depthStencil.ClearValue.Depth;
        clear.DepthStencil.Stencil = depthStencil.ClearValue.Stencil;
        dsDesc.cpuDescriptor = v->_heapView.HandleCpu();
        dsDesc.DepthBeginningAccess.Type = depthBeginningAccess;
        dsDesc.DepthBeginningAccess.Clear.ClearValue = clear;
        dsDesc.DepthEndingAccess.Type = depthEndingAccess;
        dsDesc.StencilBeginningAccess.Type = stencilBeginningAccess;
        dsDesc.StencilBeginningAccess.Clear.ClearValue = clear;
        dsDesc.StencilEndingAccess.Type = stencilEndingAccess;
        pDsDesc = &dsDesc;
    }
    cmdList4->BeginRenderPass((UINT32)rtDescs.size(), rtDescs.data(), pDsDesc, D3D12_RENDER_PASS_FLAG_NONE);
    return make_unique<CmdRenderPassD3D12>(this);
}

void CmdListD3D12::EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept {
    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    if (HRESULT hr = _cmdList->QueryInterface(IID_PPV_ARGS(cmdList4.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ABORT("{} {}::{} {} {}", Errors::D3D12, "ID3D12GraphicsCommandList", "QueryInterface", GetErrorName(hr), hr);
        return;
    }
    cmdList4->EndRenderPass();
    encoder->Destroy();
}

void CmdListD3D12::CopyBufferToBuffer(Buffer* dst_, uint64_t dstOffset, Buffer* src_, uint64_t srcOffset, uint64_t size) noexcept {
    auto src = CastD3D12Object(src_);
    auto dst = CastD3D12Object(dst_);
    _cmdList->CopyBufferRegion(dst->_buf.Get(), dstOffset, src->_buf.Get(), srcOffset, size);
}

void CmdListD3D12::CopyBufferToTexture(Texture* dst_, SubresourceRange dstRange, Buffer* src_, uint64_t srcOffset) noexcept {
    auto src = CastD3D12Object(src_);
    auto dst = CastD3D12Object(dst_);

    UINT subres = D3D12CalcSubresource(dstRange.BaseMipLevel, dstRange.BaseArrayLayer, 0, 1, dst->_desc.DepthOrArraySize);
    const D3D12_RESOURCE_DESC& dstDesc = dst->_rawDesc;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.pResource = src->_buf.Get();
    _device->_device->GetCopyableFootprints(&dstDesc, subres, 1, srcOffset, &srcLoc.PlacedFootprint, nullptr, nullptr, nullptr);
    srcLoc.PlacedFootprint.Offset = srcOffset;
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.pResource = dst->_tex.Get();
    dstLoc.SubresourceIndex = subres;

    _cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
}

CmdRenderPassD3D12::CmdRenderPassD3D12(CmdListD3D12* cmdList) noexcept
    : _cmdList(cmdList) {}

bool CmdRenderPassD3D12::IsValid() const noexcept {
    return _cmdList != nullptr;
}

void CmdRenderPassD3D12::Destroy() noexcept {
    _cmdList = nullptr;
}

void CmdRenderPassD3D12::SetViewport(Viewport viewport) noexcept {
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = viewport.X;
    vp.TopLeftY = viewport.Y;
    vp.Width = viewport.Width;
    vp.Height = viewport.Height;
    vp.MinDepth = viewport.MinDepth;
    vp.MaxDepth = viewport.MaxDepth;
    _cmdList->_cmdList->RSSetViewports(1, &vp);
}

void CmdRenderPassD3D12::SetScissor(Rect scissor) noexcept {
    D3D12_RECT rect{};
    rect.left = scissor.X;
    rect.top = scissor.Y;
    rect.right = scissor.X + scissor.Width;
    rect.bottom = scissor.Y + scissor.Height;
    _cmdList->_cmdList->RSSetScissorRects(1, &rect);
}

void CmdRenderPassD3D12::BindVertexBuffer(std::span<VertexBufferView> vbv) noexcept {
    if (_boundPso == nullptr) {
        _boundVbvs.clear();
        _boundVbvs.insert(_boundVbvs.end(), vbv.begin(), vbv.end());
    } else {
        const auto& strides = _boundPso->_arrayStrides;
        vector<D3D12_VERTEX_BUFFER_VIEW> rawVbvs;
        rawVbvs.reserve(vbv.size());
        for (size_t index = 0; index < std::min(vbv.size(), strides.size()); index++) {
            const VertexBufferView& i = vbv[index];
            D3D12_VERTEX_BUFFER_VIEW& raw = rawVbvs.emplace_back();
            auto buf = CastD3D12Object(i.Target);
            raw.BufferLocation = buf->_gpuAddr + i.Offset;
            raw.SizeInBytes = (UINT)buf->_desc.Size - i.Offset;
            raw.StrideInBytes = (UINT)strides[index];
        }
        _cmdList->_cmdList->IASetVertexBuffers(0, (UINT)rawVbvs.size(), rawVbvs.data());
    }
}

void CmdRenderPassD3D12::BindIndexBuffer(IndexBufferView ibv) noexcept {
    auto buf = CastD3D12Object(ibv.Target);
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = buf->_gpuAddr + ibv.Offset;
    view.SizeInBytes = (UINT)buf->_desc.Size - ibv.Offset;
    view.Format = ibv.Stride == 1 ? DXGI_FORMAT_R8_UINT : ibv.Stride == 2 ? DXGI_FORMAT_R16_UINT
                                                                          : DXGI_FORMAT_R32_UINT;
    _cmdList->_cmdList->IASetIndexBuffer(&view);
}

void CmdRenderPassD3D12::BindRootSignature(RootSignature* rootSig) noexcept {
    auto rs = CastD3D12Object(rootSig);
    _cmdList->_cmdList->SetGraphicsRootSignature(rs->_rootSig.Get());
    _boundRootSig = rs;
}

void CmdRenderPassD3D12::BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept {
    auto ps = CastD3D12Object(pso);
    _cmdList->_cmdList->SetPipelineState(ps->_pso.Get());
    _cmdList->_cmdList->IASetPrimitiveTopology(ps->_topo);
    _boundPso = ps;
    if (!_boundVbvs.empty()) {
        this->BindVertexBuffer(_boundVbvs);
        _boundVbvs.clear();
    }
}

void CmdRenderPassD3D12::PushConstant(const void* data, size_t length) noexcept {
    if (_boundRootSig == nullptr) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "unbound root signature");
        return;
    }
    auto [rootConstData, index] = _boundRootSig->_desc.GetRootConstant();
    if (index == std::numeric_limits<UINT>::max()) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "root signature has no root constant");
        return;
    }
    size_t rcSize = rootConstData.Num32BitValues * 4;
    if (length > rcSize) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::D3D12, Errors::InvalidOperation, "length");
        return;
    }
    _cmdList->_cmdList->SetGraphicsRoot32BitConstants(index, static_cast<UINT>(length / 4), data, 0);
}

void CmdRenderPassD3D12::BindRootDescriptor(uint32_t slot, ResourceView* view) noexcept {
    if (_boundRootSig == nullptr) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "unbound root signature");
        return;
    }
    auto [rootDescData, type, index] = _boundRootSig->_desc.GetRootDescriptor(slot);
    if (index == std::numeric_limits<UINT>::max()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::D3D12, Errors::InvalidOperation, "slot");
        return;
    }
    auto tag = view->GetTag();
    if (tag == RenderObjectTag::BufferView) {
        BufferViewD3D12* bufferView = static_cast<BufferViewD3D12*>(view);
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = bufferView->_buffer->_gpuAddr + bufferView->_desc.Range.Offset;
        auto usage = bufferView->_desc.Usage;
        if (usage.HasFlag(BufferUse::Resource)) {
            if (type != D3D12_ROOT_PARAMETER_TYPE_SRV) {
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "root parameter type mismatch");
                return;
            }
            _cmdList->_cmdList->SetGraphicsRootShaderResourceView(index, gpuAddr);
        } else if (usage.HasFlag(BufferUse::CBuffer)) {
            if (type != D3D12_ROOT_PARAMETER_TYPE_CBV) {
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "root parameter type mismatch");
                return;
            }
            _cmdList->_cmdList->SetGraphicsRootConstantBufferView(index, gpuAddr);
        } else if (usage.HasFlag(BufferUse::UnorderedAccess)) {
            if (type != D3D12_ROOT_PARAMETER_TYPE_UAV) {
                RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "root parameter type mismatch");
                return;
            }
            _cmdList->_cmdList->SetGraphicsRootUnorderedAccessView(index, gpuAddr);
        } else {
            RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "unsupported buffer view usage");
        }
    } else if (tag == RenderObjectTag::TextureView) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "cannot bind texture as root descriptor");
    } else {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "unsupported tag");
    }
}

void CmdRenderPassD3D12::BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept {
    if (_boundRootSig == nullptr) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "unbound root signature");
        return;
    }
    auto [tableData, index] = _boundRootSig->_desc.GetDescriptorTable(slot);
    if (index == std::numeric_limits<UINT>::max()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::D3D12, Errors::InvalidOperation, "slot");
        return;
    }
    auto descHeapView = CastD3D12Object(set);
    if (descHeapView->_resHeapView.IsValid()) {
        _cmdList->_cmdList->SetGraphicsRootDescriptorTable(index, descHeapView->_resHeapView.HandleGpu());
    }
    if (descHeapView->_samplerHeapView.IsValid()) {
        _cmdList->_cmdList->SetGraphicsRootDescriptorTable(index, descHeapView->_samplerHeapView.HandleGpu());
    }
}

void CmdRenderPassD3D12::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
    _cmdList->_cmdList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void CmdRenderPassD3D12::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
    _cmdList->_cmdList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
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

Nullable<Texture*> SwapChainD3D12::AcquireNext() noexcept {
    auto curr = _swapchain->GetCurrentBackBufferIndex();
    auto& frame = _frames[curr];
    ::WaitForSingleObject(frame.event.Get(), INFINITE);
    ::ResetEvent(frame.event.Get());
    return frame.image.get();
}

void SwapChainD3D12::Present() noexcept {
    UINT curr = _swapchain->GetCurrentBackBufferIndex();
    auto& frame = _frames[curr];
    UINT syncInterval = _desc.EnableSync ? 1 : 0;
    UINT presentFlags = (!_desc.EnableSync && _device->_isAllowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    if (HRESULT hr = _swapchain->Present(syncInterval, presentFlags);
        FAILED(hr)) {
        RADRAY_ABORT("{} {}::{} {} {}", Errors::D3D12, "IDXGISwapChain", "Present", GetErrorName(hr), hr);
    }
    _fenceValue++;
    auto queue = CastD3D12Object(_desc.PresentQueue);
    queue->_queue->Signal(_fence.Get(), _fenceValue);
    _fence->SetEventOnCompletion(_fenceValue, frame.event.Get());
}

Nullable<Texture*> SwapChainD3D12::GetCurrentBackBuffer() const noexcept {
    UINT curr = _swapchain->GetCurrentBackBufferIndex();
    return _frames[curr].image.get();
}

uint32_t SwapChainD3D12::GetCurrentBackBufferIndex() const noexcept {
    return _swapchain->GetCurrentBackBufferIndex();
}

uint32_t SwapChainD3D12::GetBackBufferCount() const noexcept {
    return static_cast<uint32_t>(_frames.size());
}

SwapChainDescriptor SwapChainD3D12::GetDesc() const noexcept {
    return _desc;
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

void* BufferD3D12::Map(uint64_t offset, uint64_t size) noexcept {
    D3D12_RANGE range{offset, offset + size};
    void* ptr = nullptr;
    if (HRESULT hr = _buf->Map(0, &range, &ptr);
        FAILED(hr)) {
        RADRAY_ABORT("{} {}::{} {} {}", Errors::D3D12, "ID3D12Resource", "Map", GetErrorName(hr), hr);
    }
    return ptr;
}

void BufferD3D12::Unmap(uint64_t offset, uint64_t size) noexcept {
    D3D12_RANGE range{offset, offset + size};
    _buf->Unmap(0, &range);
}

BufferViewD3D12::BufferViewD3D12(
    DeviceD3D12* device,
    BufferD3D12* buffer,
    CpuDescriptorHeapViewRAII heapView) noexcept
    : _device(device),
      _buffer(buffer),
      _heapView(std::move(heapView)) {}

bool BufferViewD3D12::IsValid() const noexcept {
    return _heapView.IsValid();
}

void BufferViewD3D12::Destroy() noexcept {
    _heapView.Destroy();
}

TextureD3D12::TextureD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Resource> tex,
    ComPtr<D3D12MA::Allocation> alloc) noexcept
    : _device(device),
      _tex(std::move(tex)),
      _alloc(std::move(alloc)) {
    _rawDesc = _tex->GetDesc();
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
    CpuDescriptorHeapViewRAII heapView) noexcept
    : _device(device),
      _texture(texture),
      _heapView(std::move(heapView)) {}

bool TextureViewD3D12::IsValid() const noexcept {
    return _heapView.IsValid();
}

void TextureViewD3D12::Destroy() noexcept {
    _heapView.Destroy();
}

bool Dxil::IsValid() const noexcept {
    return !_dxil.empty();
}

void Dxil::Destroy() noexcept {
    _dxil.clear();
    _dxil.shrink_to_fit();
}

D3D12_SHADER_BYTECODE Dxil::ToByteCode() const noexcept {
    return {_dxil.data(), _dxil.size()};
}

RootSigD3D12::RootSigD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12RootSignature> rootSig) noexcept
    : _device(device),
      _rootSig(std::move(rootSig)) {}

bool RootSigD3D12::IsValid() const noexcept {
    return _rootSig != nullptr;
}

void RootSigD3D12::Destroy() noexcept {
    _rootSig = nullptr;
}

GraphicsPsoD3D12::GraphicsPsoD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12PipelineState> pso,
    vector<uint64_t> arrayStrides,
    D3D12_PRIMITIVE_TOPOLOGY topo) noexcept
    : _device(device),
      _pso(std::move(pso)),
      _arrayStrides(std::move(arrayStrides)),
      _topo(topo) {}

bool GraphicsPsoD3D12::IsValid() const noexcept {
    return _pso != nullptr;
}

void GraphicsPsoD3D12::Destroy() noexcept {
    _pso = nullptr;
}

bool SimulateDescriptorSetLayoutD3D12::IsValid() const noexcept {
    return true;
}

void SimulateDescriptorSetLayoutD3D12::Destroy() noexcept {}

GpuDescriptorHeapViews::GpuDescriptorHeapViews(
    DeviceD3D12* device,
    GpuDescriptorHeapViewRAII resHeapView,
    GpuDescriptorHeapViewRAII samplerHeapView) noexcept
    : _device(device),
      _resHeapView(std::move(resHeapView)),
      _samplerHeapView(std::move(samplerHeapView)) {}

bool GpuDescriptorHeapViews::IsValid() const noexcept {
    return _resHeapView.IsValid() || _samplerHeapView.IsValid();
}

void GpuDescriptorHeapViews::Destroy() noexcept {
    _resHeapView.Destroy();
    _samplerHeapView.Destroy();
}

void GpuDescriptorHeapViews::SetResource(uint32_t slot, uint32_t index, ResourceView* view) noexcept {
    if (slot >= _elems.size()) {
        RADRAY_ERR_LOG("{} {} '{}", Errors::D3D12, Errors::ArgumentOutOfRange, "slot");
        return;
    }
    const auto& e = _elems[slot];
    if (index >= e._elem.Count) {
        RADRAY_ERR_LOG("{} {} '{}", Errors::D3D12, Errors::ArgumentOutOfRange, "index");
        return;
    }
    auto tag = view->GetTag();
    auto offset = _elemToHeapOffset[slot] + index;
    if (tag.HasFlag(RenderObjectTag::BufferView)) {
        BufferViewD3D12* bufferView = static_cast<BufferViewD3D12*>(view);
        bufferView->_heapView.CopyTo(0, 1, _resHeapView, offset);
    } else if (tag.HasFlag(RenderObjectTag::TextureView)) {
        TextureViewD3D12* texView = static_cast<TextureViewD3D12*>(view);
        texView->_heapView.CopyTo(0, 1, _resHeapView, offset);
    } else {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "unsupported tag");
    }
}

SamplerD3D12::SamplerD3D12(
    DeviceD3D12* device,
    CpuDescriptorHeapViewRAII heapView) noexcept
    : _device(device),
      _samplerView(std::move(heapView)) {}

bool SamplerD3D12::IsValid() const noexcept {
    return _samplerView.IsValid();
}

void SamplerD3D12::Destroy() noexcept {
    _samplerView.Destroy();
}

}  // namespace radray::render::d3d12
