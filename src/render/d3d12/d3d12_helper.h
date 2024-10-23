#pragma once

#include <string_view>

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>

#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#include <directx/d3d12shader.h>
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>

namespace radray::rhi::d3d12 {

using Microsoft::WRL::ComPtr;

const char* GetErrorName(HRESULT hr) noexcept;

void SetObjectName(std::string_view str, ID3D12Object* obj, D3D12MA::Allocation* alloc = nullptr) noexcept;

UINT SubresourceIndex(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) noexcept;

DXGI_FORMAT TypelessFormat(DXGI_FORMAT fmt) noexcept;

std::string_view to_string(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept;

}  // namespace radray::rhi::d3d12

#ifndef RADRAY_DX_CHECK
#define RADRAY_DX_CHECK(x)                                                                                               \
    do {                                                                                                                 \
        HRESULT hr_ = (x);                                                                                               \
        if (hr_ != S_OK) [[unlikely]] {                                                                                  \
            RADRAY_ABORT("D3D12 error '{} with error {} (code = {})", #x, ::radray::rhi::d3d12::GetErrorName(hr_), hr_); \
        }                                                                                                                \
    } while (false)
#endif

template <class CharT>
struct fmt::formatter<D3D12_DESCRIPTOR_HEAP_TYPE, CharT> : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(D3D12_DESCRIPTOR_HEAP_TYPE const& val, FormatContext& ctx) const {
        return formatter<std::string_view, CharT>::format(radray::rhi::d3d12::to_string(val), ctx);
    }
};
