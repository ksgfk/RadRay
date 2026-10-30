#include <radray/render/backend/d3d12_impl.h>
#include "external_resource_d3d12.h"
#include <radray/profiler.h>
#include "d3d12_diagnostics.h"

#include <bit>
#include <algorithm>
#include <limits>
#include <dxgidebug.h>

#include <radray/basic_math.h>
#include <radray/scope_guard.h>
#include <radray/text_encoding.h>
#ifdef RADRAY_ENABLE_PROFILER
#include <tracy/TracyD3D12.hpp>
#endif

// 章节索引。跳转: Grep "^// ==" 本文件。设计说明见 docs/architecture/render-rhi.md
//
//   == 命令签名与验证消息 ==
//   == 适配器枚举与选择 ==
//   == 描述符堆 ==
//   == 描述符分配器 ==
//   == DXGIFactory 与 DeviceD3D12 生命周期 ==
//   == CreateDevice ==
//   == Device: queue / cmdbuffer / fence / query / swapchain / buffer ==
//   == Device: texture 与 texture view ==
//   == Device: render pass / framebuffer / shader ==
//   == 根签名映射与构建 ==
//   == Device: pipeline layout 与 parameter set ==
//   == parameter 描述符写入 ==
//   == Device: PSO / sampler / fence ==
//   == CmdQueue / Fence / CmdList 与 barrier ==
//   == CmdList: 拷贝与 query ==
//   == 编码器与 SwapChain ==
//   == 各对象类实现 ==

namespace radray::render::d3d12 {

static void _CheckD3D12Result(DeviceD3D12* device, HRESULT hr, std::string_view operation) noexcept {
    if (SUCCEEDED(hr)) return;
    const HRESULT reason = device->_device->GetDeviceRemovedReason();
    detail::FlushDiagnostics(device->_diagnostics);
    RADRAY_ABORT("{} failed: {} {:#x}; device removed reason: {} {:#x}",
                 operation, GetErrorName(hr), static_cast<uint32_t>(hr),
                 GetErrorName(reason), static_cast<uint32_t>(reason));
}

static void _CheckD3D12DeviceStatus(DeviceD3D12* device, std::string_view operation) noexcept {
    const HRESULT reason = device->_device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) return;
    detail::FlushDiagnostics(device->_diagnostics);
    RADRAY_ABORT("{} failed: device removed reason: {} {:#x}",
                 operation, GetErrorName(reason), static_cast<uint32_t>(reason));
}

static void _CheckD3D12FenceResult(const FenceD3D12* fence, HRESULT hr, std::string_view operation) noexcept {
    ComPtr<ID3D12Device> device;
    const HRESULT parentHr = fence->_fence->GetDevice(IID_PPV_ARGS(device.GetAddressOf()));
    if (FAILED(parentHr)) {
        detail::FlushDiagnostics(fence->_device->_diagnostics);
        RADRAY_ABORT("{} failed: {} {:#x}; fence GetDevice failed: {} {:#x}",
                     operation, GetErrorName(hr), static_cast<uint32_t>(hr),
                     GetErrorName(parentHr), static_cast<uint32_t>(parentHr));
    }
    _CheckD3D12Result(fence->_device, hr, operation);
}

// == 命令签名与验证消息 ==

static ComPtr<ID3D12CommandSignature> _CreateIndirectCommandSignature(
    ID3D12Device* device,
    D3D12_INDIRECT_ARGUMENT_TYPE type,
    uint32_t byteStride) noexcept {
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = type;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = byteStride;
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;
    ComPtr<ID3D12CommandSignature> result;
    if (HRESULT hr = device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(result.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateCommandSignature failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    return result;
}

// == 适配器枚举与选择 ==

struct AdapterInfoD3D12 {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc;
    DXGIAdapterInfo publicInfo;
};

static string _GetAdapterName(const DXGI_ADAPTER_DESC1& desc) {
    wstring name{desc.Description};
    return ToMultiByte(name).value_or("???");
}

static bool _IsD3D12Supported(IDXGIAdapter1* adapter) noexcept {
    return SUCCEEDED(::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr));
}

static DXGI_FORMAT _SwapChainStorageFormat(TextureFormat format) noexcept {
    // Flip-model swapchains store UNORM; the requested sRGB interpretation belongs to the RTV.
    if (format == TextureFormat::BGRA8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (format == TextureFormat::RGBA8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_UNORM;
    return MapType(format);
}

static bool _IsPresentableHwnd(HWND hwnd) noexcept {
    if (hwnd == nullptr || ::IsWindow(hwnd) == 0 || ::IsIconic(hwnd) != 0 || ::IsWindowVisible(hwnd) == 0) {
        return false;
    }
    RECT client{};
    if (::GetClientRect(hwnd, &client) == 0) {
        return false;
    }
    return client.right > client.left && client.bottom > client.top;
}

static bool _RefreshSwapChainBackBuffers(SwapChainD3D12* swapChain) noexcept {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (HRESULT hr = swapChain->_swapchain->GetDesc1(&desc); FAILED(hr)) {
        RADRAY_ERR_LOG("IDXGISwapChain1::GetDesc1 failed: {} {}", GetErrorName(hr), hr);
        return false;
    }

    vector<SwapChainD3D12::Frame> frames;
    frames.reserve(desc.BufferCount);
    for (UINT i = 0; i < desc.BufferCount; i++) {
        auto& frame = frames.emplace_back();
        ComPtr<ID3D12Resource> rt;
        if (HRESULT hr = swapChain->_swapchain->GetBuffer(i, IID_PPV_ARGS(rt.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("IDXGISwapChain1::GetBuffer failed: {} {}", GetErrorName(hr), hr);
            return false;
        }
        auto name = fmt::format("SwapChain_BackBuffer_{}", i);
        frame.image = make_unique<TextureD3D12>(swapChain->_device, std::move(rt), ComPtr<D3D12MA::Allocation>{});
        frame.image->_name = name;
        frame.image->_dimension = TextureDimension::Dim2D;
        frame.image->_format = swapChain->_reqFormat;
        frame.image->_memory = MemoryType::Device;
        frame.image->_usage = TextureUse::CopySource | TextureUse::CopyDestination;
        if (frame.image->_rawDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
            frame.image->_usage |= TextureUse::RenderTarget;
        }
        if (!(frame.image->_rawDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
            frame.image->_usage |= TextureUse::Resource;
        }
        frame.image->_hints = ResourceHint::External;
        SetObjectName(name, frame.image->_tex.Get());
    }

    swapChain->_frames = std::move(frames);
    return true;
}

static DXGIAdapterInfo _MakeAdapterInfo(const DXGI_ADAPTER_DESC1& desc, uint32_t index, bool isD3D12Supported) {
    DXGIAdapterInfo info{};
    info.Index = index;
    info.Name = _GetAdapterName(desc);
    info.Type = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ? PhysicalDeviceType::Cpu : PhysicalDeviceType::Other;
    info.VendorId = desc.VendorId;
    info.DeviceId = desc.DeviceId;
    info.DedicatedVideoMemoryBytes = desc.DedicatedVideoMemory;
    info.DedicatedSystemMemoryBytes = desc.DedicatedSystemMemory;
    info.SharedSystemMemoryBytes = desc.SharedSystemMemory;
    info.IsSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    info.IsD3D12Supported = isD3D12Supported;
    return info;
}

static vector<AdapterInfoD3D12> _EnumerateAdapterInfos(IDXGIFactory4* factory, bool logAdapters) noexcept {
    vector<AdapterInfoD3D12> result;
    if (factory == nullptr) {
        RADRAY_ERR_LOG("DXGIFactory is null");
        return result;
    }

    for (uint32_t index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            RADRAY_ERR_LOG("IDXGIFactory4::EnumAdapters1 failed: index={} {} {}", index, GetErrorName(hr), hr);
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (HRESULT descHr = adapter->GetDesc1(&desc); FAILED(descHr)) {
            RADRAY_WARN_LOG("IDXGIAdapter1::GetDesc1 failed: index={} {} {}", index, GetErrorName(descHr), descHr);
            continue;
        }

        const bool d3d12Supported = _IsD3D12Supported(adapter.Get());
        auto publicInfo = _MakeAdapterInfo(desc, index, d3d12Supported);
        if (logAdapters && !publicInfo.IsSoftware) {
            RADRAY_INFO_LOG("d3d12 find adapter: {}", publicInfo.Name);
        }
        result.emplace_back(AdapterInfoD3D12{std::move(adapter), desc, std::move(publicInfo)});
    }
    return result;
}

static std::optional<uint32_t> _FindAdapterIndexByLuid(std::span<const AdapterInfoD3D12> adapters, LUID luid) noexcept {
    for (const auto& adapter : adapters) {
        if (adapter.desc.AdapterLuid.HighPart == luid.HighPart && adapter.desc.AdapterLuid.LowPart == luid.LowPart) {
            return adapter.publicInfo.Index;
        }
    }
    return std::nullopt;
}

static std::optional<uint32_t> _SelectHighPerformanceAdapterIndex(IDXGIFactory4* factory, std::span<const AdapterInfoD3D12> adapters) noexcept {
    if (adapters.empty()) {
        return std::nullopt;
    }

    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(factory6.GetAddressOf())))) {
        ComPtr<IDXGIAdapter1> adapter;
        for (uint32_t adapterIndex = 0;; ++adapterIndex) {
            if (FAILED(factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf())))) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) {
                continue;
            }
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || !_IsD3D12Supported(adapter.Get())) {
                continue;
            }
            auto index = _FindAdapterIndexByLuid(adapters, desc.AdapterLuid);
            if (index.has_value()) {
                return index.value();
            }
        }
    }

    for (const auto& adapter : adapters) {
        if (!adapter.publicInfo.IsSoftware && adapter.publicInfo.IsD3D12Supported) {
            return adapter.publicInfo.Index;
        }
    }
    return std::nullopt;
}

// == 描述符堆 ==

DescriptorHeap::DescriptorHeap(
    DeviceD3D12* device,
    D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept
    : _device(device->_device.Get()) {
    if (HRESULT hr = _device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(_heap.GetAddressOf()));
        FAILED(hr)) {
        detail::FlushDiagnostics(device->_diagnostics);
        RADRAY_ABORT("ID3D12Device::CreateDescriptorHeap failed: {} {}", GetErrorName(hr), hr);
    }
    _desc = _heap->GetDesc();
    _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    _gpuStart = IsShaderVisible() ? _heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{0};
    _incrementSize = _device->GetDescriptorHandleIncrementSize(_desc.Type);
    RADRAY_DEBUG_LOG(
        "create DescriptorHeap. Type={}, IsShaderVisible={}, IncrementSize={}, Length={}, all={}(bytes)",
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

// == 描述符分配器 ==

CpuDescriptorAllocator::CpuDescriptorAllocator(
    DeviceD3D12* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT basicSize,
    UINT keepFreePages) noexcept
    : _device(device),
      _type(type),
      _basicSize(basicSize),
      _freePageKeepCount(keepFreePages) {}

CpuDescriptorAllocator::Page::Page(
    unique_ptr<DescriptorHeap> heap,
    ComPtr<D3D12MA::VirtualBlock> allocator,
    size_t capacity) noexcept
    : Heap(std::move(heap)),
      Allocator(std::move(allocator)),
      Capacity(capacity) {}

std::optional<CpuDescriptorAllocator::Allocation> CpuDescriptorAllocator::Allocate(UINT count) noexcept {
    if (count == 0) {
        return std::nullopt;
    }
    const size_t request = static_cast<size_t>(count);
    D3D12MA::VIRTUAL_ALLOCATION_DESC allocDesc{};
    allocDesc.Size = static_cast<UINT64>(request);
    allocDesc.Alignment = 1;
    if (!_pages.empty()) {
        const size_t n = _pages.size();
        const size_t start = (_hint < n) ? _hint : 0;
        for (size_t step = 0; step < n; step++) {
            const size_t pageIndex = (start + step) % n;
            Page& page = *_pages[pageIndex];
            if (page.Capacity - page.Used < request) {
                continue;
            }
            D3D12MA::VirtualAllocation vAlloc{};
            UINT64 offset = 0;
            if (FAILED(page.Allocator->Allocate(&allocDesc, &vAlloc, &offset))) {
                continue;
            }
            if (offset > static_cast<UINT64>(std::numeric_limits<UINT>::max())) {
                page.Allocator->FreeAllocation(vAlloc);
                continue;
            }
            page.Used += request;
            _hint = pageIndex;
            return std::make_optional(Allocation{
                .Heap = page.Heap.get(),
                .Start = static_cast<UINT>(offset),
                .Length = count,
                .PagePtr = _pages[pageIndex].get(),
                .Offset = static_cast<size_t>(offset),
                .VirtualAlloc = vAlloc,
            });
        }
    }
    const size_t desired = std::max<size_t>(static_cast<size_t>(_basicSize), request);
    const size_t pageCapacity = std::bit_ceil(desired);
    if (pageCapacity > static_cast<size_t>(std::numeric_limits<UINT>::max())) {
        return std::nullopt;
    }
    auto heap = make_unique<DescriptorHeap>(
        _device,
        D3D12_DESCRIPTOR_HEAP_DESC{
            _type,
            static_cast<UINT>(pageCapacity),
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
            0});
    D3D12MA::VIRTUAL_BLOCK_DESC blockDesc{};
    blockDesc.Size = static_cast<UINT64>(pageCapacity);
    blockDesc.Flags = D3D12MA::VIRTUAL_BLOCK_FLAG_NONE;
    ComPtr<D3D12MA::VirtualBlock> block;
    if (FAILED(D3D12MA::CreateVirtualBlock(&blockDesc, block.GetAddressOf()))) {
        return std::nullopt;
    }
    auto newPage = make_unique<Page>(std::move(heap), std::move(block), pageCapacity);
    Page* newPagePtr = newPage.get();
    _pages.emplace_back(std::move(newPage));
    const size_t pageIndex = _pages.size() - 1;
    _hint = pageIndex;
    Page& page = *_pages.back();
    D3D12MA::VirtualAllocation vAlloc{};
    UINT64 offset = 0;
    if (FAILED(page.Allocator->Allocate(&allocDesc, &vAlloc, &offset)) ||
        offset > static_cast<UINT64>(std::numeric_limits<UINT>::max())) {
        if (vAlloc.AllocHandle != 0) {
            page.Allocator->FreeAllocation(vAlloc);
        }
        _pages.pop_back();
        return std::nullopt;
    }
    page.Used += request;
    return std::make_optional(Allocation{
        .Heap = page.Heap.get(),
        .Start = static_cast<UINT>(offset),
        .Length = count,
        .PagePtr = newPagePtr,
        .Offset = static_cast<size_t>(offset),
        .VirtualAlloc = vAlloc,
    });
}

void CpuDescriptorAllocator::Destroy(CpuDescriptorAllocator::Allocation view) noexcept {
    if (view.PagePtr == nullptr) {
        return;
    }
    auto* page = view.PagePtr;
    RADRAY_ASSERT(page->Heap.get() == view.Heap);
    page->Allocator->FreeAllocation(view.VirtualAlloc);
    page->Used -= static_cast<size_t>(view.Length);
    if (page->Used == 0) {
        TryReleaseFreePages();
    }
}

void CpuDescriptorAllocator::TryReleaseFreePages() noexcept {
    if (_freePageKeepCount == std::numeric_limits<uint32_t>::max()) {
        return;
    }
    size_t freeCount = 0;
    for (const auto& page : _pages) {
        freeCount += (page->Used == 0) ? 1 : 0;
    }
    while (freeCount > static_cast<size_t>(_freePageKeepCount) && !_pages.empty()) {
        size_t victim = _pages.size();
        for (size_t i = _pages.size(); i-- > 0;) {
            if (_pages[i]->Used == 0) {
                victim = i;
                break;
            }
        }
        if (victim == _pages.size()) {
            return;
        }
        const size_t last = _pages.size() - 1;
        if (victim != last) {
            std::swap(_pages[victim], _pages[last]);
            if (_hint == last) {
                _hint = victim;
            }
        }
        _pages.pop_back();
        if (_hint >= _pages.size()) {
            _hint = 0;
        }
        --freeCount;
    }
}

GpuDescriptorAllocator::GpuDescriptorAllocator(
    DeviceD3D12* device,
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

std::optional<GpuDescriptorAllocator::Allocation> GpuDescriptorAllocator::Allocate(UINT count) noexcept {
    const auto allocation = _allocator.Allocate(count);
    if (!allocation.has_value()) {
        return std::nullopt;
    }
    const auto& v = allocation.value();
    return std::make_optional(GpuDescriptorAllocator::Allocation{_heap.get(), static_cast<UINT>(v.Start), count, v});
}

void GpuDescriptorAllocator::Destroy(GpuDescriptorAllocator::Allocation allocation) noexcept {
    _allocator.Destroy(allocation.ParentAllocation);
}

// == DXGIFactory 与 DeviceD3D12 生命周期 ==

DXGIFactoryImpl::DXGIFactoryImpl(
    ComPtr<IDXGIFactory4> factory,
    const DXGIFactoryDescriptor& desc,
    shared_ptr<detail::DiagnosticSession> diagnostics) noexcept
    : _factory(std::move(factory)),
      _desc(desc),
      _validationEnabled(diagnostics != nullptr),
      _diagnostics(std::move(diagnostics)) {}

DXGIFactoryImpl::~DXGIFactoryImpl() noexcept {
    DestroyImpl();
}

bool DXGIFactoryImpl::IsValid() const noexcept {
    return _factory != nullptr;
}

void DXGIFactoryImpl::Destroy() noexcept {
    DestroyImpl();
}

void DXGIFactoryImpl::DestroyImpl() noexcept {
    {
        detail::DiagnosticTopologyScope change{_diagnostics};
        _factory = nullptr;
    }
    _diagnostics.reset();
    _validationEnabled = false;
    _desc.LogCallback = nullptr;
    _desc.LogUserData = nullptr;
    _desc.DiagnosticContext.reset();
}

vector<DXGIAdapterInfo> DXGIFactoryImpl::GetAdapters() const noexcept {
    vector<DXGIAdapterInfo> result;
    auto adapters = _EnumerateAdapterInfos(_factory.Get(), false);
    result.reserve(adapters.size());
    for (const auto& adapter : adapters) {
        result.emplace_back(adapter.publicInfo);
    }
    return result;
}

std::optional<uint32_t> DXGIFactoryImpl::SelectHighPerformanceAdapter() const noexcept {
    auto adapters = _EnumerateAdapterInfos(_factory.Get(), false);
    return _SelectHighPerformanceAdapterIndex(_factory.Get(), adapters);
}

Nullable<unique_ptr<DXGIFactory>> CreateDXGIFactory(const DXGIFactoryDescriptor& desc) {
    uint32_t dxgiFactoryFlags = 0;
    shared_ptr<detail::DiagnosticSession> diagnostics;
    if (desc.IsEnableDebugLayer) {
        if (!desc.DiagnosticContext) {
            RADRAY_ERR_LOG("D3D12 validation requires an explicit DXGIDiagnosticContext");
            return nullptr;
        }
        ComPtr<ID3D12Debug> debugController;
        HRESULT status = ::D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
        if (FAILED(status)) {
            RADRAY_ERR_LOG("D3D12GetDebugInterface failed: {:#x}", static_cast<uint32_t>(status));
            return nullptr;
        }
        debugController->EnableDebugLayer();
        if (desc.IsEnableGpuBasedValid) {
            ComPtr<ID3D12Debug1> debug1;
            status = debugController.As(&debug1);
            if (FAILED(status)) {
                RADRAY_ERR_LOG("ID3D12Debug1 query failed: {:#x}", static_cast<uint32_t>(status));
                return nullptr;
            }
            debug1->SetEnableGPUBasedValidation(true);
        }
        auto session = detail::CreateDiagnosticSession(desc);
        if (!session) return nullptr;
        diagnostics = session.Release();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
    ComPtr<IDXGIFactory4> factory;
    auto releaseFactory = MakeScopeGuard([&] {
        if (!factory) return;
        detail::DiagnosticTopologyScope change{diagnostics};
        factory.Reset();
    });
    HRESULT status;
    {
        detail::DiagnosticTopologyScope change{diagnostics};
        status = ::CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(factory.GetAddressOf()));
    }
    if (FAILED(status)) {
        RADRAY_ERR_LOG("CreateDXGIFactory2 failed: {} {:#x}", GetErrorName(status), static_cast<uint32_t>(status));
        return nullptr;
    }
    if (diagnostics && (!detail::FlushDiagnostics(diagnostics) || !detail::IsDiagnosticHealthy(diagnostics))) return nullptr;
    return make_unique<DXGIFactoryImpl>(factory, desc, std::move(diagnostics));
}

DeviceD3D12::DeviceD3D12(
    ComPtr<ID3D12Device> device,
    ComPtr<IDXGIFactory4> dxgiFactory,
    ComPtr<IDXGIAdapter1> dxgiAdapter,
    ComPtr<D3D12MA::Allocator> mainAlloc,
    shared_ptr<detail::DiagnosticSession> diagnostics,
    shared_ptr<detail::DiagnosticChannel> diagnosticChannel) noexcept
    : _device(std::move(device)),
      _dxgiFactory(std::move(dxgiFactory)),
      _dxgiAdapter(std::move(dxgiAdapter)),
      _mainAlloc(std::move(mainAlloc)),
      _samplerCache(this),
      _isDebugLayerEnabled(diagnostics != nullptr),
      _diagnostics(std::move(diagnostics)),
      _diagnosticChannel(std::move(diagnosticChannel)) {
    _drawIndirectSignature = _CreateIndirectCommandSignature(
        _device.Get(), D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, sizeof(DrawIndirectArguments));
    _drawIndexedIndirectSignature = _CreateIndirectCommandSignature(
        _device.Get(), D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, sizeof(DrawIndexedIndirectArguments));
    _dispatchIndirectSignature = _CreateIndirectCommandSignature(
        _device.Get(), D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, sizeof(DispatchIndirectArguments));
    _cpuResAlloc = make_unique<CpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 512, 1);
    _cpuRtvAlloc = make_unique<CpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 128, 1);
    _cpuDsvAlloc = make_unique<CpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 128, 1);
    _cpuSamplerAlloc = make_unique<CpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 128, 1);
    _gpuResHeap = make_unique<GpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 16);
    _gpuSamplerHeap = make_unique<GpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
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
    if (!_device) return;
    for (auto& queues : _queues) queues.clear();
    _samplerCache.Clear();
    _gpuResHeap = nullptr;
    _gpuSamplerHeap = nullptr;
    _cpuResAlloc = nullptr;
    _cpuRtvAlloc = nullptr;
    _cpuDsvAlloc = nullptr;
    _cpuSamplerAlloc = nullptr;
    _drawIndirectSignature = nullptr;
    _drawIndexedIndirectSignature = nullptr;
    _dispatchIndirectSignature = nullptr;
    _mainAlloc = nullptr;
    detail::FlushDiagnostics(_diagnostics);
    _diagnosticChannel.reset();
    _isDebugLayerEnabled = false;

    {
        detail::DiagnosticTopologyScope change{_diagnostics};
        _device = nullptr;
        _dxgiAdapter = nullptr;
        _dxgiFactory = nullptr;
    }
    _diagnostics.reset();
}

