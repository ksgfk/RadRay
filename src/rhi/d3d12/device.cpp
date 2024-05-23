#include "device.h"

namespace radray::rhi::d3d12 {

Device::Device(const DeviceCreateInfoD3D12& info) {
    uint32_t dxgiFactoryFlags = 0;
    if (info.IsEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
    RADRAY_DX_CHECK(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf())));
    {
        ComPtr<IDXGIAdapter1> temp;
        for (auto adapterIndex = 0u; dxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND; adapterIndex++) {
            DXGI_ADAPTER_DESC1 desc;
            temp->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                std::wstring s{desc.Description};
                RADRAY_INFO_LOG("d3d12 enum device: {}", Utf8ToString(s));
            }
            temp = nullptr;
        }
    }
    if (info.AdapterIndex.has_value()) {
        ComPtr<IDXGIAdapter> temp;
        HRESULT hr = dxgiFactory->EnumAdapters(*info.AdapterIndex, temp.GetAddressOf());
        if (hr != DXGI_ERROR_NOT_FOUND) {
            RADRAY_DX_CHECK(temp->QueryInterface(IID_PPV_ARGS(adapter.GetAddressOf())));
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device), nullptr))) {
                adapter = nullptr;
            }
        }
    } else {
        for (auto adapterIndex = 0u; dxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.GetAddressOf())) != DXGI_ERROR_NOT_FOUND; adapterIndex++) {
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
        RADRAY_ABORT("cannot find adapter support d3d12");
    }
    RADRAY_DX_CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.GetAddressOf())));
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        std::wstring s{desc.Description};
        RADRAY_INFO_LOG("d3d12 select device: {}", Utf8ToString(s));
    }
}

Device::~Device() noexcept = default;

RC<ISwapChain> Device::CreateSwapChain(const SwapChainCreateInfo& info) {
    return nullptr;
}

RC<ICommandQueue> Device::CreateCommandQueue(const CommandQueueCreateInfo& info) {
    return nullptr;
}

RC<IFence> Device::CreateFence(const FenceCreateInfo& info) {
    return nullptr;
}

RC<IBuffer> Device::CreateBuffer(const BufferCreateInfo& info) {
    return nullptr;
}

RC<ITexture> Device::CreateTexture(const TextureCreateInfo& info) {
    return nullptr;
}

}  // namespace radray::rhi::d3d12
