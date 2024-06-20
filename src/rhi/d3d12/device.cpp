#include "device.h"

namespace radray::rhi::d3d12 {

std::unique_ptr<DeviceInterfaceD3D12> CreateImpl(const DeviceCreateInfoD3D12& info) {
    ComPtr<IDXGIFactory2> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;

    uint32_t dxgiFactoryFlags = 0;
    if (info.IsEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
    RADRAY_DX_RETURN_WHEN_FAIL(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf())), std::unique_ptr<DeviceInterfaceD3D12>{});
    if (info.AdapterIndex.has_value()) {
        auto adapterIndex = info.AdapterIndex.value();
        RADRAY_DX_RETURN_WHEN_FAIL(dxgiFactory->EnumAdapters1(adapterIndex, adapter.GetAddressOf()), std::unique_ptr<DeviceInterfaceD3D12>{});
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
        RADRAY_ERR_LOG("cannot find devices support D3D12");
        return std::unique_ptr<DeviceInterfaceD3D12>{};
    }
    RADRAY_DX_RETURN_WHEN_FAIL(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.GetAddressOf())), std::unique_ptr<DeviceInterfaceD3D12>{});
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        std::wstring s{desc.Description};
        RADRAY_INFO_LOG("D3D12 device create on device: {}", Utf8ToString(s));
    }
    auto result = std::make_unique<DeviceInterfaceD3D12>();
    result->dxgiFactory = std::move(dxgiFactory);
    result->adapter = std::move(adapter);
    result->device = std::move(device);
    return result;
}

DeviceInterfaceD3D12::DeviceInterfaceD3D12() = default;

DeviceInterfaceD3D12::~DeviceInterfaceD3D12() noexcept = default;

}  // namespace radray::rhi::d3d12
