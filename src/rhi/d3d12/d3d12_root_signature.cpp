#include "d3d12_root_signature.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

RootSignature::RootSignature(Device* device, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    ComPtr<ID3DBlob> error{};
    ComPtr<ID3DBlob> rootSigSer{};
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, rootSigSer.GetAddressOf(), error.GetAddressOf());
    if (!SUCCEEDED(hr)) {
        RADRAY_DX_THROW("cannot serialize root signature, reason={}", error->GetBufferPointer());
    }
    RADRAY_DX_FTHROW(device->device->CreateRootSignature(
        0,
        rootSigSer->GetBufferPointer(),
        rootSigSer->GetBufferSize(),
        IID_PPV_ARGS(rootSig.GetAddressOf())));
}

}  // namespace radray::rhi::d3d12
