#include "d3d12_device.h"

#include <radray/basic_math.h>
#include <radray/rhi/config.h>

#include "d3d12_command_queue.h"
#include "d3d12_command_allocator.h"
#include "d3d12_command_list.h"
#include "d3d12_fence.h"
#include "d3d12_swapchain.h"
#include "d3d12_buffer.h"

namespace radray::rhi::d3d12 {

Device::Device(const RadrayDeviceDescriptorD3D12& desc) {
    uint32_t dxgiFactoryFlags = 0;
    if (desc.IsEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
    RADRAY_DX_FTHROW(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf())));
    if (desc.AdapterIndex) {
        auto adapterIndex = desc.AdapterIndex;
        RADRAY_DX_FTHROW(dxgiFactory->EnumAdapters1(adapterIndex, adapter.GetAddressOf()));
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device), nullptr))) {
            adapter = nullptr;
        }
    } else {
        ComPtr<IDXGIAdapter1> temp;
        for (
            auto adapterIndex = 0u;
            dxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
            adapterIndex++) {
            DXGI_ADAPTER_DESC1 desc;
            temp->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                std::wstring s{desc.Description};
                RADRAY_DEBUG_LOG("D3D12 find device: {}", Utf8ToString(s));
            }
            temp = nullptr;
        }
        for (
            auto adapterIndex = 0u;
            dxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
            adapterIndex++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device), nullptr))) {
                    break;
                }
            }
            adapter = nullptr;
        }
    }
    if (adapter == nullptr) {
        RADRAY_DX_THROW("cannot find devices support D3D12");
    }
    RADRAY_DX_FTHROW(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.GetAddressOf())));
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        std::wstring s{desc.Description};
        RADRAY_INFO_LOG("D3D12 device create on device: {}", Utf8ToString(s));
    }
    {
        D3D12MA::ALLOCATOR_DESC desc{};
        desc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        desc.pDevice = device.Get();
        desc.pAdapter = adapter.Get();
        D3D12MA::ALLOCATION_CALLBACKS allocationCallbacks{};
        allocationCallbacks.pAllocate = [](size_t size, size_t alignment, void*) {
            return RhiMalloc(alignment, size);
        };
        allocationCallbacks.pFree = [](void* ptr, void*) {
            if (ptr != nullptr) {
                RhiFree(ptr);
            }
        };
        desc.pAllocationCallbacks = &allocationCallbacks;
        desc.Flags |= D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED;
        RADRAY_DX_FTHROW(D3D12MA::CreateAllocator(&desc, resourceAlloc.GetAddressOf()));
    }
    {
        srvHeap = MakeUnique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 18, false);
        rtvHeap = MakeUnique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 << 16, false);
        dsvHeap = MakeUnique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 << 16, false);
        gpuSrvHeap = MakeUnique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 16, true);
        gpuSamplerHeap = MakeUnique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1 << 11, true);
    }
}

RadrayCommandQueue Device::CreateCommandQueue(RadrayQueueType type) {
    auto q = RhiNew<CommandQueue>(this, EnumConvert(type));
    return {.Ptr = q, .Native = q->queue.Get()};
}

void Device::DestroyCommandQueue(RadrayCommandQueue queue) {
    auto q = reinterpret_cast<CommandQueue*>(queue.Ptr);
    RhiDelete(q);
}

RadrayFence Device::CreateFence() {
    auto f = RhiNew<Fence>(this);
    return {.Ptr = f, .Native = f->fence.Get()};
}

void Device::DestroyFence(RadrayFence fence) {
    auto f = reinterpret_cast<Fence*>(fence.Ptr);
    RhiDelete(f);
}

RadrayCommandAllocator Device::CreateCommandAllocator(RadrayQueueType type) {
    auto a = RhiNew<CommandAllocator>(this, EnumConvert(type));
    return {.Ptr = a, .Native = a->alloc.Get()};
}

void Device::DestroyCommandAllocator(RadrayCommandAllocator alloc) {
    auto a = reinterpret_cast<CommandAllocator*>(alloc.Ptr);
    RhiDelete(a);
}

RadrayCommandList Device::CreateCommandList(RadrayCommandAllocator alloc) {
    auto a = reinterpret_cast<CommandAllocator*>(alloc.Ptr);
    auto l = RhiNew<CommandList>(this, a->alloc.Get(), a->type);
    return {.Ptr = l, .Native = l->list.Get()};
}

void Device::DestroyCommandList(RadrayCommandList list) {
    auto l = reinterpret_cast<CommandList*>(list.Ptr);
    RhiDelete(l);
}

void Device::ResetCommandAllocator(RadrayCommandAllocator alloc) {
    auto a = reinterpret_cast<CommandAllocator*>(alloc.Ptr);
    a->alloc->Reset();
}

RadraySwapChain Device::CreateSwapChain(const RadraySwapChainDescriptor& desc) {
    DXGI_SWAP_CHAIN_DESC1 chain{
        .Width = desc.Width,
        .Height = desc.Height,
        .Format = EnumConvert(desc.Format),
        .Stereo = FALSE,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = desc.BackBufferCount,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = 0};
    ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(desc.PresentQueue.Native);
    HWND hwnd = reinterpret_cast<HWND>(desc.NativeWindow);
    auto s = RhiNew<SwapChain>(this, queue, hwnd, chain, desc.EnableSync);
    return {.Ptr = s, .Native = s->swapchain.Get()};
}

void Device::DestroySwapChian(RadraySwapChain swapchain) {
    auto s = reinterpret_cast<SwapChain*>(swapchain.Ptr);
    RhiDelete(s);
}

