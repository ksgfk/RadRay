#include "d3d12_device.h"

#include "d3d12_command_queue.h"
#include "d3d12_swap_chain.h"

namespace radray::rhi::d3d12 {

std::unique_ptr<D3D12Device> CreateImpl(const DeviceCreateInfoD3D12& info) {
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
    {
        HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
        if (hr != S_OK) {
            RADRAY_ERR_LOG("at {}:{}\n    D3D12 error '{} (code = {})", __FILE__, __LINE__, ::radray::rhi::d3d12::GetErrorName(hr), hr);
            return std::unique_ptr<D3D12Device>{};
        }
    }
    if (info.AdapterIndex.has_value()) {
        auto adapterIndex = info.AdapterIndex.value();
        {
            HRESULT hr = dxgiFactory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
            if (hr != S_OK) {
                RADRAY_ERR_LOG("at {}:{}\n    D3D12 error '{} (code = {})", __FILE__, __LINE__, ::radray::rhi::d3d12::GetErrorName(hr), hr);
                return std::unique_ptr<D3D12Device>{};
            }
        }
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
        return std::unique_ptr<D3D12Device>{};
    }
    {
        HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.GetAddressOf()));
        if (hr != S_OK) {
            RADRAY_ERR_LOG("at {}:{}\n    D3D12 error '{} (code = {})", __FILE__, __LINE__, ::radray::rhi::d3d12::GetErrorName(hr), hr);
            return std::unique_ptr<D3D12Device>{};
        }
    }
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        std::wstring s{desc.Description};
        RADRAY_INFO_LOG("D3D12 device create on device: {}", Utf8ToString(s));
    }
    auto result = std::make_unique<D3D12Device>();
    result->dxgiFactory = std::move(dxgiFactory);
    result->adapter = std::move(adapter);
    result->device = std::move(device);
    return result;
}

D3D12Device::D3D12Device() = default;

D3D12Device::~D3D12Device() noexcept = default;

CommandQueueHandle D3D12Device::CreateCommandQueue(CommandListType type) {
    auto listType = ToCmdListType(type);
    auto cmdQueue = new D3D12CommandQueue{this, listType};
    return {
        reinterpret_cast<uint64_t>(cmdQueue),
        cmdQueue->queue.Get()};
}

void D3D12Device::DestroyCommandQueue(CommandQueueHandle handle) {
    auto cmdQueue = reinterpret_cast<D3D12CommandQueue*>(handle.Handle);
    delete cmdQueue;
}

SwapChainHandle D3D12Device::CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) {
    HWND hwnd = reinterpret_cast<HWND>(info.WindowHandle);
    auto cmdQueue = reinterpret_cast<D3D12CommandQueue*>(cmdQueueHandle);
    auto swapchain = new D3D12SwapChain{
        this,
        cmdQueue,
        hwnd,
        info.Width, info.Height,
        info.BackBufferCount,
        info.Vsync};
    return {
        reinterpret_cast<uint64_t>(swapchain),
        swapchain->swapChain.Get()};
}

void D3D12Device::DestroySwapChain(SwapChainHandle handle) {
    auto swapchain = reinterpret_cast<D3D12SwapChain*>(handle.Handle);
    delete swapchain;
}

}  // namespace radray::rhi::d3d12