// == CreateDevice ==

Nullable<shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc) {
    if (desc.Factory == nullptr || !desc.Factory->IsValid()) {
        RADRAY_ERR_LOG("DXGIFactory is null or invalid");
        return nullptr;
    }

    auto* factoryImpl = CastD3D12Object(desc.Factory);
    ComPtr<IDXGIFactory4> dxgiFactory = factoryImpl->_factory;
    auto adapters = _EnumerateAdapterInfos(dxgiFactory.Get(), true);
    if (adapters.empty()) {
        RADRAY_ERR_LOG("d3d12 cannot find available adapter");
        return nullptr;
    }

    uint32_t selectedAdapterIndex = std::numeric_limits<uint32_t>::max();
    if (desc.AdapterIndex.has_value()) {
        selectedAdapterIndex = desc.AdapterIndex.value();
    } else {
        auto selected = _SelectHighPerformanceAdapterIndex(dxgiFactory.Get(), adapters);
        if (!selected.has_value()) {
            RADRAY_ERR_LOG("d3d12 cannot find available adapter");
            return nullptr;
        }
        selectedAdapterIndex = selected.value();
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (const auto& adapterInfo : adapters) {
        if (adapterInfo.publicInfo.Index == selectedAdapterIndex) {
            adapter = adapterInfo.adapter;
            break;
        }
    }
    if (adapter == nullptr) {
        RADRAY_ERR_LOG("d3d12 cannot find adapter: index={}", selectedAdapterIndex);
        return nullptr;
    }
    ComPtr<ID3D12Device> device;
    auto releaseDevice = MakeScopeGuard([&] {
        if (!device) return;
        detail::DiagnosticTopologyScope change{factoryImpl->_diagnostics};
        device.Reset();
    });
    {
        detail::DiagnosticTopologyScope change{factoryImpl->_diagnostics};
        if (HRESULT hr = ::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("D3D12CreateDevice failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    {
        DXGI_ADAPTER_DESC1 adapDesc{};
        adapter->GetDesc1(&adapDesc);
        wstring s{adapDesc.Description};
        RADRAY_INFO_LOG("d3d12 select adapter: {}", ToMultiByte(s).value_or("???"));
    }
    shared_ptr<detail::DiagnosticChannel> diagnosticChannel;
    if (factoryImpl->_diagnostics) {
        ComPtr<ID3D12InfoQueue> queue;
        HRESULT status = device.As(&queue);
        if (FAILED(status)) {
            RADRAY_ERR_LOG("ID3D12InfoQueue query failed: {:#x}", static_cast<uint32_t>(status));
            return nullptr;
        }
        ComPtr<ID3D12InfoQueue1> callbackQueue;
        status = queue.As(&callbackQueue);
        if (FAILED(status) && status != E_NOINTERFACE) {
            RADRAY_ERR_LOG("ID3D12InfoQueue1 query failed: {:#x}", static_cast<uint32_t>(status));
            return nullptr;
        }
        auto channel = detail::AttachDiagnosticChannel(factoryImpl->_diagnostics, queue, callbackQueue.Get());
        if (!channel) return nullptr;
        diagnosticChannel = channel.Release();
    }
    ComPtr<D3D12MA::Allocator> alloc;
    {
        D3D12MA::ALLOCATOR_DESC allocDesc{};
        allocDesc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        allocDesc.pDevice = device.Get();
        allocDesc.pAdapter = adapter.Get();
        allocDesc.Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED;
        if (HRESULT hr = D3D12MA::CreateAllocator(&allocDesc, alloc.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("D3D12MA::CreateAllocator failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    auto result = make_shared<DeviceD3D12>(device, dxgiFactory, adapter, alloc, factoryImpl->_diagnostics, std::move(diagnosticChannel));
    if (desc.EnableExternalResource) {
        result->_externalExtension = make_unique<ExternalResourceD3D12>(result.get());
    }
    DeviceDetail& detail = result->_capabilities.Detail;
    detail.CBufferAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    detail.BufferCopyOffsetAlignment = 1;
    detail.TextureDataPitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    detail.TextureDataPlacementAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    detail.MaxVertexInputBindings = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    {
        DXGI_ADAPTER_DESC1 adapDesc{};
        if (HRESULT hr = adapter->GetDesc1(&adapDesc); SUCCEEDED(hr)) {
            wstring name{adapDesc.Description};
            detail.GpuName = ToMultiByte(name).value_or("???");
        }
    }
    {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(adapter.As(&adapter3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
            if (HRESULT hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo);
                SUCCEEDED(hr)) {
                detail.VramBudget = memInfo.Budget;
            }
        }
    }
    RADRAY_INFO_LOG("========== Feature ==========");
    {
        LARGE_INTEGER l;
        HRESULT hr = adapter->CheckInterfaceSupport(IID_IDXGIDevice, &l);
        if (SUCCEEDED(hr)) {
            const int64_t mask = 0xFFFF;
            auto quad = l.QuadPart;
            auto ver = fmt::format(
                "{}.{}.{}.{}",
                quad >> 48,
                (quad >> 32) & mask,
                (quad >> 16) & mask,
                quad & mask);
            RADRAY_INFO_LOG("d3d12 driver Version: {}", ver);
        } else {
            RADRAY_WARN_LOG("IDXGIAdapter::CheckInterfaceSupport failed: {} {}", GetErrorName(hr), hr);
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
                RADRAY_WARN_LOG("IDXGIFactory6::CheckFeatureSupport failed: {} {}", GetErrorName(hr), hr);
            }
        }
        RADRAY_INFO_LOG("Allow Tearing: {}", static_cast<bool>(allowTearing));
        result->_isAllowTearing = allowTearing;
    }
    const CD3DX12FeatureSupport& fs = result->_features;
    if (SUCCEEDED(fs.GetStatus())) {
        RADRAY_INFO_LOG("Feature Level: {}", fs.MaxSupportedFeatureLevel());
        RADRAY_INFO_LOG("Shader Model: {}", fs.HighestShaderModel());
        RADRAY_INFO_LOG("Resource Binding Tier: {}", fs.ResourceBindingTier());
        RADRAY_INFO_LOG("Resource Heap Tier: {}", fs.ResourceHeapTier());
        RADRAY_INFO_LOG("TBR: {}", static_cast<bool>(fs.TileBasedRenderer()));
        detail.IsUMA = static_cast<bool>(fs.UMA());
        RADRAY_INFO_LOG("UMA: {}", detail.IsUMA);
    } else {
        RADRAY_WARN_LOG("CD3DX12FeatureSupport::GetStatus failed");
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (HRESULT hr = device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
            SUCCEEDED(hr)) {
            detail.IsLayeredRenderingFromVertexShaderSupported =
                static_cast<bool>(options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation);
            RADRAY_INFO_LOG(
                "Layered Rendering From Vertex Shader: {}",
                detail.IsLayeredRenderingFromVertexShaderSupported);
        } else {
            RADRAY_WARN_LOG(
                "ID3D12Device::CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS) failed: {} {}",
                GetErrorName(hr),
                hr);
        }
    }
    auto& caps = result->_capabilities;
    caps.Limits = {
        .MaxColorAttachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT,
        .MaxTexture1DDimension = D3D12_REQ_TEXTURE1D_U_DIMENSION,
        .MaxTexture2DDimension = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        .MaxTexture3DDimension = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION,
        .MaxTextureArrayLayers = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION,
        .MaxBufferSize = D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * uint64_t{1024 * 1024},
        .MaxUniformBufferRange = D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * uint64_t{16},
        .MaxPushConstantBytes = D3D12_MAX_ROOT_COST * 4,
        .CBufferOffsetAlignment = detail.CBufferAlignment,
        .StorageBufferOffsetAlignment = 4};
    caps.Features = {
        .TimestampQueries = true,
        .IndirectDraw = true,
        .IndirectFirstInstance = true,
        .IndirectDispatch = true,
        .SubresourceBarriers = true,
        .IndependentDepthStencilTransitions = true,
        .UavMemoryBarrier = true,
        .UavWriteStages = render::ShaderStage::Pixel | render::ShaderStage::Compute};
    if (SUCCEEDED(fs.GetStatus()) &&
        fs.MaxSupportedFeatureLevel() >= D3D_FEATURE_LEVEL_11_1) {
        caps.Features.UavWriteStages |= render::ShaderStage::Vertex;
    }
    caps.Features.SamplerAnisotropy = caps.Features.SamplerMirrorClampToEdge = true;
    caps.Features.SamplerAnisotropyPointMip = fs.AnisoFilterWithPointMipSupported();
    caps.Features.SamplerFilterMinmax = fs.TiledResourcesTier() >= D3D12_TILED_RESOURCES_TIER_2;
    caps.Limits.MaxSamplerAnisotropy = D3D12_REQ_MAXANISOTROPY;
    caps.Limits.MinSamplerLodBias = D3D12_MIP_LOD_BIAS_MIN;
    caps.Limits.MaxSamplerLodBias = D3D12_MIP_LOD_BIAS_MAX;
    DXGI_ADAPTER_DESC1 memoryDesc{};
    if (SUCCEEDED(adapter->GetDesc1(&memoryDesc))) {
        const uint64_t memory = memoryDesc.DedicatedVideoMemory != 0 ? memoryDesc.DedicatedVideoMemory : memoryDesc.SharedSystemMemory;
        caps.Limits.MaxBufferSize = std::max(caps.Limits.MaxBufferSize,
                                             std::min(uint64_t{D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM} * 1024 * 1024, memory / 4));
    }
    if (!result->CreateQueues(desc.QueueCounts)) return nullptr;
    if (result->_diagnostics && (!detail::FlushDiagnostics(result->_diagnostics) || !detail::IsDiagnosticHealthy(result->_diagnostics) ||
                                 !detail::IsDiagnosticHealthy(result->_diagnosticChannel))) return nullptr;
    RADRAY_INFO_LOG("=============================");
    return result;
}

// == Device: queue / cmdbuffer / fence / query / swapchain / buffer ==

DeviceDetail DeviceD3D12::GetDetail() const noexcept {
    return _capabilities.Detail;
}

Nullable<CommandQueue*> DeviceD3D12::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    const size_t index = static_cast<size_t>(type);
    if (index >= _queues.size() || slot >= _queues[index].size()) return nullptr;
    return _queues[index][slot].get();
}

bool DeviceD3D12::CreateQueues(const array<uint32_t, static_cast<size_t>(QueueType::MAX_COUNT)>& counts) noexcept {
    for (size_t index = 0; index < counts.size(); ++index) {
        for (uint32_t slot = 0; slot < counts[index]; ++slot) {
            auto fence = CreateFenceD3D12(0);
            if (!fence) return false;
            ComPtr<ID3D12CommandQueue> queue;
            D3D12_COMMAND_QUEUE_DESC desc{};
            desc.Type = MapType(static_cast<QueueType>(index));
            if (const HRESULT hr = _device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf())); FAILED(hr)) {
                RADRAY_ERR_LOG("ID3D12Device::CreateCommandQueue failed: {} {}", GetErrorName(hr), hr);
                return false;
            }
            auto instance = make_unique<CmdQueueD3D12>(this, std::move(queue), desc.Type, fence.Release());
            SetObjectName(fmt::format("Queue-{}-{}", static_cast<QueueType>(index), slot), instance->_queue.Get());
            _queues[index].push_back(std::move(instance));
        }
        _capabilities.Queues[index] = {counts[index], false};
    }
    return true;
}

AttachmentResolveSupport DeviceD3D12::QueryAttachmentResolveSupport(TextureFormat format, uint32_t samples) const noexcept {
    if (samples <= 1 || (samples & (samples - 1)) || !_features.RenderPassesValid() || _features.ProgrammableSamplePositionsTier() == D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED) return {};
    const bool depth = IsDepthStencilFormat(format);
    const auto support = QueryTextureSupport({TextureDimension::Dim2D, format, depth ? TextureUse::DepthStencilWrite : TextureUse::RenderTarget});
    if (!support.Supported || !(support.SampleCounts.value() & samples) || !support.SampleCounts.HasFlag(SampleCount::X1)) return {};
    AttachmentResolveSupport result;
    if (depth) {
        result.DepthModes = ResolveMode::Average | ResolveMode::Min | ResolveMode::Max;
        if (GetTextureFormatAspects(format).HasFlag(TextureAspect::Stencil)) result.StencilModes = ResolveMode::Min | ResolveMode::Max;
        result.IndependentResolve = result.IndependentResolveNone = true;
    } else {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT native{MapType(format), {}, {}};
        if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &native, sizeof(native))))
            result.ColorAverage = !IsUintFormat(format) && !IsSintFormat(format) && (native.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) != 0;
    }
    return result;
}

TextureSupport DeviceD3D12::QueryTextureSupport(const TextureSupportQuery& query) const noexcept {
    HRESULT status = S_OK;
    return QueryTextureSupportWithNativeStatus(query, status);
}