RadrayBuffer Device::CreateBuffer(const RadrayBufferDescriptor& desc) {
    D3D12_RESOURCE_DESC buf{};
    {
        uint64_t allocationSize = desc.Size;
        if (desc.Flags & RADRAY_BUFFER_CREATE_FLAG_IS_CBUFFER) {
            allocationSize = CalcAlign(allocationSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        }
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        buf.Width = allocationSize;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.Format = DXGI_FORMAT_UNKNOWN;
        buf.SampleDesc.Count = 1;
        buf.SampleDesc.Quality = 0;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buf.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (desc.Flags & RADRAY_BUFFER_CREATE_FLAG_ALLOW_UNORDERED_ACCESS) {
            buf.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        UINT64 paddedSize = 0;
        device->GetCopyableFootprints(&buf, 0, 1, 0, nullptr, nullptr, nullptr, &paddedSize);
        if (paddedSize != UINT64_MAX) {
            allocationSize = (uint64_t)paddedSize;
            buf.Width = allocationSize;
        }
        if (desc.Usage == RADRAY_HEAP_USAGE_READBACK) {
            buf.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }
    }
    D3D12_RESOURCE_STATES initState;
    {
        RadrayResourceStates radaryInitState = desc.InitStates;
        if (desc.Usage == RADRAY_HEAP_USAGE_UPLOAD) {
            radaryInitState = RADRAY_RESOURCE_STATE_GENERIC_READ;
        } else if (desc.Usage == RADRAY_HEAP_USAGE_READBACK) {
            radaryInitState = RADRAY_RESOURCE_STATE_COPY_DEST;
        }
        initState = EnumConvert(radaryInitState);
    }
    D3D12MA::ALLOCATION_DESC allocDesc{};
    {
        allocDesc.HeapType = EnumConvert(desc.Usage);
        if (desc.Flags & RADRAY_BUFFER_CREATE_FLAG_COMMITTED) {
            allocDesc.Flags |= D3D12MA::ALLOCATION_FLAG_COMMITTED;
        }
    }
    auto b = RhiNew<Buffer>(this, buf.Width, initState, buf, allocDesc);
    return {.Ptr = b, .Native = b->buffer.Get()};
}

void Device::DestroyBuffer(RadrayBuffer buffer) {
    auto b = reinterpret_cast<Buffer*>(buffer.Ptr);
    RhiDelete(b);
}

RadrayBufferView Device::CreateBufferView(const RadrayBufferViewDescriptor& desc) {
    auto buffer = reinterpret_cast<Buffer*>(desc.Buffer.Ptr);
    BufferView* view = nullptr;
    if (desc.Type == RADRAY_RESOURCE_TYPE_CBUFFER) {
        UINT index = srvHeap->Allocate();
        auto indexGuard = MakeScopeGuard([&]() { srvHeap->Recycle(index); });
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{
            .BufferLocation = buffer->gpuAddr,
            .SizeInBytes = static_cast<UINT>(buffer->size)};
        srvHeap->Create(cbvDesc, index);
        indexGuard.Dismiss();
        view = RhiNew<BufferView>(BufferView{srvHeap.get(), index, desc.Type, desc.Format});
    } else if (desc.Type == RADRAY_RESOURCE_TYPE_BUFFER) {
        UINT index = srvHeap->Allocate();
        auto indexGuard = MakeScopeGuard([&]() { srvHeap->Recycle(index); });
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = EnumConvert(desc.Format);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = desc.FirstElementOffset;
        srvDesc.Buffer.NumElements = desc.ElementCount;
        srvDesc.Buffer.StructureByteStride = desc.ElementStride;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        if (srvDesc.Format != DXGI_FORMAT_UNKNOWN) {
            srvDesc.Buffer.StructureByteStride = 0;
        }
        srvHeap->Create(buffer->buffer.Get(), srvDesc, index);
        indexGuard.Dismiss();
        view = RhiNew<BufferView>(BufferView{srvHeap.get(), index, desc.Type, desc.Format});
    } else if (desc.Type == RADRAY_RESOURCE_TYPE_BUFFER_RW) {
        UINT index = srvHeap->Allocate();
        auto indexGuard = MakeScopeGuard([&]() { srvHeap->Recycle(index); });
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = desc.FirstElementOffset;
        uavDesc.Buffer.NumElements = desc.ElementCount;
        uavDesc.Buffer.StructureByteStride = desc.ElementStride;
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        if (desc.Format != RADRAY_FORMAT_UNKNOWN) {
            uavDesc.Format = EnumConvert(desc.Format);
            D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = {
                uavDesc.Format,
                D3D12_FORMAT_SUPPORT1_NONE,
                D3D12_FORMAT_SUPPORT2_NONE};
            HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport));
            if (!SUCCEEDED(hr) || !(formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) || !(formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
                RADRAY_DX_THROW(std::format("D3D12 cannot use UAV format {}", (uint32_t)desc.Format));
            }
        }
        if (uavDesc.Format != DXGI_FORMAT_UNKNOWN) {
            uavDesc.Buffer.StructureByteStride = 0;
        }
        srvHeap->Create(buffer->buffer.Get(), uavDesc, index);
        indexGuard.Dismiss();
        view = RhiNew<BufferView>(BufferView{srvHeap.get(), index, desc.Type, desc.Format});
    } else {
        RADRAY_DX_THROW(std::format("D3D12 cannot create buffer view for {}", (uint32_t)desc.Type));
    }
    return RadrayBufferView{view};
}

void Device::DestroyBufferView(RadrayBuffer buffer, RadrayBufferView view) {
    BufferView* v = reinterpret_cast<BufferView*>(view.Handle);
    v->heap->Recycle(v->index);
    RhiDelete(v);
}

}  // namespace radray::rhi::d3d12
