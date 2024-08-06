#include "d3d12_device.h"

#include <radray/rhi/config.h>

#include "d3d12_command_queue.h"
#include "d3d12_command_allocator.h"
#include "d3d12_command_list.h"

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
        ComPtr<IDXGIFactory6> tFactory;
        dxgiFactory.As(&tFactory);
        for (
            auto adapterIndex = 0u;
            tFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
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
            tFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
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
}

RadrayCommandQueue Device::CreateCommandQueue(RadrayQueueType type) {
    auto q = RhiNew<CommandQueue>(this, EnumConvert(type));
    return {.Ptr = q, .Native = q->queue.Get()};
}

void Device::DestroyCommandQueue(RadrayCommandQueue queue) {
    auto q = reinterpret_cast<CommandQueue*>(queue.Ptr);
    RhiDelete(q);
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

}  // namespace radray::rhi::d3d12