TextureSupport DeviceD3D12::QueryTextureSupportWithNativeStatus(const TextureSupportQuery& query, HRESULT& status) const noexcept {
    status = S_OK;
    if (!IsValidTextureSupportQuery(query)) return {};
    const DXGI_FORMAT format = MapType(query.Format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, {}, {}};
    status = _device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(status)) return {};
    D3D12_FORMAT_SUPPORT1 dimension{};
    switch (query.Dimension) {
        case TextureDimension::Dim1D:
        case TextureDimension::Dim1DArray: dimension = D3D12_FORMAT_SUPPORT1_TEXTURE1D; break;
        case TextureDimension::Dim2D:
        case TextureDimension::Dim2DArray: dimension = D3D12_FORMAT_SUPPORT1_TEXTURE2D; break;
        case TextureDimension::Cube:
        case TextureDimension::CubeArray: dimension = D3D12_FORMAT_SUPPORT1_TEXTURECUBE; break;
        case TextureDimension::Dim3D: dimension = D3D12_FORMAT_SUPPORT1_TEXTURE3D; break;
        default: return {};
    }
    if ((support.Support1 & dimension) == 0) return {};
    if (query.Usage.HasFlag(TextureUse::RenderTarget) && (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) return {};
    if ((query.Usage & (TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite)) &&
        (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) == 0) return {};
    if (query.Usage.HasFlag(TextureUse::UnorderedAccess) &&
        ((support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0 ||
         (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0 ||
         (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0)) return {};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT sampledSupport{MapShaderResourceType(query.Format), {}, {}};
    status = _device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &sampledSupport, sizeof(sampledSupport));
    if (FAILED(status)) return {};
    if (query.Usage.HasFlag(TextureUse::Resource) &&
        (sampledSupport.Support1 & (D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) == 0) return {};
    TextureSupport result{};
    result.LinearFiltering = (sampledSupport.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
    result.MinMaxFiltering = _capabilities.Features.SamplerFilterMinmax && result.LinearFiltering && !IsUintFormat(query.Format) && !IsSintFormat(query.Format);
    result.Blending = (support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0;
    const bool is1D = query.Dimension == TextureDimension::Dim1D || query.Dimension == TextureDimension::Dim1DArray;
    const bool is3D = query.Dimension == TextureDimension::Dim3D;
    const auto& limits = _capabilities.Limits;
    result.MaxWidth = is1D ? limits.MaxTexture1DDimension : is3D ? limits.MaxTexture3DDimension
                                                                 : limits.MaxTexture2DDimension;
    result.MaxHeight = is1D ? 1 : result.MaxWidth;
    result.MaxDepth = is3D ? limits.MaxTexture3DDimension : 1;
    result.MaxArrayLayers = is3D ? 1 : limits.MaxTextureArrayLayers;
    result.MaxMipLevels = D3D12_REQ_MIP_LEVELS;
    result.MaxResourceSize = limits.MaxBufferSize;
    for (uint32_t samples = 1; samples <= 16; samples *= 2) {
        if (samples > 1 && (query.Usage.HasFlag(TextureUse::UnorderedAccess) ||
                            (query.Dimension != TextureDimension::Dim2D && query.Dimension != TextureDimension::Dim2DArray))) break;
        if (samples > 1 && query.Usage.HasFlag(TextureUse::Resource) &&
            (sampledSupport.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD) == 0) continue;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
        status = _device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality));
        if (FAILED(status)) return {};
        if (quality.NumQualityLevels > 0) {
            result.SampleCounts |= static_cast<SampleCount>(samples);
        }
    }
    result.Supported = bool(result.SampleCounts);
    return result;
}

Nullable<unique_ptr<CommandAllocator>> DeviceD3D12::CreateCommandAllocator(CommandQueue* queue_) noexcept {
    Nullable<CmdQueueD3D12*> queue = CastD3D12Object(queue_);
    ComPtr<ID3D12CommandAllocator> allocator;
    const HRESULT hr = _device->CreateCommandAllocator(queue->_type, IID_PPV_ARGS(allocator.GetAddressOf()));
    if (FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateCommandAllocator failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    return make_unique<CommandAllocatorD3D12>(this, queue.Get(), std::move(allocator));
}

Nullable<unique_ptr<CommandBuffer>> DeviceD3D12::CreateCommandBuffer(CommandAllocator* allocator_) noexcept {
    Nullable<CommandAllocatorD3D12*> allocator = CastD3D12Object(allocator_);
    auto* queue = allocator->_queue;
    HRESULT hr;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Device4> device4;
    if (SUCCEEDED(_device.As(&device4))) {
        hr = device4->CreateCommandList1(0, queue->_type, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(list.GetAddressOf()));
    } else {
        hr = _device->CreateCommandList(0, queue->_type, allocator->_native.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        if (SUCCEEDED(hr)) _CheckD3D12Result(this, list->Close(), "ID3D12GraphicsCommandList::Close");
    }
    if (FAILED(hr)) {
        RADRAY_ERR_LOG("D3D12 command list creation failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    return make_unique<CmdListD3D12>(this, queue, allocator.Get(), std::move(list), queue->_type);
}

Nullable<ExternalResourceExtension*> DeviceD3D12::GetExternalResourceExtension() noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    return _externalExtension.get();
#else
    return nullptr;
#endif
}

Nullable<unique_ptr<Fence>> DeviceD3D12::CreateFence() noexcept {
    return this->CreateFenceD3D12(0);
}

Nullable<unique_ptr<QueryPool>> DeviceD3D12::CreateQueryPool(const QueryPoolDescriptor& desc) noexcept {
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = desc.Count;
    heapDesc.NodeMask = 0;

    ComPtr<ID3D12QueryHeap> heap;
    if (HRESULT hr = _device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(heap.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateQueryHeap failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }

    auto result = make_unique<QueryPoolD3D12>(this, std::move(heap), desc.Type, desc.Count);
    if (!desc.DebugName.empty()) {
        result->SetDebugName(desc.DebugName);
    }
    return result;
}

Nullable<unique_ptr<SwapChain>> DeviceD3D12::CreateSwapChain(const SwapChainDescriptor& desc) noexcept {
    detail::DiagnosticTopologyScope change{_diagnostics};
    // https://learn.microsoft.com/zh-cn/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = desc.Width;
    scDesc.Height = desc.Height;
    scDesc.Format = _SwapChainStorageFormat(desc.Format);
    scDesc.Stereo = false;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = desc.BackBufferCount;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = 0;
    scDesc.Flags |= _isAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    scDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    auto queue = CastD3D12Object(desc.PresentQueue);
    HWND hwnd = std::bit_cast<HWND>(desc.NativeHandler);
    ComPtr<IDXGISwapChain1> temp;
    if (HRESULT hr = _dxgiFactory->CreateSwapChainForHwnd(queue->_queue.Get(), hwnd, &scDesc, nullptr, nullptr, temp.GetAddressOf());
        FAILED(hr)) {
        RADRAY_ERR_LOG("IDXGIFactory::CreateSwapChainForHwnd failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    if (HRESULT hr = _dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // 阻止 Alt + Enter 进全屏
        FAILED(hr)) {
        RADRAY_WARN_LOG("IDXGIFactory::MakeWindowAssociation failed: {} {}", GetErrorName(hr), hr);
    }
    ComPtr<IDXGISwapChain3> swapchain;
    if (HRESULT hr = temp->QueryInterface(IID_PPV_ARGS(swapchain.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("IDXGISwapChain1::QueryInterface failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    constexpr uint32_t MaximumFrameLatency = 3;
    if (HRESULT hr = swapchain->SetMaximumFrameLatency(MaximumFrameLatency); FAILED(hr)) {
        RADRAY_ERR_LOG("IDXGISwapChain3::SetMaximumFrameLatency({}) failed: {} {}", MaximumFrameLatency, GetErrorName(hr), hr);
        return nullptr;
    }
    auto result = make_unique<SwapChainD3D12>(this, swapchain, desc);
    result->_frameLatencyEvent = swapchain->GetFrameLatencyWaitableObject();
    if (!_RefreshSwapChainBackBuffers(result.get())) {
        return nullptr;
    }
    return result;
}

Nullable<unique_ptr<Buffer>> DeviceD3D12::CreateBuffer(const BufferDescriptor& desc_) noexcept {
    BufferDescriptor desc = desc_;
    const uint64_t logicalSize = desc.Size;
    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // Alignment must be 64KB (D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) or 0, which is effectively 64KB.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc
    resDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    // D3D12 要求 cbuffer 是 256 字节对齐
    // https://github.com/d3dcoder/d3d12book/blob/master/Common/d3dUtil.h#L99
    uint64_t allocSize = desc.Usage.HasFlag(BufferUse::CBuffer)
                             ? Align(desc.Size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
                             : desc.Size;
    resDesc.Width = allocSize;
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
    if (desc.Memory == MemoryType::ReadBack && !desc.Usage.HasFlag(BufferUse::Resource)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    UINT64 paddedSize;
    _device->GetCopyableFootprints(&resDesc, 0, 1, 0, nullptr, nullptr, nullptr, &paddedSize);
    if (paddedSize != std::numeric_limits<uint64_t>::max()) {
        allocSize = paddedSize;
        resDesc.Width = paddedSize;
    }
    D3D12_RESOURCE_STATES rawInitState = MapMemoryTypeToResourceState(desc.Memory);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = MapType(desc.Memory);
    allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        allocDesc.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocDesc.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }
    ComPtr<ID3D12Resource> buffer;
    ComPtr<D3D12MA::Allocation> allocRes;
    /**
     * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type
     * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_flags
     * upload 和 readback 只能以 GENERIC_READ 和 COPY_DEST 状态创建且不可转换。而 UAV 需要 UNORDERED_ACCESS 状态，需求冲突
     * 利用 Custom 堆开启 L0 系统内存并设置 WRITE_COMBINE 可以绕开 Abstract 堆的限制
     * L0 系统内存既可由 CPU 读写，NUMA 可由设备通过 PCIe 读写，UMA 更是能直接读写，可以同时满足 CPU 想 Mapped 读写，GPU 想 UAV 读写的需求
     * WRITE_COMBINE 设置后 CPU 写入会合并到缓冲区批量提交，NUMA 通过 PCIe 上传到设备。但 CPU 读取会变慢
     */
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
            RADRAY_ERR_LOG("ID3D12Device::CreateCommittedResource failed: {} {}", GetErrorName(hr), hr);
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
            RADRAY_ERR_LOG("D3D12MA::Allocator::CreateResource failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    auto result = make_unique<BufferD3D12>(this, std::move(buffer), std::move(allocRes));
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints;
    result->_reqSize = logicalSize;
    return result;
}

// == Device: texture 与 texture view ==

Nullable<unique_ptr<Texture>> DeviceD3D12::CreateTexture(const TextureDescriptor& desc_) noexcept {
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
    /**
     * https://learn.microsoft.com/en-us/windows/win32/api/dxgicommon/ns-dxgicommon-dxgi_sample_desc
     * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_multisample_quality_levels
     * https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules#multisample-anti-aliasing-rasterization-rules
     * 无 MSAA 时 quality 必须为 0 或 1
     * DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN 使用标准的 MSAA 模式, 避免厂商差异
     * 使用前需要调用 CheckFeatureSupport 确定 Format 支持的 quality，或直接用 DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN
     */
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
    // 优化 clear value 尚未接线: TextureDescriptor 不带 clear value, 故恒传 nullptr。
    const D3D12_CLEAR_VALUE* clearPtr = nullptr;
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = MapType(desc.Memory);
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
        RADRAY_ERR_LOG("D3D12MA::Allocator::CreateResource failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    auto result = make_unique<TextureD3D12>(this, std::move(texture), std::move(allocRes));
    result->_dimension = desc.Dim;
    result->_format = desc.Format;
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints;
    return result;
}

static DXGI_FORMAT _TexturePlaneFormat(const TextureViewDescriptor& desc, uint32_t plane) noexcept {
    if (plane == 1) {
        return desc.Target->GetDesc().Format == TextureFormat::D24_UNORM_S8_UINT
                   ? DXGI_FORMAT_X24_TYPELESS_G8_UINT
                   : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    }
    return MapShaderResourceType(desc.Format);
}

static DXGI_FORMAT _TextureViewFormat(const TextureViewDescriptor& desc, TextureAspects aspects) noexcept {
    if (desc.Usage != TextureViewUsage::Resource) return MapType(desc.Format);
    return _TexturePlaneFormat(desc, aspects == TextureAspect::Stencil ? 1 : 0);
}

std::pair<bool, string> DeviceD3D12::ValidateTextureView(const TextureViewDescriptor& desc) const {
    const auto* texture = static_cast<const TextureD3D12*>(desc.Target);
    const auto range = ResolveSubresourceRange(texture->GetDesc(), desc.Range);
    const auto format = _TextureViewFormat(desc, range.Aspects);
    const auto resourceFormat = texture->_rawDesc.Format;
    const auto family = FormatToTypeless(MapType(desc.Format));
    const bool swapchainSrgb = texture->_hints.HasFlag(ResourceHint::External) && desc.Usage == TextureViewUsage::RenderTarget &&
                               ((resourceFormat == DXGI_FORMAT_R8G8B8A8_UNORM && format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ||
                                (resourceFormat == DXGI_FORMAT_B8G8R8A8_UNORM && format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB));
    if (resourceFormat != format && resourceFormat != family && !swapchainSrgb)
        return {false, "D3D12 view format does not belong to the resource's typed/typeless format family"};
    if (desc.Usage != TextureViewUsage::Resource &&
        (range.MipLevelCount != 1 || desc.Dim == TextureDimension::Cube || desc.Dim == TextureDimension::CubeArray))
        return {false, "D3D12 attachment and UAV views require one mip and a non-cube view dimension"};
    const auto flags = texture->_rawDesc.Flags;
    D3D12_FORMAT_SUPPORT1 required{};
    switch (desc.Usage) {
        case TextureViewUsage::Resource:
            if (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
                return {false, "D3D12 resource denies shader resource views"};
            break;
        case TextureViewUsage::RenderTarget:
            if (!(flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || IsDepthStencilFormat(desc.Format))
                return {false, "D3D12 RTV requires a color format and ALLOW_RENDER_TARGET"};
            required = D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
            break;
        case TextureViewUsage::DepthRead:
        case TextureViewUsage::DepthReadStencilWrite:
        case TextureViewUsage::DepthWriteStencilRead:
        case TextureViewUsage::DepthWrite:
            if (!(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || !IsDepthStencilFormat(desc.Format) || desc.Dim == TextureDimension::Dim3D)
                return {false, "D3D12 DSV requires a depth/stencil format, dimension and resource flag"};
            required = D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
            break;
        case TextureViewUsage::UnorderedAccess:
            if (!(flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || range.Aspects != TextureAspect::Color)
                return {false, "D3D12 UAV requires a color aspect and ALLOW_UNORDERED_ACCESS"};
            required = D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
            break;
        case TextureViewUsage::UNKNOWN: return {false, "D3D12 view usage is invalid"};
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, {}, {}};
    if (FAILED(_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        return {false, "D3D12 view format support query failed"};
    if ((support.Support1 & required) != required)
        return {false, "D3D12 view format does not support the requested usage"};
    if (desc.Usage == TextureViewUsage::Resource &&
        ((support.Support1 & (D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) == 0 ||
         (texture->_rawDesc.SampleDesc.Count > 1 && !(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD))))
        return {false, "D3D12 SRV format does not support shader access for the resource's sample count"};
    if (desc.Usage == TextureViewUsage::UnorderedAccess &&
        (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) || !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))
        return {false, "D3D12 UAV format does not support typed load/store"};
    return {true, {}};
}

Nullable<unique_ptr<TextureView>> DeviceD3D12::CreateTextureView(const TextureViewDescriptor& desc) noexcept {
    // https://learn.microsoft.com/zh-cn/windows/win32/direct3d12/subresources
    // 三种 slice: mip 横向, array 纵向, plane 看起来更像是通道
    auto tex = CastD3D12Object(desc.Target);
    const bool multisampled = tex->_rawDesc.SampleDesc.Count > 1;
    auto requestedRange = desc.Range;
    if ((desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim2D || desc.Dim == TextureDimension::Dim3D) && requestedRange.ArrayLayerCount == SubresourceRange::All) requestedRange.ArrayLayerCount = 1;
    if (!requestedRange.Aspects && desc.Usage == TextureViewUsage::Resource && IsDepthStencilFormat(tex->_format)) requestedRange.Aspects = TextureAspect::Depth;
    const auto range = ResolveSubresourceRange(tex->GetDesc(), requestedRange);
    CpuDescriptorHeapViewRAII heapView{};
    DXGI_FORMAT dxgiFormat;
    if (desc.Usage == TextureViewUsage::Resource) {
        {
            auto heap = _cpuResAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate CBV descriptor");
                return nullptr;
            }
            heapView = {heap, heapViewOpt.value()};
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = _TextureViewFormat(desc, range.Aspects);
        srvDesc.Shader4ComponentMapping = range.Aspects == TextureAspect::Stencil
                                              ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
                                              : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (desc.Dim) {
            case TextureDimension::Dim1D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvDesc.Texture1D.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture1D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureDimension::Dim1DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvDesc.Texture1DArray.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture1DArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.Texture1DArray.FirstArraySlice = range.BaseArrayLayer;
                srvDesc.Texture1DArray.ArraySize = range.ArrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                if (multisampled) {
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                } else {
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MostDetailedMip = desc.Range.BaseMipLevel;
                    srvDesc.Texture2D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                    srvDesc.Texture2D.PlaneSlice = range.Aspects == TextureAspect::Stencil ? 1 : 0;
                }
                break;
            case TextureDimension::Dim2DArray:
                if (multisampled) {
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                    srvDesc.Texture2DMSArray.FirstArraySlice = range.BaseArrayLayer;
                    srvDesc.Texture2DMSArray.ArraySize = range.ArrayLayerCount;
                } else {
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    srvDesc.Texture2DArray.MostDetailedMip = desc.Range.BaseMipLevel;
                    srvDesc.Texture2DArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                    srvDesc.Texture2DArray.FirstArraySlice = range.BaseArrayLayer;
                    srvDesc.Texture2DArray.ArraySize = range.ArrayLayerCount;
                    srvDesc.Texture2DArray.PlaneSlice = range.Aspects == TextureAspect::Stencil ? 1 : 0;
                }
                break;
            case TextureDimension::Dim3D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture3D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureDimension::Cube:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.TextureCube.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureDimension::CubeArray:
                // https://learn.microsoft.com/zh-cn/windows/win32/api/d3d12/ns-d3d12-d3d12_texcube_array_srv
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvDesc.TextureCubeArray.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.TextureCubeArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.TextureCubeArray.First2DArrayFace = range.BaseArrayLayer;
                srvDesc.TextureCubeArray.NumCubes = range.ArrayLayerCount / 6;
                break;
            default:
                RADRAY_ERR_LOG("d3d12 invalid texture view dimension: {}", desc.Dim);
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), srvDesc, heapView.GetStart());
        dxgiFormat = srvDesc.Format;
    } else if (desc.Usage == TextureViewUsage::RenderTarget) {
        {
            auto heap = _cpuRtvAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate RTV descriptor");
                return nullptr;
            }
            heapView = {heap, heapViewOpt.value()};
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = MapType(desc.Format);
        switch (desc.Dim) {
            case TextureDimension::Dim1D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                rtvDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                rtvDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture1DArray.FirstArraySlice = range.BaseArrayLayer;
                rtvDesc.Texture1DArray.ArraySize = range.ArrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                if (multisampled) {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                } else {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    rtvDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                    rtvDesc.Texture2D.PlaneSlice = 0;
                }
                break;
            case TextureDimension::Dim2DArray:
                if (multisampled) {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    rtvDesc.Texture2DMSArray.FirstArraySlice = range.BaseArrayLayer;
                    rtvDesc.Texture2DMSArray.ArraySize = range.ArrayLayerCount;
                } else {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtvDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                    rtvDesc.Texture2DArray.FirstArraySlice = range.BaseArrayLayer;
                    rtvDesc.Texture2DArray.ArraySize = range.ArrayLayerCount;
                    rtvDesc.Texture2DArray.PlaneSlice = 0;
                }
                break;
            case TextureDimension::Dim3D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtvDesc.Texture3D.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture3D.FirstWSlice = 0;
                rtvDesc.Texture3D.WSize = static_cast<UINT>(-1);
                break;
            default:
                RADRAY_ERR_LOG("d3d12 invalid texture view dimension: {}", desc.Dim);
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), rtvDesc, heapView.GetStart());
        dxgiFormat = rtvDesc.Format;
    } else if (IsDepthStencilViewUsage(desc.Usage)) {
        {
            auto heap = _cpuDsvAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate DSV descriptor");
                return nullptr;
            }
            heapView = {heap, heapViewOpt.value()};
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = MapType(desc.Format);
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        if (IsDepthReadOnly(desc.Usage)) dsvDesc.Flags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (IsStencilReadOnly(desc.Usage) && IsStencilFormatDXGI(dsvDesc.Format)) dsvDesc.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        switch (desc.Dim) {
            case TextureDimension::Dim1D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                dsvDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                dsvDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                dsvDesc.Texture1DArray.FirstArraySlice = range.BaseArrayLayer;
                dsvDesc.Texture1DArray.ArraySize = range.ArrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                if (multisampled) {
                    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
                } else {
                    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                    dsvDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                }
                break;
            case TextureDimension::Dim2DArray:
                if (multisampled) {
                    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                    dsvDesc.Texture2DMSArray.FirstArraySlice = range.BaseArrayLayer;
                    dsvDesc.Texture2DMSArray.ArraySize = range.ArrayLayerCount;
                } else {
                    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                    dsvDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                    dsvDesc.Texture2DArray.FirstArraySlice = range.BaseArrayLayer;
                    dsvDesc.Texture2DArray.ArraySize = range.ArrayLayerCount;
                }
                break;
            default:
                RADRAY_ERR_LOG("d3d12 invalid texture view dimension: {}", desc.Dim);
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), dsvDesc, heapView.GetStart());
        dxgiFormat = dsvDesc.Format;
    } else if (desc.Usage == TextureViewUsage::UnorderedAccess) {
        {
            auto heap = _cpuResAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate UAV descriptor");
                return nullptr;
            }
            heapView = {heap, heapViewOpt.value()};
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = MapShaderResourceType(desc.Format);
        switch (desc.Dim) {
            case TextureDimension::Dim1D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uavDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                uavDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture1DArray.FirstArraySlice = range.BaseArrayLayer;
                uavDesc.Texture1DArray.ArraySize = range.ArrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureDimension::Dim2DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture2DArray.FirstArraySlice = range.BaseArrayLayer;
                uavDesc.Texture2DArray.ArraySize = range.ArrayLayerCount;
                uavDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureDimension::Dim3D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavDesc.Texture3D.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture3D.FirstWSlice = 0;
                uavDesc.Texture3D.WSize = static_cast<UINT>(-1);
                break;
            default:
                RADRAY_ERR_LOG("d3d12 invalid texture view dimension: {}", desc.Dim);
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), uavDesc, heapView.GetStart());
        dxgiFormat = uavDesc.Format;
    } else {
        RADRAY_ERR_LOG("d3d12 invalid texture view usage: {}", desc.Usage);
        return nullptr;
    }
    auto result = make_unique<TextureViewD3D12>(this, tex, std::move(heapView));
    result->_desc = desc;
    result->_rawFormat = dxgiFormat;
    return result;
}

// == Device: render pass / framebuffer / shader ==

Nullable<unique_ptr<Shader>> DeviceD3D12::CreateShader(const ShaderDescriptor& desc) noexcept {
    return make_unique<Dxil>(desc.Source.begin(), desc.Source.end(), desc.Stages);
}

// == 根签名映射与构建 ==

void DescriptorDirtyRangesD3D12::Add(uint32_t start, uint32_t count) noexcept {
    RADRAY_ASSERT(count != 0 && count <= std::numeric_limits<uint32_t>::max() - start);
    uint32_t end = start + count;
    if (Ranges.empty() || Ranges.back().Start + Ranges.back().Count < start) {
        Ranges.push_back({start, count});
        return;
    }
    if (start >= Ranges.back().Start) {
        auto& last = Ranges.back();
        last.Count = std::max(last.Start + last.Count, end) - last.Start;
        return;
    }
    auto first = Ranges.begin();
    while (first != Ranges.end() && first->Start + first->Count < start) {
        ++first;
    }
    auto last = first;
    while (last != Ranges.end() && last->Start <= end) {
        start = std::min(start, last->Start);
        end = std::max(end, last->Start + last->Count);
        ++last;
    }
    first = Ranges.erase(first, last);
    Ranges.insert(first, {start, end - start});
}

std::optional<DescriptorDirtyRangeD3D12> DescriptorDirtyRangesD3D12::GetCopySpan(std::span<const uint8_t> initialized) const noexcept {
    if (Ranges.empty()) {
        return std::nullopt;
    }
    const uint64_t end = uint64_t{Ranges.back().Start} + Ranges.back().Count;
    const uint64_t start = Ranges.front().Start;
    if (end > initialized.size()) {
        return std::nullopt;
    }
    uint64_t dirtyCount = 0;
    for (const auto& range : Ranges) {
        dirtyCount += range.Count;
    }
    if (Ranges.size() != 1 && Ranges.size() <= 16 && dirtyCount * 2 < end - start) {
        return std::nullopt;
    }
    if (std::any_of(initialized.begin() + start, initialized.begin() + end, [](uint8_t value) { return value == 0; })) {
        return std::nullopt;
    }
    return DescriptorDirtyRangeD3D12{static_cast<uint32_t>(start), static_cast<uint32_t>(end - start)};
}

static D3D12_DESCRIPTOR_RANGE_TYPE MapBindingRange(BindingRegisterClass kind) noexcept {
    switch (kind) {
        case BindingRegisterClass::CBV: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case BindingRegisterClass::SRV: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        case BindingRegisterClass::UAV: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        case BindingRegisterClass::Sampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    }
    Unreachable();
}
static D3D12_ROOT_PARAMETER_TYPE MapRootType(BindingRegisterClass kind) noexcept {
    switch (kind) {
        case BindingRegisterClass::CBV: return D3D12_ROOT_PARAMETER_TYPE_CBV;
        case BindingRegisterClass::SRV: return D3D12_ROOT_PARAMETER_TYPE_SRV;
        case BindingRegisterClass::UAV: return D3D12_ROOT_PARAMETER_TYPE_UAV;
        default: Unreachable();
    }
}
static std::optional<D3D12_SAMPLER_DESC> MakeSamplerD3D12(const SamplerDescriptor& desc, const DeviceFeatures& features) noexcept {
    if (desc.MipLodBias < D3D12_MIP_LOD_BIAS_MIN || desc.MipLodBias > D3D12_MIP_LOD_BIAS_MAX ||
        desc.UnnormalizedCoordinates || (desc.AnisotropyClamp > 1 && desc.MipmapFilter == FilterMode::Nearest && !features.SamplerAnisotropyPointMip) ||
        (desc.Reduction != SamplerReductionMode::WeightedAverage && !features.SamplerFilterMinmax) ||
        (static_cast<uint32_t>(desc.BorderColor) & 1u) || (desc.Compare && desc.Reduction != SamplerReductionMode::WeightedAverage)) {
        RADRAY_ERR_LOG("unsupported D3D12 sampler configuration");
        return std::nullopt;
    }
    D3D12_SAMPLER_DESC raw{};
    raw.Filter = MapType(desc.MinFilter, desc.MagFilter, desc.MipmapFilter, desc.Compare.has_value(), desc.AnisotropyClamp);
    if (desc.Reduction != SamplerReductionMode::WeightedAverage) {
        const auto reduction = desc.Reduction == SamplerReductionMode::Min ? D3D12_FILTER_REDUCTION_TYPE_MINIMUM : D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
        raw.Filter = static_cast<D3D12_FILTER>((static_cast<uint32_t>(raw.Filter) & ~(3u << D3D12_FILTER_REDUCTION_TYPE_SHIFT)) | (reduction << D3D12_FILTER_REDUCTION_TYPE_SHIFT));
    }
    raw.AddressU = MapType(desc.AddressS);
    raw.AddressV = MapType(desc.AddressT);
    raw.AddressW = MapType(desc.AddressR);
    raw.MipLODBias = desc.MipLodBias;
    raw.MaxAnisotropy = std::max(1u, desc.AnisotropyClamp);
    raw.ComparisonFunc = desc.Compare ? MapType(*desc.Compare) : D3D12_COMPARISON_FUNC_NEVER;
    if (desc.BorderColor == SamplerBorderColor::FloatOpaqueWhite) raw.BorderColor[0] = raw.BorderColor[1] = raw.BorderColor[2] = 1;
    raw.BorderColor[3] = desc.BorderColor == SamplerBorderColor::FloatTransparentBlack ? 0.0f : 1.0f;
    raw.MinLOD = desc.LodMin;
    raw.MaxLOD = desc.LodMax;
    return raw;
}

Nullable<unique_ptr<BindingGroupLayout>> DeviceD3D12::CreateBindingGroupLayout(const BindingGroupLayoutDescriptor& desc) noexcept {
    auto result = make_unique<BindingGroupLayoutD3D12>();
    result->_device = this;
    result->_bindings.assign(desc.Bindings.begin(), desc.Bindings.end());
    std::sort(result->_bindings.begin(), result->_bindings.end(), [](const auto& a, const auto& b) { return a.BindingId < b.BindingId; });
    for (const auto& binding : result->_bindings) {
        if (!binding.D3D12 || !binding.Count) {
            RADRAY_ERR_LOG("D3D12 binding mapping is missing or empty");
            return nullptr;
        }
        const auto& native = *binding.D3D12;
        auto& entry = result->_entries.emplace_back();
        entry.Binding = binding.BindingId;
        entry.LogicalKind = binding.Kind;
        entry.Count = binding.Count;
        entry.Placement = native.Placement;
        entry.Immutable = binding.ImmutableSampler.has_value();
        if (entry.Immutable) continue;
        if (entry.IsRootDescriptor()) {
            if (binding.Count != 1 || !CanBeRootDescriptor(binding.Kind)) return nullptr;
            entry.RootIndex = static_cast<uint32_t>(result->_rootOrder.size());
            result->_rootOrder.push_back(static_cast<uint32_t>(result->_entries.size() - 1));
            continue;
        }
        const auto heap = binding.Kind == BindingKind::Sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        const auto visibility = MapShaderStages(binding.Stages);
        auto table = std::find_if(result->_tables.begin(), result->_tables.end(), [&](const auto& t) { return t.HeapType == heap && t.Visibility == visibility; });
        if (table == result->_tables.end()) {
            result->_tables.emplace_back();
            table = result->_tables.end() - 1;
            table->HeapType = heap;
            table->Visibility = visibility;
        }
        if (binding.Count > std::numeric_limits<uint32_t>::max() - table->DescriptorCount) return nullptr;
        entry.TableIndex = static_cast<uint32_t>(table - result->_tables.begin());
        entry.TableOffset = table->DescriptorCount;
        table->Ranges.push_back({MapBindingRange(native.RegisterType), binding.Count, native.Register, native.Space, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, entry.TableOffset});
        table->DescriptorCount += binding.Count;
    }
    return result;
}

Nullable<unique_ptr<PipelineLayout>> DeviceD3D12::CreatePipelineLayout(const PipelineLayoutDescriptor& desc) noexcept {
    auto result = make_unique<RootSigD3D12>();
    result->_device = this;
    result->_groups.assign(desc.Groups.begin(), desc.Groups.end());
    result->_pushes.assign(desc.PushConstants.begin(), desc.PushConstants.end());
    vector<D3D12_ROOT_PARAMETER1> parameters;
    vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    uint64_t dwords = 0;
    for (const auto& push : desc.PushConstants) {
        if (!push.D3D12 || !push.Size || push.Size % 4) return nullptr;
        result->_pushConstantBindings.push_back({push.PushId, static_cast<uint32_t>(parameters.size())});
        D3D12_ROOT_PARAMETER1 parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {push.D3D12->Register, push.D3D12->Space, push.Size / 4};
        parameter.ShaderVisibility = MapShaderStages(push.Stages);
        parameters.push_back(parameter);
        dwords += push.Size / 4;
    }
    for (const auto& slot : desc.Groups) {
        auto* group = CastD3D12Object(slot.Layout);
        auto& mapping = result->_parameterGroups.emplace_back();
        mapping.GroupSlot = slot.GroupSlot;
        mapping.Layout = group;
        for (const auto index : group->_rootOrder) {
            const auto& binding = group->_bindings[index];
            const auto& native = *binding.D3D12;
            mapping.BufferRoots.push_back(static_cast<uint32_t>(parameters.size()));
            D3D12_ROOT_PARAMETER1 parameter{};
            parameter.ParameterType = MapRootType(native.RegisterType);
            parameter.Descriptor = {native.Register, native.Space, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
            parameter.ShaderVisibility = MapShaderStages(binding.Stages);
            parameters.push_back(parameter);
            dwords += 2;
        }
        for (const auto& table : group->_tables) {
            mapping.TableRoots.push_back(static_cast<uint32_t>(parameters.size()));
            D3D12_ROOT_PARAMETER1 parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable = {static_cast<uint32_t>(table.Ranges.size()), table.Ranges.data()};
            parameter.ShaderVisibility = table.Visibility;
            parameters.push_back(parameter);
            ++dwords;
        }
        for (const auto& binding : group->_bindings) {
            if (!binding.ImmutableSampler) continue;
            const auto raw = MakeSamplerD3D12(*binding.ImmutableSampler, _capabilities.Features);
            if (!raw) return nullptr;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = raw->Filter;
            sampler.AddressU = raw->AddressU;
            sampler.AddressV = raw->AddressV;
            sampler.AddressW = raw->AddressW;
            sampler.MipLODBias = raw->MipLODBias;
            sampler.MaxAnisotropy = raw->MaxAnisotropy;
            sampler.ComparisonFunc = raw->ComparisonFunc;
            sampler.MinLOD = raw->MinLOD;
            sampler.MaxLOD = raw->MaxLOD;
            sampler.BorderColor = binding.ImmutableSampler->BorderColor == SamplerBorderColor::FloatOpaqueWhite ? D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE : binding.ImmutableSampler->BorderColor == SamplerBorderColor::FloatOpaqueBlack ? D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK
                                                                                                                                                                                                                                         : D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            sampler.ShaderRegister = binding.D3D12->Register;
            sampler.RegisterSpace = binding.D3D12->Space;
            sampler.ShaderVisibility = MapShaderStages(binding.Stages);
            samplers.push_back(sampler);
        }
    }
    if (dwords > 64) {
        RADRAY_ERR_LOG("D3D12 root signature exceeds 64 DWORDs");
        return nullptr;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC native{};
    native.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    native.Desc_1_1.NumParameters = static_cast<uint32_t>(parameters.size());
    native.Desc_1_1.pParameters = parameters.data();
    native.Desc_1_1.NumStaticSamplers = static_cast<uint32_t>(samplers.size());
    native.Desc_1_1.pStaticSamplers = samplers.data();
    native.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> blob, errors;
    auto hr = D3DX12SerializeVersionedRootSignature(&native, _features.HighestRootSignatureVersion(), blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        RADRAY_ERR_LOG("root signature serialization failed: {} {}", hr, errors ? std::string_view{static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()} : std::string_view{});
        return nullptr;
    }
    hr = _device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(result->_rootSignature.GetAddressOf()));
    if (FAILED(hr)) {
        RADRAY_ERR_LOG("CreateRootSignature failed: {}", hr);
        return nullptr;
    }
    return result;
}

Nullable<unique_ptr<ShaderParameterSet>> DeviceD3D12::CreateShaderParameterSet(const ShaderParameterSetDescriptor& desc) noexcept {
    auto* layout = CastD3D12Object(desc.Layout);
    auto result = make_unique<ShaderParameterSetD3D12>();
    result->_device = this;
    result->_layout = layout;
    result->_rootBindings.resize(layout->_rootOrder.size());
    result->_tables.reserve(layout->_tables.size());
    for (const auto& table : layout->_tables) {
        auto& storage = result->_tables.emplace_back();
        auto* cpuAllocator = table.HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? _cpuSamplerAlloc.get() : _cpuResAlloc.get();
        auto* gpuAllocator = table.HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? _gpuSamplerHeap.get() : _gpuResHeap.get();
        const auto cpu = cpuAllocator->Allocate(table.DescriptorCount);
        if (!cpu) return nullptr;
        storage.Mirror = CpuDescriptorHeapViewRAII{cpuAllocator, *cpu};
        const auto gpu = gpuAllocator->Allocate(table.DescriptorCount);
        if (!gpu) return nullptr;
        storage.Descriptors = GpuDescriptorHeapViewRAII{gpuAllocator, *gpu};
        storage.InitializedSlots.resize(table.DescriptorCount, 0);
    }
    return result;
}

// == parameter descriptor 写入 ==

struct ResolvedShaderBufferBindingD3D12 {
    BufferD3D12* Buffer;
    uint64_t Offset;
    uint64_t Size;
};

static ResolvedShaderBufferBindingD3D12 ResolveShaderBufferBindingD3D12(
    const ShaderBufferBinding& binding) noexcept {
    auto* buffer = CastD3D12Object(binding.Target);
    const uint64_t size = binding.Range.Size == BufferRange::All()
                              ? buffer->_reqSize - binding.Range.Offset
                              : binding.Range.Size;
    return {buffer, binding.Range.Offset, size};
}

// Only ever called for table bindings: a root descriptor carries no descriptor, it takes a GPU
// virtual address at command time.
static void WriteShaderParameterValueD3D12(
    CpuDescriptorHeapViewRAII& descriptors,
    BindingKind kind,
    uint32_t descriptorIndex,
    const ShaderParameterValue& value) noexcept {
    switch (kind) {
        case BindingKind::CBuffer: {
            const ShaderBufferBinding& binding = std::get<ShaderBufferBinding>(value);
            const auto resolved = ResolveShaderBufferBindingD3D12(binding);
            D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
            desc.BufferLocation = resolved.Buffer->_gpuAddr + resolved.Offset;
            desc.SizeInBytes = static_cast<UINT>(Align(
                resolved.Size,
                uint64_t{D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT}));
            descriptors.GetHeap()->Create(
                desc,
                descriptors.GetStart() + descriptorIndex);
            return;
        }
        case BindingKind::StructuredBuffer:
        case BindingKind::RWStructuredBuffer:
        case BindingKind::RawBuffer:
        case BindingKind::RWRawBuffer: {
            const ShaderBufferBinding& binding = std::get<ShaderBufferBinding>(value);
            const auto resolved = ResolveShaderBufferBindingD3D12(binding);
            const uint64_t elementSize =
                binding.StructureByteStride == 0 ? 4 : binding.StructureByteStride;
            if (!IsWritableBinding(kind)) {
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.Format = binding.StructureByteStride == 0
                                  ? DXGI_FORMAT_R32_TYPELESS
                                  : DXGI_FORMAT_UNKNOWN;
                desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Buffer.FirstElement = resolved.Offset / elementSize;
                desc.Buffer.NumElements = static_cast<UINT>(resolved.Size / elementSize);
                desc.Buffer.StructureByteStride = binding.StructureByteStride;
                desc.Buffer.Flags = binding.StructureByteStride == 0
                                        ? D3D12_BUFFER_SRV_FLAG_RAW
                                        : D3D12_BUFFER_SRV_FLAG_NONE;
                descriptors.GetHeap()->Create(
                    resolved.Buffer->_buf.Get(),
                    desc,
                    descriptors.GetStart() + descriptorIndex);
            } else {
                D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
                desc.Format = binding.StructureByteStride == 0
                                  ? DXGI_FORMAT_R32_TYPELESS
                                  : DXGI_FORMAT_UNKNOWN;
                desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                desc.Buffer.FirstElement = resolved.Offset / elementSize;
                desc.Buffer.NumElements = static_cast<UINT>(resolved.Size / elementSize);
                desc.Buffer.StructureByteStride = binding.StructureByteStride;
                desc.Buffer.CounterOffsetInBytes = 0;
                desc.Buffer.Flags = binding.StructureByteStride == 0
                                        ? D3D12_BUFFER_UAV_FLAG_RAW
                                        : D3D12_BUFFER_UAV_FLAG_NONE;
                descriptors.GetHeap()->Create(
                    resolved.Buffer->_buf.Get(),
                    desc,
                    descriptors.GetStart() + descriptorIndex);
            }
            return;
        }
        case BindingKind::TypedBuffer:
        case BindingKind::RWTypedBuffer: {
            const ShaderTexelBufferBinding& binding =
                std::get<ShaderTexelBufferBinding>(value);
            ShaderBufferBinding bufferBinding{binding.Target, binding.Range, 0};
            const auto resolved = ResolveShaderBufferBindingD3D12(bufferBinding);
            const uint32_t elementSize = GetTextureFormatBytesPerPixel(binding.Format);
            if (!IsWritableBinding(kind)) {
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.Format = MapType(binding.Format);
                desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Buffer.FirstElement = resolved.Offset / elementSize;
                desc.Buffer.NumElements = static_cast<UINT>(resolved.Size / elementSize);
                desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
                descriptors.GetHeap()->Create(
                    resolved.Buffer->_buf.Get(),
                    desc,
                    descriptors.GetStart() + descriptorIndex);
            } else {
                D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
                desc.Format = MapType(binding.Format);
                desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                desc.Buffer.FirstElement = resolved.Offset / elementSize;
                desc.Buffer.NumElements = static_cast<UINT>(resolved.Size / elementSize);
                desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
                descriptors.GetHeap()->Create(
                    resolved.Buffer->_buf.Get(),
                    desc,
                    descriptors.GetStart() + descriptorIndex);
            }
            return;
        }
        case BindingKind::Texture:
        case BindingKind::RWTexture: {
            auto* view = CastD3D12Object(std::get<TextureView*>(value));
            view->_heapView.CopyTo(0, 1, descriptors, descriptorIndex);
            return;
        }
        case BindingKind::Sampler: {
            auto* sampler = CastD3D12Object(std::get<Sampler*>(value));
            sampler->_samplerView.CopyTo(0, 1, descriptors, descriptorIndex);
            return;
        }
    }
}

ShaderParameterSetD3D12::~ShaderParameterSetD3D12() noexcept = default;

bool ShaderParameterSetD3D12::IsValid() const noexcept {
    return _device != nullptr && _layout != nullptr;
}

void ShaderParameterSetD3D12::Destroy() noexcept {
    _tables.clear();
    _copySources.clear();
    _copyDestinations.clear();
    _copyCounts.clear();
    _rootBindings.clear();
    _layout = nullptr;
    _device = nullptr;
}

void ShaderParameterSetD3D12::ResetBindingsForReuse() noexcept {}

bool ShaderParameterSetD3D12::Set(uint32_t bindingId, uint32_t arrayElement, ShaderParameterValue value) noexcept {
    const auto entry = std::find_if(_layout->_entries.begin(), _layout->_entries.end(), [&](const auto& e) { return e.Binding == bindingId; });
    if (entry->IsRootDescriptor()) {
        const auto& buffer = std::get<ShaderBufferBinding>(value);
        _rootBindings[entry->RootIndex] = CastD3D12Object(buffer.Target)->_gpuAddr + buffer.Range.Offset;
    } else {
        auto& storage = _tables[entry->TableIndex];
        const auto offset = entry->TableOffset + arrayElement;
        WriteShaderParameterValueD3D12(storage.Mirror, entry->LogicalKind, offset, value);
        storage.InitializedSlots[offset] = 1;
        storage.Dirty.Add(offset);
    }
    return true;
}

bool ShaderParameterSetD3D12::FlushWrites() noexcept {
    for (auto& storage : _tables) {
        if (storage.Dirty.Ranges.empty()) {
            continue;
        }
        const auto span = storage.Dirty.GetCopySpan(storage.InitializedSlots);
        if (span.has_value()) {
            storage.Mirror.CopyTo(span->Start, span->Count, storage.Descriptors, span->Start);
        } else {
            _copySources.clear();
            _copyDestinations.clear();
            _copyCounts.clear();
            for (const auto& range : storage.Dirty.Ranges) {
                _copySources.push_back(storage.Mirror.GetHeap()->HandleCpu(storage.Mirror.GetStart() + range.Start));
                _copyDestinations.push_back(storage.Descriptors.GetHeap()->HandleCpu(storage.Descriptors.GetStart() + range.Start));
                _copyCounts.push_back(range.Count);
            }
            _device->_device->CopyDescriptors(static_cast<UINT>(_copyCounts.size()), _copyDestinations.data(), _copyCounts.data(),
                                              static_cast<UINT>(_copyCounts.size()), _copySources.data(), _copyCounts.data(), storage.Mirror.GetHeap()->GetHeapType());
        }
        storage.Dirty.Ranges.clear();
    }
    return true;
}

// == Device: PSO / sampler / fence ==

Nullable<unique_ptr<GraphicsPipelineState>> DeviceD3D12::CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept {
    auto [topoClass, topo] = MapType(desc.Primitive.Topology);
    vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    inputElements.reserve(desc.VertexInput.Attributes.size());
    // D3D12_INPUT_ELEMENT_DESC::SemanticName 需要 null 结尾的 string, string_view 不能保证是 \0 结尾
    vector<string> semanticNames;
    semanticNames.reserve(desc.VertexInput.Attributes.size());
    std::array<std::optional<uint32_t>, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexStrides{};
    for (const VertexBufferLayout& buffer : desc.VertexInput.Buffers) {
        vertexStrides[buffer.Binding] = buffer.ArrayStride;
    }
    for (const VertexAttribute& attribute : desc.VertexInput.Attributes) {
        const auto buffer = std::ranges::find(
            desc.VertexInput.Buffers,
            attribute.BufferBinding,
            &VertexBufferLayout::Binding);
        const D3D12_INPUT_CLASSIFICATION inputClass = MapType(buffer->StepMode);
        const string& semanticName = semanticNames.emplace_back(attribute.Semantic);
        auto& inputElement = inputElements.emplace_back(D3D12_INPUT_ELEMENT_DESC{});
        inputElement.SemanticName = semanticName.c_str();
        inputElement.SemanticIndex = attribute.SemanticIndex;
        inputElement.Format = MapType(attribute.Format);
        inputElement.InputSlot = attribute.BufferBinding;
        inputElement.AlignedByteOffset = attribute.Offset;
        inputElement.InputSlotClass = inputClass;
        inputElement.InstanceDataStepRate = inputClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? 1 : 0;
    }
    DepthBiasState depBias;
    if (desc.DepthStencil.has_value()) {
        depBias = desc.DepthStencil.value().DepthBias;
    } else {
        depBias = DepthBiasState{0, 0, 0};
    }
    D3D12_RASTERIZER_DESC rawRaster{};
    rawRaster.FillMode = MapType(desc.Primitive.Poly).value();
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
    rawBlend.IndependentBlendEnable = desc.ColorTargets.size() > 1;
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
            rtb.RenderTargetWriteMask = static_cast<UINT8>(MapColorWrites(ct.WriteMask).value());
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
        const bool hardwareDepthEnable = ds.DepthTestEnable || ds.DepthWriteEnable;
        dsDesc.DepthEnable = hardwareDepthEnable;
        dsDesc.DepthWriteMask = ds.DepthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc = ds.DepthTestEnable ? MapType(ds.DepthCompare) : D3D12_COMPARISON_FUNC_ALWAYS;
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
        dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
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
    rawPsoDesc.pRootSignature = CastD3D12Object(desc.PipelineLayout)->GetNative();
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
    if (HRESULT hr = _device->CreateGraphicsPipelineState(&rawPsoDesc, IID_PPV_ARGS(pso.GetAddressOf())); FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateGraphicsPipelineState failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    auto result = make_unique<GraphicsPsoD3D12>(
        this,
        CastD3D12Object(desc.PipelineLayout),
        std::move(pso),
        std::move(vertexStrides),
        topo);
    return result;
}

Nullable<unique_ptr<ComputePipelineState>> DeviceD3D12::CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept {
    D3D12_COMPUTE_PIPELINE_STATE_DESC rawPsoDesc{};
    rawPsoDesc.pRootSignature = CastD3D12Object(desc.PipelineLayout)->GetNative();
    rawPsoDesc.CS = CastD3D12Object(desc.CS.Target)->ToByteCode();
    rawPsoDesc.NodeMask = 0;
    rawPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ComPtr<ID3D12PipelineState> pso;
    if (HRESULT hr = _device->CreateComputePipelineState(&rawPsoDesc, IID_PPV_ARGS(pso.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateComputePipelineState failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    return make_unique<ComputePsoD3D12>(
        this,
        CastD3D12Object(desc.PipelineLayout),
        std::move(pso));
}

Nullable<unique_ptr<Sampler>> DeviceD3D12::CreateSampler(const SamplerDescriptor& desc) noexcept {
    const auto raw = MakeSamplerD3D12(desc, _capabilities.Features);
    if (!raw) return nullptr;
    const auto& rawDesc = *raw;
    auto alloc = this->_cpuSamplerAlloc.get();
    CpuDescriptorHeapViewRAII heapView{};
    {
        auto opt = alloc->Allocate(1);
        if (!opt.has_value()) {
            RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate Sampler descriptors");
            return nullptr;
        }
        heapView = {alloc, opt.value()};
    }
    heapView.GetHeap()->Create(rawDesc, heapView.GetStart());
    return make_unique<SamplerD3D12>(this, std::move(heapView));
}

Nullable<Sampler*> DeviceD3D12::GetOrCreateSampler(
    const SamplerDescriptor& desc) noexcept {
    return _samplerCache.GetOrCreate(desc);
}

Nullable<unique_ptr<FenceD3D12>> DeviceD3D12::CreateFenceD3D12(uint64_t initValue) noexcept {
    ComPtr<ID3D12Fence> fence;
    if (HRESULT hr = _device->CreateFence(initValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateFence failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    std::optional<Win32Event> e = Win32Event::Create();
    if (!e.has_value()) {
        return nullptr;
    }
    auto result = make_unique<FenceD3D12>(this, std::move(fence), std::move(e.value()));
    result->_fenceValue = initValue + 1;
    return result;
}

// == CmdQueue / Fence / CmdList 与 barrier ==

#ifdef RADRAY_ENABLE_PROFILER
struct CmdListD3D12::ProfilerZoneStack {
    // D3D12ZoneScope is not movable; keep them in a deque so pushes never relocate open zones.
    deque<tracy::D3D12ZoneScope> Zones;
};
#endif

CmdQueueD3D12::CmdQueueD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12CommandQueue> queue,
    D3D12_COMMAND_LIST_TYPE type,
    unique_ptr<FenceD3D12> fence) noexcept
    : _device(device),
      _queue(std::move(queue)),
      _fence(std::move(fence)),
      _type(type) {
#ifdef RADRAY_ENABLE_PROFILER
    if (_type != D3D12_COMMAND_LIST_TYPE_COPY && _queue != nullptr) {
        _profilerContext = TracyD3D12Context(_device->_device.Get(), _queue.Get());
        const auto name = fmt::format("D3D12 {}", GetQueueType());
        TracyD3D12ContextName(static_cast<TracyD3D12Ctx>(_profilerContext), name.c_str(), static_cast<uint16_t>(name.size()));
    }
#endif
}

CmdQueueD3D12::~CmdQueueD3D12() noexcept {
    Destroy();
}

bool CmdQueueD3D12::IsValid() const noexcept {
    return _queue != nullptr;
}

void CmdQueueD3D12::Destroy() noexcept {
#ifdef RADRAY_ENABLE_PROFILER
    if (_profilerContext != nullptr) {
        TracyD3D12Destroy(static_cast<TracyD3D12Ctx>(_profilerContext));
        _profilerContext = nullptr;
    }
#endif
    _fence = nullptr;
    _queue = nullptr;
}

void CmdQueueD3D12::Submit(const CommandQueueSubmitDescriptor& desc) noexcept {
    RADRAY_PROFILE_SCOPE_N("CmdQueueD3D12::Submit");
    _CheckD3D12DeviceStatus(_device, "CommandQueue::Submit");
    for (size_t i = 0; i < desc.WaitFences.size(); ++i) {
        auto* f = CastD3D12Object(desc.WaitFences[i]);
        uint64_t waitValue = desc.WaitValues[i];
        _CheckD3D12Result(_device, _queue->Wait(f->_fence.Get(), waitValue), "ID3D12CommandQueue::Wait");
    }

    vector<ID3D12CommandList*> submits;
    submits.reserve(desc.CmdBuffers.size());
    for (auto& i : desc.CmdBuffers) {
        auto* cmdList = CastD3D12Object(i);
        submits.emplace_back(cmdList->_cmdList.Get());
    }
    if (!submits.empty()) {
        _queue->ExecuteCommandLists(static_cast<UINT>(submits.size()), submits.data());
        _CheckD3D12DeviceStatus(_device, "ID3D12CommandQueue::ExecuteCommandLists");
#ifdef RADRAY_ENABLE_PROFILER
        if (_profilerContext != nullptr) TracyD3D12Collect(static_cast<TracyD3D12Ctx>(_profilerContext));
#endif
    }

    for (size_t i = 0; i < desc.SignalFences.size(); ++i) {
        auto* f = CastD3D12Object(desc.SignalFences[i]);
        uint64_t signalValue = desc.SignalValues[i];
        _CheckD3D12Result(_device, _queue->Signal(f->_fence.Get(), signalValue), "ID3D12CommandQueue::Signal");
        if (signalValue + 1 > f->_fenceValue) {
            f->_fenceValue = signalValue + 1;
        }
    }
}

void CmdQueueD3D12::Wait() noexcept {
    _CheckD3D12Result(_device,
                      _queue->Signal(_fence->_fence.Get(), _fence->_fenceValue), "ID3D12CommandQueue::Signal");
    ++_fence->_fenceValue;
    _fence->Wait();
    if (_device->_diagnostics) detail::FlushDiagnostics(_device->_diagnostics);
}

QueueType CmdQueueD3D12::GetQueueType() const noexcept {
    switch (_type) {
        case D3D12_COMMAND_LIST_TYPE_DIRECT:
            return QueueType::Direct;
        case D3D12_COMMAND_LIST_TYPE_COMPUTE:
            return QueueType::Compute;
        case D3D12_COMMAND_LIST_TYPE_COPY:
            return QueueType::Copy;
        default:
            RADRAY_ABORT("invalid D3D12 command list type: {}", _type);
            return QueueType::Direct;
    }
}

TimestampClockCalibrationResult CmdQueueD3D12::GetTimestampClockCalibration() const noexcept {
    using Status = TimestampClockCalibrationStatus;
    if (_queue == nullptr) return {.Status = Status::InvalidQueue};
    uint64_t frequency{}, gpu{}, cpu{};
    const HRESULT frequencyResult = _queue->GetTimestampFrequency(&frequency);
    if (FAILED(frequencyResult)) {
        return {.Status = Status::ApiFailure, .NativeError = frequencyResult};
    }
    LARGE_INTEGER cpuFrequency{}, before{}, after{};
    if (!QueryPerformanceFrequency(&cpuFrequency) || !QueryPerformanceCounter(&before))
        return {.Status = Status::ApiFailure, .NativeError = static_cast<int64_t>(GetLastError())};
    const HRESULT calibrationResult = _queue->GetClockCalibration(&gpu, &cpu);
    if (FAILED(calibrationResult)) {
        return {.Status = Status::ApiFailure, .NativeError = calibrationResult};
    }
    if (!QueryPerformanceCounter(&after))
        return {.Status = Status::ApiFailure, .NativeError = static_cast<int64_t>(GetLastError())};
    if (frequency == 0 || cpuFrequency.QuadPart <= 0 || before.QuadPart < 0 || after.QuadPart < before.QuadPart)
        return {.Status = Status::InvalidSample};
    return {.Status = Status::Success,
            .CpuDomain = TimestampCpuClockDomain::QueryPerformanceCounter,
            .GpuTick = gpu,
            .GpuFrequencyHz = frequency,
            .CpuTick = cpu,
            .CpuFrequencyHz = static_cast<uint64_t>(cpuFrequency.QuadPart),
            .CpuCallBeginTick = static_cast<uint64_t>(before.QuadPart),
            .CpuCallEndTick = static_cast<uint64_t>(after.QuadPart)};
}

FenceD3D12::FenceD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Fence> fence,
    Win32Event event) noexcept
    : _device(device),
      _fence(std::move(fence)),
      _event(std::move(event)) {}

bool FenceD3D12::IsValid() const noexcept {
    return _fence != nullptr;
}

void FenceD3D12::Destroy() noexcept {
    _externalHandle.Reset();
    _externalInfo.reset();
    _fence = nullptr;
    _event.Destroy();
}

void FenceD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _fence.Get());
}

void FenceD3D12::Wait() noexcept {
    Wait(_fenceValue - 1);
}

void FenceD3D12::Wait(uint64_t value) noexcept {
    if (!WaitFor(value, 30000)) {
        detail::FlushDiagnostics(_device->_diagnostics);
        RADRAY_ABORT("D3D12 fence value {} did not complete within 30 seconds", value);
    }
}

bool FenceD3D12::WaitFor(uint64_t value, uint64_t timeoutMs) noexcept {
    if (GetCompletedValue() >= value) return true;
    if (timeoutMs == 0) return false;
    const DWORD duration = static_cast<DWORD>(std::min<uint64_t>(timeoutMs, INFINITE - 1));
    const uint64_t start = ::GetTickCount64();
    const HRESULT hr = _fence->SetEventOnCompletion(value, _event.Get());
    if (FAILED(hr)) _CheckD3D12FenceResult(this, hr, "ID3D12Fence::SetEventOnCompletion");
    while (GetCompletedValue() < value) {
        const uint64_t elapsed = ::GetTickCount64() - start;
        if (elapsed >= duration) return GetCompletedValue() >= value;
        const DWORD waitResult = ::WaitForSingleObject(_event.Get(), duration - static_cast<DWORD>(elapsed));
        if (waitResult == WAIT_TIMEOUT) return GetCompletedValue() >= value;
        if (waitResult != WAIT_OBJECT_0) {
            const DWORD error = waitResult == WAIT_FAILED ? ::GetLastError() : ERROR_GEN_FAILURE;
            _CheckD3D12FenceResult(this, HRESULT_FROM_WIN32(error), "Fence::WaitForSingleObject");
            RADRAY_ABORT("Fence::WaitForSingleObject returned unexpected status: {}", waitResult);
        }
        // An earlier timed-out registration or device removal can also wake it.
    }
    return true;
}

uint64_t FenceD3D12::GetCompletedValue() const noexcept {
    const uint64_t value = _fence->GetCompletedValue();
    if (value == std::numeric_limits<uint64_t>::max()) {
        _CheckD3D12FenceResult(this, DXGI_ERROR_DEVICE_REMOVED, "ID3D12Fence::GetCompletedValue");
    }
    return value;
}

uint64_t FenceD3D12::GetLastSignaledValue() const noexcept {
    return _fenceValue - 1;
}

CommandAllocatorD3D12::CommandAllocatorD3D12(DeviceD3D12* device, CmdQueueD3D12* queue, ComPtr<ID3D12CommandAllocator> allocator) noexcept
    : _device(device), _queue(queue), _native(std::move(allocator)) {}

CommandAllocatorD3D12::~CommandAllocatorD3D12() noexcept { Destroy(); }

bool CommandAllocatorD3D12::IsValid() const noexcept { return _native != nullptr; }

void CommandAllocatorD3D12::Destroy() noexcept {
    _native.Reset();
    _device = nullptr;
}

void CommandAllocatorD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _native.Get());
}

void CommandAllocatorD3D12::Reset() noexcept {
    RADRAY_PROFILE_SCOPE_N("CommandAllocatorD3D12::Reset");
    _CheckD3D12Result(_device, _native->Reset(), "ID3D12CommandAllocator::Reset");
}

CmdListD3D12::CmdListD3D12(
    DeviceD3D12* device,
    [[maybe_unused]] CmdQueueD3D12* queue,
    CommandAllocatorD3D12* allocator,
    ComPtr<ID3D12GraphicsCommandList> cmdList,
    D3D12_COMMAND_LIST_TYPE type) noexcept
    : _device(device),
#ifdef RADRAY_ENABLE_PROFILER
      _queue(queue),
#endif
      _allocator(allocator),
      _cmdList(std::move(cmdList)),
      _type(type)
#ifdef RADRAY_ENABLE_PROFILER
      ,
      _profilerZones(make_unique<ProfilerZoneStack>())
#endif
{
}

CmdListD3D12::~CmdListD3D12() noexcept {
    Destroy();
}

bool CmdListD3D12::IsValid() const noexcept {
    return _cmdList != nullptr;
}

void CmdListD3D12::Destroy() noexcept {
    if (!_cmdList) return;
#ifdef RADRAY_ENABLE_PROFILER
    // Open zones would record end queries into a list that is being destroyed; drop them.
    if (!_profilerZones->Zones.empty()) {
        RADRAY_WARN_LOG("d3d12 command list destroyed with {} open profiler zones", _profilerZones->Zones.size());
        _profilerZones->Zones.clear();
    }
    _deferredZonePops = 0;
    _suppressedZonePushes = 0;
    _inRenderPass = false;
#endif
    _renderPassList.Reset();
    _cmdList = nullptr;
}

void CmdListD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _cmdList.Get());
}

void CmdListD3D12::Begin() noexcept {
    _CheckD3D12Result(_device, _cmdList->Reset(_allocator->_native.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
    if (_type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        const float blendConstants[4]{};
        _cmdList->OMSetBlendFactor(blendConstants);
    }
    if (_type != D3D12_COMMAND_LIST_TYPE_COPY) {
        ID3D12DescriptorHeap* descriptorHeaps[] = {
            _device->_gpuResHeap->GetNative(),
            _device->_gpuSamplerHeap->GetNative(),
        };
        _cmdList->SetDescriptorHeaps(
            static_cast<UINT>(std::size(descriptorHeaps)),
            descriptorHeaps);
    }
}

void CmdListD3D12::End() noexcept {
#ifdef RADRAY_ENABLE_PROFILER
    // Zone end queries must be recorded before Close(); unbalanced groups are closed here.
    _profilerZones->Zones.clear();
    _deferredZonePops = 0;
    _suppressedZonePushes = 0;
#endif
    const HRESULT hr = _cmdList->Close();
    _CheckD3D12Result(_device, hr, "ID3D12GraphicsCommandList::Close");
}

void CmdListD3D12::ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept {
    vector<D3D12_RESOURCE_BARRIER> rawBarriers;
    rawBarriers.reserve(barriers.size());
    for (const auto& v : barriers) {
        if (const auto* bb = std::get_if<BarrierBufferDescriptor>(&v)) {
            auto buf = CastD3D12Object(bb->Target);
            if (buf->_memory != MemoryType::Device && !buf->_usage.HasFlag(BufferUse::UnorderedAccess)) {
                continue;
            }
            D3D12_RESOURCE_BARRIER raw{};
            if (bb->Before == BufferState::UnorderedAccess && bb->After == BufferState::UnorderedAccess) {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.UAV.pResource = buf->_buf.Get();
            } else {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.Transition.pResource = buf->_buf.Get();
                raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                raw.Transition.StateBefore = ResolveShaderResourceStatesD3D12(MapType(bb->Before), bb->BeforeStages, _type);
                raw.Transition.StateAfter = ResolveShaderResourceStatesD3D12(MapType(bb->After), bb->AfterStages, _type);
                if (bb->Before == BufferState::HostRead) raw.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                if (bb->After == BufferState::HostRead) raw.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                if (raw.Transition.StateBefore == raw.Transition.StateAfter) {
                    continue;
                }
            }
            rawBarriers.push_back(raw);
        } else if (const auto* tb = std::get_if<BarrierTextureDescriptor>(&v)) {
            auto tex = CastD3D12Object(tb->Target);
            const auto textureDesc = tex->GetDesc();
            const auto range = ResolveSubresourceRange(textureDesc, tb->Range);
            D3D12_RESOURCE_BARRIER raw{};
            if (tb->Before == TextureState::UnorderedAccess && tb->After == TextureState::UnorderedAccess) {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.UAV.pResource = tex->_tex.Get();
            } else {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                raw.Transition.pResource = tex->_tex.Get();
                const uint32_t layers = tex->_dimension == TextureDimension::Dim3D ? 1 : tex->_rawDesc.DepthOrArraySize;
                const uint32_t planes = D3D12GetFormatPlaneCount(_device->_device.Get(), tex->_rawDesc.Format);
                for (uint32_t plane = 0; plane < planes; ++plane) {
                    const auto aspect = IsDepthStencilFormat(textureDesc.Format) ? (plane == 0 ? TextureAspect::Depth : TextureAspect::Stencil) : TextureAspect::Color;
                    if (!range.Aspects.HasFlag(aspect)) continue;
                    raw.Transition.StateBefore = ResolveShaderResourceStatesD3D12(MapType(TextureStateForAspect(tb->Before, aspect)), tb->BeforeStages, _type);
                    raw.Transition.StateAfter = ResolveShaderResourceStatesD3D12(MapType(TextureStateForAspect(tb->After, aspect)), tb->AfterStages, _type);
                    if (raw.Transition.StateBefore == raw.Transition.StateAfter) continue;
                    for (uint32_t layer = range.BaseArrayLayer; layer < range.BaseArrayLayer + range.ArrayLayerCount; ++layer) {
                        for (uint32_t mip = range.BaseMipLevel; mip < range.BaseMipLevel + range.MipLevelCount; ++mip) {
                            raw.Transition.Subresource = D3D12CalcSubresource(mip, layer, plane, tex->_rawDesc.MipLevels, layers);
                            rawBarriers.push_back(raw);
                        }
                    }
                }
                continue;
            }
            rawBarriers.push_back(raw);
        } else if (const auto* uav = std::get_if<BarrierUavDescriptor>(&v)) {
            D3D12_RESOURCE_BARRIER raw{};
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            if (uav->Target->GetTag() == RenderObjectTag::Buffer) {
                raw.UAV.pResource = CastD3D12Object(static_cast<Buffer*>(uav->Target))->_buf.Get();
            } else if (uav->Target->GetTag() == RenderObjectTag::Texture) {
                raw.UAV.pResource = CastD3D12Object(static_cast<Texture*>(uav->Target))->_tex.Get();
            } else {
                RADRAY_ABORT("D3D12 UAV barrier requires a buffer or texture");
            }
            rawBarriers.push_back(raw);
        }
    }
    if (!rawBarriers.empty()) {
        _cmdList->ResourceBarrier(static_cast<UINT>(rawBarriers.size()), rawBarriers.data());
    }
}

void CmdListD3D12::PushDebugGroup(std::string_view name) noexcept {
    const auto wide = ToWideChar(name).value_or(L"RenderGraph");
    _cmdList->BeginEvent(0, wide.c_str(), static_cast<UINT>((wide.size() + 1) * sizeof(wchar_t)));
#ifdef RADRAY_ENABLE_PROFILER
    if (_inRenderPass) {
        // Query resolution is illegal inside a render pass; this pass joins the zone opened by its raster group.
        ++_suppressedZonePushes;
        return;
    }
    if (_queue->_profilerContext != nullptr) {
        _profilerZones->Zones.emplace_back(
            static_cast<TracyD3D12Ctx>(_queue->_profilerContext), TracyLine, TracyFile, strlen(TracyFile), TracyFunction, strlen(TracyFunction),
            name.data(), name.size(), _cmdList.Get(), true);
    }
#endif
}

void CmdListD3D12::PopDebugGroup() noexcept {
    _cmdList->EndEvent();
#ifdef RADRAY_ENABLE_PROFILER
    if (_suppressedZonePushes > 0) {
        --_suppressedZonePushes;
        return;
    }
    if (_inRenderPass) {
        ++_deferredZonePops;
        return;
    }
    if (!_profilerZones->Zones.empty()) _profilerZones->Zones.pop_back();
#endif
}

Nullable<unique_ptr<GraphicsCommandEncoder>> CmdListD3D12::BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept {
    const bool hasResolve = std::any_of(desc.Colors.begin(), desc.Colors.end(), [](const auto& color) { return bool(color.ResolveTarget); }) ||
                            (desc.DepthStencil && desc.DepthStencil->Resolve);
    if (hasResolve && !_device->_features.RenderPassesValid()) {
        RADRAY_ERR_LOG("D3D12 attachment resolve requires a runtime with valid render pass support");
        return nullptr;
    }
    if (!_renderPassList) {
        const HRESULT result = _cmdList->QueryInterface(IID_PPV_ARGS(_renderPassList.GetAddressOf()));
        if (FAILED(result)) {
            RADRAY_ERR_LOG("D3D12 render pass interface failed: {}", result);
            return nullptr;
        }
    }
    auto encoder = make_unique<CmdRenderPassD3D12>(this);
    // BeginRenderPass snapshots attachment descriptors. Limit clear/discard to the pass layers.
    vector<unique_ptr<TextureView>> layerViews;
    const auto attachmentView = [&](TextureView* view) -> Nullable<TextureViewD3D12*> {
        auto vd = view->GetDesc();
        const auto range = ResolveSubresourceRange(vd.Target->GetDesc(), vd.Range);
        if (range.ArrayLayerCount == desc.Layers) return CastD3D12Object(view);
        vd.Range.ArrayLayerCount = desc.Layers;
        auto subset = _device->CreateTextureView(vd);
        if (!subset) return nullptr;
        layerViews.push_back(subset.Release());
        return CastD3D12Object(layerViews.back().get());
    };
    const auto renderArea = desc.RenderArea.value_or(Rect{0, 0, desc.Width, desc.Height});
    const auto resolve = [&](TextureView* source, TextureView* target, ResolveMode mode, uint32_t plane, StoreAction store, D3D12_RENDER_PASS_ENDING_ACCESS& access) {
        auto* src = CastD3D12Object(source);
        auto* dst = CastD3D12Object(target);
        auto* srcTexture = CastD3D12Object(src->_desc.Target);
        auto* dstTexture = CastD3D12Object(dst->_desc.Target);
        const auto sr = ResolveSubresourceRange(srcTexture->GetDesc(), src->_desc.Range);
        const auto dr = ResolveSubresourceRange(dstTexture->GetDesc(), dst->_desc.Range);
        auto& regions = encoder->_resolveSubresources.emplace_back();
        for (uint32_t layer = 0; layer < desc.Layers; ++layer) {
            regions.push_back({D3D12CalcSubresource(sr.BaseMipLevel, sr.BaseArrayLayer + layer, plane, srcTexture->_rawDesc.MipLevels, srcTexture->_rawDesc.DepthOrArraySize),
                               D3D12CalcSubresource(dr.BaseMipLevel, dr.BaseArrayLayer + layer, plane, dstTexture->_rawDesc.MipLevels, dstTexture->_rawDesc.DepthOrArraySize),
                               static_cast<UINT>(renderArea.X),
                               static_cast<UINT>(renderArea.Y),
                               {renderArea.X, renderArea.Y, renderArea.X + static_cast<LONG>(renderArea.Width), renderArea.Y + static_cast<LONG>(renderArea.Height)}});
        }
        const bool fullSource = renderArea.X == 0 && renderArea.Y == 0 && renderArea.Width == std::max(1u, srcTexture->GetDesc().Width >> sr.BaseMipLevel) && renderArea.Height == std::max(1u, srcTexture->GetDesc().Height >> sr.BaseMipLevel);
        access.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
        access.Resolve = {srcTexture->_tex.Get(), dstTexture->_tex.Get(), static_cast<UINT>(regions.size()), regions.data(), _TexturePlaneFormat(src->_desc, plane),
                          mode == ResolveMode::Min ? D3D12_RESOLVE_MODE_MIN : mode == ResolveMode::Max ? D3D12_RESOLVE_MODE_MAX
                                                                                                       : D3D12_RESOLVE_MODE_AVERAGE,
                          store == StoreAction::Store || !fullSource};
    };
    vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> colors;
    std::optional<D3D12_RENDER_PASS_DEPTH_STENCIL_DESC> depth;
    std::optional<D3D12_RECT> area;
    vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> rectClears;
    D3D12_CLEAR_FLAGS depthClearFlags{};
    D3D12_CLEAR_VALUE depthClear{};
    const auto partial = [&](TextureView* view) {
        const auto vd = view->GetDesc();
        const auto td = vd.Target->GetDesc();
        const auto range = ResolveSubresourceRange(td, vd.Range);
        return renderArea.X || renderArea.Y || renderArea.Width != std::max(1u, td.Width >> range.BaseMipLevel) ||
               renderArea.Height != std::max(1u, td.Height >> range.BaseMipLevel);
    };
    if (std::any_of(desc.Colors.begin(), desc.Colors.end(), [&](const auto& color) { return partial(color.View); }) ||
        (desc.DepthStencil && partial(desc.DepthStencil->View)))
        area = {renderArea.X, renderArea.Y, renderArea.X + static_cast<LONG>(renderArea.Width), renderArea.Y + static_cast<LONG>(renderArea.Height)};
    auto flags = desc.AllowUavWrites ? D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES : D3D12_RENDER_PASS_FLAG_NONE;
    for (const auto& color : desc.Colors) {
        const auto selected = attachmentView(color.View);
        if (!selected) return nullptr;
        auto* view = selected.Get();
        D3D12_RENDER_PASS_RENDER_TARGET_DESC rt{};
        rt.cpuDescriptor = view->_heapView.HandleCpu();
        rt.BeginningAccess.Type = MapType(color.Load);
        rt.EndingAccess.Type = MapType(color.Store);
        rt.BeginningAccess.Clear.ClearValue.Format = view->_rawFormat;
        for (uint32_t c = 0; c < 4; ++c) rt.BeginningAccess.Clear.ClearValue.Color[c] = color.Clear.Value[c];
        if (area && color.Load == LoadAction::Clear) {
            rectClears.push_back(rt);
            rt.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
        }
        if (area) {
            rt.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        }
        if (color.ResolveTarget) resolve(color.View, color.ResolveTarget.Get(), ResolveMode::Average, 0, color.Store, rt.EndingAccess);
        colors.push_back(rt);
    }
    if (desc.DepthStencil) {
        const auto& d = *desc.DepthStencil;
        const auto selected = attachmentView(d.View);
        if (!selected) return nullptr;
        auto* view = selected.Get();
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds{};
        ds.cpuDescriptor = view->_heapView.HandleCpu();
        ds.DepthBeginningAccess.Type = MapType(d.DepthLoad);
        ds.DepthEndingAccess.Type = MapType(d.DepthStore);
        ds.StencilBeginningAccess.Type = IsStencilFormatDXGI(view->_rawFormat) ? MapType(d.StencilLoad) : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
        ds.StencilEndingAccess.Type = IsStencilFormatDXGI(view->_rawFormat) ? MapType(d.StencilStore) : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = view->_rawFormat;
        clear.DepthStencil = {d.Clear.Depth, static_cast<UINT8>(d.Clear.Stencil)};
        ds.DepthBeginningAccess.Clear.ClearValue = clear;
        ds.StencilBeginningAccess.Clear.ClearValue = clear;
        if (IsDepthReadOnly(view->_desc.Usage)) flags |= D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_DEPTH;
        if (IsStencilReadOnly(view->_desc.Usage) && IsStencilFormatDXGI(view->_rawFormat)) flags |= D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_STENCIL;
        if (area) {
            depthClear = clear;
            if (d.DepthLoad == LoadAction::Clear) {
                depthClearFlags |= D3D12_CLEAR_FLAG_DEPTH;
                ds.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            }
            if (IsStencilFormatDXGI(view->_rawFormat) && d.StencilLoad == LoadAction::Clear) {
                depthClearFlags |= D3D12_CLEAR_FLAG_STENCIL;
                ds.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            }
        }
        if (area) {
            ds.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            ds.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            if (IsStencilFormatDXGI(view->_rawFormat)) {
                ds.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                ds.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            }
        }
        if (d.Resolve) {
            if (d.Resolve->DepthMode != ResolveMode::None) resolve(d.View, d.Resolve->Target, d.Resolve->DepthMode, 0, d.DepthStore, ds.DepthEndingAccess);
            if (d.Resolve->StencilMode != ResolveMode::None) resolve(d.View, d.Resolve->Target, d.Resolve->StencilMode, 1, d.StencilStore, ds.StencilEndingAccess);
        }
        depth = ds;
    }
    if (area) {
        for (const auto& clear : rectClears) _cmdList->ClearRenderTargetView(clear.cpuDescriptor, clear.BeginningAccess.Clear.ClearValue.Color, 1, &*area);
        if (depthClearFlags) _cmdList->ClearDepthStencilView(depth->cpuDescriptor, depthClearFlags,
                                                             depthClear.DepthStencil.Depth, depthClear.DepthStencil.Stencil, 1, &*area);
    }
    _renderPassList->BeginRenderPass(static_cast<UINT>(colors.size()), colors.data(), depth ? &*depth : nullptr, flags);
#ifdef RADRAY_ENABLE_PROFILER
    _inRenderPass = true;
#endif
    return encoder;
}

void CmdListD3D12::EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept {
    _renderPassList->EndRenderPass();
#ifdef RADRAY_ENABLE_PROFILER
    _inRenderPass = false;
#endif
    encoder.reset();
#ifdef RADRAY_ENABLE_PROFILER
    // Pops deferred while the render pass was recording close their zones now that query resolution is legal.
    for (; _deferredZonePops > 0; --_deferredZonePops)
        if (!_profilerZones->Zones.empty()) _profilerZones->Zones.pop_back();
#endif
}

Nullable<unique_ptr<ComputeCommandEncoder>> CmdListD3D12::BeginComputePass() noexcept {
    auto encoder = make_unique<CmdComputePassD3D12>(this);
    return encoder;
}

void CmdListD3D12::EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept {
    encoder.reset();
}

// == CmdList: 拷贝与 query ==

void CmdListD3D12::CopyBufferToBuffer(Buffer* dst_, uint64_t dstOffset, Buffer* src_, uint64_t srcOffset, uint64_t size) noexcept {
    auto src = CastD3D12Object(src_);
    auto dst = CastD3D12Object(dst_);
    _cmdList->CopyBufferRegion(dst->_buf.Get(), dstOffset, src->_buf.Get(), srcOffset, size);
}

bool CmdListD3D12::CopyBufferToTextureRegion(const BufferToTextureCopyDescriptor& desc) noexcept {
    auto src = CastD3D12Object(desc.Source);
    auto dst = CastD3D12Object(desc.Destination);
    const auto& r = desc.Region;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = src->_buf.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = r.SourceOffset;
    from.PlacedFootprint.Footprint = {dst->_rawDesc.Format, r.Width, r.Height, 1, r.RowPitch};
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = dst->_tex.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = D3D12CalcSubresource(r.MipLevel, r.ArrayLayer, 0, dst->_rawDesc.MipLevels, dst->_rawDesc.DepthOrArraySize);
    _cmdList->CopyTextureRegion(&to, r.X, r.Y, 0, &from, nullptr);
    return true;
}

void CmdListD3D12::CopyBufferToTexture(Texture* dst_, SubresourceRange dstRange, Buffer* src_, uint64_t srcOffset) noexcept {
    auto src = CastD3D12Object(src_);
    auto dst = CastD3D12Object(dst_);
    const D3D12_RESOURCE_DESC& dstDesc = dst->_rawDesc;
    dstRange = ResolveSubresourceRange(dst->GetDesc(), dstRange);
    const uint32_t plane = dstRange.Aspects == TextureAspect::Stencil ? 1 : 0;

    uint32_t mipLevels = dstRange.MipLevelCount;
    uint32_t layerCount = dstRange.ArrayLayerCount;
    uint32_t arraySize = dst->_dimension == TextureDimension::Dim3D ? 1u : dst->_rawDesc.DepthOrArraySize;

    uint64_t bufferOffset = srcOffset;
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        uint32_t mipLevel = dstRange.BaseMipLevel + mip;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            uint32_t arrayLayer = dstRange.BaseArrayLayer + layer;
            UINT subres = D3D12CalcSubresource(mipLevel, arrayLayer, plane, dst->_rawDesc.MipLevels, arraySize);

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 totalBytes = 0;
            _device->_device->GetCopyableFootprints(&dstDesc, subres, 1, bufferOffset, &footprint, nullptr, nullptr, &totalBytes);

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.pResource = src->_buf.Get();
            srcLoc.PlacedFootprint = footprint;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.pResource = dst->_tex.Get();
            dstLoc.SubresourceIndex = subres;

            _cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
            bufferOffset = Align(footprint.Offset + totalBytes, uint64_t{D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT});
        }
    }
}

void CmdListD3D12::CopyTextureToBuffer(Buffer* dst_, uint64_t dstOffset, Texture* src_, SubresourceRange srcRange) noexcept {
    auto dst = CastD3D12Object(dst_);
    auto src = CastD3D12Object(src_);
    const D3D12_RESOURCE_DESC& srcDesc = src->_rawDesc;
    srcRange = ResolveSubresourceRange(src->GetDesc(), srcRange);
    const uint32_t plane = srcRange.Aspects == TextureAspect::Stencil ? 1 : 0;

    uint32_t mipLevels = srcRange.MipLevelCount;
    uint32_t layerCount = srcRange.ArrayLayerCount;
    uint32_t arraySize = src->_dimension == TextureDimension::Dim3D ? 1u : src->_rawDesc.DepthOrArraySize;

    uint64_t bufferOffset = dstOffset;
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        uint32_t mipLevel = srcRange.BaseMipLevel + mip;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            uint32_t arrayLayer = srcRange.BaseArrayLayer + layer;
            UINT subres = D3D12CalcSubresource(mipLevel, arrayLayer, plane, src->_rawDesc.MipLevels, arraySize);

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 totalBytes = 0;
            _device->_device->GetCopyableFootprints(&srcDesc, subres, 1, bufferOffset, &footprint, nullptr, nullptr, &totalBytes);

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.pResource = src->_tex.Get();
            srcLoc.SubresourceIndex = subres;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dstLoc.pResource = dst->_buf.Get();
            dstLoc.PlacedFootprint = footprint;

            _cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
            bufferOffset = Align(footprint.Offset + totalBytes, uint64_t{D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT});
        }
    }
}

void CmdListD3D12::CopyTextureToTexture(const TextureCopyDescriptor& desc) noexcept {
    auto* src = CastD3D12Object(desc.Source);
    auto* dst = CastD3D12Object(desc.Destination);
    const bool is3D = src->_rawDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const uint32_t srcArraySize = is3D ? 1u : src->_rawDesc.DepthOrArraySize;
    const uint32_t dstArraySize = is3D ? 1u : dst->_rawDesc.DepthOrArraySize;
    const bool multisampled = src->_rawDesc.SampleDesc.Count > 1;

    D3D12_BOX sourceBox{
        desc.SourceX,
        desc.SourceY,
        desc.SourceZ,
        desc.SourceX + desc.Width,
        desc.SourceY + desc.Height,
        desc.SourceZ + desc.Depth};
    for (uint32_t layer = 0; layer < desc.ArrayLayerCount; ++layer) {
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = src->_tex.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sourceLocation.SubresourceIndex = D3D12CalcSubresource(
            desc.SourceMipLevel,
            desc.SourceArrayLayer + layer,
            0,
            src->_rawDesc.MipLevels,
            srcArraySize);

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = dst->_tex.Get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = D3D12CalcSubresource(
            desc.DestinationMipLevel,
            desc.DestinationArrayLayer + layer,
            0,
            dst->_rawDesc.MipLevels,
            dstArraySize);
        _cmdList->CopyTextureRegion(
            &destinationLocation,
            desc.DestinationX,
            desc.DestinationY,
            desc.DestinationZ,
            &sourceLocation,
            multisampled ? nullptr : &sourceBox);
    }
}

void CmdListD3D12::ResolveTexture(const TextureResolveDescriptor& desc) noexcept {
    auto* src = CastD3D12Object(desc.Source);
    auto* dst = CastD3D12Object(desc.Destination);
    const uint32_t srcArraySize = src->_rawDesc.DepthOrArraySize;
    const uint32_t dstArraySize = dst->_rawDesc.DepthOrArraySize;

    const DXGI_FORMAT format = MapType(src->_format);
    for (uint32_t layer = 0; layer < desc.ArrayLayerCount; ++layer) {
        const uint32_t sourceSubresource = D3D12CalcSubresource(
            desc.SourceMipLevel,
            desc.SourceArrayLayer + layer,
            0,
            src->_rawDesc.MipLevels,
            srcArraySize);
        const uint32_t destinationSubresource = D3D12CalcSubresource(
            desc.DestinationMipLevel,
            desc.DestinationArrayLayer + layer,
            0,
            dst->_rawDesc.MipLevels,
            dstArraySize);
        _cmdList->ResolveSubresource(
            dst->_tex.Get(), destinationSubresource, src->_tex.Get(), sourceSubresource, format);
    }
}

void CmdListD3D12::ResetQueryPool(QueryPool*, uint32_t, uint32_t) noexcept {}

void CmdListD3D12::WriteTimestamp(const QueryTimestampDescriptor& desc) noexcept {
    auto pool = CastD3D12Object(desc.Pool);
    _cmdList->EndQuery(pool->_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, desc.Index);
}

void CmdListD3D12::ResolveQueryData(const QueryResolveDescriptor& desc) noexcept {
    auto pool = CastD3D12Object(desc.Pool);
    auto dst = CastD3D12Object(desc.Destination);
    _cmdList->ResolveQueryData(
        pool->_heap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        desc.FirstIndex,
        desc.Count,
        dst->_buf.Get(),
        desc.DestinationOffset);
}

// == 编码器与 SwapChain ==

CmdRenderPassD3D12::CmdRenderPassD3D12(CmdListD3D12* cmdList) noexcept
    : _cmdList(cmdList) {}

bool CmdRenderPassD3D12::IsValid() const noexcept {
    return _cmdList != nullptr;
}

void CmdRenderPassD3D12::Destroy() noexcept {
    _boundRs = nullptr;
    _boundPso = nullptr;
    _cmdList = nullptr;
}

CommandBuffer* CmdRenderPassD3D12::GetCommandBuffer() const noexcept {
    return _cmdList;
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

void CmdRenderPassD3D12::BindVertexBuffers(std::span<const VertexBufferBinding> bindings) noexcept {
    if (bindings.empty()) return;
    uint32_t lowest = std::numeric_limits<uint32_t>::max(), highest = 0;
    for (const auto& binding : bindings) {
        lowest = std::min(lowest, binding.Binding);
        highest = std::max(highest, binding.Binding);
    }
    const uint32_t slotCount = highest - lowest + 1;
    std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> rawViews{};
    for (const VertexBufferBinding& binding : bindings) {
        const std::optional<uint32_t>& stride = _boundPso->_vertexStrides[binding.Binding];
        if (!stride.has_value()) {
            continue;
        }
        const VertexBufferView& view = binding.View;
        const BufferD3D12* buffer = CastD3D12Object(view.Target);
        rawViews[binding.Binding - lowest] = D3D12_VERTEX_BUFFER_VIEW{
            .BufferLocation = buffer->_gpuAddr + view.Offset,
            .SizeInBytes = static_cast<UINT>(view.Size),
            .StrideInBytes = stride.value()};
    }
    _cmdList->_cmdList->IASetVertexBuffers(lowest, slotCount, rawViews.data());
}

void CmdRenderPassD3D12::BindIndexBuffer(IndexBufferView ibv) noexcept {
    auto buf = CastD3D12Object(ibv.Target);
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = buf->_gpuAddr + ibv.Offset;
    view.SizeInBytes = (UINT)buf->_rawDesc.Width - ibv.Offset;
    view.Format = ibv.Stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    _cmdList->_cmdList->IASetIndexBuffer(&view);
}

void CmdRenderPassD3D12::BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept {
    auto ps = CastD3D12Object(pso);
    if (_boundPso == ps) {
        return;
    }
    if (_boundRs != ps->_layout) {
        _cmdList->_cmdList->SetGraphicsRootSignature(ps->_layout->GetNative());
        _boundRs = ps->_layout;
    }
    _cmdList->_cmdList->SetPipelineState(ps->_pso.Get());
    _cmdList->_cmdList->IASetPrimitiveTopology(ps->_topo);
    _boundPso = ps;
}

static bool BindShaderParameterSetD3D12(
    ID3D12GraphicsCommandList* cmdList,
    RootSigD3D12* destinationLayout,
    uint32_t groupIndex,
    ShaderParameterSetD3D12* set,
    std::span<const ShaderParameterDynamicOffset> dynamicOffsets,
    bool graphics) noexcept {
    const auto destinationGroup = destinationLayout->FindParameterGroup(groupIndex);
    const auto& destination = *destinationGroup.Get();
    for (size_t i = 0; i < destination.TableRoots.size(); ++i) {
        const auto handle = set->_tables[i].Descriptors.HandleGpu();
        if (graphics)
            cmdList->SetGraphicsRootDescriptorTable(destination.TableRoots[i], handle);
        else
            cmdList->SetComputeRootDescriptorTable(destination.TableRoots[i], handle);
    }
    for (const auto& offset : dynamicOffsets) {
        const auto& entries = set->_layout->_entries;
        const auto source = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.Binding == offset.BindingId; });
        const auto& targetEntries = destination.Layout->_entries;
        const auto target = std::find_if(targetEntries.begin(), targetEntries.end(), [&](const auto& e) { return e.Binding == offset.BindingId; });
        const auto address = set->_rootBindings[source->RootIndex] + offset.Offset;
        const auto root = destination.BufferRoots[target->RootIndex];
        switch (GetBindingRegisterClass(target->LogicalKind)) {
            case BindingRegisterClass::CBV:
                if (graphics)
                    cmdList->SetGraphicsRootConstantBufferView(root, address);
                else
                    cmdList->SetComputeRootConstantBufferView(root, address);
                break;
            case BindingRegisterClass::SRV:
                if (graphics)
                    cmdList->SetGraphicsRootShaderResourceView(root, address);
                else
                    cmdList->SetComputeRootShaderResourceView(root, address);
                break;
            case BindingRegisterClass::UAV:
                if (graphics)
                    cmdList->SetGraphicsRootUnorderedAccessView(root, address);
                else
                    cmdList->SetComputeRootUnorderedAccessView(root, address);
                break;
            default: Unreachable();
        }
    }
    return true;
}

void CmdRenderPassD3D12::BindShaderParameterSet(
    uint32_t groupIndex,
    ShaderParameterSet* set,
    std::span<const ShaderParameterDynamicOffset> dynamicOffsets) noexcept {
    BindShaderParameterSetD3D12(
        _cmdList->_cmdList.Get(),
        _boundRs,
        groupIndex,
        CastD3D12Object(set),
        dynamicOffsets,
        true);
}

static bool SetPushConstantsD3D12(CmdListD3D12* commandBuffer, RootSigD3D12* boundLayout,
                                  uint32_t pushId, uint32_t byteOffset, std::span<const byte> data, bool graphics) noexcept {
    const auto push = std::find_if(boundLayout->_pushConstantBindings.begin(), boundLayout->_pushConstantBindings.end(), [&](const auto& p) { return p.PushId == pushId; });
    if (graphics)
        commandBuffer->_cmdList->SetGraphicsRoot32BitConstants(push->RootParameterIndex, static_cast<uint32_t>(data.size() / 4), data.data(), byteOffset / 4);
    else
        commandBuffer->_cmdList->SetComputeRoot32BitConstants(push->RootParameterIndex, static_cast<uint32_t>(data.size() / 4), data.data(), byteOffset / 4);
    return true;
}

bool CmdRenderPassD3D12::SetPushConstants(
    uint32_t pushId, uint32_t byteOffset,
    std::span<const byte> data) noexcept {
    return SetPushConstantsD3D12(
        _cmdList,
        _boundRs,
        pushId, byteOffset,
        data,
        true);
}

void CmdRenderPassD3D12::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
    _cmdList->_cmdList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void CmdRenderPassD3D12::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
    _cmdList->_cmdList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void CmdRenderPassD3D12::DrawIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount) noexcept {
    auto* buffer = CastD3D12Object(argumentBuffer);
    _cmdList->_cmdList->ExecuteIndirect(
        _cmdList->_device->_drawIndirectSignature.Get(),
        drawCount,
        buffer->_buf.Get(),
        argumentOffset,
        nullptr,
        0);
}

void CmdRenderPassD3D12::DrawIndexedIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount) noexcept {
    auto* buffer = CastD3D12Object(argumentBuffer);
    _cmdList->_cmdList->ExecuteIndirect(
        _cmdList->_device->_drawIndexedIndirectSignature.Get(),
        drawCount,
        buffer->_buf.Get(),
        argumentOffset,
        nullptr,
        0);
}

CmdComputePassD3D12::CmdComputePassD3D12(CmdListD3D12* cmdList) noexcept
    : _cmdList(cmdList) {}

bool CmdComputePassD3D12::IsValid() const noexcept {
    return _cmdList != nullptr;
}

void CmdComputePassD3D12::Destroy() noexcept {
    _boundRs = nullptr;
    _boundPso = nullptr;
    _cmdList = nullptr;
}

CommandBuffer* CmdComputePassD3D12::GetCommandBuffer() const noexcept {
    return _cmdList;
}

void CmdComputePassD3D12::BindComputePipelineState(ComputePipelineState* pso) noexcept {
    auto ps = CastD3D12Object(pso);
    if (_boundPso == ps) {
        return;
    }
    if (_boundRs != ps->_layout) {
        _cmdList->_cmdList->SetComputeRootSignature(ps->_layout->GetNative());
        _boundRs = ps->_layout;
    }
    _cmdList->_cmdList->SetPipelineState(ps->_pso.Get());
    _boundPso = ps;
}

void CmdComputePassD3D12::BindShaderParameterSet(
    uint32_t groupIndex,
    ShaderParameterSet* set,
    std::span<const ShaderParameterDynamicOffset> dynamicOffsets) noexcept {
    BindShaderParameterSetD3D12(
        _cmdList->_cmdList.Get(),
        _boundRs,
        groupIndex,
        CastD3D12Object(set),
        dynamicOffsets,
        false);
}

bool CmdComputePassD3D12::SetPushConstants(
    uint32_t pushId, uint32_t byteOffset,
    std::span<const byte> data) noexcept {
    return SetPushConstantsD3D12(
        _cmdList,
        _boundRs,
        pushId, byteOffset,
        data,
        false);
}

void CmdComputePassD3D12::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
    _cmdList->_cmdList->Dispatch(groupCountX, groupCountY, groupCountZ);
}

void CmdComputePassD3D12::DispatchIndirect(Buffer* argumentBuffer, uint64_t argumentOffset) noexcept {
    auto* buffer = CastD3D12Object(argumentBuffer);
    _cmdList->_cmdList->ExecuteIndirect(
        _cmdList->_device->_dispatchIndirectSignature.Get(),
        1,
        buffer->_buf.Get(),
        argumentOffset,
        nullptr,
        0);
}

SwapChainD3D12::SwapChainD3D12(
    DeviceD3D12* device,
    ComPtr<IDXGISwapChain3> swapchain,
    const SwapChainDescriptor& desc) noexcept
    : _device(device),
      _presentQueue(CastD3D12Object(desc.PresentQueue)),
      _swapchain(std::move(swapchain)),
      _nativeHandler(desc.NativeHandler),
      _mode(desc.PresentMode),
      _reqFormat(desc.Format) {}

SwapChainD3D12::~SwapChainD3D12() noexcept { Destroy(); }

bool SwapChainD3D12::IsValid() const noexcept {
    return _swapchain != nullptr;
}

void SwapChainD3D12::Destroy() noexcept {
    if (!_swapchain) return;
    detail::DiagnosticTopologyScope change{_device->_diagnostics};
    _frames.clear();
    _swapchain = nullptr;
    if (_frameLatencyEvent) {
        CloseHandle(_frameLatencyEvent);
        _frameLatencyEvent = nullptr;
    }
}

SwapChainAcquireResult SwapChainD3D12::AcquireNext(uint64_t timeoutMs) noexcept {
    SwapChainAcquireResult result{};
    if (_requiresRecreateAfterDiscard) {
        result.Status = SwapChainStatus::RequireRecreate;
        return result;
    }
    if (_frameLatencyEvent == nullptr) {
        return result;
    }
    const HWND hwnd = std::bit_cast<HWND>(_nativeHandler);
    if (!_IsPresentableHwnd(hwnd)) {
        result.Status = SwapChainStatus::RetryLater;
        result.NativeStatusCode = static_cast<int64_t>(DXGI_STATUS_OCCLUDED);
        return result;
    }
    DWORD milliseconds;
    if (timeoutMs == std::numeric_limits<uint64_t>::max()) {
        milliseconds = INFINITE;
    } else if (timeoutMs > static_cast<uint64_t>(INFINITE) - 1) {
        milliseconds = INFINITE - 1;
    } else {
        milliseconds = static_cast<DWORD>(timeoutMs);
    }
    // A discarded/skipped frame did not enqueue DXGI Present. Keep its acquired
    // readiness until a real Present, rather than consuming another wait slot.
    const DWORD waitResult = _frameLatencyReady ? WAIT_OBJECT_0 : ::WaitForSingleObjectEx(_frameLatencyEvent, milliseconds, false);
    if (waitResult == WAIT_OBJECT_0) {
        _frameLatencyReady = true;
        const auto curr = static_cast<uint32_t>(_swapchain->GetCurrentBackBufferIndex());
        ++_outstandingFrameToken;
        SwapChainFrame frame = MakeFrame(
            this,
            _outstandingFrameToken,
            _frames[curr].image.get(),
            curr,
            nullptr,
            nullptr);
        result.Status = SwapChainStatus::Success;
        result.NativeStatusCode = 0;
        result.Frame = std::move(frame);
        return result;
    } else if (waitResult == WAIT_TIMEOUT) {
        result.Status = SwapChainStatus::RetryLater;
        result.NativeStatusCode = static_cast<int64_t>(waitResult);
        return result;
    } else {
        result.Status = SwapChainStatus::Error;
        result.NativeStatusCode = static_cast<int64_t>(waitResult);
        return result;
    }
}

SwapChainPresentResult SwapChainD3D12::Present(SwapChainFrame frame) noexcept {
    auto diagnostics = MakeScopeGuard([&] { if (_device->_diagnostics) detail::FlushDiagnostics(_device->_diagnostics); });
    SwapChainPresentResult result{};
    InvalidateFrame(frame);
    const HWND hwnd = std::bit_cast<HWND>(_nativeHandler);
    if (!_IsPresentableHwnd(hwnd)) {
        RADRAY_WARN_LOG("IDXGISwapChain::Present skipped: hwnd is not presentable");
        result.NativeStatusCode = static_cast<int64_t>(DXGI_STATUS_OCCLUDED);
        result.Status = SwapChainStatus::RetryLater;
        return result;
    }
    UINT syncInterval = 0;
    UINT presentFlags = DXGI_PRESENT_DO_NOT_WAIT;
    switch (_mode) {
        case PresentMode::FIFO: {
            syncInterval = 1;
            break;
        }
        case PresentMode::Mailbox: {
            syncInterval = 0;
            break;
        }
        case PresentMode::Immediate: {
            syncInterval = 0;
            if (_device->_isAllowTearing) presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
            break;
        }
    }
    const HRESULT hr = _swapchain->Present(syncInterval, presentFlags);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        result.NativeStatusCode = static_cast<int64_t>(hr);
        result.Status = SwapChainStatus::RetryLater;
        return result;
    }
    _CheckD3D12Result(_device, hr, "IDXGISwapChain::Present");
    result.NativeStatusCode = static_cast<int64_t>(hr);
    result.Status = hr == DXGI_STATUS_OCCLUDED ? SwapChainStatus::RetryLater : SwapChainStatus::Success;
    if (hr != DXGI_STATUS_OCCLUDED) _frameLatencyReady = false;
    return result;
}

SwapChainPresentResult SwapChainD3D12::DiscardAcquiredFrame(SwapChainFrame frame) noexcept {
    InvalidateFrame(frame);
    _requiresRecreateAfterDiscard = true;
    return {.NativeStatusCode = 0, .Status = SwapChainStatus::Success};
}

bool SwapChainD3D12::Recreate(uint32_t width, uint32_t height, TextureFormat format, PresentMode presentMode) noexcept {
    detail::DiagnosticTopologyScope change{_device->_diagnostics};
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (HRESULT hr = _swapchain->GetDesc1(&desc); FAILED(hr)) {
        RADRAY_ERR_LOG("IDXGISwapChain1::GetDesc1 failed: {} {}", GetErrorName(hr), hr);
        return false;
    }

    const DXGI_FORMAT rawFormat = _SwapChainStorageFormat(format);
    _frames.clear();
    const HRESULT hr = _swapchain->ResizeBuffers(desc.BufferCount, width, height, rawFormat, desc.Flags);
    _CheckD3D12Result(_device, hr, "IDXGISwapChain::ResizeBuffers");
    _reqFormat = format;
    _mode = presentMode;
    if (!_RefreshSwapChainBackBuffers(this)) {
        return false;
    }
    _requiresRecreateAfterDiscard = false;
    return true;
}

uint32_t SwapChainD3D12::GetBackBufferCount() const noexcept {
    return static_cast<uint32_t>(_frames.size());
}

SwapChainDescriptor SwapChainD3D12::GetDesc() const noexcept {
    DXGI_SWAP_CHAIN_DESC1 desc;
    _swapchain->GetDesc1(&desc);
    SwapChainDescriptor result{};
    result.PresentQueue = _presentQueue;
    result.NativeHandler = _nativeHandler;
    result.Width = desc.Width;
    result.Height = desc.Height;
    result.BackBufferCount = desc.BufferCount;
    result.Format = _reqFormat;
    result.PresentMode = _mode;
    return result;
}

// == 各对象类实现 ==

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
    _externalHandle.Reset();
    _externalInfo.reset();
    if (_mappedData) {
        Unmap();
    }
    _mappedData = nullptr;
    _buf = nullptr;
    _alloc = nullptr;
}

void* BufferD3D12::Map(uint64_t offset, uint64_t size) noexcept {
    if (_hints.HasFlag(ResourceHint::PersistentMap) && _mappedData != nullptr) {
        return static_cast<byte*>(_mappedData) + offset;
    }
    const D3D12_RANGE readRange = _usage.HasFlag(BufferUse::MapRead)
                                      ? D3D12_RANGE{offset, offset + size}
                                      : D3D12_RANGE{0, 0};
    void* ptr = nullptr;
    if (HRESULT hr = _buf->Map(0, &readRange, &ptr);
        FAILED(hr)) {
        detail::FlushDiagnostics(_device->_diagnostics);
        RADRAY_ABORT("ID3D12Resource::Map failed: {} {}", GetErrorName(hr), hr);
    }
    _mappedData = ptr;
    return static_cast<byte*>(ptr) + offset;
}

void BufferD3D12::Unmap() noexcept {
    const D3D12_RANGE noWriteRange{0, 0};
    _buf->Unmap(0, _usage.HasFlag(BufferUse::MapWrite) ? nullptr : &noWriteRange);
    _mappedData = nullptr;
}

void BufferD3D12::FlushMappedRange(BufferRange) noexcept {}

void BufferD3D12::InvalidateMappedRange(BufferRange) noexcept {}

void BufferD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    SetObjectName(name, _buf.Get(), _alloc.Get());
}

BufferDescriptor BufferD3D12::GetDesc() const noexcept {
    return BufferDescriptor{
        .Size = _reqSize,
        .Memory = _memory,
        .Usage = _usage,
        .Hints = _hints};
}

QueryPoolD3D12::QueryPoolD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12QueryHeap> heap,
    QueryType type,
    uint32_t count) noexcept
    : _device(device),
      _heap(std::move(heap)),
      _type(type),
      _count(count) {}

bool QueryPoolD3D12::IsValid() const noexcept {
    return _heap != nullptr;
}

void QueryPoolD3D12::Destroy() noexcept {
    _heap = nullptr;
}

void QueryPoolD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _heap.Get());
}

QueryType QueryPoolD3D12::GetType() const noexcept {
    return _type;
}

uint32_t QueryPoolD3D12::GetCount() const noexcept {
    return _count;
}

TimestampQueryCalibration QueryPoolD3D12::GetTimestampCalibration(CommandQueue* queue_) const noexcept {
    auto queue = CastD3D12Object(queue_);
    uint64_t frequency = 0;
    if (HRESULT hr = queue->_queue->GetTimestampFrequency(&frequency);
        FAILED(hr) || frequency == 0) {
        RADRAY_ERR_LOG("ID3D12CommandQueue::GetTimestampFrequency failed: {} {}", GetErrorName(hr), hr);
        return {};
    }
    return TimestampQueryCalibration{
        .FrequencyHz = frequency,
        .TickPeriodNs = 1'000'000'000.0 / static_cast<double>(frequency)};
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
    _externalHandle.Reset();
    _externalInfo.reset();
    _tex = nullptr;
    _alloc = nullptr;
}

void TextureD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    SetObjectName(name, _tex.Get(), _alloc.Get());
}

TextureDescriptor TextureD3D12::GetDesc() const noexcept {
    return TextureDescriptor{
        _dimension,
        static_cast<uint32_t>(_rawDesc.Width),
        static_cast<uint32_t>(_rawDesc.Height),
        static_cast<uint32_t>(_rawDesc.DepthOrArraySize),
        _rawDesc.MipLevels,
        _rawDesc.SampleDesc.Count,
        _format,
        _memory,
        _usage,
        _hints};
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

void TextureViewD3D12::SetDebugName(std::string_view name) noexcept {
    RADRAY_UNUSED(name);
}

bool Dxil::IsValid() const noexcept {
    return !_dxil.empty();
}

void Dxil::Destroy() noexcept {
    _dxil.clear();
    _dxil.shrink_to_fit();
    _stages = ShaderStage::UNKNOWN;
}

D3D12_SHADER_BYTECODE Dxil::ToByteCode() const noexcept {
    return {_dxil.data(), _dxil.size()};
}

BindingGroupLayoutD3D12::~BindingGroupLayoutD3D12() noexcept { Destroy(); }
bool BindingGroupLayoutD3D12::IsValid() const noexcept { return _device != nullptr; }
void BindingGroupLayoutD3D12::Destroy() noexcept {
    _entries.clear();
    _tables.clear();
    _bindings.clear();
    _rootOrder.clear();
    _device = nullptr;
}
void BindingGroupLayoutD3D12::SetDebugName(std::string_view name) noexcept { RADRAY_UNUSED(name); }
BindingGroupLayoutDescriptor BindingGroupLayoutD3D12::GetDesc() const noexcept { return {_bindings}; }
RootSigD3D12::~RootSigD3D12() noexcept { Destroy(); }
ID3D12RootSignature* RootSigD3D12::GetNative() const noexcept { return _rootSignature.Get(); }
bool RootSigD3D12::IsValid() const noexcept { return _rootSignature != nullptr; }
void RootSigD3D12::Destroy() noexcept {
    _rootSignature.Reset();
    _parameterGroups.clear();
    _pushConstantBindings.clear();
    _groups.clear();
    _pushes.clear();
    _device = nullptr;
}
void RootSigD3D12::SetDebugName(std::string_view name) noexcept { SetObjectName(name, GetNative()); }
PipelineLayoutDescriptor RootSigD3D12::GetDesc() const noexcept { return {_groups, _pushes}; }
Nullable<const ShaderParameterGroupLayoutD3D12*> RootSigD3D12::FindParameterGroup(uint32_t slot) const noexcept {
    const auto it = std::find_if(_parameterGroups.begin(), _parameterGroups.end(), [&](const auto& g) { return g.GroupSlot == slot; });
    return it == _parameterGroups.end() ? Nullable<const ShaderParameterGroupLayoutD3D12*>{nullptr} : &*it;
}

GraphicsPsoD3D12::GraphicsPsoD3D12(
    DeviceD3D12* device,
    RootSigD3D12* layout,
    ComPtr<ID3D12PipelineState> pso,
    std::array<std::optional<uint32_t>, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexStrides,
    D3D12_PRIMITIVE_TOPOLOGY topo) noexcept
    : _device(device),
      _layout(layout),
      _pso(std::move(pso)),
      _vertexStrides(std::move(vertexStrides)),
      _topo(topo) {}

bool GraphicsPsoD3D12::IsValid() const noexcept {
    return _pso != nullptr;
}

void GraphicsPsoD3D12::Destroy() noexcept {
    _pso = nullptr;
}

void GraphicsPsoD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _pso.Get());
}

ComputePsoD3D12::ComputePsoD3D12(
    DeviceD3D12* device,
    RootSigD3D12* layout,
    ComPtr<ID3D12PipelineState> pso) noexcept
    : _device(device),
      _layout(layout),
      _pso(std::move(pso)) {}

bool ComputePsoD3D12::IsValid() const noexcept {
    return _pso != nullptr;
}

void ComputePsoD3D12::Destroy() noexcept {
    _pso = nullptr;
}

void ComputePsoD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _pso.Get());
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

void SamplerD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
}

}  // namespace radray::render::d3d12
