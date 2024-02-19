#include <radray/d3d12/device.h>

namespace radray::d3d12 {

Device::Device() noexcept {
    uint32_t dxgiFactoryFlags = 0;
#if defined(RADRAY_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf())));
    {
        ComPtr<IDXGIAdapter1> temp;
        for (auto adapterIndex = 0u; dxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND; adapterIndex++) {
            DXGI_ADAPTER_DESC1 desc;
            temp->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                std::wstring s{desc.Description};
                RADRAY_LOG_INFO("d3d12 find device: {}", Utf8ToString(s));
            }
            temp = nullptr;
        }
    }
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
    if (adapter == nullptr) {
        RADRAY_ABORT("cannot find devices support d3d12");
    }
    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.GetAddressOf())));
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        std::wstring s{desc.Description};
        RADRAY_LOG_INFO("d3d12 select device: {}", Utf8ToString(s));
    }
    globalResHeap = std::make_unique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 500000, false);

    staticSamplerDescs.reserve(16);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            D3D12_SAMPLER_DESC v{};
            v.MaxAnisotropy = 0;
            switch (j) {
                case 0:
                    v.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                    break;
                case 1:
                    v.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                    break;
                case 2:
                    v.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    break;
                case 3:
                    v.MaxAnisotropy = 16;
                    v.Filter = D3D12_FILTER_ANISOTROPIC;
                    break;
                default: RADRAY_ABORT("impossible"); break;
            }
            D3D12_TEXTURE_ADDRESS_MODE address = [&] {
                switch (i) {
                    case 0:
                        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    case 1:
                        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    case 2:
                        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                    default:
                        v.BorderColor[0] = 0;
                        v.BorderColor[1] = 0;
                        v.BorderColor[2] = 0;
                        v.BorderColor[3] = 0;
                        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                }
            }();
            v.AddressU = address;
            v.AddressV = address;
            v.AddressW = address;
            v.MipLODBias = 0;
            v.MinLOD = 0;
            v.MaxLOD = 16;
            staticSamplerDescs.emplace_back(v);
        }
    }
    globalSamplerHeap = std::make_unique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, staticSamplerDescs.size(), true);
    for (size_t i = 0; i < staticSamplerDescs.size(); i++) {
        device->CreateSampler(&staticSamplerDescs[i], globalSamplerHeap->HandleCPU(i));
    }
}

}  // namespace radray::d3d12
